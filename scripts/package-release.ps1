#!/usr/bin/env pwsh
#Requires -Version 5.1
# ============================================================================
# Stage and zip the two release archives for Black Mesa Head Tracking.
# ============================================================================
# Usage: pixi run package   (runs build-release first)
#
# Consumes whatever is committed under vendor/ - it never refreshes the loader
# and never reaches the network, so CI packages exactly what the repo holds.
# Bumping the loader is `pixi run update-deps` plus a commit, done by hand.
#
# No prompts, no confirmations: this runs unattended under `pixi run` and from
# CI. Every precondition fails fast with a non-zero exit instead.
# ============================================================================

# ValidateSet, not a bare string: $Configuration is concatenated straight into
# bin\<Configuration>, so anything else is a path the packager was never meant
# to read from. Same set deploy.ps1 accepts.
param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release')

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path "$PSScriptRoot\.."
$binDir   = Join-Path $repoRoot "bin\$Configuration"
$outDir   = Join-Path $repoRoot 'release'

Import-Module (Join-Path $repoRoot 'cameraunlock-core\powershell\ReleaseWorkflow.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'ModVersion.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'PackageGuards.psm1') -Force

# A staging directory is rebuilt from scratch every run: a leftover tree from an
# aborted package would otherwise be zipped along with this one's files.
function New-StageDirectory {
    [OutputType([string])]
    param([Parameter(Mandatory = $true)][string]$Path)

    if (Test-Path $Path) { Remove-Item $Path -Recurse -Force }
    New-Item -ItemType Directory -Path $Path | Out-Null
    return $Path
}

# Zip a finished stage and take the stage down, so `release\` holds archives
# and never a half-built tree that the next run would have to reason about.
function Write-StageArchive {
    param(
        [Parameter(Mandatory = $true)][string]$StageDir,
        [Parameter(Mandatory = $true)][string]$ZipPath,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
    Compress-Archive -Path "$StageDir\*" -DestinationPath $ZipPath
    Remove-Item $StageDir -Recurse -Force
    Write-Host ("Packaged {0,-10} {1}" -f "${Label}:", $ZipPath) -ForegroundColor Green
}

if (-not (Test-Path $binDir)) {
    throw "Build output not found at $binDir. Run: pixi run build-release"
}

# src\version.h is canonical; ModVersion.psm1 owns the path and the pattern,
# and reads it through the same Get-ProjectVersion release.yml uses.
$version = Get-ModVersion -ProjectRoot $repoRoot

$modName = 'BlackMesaHeadTracking'
$modSlug = 'black-mesa-headtracking'

# Vendored Ultimate ASI Loader: install-time source of truth, extracted to
# <game>\winmm.dll by install.cmd, at the game root beside bms.exe - a proxy in
# <game>\bin\ never loads under Steam. dinput8.dll and its LICENSE are both
# mandatory - the loader is MIT, and its license has to travel with the binary.
# README.md is the vendoring provenance note and travels with it.
$vendorFiles = @('dinput8.dll', 'LICENSE', 'README.md') |
    ForEach-Object { Join-Path $repoRoot "vendor\ultimate-asi-loader\$_" }
foreach ($f in $vendorFiles) {
    if (-not (Test-Path $f)) { throw "Missing vendored loader file: $f. Run: pixi run update-deps" }
}
$loaderPath = Join-Path $repoRoot 'vendor\ultimate-asi-loader\dinput8.dll'
Assert-I386Image -Path $loaderPath

# The installer ZIP redistributes that binary, and the upstream x86 loader
# carries binkw32.dll (RAD Game Tools, proprietary), wndmode.dll and
# vorbisfile.dll as RCDATA resources. None of the three is ours to ship, so a
# loader that still has them never reaches a release.
# See vendor/ultimate-asi-loader/README.md.
& (Join-Path $PSScriptRoot 'strip-loader-payload.ps1') -Path $loaderPath -VerifyOnly

# The loader is resolved first so the mod payload can be compared against it:
# both are i386 PEs, so nothing else downstream can tell a real build from a
# copy of the loader renamed to $modName.asi.
$asiPath = Join-Path $binDir "$modName.asi"
if (-not (Test-Path $asiPath)) { throw "Missing build output: $asiPath" }
Assert-I386Image -Path $asiPath
Assert-NotVendoredLoader -Path $asiPath -LoaderPath $loaderPath

# Everything else the ZIPs take out of the repo is validated here, before a
# single staging file is written: a missing manifest or an LF-mangled wrapper
# then costs no disk churn and leaves no half-built stage tree behind.

# cmd.exe needs CRLF: an LF batch file mis-parses labels and parenthesised
# blocks, and both wrappers are built out of those, so the failure is an
# installer that exits having done nothing. .gitattributes marks these
# eol=crlf, but the packager copies from the WORKING TREE, which is what a file
# written by an LF-defaulting tool ships as. Checked rather than repaired,
# because a wrapper that reached this point with LF endings was edited by
# something that will do it again. A lone LF is the test, not "no CRLF at all":
# a mixed-ending file mis-parses exactly the same way.
$wrappers = @('install.cmd', 'uninstall.cmd') |
    ForEach-Object { Join-Path $repoRoot "scripts\$_" }
foreach ($w in $wrappers) {
    if (-not (Test-Path $w)) { throw "Missing installer wrapper: $w" }
    if ([IO.File]::ReadAllText($w) -match "(?<!`r)`n") {
        throw "$w has LF line endings. cmd.exe needs CRLF - run: unix2dos $w"
    }
}

# Launcher manifest: the launcher reads launcher-manifest.json from the release
# root to route the install (delivery_mode).
$manifestPath = Join-Path $repoRoot 'launcher-manifest.json'
if (-not (Test-Path $manifestPath)) { throw "Missing manifest: $manifestPath" }

if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }

# ---------- Installer ZIP (GitHub Release) ----------
$installerStage = New-StageDirectory (Join-Path $outDir "$modSlug-installer-stage")

# Mod payload deployed to the game root by install.cmd. The mod creates
# CameraUnlock.ini on first launch, so no config is shipped here.
$pluginsDir = Join-Path $installerStage 'plugins'
New-Item -ItemType Directory -Path $pluginsDir | Out-Null
Copy-Item $asiPath $pluginsDir

$vendorStage = Join-Path $installerStage 'vendor\ultimate-asi-loader'
New-Item -ItemType Directory -Path $vendorStage | Out-Null
Copy-Item $vendorFiles -Destination $vendorStage

Copy-Item $wrappers -Destination $installerStage

# Version is stamped from src/version.h so the manifest stays in lockstep with
# the built binary.
# -Encoding UTF8: PS 5.1's Get-Content falls back to the ANSI codepage for a
# file with no BOM, and the write below deliberately emits BOM-less UTF-8. Read
# without it, a manifest carrying any non-ASCII character (an author name, a
# game title) came back mojibake and was written back out that way.
$manifest = Get-Content $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
$manifest.mod_info.version = $version
# Depth well past anything a manifest nests: past the limit ConvertTo-Json
# replaces the node with its type name rather than failing, so a manifest that
# grew a level would ship silently corrupted.
$manifestJson = $manifest | ConvertTo-Json -Depth 64
# WriteAllText with a BOM-less UTF8 encoder, not Set-Content -Encoding utf8:
# PS 5.1's utf8 emits a BOM, and utf8NoBOM does not exist there. Both consumers
# currently strip a BOM defensively, but the manifest is a contract file and
# should not need the workaround.
[IO.File]::WriteAllText(
    (Join-Path $installerStage 'launcher-manifest.json'),
    $manifestJson,
    (New-Object System.Text.UTF8Encoding $false))

# shared/ is mandatory wherever install.cmd ships: both wrappers resolve the
# game through shared/find-game.ps1 on every run, even when handed an explicit
# path. A ZIP without it fails on the user's first invocation.
Copy-SharedBundle -StagingDir $installerStage

# The vendored loader's MIT notice has to accompany the binaries in this ZIP,
# so a missing one fails the package rather than quietly producing a release
# that cannot be distributed. THIRD-PARTY-NOTICES.md is the list of what the
# archive carries: add an entry when the build starts linking something new,
# and only once the built binary has been checked for it.
Copy-LicenceNotices -StagingDir $installerStage -ProjectRoot $repoRoot `
    -Additional @('README.md', 'CHANGELOG.md')

Write-StageArchive -StageDir $installerStage -Label 'installer' `
    -ZipPath (Join-Path $outDir "$modName-v$version-installer.zip")

# ---------- No Nexus ZIP ----------
# Deliberate, and stated here so the next session does not helpfully add one
# back. This mod's payload lands at the game root, beside bms.exe, and a mod
# manager deploys into one fixed subtree per game. Vortex ships no bundled Black
# Mesa extension, so there is no `queryModPath` to read and nothing establishes
# that a manager can put a file at the game root at all. An archive that a
# manager silently deploys to the wrong place is the worst outcome available:
# it installs cleanly, the game starts normally, no HeadTracking.log is ever
# written, and the report that comes back is "no head tracking".
#
# So this mod is installer-only: one ZIP, install.cmd, no NEXUS_MODS.md, and
# `Publish-NightlyBuild -NoNexusZip` in release-nightly.ps1. If someone verifies
# a route mechanically - by reading queryModPath and any registerModType out of
# an installed Black Mesa game extension - that is the point to reinstate this,
# not before.
