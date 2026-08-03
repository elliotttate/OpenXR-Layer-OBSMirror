[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [string]$OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$project = Join-Path $repoRoot 'ControlCenter\OBSMirror.ControlCenter.csproj'
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repoRoot "bin\x64\$Configuration\ControlCenter"
}
$output = [IO.Path]::GetFullPath($OutputDirectory)

& dotnet publish $project -c $Configuration -p:Platform=x64 -r win-x64 `
    --self-contained true -o $output `
    --source https://api.nuget.org/v3/index.json
if ($LASTEXITCODE -ne 0) {
    throw "Control Center publish failed (exit $LASTEXITCODE)."
}

$executable = Join-Path $output 'OBSMirror.ControlCenter.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Published Control Center was not found: $executable"
}

[pscustomobject]@{
    Configuration = $Configuration
    Executable = $executable
    SHA256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $executable).Hash
}
