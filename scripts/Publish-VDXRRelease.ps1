[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^[^/]+/[^/]+$')]
    [string]$Repository,
    [string]$Version = '1.0.3',
    [string]$Remote,
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
$branch = (& git -c "safe.directory=$safeRepo" -C $repoRoot branch --show-current).Trim()
if (-not $branch) { throw 'The local repository has no current branch.' }
$repositoryPattern = '(?i)^(?:(?:[a-z][a-z0-9+.-]*://(?:[^/@\s]+@)?github\.com(?::\d+)?/)|(?:[^/@\s]+@)?github\.com[:/])' +
    [regex]::Escape($Repository) + '(?:\.git)?/?$'
$tag = "vdxr-v$Version"
$archive = Join-Path $repoRoot "artifacts\vdxr\VDXR-OBS-Mirror-$Version-win64.zip"
$checksums = Join-Path $repoRoot "artifacts\vdxr\VDXR-OBS-Mirror-$Version-win64-SHA256.txt"
$notes = Join-Path $repoRoot "docs\release-notes\v$Version-vdxr.md"
foreach ($file in @($archive, $checksums, $notes)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing release file: $file" }
}

if ($CreateRepository) {
    $visibility = if ($Public) { '--public' } else { '--private' }
    if (-not $Remote) { $Remote = 'publish' }
    if (& git -c "safe.directory=$safeRepo" -C $repoRoot remote get-url $Remote 2>$null) {
        throw "Git remote '$Remote' already exists; choose another -Remote name."
    }
    if ($PSCmdlet.ShouldProcess($Repository, "Create $visibility GitHub repository")) {
        & $gh.Source repo create $Repository $visibility --source $repoRoot --remote $Remote
        if ($LASTEXITCODE -ne 0) { throw 'GitHub repository creation failed.' }
    } else {
        return
    }
}

if (-not $Remote) {
    foreach ($candidate in (& git -c "safe.directory=$safeRepo" -C $repoRoot remote)) {
        $candidateRawUrls = @(& git -c "safe.directory=$safeRepo" -C $repoRoot remote get-url --push --all $candidate 2>$null)
        $candidateExitCode = $LASTEXITCODE
        $candidateUrls = @($candidateRawUrls | ForEach-Object { $_.Trim() } | Where-Object { $_ })
        $invalidCandidateUrls = @($candidateUrls | Where-Object { $_ -notmatch $repositoryPattern })
        if ($candidateExitCode -eq 0 -and $candidateUrls.Count -gt 0 -and $invalidCandidateUrls.Count -eq 0) {
            $Remote = $candidate
            break
        }
    }
}
if (-not $Remote) {
    throw "No Git remote matches $Repository. Add the remote or pass -Remote explicitly."
}
$remoteRawUrls = @(& git -c "safe.directory=$safeRepo" -C $repoRoot remote get-url --push --all $Remote 2>$null)
$remoteExitCode = $LASTEXITCODE
$remoteUrls = @($remoteRawUrls | ForEach-Object { $_.Trim() } | Where-Object { $_ })
if ($remoteExitCode -ne 0 -or $remoteUrls.Count -eq 0) {
    throw "Git remote '$Remote' does not exist."
}
$invalidRemoteUrls = @($remoteUrls | Where-Object { $_ -notmatch $repositoryPattern })
if ($invalidRemoteUrls.Count -gt 0) {
    throw "Git remote '$Remote' has a push URL that does not point to '$Repository': $($invalidRemoteUrls -join ', ')"
}
if ($PSCmdlet.ShouldProcess("$Repository/$branch", 'Push source branch')) {
    & git -c "safe.directory=$safeRepo" -C $repoRoot push -u $Remote $branch
    if ($LASTEXITCODE -ne 0) { throw 'Git push failed.' }
}

if ($PSCmdlet.ShouldProcess("$Repository release $tag", 'Create public GitHub release and upload assets')) {
    & $gh.Source release create $tag $archive $checksums `
        --repo $Repository --target $branch --title "VDXR OBS Mirror $Version" `
        --notes-file $notes --latest
    if ($LASTEXITCODE -ne 0) { throw 'GitHub release creation failed.' }
}
