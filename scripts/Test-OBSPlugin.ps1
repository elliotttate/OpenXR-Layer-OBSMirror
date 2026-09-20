[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$OBSInstallPath,

    [Parameter(Mandatory)]
    [string]$PortablePackage
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$runtimeDirectory = Join-Path $OBSInstallPath 'bin\64bit'
$testExecutable = Join-Path $runtimeDirectory 'obs-plugin-test.exe'
# Place the executable beside the runtime so Windows resolves its obs.dll and
# transitive dependencies from this OBS version, not another installed copy.
Copy-Item (Join-Path $repoRoot 'obj\OBSPlugin\Release\obs-plugin-test.exe') $testExecutable -Force
$testPackage = Join-Path $repoRoot 'obj\OBSPlugin\package-test'
Expand-Archive -LiteralPath $PortablePackage -DestinationPath $testPackage -Force
$payload = Join-Path $testPackage 'OpenXR OBS Mirror'
Write-Host "Testing packaged plugin against OBS $((Get-Item (Join-Path $runtimeDirectory 'obs64.exe')).VersionInfo.ProductVersion)"
& $testExecutable (Join-Path $payload 'bin\x64\Release\OBS_Plugin\win-openxr.dll') `
    (Join-Path $payload 'OBSPlugin\win-openxr\data')
if ($LASTEXITCODE -ne 0) { throw "OBS source registration test failed (exit $LASTEXITCODE)." }
