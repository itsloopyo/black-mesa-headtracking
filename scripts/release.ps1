#!/usr/bin/env pwsh
#Requires -Version 5.1
<#
.SYNOPSIS
    Release workflow for Black Mesa Head Tracking.

.DESCRIPTION
    1. Validate semver + git state.
    2. Regenerate CHANGELOG.md from conventional commits (via
       cameraunlock-core/powershell/ReleaseWorkflow.psm1).
    3. Bump the version in src/version.h, scripts/install.cmd and CMakeLists.txt.
    4. Build the x86 release.
    5. Commit the version + changelog as "Release v<version>".
    6. Create annotated tag v<version> and push it; CI picks up the tag and
       publishes the GitHub release artifacts.

    Runs unattended end to end. The preconditions in step 1 are the whole
    safety net - there is no confirmation prompt, and nothing here reads stdin.

.EXAMPLE
    pixi run release 1.0.0
    pixi run release patch
    pixi run release nightly
#>
param(
    [Parameter(Position = 0)]
    [string]$Version = '',
    # Ship a release even when there are no user-facing commits since the last
    # tag (writes a maintenance changelog entry instead of aborting).
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptDir      = $PSScriptRoot
$projectDir     = Split-Path -Parent $scriptDir
$installCmdPath = Join-Path $projectDir 'scripts\install.cmd'
$cmakePath      = Join-Path $projectDir 'CMakeLists.txt'
$changelogPath  = Join-Path $projectDir 'CHANGELOG.md'

# Every git call below runs against the CURRENT DIRECTORY, and not only the ones
# written here: Test-CleanGitStatus, Test-GitTagExists and
# New-ChangelogFromCommits all shell out to bare `git` too. Started from another
# directory - a sibling mod repo, a submodule - this script would read that
# repo's branch and tags, then commit, tag and push there while rewriting the
# version files here. Pin the directory once, before anything reads git.
Set-Location -LiteralPath $projectDir

Import-Module (Join-Path $projectDir 'cameraunlock-core\powershell\ReleaseWorkflow.psm1') -Force
Import-Module (Join-Path $scriptDir 'ModVersion.psm1') -Force

$versionPath = Get-ModVersionPath -ProjectRoot $projectDir

# THIRD-PARTY-NOTICES.md names the cameraunlock-core commit compiled into the
# release ZIPs, and bumping the submodule does not touch it. Packaging refuses
# to ship that mismatch, so a bump with no notices edit stopped the release
# here, or in CI once the tag had already been pushed. Re-sync it and let this
# release carry the correction.
#
# Called only once the preconditions below have passed: it writes a commit, and
# a commit must never land off the back of an invocation that then aborts for a
# dirty tree, the wrong branch, an existing tag, or no version argument at all.
function Sync-CoreNotices {
    & git -C $projectDir diff --quiet -- THIRD-PARTY-NOTICES.md
    if ($LASTEXITCODE -ne 0) { throw "THIRD-PARTY-NOTICES.md has uncommitted edits. Commit or discard them, then re-run." }
    # $LASTEXITCODE is only written by a NATIVE command, and sync-core-notices.ps1
    # returns without calling `exit` on its success path. Left as it stood, the
    # check below would be reading the 0 that `git diff --quiet` just set, so a
    # future reordering of these two lines turns the guard into a no-op.
    $global:LASTEXITCODE = 0
    & (Join-Path $projectDir 'cameraunlock-core\scripts\sync-core-notices.ps1') -Repo $projectDir
    if ($LASTEXITCODE -ne 0) { throw "sync-core-notices.ps1 exited $LASTEXITCODE - fix THIRD-PARTY-NOTICES.md before releasing." }
    & git -C $projectDir diff --quiet -- THIRD-PARTY-NOTICES.md
    if ($LASTEXITCODE -ne 0) {
        & git -C $projectDir commit -q -m 'chore: record the cameraunlock-core commit this build compiles' -- THIRD-PARTY-NOTICES.md
        if ($LASTEXITCODE -ne 0) { throw "Could not commit the re-synced THIRD-PARTY-NOTICES.md." }
        Write-Host 'THIRD-PARTY-NOTICES.md re-synced to the pinned cameraunlock-core commit.' -ForegroundColor Yellow
    }
}

# Mirrors New-ChangelogFromCommits' insertion so a -Force maintenance entry
# lands in the same place with the same shape.
function Add-MaintenanceChangelogEntry {
    param([string]$Path, [string]$NewVersion)
    $date = Get-Date -Format 'yyyy-MM-dd'
    $entry = "## [$NewVersion] - $date`n`n### Changed`n`n- Maintenance release (no user-facing changes).`n`n"
    # -Encoding UTF8 / WriteAllText, matching New-ChangelogFromCommits: on
    # PS 5.1 Get-Content and Set-Content both default to the ANSI codepage, so
    # the pair would mangle every non-ASCII character already in the changelog.
    $changelog = Get-Content $Path -Raw -Encoding UTF8
    if ($changelog -match '(?s)(# Changelog.*?)(## \[)') {
        $changelog = $changelog -replace '(?s)(# Changelog.*?\n\n)', "`$1$entry"
    } else {
        $changelog = $changelog -replace '(?s)(# Changelog.*?\n)', "`$1$entry"
    }
    $changelog = $changelog.TrimEnd() + "`n"
    [System.IO.File]::WriteAllText($Path, $changelog, (New-Object System.Text.UTF8Encoding $false))
}

Write-Host ''
Write-Host '=== Black Mesa Head Tracking Release ===' -ForegroundColor Cyan
Write-Host ''

$current = Get-ModVersion -ProjectRoot $projectDir

if ([string]::IsNullOrWhiteSpace($Version)) {
    Write-Host "Current version: $current" -ForegroundColor Yellow
    Write-Host 'Usage: pixi run release <major|minor|patch|nightly|X.Y.Z>'
    exit 0
}

if ($Version -eq 'nightly') {
    # Same stale-exit-code trap as in Sync-CoreNotices: release-nightly.ps1 never
    # calls `exit`, so without the reset this propagates whatever code the last
    # native command inside it happened to leave behind - and on a fresh session
    # $LASTEXITCODE is not set at all, which Set-StrictMode turns into an error.
    # A real failure in there is a terminating error and propagates on its own.
    $global:LASTEXITCODE = 0
    & (Join-Path $scriptDir 'release-nightly.ps1')
    exit $LASTEXITCODE
}

try {
    $Version = Resolve-ReleaseVersion -Argument $Version -CurrentVersion $current
} catch {
    Write-Host "Error: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

$tag = "v$Version"

$branch = git rev-parse --abbrev-ref HEAD
if ($branch -ne 'main') {
    Write-Host "Must be on main branch to release (currently on '$branch')" -ForegroundColor Red
    exit 1
}
if (-not (Test-CleanGitStatus)) {
    Write-Host 'Working tree has uncommitted changes - commit or stash first.' -ForegroundColor Red
    git status --short
    exit 1
}
if (Test-GitTagExists -Tag $tag) {
    Write-Host "Tag '$tag' already exists." -ForegroundColor Red
    exit 1
}

Sync-CoreNotices

Write-Host "Current version: $current" -ForegroundColor Gray
Write-Host "New version:     $Version" -ForegroundColor Green
Write-Host ''

# Step 1 - changelog first, because it is the gate that can fail. Generating it
# before mutating any version file means an abort leaves the tree clean rather
# than stranding a half-applied bump with no tag.
Write-Host 'Generating CHANGELOG from commits...' -ForegroundColor Cyan
$hasTags = git tag -l 2>$null
if (-not $hasTags) {
    $date = Get-Date -Format 'yyyy-MM-dd'
    # WriteAllText with a BOM-less UTF8 encoder, for the reason spelled out in
    # Add-MaintenanceChangelogEntry: PS 5.1's Set-Content defaults to the ANSI
    # codepage, and the next release reads this file back with -Encoding UTF8.
    [System.IO.File]::WriteAllText($changelogPath,
        "# Changelog`n`n## [$Version] - $date`n`nFirst release.`n",
        (New-Object System.Text.UTF8Encoding $false))
} else {
    try {
        # Same pathspecs release.yml passes to generate-release-notes.ps1, so
        # the changelog and the GitHub release notes cover the same commits.
        New-ChangelogFromCommits -ChangelogPath $changelogPath -Version $Version `
            -ArtifactPaths @('src/', 'cameraunlock-core', 'scripts/install.cmd', 'scripts/uninstall.cmd')
    } catch {
        if (-not $Force) {
            Write-Host "Error: $($_.Exception.Message)" -ForegroundColor Red
            Write-Host 'No user-facing changes to release. Re-run with -Force for a maintenance release.' -ForegroundColor Yellow
            exit 1
        }
        Write-Host 'No user-facing commits since last tag - writing maintenance entry (-Force).' -ForegroundColor Yellow
        Add-MaintenanceChangelogEntry -Path $changelogPath -NewVersion $Version
    }
}

# Step 2 - src/version.h is the canonical version: the packager reads it to name
# the ZIPs and to stamp launcher-manifest.json. The other two copies are written
# here so they cannot drift away from it - install.cmd's MOD_VERSION, which the
# installer records in the user's .headtracking-state.json, and CMakeLists.txt's
# project version, which nothing reads but which is the first place a reader
# looks.
Write-Host "Updating src/version.h to $Version..." -ForegroundColor Cyan
Set-ModVersion -ProjectRoot $projectDir -Version $Version

Write-Host "Updating scripts/install.cmd MOD_VERSION to $Version..." -ForegroundColor Cyan
Update-VersionLiteral -Path $installCmdPath `
    -Pattern 'set "MOD_VERSION=[^"]+"' -Replacement "set `"MOD_VERSION=$Version`""

Write-Host "Updating CMakeLists.txt project version to $Version..." -ForegroundColor Cyan
Update-VersionLiteral -Path $cmakePath `
    -Pattern '(?m)^(project\([^)]*VERSION\s+)\d+\.\d+\.\d+' -Replacement "`${1}$Version"

# Step 3 - build
Write-Host 'Building release (x86)...' -ForegroundColor Cyan
Push-Location $projectDir
try {
    pixi run build-release
    if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
} finally {
    Pop-Location
}

# Step 4 - commit named files only, so build artifacts cannot sweep in
Write-Host 'Committing version + changelog...' -ForegroundColor Cyan
git add $versionPath $changelogPath $installCmdPath $cmakePath
if ($LASTEXITCODE -ne 0) { throw 'git add failed - the version bump was not staged.' }
git diff --cached --quiet
if ($LASTEXITCODE -eq 0) {
    Write-Host 'No version/changelog changes - tagging existing HEAD.' -ForegroundColor Yellow
} else {
    git commit -m "Release v$Version"
    if ($LASTEXITCODE -ne 0) { throw 'Commit failed' }
}

# Step 5 - tag + push. Every step is checked, because git failures are exit
# codes rather than exceptions and $ErrorActionPreference does not see them.
# The push order is load-bearing: pushing the tag first, or pushing it after a
# main push that was rejected (a non-fast-forward, a protected branch), lands a
# release tag on a commit that is not on main - and the tag push carries the
# commit's objects with it, so CI happily builds and publishes from it.
Write-Host "Creating tag $tag..." -ForegroundColor Cyan
git tag -a $tag -m "Release $tag"
if ($LASTEXITCODE -ne 0) { throw "Could not create tag $tag." }
git push origin main
if ($LASTEXITCODE -ne 0) { throw "Pushing main failed - the local tag $tag was NOT pushed. Fix the push, then run: git push origin main; git push origin $tag" }
git push origin $tag
if ($LASTEXITCODE -ne 0) { throw "Pushing tag $tag failed - main is pushed, so CI has not been triggered. Re-run: git push origin $tag" }

Write-Host ''
Write-Host "Release $tag pushed - CI will build and publish artifacts." -ForegroundColor Green
