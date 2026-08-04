[CmdletBinding()]
param(
    [string]$LayerBuildDirectory,
    [string]$PluginBinary,
    [string]$LayerInstallDirectory = (Join-Path $env:LOCALAPPDATA 'OpenXR-OBSMirror'),
    [string]$OBSPluginDirectory = (Join-Path $env:ProgramData 'obs-studio\plugins\win-openxr'),
    [switch]$AllowRunningOBS,
    [switch]$SkipPluginInstall
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-Sha256 {
    param([Parameter(Mandatory)][string]$LiteralPath)

    $stream = [IO.File]::Open(
        [IO.Path]::GetFullPath($LiteralPath),
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    try {
        $sha256 = [Security.Cryptography.SHA256]::Create()
        try {
            return ([BitConverter]::ToString($sha256.ComputeHash($stream))).Replace('-', '')
        } finally {
            $sha256.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $LayerBuildDirectory) {
    $LayerBuildDirectory = Join-Path $repoRoot 'bin\x64\Release'
}
if (-not $PluginBinary) {
    $PluginBinary = Join-Path $repoRoot 'bin\x64\Release\OBS_Plugin\win-openxr.dll'
}

$runningOBS = Get-Process -Name obs64 -ErrorAction SilentlyContinue
if ($runningOBS -and -not $AllowRunningOBS) {
    throw 'OBS is running. Close it before installing or updating win-openxr.dll.'
}
$layerDll = Join-Path $LayerBuildDirectory 'XR_APILAYER_NOVENDOR_OBSMirror.dll'
$layerManifest = Join-Path $LayerBuildDirectory 'XR_APILAYER_NOVENDOR_OBSMirror.json'
$requiredPaths = @($layerDll, $layerManifest)
if (-not $SkipPluginInstall) { $requiredPaths += $PluginBinary }
foreach ($requiredPath in $requiredPaths) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required build artifact not found: $requiredPath"
    }
}

$layerInstall = [IO.Path]::GetFullPath($LayerInstallDirectory)
$pluginInstall = [IO.Path]::GetFullPath($OBSPluginDirectory)
$pluginBinDirectory = Join-Path $pluginInstall 'bin\64bit'
$pluginDataDirectory = Join-Path $pluginInstall 'data'
New-Item -ItemType Directory -Path $layerInstall -Force | Out-Null
if (-not $SkipPluginInstall) {
    New-Item -ItemType Directory -Path $pluginBinDirectory, $pluginDataDirectory -Force | Out-Null
}

$layerSourceHash = Get-Sha256 -LiteralPath $layerDll
$versionedLayerName = "XR_APILAYER_NOVENDOR_OBSMirror.$($layerSourceHash.Substring(0, 12).ToLowerInvariant()).dll"
$versionedLayerPath = Join-Path $layerInstall $versionedLayerName
Copy-Item -LiteralPath $layerDll -Destination $versionedLayerPath -Force

# OpenXR applications keep their loaded layer DLL open. Install immutable,
# hash-versioned binaries and point the manifest at the new one so an update can
# be staged safely without interrupting the current headset session.
$installedManifest = Join-Path $layerInstall 'XR_APILAYER_NOVENDOR_OBSMirror.json'
$manifestData = Get-Content -LiteralPath $layerManifest -Raw | ConvertFrom-Json
$manifestData.api_layer.library_path = ".\$versionedLayerName"
$manifestData | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $installedManifest -Encoding utf8

foreach ($scriptName in @('Install-Layer.ps1', 'Uninstall-Layer.ps1')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $scriptName) -Destination $layerInstall -Force
}

$pluginDestination = Join-Path $pluginBinDirectory 'win-openxr.dll'
if (-not $SkipPluginInstall) {
    $pluginSourceHash = Get-Sha256 -LiteralPath $PluginBinary
    $pluginAlreadyCurrent = (Test-Path -LiteralPath $pluginDestination -PathType Leaf) -and
        ((Get-Sha256 -LiteralPath $pluginDestination) -eq $pluginSourceHash)
    if ($runningOBS -and -not $pluginAlreadyCurrent) {
        Write-Warning 'OBS is running; close it before installing the updated plugin binary.'
    } elseif ($pluginAlreadyCurrent) {
        Write-Verbose "OBS plugin is already current; skipping the in-use DLL copy."
    } else {
        Copy-Item -LiteralPath $PluginBinary -Destination $pluginDestination -Force
        Copy-Item -Path (Join-Path $repoRoot 'OBSPlugin\win-openxr\data\*') `
            -Destination $pluginDataDirectory -Recurse -Force
    }
}

& (Join-Path $PSScriptRoot 'Install-Layer.ps1') -Scope CurrentUser `
    -ManifestPath $installedManifest

[pscustomobject]@{
    LayerManifest = $installedManifest
    LayerBinary = $versionedLayerPath
    LayerSHA256 = Get-Sha256 -LiteralPath $versionedLayerPath
    OBSPlugin = if (Test-Path -LiteralPath $pluginDestination -PathType Leaf) { $pluginDestination } else { $null }
    PluginSHA256 = if (Test-Path -LiteralPath $pluginDestination -PathType Leaf) {
        Get-Sha256 -LiteralPath $pluginDestination
    } else { $null }
}
