[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(ParameterSetName = 'Enable', Mandatory)]
    [switch]$Enable,

    [Parameter(ParameterSetName = 'Disable', Mandatory)]
    [switch]$Disable,

    [Parameter(ParameterSetName = 'Enable')]
    [ValidateRange(100, 150)]
    [int]$HorizontalPercent = 115,

    [Parameter(ParameterSetName = 'Enable')]
    [ValidateRange(100, 150)]
    [int]$VerticalPercent = 108
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$registryPath = 'HKCU:\Software\OpenXR-OBSMirror'

if ($Disable) {
    if ($PSCmdlet.ShouldProcess($registryPath, 'Disable recording overscan')) {
        if (Test-Path -LiteralPath $registryPath) {
            Set-ItemProperty -LiteralPath $registryPath -Name 'RecordingOverscan' -Value 0 -Type DWord
        }
    }
    Write-Output 'Recording overscan disabled. Restart the VR application for the change to take effect.'
    return
}

if ($PSCmdlet.ShouldProcess($registryPath, "Enable recording overscan ${HorizontalPercent}% x ${VerticalPercent}%")) {
    New-Item -Path $registryPath -Force | Out-Null
    Set-ItemProperty -LiteralPath $registryPath -Name 'RecordingOverscan' -Value 1 -Type DWord
    Set-ItemProperty -LiteralPath $registryPath -Name 'OverscanHorizontalPercent' -Value $HorizontalPercent -Type DWord
    Set-ItemProperty -LiteralPath $registryPath -Name 'OverscanVerticalPercent' -Value $VerticalPercent -Type DWord
}

$extraPixels = [math]::Round((($HorizontalPercent / 100.0) * ($VerticalPercent / 100.0) - 1.0) * 100.0, 1)
Write-Output "Recording overscan enabled: ${HorizontalPercent}% horizontal, ${VerticalPercent}% vertical (~${extraPixels}% more rendered pixels)."
Write-Output 'Restart the VR application for the change to take effect. The headset view is unaffected; OBS receives the wider image.'
