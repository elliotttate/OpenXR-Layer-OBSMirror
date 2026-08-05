[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [string]$OutputDirectory,

    # Release builds pass the version so the app stamps the release it belongs
    # to. The app compares this against the GitHub release list to decide
    # whether an update exists, so a stale value makes it offer an update it
    # has already installed, forever. Omitted for developer builds, which fall
    # back to the version in the project file.
    [string]$Version,

    [string]$FileVersion
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$project = Join-Path $repoRoot 'ControlCenter\OBSMirror.ControlCenter.csproj'
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repoRoot "bin\x64\$Configuration\ControlCenter"
}
$output = [IO.Path]::GetFullPath($OutputDirectory)

$versionArgs = @()
if ($Version) {
    $versionArgs += "-p:Version=$Version"
    $versionArgs += "-p:InformationalVersion=$Version"
}
if ($FileVersion) {
    $versionArgs += "-p:AssemblyVersion=$FileVersion"
    $versionArgs += "-p:FileVersion=$FileVersion"
}

& dotnet publish $project -c $Configuration -p:Platform=x64 -r win-x64 `
    --self-contained true -o $output @versionArgs `
    --source https://api.nuget.org/v3/index.json
if ($LASTEXITCODE -ne 0) {
    throw "Control Center publish failed (exit $LASTEXITCODE)."
}

$executable = Join-Path $output 'OBSMirror.ControlCenter.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Published Control Center was not found: $executable"
}

# The app reads its own version to decide whether a GitHub release is newer, so
# a build that shipped the wrong one would offer an update it already has.
if ($Version) {
    $stamped = (Get-Item -LiteralPath $executable).VersionInfo.ProductVersion
    if ($stamped -notlike "$Version*") {
        throw "Control Center reports version '$stamped' but this build is '$Version'."
    }
}

[pscustomobject]@{
    Configuration = $Configuration
    Executable = $executable
    SHA256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $executable).Hash
}
