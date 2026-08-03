[CmdletBinding(SupportsShouldProcess)]
param(
    [ValidateSet('CurrentUser', 'AllUsers')]
    [string]$Scope = 'CurrentUser',

    [string]$ManifestPath = (Join-Path $PSScriptRoot 'XR_APILAYER_NOVENDOR_OBSMirror.json')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$manifestFullPath = [IO.Path]::GetFullPath($ManifestPath)
if (-not (Test-Path -LiteralPath $manifestFullPath -PathType Leaf)) {
    throw "OpenXR layer manifest not found: $manifestFullPath"
}

$manifest = Get-Content -Raw -LiteralPath $manifestFullPath | ConvertFrom-Json
$libraryPath = $manifest.api_layer.library_path
if (-not [IO.Path]::IsPathRooted($libraryPath)) {
    $libraryPath = Join-Path (Split-Path -Parent $manifestFullPath) $libraryPath
}
if (-not (Test-Path -LiteralPath $libraryPath -PathType Leaf)) {
    throw "OpenXR layer library referenced by the manifest was not found: $libraryPath"
}

$registryPath = if ($Scope -eq 'AllUsers') {
    'HKLM:\Software\Khronos\OpenXR\1\ApiLayers\Implicit'
} else {
    'HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit'
}

if ($Scope -eq 'AllUsers') {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'AllUsers installation requires an elevated PowerShell session.'
    }
}

if ($PSCmdlet.ShouldProcess($registryPath, "Register $manifestFullPath")) {
    New-Item -Path $registryPath -Force | Out-Null

    $manifestName = [IO.Path]::GetFileName($manifestFullPath)
    $propertyNames = @(
        (Get-ItemProperty -LiteralPath $registryPath).PSObject.Properties |
            ForEach-Object { $_.Name }
    )
    foreach ($propertyName in $propertyNames) {
        if (-not $propertyName.StartsWith('PS') -and
            [IO.Path]::GetFileName($propertyName) -eq $manifestName -and
            $propertyName -ne $manifestFullPath) {
            Remove-ItemProperty -LiteralPath $registryPath -Name $propertyName -Force
        }
    }

    New-ItemProperty -Path $registryPath -Name $manifestFullPath -PropertyType DWord -Value 0 -Force |
        Out-Null
}

Write-Output "Registered OpenXR OBS Mirror for $Scope`: $manifestFullPath"
