#!/usr/bin/env pwsh
#Requires -Version 5.1
# ============================================================================
# The mod's canonical version, read and written in one place.
# ============================================================================
# src/version.h is the single source of truth. The packager names the ZIPs from
# it and stamps launcher-manifest.json with it, release-nightly.ps1 labels the
# nightly with it, release.ps1 mirrors it into scripts/install.cmd and
# CMakeLists.txt, and release.yml re-reads it to check the pushed tag agrees.
#
# Those five readers used to be three different parsers - Get-Content plus
# -match, Get-ProjectVersion, and Select-String - each carrying its own copy of
# the path and the regex. The parse happens here now, through the same
# Get-ProjectVersion that cameraunlock-core's release-mod.yml calls, so a local
# read and a CI read are the same code and cannot disagree.
# ============================================================================

Set-StrictMode -Version Latest

# No -Force: a forced nested import REMOVES ReleaseWorkflow from the calling
# script's scope before re-importing it privately here, which took
# Copy-SharedBundle and Resolve-ReleaseVersion away from the packager and the
# release script that had just imported them.
Import-Module (Join-Path (Split-Path -Parent $PSScriptRoot) 'cameraunlock-core\powershell\ReleaseWorkflow.psm1')

# Byte-identical to release.yml's version-pattern input. CI fails the release
# when the tag and this capture disagree, so the two must stay the same string.
$script:VersionStringPattern = 'HEADTRACKING_VERSION_STRING\s+"([^"]+)"'

# The shape Resolve-ReleaseVersion writes, asserted on both the read and the
# write. version.h is a hand-editable header and its value does not stay a
# display string: it becomes the release ZIP filenames, the
# launcher-manifest.json version field and the git tag CI builds from. A
# leading 'v' or a stray space passes the capture regex above, so without this
# the first thing that objects is release.yml's tag-vs-file comparison, by
# which point the tag is pushed.
$script:VersionValuePattern = '^\d+\.\d+\.\d+(-[a-zA-Z0-9.]+)?$'

function Get-ModVersionPath {
    [OutputType([string])]
    param([Parameter(Mandatory = $true)][string]$ProjectRoot)

    return (Join-Path $ProjectRoot 'src\version.h')
}

function Get-ModVersion {
    [OutputType([string])]
    param([Parameter(Mandatory = $true)][string]$ProjectRoot)

    $path = Get-ModVersionPath -ProjectRoot $ProjectRoot
    $version = Get-ProjectVersion -Source regex -Path $path -Pattern $script:VersionStringPattern
    if ($version -notmatch $script:VersionValuePattern) {
        throw "HEADTRACKING_VERSION_STRING in $path is '$version', which is not X.Y.Z[-prerelease]. The packager, the launcher manifest and the release tag are all named from it."
    }
    return $version
}

# Rewrite one version literal in one file. The caller supplies the pattern that
# locates it, so a rename of the literal fails loudly here rather than writing
# nothing and letting the release carry a stale version through to the tag.
function Update-VersionLiteral {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][string]$Replacement
    )

    # ReadAllText/WriteAllText rather than Get-/Set-Content: install.cmd is CRLF
    # and the packager refuses to ship it otherwise, so a rewrite must leave the
    # file's existing line endings exactly as it found them.
    $raw = [System.IO.File]::ReadAllText($Path)
    if (-not [regex]::IsMatch($raw, $Pattern)) {
        throw "Pattern '$Pattern' matched nothing in $Path - the version was not updated."
    }
    [System.IO.File]::WriteAllText($Path, [regex]::Replace($raw, $Pattern, $Replacement))
}

function Set-ModVersion {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$Version
    )

    if ($Version -notmatch $script:VersionValuePattern) {
        throw "Refusing to write version '$Version' - it is not X.Y.Z[-prerelease]."
    }

    $path = Get-ModVersionPath -ProjectRoot $ProjectRoot
    # Resolve-ReleaseVersion accepts X.Y.Z[-prerelease], and the three numeric
    # macros are ints - a '1.0.0-rc1' would otherwise write PATCH as '0-rc1'.
    # Only the string literal carries the prerelease.
    $parts = $Version.Split('-')[0].Split('.')
    Update-VersionLiteral -Path $path -Replacement "`${1}$($parts[0])" `
        -Pattern '(?m)^(#define HEADTRACKING_VERSION_MAJOR\s+)\d+'
    Update-VersionLiteral -Path $path -Replacement "`${1}$($parts[1])" `
        -Pattern '(?m)^(#define HEADTRACKING_VERSION_MINOR\s+)\d+'
    Update-VersionLiteral -Path $path -Replacement "`${1}$($parts[2])" `
        -Pattern '(?m)^(#define HEADTRACKING_VERSION_PATCH\s+)\d+'
    Update-VersionLiteral -Path $path -Replacement "HEADTRACKING_VERSION_STRING `"$Version`"" `
        -Pattern $script:VersionStringPattern
}

Export-ModuleMember -Function Get-ModVersionPath, Get-ModVersion, Set-ModVersion, Update-VersionLiteral
