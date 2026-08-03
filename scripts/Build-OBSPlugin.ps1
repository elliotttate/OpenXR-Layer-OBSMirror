[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$OBSSourcePath,

    [string]$OBSInstallPath = 'C:\Program Files\obs-studio',

    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$obsSource = [IO.Path]::GetFullPath($OBSSourcePath)
$obsDll = Join-Path $OBSInstallPath 'bin\64bit\obs.dll'
$obsExe = Join-Path $OBSInstallPath 'bin\64bit\obs64.exe'
$obsConfig = Join-Path $obsSource 'libobs\obs-config.h'
$obsConfigTemplate = Join-Path $obsSource 'libobs\obsconfig.h.in'

foreach ($requiredPath in @($obsDll, $obsExe, $obsConfig, $obsConfigTemplate)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required OBS file not found: $requiredPath"
    }
}

$installedVersion = [Version](Get-Item -LiteralPath $obsExe).VersionInfo.ProductVersion
$configText = Get-Content -Raw -LiteralPath $obsConfig
$versionParts = @('MAJOR', 'MINOR', 'PATCH') | ForEach-Object {
    $match = [regex]::Match($configText, "#define LIBOBS_API_${_}_VER\s+(\d+)")
    if (-not $match.Success) { throw "Could not read LIBOBS API version from $obsConfig" }
    [int]$match.Groups[1].Value
}
$sourceVersion = [Version]::new($versionParts[0], $versionParts[1], $versionParts[2])
if ($sourceVersion -ne $installedVersion) {
    throw "OBS source version $sourceVersion does not match installed OBS $installedVersion."
}

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw "Visual Studio locator not found: $vswhere"
}
$vsPath = & $vswhere -latest -version '[17.0,18.0)' -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'Visual Studio 2022 C++ build tools were not found.' }

$toolsVersionFile = Join-Path $vsPath 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
$toolsVersion = (Get-Content -Raw -LiteralPath $toolsVersionFile).Trim()
$toolPath = Join-Path $vsPath "VC\Tools\MSVC\$toolsVersion\bin\Hostx64\x64"
$dumpbin = Join-Path $toolPath 'dumpbin.exe'
$lib = Join-Path $toolPath 'lib.exe'

$buildDirectory = Join-Path $repoRoot 'obj\OBSPlugin'
$importDirectory = Join-Path $buildDirectory 'import'
New-Item -ItemType Directory -Path $importDirectory -Force | Out-Null

$definitionPath = Join-Path $importDirectory 'obs.def'
$importLibrary = Join-Path $importDirectory 'obs.lib'
$exports = & $dumpbin /exports $obsDll
$exportNames = $exports | ForEach-Object {
    if ($_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)') { $Matches[1] }
}
if (-not $exportNames) { throw "No exports were found in $obsDll" }
@('LIBRARY obs.dll', 'EXPORTS') + ($exportNames | ForEach-Object { "    $_" }) |
    Set-Content -Encoding ascii -LiteralPath $definitionPath
& $lib /nologo "/def:$definitionPath" /machine:x64 "/out:$importLibrary"
if ($LASTEXITCODE -ne 0) { throw "Failed to create OBS import library (exit $LASTEXITCODE)." }

$pluginSource = Join-Path $repoRoot 'OBSPlugin\win-openxr'
& cmake -S $pluginSource -B $buildDirectory -G 'Visual Studio 17 2022' -A x64 `
    "-DOBS_IMPORT_LIBRARY=$importLibrary" "-DOBS_SOURCE_DIR=$obsSource"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)." }
& cmake --build $buildDirectory --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "OBS plugin build failed (exit $LASTEXITCODE)." }

$builtPlugin = Join-Path $buildDirectory "$Configuration\win-openxr.dll"
if (-not (Test-Path -LiteralPath $builtPlugin -PathType Leaf)) {
    throw "Built OBS plugin was not found: $builtPlugin"
}

$outputDirectory = Join-Path $repoRoot "bin\x64\$Configuration\OBS_Plugin"
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$outputPlugin = Join-Path $outputDirectory 'win-openxr.dll'
Copy-Item -LiteralPath $builtPlugin -Destination $outputPlugin -Force

$builtPdb = Join-Path $buildDirectory "$Configuration\win-openxr.pdb"
if (Test-Path -LiteralPath $builtPdb -PathType Leaf) {
    Copy-Item -LiteralPath $builtPdb -Destination (Join-Path $outputDirectory 'win-openxr.pdb') -Force
}

[pscustomobject]@{
    OBSVersion = $installedVersion.ToString()
    Plugin = $outputPlugin
    SHA256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $outputPlugin).Hash
}
