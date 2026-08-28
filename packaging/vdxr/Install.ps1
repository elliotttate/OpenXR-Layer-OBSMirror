[CmdletBinding()]
param(
    [string]$OBSInstallPath = 'C:\Program Files\obs-studio',
    [switch]$ValidateOnly,
    [switch]$InstallSharedPlugin
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Install-SharedOBSPlugin {
    param(
        [Parameter(Mandatory)] [string]$PluginSource,
        [Parameter(Mandatory)] [string]$PluginDataSource
    )

    $pluginDirectory = Join-Path $env:ProgramData 'obs-studio\plugins\win-openxr'
    $pluginBin = Join-Path $pluginDirectory 'bin\64bit'
    $pluginData = Join-Path $pluginDirectory 'data'
    New-Item -ItemType Directory -Force -Path $pluginBin, $pluginData | Out-Null
    Copy-Item -LiteralPath $PluginSource -Destination (Join-Path $pluginBin 'win-openxr.dll') -Force
    Copy-Item -Path (Join-Path $PluginDataSource '*') -Destination $pluginData -Recurse -Force
}

if (-not $ValidateOnly -and (Get-Process -Name obs64 -ErrorAction SilentlyContinue)) {
    throw 'OBS is running. Close it before installing the source plugin.'
}

$obsExe = Join-Path $OBSInstallPath 'bin\64bit\obs64.exe'
if (-not (Test-Path -LiteralPath $obsExe -PathType Leaf)) {
    throw "OBS Studio was not found at $OBSInstallPath"
}
$obsVersion = [Version](Get-Item -LiteralPath $obsExe).VersionInfo.ProductVersion
if ($obsVersion -ne [Version]'32.2.0') {
    throw "This plugin is ABI-matched to OBS 32.2.0; installed OBS is $obsVersion. Download a build matching your OBS version."
}

$vdExe = 'C:\Program Files\Virtual Desktop Streamer\VirtualDesktop.Streamer.exe'
$vdVersion = $null
if (Test-Path -LiteralPath $vdExe -PathType Leaf) {
    $vdVersion = [Version](Get-Item -LiteralPath $vdExe).VersionInfo.ProductVersion
    if ($vdVersion -lt [Version]'1.34.22.0') {
        throw "Virtual Desktop 1.34.22 or newer is required; installed version is $vdVersion."
    }
}

$layerSource = Join-Path $PSScriptRoot 'bin\XR_APILAYER_NOVENDOR_OBSMirror.dll'
$manifestSource = Join-Path $PSScriptRoot 'bin\XR_APILAYER_NOVENDOR_OBSMirror.json'
$pluginSource = Join-Path $PSScriptRoot 'obs-plugin\bin\64bit\win-openxr.dll'
$pluginDataSource = Join-Path $PSScriptRoot 'obs-plugin\data'
$buildInfoSource = Join-Path $PSScriptRoot 'BUILD-INFO.json'
foreach ($required in @($layerSource, $manifestSource, $pluginSource, $pluginDataSource, $buildInfoSource)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Package file is missing: $required"
    }
}

$manifest = Get-Content -Raw -LiteralPath $manifestSource | ConvertFrom-Json
if ($manifest.api_layer.name -ne 'XR_APILAYER_NOVENDOR_OBSMirror') {
    throw 'The OpenXR layer manifest is invalid.'
}

$buildInfo = Get-Content -Raw -LiteralPath $buildInfoSource | ConvertFrom-Json
$layerHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $layerSource).Hash.ToUpperInvariant()
$pluginHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $pluginSource).Hash.ToUpperInvariant()
$expectedLayerHash = ([string]$buildInfo.openxr_layer_sha256).ToUpperInvariant()
$expectedPluginHash = ([string]$buildInfo.obs_plugin_sha256).ToUpperInvariant()
if ($expectedLayerHash -notmatch '^[0-9A-F]{64}$' -or $expectedPluginHash -notmatch '^[0-9A-F]{64}$') {
    throw 'BUILD-INFO.json does not contain valid SHA-256 digests.'
}
if ($layerHash -ne $expectedLayerHash) {
    throw "OpenXR layer checksum mismatch. Expected $expectedLayerHash; found $layerHash."
}
if ($pluginHash -ne $expectedPluginHash) {
    throw "OBS plugin checksum mismatch. Expected $expectedPluginHash; found $pluginHash."
}

if ($ValidateOnly) {
    [pscustomobject]@{
        Status = 'Package validation passed'
        VirtualDesktop = if ($vdVersion) { $vdVersion.ToString() } else { 'not detected' }
        OBS = $obsVersion.ToString()
        LayerSHA256 = $layerHash
        PluginSHA256 = $pluginHash
    }
    return
}

# This internal mode is the only work performed by the elevated process. The
# OpenXR layer is deliberately registered later by the original user process,
# so alternate administrator credentials cannot redirect HKCU/LOCALAPPDATA.
if ($InstallSharedPlugin) {
    if (-not (Test-IsAdministrator)) {
        throw 'Installing the shared OBS plugin requires administrator privileges.'
    }
    Install-SharedOBSPlugin -PluginSource $pluginSource -PluginDataSource $pluginDataSource
    return
}

# Complete every shared plugin copy before activating the per-user layer. This
# prevents a copy failure from leaving an active layer with no OBS consumer.
if (Test-IsAdministrator) {
    Install-SharedOBSPlugin -PluginSource $pluginSource -PluginDataSource $pluginDataSource
} else {
    # Start-Process joins ArgumentList into a Windows command line. Double any
    # trailing backslashes so they do not escape the closing quote.
    $quotedOBSInstallPath = '"{0}"' -f ($OBSInstallPath -replace '(\\+)$', '$1$1')
    $arguments = @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', ('"{0}"' -f $PSCommandPath),
        '-OBSInstallPath', $quotedOBSInstallPath,
        '-InstallSharedPlugin'
    )
    Write-Host 'Administrator approval is required to install the shared OBS plugin.' -ForegroundColor Yellow
    $elevated = Start-Process -FilePath 'powershell.exe' -ArgumentList $arguments -Verb RunAs -Wait -PassThru
    if ($elevated.ExitCode -ne 0) {
        throw "Shared OBS plugin installation failed or was cancelled (exit $($elevated.ExitCode))."
    }
}

$layerDirectory = Join-Path $env:LOCALAPPDATA 'OpenXR-OBSMirror'
New-Item -ItemType Directory -Force -Path $layerDirectory | Out-Null
$hash = $layerHash.ToLowerInvariant()
$versionedName = "XR_APILAYER_NOVENDOR_OBSMirror.$($hash.Substring(0, 12)).dll"
$versionedLayer = Join-Path $layerDirectory $versionedName
Copy-Item -LiteralPath $layerSource -Destination $versionedLayer -Force

$manifest.api_layer.library_path = ".\$versionedName"
$installedManifest = Join-Path $layerDirectory 'XR_APILAYER_NOVENDOR_OBSMirror.json'
[IO.File]::WriteAllText(
    $installedManifest,
    ($manifest | ConvertTo-Json -Depth 10),
    [Text.UTF8Encoding]::new($false))

$registryPath = 'HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit'
New-Item -Path $registryPath -Force | Out-Null
New-ItemProperty -Path $registryPath -Name $installedManifest -PropertyType DWord -Value 0 -Force | Out-Null

# OBS 32.2.0 does not discover this older per-user plugin location. Remove only
# this product's exact legacy folder so Plugin Manager does not retain a stale
# "Missing Plugins" entry after upgrading from 1.0.0.
$legacyPluginDirectory = Join-Path $env:APPDATA 'obs-studio\plugins\win-openxr'
if (Test-Path -LiteralPath $legacyPluginDirectory) {
    Remove-Item -LiteralPath $legacyPluginDirectory -Recurse -Force
}

[pscustomobject]@{
    VirtualDesktop = if ($vdVersion) { $vdVersion.ToString() } else { 'not detected' }
    OBS = $obsVersion.ToString()
    Layer = $versionedLayer
    OBSPlugin = Join-Path $env:ProgramData 'obs-studio\plugins\win-openxr\bin\64bit\win-openxr.dll'
    LayerSHA256 = $layerHash
    PluginSHA256 = $pluginHash
}

Write-Host 'Installed. Restart OBS and the OpenXR application, then add VR Mirror Capture in OBS.' -ForegroundColor Green
