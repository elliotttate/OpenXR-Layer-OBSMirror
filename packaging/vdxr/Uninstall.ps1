[CmdletBinding(SupportsShouldProcess)]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-IsAdministrator)) {
    Write-Host 'Administrator approval is required to remove the shared OBS plugin.' -ForegroundColor Yellow
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"{0}"' -f $PSCommandPath))
    $elevated = Start-Process -FilePath 'powershell.exe' -ArgumentList $arguments -Verb RunAs -Wait -PassThru
    exit $elevated.ExitCode
}

if (Get-Process -Name obs64 -ErrorAction SilentlyContinue) {
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

$registryPath = 'HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit'
if (Test-Path -LiteralPath $registryPath) {
    $properties = (Get-ItemProperty -LiteralPath $registryPath).PSObject.Properties
    foreach ($property in $properties) {
        if (-not $property.Name.StartsWith('PS') -and
            $property.Name.StartsWith($layerDirectory, [StringComparison]::OrdinalIgnoreCase)) {
            if ($PSCmdlet.ShouldProcess($property.Name, 'Unregister OpenXR mirror layer')) {
                Remove-ItemProperty -LiteralPath $registryPath -Name $property.Name -Force
            }
        }
    }
}

foreach ($directory in @($layerDirectory, $sharedPluginDirectory, $legacyPluginDirectory)) {
    if ((Test-Path -LiteralPath $directory) -and $PSCmdlet.ShouldProcess($directory, 'Remove mirror files')) {
        Remove-Item -LiteralPath $directory -Recurse -Force
    }
}

Write-Host 'VDXR OBS Mirror was removed.' -ForegroundColor Green
