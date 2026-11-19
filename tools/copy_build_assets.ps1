# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors
#requires -Version 5.1
#
# tools/copy_build_assets.ps1 — post-link asset copy for the `compile` Makefile target.
#
# Pulled out of the Makefile's inline PowerShell one-liner because nested
# quoting inside that string broke whenever `make` was invoked from a non-cmd.exe shell (Git
# Bash / MSYS2 sh) — the shell's own quote-stripping mangled the embedded PowerShell before
# the PowerShell host ever saw it, producing "Missing condition in if statement after 'if ('".
# Invoked via `-File` instead of `-Command` so argument passing is plain argv, not a
# string embedded inside another shell's command line — no nested-quoting surface at all.
#
# Copies SDL3.dll and font/ into the build output directory, checking both the current
# directory and its parent (Makefile is normally invoked from the repo root, but this
# supports being run one level down too, matching the original one-liner's behavior).

param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDir,
    [switch]$ExcludeOptionalFonts,
    [string]$Sdl3DllPath = '',
    # The shared generated project-font directory (the Makefile passes $(BUILD_ROOT)/project-font).
    [string]$ProjectFontDir = ''
)

# A copy that fails (a destination locked by a running player, access denied) must stop the script
# with a non-zero exit, not print a non-terminating error and let make report a build with missing
# or stale assets. Windows PowerShell 5.1's Copy-Item ignores -ErrorAction Stop for some file
# system errors, so the preference is set for the whole script. Native commands whose stderr is
# redirected run under a local 'Continue' (5.1 turns redirected native stderr into a terminating
# error under 'Stop').
$ErrorActionPreference = 'Stop'

New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null

# The toolchain directories: the directory of every gcc on PATH, in PATH order. The first gcc is
# not always the compiler: a compiler cache (ccache's masquerade link or copy, a wrapper script)
# puts a gcc of its own ahead of the toolchain, in a directory that holds nothing else, and runs
# the next gcc on PATH. Each toolchain file below is therefore taken from the first of these
# directories that holds it, which is the first gcc's own directory when that gcc is the compiler.
# Otherwise the same order as tools/nk_cli.py: gcc on PATH wins; MSYS_PATH (or the default MSYS2
# UCRT64 folder) is the fallback when PATH has no gcc.
$toolchainDirs = @()
foreach ($gccCmd in @(Get-Command gcc -All -CommandType Application -ErrorAction SilentlyContinue)) {
    $gccDir = Split-Path -Parent $gccCmd.Source
    if ($toolchainDirs -notcontains $gccDir) {
        $toolchainDirs += $gccDir
    }
}
if (-not $toolchainDirs) {
    $defaultMsysBin = if ($env:MSYS_PATH) { $env:MSYS_PATH } else { "C:\msys64\ucrt64\bin" }
    if (Test-Path -LiteralPath $defaultMsysBin -PathType Container) {
        $toolchainDirs += $defaultMsysBin
    }
}

function Find-ToolchainFile {
    param([string]$Name)
    foreach ($dir in $toolchainDirs) {
        $candidate = Join-Path $dir $Name
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    return ''
}

$copiedSdl = $false
if ($Sdl3DllPath -and (Test-Path $Sdl3DllPath)) {
    Copy-Item $Sdl3DllPath $BuildDir -Force
    $copiedSdl = $true
}

if (-not $copiedSdl) {
    if (Test-Path 'SDL3.dll') {
        Copy-Item 'SDL3.dll' $BuildDir -Force
    } elseif (Test-Path '../SDL3.dll') {
        Copy-Item '../SDL3.dll' $BuildDir -Force
    } else {
        $toolchainSdl = Find-ToolchainFile 'SDL3.dll'
        if ($toolchainSdl) {
            Copy-Item -LiteralPath $toolchainSdl -Destination $BuildDir -Force
        }
    }
}

if (-not $ExcludeOptionalFonts) {
    $fontSrc = if (Test-Path 'font') { 'font' } elseif (Test-Path '../font') { '../font' } else { '' }
    if ($fontSrc) {
        $fontDst = Join-Path $BuildDir 'font'
        if (Test-Path $fontDst) {
            Remove-Item $fontDst -Recurse -Force
        }
        Copy-Item $fontSrc $fontDst -Recurse -Force
    }
}

# The project PGFs (Japanese, Korean and the sixteen Latin faces) are generated from their pinned
# sources before compile.
# Stage them after the optional source-font copy so that replacing the destination
# directory cannot remove a generated default. They are part of every runtime
# build, including builds that omit optional source fonts.
$generatedFontDir = if ($ProjectFontDir) { $ProjectFontDir } else { Join-Path $BuildDir 'project-font' }
if (-not (Test-Path -LiteralPath $generatedFontDir -PathType Container)) {
    throw "PROJECT_FONT_OUTPUT_DIR_MISSING: $generatedFontDir was not generated; the compile target builds it (project-fonts)"
}
if (Test-Path -LiteralPath $generatedFontDir -PathType Container) {
    $fontDst = Join-Path $BuildDir 'font'
    New-Item -ItemType Directory -Path $fontDst -Force | Out-Null
    foreach ($name in @(
        'nkjpn.pgf', 'nkjpn.json', 'nkjpn-OFL.txt',
        'nkltn.pgf', 'nkltn.json', 'nkltn-OFL.txt',
        'nkltns.pgf', 'nkltns.json', 'nkltns-OFL.txt',
        'nkltn1.pgf', 'nkltn1.json', 'nkltn1-OFL.txt',
        'nkltn2.pgf', 'nkltn2.json', 'nkltn2-OFL.txt',
        'nkltn3.pgf', 'nkltn3.json', 'nkltn3-OFL.txt',
        'nkltn4.pgf', 'nkltn4.json', 'nkltn4-OFL.txt',
        'nkltn5.pgf', 'nkltn5.json', 'nkltn5-OFL.txt',
        'nkltn6.pgf', 'nkltn6.json', 'nkltn6-OFL.txt',
        'nkltn7.pgf', 'nkltn7.json', 'nkltn7-OFL.txt',
        'nkltn9.pgf', 'nkltn9.json', 'nkltn9-OFL.txt',
        'nkltn10.pgf', 'nkltn10.json', 'nkltn10-OFL.txt',
        'nkltn11.pgf', 'nkltn11.json', 'nkltn11-OFL.txt',
        'nkltn12.pgf', 'nkltn12.json', 'nkltn12-OFL.txt',
        'nkltn13.pgf', 'nkltn13.json', 'nkltn13-OFL.txt',
        'nkltn14.pgf', 'nkltn14.json', 'nkltn14-OFL.txt',
        'nkltn15.pgf', 'nkltn15.json', 'nkltn15-OFL.txt',
        'nkkr.pgf', 'nkkr.json', 'nkkr-OFL.txt'
    )) {
        $source = Join-Path $generatedFontDir $name
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "PROJECT_FONT_OUTPUT_INCOMPLETE: expected $name under $generatedFontDir; run the project-fonts target"
        }
        Copy-Item -LiteralPath $source -Destination $fontDst -Force
    }
}

# Stage the toolchain runtime DLLs the build directory's binaries import, transitively: SDL3.dll
# imports libiconv-2.dll, and libstdc++-6.dll imports libgcc_s_seh-1.dll and libwinpthread-1.dll.
# Each pass re-reads the imports of every .exe and .dll in the build directory, so a DLL staged in
# one pass has its own imports staged in the next; the passes end when one copies nothing, which
# happens after at most one copy per listed DLL. A staged copy whose bytes differ from the
# toolchain's is replaced, so a toolchain update cannot leave a stale runtime DLL beside a relinked
# executable (the loader searches the executable's own directory first). An imported DLL that is
# in no toolchain directory and not already staged is a named stop. This list is the
# compile-asset-copy route's licence obligation in assets/third_party_components.json.
if ($toolchainDirs) {
    $runtimeDlls = @('libgcc_s_seh-1.dll', 'libwinpthread-1.dll', 'libstdc++-6.dll', 'libiconv-2.dll')
    $objdumpCmd = Get-Command objdump -ErrorAction SilentlyContinue
    if ($objdumpCmd) {
        $copiedInPass = $true
        while ($copiedInPass) {
            $copiedInPass = $false
            $importer = @{}
            $binaries = Get-ChildItem -LiteralPath $BuildDir -File | Where-Object { $_.Extension -in '.exe', '.dll' }
            foreach ($bin in $binaries) {
                $dump = & { $ErrorActionPreference = 'Continue'; & $objdumpCmd.Source -p $bin.FullName 2>$null }
                foreach ($match in ($dump | Select-String -Pattern 'DLL Name:\s*(\S+)')) {
                    $importer[$match.Matches[0].Groups[1].Value.ToLowerInvariant()] = $bin.Name
                }
            }
            foreach ($dll in $runtimeDlls) {
                $key = $dll.ToLowerInvariant()
                if (-not $importer.ContainsKey($key)) {
                    continue
                }
                $srcDll = Find-ToolchainFile $dll
                $dstDll = Join-Path $BuildDir $dll
                $staged = Test-Path -LiteralPath $dstDll -PathType Leaf
                if (-not $srcDll) {
                    if ($staged) {
                        continue
                    }
                    throw "RUNTIME_DLL_UNRESOLVED: $($importer[$key]) imports $dll, which is in none of the toolchain directories ($($toolchainDirs -join '; ')) and is not staged in $BuildDir"
                }
                if ($staged -and ((Get-FileHash -LiteralPath $srcDll).Hash -eq (Get-FileHash -LiteralPath $dstDll).Hash)) {
                    continue
                }
                Copy-Item -LiteralPath $srcDll -Destination $dstDll -Force
                $copiedInPass = $true
            }
        }
    }
}

# Generate third-party notices bundle and relink materials
$pyCmd = Get-Command python -ErrorAction SilentlyContinue
$noticesScript = if ($PSScriptRoot -and (Test-Path (Join-Path $PSScriptRoot 'package_notices.py'))) {
    Join-Path $PSScriptRoot 'package_notices.py'
} elseif (Test-Path 'tools/package_notices.py') {
    'tools/package_notices.py'
} elseif (Test-Path '../tools/package_notices.py') {
    '../tools/package_notices.py'
} else {
    ''
}

$hasBinaries = Get-ChildItem -Path $BuildDir -File | Where-Object { $_.Extension -in '.exe', '.dll' }
if ($hasBinaries) {
    # Fail closed: a run directory that ships binaries must also ship their license notices.
    if (-not $pyCmd -or -not $noticesScript) {
        throw "PACKAGE_NOTICES_UNAVAILABLE: python or tools/package_notices.py not found; cannot generate third-party notices for $BuildDir"
    }
    & $pyCmd.Source $noticesScript $BuildDir
    if ($LASTEXITCODE -ne 0) {
        throw "PACKAGE_NOTICES_FAILED: tools/package_notices.py exited with status $LASTEXITCODE"
    }
}
