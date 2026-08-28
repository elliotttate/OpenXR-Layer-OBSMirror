[CmdletBinding(SupportsShouldProcess)]
param(
    [switch]$RemoveSharedPlugin
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$scriptCmdlet = $PSCmdlet

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Remove-EmptyDirectory {
    param([Parameter(Mandatory)] [string]$Path)
    if ((Test-Path -LiteralPath $Path -PathType Container) -and
        -not (Get-ChildItem -LiteralPath $Path -Force | Select-Object -First 1) -and
        $scriptCmdlet.ShouldProcess($Path, 'Remove empty plugin directory')) {
        Remove-Item -LiteralPath $Path -Force
    }
}

function Remove-SharedOBSPluginFiles {
    param([Parameter(Mandatory)] [string]$PluginDirectory)

    # openvr_api.dll is shipped only by the full OBS Mirror package. Its
    # presence means the shared win-openxr.dll and data are jointly owned, so
    # the slim VDXR uninstaller must leave the complete full install intact.
    $fullInstallMarker = Join-Path $PluginDirectory 'bin\64bit\openvr_api.dll'
    if (Test-Path -LiteralPath $fullInstallMarker -PathType Leaf) {
        Write-Warning 'Full OpenXR/SteamVR OBS Mirror installation detected; leaving its shared plugin files installed.'
        return
    }

    $ownedFiles = @(
        (Join-Path $PluginDirectory 'bin\64bit\win-openxr.dll'),
        (Join-Path $PluginDirectory 'data\locale\en-US.ini'),
        (Join-Path $PluginDirectory 'data\win_openxrmirror-presets.ini')
    )
    foreach ($file in $ownedFiles) {
        if ((Test-Path -LiteralPath $file -PathType Leaf) -and
            $scriptCmdlet.ShouldProcess($file, 'Remove VDXR package file')) {
            Remove-Item -LiteralPath $file -Force
        }
    }

    foreach ($directory in @(
        (Join-Path $PluginDirectory 'bin\64bit'),
        (Join-Path $PluginDirectory 'bin'),
        (Join-Path $PluginDirectory 'data\locale'),
        (Join-Path $PluginDirectory 'data'),
        $PluginDirectory
    )) {
        Remove-EmptyDirectory -Path $directory
    }
}

if (-not $WhatIfPreference -and (Get-Process -Name obs64 -ErrorAction SilentlyContinue)) {
    throw 'OBS is running. Close it before uninstalling the source plugin.'
}

$layerDirectory = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'OpenXR-OBSMirror'))
$sharedPluginDirectory = [IO.Path]::GetFullPath((Join-Path $env:ProgramData 'obs-studio\plugins\win-openxr'))
$legacyPluginDirectory = [IO.Path]::GetFullPath((Join-Path $env:APPDATA 'obs-studio\plugins\win-openxr'))
$expectedLayer = [IO.Path]::GetFullPath($env:LOCALAPPDATA).TrimEnd('\') + '\OpenXR-OBSMirror'
$expectedSharedPlugin = [IO.Path]::GetFullPath($env:ProgramData).TrimEnd('\') + '\obs-studio\plugins\win-openxr'
$expectedLegacyPlugin = [IO.Path]::GetFullPath($env:APPDATA).TrimEnd('\') + '\obs-studio\plugins\win-openxr'
if (-not $layerDirectory.Equals($expectedLayer, [StringComparison]::OrdinalIgnoreCase) -or
    -not $sharedPluginDirectory.Equals($expectedSharedPlugin, [StringComparison]::OrdinalIgnoreCase) -or
    -not $legacyPluginDirectory.Equals($expectedLegacyPlugin, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Resolved install paths did not match the expected product directories.'
}

if ($RemoveSharedPlugin) {
    if (-not $WhatIfPreference -and -not (Test-IsAdministrator)) {
        throw 'Removing the shared OBS plugin requires administrator privileges.'
    }
    Remove-SharedOBSPluginFiles -PluginDirectory $sharedPluginDirectory
    return
}

# Remove the shared files first for a real uninstall. If UAC is cancelled or
# the elevated removal fails, the current user's layer remains fully usable.
# WhatIf stays in-process and previews both shared and per-user operations.
if ($WhatIfPreference -or (Test-IsAdministrator)) {
    Remove-SharedOBSPluginFiles -PluginDirectory $sharedPluginDirectory
} else {
    $arguments = @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', ('"{0}"' -f $PSCommandPath),
        '-RemoveSharedPlugin'
    )
    Write-Host 'Administrator approval is required to remove the shared OBS plugin.' -ForegroundColor Yellow
    if (-not $PSCmdlet.ShouldProcess($sharedPluginDirectory, 'Remove shared OBS plugin')) {
        return
    }
    $elevated = Start-Process -FilePath 'powershell.exe' -ArgumentList $arguments -Verb RunAs -Wait -PassThru
    if ($elevated.ExitCode -ne 0) {
        throw "Shared OBS plugin removal failed or was cancelled (exit $($elevated.ExitCode))."
    }
}

$installedManifest = Join-Path $layerDirectory 'XR_APILAYER_NOVENDOR_OBSMirror.json'
$registryPath = 'HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit'
if (Test-Path -LiteralPath $registryPath) {
    $properties = (Get-ItemProperty -LiteralPath $registryPath).PSObject.Properties
    foreach ($property in $properties) {
        if (-not $property.Name.StartsWith('PS') -and
            $property.Name.Equals($installedManifest, [StringComparison]::OrdinalIgnoreCase) -and
            $PSCmdlet.ShouldProcess($property.Name, 'Unregister OpenXR mirror layer')) {
            Remove-ItemProperty -LiteralPath $registryPath -Name $property.Name -Force
        }
    }
}

foreach ($directory in @($layerDirectory, $legacyPluginDirectory)) {
    if ((Test-Path -LiteralPath $directory) -and
        $PSCmdlet.ShouldProcess($directory, 'Remove per-user mirror files')) {
        Remove-Item -LiteralPath $directory -Recurse -Force
    }
}

if ($WhatIfPreference) {
    Write-Host 'VDXR OBS Mirror uninstall dry run completed; nothing was removed.' -ForegroundColor Cyan
} else {
    Write-Host 'VDXR OBS Mirror was removed.' -ForegroundColor Green
}
