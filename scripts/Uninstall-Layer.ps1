[CmdletBinding(SupportsShouldProcess)]
param(
    [ValidateSet('CurrentUser', 'AllUsers')]
    [string]$Scope = 'CurrentUser',

    [string]$ManifestPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$registryPath = if ($Scope -eq 'AllUsers') {
    'HKLM:\Software\Khronos\OpenXR\1\ApiLayers\Implicit'
} else {
    'HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit'
}

if ($Scope -eq 'AllUsers') {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'AllUsers uninstall requires an elevated PowerShell session.'
    }
}

if (-not (Test-Path -LiteralPath $registryPath)) {
    Write-Output "No OpenXR implicit-layer registry key exists for $Scope."
    return
}

$manifestName = 'XR_APILAYER_NOVENDOR_OBSMirror.json'
$manifestFullPath = if ($ManifestPath) { [IO.Path]::GetFullPath($ManifestPath) } else { $null }
$propertyNames = @(
    (Get-ItemProperty -LiteralPath $registryPath).PSObject.Properties |
        ForEach-Object { $_.Name }
)
$removed = 0

foreach ($propertyName in $propertyNames) {
    $matchesManifest = if ($manifestFullPath) {
        $propertyName -eq $manifestFullPath
    } else {
        [IO.Path]::GetFileName($propertyName) -eq $manifestName
    }

    if (-not $propertyName.StartsWith('PS') -and $matchesManifest -and
        $PSCmdlet.ShouldProcess($registryPath, "Remove $propertyName")) {
        Remove-ItemProperty -LiteralPath $registryPath -Name $propertyName -Force
        $removed++
    }
}

Write-Output "Removed $removed OpenXR OBS Mirror registration(s) for $Scope."
