#!/usr/bin/env pwsh
#Requires -Version 5.1
# ============================================================================
# Release-time asserts on the binaries the ZIPs are about to carry.
# ============================================================================
# Kept in a module rather than inline in package-release.ps1 so the rules are
# covered by scripts/tests/PackageGuards.Tests.ps1 - a packaging gate that has
# never been exercised is a gate nobody knows the shape of.
# ============================================================================

Set-StrictMode -Version Latest

# Black Mesa ships a 32-bit bms.exe, so every binary in a release ZIP has to be
# an i386 image: the vendored Ultimate ASI Loader (the x86 asset, not the
# _x64 one) and the .asi CMake builds with -A Win32.
#
# Both sides can go wrong without anyone editing a file. Upstream decides what
# lives inside Ultimate-ASI-Loader.zip, and the `setup` task reconfigures only
# when build\CMakeCache.txt is absent, so a directory left behind by an x64
# configure keeps producing x64 output. Neither shows up as a build failure:
# the loader simply never loads the plugin, the player sees no head tracking,
# and the log the README points them at was never written.
function Assert-I386Image {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Cannot check the image architecture of $Path - the file does not exist."
    }

    # The headers, not the file. Every field read below sits in the first few
    # hundred bytes, and the vendored loader is 5 MB: ReadAllBytes pulled the
    # whole image through memory to look at six bytes of it, on both binaries,
    # every time a release was packaged.
    $stream = [System.IO.File]::Open(
        $Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
    try {
        $reader = New-Object System.IO.BinaryReader($stream)

        # ReadBytes stops at end of stream and returns a short array, which is
        # how an empty or truncated file is caught rather than indexed past.
        $dosHeader = $reader.ReadBytes(0x40)
        if ($dosHeader.Length -lt 0x40 -or $dosHeader[0] -ne 0x4D -or $dosHeader[1] -ne 0x5A) {
            throw "$Path is not a PE image (no MZ signature)."
        }

        $peOffset = [BitConverter]::ToInt32($dosHeader, 0x3C)
        $peHeader = [byte[]]@()
        if ($peOffset -ge 0x40 -and $peOffset -le ($stream.Length - 6)) {
            $stream.Position = $peOffset
            $peHeader = $reader.ReadBytes(6)
        }
        if ($peHeader.Length -lt 6 -or
            $peHeader[0] -ne 0x50 -or $peHeader[1] -ne 0x45 -or
            $peHeader[2] -ne 0x00 -or $peHeader[3] -ne 0x00) {
            throw "$Path has no PE signature at e_lfanew (0x$($peOffset.ToString('X'))) - it is truncated or not a Windows binary."
        }

        $machine = [BitConverter]::ToUInt16($peHeader, 4)
    } finally {
        $stream.Dispose()
    }
    if ($machine -ne 0x014C) {
        throw ("{0} is PE machine 0x{1:X4}, not x86 (0x014C). Black Mesa's bms.exe is 32-bit and will not load it." -f $Path, $machine)
    }
}

# The mod payload must not BE the loader. Both are i386 PEs of the same shape,
# so Assert-I386Image passes on either, and every downstream step - the ZIPs,
# the manifest, the installer - treats whatever sits at bin\<config>\<mod>.asi
# as the mod. A stand-in copied there to exercise the packager therefore ships
# as a release, and what ships is ThirteenAG's MIT-licensed binary presented
# under our name and our LICENSE. That happened once in this repo and produced
# two ZIPs before anyone noticed, because nothing downstream can tell the two
# files apart. Content hash, not size or name: a rename is exactly the case
# that got through.
function Assert-NotVendoredLoader {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$LoaderPath
    )

    foreach ($p in @($Path, $LoaderPath)) {
        if (-not (Test-Path -LiteralPath $p)) {
            throw "Cannot compare $Path against the vendored loader - $p does not exist."
        }
    }

    # Not Get-FileHash: Windows PowerShell autoloads it from a script module,
    # and when launched from pwsh (CI's shell: pwsh) it inherits pwsh's
    # PSModulePath, finds the Core-only Microsoft.PowerShell.Utility first and
    # reports the cmdlet as not recognized.
    $hashes = foreach ($p in @($Path, $LoaderPath)) {
        $sha    = [System.Security.Cryptography.SHA256]::Create()
        $stream = [System.IO.File]::OpenRead($p)
        try {
            [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '')
        } finally {
            $stream.Dispose()
            $sha.Dispose()
        }
    }
    $modHash, $loaderHash = $hashes
    if ($modHash -eq $loaderHash) {
        throw ("{0} is byte-identical to the vendored Ultimate ASI Loader ({1}, sha256 {2}). " -f $Path, $LoaderPath, $modHash.ToLower()) +
              'That is a third-party MIT binary renamed as this mod, not a build of this mod. ' +
              'Delete bin\ and run: pixi run build-release'
    }
}

Export-ModuleMember -Function Assert-I386Image, Assert-NotVendoredLoader
