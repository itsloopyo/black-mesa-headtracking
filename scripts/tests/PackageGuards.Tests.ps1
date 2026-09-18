#!/usr/bin/env pwsh
#Requires -Version 5.1
# ============================================================================
# Tests for scripts/PackageGuards.psm1
# ============================================================================
# Run: pixi run test
#
# Assert-I386Image is the last thing standing between an x64 build and a
# release ZIP, and a wrong-architecture binary produces no error anywhere -
# the loader simply never loads it. So the guard is exercised against real PE
# images from the running system rather than against a hand-rolled header,
# which could agree with a wrong implementation.
#
# No Pester dependency, matching cameraunlock-core/powershell/tests.
# ============================================================================

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path (Split-Path -Parent $PSScriptRoot) 'PackageGuards.psm1') -Force

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

$sandbox = Join-Path ([System.IO.Path]::GetTempPath()) "bmht-guards-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $sandbox -Force | Out-Null

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

# --- accepts a real x86 image ------------------------------------------------

# SysWOW64 holds the 32-bit system DLLs on every x64 Windows, so this is a
# genuine i386 PE rather than a fixture written to agree with our own parser.
$x86 = Join-Path $env:WINDIR 'SysWOW64\kernel32.dll'
if (Test-Path -LiteralPath $x86) {
    Check 'Assert-I386Image accepts a 32-bit PE' `
        ((Get-ThrownMessage { Assert-I386Image -Path $x86 }) -eq '') `
        'rejected a known-good i386 image'
} else {
    Write-Host "SKIP  Assert-I386Image accepts a 32-bit PE - $x86 not present" -ForegroundColor Yellow
}

# The binary this repo actually ships. install.cmd renames it to winmm.dll and
# deploys it beside bms.exe at the game root - ASI_SUBDIR is empty, because a
# proxy in <game>\bin never loads under Steam. That exe is 32-bit, so a vendor
# bump that landed the x64 asset has to fail here rather than in a player's game.
$vendored = Join-Path $repoRoot 'vendor\ultimate-asi-loader\dinput8.dll'
if (Test-Path -LiteralPath $vendored) {
    Check 'the vendored Ultimate ASI Loader is 32-bit' `
        ((Get-ThrownMessage { Assert-I386Image -Path $vendored }) -eq '') `
        'vendor/ultimate-asi-loader/dinput8.dll is not an i386 image'
} else {
    Write-Host 'SKIP  the vendored Ultimate ASI Loader is 32-bit - not present' -ForegroundColor Yellow
}

# --- rejects everything else -------------------------------------------------

$x64 = Join-Path $env:WINDIR 'System32\kernel32.dll'
$x64Message = Get-ThrownMessage { Assert-I386Image -Path $x64 }
Check 'Assert-I386Image rejects a 64-bit PE' `
    ($x64Message -match '0x8664') `
    "expected the machine type in the message, got '$x64Message'"

$text = Join-Path $sandbox 'not-a-binary.txt'
[System.IO.File]::WriteAllText($text, 'this is not a PE image')
Check 'Assert-I386Image rejects a file with no MZ signature' `
    ((Get-ThrownMessage { Assert-I386Image -Path $text }) -match 'not a PE image') `
    'a text file was accepted as a binary'

# A truncated copy keeps the MZ header and loses the PE header, which is the
# shape a half-written vendor file has on disk.
$truncated = Join-Path $sandbox 'truncated.dll'
$full = [System.IO.File]::ReadAllBytes($x64)
[System.IO.File]::WriteAllBytes($truncated, $full[0..0x3F])
Check 'Assert-I386Image rejects a truncated PE' `
    ((Get-ThrownMessage { Assert-I386Image -Path $truncated }) -match 'no PE signature') `
    'a file truncated before its PE header was accepted'

# Zero-length is what an interrupted copy leaves behind.
$empty = Join-Path $sandbox 'empty.dll'
[System.IO.File]::WriteAllBytes($empty, [byte[]]@())
Check 'Assert-I386Image rejects an empty file' `
    ((Get-ThrownMessage { Assert-I386Image -Path $empty }) -ne '') `
    'a zero-length file was accepted'

Check 'Assert-I386Image reports a missing file as missing' `
    ((Get-ThrownMessage { Assert-I386Image -Path (Join-Path $sandbox 'no-such.dll') }) -match 'does not exist') `
    'a missing path did not name itself in the error'

# --- Assert-NotVendoredLoader -----------------------------------------------

# A stand-in copied into bin\Release to exercise the packager is an i386 PE, so
# every other gate passes it and two ZIPs shipped ThirteenAG's binary under our
# name. The fixtures are a real copy of the vendored loader and a real distinct
# PE, not synthetic bytes, because content hashing is the whole mechanism.
$fakeLoader = Join-Path $sandbox 'vendored-loader.dll'
$renamedAsi = Join-Path $sandbox 'BlackMesaHeadTracking.asi'
[System.IO.File]::WriteAllBytes($fakeLoader, $full)
[System.IO.File]::Copy($fakeLoader, $renamedAsi, $true)

$renameMessage = Get-ThrownMessage { Assert-NotVendoredLoader -Path $renamedAsi -LoaderPath $fakeLoader }
Check 'Assert-NotVendoredLoader rejects the loader renamed as the mod' `
    ($renameMessage -match 'byte-identical') `
    "expected a byte-identical rejection, got '$renameMessage'"

Check 'Assert-NotVendoredLoader names the offending hash' `
    ($renameMessage -match '[0-9a-f]{64}') `
    "expected the sha256 in the message, got '$renameMessage'"

# One flipped byte in a same-sized image: the guard has to discriminate on
# content, since a real build and the loader can agree on name, size and
# machine type and still be different binaries.
$realBuild = Join-Path $sandbox 'real-build.asi'
$buildBytes = [byte[]]::new($full.Length)
[System.Array]::Copy($full, $buildBytes, $full.Length)
$buildBytes[$buildBytes.Length - 1] = $buildBytes[$buildBytes.Length - 1] -bxor 0xFF
[System.IO.File]::WriteAllBytes($realBuild, $buildBytes)
Check 'Assert-NotVendoredLoader accepts a payload that differs from the loader' `
    ((Get-ThrownMessage { Assert-NotVendoredLoader -Path $realBuild -LoaderPath $fakeLoader }) -eq '') `
    'a genuine, distinct binary was rejected'

Check 'Assert-NotVendoredLoader reports a missing loader as missing' `
    ((Get-ThrownMessage {
        Assert-NotVendoredLoader -Path $realBuild -LoaderPath (Join-Path $sandbox 'no-such.dll')
    }) -match 'does not exist') `
    'a missing loader path did not name itself in the error'

Remove-Item -LiteralPath $sandbox -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ''
if ($script:Failures -gt 0) {
    Write-Host "$($script:Failures) failure(s)." -ForegroundColor Red
    exit 1
}
Write-Host 'All PackageGuards checks passed.' -ForegroundColor Green
