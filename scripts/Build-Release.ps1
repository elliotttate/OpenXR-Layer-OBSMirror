[CmdletBinding()]
param(
    [string]$Version = '0.3.0-beta.4',
    [string]$FileVersion = '0.3.0.4',
    [string]$OBSSourcePath = 'E:\Github\obs-studio',
    [string]$OBSInstallPath = 'C:\Program Files\obs-studio'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($Version -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$') {
    throw "Version must look like 0.3.0 or 0.3.0-beta.4: $Version"
}
if ($FileVersion -notmatch '^\d+\.\d+\.\d+\.\d+$') {
    throw "FileVersion must contain four numeric components: $FileVersion"
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$tag = "v$Version"
$artifactsRoot = Join-Path $repoRoot 'artifacts'
$releaseDirectory = Join-Path $artifactsRoot $tag
$stageDirectory = Join-Path $artifactsRoot ".stage-$tag"
$payloadRoot = Join-Path $stageDirectory 'OpenXR OBS Mirror'

foreach ($target in @($releaseDirectory, $stageDirectory)) {
    $fullTarget = [IO.Path]::GetFullPath($target)
    $fullArtifactsRoot = [IO.Path]::GetFullPath($artifactsRoot).TrimEnd('\') + '\'
    if (-not $fullTarget.StartsWith($fullArtifactsRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a path outside the repository artifacts directory: $fullTarget"
    }
    if (Test-Path -LiteralPath $fullTarget) {
        Remove-Item -LiteralPath $fullTarget -Recurse -Force
    }
}
New-Item -ItemType Directory -Path $releaseDirectory, $payloadRoot -Force | Out-Null

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw "Visual Studio locator not found: $vswhere"
}
$msbuild = & $vswhere -latest -version '[17.0,18.0)' -products * `
    -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' |
    Select-Object -First 1
if (-not $msbuild) { throw 'Visual Studio 2022 MSBuild was not found.' }

& $msbuild (Join-Path $repoRoot 'OpenXR-Layer-OBSMirror.sln') /m `
    /t:Build /p:Configuration=Release /p:Platform=x64
if ($LASTEXITCODE -ne 0) { throw "Native layer build failed (exit $LASTEXITCODE)." }

& (Join-Path $PSScriptRoot 'Build-OBSPlugin.ps1') `
    -OBSSourcePath $OBSSourcePath -OBSInstallPath $OBSInstallPath -Configuration Release
if ($LASTEXITCODE -ne 0) { throw "OBS plugin build failed (exit $LASTEXITCODE)." }

$controlCenterOutput = Join-Path $payloadRoot 'ControlCenter'
& (Join-Path $PSScriptRoot 'Build-ControlCenter.ps1') `
    -Configuration Release -OutputDirectory $controlCenterOutput
if ($LASTEXITCODE -ne 0) { throw "Control Center build failed (exit $LASTEXITCODE)." }

$payloadDirectories = @(
    (Join-Path $payloadRoot 'bin\x64\Release\OBS_Plugin'),
    (Join-Path $payloadRoot 'scripts'),
    (Join-Path $payloadRoot 'OBSPlugin\win-openxr\data'),
    (Join-Path $payloadRoot 'docs\release-notes')
)
New-Item -ItemType Directory -Path $payloadDirectories -Force | Out-Null

$releaseBin = Join-Path $repoRoot 'bin\x64\Release'
foreach ($fileName in @(
    'XR_APILAYER_NOVENDOR_OBSMirror.dll',
    'XR_APILAYER_NOVENDOR_OBSMirror.json'
)) {
    Copy-Item -LiteralPath (Join-Path $releaseBin $fileName) `
        -Destination (Join-Path $payloadRoot 'bin\x64\Release') -Force
}
Copy-Item -LiteralPath (Join-Path $releaseBin 'OBS_Plugin\win-openxr.dll') `
    -Destination (Join-Path $payloadRoot 'bin\x64\Release\OBS_Plugin') -Force

foreach ($scriptName in @(
    'Setup-OBS.ps1',
    'Install-Layer.ps1',
    'Uninstall-Layer.ps1',
    'Uninstall-OBSMirror.ps1',
    'Set-RecordingOverscan.ps1'
)) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $scriptName) `
        -Destination (Join-Path $payloadRoot 'scripts') -Force
}
Copy-Item -Path (Join-Path $repoRoot 'OBSPlugin\win-openxr\data\*') `
    -Destination (Join-Path $payloadRoot 'OBSPlugin\win-openxr\data') -Recurse -Force

foreach ($fileName in @('README.md', 'LICENSE', 'THIRD_PARTY', 'Launch OpenXR OBS Mirror.cmd')) {
    Copy-Item -LiteralPath (Join-Path $repoRoot $fileName) -Destination $payloadRoot -Force
}
Copy-Item -LiteralPath (Join-Path $repoRoot 'docs\INSTALL.md') `
    -Destination (Join-Path $payloadRoot 'docs') -Force
Copy-Item -LiteralPath (Join-Path $repoRoot "docs\release-notes\$tag.md") `
    -Destination (Join-Path $payloadRoot 'docs\release-notes') -Force

$metadata = [ordered]@{
    product = 'OpenXR OBS Mirror'
    version = $Version
    tag = $tag
    platform = 'Windows x64'
    obs_version = (Get-Item -LiteralPath (Join-Path $OBSInstallPath 'bin\64bit\obs64.exe')).VersionInfo.ProductVersion
    graphics_apis = @('Direct3D 11', 'Direct3D 12')
}
$metadata | ConvertTo-Json -Depth 4 | Set-Content `
    -LiteralPath (Join-Path $payloadRoot 'release.json') -Encoding utf8

$iscc = 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
if (-not (Test-Path -LiteralPath $iscc -PathType Leaf)) {
    throw "Inno Setup 6 compiler not found: $iscc"
}
& $iscc '/Qp' "/DMyAppVersion=$Version" "/DMyFileVersion=$FileVersion" `
    "/DPayloadRoot=$payloadRoot" "/DOutputDirectory=$releaseDirectory" `
    (Join-Path $repoRoot 'installer\OpenXR-OBSMirror.iss')
if ($LASTEXITCODE -ne 0) { throw "Installer build failed (exit $LASTEXITCODE)." }

$zipPath = Join-Path $releaseDirectory "OpenXR-OBSMirror-$Version-Portable.zip"
Compress-Archive -LiteralPath $payloadRoot -DestinationPath $zipPath -CompressionLevel Optimal

$releaseFiles = Get-ChildItem -LiteralPath $releaseDirectory -File |
    Where-Object { $_.Name -ne 'SHA256SUMS.txt' } |
    Sort-Object Name
$checksumLines = foreach ($file in $releaseFiles) {
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash.ToLowerInvariant()
    "$hash *$($file.Name)"
}
$checksumPath = Join-Path $releaseDirectory 'SHA256SUMS.txt'
$checksumLines | Set-Content -LiteralPath $checksumPath -Encoding ascii

Remove-Item -LiteralPath $stageDirectory -Recurse -Force

[pscustomobject]@{
    Version = $Version
    Directory = $releaseDirectory
    Installer = Join-Path $releaseDirectory "OpenXR-OBSMirror-$Version-Setup.exe"
    Portable = $zipPath
    Checksums = $checksumPath
}
