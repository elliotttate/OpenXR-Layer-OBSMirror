[CmdletBinding()]
param(
    [string]$Version = '1.0.2',
    [string]$TargetOBS = '32.2.0',
    [string]$TargetVirtualDesktop = '1.34.22.0',
    [string]$TargetVDXR = '1.0.10',
    [string]$UpstreamRef = 'origin/main'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($Version -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$') {
    throw "Invalid version: $Version"
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$outputRoot = Join-Path $repoRoot 'artifacts\vdxr'
$packageName = "VDXR-OBS-Mirror-$Version-win64"
$stage = Join-Path $outputRoot $packageName
$zipPath = Join-Path $outputRoot "$packageName.zip"
$checksumPath = Join-Path $outputRoot 'SHA256SUMS.txt'

$resolvedOutput = [IO.Path]::GetFullPath($outputRoot).TrimEnd('\') + '\'
foreach ($target in @($stage, $zipPath, $checksumPath)) {
    $resolvedTarget = [IO.Path]::GetFullPath($target)
    if (-not $resolvedTarget.StartsWith($resolvedOutput, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a path outside $outputRoot`: $resolvedTarget"
    }
    if (Test-Path -LiteralPath $resolvedTarget) {
        Remove-Item -LiteralPath $resolvedTarget -Recurse -Force
    }
}

$nativeBin = Join-Path $repoRoot 'bin\x64\Release'
$layerDll = Join-Path $nativeBin 'XR_APILAYER_NOVENDOR_OBSMirror.dll'
$layerManifest = Join-Path $nativeBin 'XR_APILAYER_NOVENDOR_OBSMirror.json'
$pluginDll = Join-Path $nativeBin 'OBS_Plugin\win-openxr.dll'
$templateRoot = Join-Path $repoRoot 'packaging\vdxr'
foreach ($required in @($layerDll, $layerManifest, $pluginDll, $templateRoot)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required build output or template is missing: $required"
    }
}

$directories = @(
    (Join-Path $stage 'bin'),
    (Join-Path $stage 'obs-plugin\bin\64bit'),
    (Join-Path $stage 'obs-plugin\data')
)
New-Item -ItemType Directory -Force -Path (@($outputRoot, $stage) + $directories) | Out-Null

Copy-Item -LiteralPath $layerDll -Destination (Join-Path $stage 'bin') -Force
Copy-Item -LiteralPath $layerManifest -Destination (Join-Path $stage 'bin') -Force
Copy-Item -LiteralPath $pluginDll -Destination (Join-Path $stage 'obs-plugin\bin\64bit') -Force
Copy-Item -Path (Join-Path $repoRoot 'OBSPlugin\win-openxr\data\*') `
    -Destination (Join-Path $stage 'obs-plugin\data') -Recurse -Force
Copy-Item -LiteralPath (Join-Path $templateRoot 'Install.ps1') -Destination $stage -Force
Copy-Item -LiteralPath (Join-Path $templateRoot 'Uninstall.ps1') -Destination $stage -Force
Copy-Item -LiteralPath (Join-Path $templateRoot 'README.md') -Destination $stage -Force
Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE') -Destination $stage -Force
Copy-Item -LiteralPath (Join-Path $templateRoot 'LICENSE-OBS-GPL-2.0.txt') -Destination $stage -Force
Copy-Item -LiteralPath (Join-Path $templateRoot 'NOTICE.md') -Destination $stage -Force
Copy-Item -LiteralPath (Join-Path $repoRoot 'THIRD_PARTY') -Destination $stage -Force
Copy-Item -LiteralPath (Join-Path $repoRoot 'docs\VDXR-COMPATIBILITY.md') -Destination $stage -Force
$releaseNotes = Join-Path $repoRoot "docs\release-notes\v$Version-vdxr.md"
if (-not (Test-Path -LiteralPath $releaseNotes -PathType Leaf)) {
    throw "Release notes are missing: $releaseNotes"
}
Copy-Item -LiteralPath $releaseNotes `
    -Destination (Join-Path $stage 'RELEASE-NOTES.md') -Force

$sourceDiff = Join-Path $stage 'SOURCE-CHANGES.diff'
$git = Get-Command git -ErrorAction Stop
$safeRepo = $repoRoot.Replace('\', '/')
$upstreamCommit = (& $git.Source -c "safe.directory=$safeRepo" -C $repoRoot rev-parse $UpstreamRef).Trim()
if ($LASTEXITCODE -ne 0 -or -not $upstreamCommit) {
    throw "Could not resolve upstream ref: $UpstreamRef"
}
$diffText = & $git.Source -c "safe.directory=$safeRepo" -C $repoRoot diff $upstreamCommit HEAD -- `
    'XR_APILAYER_NOVENDOR_OBSMirror/layer.cpp' `
    'XR_APILAYER_NOVENDOR_OBSMirror/XR_APILAYER_NOVENDOR_OBSMirror.vcxproj' `
    'OBSPlugin/win-openxr/CMakeLists.txt' `
    'scripts/Build-OBSPlugin.ps1' `
    'scripts/Build-Release.ps1'
if ($LASTEXITCODE -ne 0) { throw 'Could not generate SOURCE-CHANGES.diff.' }
[IO.File]::WriteAllLines($sourceDiff, [string[]]$diffText, [Text.UTF8Encoding]::new($false))

$releaseCommit = (& $git.Source -c "safe.directory=$safeRepo" -C $repoRoot rev-parse HEAD).Trim()
$buildTimeText = (& $git.Source -c "safe.directory=$safeRepo" -C $repoRoot show -s --format=%cI HEAD).Trim()
$buildTime = [DateTimeOffset]::Parse($buildTimeText).UtcDateTime
$layerHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $stage 'bin\XR_APILAYER_NOVENDOR_OBSMirror.dll')).Hash
$pluginHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $stage 'obs-plugin\bin\64bit\win-openxr.dll')).Hash
$metadata = [ordered]@{
    product = 'VDXR OBS Mirror'
    version = $Version
    platform = 'Windows x64'
    built_utc = $buildTime.ToString('o')
    target_virtual_desktop = $TargetVirtualDesktop
    target_vdxr = $TargetVDXR
    target_obs = $TargetOBS
    graphics_apis = @('Direct3D 11', 'Direct3D 12')
    capture_transport = 'D3D11 GPU shared texture ring'
    openxr_layer_sha256 = $layerHash
    obs_plugin_sha256 = $pluginHash
    upstream_commit = $upstreamCommit
    release_commit = $releaseCommit
}
$metadata | ConvertTo-Json -Depth 4 | Set-Content `
    -LiteralPath (Join-Path $stage 'BUILD-INFO.json') -Encoding utf8

# Validate the finished directory before archiving it.
& (Join-Path $stage 'Install.ps1') -ValidateOnly
if ($LASTEXITCODE -ne 0) { throw 'Package validation failed.' }

# Stable file timestamps make repeated packaging of the same commit produce a
# stable archive when the input binaries are unchanged.
Get-ChildItem -LiteralPath $stage -Recurse -Force | ForEach-Object {
    $_.LastWriteTimeUtc = $buildTime
}
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zipStream = [IO.File]::Open($zipPath, [IO.FileMode]::CreateNew)
try {
    $archive = [IO.Compression.ZipArchive]::new(
        $zipStream,
        [IO.Compression.ZipArchiveMode]::Create,
        $false)
    try {
        foreach ($file in (Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName)) {
            $relativePath = $file.FullName.Substring($stage.Length).TrimStart('\').Replace('\', '/')
            $entry = $archive.CreateEntry($relativePath, [IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = [DateTimeOffset]::new($buildTime, [TimeSpan]::Zero)
            $inputStream = $file.OpenRead()
            $entryStream = $entry.Open()
            try {
                $inputStream.CopyTo($entryStream)
            } finally {
                $entryStream.Dispose()
                $inputStream.Dispose()
            }
        }
    } finally {
        $archive.Dispose()
    }
} finally {
    $zipStream.Dispose()
}
$zipHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath).Hash.ToLowerInvariant()
"$zipHash *$([IO.Path]::GetFileName($zipPath))" | Set-Content -LiteralPath $checksumPath -Encoding ascii

[pscustomobject]@{
    Version = $Version
    Directory = $stage
    Archive = $zipPath
    SHA256 = $zipHash
    Checksums = $checksumPath
    LayerSHA256 = $layerHash.ToLowerInvariant()
    PluginSHA256 = $pluginHash.ToLowerInvariant()
}
