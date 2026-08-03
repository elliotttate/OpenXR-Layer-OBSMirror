[CmdletBinding(SupportsShouldProcess)]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

& (Join-Path $PSScriptRoot 'Uninstall-Layer.ps1') -Scope CurrentUser

$layerDirectory = [IO.Path]::GetFullPath(
    (Join-Path $env:LOCALAPPDATA 'OpenXR-OBSMirror'))
$expectedDirectory = [IO.Path]::GetFullPath($env:LOCALAPPDATA).TrimEnd('\') + `
    '\OpenXR-OBSMirror'
if (-not $layerDirectory.Equals($expectedDirectory, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to remove an unexpected layer directory: $layerDirectory"
}

if ((Test-Path -LiteralPath $layerDirectory) -and
    $PSCmdlet.ShouldProcess($layerDirectory, 'Remove installed OpenXR layer files')) {
    Remove-Item -LiteralPath $layerDirectory -Recurse -Force
}
