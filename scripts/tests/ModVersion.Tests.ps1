#!/usr/bin/env pwsh
#Requires -Version 5.1
# ============================================================================
# Tests for scripts/ModVersion.psm1
# ============================================================================
# Run: pixi run test
#
# These lock the behaviour release.ps1, package-release.ps1 and
# release-nightly.ps1 had when each parsed src/version.h its own way, so the
# move to one shared parser is provably not a change: same value on a
# well-formed header, same throw on a missing file and on a missing macro, and
# the same bytes written back for everything the rewrite does not target.
#
# The one deliberate difference from those three parsers is the shape assert on
# the value itself, on both the read and the write, covered below.
#
# No Pester dependency, matching cameraunlock-core/powershell/tests.
# ============================================================================

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path (Split-Path -Parent $PSScriptRoot) 'ModVersion.psm1') -Force

$script:Failures = 0

function Check {
    param([string]$Name, [bool]$Condition, [string]$Detail)
    if ($Condition) {
        Write-Host "PASS  $Name" -ForegroundColor Green
    } else {
        Write-Host "FAIL  $Name - $Detail" -ForegroundColor Red
        $script:Failures++
    }
}

# Returns '' when the action completed, or the terminating error's message when
# it did not - a string either way, so Check's detail never dereferences $null
# under Set-StrictMode.
function Get-ThrownMessage {
    param([scriptblock]$Action)
    try { & $Action | Out-Null; return '' } catch { return $_.Exception.Message }
}

$sandbox = Join-Path ([System.IO.Path]::GetTempPath()) "bmht-modversion-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $sandbox -Force | Out-Null

# The header shape release.ps1 rewrites: three numeric macros and the string
# the packager and release.yml both read. Written CRLF, as src/version.h will
# be checked out under this repo's .gitattributes.
function New-ProjectRoot {
    param([string]$Name, [string]$Version = '1.2.3', [string[]]$Lines)

    $root = Join-Path $sandbox $Name
    New-Item -ItemType Directory -Path (Join-Path $root 'src') -Force | Out-Null
    if (-not $PSBoundParameters.ContainsKey('Lines')) {
        $parts = $Version.Split('.')
        $Lines = @(
            '// SPDX-License-Identifier: MIT',
            '#pragma once',
            '',
            "#define HEADTRACKING_VERSION_MAJOR $($parts[0])",
            "#define HEADTRACKING_VERSION_MINOR $($parts[1])",
            "#define HEADTRACKING_VERSION_PATCH $($parts[2])",
            "#define HEADTRACKING_VERSION_STRING `"$Version`"",
            ''
        )
    }
    [System.IO.File]::WriteAllText((Join-Path $root 'src\version.h'), ($Lines -join "`r`n"))
    return $root
}

# --- reading -----------------------------------------------------------------

$root = New-ProjectRoot -Name 'read' -Version '4.5.6'
$readVersion = Get-ModVersion -ProjectRoot $root
Check 'Get-ModVersion returns HEADTRACKING_VERSION_STRING' `
    ($readVersion -eq '4.5.6') "got '$readVersion'"

$readPath = Get-ModVersionPath -ProjectRoot $root
Check 'Get-ModVersionPath points at src\version.h' `
    ($readPath -eq (Join-Path $root 'src\version.h')) "got '$readPath'"

$missing = Join-Path $sandbox 'no-such-root'
Check 'Get-ModVersion throws when src\version.h is absent' `
    ((Get-ThrownMessage { Get-ModVersion -ProjectRoot $missing }) -ne '') `
    'returned instead of throwing'

$noMacro = New-ProjectRoot -Name 'no-macro' -Lines @('#pragma once', '')
Check 'Get-ModVersion throws when the macro is absent' `
    ((Get-ThrownMessage { Get-ModVersion -ProjectRoot $noMacro }) -ne '') `
    'returned instead of throwing'

# The captured value becomes both release ZIP filenames, the
# launcher-manifest.json version field and the git tag CI builds from, and
# version.h is hand-editable. A leading 'v' satisfies the capture regex, so
# without the shape assert the first objection comes from release.yml's
# tag-vs-file check, after the tag is already pushed.
$badVersion = New-ProjectRoot -Name 'bad-version' -Lines @(
    '#pragma once',
    '#define HEADTRACKING_VERSION_STRING "v1.2.3"',
    '')
Check 'Get-ModVersion throws on a version that is not X.Y.Z' `
    ((Get-ThrownMessage { Get-ModVersion -ProjectRoot $badVersion }) -match 'v1\.2\.3') `
    'a malformed version was returned instead of throwing'

$preRead = New-ProjectRoot -Name 'read-prerelease' -Lines @(
    '#pragma once',
    '#define HEADTRACKING_VERSION_STRING "1.2.3-rc.1"',
    '')
Check 'Get-ModVersion accepts a prerelease version' `
    ((Get-ModVersion -ProjectRoot $preRead) -eq '1.2.3-rc.1') `
    'the prerelease form Resolve-ReleaseVersion emits was rejected'

# --- writing -----------------------------------------------------------------

$root = New-ProjectRoot -Name 'write' -Version '0.1.2'
$before = [System.IO.File]::ReadAllText((Get-ModVersionPath -ProjectRoot $root))
Set-ModVersion -ProjectRoot $root -Version '10.20.30'
$after = [System.IO.File]::ReadAllText((Get-ModVersionPath -ProjectRoot $root))

$written = Get-ModVersion -ProjectRoot $root
Check 'Set-ModVersion updates the version string' `
    ($written -eq '10.20.30') "got '$written'"
Check 'Set-ModVersion updates MAJOR, MINOR and PATCH' `
    ($after -match '(?m)^#define HEADTRACKING_VERSION_MAJOR 10\r?$' -and
     $after -match '(?m)^#define HEADTRACKING_VERSION_MINOR 20\r?$' -and
     $after -match '(?m)^#define HEADTRACKING_VERSION_PATCH 30\r?$') `
    'one of the numeric macros was not rewritten'
Check 'Set-ModVersion preserves CRLF line endings' `
    (([regex]::Matches($after, "`r`n").Count -eq [regex]::Matches($before, "`r`n").Count) -and
     ($after -notmatch "(?<!`r)`n")) `
    'line endings changed'
Check 'Set-ModVersion leaves untargeted lines byte-identical' `
    ($after.StartsWith("// SPDX-License-Identifier: MIT`r`n#pragma once`r`n`r`n")) `
    'the header preamble was disturbed'

# Re-running a release with the version already in place is an expected path -
# release.ps1 has a branch for "no version/changelog changes". Rewriting the
# same value must not be treated as a failed match.
$reapplied = Get-ThrownMessage { Set-ModVersion -ProjectRoot $root -Version '10.20.30' }
Check 'Set-ModVersion is idempotent for an unchanged version' `
    ($reapplied -eq '' -and (Get-ModVersion -ProjectRoot $root) -eq '10.20.30') `
    "threw '$reapplied'"

# Resolve-ReleaseVersion accepts X.Y.Z[-prerelease] and hands it straight to
# Set-ModVersion, so the numeric macros have to stay numeric: a naive split on
# '.' wrote '#define HEADTRACKING_VERSION_PATCH 0-rc1'.
$pre = New-ProjectRoot -Name 'prerelease' -Version '0.1.2'
Set-ModVersion -ProjectRoot $pre -Version '1.2.3-rc1'
$preAfter = [System.IO.File]::ReadAllText((Get-ModVersionPath -ProjectRoot $pre))
Check 'Set-ModVersion keeps the numeric macros numeric for a prerelease' `
    ($preAfter -match '(?m)^#define HEADTRACKING_VERSION_PATCH 3\r?$') `
    'PATCH picked up the prerelease suffix'
Check 'Set-ModVersion keeps the prerelease in the version string' `
    ((Get-ModVersion -ProjectRoot $pre) -eq '1.2.3-rc1') `
    "got '$(Get-ModVersion -ProjectRoot $pre)'"

# The write side asserts the same shape as the read side. Without it a caller
# that bypasses Resolve-ReleaseVersion wrote a header the next Get-ModVersion
# could not parse back, so the version file became unreadable rather than
# wrong-but-recoverable.
$rejectRoot = New-ProjectRoot -Name 'reject-write' -Version '1.0.0'
$rejectBefore = [System.IO.File]::ReadAllText((Get-ModVersionPath -ProjectRoot $rejectRoot))
$rejected = Get-ThrownMessage { Set-ModVersion -ProjectRoot $rejectRoot -Version '1.0' }
Check 'Set-ModVersion refuses a version that is not X.Y.Z' `
    ($rejected -ne '') `
    'a two-component version was written'
Check 'Set-ModVersion leaves version.h untouched when it refuses' `
    ([System.IO.File]::ReadAllText((Get-ModVersionPath -ProjectRoot $rejectRoot)) -eq $rejectBefore) `
    'the header was partially rewritten before the refusal'
# --- Update-VersionLiteral ---------------------------------------------------

$installCmd = Join-Path $sandbox 'install.cmd'
[System.IO.File]::WriteAllText($installCmd, (@(
    '@echo off',
    'set "MOD_VERSION=0.0.0"',
    'set "MOD_NAME=BlackMesaHeadTracking"'
) -join "`r`n"))
Update-VersionLiteral -Path $installCmd `
    -Pattern 'set "MOD_VERSION=[^"]+"' -Replacement 'set "MOD_VERSION=9.9.9"'
$installAfter = [System.IO.File]::ReadAllText($installCmd)
Check 'Update-VersionLiteral rewrites install.cmd MOD_VERSION' `
    ($installAfter -match 'set "MOD_VERSION=9\.9\.9"') `
    'MOD_VERSION was not rewritten'
Check 'Update-VersionLiteral keeps install.cmd CRLF' `
    (($installAfter -notmatch "(?<!`r)`n") -and ($installAfter -match "`r`n")) `
    'install.cmd would ship with LF endings, which cmd.exe mis-parses'
Check 'Update-VersionLiteral leaves neighbouring lines alone' `
    ($installAfter -match 'set "MOD_NAME=BlackMesaHeadTracking"') `
    'an untargeted line was rewritten'

$cmake = Join-Path $sandbox 'CMakeLists.txt'
[System.IO.File]::WriteAllText($cmake, (@(
    'cmake_minimum_required(VERSION 3.20)',
    'project(BlackMesaHeadTracking VERSION 0.0.0 LANGUAGES CXX)'
) -join "`r`n"))
Update-VersionLiteral -Path $cmake `
    -Pattern '(?m)^(project\([^)]*VERSION\s+)\d+\.\d+\.\d+' -Replacement '${1}9.9.9'
Check 'Update-VersionLiteral rewrites the CMake project version' `
    ([System.IO.File]::ReadAllText($cmake) -match 'project\(BlackMesaHeadTracking VERSION 9\.9\.9 LANGUAGES CXX\)') `
    'the project() version was not rewritten'

$unmatched = Get-ThrownMessage {
    Update-VersionLiteral -Path $cmake -Pattern 'set "MOD_VERSION=[^"]+"' -Replacement 'x'
}
Check 'Update-VersionLiteral throws when the pattern is absent' `
    ($unmatched -ne '') `
    'silently wrote nothing instead of throwing'

# --- import scope ------------------------------------------------------------

# ModVersion.psm1 imports ReleaseWorkflow.psm1 for Get-ProjectVersion. Doing
# that with -Force removed ReleaseWorkflow from the scope of the script that
# had already imported it, so package-release.ps1 lost Copy-SharedBundle and
# release.ps1 lost Resolve-ReleaseVersion. Both entry points import the two
# modules in that order, so the order is what is checked.
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$scopeProbe = {
    param([string]$RepoRoot)
    Import-Module (Join-Path $RepoRoot 'cameraunlock-core\powershell\ReleaseWorkflow.psm1') -Force
    Import-Module (Join-Path $RepoRoot 'scripts\ModVersion.psm1') -Force
    $needed = @('Copy-SharedBundle', 'Copy-LicenceNotices', 'Resolve-ReleaseVersion',
                'Test-CleanGitStatus', 'Test-GitTagExists', 'New-ChangelogFromCommits',
                'Get-ModVersion', 'Set-ModVersion', 'Update-VersionLiteral')
    return (@($needed | Where-Object { -not (Get-Command $_ -ErrorAction SilentlyContinue) }) -join ',')
}
$lost = & powershell -NoProfile -ExecutionPolicy Bypass -Command $scopeProbe -args $repoRoot
Check 'importing ModVersion keeps ReleaseWorkflow reachable in the caller' `
    ([string]::IsNullOrEmpty("$lost")) `
    "these commands went out of scope: $lost"

Remove-Item -LiteralPath $sandbox -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ''
if ($script:Failures -gt 0) {
    Write-Host "$($script:Failures) failure(s)." -ForegroundColor Red
    exit 1
}
Write-Host 'All ModVersion checks passed.' -ForegroundColor Green
