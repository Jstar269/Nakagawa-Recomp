#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Build source-owned PSP showcase images and validated runtime packages.

The same route runs on every host. The PSPDEV toolchain is a POSIX toolchain, so
a Windows host reaches it through WSL (`wsl -e bash -lc ...`) and a Linux host
runs it directly; nothing else about the route changes. The package build and
the headless smoke therefore exercise the same code on both, which is what makes
the Linux result comparable with the Windows one.

A host without PSPDEV can still build the packages: `build-prx --out-dir D` on a
PSPDEV host writes each demo's PRX and the PSPSDK license notice to D, and
`build --prebuilt-prx-dir D` uses those instead of running PSPDEV. The release
job builds the PRXs on Linux this way and the packages on Windows.
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
from nk_core import package_cache
from ge_stat_windows import (WINDOW_VBLANKS, GeStatWindow, GeStatWindowError, first_window,
                             parse_windows)
from prxload import Prx

# Where the staged demos live. The defaults are inside the repository's ignored
# build tree, which is what a developer or a CI checkout wants. The two
# overrides exist for a source tree that is not a Git checkout (a `git archive`
# export, for instance), where the package route cannot prove its output
# directory is ignored and an out-of-tree output is the only honest option.
BUILD_ROOT = Path(os.environ.get("NK_SHOWCASE_BUILD_ROOT") or ROOT / "build" / "showcase")
DEMO_ROOT = Path(os.environ.get("NK_SHOWCASE_DEMO_ROOT") or ROOT / "build" / "demos")
SCREENSHOT_ROOT = BUILD_ROOT / "screenshots"
# Vblank window (inclusive) whose presented frames the headless capture smoke publishes
# and checks. A window, not a single vblank: a demo need not flip on every vblank (the 3D
# scene flips about every fourth one in the capture run), and every frame it does present
# inside the window must be captured.
HEADLESS_CAPTURE_WINDOW = (60, 71)
SECTOR = 2048
PSP_PRX_ELF_TYPE = 0xFFA0
# Where a demo's executable looks for the SDL3 libraries on Linux, relative to its own
# folder. The install holds `<player folder>/demos/packages/<disc id>/<executable>` (the
# layout player_app_discover_showcase reads), and the player folder holds the libraries
# beside the player, so three folders up is that folder. Read by the Makefile as
# RUNTIME_RUNPATH_ORIGIN; nothing else about a build changes with it.
RUNTIME_RUNPATH_ORIGIN = "../../.."

# The first-frame checkpoint is guest progress: the first SR_GESTAT window whose
# counters show the demo's own drawing ("frame_counters" below), at whatever vblank
# that window closed. VCOUNT is host time in the default paced mode, so a slow host
# boots the guest later and its first drawn frame lands in a later window; that is
# still a reached checkpoint. The budget bounds the wait: the runtime exits cleanly
# once VCOUNT reaches SMOKE_EXIT_VBLANK (SR_EXIT_AT_VBLANK, three windows), and
# SMOKE_TIMEOUT_SECONDS bounds the whole host process, so a guest that stops making
# progress fails instead of hanging CI.
SMOKE_EXIT_VBLANK = 3 * WINDOW_VBLANKS
SMOKE_TIMEOUT_SECONDS = 15
# Scripted presses as SR_PADSCRIPT rows: (vblank, button mask, width in vblanks).
# Cross plays each demo's tone; Start saves and exits Breakout's savedata run.
CROSS_PRESS = (12, 0x4000, 4)
START_PRESS = (240, 0x0008, 4)
SAVE_START_PRESS = (120, 0x0008, 4)
PAUSE_CROSS_PRESS = (121, 0x4000, 4)
# Breakout's scheduler pause: requested at VCOUNT 120, held for 1500 ms of host time.
PAUSE_TRIGGER_VCOUNT = 120
PAUSE_HOLD_MS = 1500
AUDIO_PAUSE_CROSS_PRESS = (12, 0x4000, 4)
AUDIO_PAUSE_TRIGGER_VCOUNT = 12
HEADLESS_PAUSE_SCRIPT = "30:500"


def _pause_timeout_seconds(host_ms: int) -> int:
    """Leave the ordinary smoke cap intact and budget only the requested pause."""
    return SMOKE_TIMEOUT_SECONDS + (host_ms + 999) // 1000 + 2

DEMOS = (
    {
        "id": "showcase-scene-v1",
        "disc_id": "TEST00007",
        "display_name": "Nakagawa 3D Showcase",
        "folder": "ge_scene",
        "target": "showcase_scene_app",
        "manifest": "assets/titles/showcase-scene.json",
        "palette": ((23, 54, 81), (78, 204, 190)),
        # The lit cube: transform-mode triangles with visible pixels.
        "frame_counters": ("tri3d", "px3d"),
    },
    {
        "id": "showcase-breakout-v1",
        "disc_id": "TEST00008",
        "display_name": "Nakagawa Breakout Showcase",
        "folder": "breakout",
        "target": "showcase_breakout_app",
        "manifest": "assets/titles/showcase-breakout.json",
        "palette": ((49, 35, 78), (252, 200, 87)),
        # Paddle, ball, bricks and score: through-mode sprites with visible pixels.
        "frame_counters": ("spr2d", "px2d"),
    },
)


class ShowcaseError(RuntimeError):
    """An explicit failure in the optional showcase build route."""


# ELF loader records, read without a toolchain so the audit runs wherever the build did.
_PT_LOAD, _PT_DYNAMIC = 1, 2
_DT_NEEDED, _DT_STRTAB, _DT_RPATH, _DT_RUNPATH = 1, 5, 15, 29


def elf_run_paths(path: Path) -> tuple[list[str], list[str]] | None:
    """The libraries an ELF object needs and the folders it searches for them.

    Returns (DT_NEEDED names, entries of DT_RUNPATH and DT_RPATH), or None for a file that
    is not a little-endian ELF object. A static executable or relocatable object has no
    dynamic section and returns two empty lists.
    """
    with path.open("rb") as stream:
        head = stream.read(20)
        if len(head) < 20 or head[:4] != b"\x7fELF" or head[5] != 1 or head[4] not in (1, 2):
            return None
        stream.seek(0)
        data = stream.read()
    wide = data[4] == 2
    if wide:
        phoff, = struct.unpack_from("<Q", data, 0x20)
        phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    else:
        phoff, = struct.unpack_from("<I", data, 0x1C)
        phentsize, phnum = struct.unpack_from("<HH", data, 0x2A)
    loads: list[tuple[int, int, int]] = []
    dynamic: tuple[int, int] | None = None
    for index in range(phnum):
        base = phoff + index * phentsize
        if wide:
            kind, _, offset, vaddr, _, filesz, _, _ = struct.unpack_from("<IIQQQQQQ", data, base)
        else:
            kind, offset, vaddr, _, filesz, _, _, _ = struct.unpack_from("<IIIIIIII", data, base)
        if kind == _PT_LOAD:
            loads.append((vaddr, offset, filesz))
        elif kind == _PT_DYNAMIC:
            dynamic = (offset, filesz)
    if dynamic is None:
        return [], []
    entry_size = 16 if wide else 8
    entries: list[tuple[int, int]] = []
    for position in range(dynamic[0], dynamic[0] + dynamic[1], entry_size):
        tag, value = struct.unpack_from("<qQ" if wide else "<iI", data, position)
        if tag == 0:
            break
        entries.append((tag, value))
    strtab = next((value for tag, value in entries if tag == _DT_STRTAB), None)
    if strtab is None:
        return [], []
    table = next((offset + (strtab - vaddr) for vaddr, offset, filesz in loads
                  if vaddr <= strtab < vaddr + filesz), None)
    if table is None:
        raise ShowcaseError(f"{path}: the dynamic string table lies outside every loaded segment")

    def text(offset: int) -> str:
        end = data.index(b"\0", table + offset)
        return data[table + offset:end].decode("utf-8", errors="replace")

    needed = [text(value) for tag, value in entries if tag == _DT_NEEDED]
    folders = [folder for tag, value in entries if tag in (_DT_RUNPATH, _DT_RPATH)
               for folder in text(value).split(":")]
    return needed, folders


_EM_AARCH64 = 183  # the ELF machine of an arm64 library (Gradle's nk.demos check uses the same)


def is_android_library(path: Path) -> bool:
    """True for `lib<game>.so` built for arm64: the library an Android package ships (#360).

    `showcase.py build --target android` links each demo as the shared library TitleActivity
    loads. It runs inside the APK, where the app's own library folder holds SDL3, so it has
    no run path and none is owed. A desktop demo is never both named `lib*.so` and an arm64
    object, so this cannot excuse a Linux package that lost its run path.
    """
    if not (path.name.startswith("lib") and path.name.endswith(".so")):
        return False
    with path.open("rb") as stream:
        head = stream.read(20)
    if len(head) < 20 or head[:4] != b"ELF" or head[5] != 1:
        return False
    return struct.unpack_from("<H", head, 18)[0] == _EM_AARCH64


def audit_run_paths(paths: list[Path]) -> list[str]:
    """Name every run path entry of the given ELF files that is not relative to the file.

    A shipped executable finds the libraries beside it through `$ORIGIN`; an entry that is
    an absolute folder names the machine that linked it (a developer's `/usr/local/lib`),
    and a bare relative entry is resolved against the caller's working directory. Files
    that are not ELF objects are skipped.
    """
    problems = []
    for path in paths:
        records = elf_run_paths(path)
        if records is None:
            continue
        for folder in records[1]:
            if not (folder in ("$ORIGIN", "${ORIGIN}") or folder.startswith(("$ORIGIN/", "${ORIGIN}/"))):
                problems.append(f"{path}: run path entry '{folder}' is not relative to $ORIGIN")
    return problems


def audit_required_run_path(paths: list[Path], required: str,
                            library_prefix: str = "libSDL3.so") -> list[str]:
    """Name every ELF executable that needs SDL3 but does not search `required` for it.

    A run path that is only well formed is not enough: a demo linked with none still starts
    on a machine whose loader cache holds an SDL3 and fails on one that does not. Each ELF
    file that lists a library starting with `library_prefix` must carry `required` (for the
    demos `$ORIGIN/../../..`, the player's folder) among its run path entries.
    """
    problems = []
    for path in paths:
        records = elf_run_paths(path)
        if records is None or not any(name.startswith(library_prefix) for name in records[0]):
            continue
        if required not in records[1]:
            found = ", ".join(records[1]) or "none"
            problems.append(f"{path}: needs {library_prefix} but its run path ({found}) lacks {required}")
    return problems


def system_library_files(prefix: str) -> list[Path]:
    """The real files of the system's copies of a library, from the loader's cache (Linux)."""
    listing = subprocess.run(["/sbin/ldconfig", "-p"], check=False, capture_output=True, text=True)
    found = {Path(line.rsplit("=>", 1)[1].strip()).resolve()
             for line in listing.stdout.splitlines()
             if line.strip().startswith(prefix) and "=>" in line}
    return sorted(found)


def unresolved_libraries(executable: Path, prefix: str = "libSDL3.so") -> list[str]:
    """What `ldd` cannot resolve for `executable` with every system copy of a library hidden.

    The system copies are hidden by binding an empty file over each of them inside a private
    mount namespace (no privileges needed, nothing outside the namespace changes), and the
    run starts with no LD_LIBRARY_PATH, so what resolves is what the executable's own run
    path reaches: the library installed beside it, or nothing. Returns the lines `ldd` could
    not resolve; raises ShowcaseError where the namespace cannot be made.
    """
    hidden = [str(path) for path in system_library_files(prefix)]
    script = ('for f in "$@"; do mount --bind /dev/null "$f" || exit 97; done; '
              'ldd "$NK_AUDITED" 2>&1')
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "NK_AUDITED": str(executable)}
    run = subprocess.run(["unshare", "-rm", "sh", "-c", script, "sh", *hidden], check=False,
                         capture_output=True, text=True, env=env)
    if run.returncode == 97 or "unshare:" in run.stderr:
        raise ShowcaseError(f"a private mount namespace is not available: {run.stderr.strip()}")
    return [line.strip() for line in run.stdout.splitlines()
            if "not found" in line or "file too short" in line or "cannot open" in line
            or "invalid ELF" in line]


def files_naming(root: Path, needles: list[str]) -> list[str]:
    """The files under `root`, as relative paths, whose bytes spell any of the given folders.

    Each folder is also tried with the other slash and with JSON escaping, so a Windows
    path matches however a tool wrote it.
    """
    spellings: set[bytes] = set()
    for needle in needles:
        if not needle:
            continue
        forward = needle.replace("\\", "/")
        backward = forward.replace("/", "\\")
        for form in (needle, forward, backward, backward.replace("\\", "\\\\")):
            spellings.add(form.encode("utf-8"))
    hits = []
    for path in sorted(root.rglob("*")):
        if path.is_symlink() or not path.is_file():
            continue
        data = path.read_bytes()
        if any(spelling in data for spelling in spellings):
            hits.append(path.relative_to(root).as_posix())
    return hits


def audit_demo_root(demo_root: Path) -> list[str]:
    """What stops the staged demos from being moved to another machine, one line each.

    Every ELF file in a package folder must have only `$ORIGIN`-relative run paths, an
    executable that needs SDL3 must search the player's folder for it (the arm64 `lib<game>.so`
    of an Android demos folder is exempt: it loads SDL3 from the APK), and no file in the demo
    folder may name this checkout, the showcase build folder or the user's home folder.
    """
    executables = [path for path in sorted((demo_root / "packages").glob("*/*")) if path.is_file()]
    problems = audit_run_paths(executables)
    desktop = [path for path in executables if not is_android_library(path)]
    problems.extend(audit_required_run_path(desktop, f"$ORIGIN/{RUNTIME_RUNPATH_ORIGIN}"))
    needles = [str(ROOT), ROOT.as_posix(), str(BUILD_ROOT), BUILD_ROOT.as_posix()]
    home = Path.home()
    if home.parent != home:  # not the file system root
        needles.extend([str(home), home.as_posix()])
    problems.extend(f"{demo_root / name}: names the build folder"
                    for name in files_naming(demo_root, needles))
    return problems


def _png_chunk(kind: bytes, payload: bytes) -> bytes:
    body = kind + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def make_icon_png(palette: tuple[tuple[int, int, int], tuple[int, int, int]]) -> bytes:
    """Generate fixed-size source-owned ICON0 art using only the standard library."""
    width, height = 144, 80
    first, second = palette
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            stripe = ((x // 12) + (y // 10)) % 2
            color = second if stripe else first
            cx, cy = x - width // 2, y - height // 2
            if -22 <= cx <= 22 and -22 <= cy <= 22 and (abs(cx) < 5 or abs(cy) < 5):
                color = (245, 248, 250)
            rows.extend((*color, 255))
    ihdr = struct.pack(">2I5B", width, height, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + _png_chunk(b"IHDR", ihdr) + \
        _png_chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + _png_chunk(b"IEND", b"")


def _both_endian32(value: int) -> bytes:
    return value.to_bytes(4, "little") + value.to_bytes(4, "big")


def _both_endian16(value: int) -> bytes:
    return value.to_bytes(2, "little") + value.to_bytes(2, "big")


def _dir_record(name: bytes, lba: int, length: int, directory: bool) -> bytes:
    record = bytearray(33 + len(name))
    record[2:10] = _both_endian32(lba)
    record[10:18] = _both_endian32(length)
    record[25] = 0x02 if directory else 0
    record[28:32] = _both_endian16(1)
    record[32] = len(name)
    record[33:] = name
    if len(record) & 1:
        record.append(0)
    record[0] = len(record)
    return bytes(record)


def make_param_sfo(disc_id: str, title: str) -> bytes:
    entries = [
        ("DISC_ID", disc_id.encode("ascii") + b"\0"),
        ("DISC_VERSION", b"1.00\0"),
        ("TITLE", title.encode("utf-8") + b"\0"),
    ]
    entries.sort(key=lambda item: item[0])
    keys = bytearray()
    values = bytearray()
    table = bytearray()
    for key, value in entries:
        key_offset = len(keys)
        keys.extend(key.encode("ascii") + b"\0")
        data_offset = len(values)
        values.extend(value)
        while len(values) & 3:
            values.append(0)
        table.extend(struct.pack("<HHIII", key_offset, 0x0204, len(value), len(value), data_offset))
    key_start = 20 + len(table)
    data_start = key_start + len(keys)
    while data_start & 3:
        keys.append(0)
        data_start += 1
    header = struct.pack("<4s4sIII", b"\0PSF", b"\1\1\0\0", key_start, data_start, len(entries))
    return header + table + keys + values


def write_iso(path: Path, disc_id: str, title: str, icon: bytes,
              eboot: bytes, license_notice: bytes) -> None:
    """Write a minimal deterministic ISO9660 tree for one source-owned PSP app."""
    if len(disc_id) != 9 or not eboot.startswith(b"\x7fELF"):
        raise ShowcaseError("showcase disc ID or plaintext EBOOT.BIN is invalid")
    blobs = {
        "PARAM.SFO;1": make_param_sfo(disc_id, title),
        "ICON0.PNG;1": icon,
        "EBOOT.BIN;1": eboot,
        "PSPSDK.LICENSE;1": license_notice,
    }
    lbas: dict[str, int] = {}
    lba = 21
    for name, content in blobs.items():
        lbas[name] = lba
        lba += (len(content) + SECTOR - 1) // SECTOR
    total_sectors = max(lba, 1024)
    image = bytearray(total_sectors * SECTOR)

    pvd = memoryview(image)[16 * SECTOR:17 * SECTOR]
    pvd[0:7] = b"\x01CD001\x01"
    pvd[8:40] = b"NAKAGAWA SHOWCASE".ljust(32, b" ")
    pvd[40:72] = b"NKSHOWCASE".ljust(32, b" ")
    pvd[80:88] = _both_endian32(total_sectors)
    pvd[120:124] = _both_endian16(1)
    pvd[124:128] = _both_endian16(1)
    pvd[128:132] = _both_endian16(SECTOR)
    pvd[813:830] = b"2026092400000000\0"
    pvd[881] = 1
    pvd[156:190] = _dir_record(b"\0", 18, SECTOR, True)
    image[17 * SECTOR] = 0xFF
    image[17 * SECTOR + 1:17 * SECTOR + 7] = b"CD001\x01"

    root_entries = (
        _dir_record(b"\0", 18, SECTOR, True)
        + _dir_record(b"\1", 18, SECTOR, True)
        + _dir_record(b"PSP_GAME", 19, SECTOR, True)
    )
    game_entries = (
        _dir_record(b"\0", 19, SECTOR, True)
        + _dir_record(b"\1", 18, SECTOR, True)
        + _dir_record(b"PARAM.SFO;1", lbas["PARAM.SFO;1"], len(blobs["PARAM.SFO;1"]), False)
        + _dir_record(b"ICON0.PNG;1", lbas["ICON0.PNG;1"], len(icon), False)
        + _dir_record(b"SYSDIR", 20, SECTOR, True)
        + _dir_record(b"PSPSDK.LICENSE;1", lbas["PSPSDK.LICENSE;1"], len(license_notice), False)
    )
    sysdir_entries = (
        _dir_record(b"\0", 20, SECTOR, True)
        + _dir_record(b"\1", 19, SECTOR, True)
        + _dir_record(b"EBOOT.BIN;1", lbas["EBOOT.BIN;1"], len(eboot), False)
    )
    for target, content in ((18, root_entries), (19, game_entries), (20, sysdir_entries)):
        if len(content) > SECTOR:
            raise ShowcaseError("showcase ISO directory exceeds one sector")
        start = target * SECTOR
        image[start:start + len(content)] = content
    for name, content in blobs.items():
        start = lbas[name] * SECTOR
        image[start:start + len(content)] = content
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(image)


def stage_iso(out_dir: Path) -> Path:
    """Write the TEST00007 disc the Windows staging smoke stages, with no PSPDEV.

    The Linux gate stages the PSPDEV-built scene. A Windows runner has no PSPDEV, so
    this wraps the source-owned display guest in the showcase's synthetic identity.
    Staging reads the disc identity, the manifest and the executable's own bytes; it
    never runs the guest, so the staging path itself is exercised unchanged (#308).
    """
    demo = DEMOS[0]
    guest_dir = out_dir / "guest"
    completed = subprocess.run(
        [sys.executable, str(ROOT / "tests" / "fixtures" / "display_smoke" / "generate.py"),
         "generate", "--out-dir", str(guest_dir)],
        capture_output=True, text=True, check=False)
    if completed.returncode:
        raise ShowcaseError("display smoke guest generation failed: " + completed.stderr.strip())
    eboot = bytearray((guest_dir / "guest.prx").read_bytes())
    # The guest uses load-relative zero addresses. Only each program header's
    # congruence field changes, to pass the staging preflight; no code or data does.
    phoff = struct.unpack_from("<I", eboot, 28)[0]
    phnum = struct.unpack_from("<H", eboot, 44)[0]
    for index in range(phnum):
        struct.pack_into("<I", eboot, phoff + index * 32 + 28, 0x100)
    iso = out_dir / "TEST00007.iso"
    write_iso(iso, demo["disc_id"], demo["display_name"], make_icon_png(demo["palette"]),
              bytes(eboot), b"Source-owned synthetic notice\n")
    return iso


def _host_path(path: Path) -> str:
    """Spell `path` for the shell that owns the PSP toolchain.

    A Windows host runs the toolchain inside WSL, so a native Windows path has
    to be converted first. A POSIX host already speaks the toolchain's language.
    """
    if os.name == "nt":
        result = subprocess.run(["wsl", "-e", "wslpath", "-u", str(path)],
                                capture_output=True, text=True, check=False)
        if result.returncode:
            raise ShowcaseError("WSL path conversion failed: " + result.stderr.strip())
        return result.stdout.strip()
    return str(path)


def _toolchain_shell(command: str) -> list[str]:
    """Argv that runs `command` in a POSIX shell next to the PSP toolchain."""
    if os.name == "nt":
        return ["wsl", "-e", "bash", "-lc", command]
    return ["bash", "-lc", command]


def _check_pspdev() -> None:
    command = "test -x /usr/local/pspdev/bin/psp-gcc && test -d /usr/local/pspdev/psp/sdk"
    result = subprocess.run(_toolchain_shell(command),
                            capture_output=True, text=True, check=False)
    if result.returncode:
        where = "WSL" if os.name == "nt" else "this host"
        raise ShowcaseError(
            f"PSPDEV/PSPSDK was not found in {where} at /usr/local/pspdev. "
            "Install PSPDEV v20261101 there with the steps in docs/SETUP.md (Showcase demos on Windows, "
            "or Linux headless showcase), then rerun the showcase target."
        )


def _build_prx(demo: dict[str, object], out_dir: Path) -> Path:
    source_dir = ROOT / "tests" / "fixtures" / "showcase" / str(demo["folder"])
    out_dir.mkdir(parents=True, exist_ok=True)
    makefile = _host_path((ROOT / "tests" / "fixtures" / "showcase" / "Makefile").resolve())
    host_out = _host_path(out_dir.resolve())
    host_source = _host_path(source_dir.resolve())
    command = (
        "export PSPDEV=/usr/local/pspdev; export PATH=\"$PSPDEV/bin:$PATH\"; "
        f"make -B -f '{makefile}' -C '{host_out}' CC=psp-gcc VPATH='{host_source}' "
        f"TARGET='{demo['target']}'"
    )
    result = subprocess.run(_toolchain_shell(command), cwd=ROOT, text=True, check=False)
    if result.returncode:
        raise ShowcaseError(f"PSPDEV build failed for {demo['id']} (exit {result.returncode})")
    prx = out_dir / f"{demo['target']}.prx"
    if not prx.is_file():
        raise ShowcaseError(f"PSPDEV did not produce the expected PRX: {prx}")
    return prx


def _build_loadexec_prx(out_dir: Path, target: str, define: str) -> Path:
    """Build one side of the LoadExec chain from the shared PSPSDK source."""
    source_dir = ROOT / "tests" / "fixtures" / "showcase" / "ge_scene"
    out_dir.mkdir(parents=True, exist_ok=True)
    makefile = _host_path((ROOT / "tests" / "fixtures" / "showcase" / "Makefile").resolve())
    host_out = _host_path(out_dir.resolve())
    host_source = _host_path(source_dir.resolve())
    command = (
        "export PSPDEV=/usr/local/pspdev; export PATH=\"$PSPDEV/bin:$PATH\"; "
        f"make -B -f '{makefile}' -C '{host_out}' CC=psp-gcc VPATH='{host_source}' "
        f"TARGET='{target}' SHOWCASE_DEFINES='-D{define}'"
    )
    if define == "NK_LOAD_EXEC_B":
        exports = out_dir / "loadexec_b_exports.exp"
        exports.write_text(
            "PSP_BEGIN_EXPORTS\n"
            "PSP_EXPORT_START(syslib, 0, 0x8000)\n"
            "PSP_EXPORT_FUNC_HASH(module_start)\n"
            "PSP_EXPORT_VAR_HASH(module_info)\n"
            "PSP_EXPORT_END\n"
            "PSP_END_EXPORTS\n",
            encoding="ascii",
        )
        host_exports = _host_path(exports.resolve())
        command += f" PRX_EXPORTS='{host_exports}' SHOWCASE_LDFLAGS='-nostartfiles'"
    result = subprocess.run(_toolchain_shell(command), cwd=ROOT, text=True, check=False)
    if result.returncode:
        raise ShowcaseError(f"PSPDEV build failed for {target} (exit {result.returncode})")
    prx = out_dir / f"{target}.prx"
    if not prx.is_file():
        raise ShowcaseError(f"PSPDEV did not produce the expected PRX: {prx}")
    return prx


def loadexec_smoke() -> None:
    """Build and run project-owned A -> package-local B through production HLE."""
    _check_pspdev()
    root = BUILD_ROOT / "loadexec"
    program_dir = root / "program-a"
    module_dir = root / "program-b"
    manifest_path = root / "title.json"

    source_manifest = json.loads(
        (ROOT / "assets" / "titles" / "showcase-scene.json").read_text(encoding="utf-8")
    )
    manifest = json.loads(json.dumps(source_manifest))
    manifest["id"] = "showcase-loadexec-v1"
    manifest["display_name"] = "Nakagawa LoadExec Showcase"
    manifest["modules"] = [{
        "name": "loadexec_b.prx",
        "required": True,
        "role": "guest-prx",
        "placement": "runtime",
        "guest_path": "ms0:/PSP/GAME/NAKAGAWA/LOADEXEC_B.PRX",
    }]
    manifest["filesystem"]["memory_stick_root"] = "build/showcase/loadexec/package/memstick"
    manifest["profile_zero"]["source_program"]["source_files"] = [
        "tests/fixtures/showcase/ge_scene/main.c",
    ]
    manifest["profile_zero"]["build"]["target"] = "loadexec_a.prx"
    manifest["notes"] = (
        "Source-owned two-program LoadExec smoke; both programs are built from "
        "tests/fixtures/showcase/ge_scene/main.c with PSPDEV/PSPSDK."
    )
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    program_a = _build_loadexec_prx(program_dir, "loadexec_a", "NK_LOAD_EXEC_A")
    program_b = _build_loadexec_prx(module_dir, "loadexec_b", "NK_LOAD_EXEC_B")
    base = int(manifest["executable"]["base"])
    manifest["executable"]["entry"] = validate_psp_prx(program_a, base)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    demo_fingerprint = _package_fingerprint(
        {"id": manifest["id"], "manifest": manifest_path.relative_to(ROOT).as_posix()},
        program_a,
    )
    fingerprint = hashlib.sha256(
        (demo_fingerprint + hashlib.sha256(program_b.read_bytes()).hexdigest()).encode("ascii")
    ).hexdigest()[:16]
    package_dir = root / f"package-{fingerprint}"

    command = [
        sys.executable, str(ROOT / "tools" / "title_codegen_plan.py"),
        str(manifest_path), "--package", "--public-safe",
        "--game-elf", str(program_a), "--module-dir", str(module_dir),
        "--output-dir", str(package_dir), "--python-command", sys.executable,
    ]
    result = subprocess.run(command, cwd=ROOT, check=False)
    if result.returncode:
        raise ShowcaseError(f"LoadExec AOT package build failed (exit {result.returncode})")
    metadata = json.loads((package_dir / "package.json").read_text(encoding="utf-8"))
    executable = package_dir / metadata["executable"]["path"]
    # The package names its image after its executable (the game name), not the manifest id.
    image = package_dir / f"{executable.stem}_image.bin"
    module_root = package_dir / "modules"
    module_root.mkdir(parents=True, exist_ok=True)
    packaged_module = module_root / "loadexec_b.prx"
    shutil.copyfile(program_b, packaged_module)
    if not executable.is_file() or not image.is_file() or not packaged_module.is_file():
        raise ShowcaseError("LoadExec package is missing its executable, image, or B module")

    # Mirror B into the package's virtual Memory Stick tree so the path named by A
    # is also present in the package filesystem used by the smoke.
    memory_stick_root = package_dir / "memstick"
    package_guest_path = memory_stick_root / "PSP" / "GAME" / "NAKAGAWA" / "LOADEXEC_B.PRX"
    package_guest_path.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(packaged_module, package_guest_path)

    driver_command = [
        str(executable), "--image", str(image), f"0x{base:08x}",
        metadata["runtime"]["run_entry"], "none", "none", "--sched",
        "--expect-u32=0x09900000:0x4c454d42",
    ]
    env = os.environ.copy()
    for name in ("SR_DATAROOT", "SR_FSDIR", "PSP_ISO", "SR_BOOT_PATH", "SR_EXIT_AT_VBLANK"):
        env.pop(name, None)
    env["SR_MODULE_DIR"] = str(module_root)
    env["SR_MEMSTICK"] = str(memory_stick_root)
    env["SR_NOVBPACE"] = "1"
    env["SDL_VIDEODRIVER"] = "dummy"
    env["SDL_AUDIODRIVER"] = "dummy"
    result = subprocess.run(driver_command, cwd=package_dir, env=env, text=True,
                            capture_output=True, timeout=30, check=False)
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise ShowcaseError(f"LoadExec runtime failed ({result.returncode}):\n{output[-4000:]}")
    if "LOAD_EXEC_B_ARGV" not in output or "loadexec-argv=project-built" not in output:
        raise ShowcaseError(f"LoadExec B did not print the forwarded argv:\n{output[-4000:]}")
    if "LOAD_EXEC_B_RESET: guest_ram_reset=1" not in output:
        raise ShowcaseError(f"LoadExec did not clear program A's guest RAM:\n{output[-4000:]}")
    if "DRIVER_EXPECT_U32 addr=0x09900000 got=0x4c454d42 expected=0x4c454d42 status=PASS" not in output:
        raise ShowcaseError(f"LoadExec argv marker was not written by B:\n{output[-4000:]}")
    print("SHOWCASE_LOAD_EXEC: PASS project-built A -> package-local B; argv forwarded; no sockets")


def validate_psp_prx(path: Path, base: int) -> int:
    """Require the rebased, relocation-bearing PRX emitted by psp-prxgen."""
    header = path.read_bytes()[:52]
    if len(header) < 52 or header[:4] != b"\x7fELF" or \
            struct.unpack_from("<H", header, 16)[0] != PSP_PRX_ELF_TYPE:
        raise ShowcaseError(
            f"PSPDEV output is not a final relocatable PSP PRX: {path}"
        )
    try:
        image = Prx(str(path), base)
        relocations = image.relocate()
    except (OSError, ValueError) as exc:
        raise ShowcaseError(f"PSPDEV PRX cannot be loaded by Nakagawa: {exc}") from exc
    if relocations == 0:
        raise ShowcaseError(
            f"PSPDEV PRX has no MIPS relocations; use psp-prxgen output: {path}"
        )
    return base + image.entry


def _clear_owned(path: Path, parent: Path) -> None:
    resolved_parent = parent.resolve()
    resolved_path = path.resolve()
    if resolved_path == resolved_parent or resolved_parent not in resolved_path.parents:
        raise ShowcaseError(f"refusing to replace output outside build tree: {path}")
    if path.exists():
        if path.is_symlink():
            raise ShowcaseError(f"refusing to replace symlink output: {path}")
        shutil.rmtree(path)


def _pspsdk_license() -> bytes:
    command = (
        "export PSPDEV=/usr/local/pspdev; "
        "cat \"$PSPDEV/psp/share/licenses/pspsdk/LICENSE\""
    )
    result = subprocess.run(_toolchain_shell(command),
                            capture_output=True, check=False)
    if result.returncode or not result.stdout:
        raise ShowcaseError("Could not read the installed PSPSDK license notice.")
    return result.stdout


def _package_fingerprint(demo: dict[str, object], prx: Path) -> str:
    digest = hashlib.sha256()
    inputs = [
        ROOT / "Makefile", ROOT / "tools" / "copy_build_assets.ps1",
        ROOT / "tools" / "analyze.py", ROOT / "tools" / "codegen.py",
        ROOT / "tools" / "imports.py", ROOT / "tools" / "title_codegen_plan.py",
        ROOT / "tools" / "title_runtime_config.py", ROOT / "tools" / "build_profile.py",
        ROOT / "tools" / "shader_embed.py", ROOT / str(demo["manifest"]), prx,
    ]
    for tree in (ROOT / "src" / "rt", ROOT / "src" / "core", ROOT / "mk"):
        inputs.extend(path for path in tree.rglob("*") if path.is_file())
    for path in sorted(set(inputs), key=lambda item: item.as_posix()):
        try:
            name = path.resolve().relative_to(ROOT.resolve()).as_posix()
        except ValueError:
            name = path.name
        digest.update(name.encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
    return digest.hexdigest()[:16]


# The file `build-prx` writes beside the PRXs, and `build --prebuilt-prx-dir` reads:
# the installed PSPSDK's license notice, which every image and package that links
# PSPSDK code carries.
PSPSDK_LICENSE_NAME = "PSPSDK-LICENSE.txt"


def _check_prx_entry(demo: dict[str, object], prx: Path) -> None:
    """Require the PSPDEV PRX the demo's manifest describes (base and entry)."""
    manifest_path = ROOT / str(demo["manifest"])
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    loaded_entry = validate_psp_prx(prx, int(manifest["executable"]["base"]))
    if loaded_entry != int(manifest["executable"]["entry"]):
        raise ShowcaseError(
            f"{demo['id']} manifest entry 0x{manifest['executable']['entry']:08x} "
            f"does not match the PSPDEV PRX entry 0x{loaded_entry:08x}"
        )


def _prebuilt_prx(demo: dict[str, object], prebuilt_dir: Path, out_dir: Path) -> Path:
    """Copy the PRX `build-prx` wrote for `demo` to where `_build_prx` would put it."""
    source = prebuilt_dir / f"{demo['target']}.prx"
    if not source.is_file():
        raise ShowcaseError(f"prebuilt PRX for {demo['id']} is missing: {source}")
    out_dir.mkdir(parents=True, exist_ok=True)
    prx = out_dir / source.name
    shutil.copyfile(source, prx)
    return prx


def _prebuilt_license(prebuilt_dir: Path) -> bytes:
    path = prebuilt_dir / PSPSDK_LICENSE_NAME
    notice = path.read_bytes() if path.is_file() else b""
    if not notice:
        raise ShowcaseError(f"prebuilt PSPSDK license notice is missing or empty: {path}")
    return notice


def title_runtime_sources(names: set[str]) -> set[str]:
    """The files of a package folder that `nk_add_title_runtime` compiles (CMakeLists.txt).

    The Android `nk.titles` route (docs/ANDROID_DEVELOPMENT.md) builds a title's runtime
    library from the package it is pointed at: `<id>_recomp.c`, `<id>_recomp_*.c`,
    `nk_lib_*.c` and `sr_title_config.h`, plus the generated header they include. Those
    are not run-time files of the PC player but they hold no builder path, so the trim keeps
    them. `names` are file names at the top of the package folder.
    """
    return {name for name in names
            if fnmatch.fnmatchcase(name, "*_recomp.c") or fnmatch.fnmatchcase(name, "*_recomp_*.c")
            or fnmatch.fnmatchcase(name, "nk_lib_*.c") or name == "sr_title_config.h"
            or fnmatch.fnmatchcase(name, "*_recomp_funcs.h")}


def package_game_name(demo: dict[str, object]) -> str:
    """The game name the planner gave this demo's package, taken from the same code.

    The executable's file name is not it: an Android library is `lib<game>.so`.
    """
    import title_codegen_plan  # imported here: the module is large and only the build needs it

    manifest = json.loads((ROOT / str(demo["manifest"])).read_text(encoding="utf-8"))
    return str(title_codegen_plan._package_game_name(manifest, None))


def trim_to_runtime_package(package_dir: Path, game_name: str) -> int:
    """Drop the build's own files from a package folder, as `nk_cli build-package` does (#306).

    The planner leaves the build's files beside the runtime: dependency files, objects, the
    link map and response file and the build stamps. The compiler and linker wrote the folder
    the build ran in into them, so a package that carries them names the builder's machine
    and cannot move. The runtime package (schema 3) declares no generated objects, so none
    of them is a runtime file; the player's own route removes the same set
    (nk_cli._remove_package_build_outputs) and writes the completion manifest over what
    remains. This does the same, then runs the full package check on the result, so the
    manifest still lists every remaining file and its digest, and a damaged package is
    refused before and after. The generated C that the title runtime route compiles stays
    (title_runtime_sources). Call it on the copy that is installed, never on the build
    folder. Returns how many files were removed.
    """
    import nk_cli  # imported here: the module is large and the build route needs it only once

    valid, reason = package_cache.validate_package_cache(package_dir)
    if not valid:
        raise ShowcaseError(f"the planner's package is not valid, so it is not trimmed: {reason}")
    try:
        package = package_cache.read_bounded_json(package_dir / "package.json")
        completion = package_cache.read_bounded_json(package_dir / "completion-manifest.json")
    except (OSError, ValueError) as exc:
        raise ShowcaseError(f"package metadata is unreadable: {exc}") from exc
    if package.get("schema_version") != 3 or package.get("generated_objects") != []:
        raise ShowcaseError("only a runtime package (schema 3, no generated objects) is trimmed")
    key = package_cache.package_cache_key(package_dir)
    if key is None:
        raise ShowcaseError("the package carries no cache key")
    outputs = nk_cli._package_build_output_paths(package_dir)
    kept = title_runtime_sources({name for name in outputs if "/" not in name})
    removed, _ = nk_cli._remove_package_build_outputs(package_dir, game_name, outputs - kept)
    for folder in sorted((p for p in package_dir.rglob("*") if p.is_dir()), reverse=True):
        if not any(folder.iterdir()):
            folder.rmdir()
    package_cache.write_completion_manifest(
        package_dir, key, title_input_identity=package["title_input_identity"],
        backends=completion.get("backends"), limits=completion.get("limits"))
    valid, reason = package_cache.validate_package_cache(package_dir, expected_key=key)
    if not valid:
        raise ShowcaseError(f"the trimmed package failed its check: {reason}")
    return int(removed)


def _build_one(demo: dict[str, object], license_notice: bytes,
               prebuilt_prx_dir: Path | None = None, target: str = "host") -> None:
    title_id = str(demo["id"])
    stage = BUILD_ROOT / title_id
    if prebuilt_prx_dir is None:
        prx = _build_prx(demo, stage / "psp-build")
    else:
        prx = _prebuilt_prx(demo, prebuilt_prx_dir, stage / "psp-build")
    _check_prx_entry(demo, prx)
    icon = make_icon_png(demo["palette"])
    (stage / "ICON0.PNG").write_bytes(icon)
    iso_path = stage / f"{demo['disc_id']}.iso"
    write_iso(iso_path, str(demo["disc_id"]), str(demo["display_name"]), icon,
              prx.read_bytes(), license_notice)

    # A host and an Android package of the same inputs are different builds: each has its
    # own stage, so neither reuses the other's objects or executable.
    fingerprint = _package_fingerprint(demo, prx)
    package_stage = stage / (f"package-{fingerprint}" if target == "host"
                             else f"package-{target}-{fingerprint}")
    if package_stage.is_symlink():
        raise ShowcaseError(f"refusing to use symlink package output: {package_stage}")
    package_stage.mkdir(parents=True, exist_ok=True)
    command = [
        sys.executable, str(ROOT / "tools" / "title_codegen_plan.py"),
        str(ROOT / str(demo["manifest"])), "--package", "--public-safe",
        "--game-elf", str(prx), "--output-dir", str(package_stage),
        "--target", target,
    ]
    # The runtime looks for the SDL3 libraries beside the player it ships under (Linux; the
    # Makefile ignores the setting elsewhere), never in a folder of the building machine.
    env = dict(os.environ)
    if target == "host":
        env["RUNTIME_RUNPATH_ORIGIN"] = RUNTIME_RUNPATH_ORIGIN
    else:
        env.pop("RUNTIME_RUNPATH_ORIGIN", None)  # the APK's own library folder holds SDL3
    result = subprocess.run(command, cwd=ROOT, env=env, check=False)
    if result.returncode:
        raise ShowcaseError(f"analyze/codegen/package failed for {title_id} (exit {result.returncode})")
    package_meta = json.loads((package_stage / "package.json").read_text(encoding="utf-8"))
    if package_meta.get("format") != "nakagawa-aot-package":
        raise ShowcaseError(f"{title_id} did not produce nakagawa-aot-package v1")
    exe = package_stage / package_meta["executable"]["path"]
    if not exe.is_file():
        raise ShowcaseError(f"validated package executable is missing for {title_id}")

    release_dir = DEMO_ROOT / "packages" / str(demo["disc_id"])
    _clear_owned(release_dir, DEMO_ROOT / "packages")
    release_dir.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(package_stage, release_dir)
    # Only the installed copy loses the build's files; the build folder stays whole.
    trim_to_runtime_package(release_dir, package_game_name(demo))
    image = DEMO_ROOT / "images" / f"{demo['disc_id']}.iso"
    image.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(iso_path, image)
    license_dir = DEMO_ROOT / "THIRD_PARTY_NOTICES"
    license_dir.mkdir(parents=True, exist_ok=True)
    (license_dir / "PSPSDK-LICENSE.txt").write_bytes(license_notice)
    (DEMO_ROOT / "data" / title_id).mkdir(parents=True, exist_ok=True)
    print(f"SHOWCASE_PACKAGE: {demo['disc_id']} {release_dir}")


def build_all(prebuilt_prx_dir: Path | None = None, target: str = "host") -> None:
    """Build every demo's image and package into DEMO_ROOT.

    With `prebuilt_prx_dir` (what `build-prx` wrote, on a host that has PSPDEV) the
    PSPDEV step is skipped and those PRXs and that license notice are used, so a host
    without PSPDEV, such as the Windows release job, builds the same packages.

    `target` is the system the packages run on: `host`, or `android` for the packages the
    Android app bundles (title_codegen_plan.py --target android: each package's executable
    is the library lib<game>.so, and the build needs CC naming an Android Clang and
    ANDROID_SDL3_INCLUDE and ANDROID_SDL3_LIBDIR naming the APK's SDL3;
    docs/ANDROID_DEVELOPMENT.md). The demos folder has the same layout either way.
    """
    if prebuilt_prx_dir is None:
        _check_pspdev()
        license_notice = _pspsdk_license()
    else:
        license_notice = _prebuilt_license(prebuilt_prx_dir)
    for demo in DEMOS:
        _build_one(demo, license_notice, prebuilt_prx_dir, target)


def build_prx_all(out_dir: Path) -> None:
    """Build each demo's PSPDEV PRX and write it, with PSPSDK's license, to `out_dir`."""
    _check_pspdev()
    license_notice = _pspsdk_license()
    out_dir.mkdir(parents=True, exist_ok=True)
    for demo in DEMOS:
        prx = _build_prx(demo, BUILD_ROOT / str(demo["id"]) / "psp-build")
        _check_prx_entry(demo, prx)
        target = out_dir / prx.name
        shutil.copyfile(prx, target)
        print(f"SHOWCASE_PRX: {demo['disc_id']} {target}")
    (out_dir / PSPSDK_LICENSE_NAME).write_bytes(license_notice)


def _runtime_command(demo: dict[str, object], padscript: Path | None,
                     pause_script: str | None = None) -> tuple[list[str], dict[str, str], Path]:
    disc_id = str(demo["disc_id"])
    package_dir = DEMO_ROOT / "packages" / disc_id
    meta = json.loads((package_dir / "package.json").read_text(encoding="utf-8"))
    manifest = json.loads((ROOT / str(demo["manifest"])).read_text(encoding="utf-8"))
    exe = package_dir / meta["executable"]["path"]
    image = package_dir / f"{exe.stem}_image.bin"
    base = int(manifest["executable"]["base"])
    command = [str(exe), "--image", str(image), f"0x{base:08x}",
               meta["runtime"]["run_entry"], "none", "none", "--sched"]
    env = os.environ.copy()
    env.pop("SR_PAUSE_SCRIPT", None)
    env.pop("SR_PADSCRIPT", None)
    env.update({
        "SR_AUDIOSTAT": "1",
        "SR_AUDIO_BACKEND": "sdl",
        "SR_GESTAT": "1",
        "SR_FBSNAP": "1",
        "SR_INLOG": "1",
        "SR_NOINPUT": "1",
        "SR_EXIT_AT_VBLANK": str(SMOKE_EXIT_VBLANK),
        "SDL_VIDEODRIVER": "dummy",
        "SDL_AUDIODRIVER": "dummy",
        "SR_MEMSTICK": str(BUILD_ROOT / "smoke" / disc_id / "memstick"),
    })
    # No pad script means no scripted input (SR_NOINPUT keeps the host pad out too); the
    # runtime refuses an empty route file, so a run without presses names no script at all.
    if padscript is not None:
        env["SR_PADSCRIPT"] = str(padscript)
    if pause_script is not None:
        env["SR_PAUSE_SCRIPT"] = pause_script
    return command, env, package_dir


def _headless_capture_command(
    demo: dict[str, object], padscript: Path | None, pause_script: str | None = None
) -> tuple[list[str], dict[str, str], Path]:
    """The scripted run on the explicit headless presenter with one capture switch.

    SR_FBSNAP_WINDOWS is the only capture variable set, so the run proves both that a window
    is self-sufficient (every present inside it is captured) and that the offscreen sink
    publishes the frames it presents. The run must not depend on host speed: with real-time
    vblank pacing a loaded host lets wall-clock vblanks pass without a guest flip, so
    SR_NOVBPACE=1 advances vblanks with the guest instead, and SR_FPS_CAP=0 hands every
    guest flip to the presenter (the wall-clock output cap would legitimately drop, and
    report SKIPPED, presents that land early in their output slot). SR_PRESENT_TRACE=1 logs
    every presented vblank so the published set can be checked against it."""
    command, env, package_dir = _runtime_command(demo, padscript)
    command.append("--gui")
    for name in ("SR_FBSNAP", "SR_FBSNAP_AFTER", "SR_FBDUMP"):
        env.pop(name, None)
    first, last = HEADLESS_CAPTURE_WINDOW
    env.update({
        "SR_VIDEO": "offscreen",
        "SR_FBSNAP_WINDOWS": f"{first}-{last}",
        "SR_NOVBPACE": "1",
        "SR_FPS_CAP": "0",
        "SR_PRESENT_TRACE": "1",
        "SR_EXIT_AT_VBLANK": str(last + 20),
    })
    if pause_script is not None:
        env["SR_PAUSE_SCRIPT"] = pause_script
    return command, env, package_dir


def _ppm_size(path: Path) -> tuple[int, int]:
    parts = path.read_bytes().split(b"\n", 3)
    if len(parts) != 4 or parts[0] != b"P6" or parts[2] != b"255":
        raise ShowcaseError(f"invalid P6 framebuffer capture: {path}")
    width, height = (int(value) for value in parts[1].split())
    if len(parts[3]) != width * height * 3:
        raise ShowcaseError(f"truncated P6 framebuffer capture: {path}")
    return width, height


def _ppm_metrics(path: Path, background: bytes) -> tuple[int, int]:
    parts = path.read_bytes().split(b"\n", 3)
    if len(parts) != 4 or parts[0] != b"P6" or len(background) != 3:
        raise ShowcaseError(f"invalid P6 framebuffer capture: {path}")
    width, height = (int(value) for value in parts[1].split())
    pixels = parts[3]
    if len(pixels) != width * height * 3:
        raise ShowcaseError(f"truncated P6 framebuffer capture: {path}")
    colors: set[bytes] = set()
    foreground = 0
    for offset in range(0, len(pixels), 3):
        pixel = pixels[offset:offset + 3]
        colors.add(pixel)
        foreground += pixel != background
    return len(colors), foreground


def first_frame_checkpoint(demo: dict[str, object], output: str, package_dir: Path,
                           log_path: Path) -> tuple[GeStatWindow, Path]:
    """The window in which a smoke run's guest first drew its frame, and its capture.

    The checkpoint is the first GE statistics window whose counters show the demo's
    own drawing (``frame_counters``), labelled with the vblank that closed it. Both
    are read from the log, never assumed: a loaded host steps VCOUNT past 60 and boots
    the guest later, which moves the checkpoint to a later window but does not make a
    reached checkpoint look missed. The run's vblank budget bounds the wait.
    """
    disc_id = str(demo["disc_id"])
    counters = tuple(str(name) for name in demo["frame_counters"])
    try:
        checkpoint = first_window(output, require=counters)
    except GeStatWindowError as exc:
        raise ShowcaseError(f"{disc_id} {exc}; see {log_path}") from exc
    if checkpoint is None:
        closed = ", ".join(str(window.frame) for window in parse_windows(output)) or "none"
        raise ShowcaseError(
            f"{disc_id} did not reach its first frame checkpoint: no GE statistics window "
            f"before vblank {SMOKE_EXIT_VBLANK} drew {' and '.join(counters)} "
            f"(windows closed at vblank: {closed}); see {log_path}"
        )
    ppm = package_dir / checkpoint.snapshot_name
    if not ppm.is_file():
        raise ShowcaseError(
            f"{disc_id} produced no GE framebuffer capture at its first frame checkpoint "
            f"(vblank {checkpoint.frame}); see {log_path}"
        )
    return checkpoint, ppm


def _padscript(*presses: tuple[int, int, int]) -> str:
    return "".join(f"{vblank} {mask:04x} {width}\n" for vblank, mask, width in presses)


def scripted_press_note(output: str, press: tuple[int, int, int]) -> str:
    """Say whether the guest read a scripted press, for a failed check that depends on it.

    The runtime delivers scripted input in guest time (src/rt/scripted_input.h): a press
    is held until the guest has read it, however far the host falls behind, and a press
    the guest never reads fails the run by name. So when an input, audio or savedata check
    fails, the useful fact is whether the guest ever read the press, from SR_INLOG's
    "guest read scripted" line. A press that was read points at the guest or the HLE, one
    that was never read points at a guest that stopped polling the pad.
    """
    vblank, mask, _width = press
    read = re.search(rf"ctrl_read: vcount=(\d+) guest read scripted 0x{mask:04x}\b", output)
    if read:
        return f" (the guest read the scripted press due at vblank {vblank} at vblank {read.group(1)})"
    return f" (the guest never read the scripted press due at vblank {vblank})"


def _named_failure(output: str) -> str:
    """The runtime's own named failure (ROUTE_FAIL ...) in a log, as a message suffix."""
    match = re.search(r"^ROUTE_FAIL: ([^\n]*)", output, re.MULTILINE)
    return f" ({match.group(1).strip()})" if match else ""


def parse_pause_lines(output: str) -> dict[str, list[dict[str, str]]]:
    """Parse the scheduler's stable SR_PAUSE key/value lines from a runtime log."""
    parsed: dict[str, list[dict[str, str]]] = {
        "requested": [], "entered": [], "resumed": [], "refused": []
    }
    for line in output.splitlines():
        match = re.match(r"^SR_PAUSE: (requested|entered|resumed|refused)\b(.*)$", line)
        if not match:
            continue
        fields = dict(re.findall(r"([a-z_]+)=([^\s]+)", match.group(2)))
        parsed[match.group(1)].append(fields)
    return parsed


def _pause_window(output: str, label: str) -> tuple[dict[str, str], dict[str, str], int, int]:
    records = parse_pause_lines(output)
    if len(records["entered"]) != 1 or len(records["resumed"]) != 1:
        raise ShowcaseError(
            f"{label} expected one SR_PAUSE entered/resumed pair, got "
            f"{len(records['entered'])}/{len(records['resumed'])}; "
            f"requested={records['requested']} refused={records['refused']}"
        )
    lines = output.splitlines()
    entry_index = next(i for i, line in enumerate(lines) if line.startswith("SR_PAUSE: entered "))
    resume_index = next(i for i, line in enumerate(lines) if line.startswith("SR_PAUSE: resumed "))
    if resume_index <= entry_index:
        raise ShowcaseError(f"{label} resumed before its pause entry")
    return records["entered"][0], records["resumed"][0], entry_index, resume_index


def _pause_pair_frozen(output: str, label: str) -> tuple[dict[str, str], dict[str, str]]:
    entered, resumed, entry_index, resume_index = _pause_window(output, label)
    for field in ("vcount", "vbl"):
        if entered.get(field) != resumed.get(field):
            raise ShowcaseError(
                f"{label} {field} changed while paused "
                f"({entered.get(field)} -> {resumed.get(field)})"
            )
    inside = "\n".join(output.splitlines()[entry_index + 1:resume_index])
    if re.search(r"^(?:ctrl_latch:|GESTAT\b)", inside, re.MULTILINE):
        raise ShowcaseError(f"{label} latched guest input or closed a GE window while paused")
    return entered, resumed


def _audio_stats(output: str, label: str) -> dict[str, str]:
    lines = re.findall(r"^AUDIOSTAT_HOST: ([^\n]+)$", output, re.MULTILINE)
    if not lines:
        raise ShowcaseError(f"{label} has no AUDIOSTAT_HOST telemetry")
    return dict(re.findall(r"([a-z_]+)=([^\s]+)", lines[-1]))


def _exit_at_vblank(output: str, label: str) -> int:
    """The run ended through SR_EXIT_AT_VBLANK, at the first VCOUNT at or past its target.

    VCOUNT follows the host clock in the paced smoke, and a host that falls behind advances
    it by several periods at once, so the runtime can stop past the target; never before it.
    """
    match = re.search(
        r"^BOOT_EVENT phase=exit_at_vblank vblanks=(\d+) \(SR_EXIT_AT_VBLANK=(\d+)\)",
        output, re.MULTILINE,
    )
    if not match:
        raise ShowcaseError(f"{label} did not report its SR_EXIT_AT_VBLANK completion")
    actual, target = (int(value) for value in match.groups())
    if actual < target:
        raise ShowcaseError(f"{label} exited at VCOUNT {actual}, before its target {target}")
    return actual


CTRL_LATCH = re.compile(
    r"^ctrl_latch: vcount=(\d+) buttons 0x[0-9a-fA-F]{4} -> 0x([0-9a-fA-F]{4})\b")


def _check_pause_input(output: str, press: tuple[int, int, int], label: str) -> None:
    """The scripted press due around the pause reaches the guest, and never while paused.

    Scripted input is delivered in guest time (src/rt/scripted_input.h): a segment never
    starts before its row's VCOUNT, and it ends only once it has been latched for its width
    and the guest has been handed it; the release that follows is a segment of its own that
    cannot start before the row's end. This check holds those bounds exactly. No latch of the
    press comes before its row, and none while paused. A press latched after the resume is
    the first latch after it. The guest is handed that segment. The release comes after that
    read and not before the row's end.

    There is deliberately no upper bound on when the press is latched, read or released. In
    this real-time smoke VCOUNT is host time: a host that falls behind services several
    vblanks at once, so they come at later VCOUNTs than authored, several samples can share
    one VCOUNT, and VCOUNT can even step past the pause trigger onto the press's row (the
    press is then latched before the pause). A bound there would measure the host's load, not
    the guest; the only limit left is the run's own exit, some 60 vblanks after the row. The
    exact authored timing on a host that keeps up, and each press lasting its full width on a
    starved one, are tested independently of host speed by tools/test_scripted_input.py, and
    hle_thread_selftest drives the same mechanism through the scheduler. The controller
    sampling contract beneath it (empty reads, partial consumption) is #1390.
    """
    _entered, _resumed, entry_index, resume_index = _pause_window(output, label)
    start, mask, width = press
    lines = output.splitlines()
    latches = [(index, latch) for index, line in enumerate(lines)
               if (latch := CTRL_LATCH.match(line))]
    for _index, latch in latches:
        if int(latch.group(2), 16) & mask and int(latch.group(1)) < start:
            raise ShowcaseError(
                f"{label} latched the press at VCOUNT {latch.group(1)}, before its row at {start}")
    pressed = next(((index, latch) for index, latch in latches
                    if int(latch.group(2), 16) & mask), None)
    if pressed is None:
        raise ShowcaseError(f"{label} never latched the scheduled press")
    press_index, press_latch = pressed
    first_latch_vcount = int(press_latch.group(1))
    if entry_index < press_index < resume_index:
        raise ShowcaseError(f"{label} latched the scheduled press while paused")
    first_after_resume = next(index for index, _latch in latches if index > resume_index) \
        if press_index > resume_index else press_index
    if first_after_resume != press_index or int(press_latch.group(2), 16) != mask:
        raise ShowcaseError(f"{label} did not latch the scheduled press first after resume")
    read_pattern = re.compile(
        rf"^ctrl_read: vcount=(\d+) guest read scripted 0x{mask:04x} "
        r"\(latched (\d+) samples since vblank (\d+)\)$"
    )
    read = next(((index, found) for index, line in enumerate(lines)
                 if index > press_index and (found := read_pattern.match(line))), None)
    if read is None:
        raise ShowcaseError(f"{label} never handed the guest the scheduled press")
    read_index, found = read
    read_vcount, samples, since = (int(value) for value in found.groups())
    if not start <= since <= first_latch_vcount or read_vcount < first_latch_vcount or samples < 1:
        raise ShowcaseError(
            f"{label} handed the guest a press segment that does not match its latch "
            f"(first_latch={first_latch_vcount}, read={found.groups()}, row={start})")
    release = next(((index, int(latch.group(1))) for index, latch in latches
                    if index > press_index and not int(latch.group(2), 16) & mask), None)
    if release is None:
        raise ShowcaseError(f"{label} never released the scheduled press before exit")
    if release[0] < read_index:
        raise ShowcaseError(f"{label} released the press before the guest was handed it")
    if release[1] < start + width:
        raise ShowcaseError(
            f"{label} released the press at VCOUNT {release[1]}, before its row ended at "
            f"{start + width}")


def _check_scripted_pause_refusal(output: str, label: str) -> None:
    records = parse_pause_lines(output)
    if records["entered"] or records["resumed"]:
        raise ShowcaseError(f"{label} unexpectedly entered an interactive pause")
    if len(records["refused"]) != 1:
        raise ShowcaseError(f"{label} expected exactly one logged pause refusal")
    refusal = records["refused"][0]
    if refusal.get("source") != "user" or refusal.get("reason") not in {"no_presenter", "automation"}:
        raise ShowcaseError(f"{label} logged the wrong user-pause refusal: {refusal}")


def _run_runtime(command: list[str], env: dict[str, str], cwd: Path,
                 log_path: Path, label: str,
                 timeout_seconds: int = SMOKE_TIMEOUT_SECONDS) -> str:
    """Run one bounded runtime smoke and keep its complete log, then return the output.

    The log is written whether the runtime passes, fails, or overruns the host timeout,
    so a failed gate always leaves the evidence that names its cause.
    """
    # Window captures are per-run evidence: never judge a frame an earlier run left behind.
    for stale in cwd.glob("snap_f*.ppm"):
        stale.unlink()
    try:
        result = subprocess.run(command, cwd=cwd, env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=timeout_seconds, check=False)
    except subprocess.TimeoutExpired as exc:
        partial = exc.stdout or ""
        if isinstance(partial, bytes):
            partial = partial.decode("utf-8", errors="replace")
        log_path.write_text(partial + "\nSHOWCASE_SMOKE_TIMEOUT\n", encoding="utf-8", errors="replace")
        raise ShowcaseError(
            f"{label} exceeded the {timeout_seconds} second smoke window; see {log_path}"
        ) from exc
    log_path.write_text(result.stdout, encoding="utf-8", errors="replace")
    if result.returncode != 0:
        raise ShowcaseError(
            f"{label} exited {result.returncode}{_named_failure(result.stdout)}; see {log_path}")
    return result.stdout


def _window_vblanks(pattern: str, output: str) -> set[int]:
    first, last = HEADLESS_CAPTURE_WINDOW
    return {int(v) for v in re.findall(pattern, output, re.MULTILINE) if first <= int(v) <= last}


def _smoke_headless_capture(demo: dict[str, object], padscript: Path | None, smoke_dir: Path,
                            pause_script: str | None = None) -> list[Path]:
    """Run the demo headless and prove every frame it presented inside the capture window
    was published as a real 480x272 frame, and nothing else was."""
    disc_id = str(demo["disc_id"])
    command, env, package_dir = _headless_capture_command(demo, padscript, pause_script)
    for stale in package_dir.glob("frame_v*.ppm"):
        stale.unlink()
    try:
        timeout_seconds = 15 + ((500 + 999) // 1000 + 2 if pause_script else 0)
        result = subprocess.run(command, cwd=package_dir, env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=timeout_seconds, check=False)
    except subprocess.TimeoutExpired as exc:
        raise ShowcaseError(
            f"{disc_id} headless capture run exceeded the {timeout_seconds} second window"
        ) from exc
    output = result.stdout
    log_path = smoke_dir / "runtime-headless-capture.log"
    log_path.write_text(output, encoding="utf-8", errors="replace")
    if result.returncode != 0:
        raise ShowcaseError(f"{disc_id} headless capture run exited {result.returncode}; see {log_path}")
    if pause_script is not None:
        entered, resumed = _pause_pair_frozen(output, f"{disc_id} headless-capture pause")
        if entered.get("vtime_us") != resumed.get("vtime_us"):
            raise ShowcaseError(f"{disc_id} turbo capture pause advanced guest time; see {log_path}")
    if "BOOT_EVENT phase=window_ready backend=offscreen" not in output:
        raise ShowcaseError(f"{disc_id} did not select the offscreen presenter; see {log_path}")
    presented = _window_vblanks(r"^HOST_PRESENT_SUBMITTED f=(\d+) ", output)
    # A latched flip is reported at the next SetFrameBuf, so read the vblank from the file
    # name the capture was armed with, not from the report's own f= field.
    reported = _window_vblanks(
        r"^FBSNAP f=\d+ swapchain capture -> frame_v(\d+)\.ppm \(result=1\)$", output
    )
    written = {int(path.stem[len("frame_v"):]) for path in package_dir.glob("frame_v*.ppm")}
    if not presented:
        raise ShowcaseError(f"{disc_id} presented no frame inside the capture window; see {log_path}")
    if not (presented == reported and reported == written):
        raise ShowcaseError(
            f"{disc_id} offscreen captures do not match the presented frames "
            f"(presented {sorted(presented)}, published {sorted(reported)}, "
            f"files {sorted(written)}); see {log_path}"
        )
    captures = [package_dir / f"frame_v{vblank}.ppm" for vblank in sorted(written)]
    for capture in captures:
        if _ppm_size(capture) != (480, 272):
            raise ShowcaseError(f"{disc_id} offscreen capture is not a 480x272 frame: {capture}")
        color_count, lit_pixels = _ppm_metrics(capture, bytes((0, 0, 0)))
        if color_count < 4 or lit_pixels < 1000:
            raise ShowcaseError(
                f"{disc_id} offscreen capture is visually empty ({color_count} colors, "
                f"{lit_pixels} non-black pixels); see {capture}"
            )
    return captures


def _run_pause_scenario(demo: dict[str, object], padscript: Path, smoke_dir: Path,
                        pause_script: str, label: str) -> tuple[str, float, Path]:
    command, env, package_dir = _runtime_command(demo, padscript, pause_script)
    log_path = smoke_dir / f"{label}.log"
    host_ms = int(pause_script.split(":", 2)[1])
    started = time.monotonic()
    output = _run_runtime(command, env, package_dir, log_path, f"{demo['disc_id']} {label}",
                          timeout_seconds=_pause_timeout_seconds(host_ms))
    return output, time.monotonic() - started, log_path


def _check_pause_held(resumed: dict[str, str], requested_ms: int, elapsed_seconds: float,
                      label: str) -> None:
    """Check the scheduler's own record of how long it held the guest.

    paused_ms is the host-clock time from the pause's entry to its resume. Without a
    presenter (the headless smoke), a scripted pause resumes only once its deadline has
    passed, so a pause that was skipped or cut short reports less than it asked for: no
    tolerance is needed below the request. That the run's wall time contains the hold only
    guards against a broken clock.
    """
    held = resumed.get("paused_ms", "")
    if not held.isdigit():
        raise ShowcaseError(f"{label} resume record carries no paused_ms")
    if int(held) < requested_ms:
        raise ShowcaseError(
            f"{label} held the scheduler {held} ms, less than the requested {requested_ms} ms")
    if elapsed_seconds * 1000 < int(held):
        raise ShowcaseError(
            f"{label} reported a {held} ms hold inside a {elapsed_seconds:.3f} s run")


def _check_pause_timing_and_vblank(output: str, label: str,
                                   requested_ms: int | None = None,
                                   elapsed_seconds: float | None = None) -> tuple[dict[str, str], dict[str, str]]:
    entered, resumed = _pause_pair_frozen(output, label)
    if entered.get("vtime_us") != resumed.get("vtime_us"):
        raise ShowcaseError(f"{label} advanced guest time while paused")
    if int(entered.get("latency_us", "-1")) > 2 * 16683:
        raise ShowcaseError(f"{label} request-to-entry latency exceeded two display periods")
    if requested_ms is not None:
        if elapsed_seconds is None:
            raise ShowcaseError(f"{label} has no measured run time to hold the pause")
        _check_pause_held(resumed, requested_ms, elapsed_seconds, label)
    return entered, resumed


def _check_pause_smoke(output: str, elapsed_seconds: float, label: str) -> None:
    entered, _resumed = _check_pause_timing_and_vblank(
        output, label, PAUSE_HOLD_MS, elapsed_seconds)
    _exit_at_vblank(output, label)
    _check_pause_input(output, PAUSE_CROSS_PRESS, label)
    # At or after the trigger: a host that falls behind can step VCOUNT past it.
    if int(entered["vcount"]) < PAUSE_TRIGGER_VCOUNT:
        raise ShowcaseError(
            f"{label} entered at vcount {entered['vcount']}, before its trigger "
            f"{PAUSE_TRIGGER_VCOUNT}")


def _check_audio_pause_smoke(output: str, baseline_output: str, label: str,
                             request_vcount: int) -> None:
    entered, resumed = _pause_pair_frozen(output, label)
    if entered.get("vtime_us") != resumed.get("vtime_us"):
        raise ShowcaseError(f"{label} advanced guest time while paused")
    baseline = _audio_stats(baseline_output, f"{label} unpaused baseline")
    paused = _audio_stats(output, label)
    if paused.get("underruns") != baseline.get("underruns"):
        raise ShowcaseError(
            f"{label} changed underruns ({baseline.get('underruns')} -> {paused.get('underruns')})"
        )
    if paused.get("pause_enters") != "1" or int(paused.get("pause_held_frames", "0")) <= 0 or \
            paused.get("pause_stash_failures") != "0":
        raise ShowcaseError(f"{label} has incomplete audio pause telemetry: {paused}")
    requested = parse_pause_lines(output)["requested"]
    observed_request = int(requested[0].get("vcount", "-1")) if requested else -1
    if observed_request < request_vcount:
        raise ShowcaseError(
            f"{label} request was observed at VCOUNT {observed_request}, "
            f"before its target {request_vcount}"
        )
    read = re.search(
        r"^ctrl_read: vcount=(\d+) guest read scripted 0x4000 ", output, re.MULTILINE)
    entry_line = next(line for line in output.splitlines() if line.startswith("SR_PAUSE: entered "))
    read_line = next((line for line in output.splitlines()
                      if line.startswith("ctrl_read:") and "guest read scripted 0x4000" in line), None)
    if not read or not read_line or output.splitlines().index(read_line) > \
            output.splitlines().index(entry_line):
        raise ShowcaseError(f"{label} entered before the guest consumed Cross and queued its tone")
    if int(entered.get("vcount", "0")) < observed_request or \
            resumed.get("vcount") != entered.get("vcount"):
        raise ShowcaseError(f"{label} did not hold VCOUNT after its queued-tone request")


def _check_savedata_pause_smoke(output: str, label: str) -> None:
    entered, resumed = _check_pause_timing_and_vblank(output, label)
    if (entered.get("quit_allowed"), entered.get("reason")) != ("0", "savedata") or \
            (resumed.get("quit_allowed"), resumed.get("reason")) != ("0", "savedata"):
        raise ShowcaseError(f"{label} did not report savedata as blocking Quit")
    if "BOOT_EVENT phase=exit_at_vblank" in output:
        raise ShowcaseError(f"{label} ended by the vblank budget instead of the guest's own exit")


def _check_interactive_pause_refusal(output: str, label: str) -> None:
    _check_scripted_pause_refusal(output, label)


def smoke_all() -> None:
    SCREENSHOT_ROOT.mkdir(parents=True, exist_ok=True)
    padscript_root = BUILD_ROOT / "smoke"
    padscript_root.mkdir(parents=True, exist_ok=True)
    ppm_converter = ROOT / "tools" / "ppm2png.py"
    for demo in DEMOS:
        disc_id = str(demo["disc_id"])
        smoke_dir = padscript_root / disc_id
        smoke_dir.mkdir(parents=True, exist_ok=True)
        main_cross_press = PAUSE_CROSS_PRESS if disc_id == "TEST00008" else CROSS_PRESS
        (smoke_dir / "padscript.txt").write_text(_padscript(main_cross_press, START_PRESS),
                                                 encoding="ascii")
        command, env, package_dir = _runtime_command(demo, smoke_dir / "padscript.txt")
        log_path = smoke_dir / "runtime.log"
        output = _run_runtime(command, env, package_dir, log_path, f"{disc_id} runtime")
        checkpoint, ppm = first_frame_checkpoint(demo, output, package_dir, log_path)
        press_note = scripted_press_note(output, main_cross_press)
        if f"-> 0x{main_cross_press[1]:04x}" not in output:
            raise ShowcaseError(
                f"{disc_id} missed the scripted Cross input sample{press_note}; see {log_path}")
        if "AUDIOSTAT_HOST:" not in output or "pushed=0" in output:
            raise ShowcaseError(
                f"{disc_id} missed audio-submission telemetry{press_note}; see {log_path}")
        color_count, foreground_pixels = _ppm_metrics(ppm, bytes((16, 24, 32)))
        # A flat-colour 2D frame legitimately has only a handful of colours; this
        # catches an empty or single-colour frame, and the screenshots are reviewed.
        if color_count < 4 or foreground_pixels < 1000:
            raise ShowcaseError(
                f"{disc_id} framebuffer is visually empty ({color_count} colors, "
                f"{foreground_pixels} non-background pixels); see {ppm}"
            )
        screenshot = SCREENSHOT_ROOT / f"{disc_id}.png"
        converted = subprocess.run([sys.executable, str(ppm_converter), str(ppm), str(screenshot)],
                                   cwd=ROOT, text=True, capture_output=True, check=False)
        if converted.returncode:
            raise ShowcaseError(f"could not convert {ppm} to PNG: {converted.stderr.strip()}")

        if disc_id == "TEST00007":
            pause_padscript = smoke_dir / "padscript-pause-audio.txt"
            pause_padscript.write_text(_padscript(AUDIO_PAUSE_CROSS_PRESS, START_PRESS),
                                       encoding="ascii")
            audio_output, _audio_seconds, audio_log = _run_pause_scenario(
                demo, pause_padscript, smoke_dir,
                f"{AUDIO_PAUSE_TRIGGER_VCOUNT}:800", "runtime-pause-audio")
            try:
                _check_audio_pause_smoke(audio_output, output, f"{disc_id} audio pause",
                                         AUDIO_PAUSE_TRIGGER_VCOUNT)
            except ShowcaseError as exc:
                raise ShowcaseError(f"{exc}; see {audio_log}") from exc

        if disc_id == "TEST00008":
            pause_padscript = smoke_dir / "padscript-pause.txt"
            pause_padscript.write_text(_padscript(PAUSE_CROSS_PRESS, START_PRESS), encoding="ascii")
            pause_output, pause_seconds, pause_log = _run_pause_scenario(
                demo, pause_padscript, smoke_dir,
                f"{PAUSE_TRIGGER_VCOUNT}:{PAUSE_HOLD_MS}", "runtime-pause")
            try:
                _check_pause_smoke(pause_output, pause_seconds, f"{disc_id} scheduler pause")
            except ShowcaseError as exc:
                raise ShowcaseError(f"{exc}; see {pause_log}") from exc

            save_padscript = smoke_dir / "padscript-save.txt"
            save_padscript.write_text(_padscript(CROSS_PRESS, SAVE_START_PRESS), encoding="ascii")
            save_command, save_env, save_package_dir = _runtime_command(
                demo, save_padscript, "122:1000")
            save_env.pop("SR_EXIT_AT_VBLANK", None)
            save_log_path = smoke_dir / "runtime-save.log"
            try:
                save_output = _run_runtime(
                    save_command, save_env, save_package_dir, save_log_path,
                    f"{disc_id} savedata pause smoke", timeout_seconds=_pause_timeout_seconds(1000))
            except ShowcaseError as exc:
                # The savedata run ends only when the guest acts on its scripted Start.
                save_log = save_log_path.read_text(encoding="utf-8", errors="replace")
                raise ShowcaseError(f"{exc}{scripted_press_note(save_log, SAVE_START_PRESS)}") from exc
            try:
                _check_savedata_pause_smoke(save_output, f"{disc_id} savedata pause")
            except ShowcaseError as exc:
                raise ShowcaseError(f"{exc}; see {save_log_path}") from exc
            save_root = Path(env["SR_MEMSTICK"])
            if not save_root.exists() or not any(path.is_file() for path in save_root.rglob("*")):
                raise ShowcaseError(f"Breakout did not create savedata under {save_root}")
            refusal_command, refusal_env, refusal_package = _runtime_command(
                demo, pause_padscript, "60:1:user")
            refusal_log = smoke_dir / "runtime-pause-refusal.log"
            refusal_output = _run_runtime(
                refusal_command, refusal_env, refusal_package, refusal_log,
                f"{disc_id} interactive pause refusal", timeout_seconds=_pause_timeout_seconds(1))
            try:
                _check_interactive_pause_refusal(refusal_output, f"{disc_id} pause refusal")
            except ShowcaseError as exc:
                raise ShowcaseError(f"{exc}; see {refusal_log}") from exc

        # The capture run presses nothing, so it names no pad script.
        captures = _smoke_headless_capture(demo, None, smoke_dir, HEADLESS_PAUSE_SCRIPT)
        print(f"SHOWCASE_SMOKE: PASS {disc_id} frame/input/audio/pause/headless-capture; "
              f"first frame checkpoint=vblank {checkpoint.frame}; screenshot={screenshot} "
              f"captures={len(captures)} first={captures[0]}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action",
                        choices=("build", "build-prx", "smoke", "stage-iso", "loadexec-smoke",
                                 "audit", "audit-run-paths"))
    parser.add_argument("files", nargs="*", type=Path,
                        help="audit-run-paths: the ELF files whose run paths are audited")
    parser.add_argument("--out-dir", type=Path,
                        help="stage-iso: directory that receives TEST00007.iso; "
                             "build-prx: directory that receives each demo's PRX and "
                             f"{PSPSDK_LICENSE_NAME}")
    parser.add_argument("--prebuilt-prx-dir", type=Path,
                        help="build: use the PRXs and license notice build-prx wrote there "
                             "instead of running PSPDEV")
    parser.add_argument("--target", choices=("host", "android"), default=None,
                        help="build: the system the packages run on (default host); android "
                             "builds the packages the Android app bundles")
    args = parser.parse_args(argv)
    if args.prebuilt_prx_dir is not None and args.action != "build":
        parser.error("--prebuilt-prx-dir applies only to build")
    if args.files and args.action != "audit-run-paths":
        parser.error("files apply only to audit-run-paths")
    if args.action == "audit-run-paths" and not args.files:
        parser.error("audit-run-paths requires at least one file")
    if args.target is not None and args.action != "build":
        parser.error("--target applies only to build")
    try:
        if args.action == "build":
            build_all(args.prebuilt_prx_dir, args.target or "host")
        elif args.action == "build-prx":
            if args.out_dir is None:
                parser.error("build-prx requires --out-dir")
            build_prx_all(args.out_dir)
        elif args.action == "smoke":
            smoke_all()
        elif args.action == "loadexec-smoke":
            loadexec_smoke()
        elif args.action in ("audit", "audit-run-paths"):
            problems = (audit_demo_root(DEMO_ROOT) if args.action == "audit"
                        else audit_run_paths(args.files))
            for problem in problems:
                print(f"SHOWCASE_AUDIT: {problem}", file=sys.stderr)
            if problems:
                raise ShowcaseError(f"{len(problems)} run path or build path finding(s)")
            print(f"SHOWCASE_AUDIT: ok ({args.action})")
        else:
            if args.out_dir is None:
                parser.error("stage-iso requires --out-dir")
            stage_iso(args.out_dir)
            print("SHOWCASE_STAGE_ISO disc_id=TEST00007")
    except (ShowcaseError, OSError, subprocess.SubprocessError, ValueError, KeyError) as exc:
        print(f"SHOWCASE_ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
