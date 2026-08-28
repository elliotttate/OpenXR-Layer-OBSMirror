[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^[^/]+/[^/]+$')]
    [string]$Repository,
    [string]$Version = '1.0.1',
    [switch]$CreateRepository,
    [switch]$Public
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$gh = Get-Command gh -ErrorAction SilentlyContinue
if (-not $gh) {
    throw 'GitHub CLI is required. Install it with: winget install --id GitHub.cli'
}
& $gh.Source auth status
if ($LASTEXITCODE -ne 0) { throw 'Authenticate first with: gh auth login' }

$repoRoot = Split-Path -Parent $PSScriptRoot
$safeRepo = $repoRoot.Replace('\', '/')
$tag = "vdxr-v$Version"
$archive = Join-Path $repoRoot "artifacts\vdxr\VDXR-OBS-Mirror-$Version-win64.zip"
$checksums = Join-Path $repoRoot 'artifacts\vdxr\SHA256SUMS.txt'
$notes = Join-Path $repoRoot 'docs\release-notes\v1.0.1-vdxr.md'
foreach ($file in @($archive, $checksums, $notes)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing release file: $file" }
}

if ($CreateRepository) {
    $visibility = if ($Public) { '--public' } else { '--private' }
    if ($PSCmdlet.ShouldProcess($Repository, "Create $visibility GitHub repository")) {
        & $gh.Source repo create $Repository $visibility --source $repoRoot --remote fork --push
        if ($LASTEXITCODE -ne 0) { throw 'GitHub repository creation failed.' }
    }
}

$branch = (& git -c "safe.directory=$safeRepo" -C $repoRoot branch --show-current).Trim()
if (-not $branch) { throw 'The local repository has no current branch.' }
if ($PSCmdlet.ShouldProcess("$Repository/$branch", 'Push source branch')) {
    & git -c "safe.directory=$safeRepo" -C $repoRoot push -u fork $branch
    if ($LASTEXITCODE -ne 0) { throw 'Git push failed.' }
}

if ($PSCmdlet.ShouldProcess("$Repository release $tag", 'Create public GitHub release and upload assets')) {
    & $gh.Source release create $tag $archive $checksums `
        --repo $Repository --title "VDXR OBS Mirror $Version" `
        --notes-file $notes --latest
    if ($LASTEXITCODE -ne 0) { throw 'GitHub release creation failed.' }
}
