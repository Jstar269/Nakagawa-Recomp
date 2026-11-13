# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Headless SDL regressions for the native player's real event/render loop."""

from __future__ import annotations

import atexit
import os
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from typing import Sequence
from urllib.parse import parse_qs, quote, urlsplit
import zipfile
import zlib


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from nk_core import package_cache  # noqa: E402  (the package-layer epochs the fixture must spell)
from test_player_package_route import (  # noqa: E402
    build_homebrew_eboot,
    build_plain_mips_elf,
    build_psp_container,
    create_test_iso_with_executables,
)
PLAYER_EXE = Path(
    os.environ.get("NAKAGAWA_PLAYER_UI_TEST_EXE", "build/nakagawa_player_ui_test.exe")
)
if not PLAYER_EXE.is_absolute():
    PLAYER_EXE = ROOT / PLAYER_EXE

# The UI tests must not discover showcase packages from ``build/demos``.  Keep
# the test executable in a short-lived sibling directory with its runtime DLLs,
# so its executable directory has no demos folder even after ``make showcase``.
_PLAYER_EXE_BUILD_PATH = PLAYER_EXE
_PLAYER_EXE_SANDBOX = None
if _PLAYER_EXE_BUILD_PATH.is_file():
    _PLAYER_EXE_SANDBOX = tempfile.TemporaryDirectory(
        prefix=".player-ui-regressions-", dir=_PLAYER_EXE_BUILD_PATH.parent,
        ignore_cleanup_errors=True,
    )
    # Remove the sandbox copy when the test process exits; a player still holding
    # the executable open on Windows must not turn cleanup into a test failure.
    atexit.register(_PLAYER_EXE_SANDBOX.cleanup)
    PLAYER_EXE = Path(_PLAYER_EXE_SANDBOX.name) / _PLAYER_EXE_BUILD_PATH.name
    shutil.copy2(_PLAYER_EXE_BUILD_PATH, PLAYER_EXE)
    for runtime_dll in _PLAYER_EXE_BUILD_PATH.parent.glob("*.dll"):
        shutil.copy2(runtime_dll, PLAYER_EXE.parent / runtime_dll.name)
    if (PLAYER_EXE.parent / "demos").exists():
        raise RuntimeError("player UI regression executable sandbox contains demos/")

# Starts argv[1] (with the rest of argv) with no console and null standard handles, as
# Explorer starts the GUI player, and exits with its exit code.
CONSOLE_LESS_LAUNCHER = """
import ctypes, subprocess, sys
from ctypes import wintypes
class STARTUPINFOW(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("lpReserved", wintypes.LPWSTR),
                ("lpDesktop", wintypes.LPWSTR), ("lpTitle", wintypes.LPWSTR)] + [
                (name, wintypes.DWORD) for name in ("dwX", "dwY", "dwXSize", "dwYSize",
                "dwXCountChars", "dwYCountChars", "dwFillAttribute", "dwFlags")] + [
                ("wShowWindow", wintypes.WORD), ("cbReserved2", wintypes.WORD),
                ("lpReserved2", ctypes.c_void_p), ("hStdInput", wintypes.HANDLE),
                ("hStdOutput", wintypes.HANDLE), ("hStdError", wintypes.HANDLE)]
class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wintypes.HANDLE), ("hThread", wintypes.HANDLE),
                ("dwProcessId", wintypes.DWORD), ("dwThreadId", wintypes.DWORD)]
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.CreateProcessW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, ctypes.c_void_p,
    ctypes.c_void_p, wintypes.BOOL, wintypes.DWORD, ctypes.c_void_p, wintypes.LPCWSTR,
    ctypes.POINTER(STARTUPINFOW), ctypes.POINTER(PROCESS_INFORMATION)]
info = STARTUPINFOW()
info.cb = ctypes.sizeof(info)
info.dwFlags = 0x00000100  # STARTF_USESTDHANDLES with every handle left null
process = PROCESS_INFORMATION()
command = ctypes.create_unicode_buffer(subprocess.list2cmdline(sys.argv[1:]))
if not kernel32.CreateProcessW(None, command, None, None, False, 0x00000008,  # DETACHED_PROCESS
                               None, None, ctypes.byref(info), ctypes.byref(process)):
    sys.exit(100)
kernel32.WaitForSingleObject(process.hProcess, 60000)
code = wintypes.DWORD()
kernel32.GetExitCodeProcess(process.hProcess, ctypes.byref(code))
kernel32.CloseHandle(process.hThread)
kernel32.CloseHandle(process.hProcess)
sys.exit(code.value)
"""

# NAKAGAWA_PLAYER_UI_SHARD=<k>/<n> runs only the tests whose position in test-id order
# is k modulo n. The Makefile's player-ui-regressions runs all n shards side by side,
# so together they run every test exactly once; unset, every test runs here.
PLAYER_UI_SHARD_ENV = "NAKAGAWA_PLAYER_UI_SHARD"
# NAKAGAWA_PLAYER_UI_ECHO=1 copies each player run's own output to stderr, so a
# repeat check can see the player's markers (UI_NEXT_ACTIVE) on passing runs too.
PLAYER_UI_ECHO_ENV = "NAKAGAWA_PLAYER_UI_ECHO"


def parse_shard(spec: str) -> tuple[int, int]:
    match = re.fullmatch(r"(0|[1-9][0-9]{0,3})/([1-9][0-9]{0,3})", spec)
    if match is None or int(match.group(1)) >= int(match.group(2)):
        raise ValueError(f"{PLAYER_UI_SHARD_ENV} must be k/n with 0 <= k < n, not {spec!r}")
    return int(match.group(1)), int(match.group(2))


def _flatten(suite: unittest.TestSuite):
    for test in suite:
        if isinstance(test, unittest.TestSuite):
            yield from _flatten(test)
        else:
            yield test


def select_shard(tests: unittest.TestSuite, spec: str) -> unittest.TestSuite:
    """The tests of shard k/n in test-id order. A shard that admits nothing is refused."""
    shard, count = parse_shard(spec)
    ordered = sorted(_flatten(tests), key=lambda test: test.id())
    selected = [test for index, test in enumerate(ordered) if index % count == shard]
    if not selected:
        raise ValueError(f"{PLAYER_UI_SHARD_ENV}={spec} admits none of {len(ordered)} tests")
    return unittest.TestSuite(selected)


def load_tests(loader: unittest.TestLoader, standard_tests: unittest.TestSuite,
               pattern: str | None) -> unittest.TestSuite:
    spec = os.environ.get(PLAYER_UI_SHARD_ENV, "").strip()
    return select_shard(standard_tests, spec) if spec else standard_tests


# The player accepts a package only for the CpuState ABI it was compiled with, so a
# synthetic ready package must name that same version (src/rt/recomp.h).
CPU_STATE_ABI_VERSION = int(
    re.search(
        r"#define SR_CPUSTATE_ABI_VERSION (\d+)u",
        (ROOT / "src" / "rt" / "recomp.h").read_text(encoding="utf-8"),
    ).group(1)
)


def read_bmp(path: Path) -> tuple[int, int, int, bytes]:
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise AssertionError("SDL did not save a Windows BMP frame")
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    dib_size = struct.unpack_from("<I", data, 14)[0]
    if dib_size < 40:
        raise AssertionError(f"unsupported BMP DIB header size {dib_size}")
    width, signed_height = struct.unpack_from("<ii", data, 18)
    bits_per_pixel = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]
    if (
        width <= 0
        or signed_height == 0
        or bits_per_pixel not in (24, 32)
        or compression not in (0, 3)
        or (compression == 3 and bits_per_pixel != 32)
    ):
        raise AssertionError("SDL BMP has an unsupported pixel layout")
    if compression == 3:
        red_mask, green_mask, blue_mask, _alpha_mask = struct.unpack_from("<IIII", data, 54)
        if (red_mask, green_mask, blue_mask) != (0x00FF0000, 0x0000FF00, 0x000000FF):
            raise AssertionError("SDL BMP uses unexpected channel masks")
    return width, abs(signed_height), bits_per_pixel, data[pixel_offset:]


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    body = kind + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))


def png_pixel(red: int, green: int, blue: int) -> bytes:
    return (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
        + png_chunk(b"IDAT", zlib.compress(b"\x00" + bytes((red, green, blue, 255))))
        + png_chunk(b"IEND", b"")
    )


def synthetic_art_iso(
    path: Path, *, include_picture: bool = True, icon_bytes: bytes | None = None,
    source_executable: bytes | None = None,
) -> None:
    sector_size = 2048
    icon_image = png_pixel(50, 190, 120) if icon_bytes is None else icon_bytes
    assert len(icon_image) <= sector_size, "the synthetic icon must fit one sector"
    image = bytearray((23 if source_executable is not None else 22) * sector_size)
    if source_executable is not None:
        assert len(source_executable) <= sector_size, "the synthetic executable must fit one sector"

    def both_endian(value: int) -> bytes:
        return struct.pack("<I", value) + struct.pack(">I", value)

    def record(name: bytes, lba: int, size: int, directory: bool) -> bytes:
        record_size = 33 + len(name)
        if record_size & 1:
            record_size += 1
        result = bytearray(record_size)
        result[0] = record_size
        result[2:10] = both_endian(lba)
        result[10:18] = both_endian(size)
        result[25] = 2 if directory else 0
        result[28] = 1
        result[31] = 1
        result[32] = len(name)
        result[33 : 33 + len(name)] = name
        return bytes(result)

    pvd = memoryview(image)[16 * sector_size : 17 * sector_size]
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[158:166] = both_endian(17)
    pvd[166:174] = both_endian(sector_size)
    pvd[156:190] = record(b"\x00", 17, sector_size, True)

    root_records = (
        record(b"\x00", 17, sector_size, True)
        + record(b"\x01", 17, sector_size, True)
        + record(b"PSP_GAME", 18, sector_size, True)
    )
    game_records = (
        record(b"\x00", 18, sector_size, True)
        + record(b"\x01", 17, sector_size, True)
        + record(b"ICON0.PNG;1", 19, len(icon_image), False)
    )
    picture_size = 0
    if include_picture:
        game_records += record(b"PIC1.PNG;1", 20, len(png_pixel(50, 100, 190)), False)
        picture_size = len(png_pixel(50, 100, 190))
    if source_executable is not None:
        game_records += record(b"SYSDIR", 21, sector_size, True)
    image[17 * sector_size : 17 * sector_size + len(root_records)] = root_records
    image[18 * sector_size : 18 * sector_size + len(game_records)] = game_records
    image[19 * sector_size : 19 * sector_size + len(icon_image)] = icon_image
    if picture_size:
        image[20 * sector_size : 20 * sector_size + picture_size] = png_pixel(50, 100, 190)
    if source_executable is not None:
        sysdir_records = (
            record(b"\x00", 21, sector_size, True)
            + record(b"\x01", 18, sector_size, True)
            + record(b"EBOOT.BIN;1", 22, len(source_executable), False)
        )
        image[21 * sector_size : 21 * sector_size + len(sysdir_records)] = sysdir_records
        image[22 * sector_size : 22 * sector_size + len(source_executable)] = source_executable
    path.write_bytes(image)


def write_large_library(scratch: Path, data: Path, count: int) -> None:
    """A saved library of `count` experimental titles, each on its own synthetic
    disc with its own one-pixel ICON0.PNG, in the player's data folder `data`."""
    discs = scratch / "large-library"
    discs.mkdir()
    games = []
    for index in range(count):
        disc_id = f"SCAL{index:05d}"
        iso = discs / f"{disc_id}.iso"
        synthetic_art_iso(iso, include_picture=False,
                          icon_bytes=png_pixel(index % 251, (index * 7) % 251, 90))
        games.append({
            "disc_id": disc_id, "title_name": f"Synthetic title {index}",
            "iso_path": str(iso), "title_id": f"experimental-{disc_id.lower()}",
            "is_experimental": True, "selected_executable": "EBOOT.BIN",
        })
    data.mkdir(parents=True)
    (data / "library.json").write_text(
        json.dumps({"schema_version": 1, "games": games}), encoding="utf-8")


def synthetic_pbp_package(path: Path, *, disc_id: str = "TEST00424",
                          title: str = "Store Package") -> None:
    """Write a synthetic PlayStation Store package named like a disc image.

    The package carries "\0PBP" at offset 0 and a PARAM.SFO in its first
    section. Nothing in it is executed or extracted; it exists only so the
    player has a file whose extension lies about its format.
    """
    entries = sorted([
        ("DISC_ID", disc_id.encode("utf-8") + b"\0"),
        ("TITLE", title.encode("utf-8") + b"\0"),
    ])
    key_table = bytearray()
    data_table = bytearray()
    entry_table = bytearray()
    for key, value in entries:
        key_offset = len(key_table)
        key_table.extend(key.encode("utf-8") + b"\0")
        data_offset = len(data_table)
        data_table.extend(value)
        while len(data_table) % 4:
            data_table.append(0)
        entry_table.extend(
            struct.pack("<HHIII", key_offset, 0x0204, len(value), len(value), data_offset)
        )
    key_table_start = 20 + len(entry_table)
    data_table_start = key_table_start + len(key_table)
    while data_table_start % 4:
        key_table.append(0)
        data_table_start += 1
    sfo = bytes(
        struct.pack("<4s4sIII", b"\x00PSF", b"\x01\x01\x00\x00",
                    key_table_start, data_table_start, len(entries))
    ) + bytes(entry_table) + bytes(key_table) + bytes(data_table)

    header = bytearray(40)
    header[:4] = b"\x00PBP"
    struct.pack_into("<I", header, 4, 0x00010000)
    offsets = [40] + [40 + len(sfo)] * 7
    for index, value in enumerate(offsets):
        struct.pack_into("<I", header, 8 + index * 4, value)
    path.write_bytes(bytes(header) + sfo)


def synthetic_stale_package(runtime_root: Path) -> None:
    package = runtime_root / "packages" / "TEST00006"
    package.mkdir(parents=True)
    package_json = {
        "format": "nakagawa-aot-package",
        "schema_version": 1,
        "title": {
            "id": "stale-synthetic-title",
            "display_name": "Synthetic stale package",
            "kind": "retail",
            "manifest_sha256": "0" * 64,
            "protected_digest": "0" * 64,
        },
        "inputs": {},
        "runtime": {},
        "executable": {"path": "stale-synthetic.exe"},
        "cache": {},
        "generated_objects": [],
        "required_local_assets": [],
        "build_report": "build-report.json",
    }
    (package / "package.json").write_text(json.dumps(package_json), encoding="utf-8")
    (package / "build-report.json").write_text("{}", encoding="utf-8")
    (package / "stale-synthetic.exe").write_bytes(b"synthetic stale executable")


def break_package_manifest(runtime_root: Path) -> None:
    """Make a built package unreadable, as an upgrade would: the validator calls it incompatible."""
    package = runtime_root / "packages" / "TEST00006" / "package.json"
    assert package.is_file()
    package.write_text("{ not a package manifest", encoding="utf-8")


# The runtime source digest package_cache.source_tree_digest gives the v0.0.1 tag:
# what every package v0.0.1 built records in cache.key.native.components.
V001_RUNTIME_SOURCE_DIGEST = "d1de93861db9fa8c572e2ff473bc6aab1245e8338469d488e5cebbe0fce80beb"


def current_build_identity() -> dict[str, str]:
    """What a package build from this checkout records about its Nakagawa
    (NkPackageBuildIdentity in src/core/nk_title_manifest.h)."""
    return {
        "runtime_source_digest": package_cache.source_tree_digest(ROOT),
        "analyzer_sha256": package_cache.analyzer_sources_sha256(ROOT / "tools"),
        "codegen_sha256": package_cache.sha256_file(ROOT / "tools" / "codegen.py"),
        "planner_sha256": package_cache.sha256_file(ROOT / "tools" / "title_codegen_plan.py"),
    }


def synthetic_ready_package(
    runtime_root: Path,
    source_executable: bytes,
    *,
    data_root_present: bool = True,
    disc_id: str = "TEST00006",
    title_id: str = "display-smoke-v1",
    built_by: str | None = None,
) -> None:
    """Write a source-owned package accepted by the real native validator.

    ``built_by`` names the Nakagawa the package records as its builder:
    ``"current"`` for this checkout, ``"v0.0.1"`` for the same checkout with
    v0.0.1's runtime sources, and None for no build identity at all (all-zero
    tool digests and no planner digest), which the player does not judge."""
    package = runtime_root / "packages" / disc_id
    package.mkdir(parents=True)
    executable_name = f"{title_id}.exe"
    image_name = f"{title_id}_image.bin"
    executable_bytes = b"synthetic ready package executable\n"
    input_executable_hash = hashlib.sha256(source_executable).hexdigest()
    executable_hash = hashlib.sha256(executable_bytes).hexdigest()
    zero_hash = "0" * 64

    def write_json(path: Path, value: object) -> None:
        path.write_text(json.dumps(value, separators=(",", ":")) + "\n", encoding="utf-8")

    def canonical_hash(value: object) -> str:
        canonical = json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n"
        return hashlib.sha256(canonical.encode("utf-8")).hexdigest()

    identity = {
        "container": None,
        "disc": {"disc_version": None, "id": disc_id, "region": "TEST"},
        "format": "nakagawa-title-input-identity",
        "main_executable": {"name": "EBOOT.BIN", "sha256": input_executable_hash},
        "manifest": {"id": title_id, "schema_version": 1},
        "modules": [],
        "param_sfo": None,
        "psp_header": None,
        "schema_version": 2,
        "source_media": {
            "executable": {
                "path": "PSP_GAME/SYSDIR/EBOOT.BIN",
                "sha256": input_executable_hash,
            },
            "modules": [],
        },
    }
    identity_text = json.dumps(identity, separators=(",", ":")) + "\n"
    identity_digest = canonical_hash(identity)
    identity_path = runtime_root / "title-input-identities" / disc_id / "title-input-identity.json"
    identity_path.parent.mkdir(parents=True)
    identity_path.write_text(identity_text, encoding="utf-8")

    builder: dict[str, str | None] = {
        "runtime_source_digest": zero_hash, "analyzer_sha256": zero_hash,
        "codegen_sha256": zero_hash, "planner_sha256": None,
    }
    if built_by is not None:
        builder = dict(current_build_identity())
        if built_by == "v0.0.1":
            builder["runtime_source_digest"] = V001_RUNTIME_SOURCE_DIGEST
        elif built_by != "current":
            raise ValueError(f"unknown package builder: {built_by}")
    codegen_options = (
        {} if builder["planner_sha256"] is None
        else {"planner_sha256": builder["planner_sha256"]}
    )
    aot_components = {
        "analyzer_codegen_epoch": "analyzer-codegen-v1",
        "analyzer_sha256": builder["analyzer_sha256"],
        "codegen_options_sha256": canonical_hash(codegen_options),
        "codegen_sha256": builder["codegen_sha256"],
        "executable_sha256": input_executable_hash,
        "generated_code_abi_epoch": package_cache.GENERATED_CODE_ABI_EPOCH,
        "manifest_sha256": zero_hash,
        "modules_sha256": canonical_hash([]),
        "psp_header_sha256": None,
        "runtime_abi_epoch": package_cache.RUNTIME_ABI_EPOCH,
        "title_input_identity_sha256": identity_digest,
    }
    native_components = {
        "compile_flags": "",
        "compiler_identity": "gcc-fixture",
        "compiler_target": "fixture-target",
        "generated_code_digest": zero_hash,
        "link_flags": "",
        "runtime_abi_epoch": package_cache.RUNTIME_ABI_EPOCH,
        "runtime_source_digest": builder["runtime_source_digest"],
    }
    cache_key = {
        "schema_version": 2,
        "aot": {"digest": canonical_hash(aot_components), "components": aot_components},
        "native": {"digest": canonical_hash(native_components), "components": native_components},
    }
    cache = {
        "format": "nakagawa-aot-cache",
        "schema_version": 2,
        "key": cache_key,
        "codegen_options": codegen_options,
        "runtime_abi_compatibility": {
            "current_epoch": package_cache.RUNTIME_ABI_EPOCH, "generated_code_reusable": True,
        },
    }
    inputs = {
        "manifest": {"sha256": zero_hash},
        "executable": {"sha256": input_executable_hash},
        "modules": [],
        "psp_header": None,
    }
    report = {
        "format": "nakagawa-build-report",
        "schema_version": 1,
        "title_id": title_id,
        "runtime_abi": {"name": "CpuState", "version": CPU_STATE_ABI_VERSION},
        "cache": cache,
        "input_hashes": inputs,
        "tools": {},
        "coverage": {},
        "unsupported": {"imports": [], "instructions": [], "regions": []},
        "analysis_diagnostics": [],
        "artifacts": {},
    }
    package_json = {
        "format": "nakagawa-aot-package",
        "schema_version": 2,
        "cache": cache,
        "title": {
            "id": title_id,
            "display_name": "Synthetic fixture",
            "kind": "retail",
            "manifest_sha256": zero_hash,
            "protected_digest": zero_hash,
        },
        "inputs": inputs,
        "title_input_identity": identity,
        "runtime": {
            "abi": "CpuState",
            "abi_version": CPU_STATE_ABI_VERSION,
            "abi_header_sha256": zero_hash,
            "run_entry": "0x00000000",
            "runtime_contract": None,
            "runtime_bindings": {},
            "required_runtime_bindings": [],
        },
        "executable": {
            "path": executable_name,
            "sha256": executable_hash,
            "guest_entry": "0x00000000",
        },
        "generated_objects": [],
        "required_local_assets": [],
        "build_report": "build-report.json",
    }

    executable = package / executable_name
    image = package / image_name
    executable.write_bytes(executable_bytes)
    image.write_bytes(b"synthetic runtime image\n")
    write_json(package / "build-report.json", report)
    write_json(package / "package.json", package_json)

    completion = {
        "format": "nakagawa-aot-cache-completion",
        "schema_version": 2,
        "status": "complete",
        "cache_key": cache_key,
        "title_input_identity": identity,
        "artifacts": [
            {"path": "package.json", "sha256": hashlib.sha256((package / "package.json").read_bytes()).hexdigest()},
            {"path": "build-report.json", "sha256": hashlib.sha256((package / "build-report.json").read_bytes()).hexdigest()},
            {"path": executable_name, "sha256": hashlib.sha256(executable.read_bytes()).hexdigest()},
            {"path": image_name, "sha256": hashlib.sha256(image.read_bytes()).hexdigest()},
        ],
    }
    write_json(package / "completion-manifest.json", completion)
    if data_root_present:
        (runtime_root / "fixtures" / "display_smoke").mkdir(parents=True, exist_ok=True)


def synthetic_experimental_profile(
    runtime_root: Path, disc_id: str, source_executable: bytes | None
) -> None:
    """Write a synthetic experimental profile for an imported or built title.

    ``None`` is the valid imported state when EBOOT.BIN was encrypted and no
    executable has been selected. Bytes model a profile after a passing build.
    """
    title_id = f"experimental-{disc_id.lower()}"
    executable_hash = (
        hashlib.sha256(source_executable).hexdigest()
        if source_executable is not None else None
    )
    profile = {
        "schema_version": 1,
        "manifest": {
            "schema_version": 1,
            "id": title_id,
            "game_name": title_id,
            "display_name": "Synthetic experiment",
            "kind": "retail",
            "disc": {"id": disc_id, "region": "NA", "revision_policy": "exact-disc-id"},
            "executable": {
                "base": 0,
                "entry": 0,
                "bss_metadata_source": "none",
                "extra_executable_spans": [],
            },
            "modules": [],
            "filesystem": {
                "data_root": "",
                "memory_stick_root": "savedata",
                "device_prefixes": ["disc0:", "ms0:"],
            },
            "hle_profile": "generic",
            "feature_requirements": [],
            "verification_profile": "experimental-unverified",
        },
        "input_identity": {
            "disc_id": disc_id,
            "selected_executable": (
                "PSP_GAME/SYSDIR/EBOOT.BIN" if source_executable is not None else ""
            ),
            "executable_sha256": executable_hash,
            "elf_sha256": executable_hash,
        },
    }
    profile_path = runtime_root / "experimental" / disc_id / "profile.json"
    profile_path.parent.mkdir(parents=True)
    profile_path.write_text(json.dumps(profile, separators=(",", ":")) + "\n", encoding="utf-8")


def parse_frames(output: str) -> list[dict[str, str]]:
    frames = []
    for line in output.splitlines():
        if not line.startswith("[PLAYER_UI_TEST] frame="):
            continue
        fields = dict(re.findall(r"([a-z_]+)=(\S+)", line))
        if "frame" not in fields or "view" not in fields:
            raise AssertionError(f"malformed player frame record: {line}")
        frames.append(fields)
    return frames


DEFAULT_BASE_TIMEOUT_SECONDS: float = 30.0
TIMEOUT_BUFFER_SECONDS: float = 15.0


def compute_effective_timeout(
    event_script: Sequence[str],
    explicit_timeout: float | None = None,
) -> float:
    """Compute adaptive subprocess timeout based on scripted UI test events.

    Scripted wait events parse as:
      - WAIT_MS=<duration_ms>: waits for explicit millisecond duration.
      - WAIT_VIEW=<view_mask>,<timeout_ms>: waits up to timeout_ms for view match.
      - WAIT_PACKAGE_STATUS=<status>,<timeout_ms>: waits for package validation.
      - WAIT_UPDATE=<phase>,<timeout_ms>: waits until the update phase is <phase>, by its
        "update=" name (available, downloaded, installed, failed, idle, ...); the run fails,
        naming the phase it last saw, when timeout_ms passes first.
      - WAIT_REPORT=<phase>,<timeout_ms>: waits for the Problem report phase number.

    When explicit_timeout is None, sums all wait event bounds and adds
    TIMEOUT_BUFFER_SECONDS over DEFAULT_BASE_TIMEOUT_SECONDS to ensure slow
    or heavily loaded test runners do not trigger premature TimeoutExpired.
    """
    if explicit_timeout is not None:
        return explicit_timeout
    effective = DEFAULT_BASE_TIMEOUT_SECONDS
    for ev in event_script:
        if ev.startswith("WAIT_MS="):
            try:
                effective += int(ev.split("=", 1)[1]) / 1000.0
            except ValueError:
                pass
        elif ev.startswith(("WAIT_VIEW=", "WAIT_REPORT=")) and "," in ev:
            try:
                effective += int(ev.split(",", 1)[1]) / 1000.0
            except ValueError:
                pass
        elif ev.startswith(("WAIT_PACKAGE_STATUS=", "WAIT_UPDATE=")) and "," in ev:
            try:
                effective += int(ev.rsplit(",", 1)[1]) / 1000.0
            except ValueError:
                pass
    return effective + TIMEOUT_BUFFER_SECONDS


class PlayerUiIdSourceTests(unittest.TestCase):
    """Two controls with one PLAYER_UI_ID value share a layout element, and the second
    one's screen lays out empty (the title page's Report a problem and Rebuild once did)."""

    def test_every_player_ui_id_value_is_unique(self) -> None:
        header = (ROOT / "src" / "player" / "ui" / "player_ui_nk.h").read_text(encoding="utf-8")
        seen: dict[int, str] = {}
        for name, value in re.findall(r"\b(PLAYER_UI_ID_\w+)\s*=\s*(0x[0-9A-Fa-f]+)u", header):
            number = int(value, 16)
            self.assertNotIn(number, seen, f"{name} reuses {value} from {seen.get(number)}")
            seen[number] = name
        self.assertGreater(len(seen), 50)

    # nk_ui.h's id scheme: the files that allocate element ids, each through its
    # screen's macro.
    ID_FILES = ("src/player/ui/*.c", "src/player/ui/scr_*.h", "src/rt/overlay/*.c",
                "src/rt/overlay/*.h")

    @staticmethod
    def check_families(owner: str, ids: dict[str, int], spans: dict[str, int]) -> list[str]:
        """Problems with one screen's ids: a value used twice, an id inside a family
        that is not its own, or two families that overlap."""
        problems = []
        by_value: dict[int, str] = {}
        for name, value in sorted(ids.items()):
            if value in by_value:
                problems.append(f"{owner}: {name} and {by_value[value]} are both {value:#x}")
            by_value[value] = name
        ranges = sorted((ids[name], ids[name] + span, name) for name, span in spans.items())
        for (_, end, name), (next_start, _, next_name) in zip(ranges, ranges[1:], strict=False):
            if next_start < end:
                problems.append(f"{owner}: families {name} and {next_name} overlap")
        for start, end, family in ranges:
            for name, value in ids.items():
                if name != family and start <= value < end:
                    problems.append(f"{owner}: {name} ({value:#x}) is inside family {family}")
        return problems

    def test_every_screen_id_comes_from_the_central_table(self) -> None:
        """One allocation scheme for every element id (nk_ui.h): a screen's ids are
        NK_UI_ID(screen, offset) through one macro per screen, with the screen's
        byte from the table; each id family made by adding to a base declares a
        _SPAN that holds every id made from it, literal offsets included; no id
        falls inside another's family, and no screen uses one value twice."""
        header = (ROOT / "src" / "ui" / "nk_ui.h").read_text(encoding="utf-8")
        table = {name: int(value, 16) for name, value in
                 re.findall(r"^#define NK_UI_ID_SCREEN_(\w+) (0x[0-9A-Fa-f]+)u", header, re.M)}
        self.assertEqual(table.pop("SHARED"), 0, "screen 0x00 is the shared ids'")
        self.assertEqual(len(set(table.values())), len(table), "two screens share a byte")
        self.assertTrue(all(0 < byte < 0x80 for byte in table.values()),
                        "0x80 and up belong to nk_ui_child_id")
        ids: dict[str, dict[str, int]] = {}
        spans: dict[str, dict[str, int]] = {}
        problems: list[str] = []
        sources: list[tuple[Path, str]] = []
        for pattern in self.ID_FILES:
            for path in sorted(ROOT.glob(pattern)):
                text = path.read_text(encoding="utf-8")
                sources.append((path, text))
                where = path.relative_to(ROOT).as_posix()
                macros = {}
                for prefix, screen in re.findall(
                        r"^#define (\w+)_ID\(offset\) NK_UI_ID\(NK_UI_ID_SCREEN_(\w+), \(offset\)\)",
                        text, re.M):
                    self.assertIn(screen, table, f"{where}: {prefix}_ID names screen {screen}, "
                                  "which nk_ui.h's table does not have")
                    macros[prefix] = screen
                for name in re.findall(r"^#define (\w+_ID_\w+)\s+0x[0-9A-Fa-f]+u?\b", text, re.M):
                    if not name.endswith("_SPAN"):
                        problems.append(f"{where}: {name} is a raw value; make it with its "
                                        "screen's _ID(offset) macro")
                for name, prefix, offset in re.findall(
                        r"^#define (\w+_ID_\w+)\s+(\w+)_ID\(0x([0-9A-Fa-f]{6})u\)", text, re.M):
                    self.assertIn(prefix, macros, f"{where}: {name} uses {prefix}_ID, which "
                                  "this file does not define")
                    screen = macros[prefix]
                    ids.setdefault(screen, {})[name] = (table[screen] << 24) + int(offset, 16)
                for name, span in re.findall(r"^#define (\w+_ID_\w+)_SPAN (0x[0-9A-Fa-f]+|\d+)u",
                                             text, re.M):
                    owner = next((s for s, named in ids.items() if name in named), None)
                    self.assertIsNotNone(owner, f"{where}: {name}_SPAN has no id {name}")
                    spans.setdefault(str(owner), {})[name] = int(span, 0)
        self.assertGreater(sum(len(named) for named in ids.values()), 200)
        every = {name: screen for screen, named in ids.items() for name in named}
        for path, text in sources:
            where = path.relative_to(ROOT).as_posix()
            for name, literal in re.findall(r"\b(\w+_ID_\w+)\s*\+\s*(0x[0-9A-Fa-f]+|\d+)?", text):
                if name not in every or name.endswith("_SPAN"):
                    continue
                span = spans.get(every[name], {}).get(name)
                if span is None:
                    problems.append(f"{where}: ids are made by adding to {name}, "
                                    f"which declares no {name}_SPAN")
                elif literal and int(literal, 0) >= span:
                    problems.append(f"{where}: {name} + {literal} is past its span {span:#x}")
        for screen, named in ids.items():
            problems += self.check_families(screen, named, spans.get(screen, {}))

        shared_header = (ROOT / "src" / "player" / "ui" / "player_ui_nk.h").read_text(encoding="utf-8")
        shared = {name: int(value, 16) for name, value in
                  re.findall(r"\b(PLAYER_UI_ID_\w+)\s*=\s*(0x[0-9A-Fa-f]+)u", shared_header)}
        shared_spans = {name: int(span, 0) for name, span in
                        re.findall(r"^#define (PLAYER_UI_ID_\w+)_SPAN (0x[0-9A-Fa-f]+)u",
                                   shared_header, re.M)}
        self.assertTrue(all(value < 0x01000000 for value in shared.values()),
                        "the shared ids are screen 0x00's")
        problems += self.check_families("shared", shared, shared_spans)
        self.assertEqual(problems, [])


class ErrorCodeSourceTests(unittest.TestCase):
    """#2007: every code the player raises as a card has a badge, so none falls through to a
    bare "Problem". The codes are read from the source, so a new one fails here until
    player_error_badge (player_state.c) knows it."""

    RAISERS = re.compile(
        r"player_app_(?:set_error|set_error_with_details|prereq_fail)\(\s*&?\w+\s*,"
        r"\s*\"([A-Z][A-Z0-9_]+)\"")

    def test_every_literal_error_code_has_a_badge(self) -> None:
        state = (ROOT / "src" / "player" / "player_state.c").read_text(encoding="utf-8")
        table = state[state.index("k_error_badges[]"):state.index("const char *player_error_badge")]
        codes_part, families_part = table.split("k_error_badge_families")
        exact = set(re.findall(r'\{"([A-Z][A-Z0-9_]+)",\s*"', codes_part))
        prefixes = re.findall(r'\{"([A-Z][A-Z0-9_]*_)",\s*"', families_part)
        self.assertGreater(len(exact), 30)
        codes: dict[str, str] = {}
        for path in sorted((ROOT / "src" / "player").glob("*.c")):
            for code in self.RAISERS.findall(path.read_text(encoding="utf-8")):
                codes.setdefault(code, path.name)
        self.assertGreater(len(codes), 20, codes)
        missing = sorted(f"{code} ({path})" for code, path in codes.items()
                         if code not in exact and not any(code.startswith(p) for p in prefixes))
        self.assertEqual(missing, [], "codes raised as cards with no badge in player_error_badge")


class LegacyRendererSourceTests(unittest.TestCase):
    """The old renderer is gone (#2071): nk_ui is the launcher, with no switch back."""

    def test_shipped_player_has_no_legacy_switch(self) -> None:
        main = (ROOT / "src" / "player" / "main.c").read_text(encoding="utf-8")
        self.assertNotIn('getenv("NK_UI")', main)
        # --ui= is named once, in the notice that the switch is gone: it selects nothing.
        self.assertEqual(main.count('"--ui="'), 1)
        self.assertNotIn("--ui=legacy", main)
        self.assertIn("launcher is the only launcher.", main)
        self.assertFalse((ROOT / "src" / "player" / "ui_renderer.c").exists())
        self.assertFalse((ROOT / "src" / "player" / "ui_renderer.h").exists())
        for source in sorted((ROOT / "src" / "player").rglob("*.[ch]")):
            text = source.read_text(encoding="utf-8")
            self.assertNotIn('#include "ui_renderer.h"', text, source.name)
            self.assertNotIn("ui_render_frame(", text, source.name)


UPDATE_RELEASES_URL = "https://api.github.com/repos/Jstar269/nakagawa-recomp/releases?per_page=10"
UPDATE_DOWNLOAD_PREFIX = "https://github.com/Jstar269/nakagawa-recomp/releases/download/"
# The most one update step (check, download, install) may take in a WAIT_UPDATE step. A
# ceiling, not a sleep: the script moves on as soon as the phase is reached.
UPDATE_STEP_TIMEOUT_MS = 60000


def _release_newer_than_player() -> str:
    """One minor step past the player's own NK_PLAYER_VERSION, so the fixture release stays newer.

    A fixed number goes stale at the next version bump: the player then rightly calls the
    fixture's release current, and every update test reads "current" instead of "available".
    """
    header = (ROOT / "src" / "player" / "ui" / "player_ui_nk.h").read_text(encoding="utf-8")
    found = re.search(r'#define\s+NK_PLAYER_VERSION\s+"(\d+)\.(\d+)\.(\d+)', header)
    if found is None:
        raise AssertionError("src/player/ui/player_ui_nk.h no longer defines NK_PLAYER_VERSION as x.y.z")
    return f"{int(found[1])}.{int(found[2]) + 1}.0"


# The release the update fixture serves as the newest one.
UPDATE_NEWER_VERSION = _release_newer_than_player()

# A TEST-ONLY minisign signer for the synthetic releases: Ed25519 (RFC 8032) in plain
# Python, so no package is needed. Its key pair is derived from a fixed label at run
# time, so no key is stored anywhere. It is not the update-signing key: the player
# accepts it only in the regression build, from NK_UI_TEST_UPDATE_DIR's
# update_signing_key.pub, which these tests write. Output of this signer was checked
# with `minisign -V` (minisign 0.12) when it was written.
_ED_P = 2**255 - 19
_ED_L = 2**252 + 27742317777372353535851937790883648493
_ED_D = -121665 * pow(121666, _ED_P - 2, _ED_P) % _ED_P
TEST_ONLY_SIGNER_LABEL = b"Nakagawa Recomp TEST-ONLY update signing key; never for releases"


def _ed_add(a: tuple[int, int, int, int], b: tuple[int, int, int, int]) -> tuple[int, int, int, int]:
    p = _ED_P
    x1, y1, z1, t1 = a
    x2, y2, z2, t2 = b
    aa = (y1 - x1) * (y2 - x2) % p
    bb = (y1 + x1) * (y2 + x2) % p
    cc = 2 * t1 * t2 * _ED_D % p
    dd = 2 * z1 * z2 % p
    e, f, g, h = bb - aa, dd - cc, dd + cc, bb + aa
    return (e * f % p, g * h % p, f * g % p, e * h % p)


def _ed_mul(scalar: int, point: tuple[int, int, int, int]) -> tuple[int, int, int, int]:
    result = (0, 1, 1, 0)
    while scalar:
        if scalar & 1:
            result = _ed_add(result, point)
        point = _ed_add(point, point)
        scalar >>= 1
    return result


def _ed_encode(point: tuple[int, int, int, int]) -> bytes:
    x, y, z, _ = point
    inverse = pow(z, _ED_P - 2, _ED_P)
    x, y = x * inverse % _ED_P, y * inverse % _ED_P
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


def _ed_base() -> tuple[int, int, int, int]:
    y = 4 * pow(5, _ED_P - 2, _ED_P) % _ED_P
    xx = (y * y - 1) * pow(_ED_D * y * y + 1, _ED_P - 2, _ED_P) % _ED_P
    x = pow(xx, (_ED_P + 3) // 8, _ED_P)
    if (x * x - xx) % _ED_P:
        x = x * pow(2, (_ED_P - 1) // 4, _ED_P) % _ED_P
    if x & 1:
        x = _ED_P - x
    return (x, y, 1, x * y % _ED_P)


class TestOnlyMinisigner:
    """minisign's formats (src/core/nk_sig.h) over a key derived from label."""

    def __init__(self, label: bytes = TEST_ONLY_SIGNER_LABEL) -> None:
        seed = hashlib.blake2b(label, digest_size=32).digest()
        # minisign calls these 8 bytes the key ID; they are public.
        self.signer_id = hashlib.blake2b(label, digest_size=64).digest()[:8]
        digest = hashlib.sha512(seed).digest()
        scalar = int.from_bytes(digest[:32], "little")
        self._scalar = (scalar & ((1 << 254) - 8)) | (1 << 254)
        self._prefix = digest[32:]
        self._base = _ed_base()
        self.public = _ed_encode(_ed_mul(self._scalar, self._base))

    def _sign(self, message: bytes) -> bytes:
        r = int.from_bytes(hashlib.sha512(self._prefix + message).digest(), "little") % _ED_L
        big_r = _ed_encode(_ed_mul(r, self._base))
        k = int.from_bytes(hashlib.sha512(big_r + self.public + message).digest(),
                           "little") % _ED_L
        return big_r + ((r + k * self._scalar) % _ED_L).to_bytes(32, "little")

    def public_file(self) -> bytes:
        import base64
        number = int.from_bytes(self.signer_id, "little")
        line = base64.b64encode(b"Ed" + self.signer_id + self.public).decode("ascii")
        return f"untrusted comment: minisign public key {number:016X}\n{line}\n".encode("ascii")

    def minisig(self, data: bytes, comment_text: str) -> bytes:
        """What `minisign -S -t comment_text` writes: the prehashed (ED) form."""
        import base64
        signature = self._sign(hashlib.blake2b(data, digest_size=64).digest())
        comment = comment_text.encode("utf-8")
        global_signature = self._sign(signature + comment)
        line = base64.b64encode(b"ED" + self.signer_id + signature).decode("ascii")
        return (f"untrusted comment: signature from minisign secret key\n{line}\n"
                f"trusted comment: {comment_text}\n"
                f"{base64.b64encode(global_signature).decode('ascii')}\n").encode("utf-8")


def write_update_fixture(
    folder: Path, version: str = UPDATE_NEWER_VERSION, *, wrong_checksum: bool = False,
    missing_file: bool = False, signed: bool = True, bad_signature: bool = False,
    player_key: bool = True,
) -> None:
    """Synthetic GitHub answers for Check for updates, as the regression build's
    transport reads them from NK_UI_TEST_UPDATE_DIR: releases.json for
    the Releases API, then each asset by its file name. The ZIP has the release
    ZIP's layout (bin/nakagawa_player.exe and SHA256SUMS.txt). The checksum file
    is signed with the TEST-ONLY signer (unless signed is False; bad_signature
    signs other bytes), and update_signing_key.pub gives the regression player
    that signer's public key (unless player_key is False: a build with no key)."""
    import io
    import zipfile

    name = f"nakagawa-recomp-{version}-windows-x64.zip"
    files = {"bin/nakagawa_player.exe": b"synthetic player\n", "START HERE.txt": b"synthetic\n"}
    listed = dict(files)
    if missing_file:
        listed["bin/SDL3.dll"] = b"listed but left out of the ZIP\n"
    sums = "".join(f"{hashlib.sha256(data).hexdigest()}  {path}\n" for path, data in listed.items())
    archive = io.BytesIO()
    with zipfile.ZipFile(archive, "w") as package:
        for path, data in files.items():
            package.writestr(path, data)
        package.writestr("SHA256SUMS.txt", sums)
    data = archive.getvalue()
    (folder / name).write_bytes(data)
    digest = "0" * 64 if wrong_checksum else hashlib.sha256(data).hexdigest()
    checksum = f"{digest}  {name}".encode("ascii") + b"\r\n"
    (folder / f"{name}.sha256").write_bytes(checksum)
    signer = TestOnlyMinisigner()
    if player_key:
        (folder / "update_signing_key.pub").write_bytes(signer.public_file())
    signature = signer.minisig(checksum + (b"tampered" if bad_signature else b""),
                               f"nakagawa-recomp {version} windows-x64 zip checksum")
    if signed:
        (folder / f"{name}.sha256.minisig").write_bytes(signature)
    tag = f"v{version}"
    release = {
        "tag_name": tag,
        "html_url": f"https://github.com/Jstar269/nakagawa-recomp/releases/tag/{tag}",
        "draft": False,
        "prerelease": True,
        "published_at": "2026-11-04T12:00:00Z",
        "body": "Second preview. Faster builds.",
        "assets": [
            {"name": name, "size": len(data), "state": "uploaded",
             "browser_download_url": f"{UPDATE_DOWNLOAD_PREFIX}{tag}/{name}"},
            {"name": f"{name}.sha256", "size": 105, "state": "uploaded",
             "browser_download_url": f"{UPDATE_DOWNLOAD_PREFIX}{tag}/{name}.sha256"},
        ],
    }
    if signed:
        release["assets"].append(  # type: ignore[union-attr]
            {"name": f"{name}.sha256.minisig", "size": len(signature), "state": "uploaded",
             "browser_download_url": f"{UPDATE_DOWNLOAD_PREFIX}{tag}/{name}.sha256.minisig"})
    (folder / "releases.json").write_text(json.dumps([release]), encoding="utf-8")


def update_report(stdout: str) -> tuple[list[dict[str, str]], list[str]]:
    """The update steps the player settled on, and the URLs it asked for. A step
    in progress (checking, downloading, installing) and the quiet return to idle
    after a failed check at start are seen only when a frame falls inside them,
    so they are left out."""
    steps = []
    requests = []
    for line in stdout.splitlines():
        if line.startswith("[PLAYER_UI_TEST] update="):
            step = dict(re.findall(r"([a-z_]+)=(\S+)", line))
            if step["update"] not in ("checking", "downloading", "installing", "idle"):
                steps.append(step)
        elif line.startswith("[PLAYER_UI_TEST] update_request="):
            requests.append(line.split("=", 1)[1])
    return steps, requests


class NativePlayerUiTests(unittest.TestCase):
    def run_player(
        self,
        view: str,
        events: tuple[str, ...] = (),
        *,
        error_code: str | None = None,
        runtime_ready: bool = False,
        invalid_profile: bool = False,
        drop_invalid_iso: bool = False,
        drop_pbp_iso: bool = False,
        drop_bytes: tuple[str, bytes] | None = None,
        art_iso: str | None = None,
        stale_package: bool = False,
        incompatible_package: bool = False,
        wait_background: bool = False,
        catalog_reload_count: int = 0,
        build_ready_test: bool = False,
        experimental_build_ready: bool = False,
        post_build_ready: bool = False,
        downloaded_tool_payload: bool = False,
        data_root_present: bool = True,
        profile_refusal: bool = False,
        build_preflight: str | None = None,
        legacy_data: bool = False,
        env_extra: dict[str, str] | None = None,
        width: int = 1280,
        height: int = 720,
        timeout: float | None = None,
        library_titles: int = 0,
        dedicated_data_root: bool = False,
        settings: dict[str, object] | None = None,
        package_built_by: str | None = None,
        disc_list: str | None = None,
        enhancement_packages: dict[str, str] | None = None,
        enhancement_package_fields: dict[str, object] | None = None,
        expect_failure: bool = False,
    ) -> dict[str, object]:
        with tempfile.TemporaryDirectory(prefix=".player-ui-test-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            appdata = scratch / "appdata"
            localappdata = scratch / "localappdata"
            userprofile = scratch / "profile"
            for directory in (appdata, localappdata, userprofile):
                directory.mkdir()
            legacy_root = userprofile / "Nakagawa" / "data"
            if legacy_data:
                legacy_root.mkdir(parents=True)
            # The player's data folder (LOCALAPPDATA on Windows, XDG_DATA_HOME elsewhere), with
            # a disc list the user imported (nk_disc_verify.h).
            data_folder = (localappdata / "Nakagawa" / "data" if os.name == "nt"
                           else userprofile / "data" / "nakagawa-recomp")
            if disc_list is not None:
                (data_folder / "verification").mkdir(parents=True)
                (data_folder / "verification" / "redump.dat").write_text(disc_list,
                                                                        encoding="utf-8")
            profile = scratch / "controller-profile.json"
            if invalid_profile:
                profile.write_text("{ definitely not a valid controller profile", encoding="utf-8")
            runtime_root = scratch / "runtime"
            runtime_root.mkdir()
            for disc, title_id in (enhancement_packages or {}).items():
                # A title's own enhancement package (docs/ENHANCEMENT_CONTRACT.md section 9.4):
                # the player reads its id, target title and experimental flag, so Ace is offered for it.
                enhancements = runtime_root / "enhancements"
                enhancements.mkdir(exist_ok=True)
                (enhancements / f"{disc.lower()}-60hz.json").write_text(
                    json.dumps({"id": "fixture-60hz", "target_title_id": title_id,
                                **(enhancement_package_fields or {})}),
                    encoding="utf-8")
            tool_marker: Path | None = None
            if downloaded_tool_payload:
                tool_marker = runtime_root / "prerequisites" / "synthetic-tool" / "marker.txt"
                tool_marker.parent.mkdir(parents=True)
                tool_marker.write_text("synthetic downloaded tool", encoding="utf-8")
            if profile_refusal:
                manifest_dir = runtime_root / "manifests"
                manifest_dir.mkdir()
                (manifest_dir / "retired-binding.json").write_text(
                    json.dumps({
                        "schema_version": 1,
                        "id": "retired-profile-fixture-v1",
                        "display_name": "Retired Profile Fixture",
                        "kind": "retail",
                        "disc": {
                            "id": "TEST00001",
                            "region": "NA",
                            "revision_policy": "exact-disc-id",
                        },
                        "executable": {
                            "base": "0x08800000",
                            "entry": "0x08804000",
                            "bss_metadata_source": "elf",
                            "extra_executable_spans": [],
                        },
                        "modules": [],
                        "filesystem": {
                            "data_root": "fixtures/profile_zero",
                            "memory_stick_root": "savedata",
                            "device_prefixes": ["disc0:", "ms0:"],
                        },
                        "hle_profile": "generic",
                        "feature_requirements": [],
                        "verification_profile": "unverified",
                        "runtime_bindings": {
                            "schema_version": 1,
                            "vblank_frame_counter_addr": "0x08804000",
                        },
                    }, separators=(",", ":")) + "\n",
                    encoding="utf-8",
                )
            ready_source_iso: Path | None = None
            if build_ready_test:
                source_executable = b"synthetic input executable\n"
                ready_source_iso = scratch / "ready-source.iso"
                synthetic_art_iso(
                    ready_source_iso, source_executable=source_executable
                )
                synthetic_ready_package(
                    runtime_root, source_executable,
                    data_root_present=data_root_present,
                )
            elif experimental_build_ready:
                source_executable = b"synthetic input executable\n"
                ready_source_iso = scratch / "ready-source.iso"
                synthetic_art_iso(
                    ready_source_iso, source_executable=source_executable
                )
                synthetic_experimental_profile(runtime_root, "TEST00009", source_executable)
                synthetic_ready_package(
                    runtime_root, source_executable,
                    disc_id="TEST00009", title_id="experimental-test00009",
                )
            elif post_build_ready or incompatible_package:
                source_executable = (
                    build_plain_mips_elf() if incompatible_package
                    else b"synthetic input executable\n"
                )
                ready_source_iso = scratch / "ready-source.iso"
                if incompatible_package:
                    create_test_iso_with_executables(
                        ready_source_iso, source_executable, disc_id="TEST00006",
                        title="Synthetic Incompatible Package",
                    )
                    synthetic_ready_package(
                        runtime_root, source_executable,
                        disc_id="TEST00006", title_id="experimental-test00006",
                        data_root_present=data_root_present,
                    )
                else:
                    synthetic_art_iso(
                        ready_source_iso, source_executable=source_executable
                    )
                    synthetic_ready_package(
                        runtime_root, source_executable,
                        data_root_present=data_root_present,
                        built_by=package_built_by,
                    )
            catalog_overlay: Path | None = None
            if catalog_reload_count:
                catalog_overlay = scratch / "catalog-overlay.json"
                catalog_overlay.write_text(
                    json.dumps(
                        {
                            "schema_version": 1,
                            "id": "synthetic-ui-catalog-reload-v1",
                            "display_name": "Synthetic UI catalog reload fixture",
                            "kind": "retail",
                            "disc": {
                                "id": "TEST00999",
                                "region": "NA",
                                "revision_policy": "exact-disc-id",
                            },
                            "executable": {
                                "base": "0x08800000",
                                "entry": "0x08800000",
                                "bss_metadata_source": "elf",
                                "extra_executable_spans": [],
                            },
                            "modules": [],
                            "filesystem": {
                                "data_root": "data",
                                "memory_stick_root": "ms",
                                "device_prefixes": ["host0:"],
                            },
                            "hle_profile": "standard",
                            "feature_requirements": ["allegrex"],
                            "verification_profile": "smoke",
                        },
                        separators=(",", ":"),
                    ),
                    encoding="utf-8",
                )
            ui_iso_path: Path | None = None
            if build_preflight is not None:
                if build_preflight == "encrypted":
                    executable = build_psp_container()
                elif build_preflight == "missing-manifest":
                    executable = build_plain_mips_elf()
                else:
                    raise ValueError(f"unknown build preflight fixture: {build_preflight}")
                ui_iso_path = scratch / "build-preflight.iso"
                create_test_iso_with_executables(
                    ui_iso_path, executable, disc_id="TEST00009",
                    title="Synthetic Build Preflight",
                )
                if build_preflight == "encrypted":
                    synthetic_experimental_profile(runtime_root, "TEST00009", None)
            if art_iso is not None:
                if art_iso not in ("available", "unavailable", "icon-only"):
                    raise ValueError(f"unknown synthetic ISO mode: {art_iso}")
                ui_iso_path = scratch / "source-owned-synthetic.iso"
                synthetic_art_iso(ui_iso_path, include_picture=art_iso != "icon-only")
                if art_iso == "unavailable":
                    ui_iso_path.rename(scratch / "source-owned-synthetic.iso.offline")
            if ready_source_iso is not None:
                ui_iso_path = ready_source_iso
            if library_titles:
                # Where this run's player reads its library (LOCALAPPDATA on Windows,
                # XDG_DATA_HOME elsewhere; both point into the scratch folder below).
                write_large_library(
                    scratch,
                    localappdata / "Nakagawa" / "data" if os.name == "nt"
                    else userprofile / "data" / "nakagawa-recomp",
                    library_titles,
                )
            if settings is not None:
                # A settings.json saved before this run, where the player reads it.
                config = (localappdata / "Nakagawa" / "config" if os.name == "nt"
                          else userprofile / "config" / "nakagawa-recomp")
                config.mkdir(parents=True, exist_ok=True)
                (config / "settings.json").write_text(
                    json.dumps({"schema_version": 2, **settings}), encoding="utf-8")
            if stale_package:
                synthetic_stale_package(runtime_root)
            if incompatible_package:
                break_package_manifest(runtime_root)

            env = os.environ.copy()
            env.update(
                {
                    "SDL_VIDEODRIVER": "dummy",
                    "SDL_RENDER_DRIVER": "software",
                    "APPDATA": str(appdata),
                    "LOCALAPPDATA": str(localappdata),
                    "USERPROFILE": str(userprofile),
                    "HOME": str(userprofile),
                    "XDG_CONFIG_HOME": str(userprofile / "config"),
                    "XDG_DATA_HOME": str(userprofile / "data"),
                    "XDG_CACHE_HOME": str(userprofile / "cache"),
                    "NK_INPUT_PROFILE": str(profile),
                }
            )
            if env_extra:
                env.update(env_extra)
            if incompatible_package:
                env["NK_UI_TEST_BIND_EBOOT"] = "1"
            if build_ready_test:
                view = "build-ready"
            elif experimental_build_ready:
                view = "build-ready-experimental"
            args = [str(PLAYER_EXE), f"--view={view}"]
            event_script = list(events)
            if drop_invalid_iso:
                invalid_iso = scratch / "source-owned-invalid.iso"
                invalid_iso.write_bytes(b"synthetic invalid PSP disc image\n")
                event_script.insert(0, f"DROP_FILE={invalid_iso}")
            elif drop_pbp_iso:
                package_iso = scratch / "source-owned-store-package.iso"
                synthetic_pbp_package(package_iso)
                event_script.insert(0, f"DROP_FILE={package_iso}")
            elif drop_bytes is not None:
                dropped = scratch / drop_bytes[0]
                dropped.parent.mkdir(parents=True, exist_ok=True)
                dropped.write_bytes(drop_bytes[1])
                event_script.insert(0, f"DROP_FILE={dropped}")
            args.append("--ui-test-events=" + ";".join(event_script))
            screenshot = scratch / "last-frame.bmp"
            args.append(f"--ui-test-screenshot={screenshot}")
            if ui_iso_path is not None:
                args.append(f"--ui-test-iso-path={ui_iso_path}")
            if wait_background:
                args.append("--ui-test-wait-background")
            if catalog_overlay is not None:
                args.extend(
                    (
                        f"--ui-test-catalog-reload-path={catalog_overlay}",
                        f"--ui-test-catalog-reload-count={catalog_reload_count}",
                    )
                )
            args.append(f"--width={width}")
            args.append(f"--height={height}")
            if error_code:
                args.append(f"--ui-test-error-code={error_code}")

            if runtime_ready:
                package = runtime_root / "build" / "display-smoke-v1"
                package.mkdir(parents=True)
                # The launcher resolves a developer package by path existence;
                # these empty files are synthetic sentinels, never executed.
                (package / "display-smoke-v1.exe").write_bytes(b"")
                (package / "display-smoke-v1").write_bytes(b"")
                (package / "display-smoke-v1_image.bin").write_bytes(b"")
                (runtime_root / "fixtures" / "display_smoke").mkdir(parents=True, exist_ok=True)
            args.append(f"--runtime-root={runtime_root}")
            dedicated = scratch / "dedicated-data-root"
            if dedicated_data_root:
                args.append(f"--user-data-root={dedicated}")

            effective_timeout = compute_effective_timeout(event_script, timeout)

            started = time.perf_counter()
            completed = subprocess.run(
                args,
                cwd=ROOT,
                env=env,
                capture_output=True,
                text=True,
                timeout=effective_timeout,
                check=False,
            )
            wall_seconds = time.perf_counter() - started
            if os.environ.get(PLAYER_UI_ECHO_ENV) == "1":
                sys.stderr.write(completed.stdout)
            if expect_failure:
                # A script the harness must stop: only the exit code and output count.
                self.assertNotEqual(completed.returncode, 0,
                                    completed.stdout + completed.stderr)
                return {"stdout": completed.stdout, "stderr": completed.stderr,
                        "returncode": completed.returncode}
            if completed.returncode != 0:
                self.fail(
                    f"player returned {completed.returncode}\n"
                    f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
                )
            frames = parse_frames(completed.stdout)
            screenshot_line = next(
                (line for line in completed.stdout.splitlines() if line.startswith("[PLAYER_UI_TEST] screenshot=")),
                "",
            )
            self.assertIn("screenshot=PASS", screenshot_line, completed.stdout)
            self.assertGreaterEqual(len(frames), 2, completed.stdout)
            self.assertTrue(all(int(frame["pixels"], 16) != 0 for frame in frames))
            self.assertTrue(screenshot.is_file())
            bmp = read_bmp(screenshot)
            self.assertEqual((bmp[0], bmp[1]), (width, height))
            persisted_settings = None
            for settings_path in scratch.rglob("settings.json"):
                try:
                    candidate = json.loads(settings_path.read_text(encoding="utf-8"))
                except (OSError, json.JSONDecodeError):
                    continue
                if isinstance(candidate, dict) and "fps_cap" in candidate:
                    persisted_settings = candidate
                    break

            library_file = data_folder / "library.json"
            persisted_library = (json.loads(library_file.read_text(encoding="utf-8"))
                                 if library_file.is_file() else None)

            def files_under(folder: Path) -> list[str]:
                return sorted(path.relative_to(folder).as_posix()
                              for path in folder.rglob("*") if path.is_file())
            return {
                "frames": frames,
                "bmp": bmp,
                "profile_exists": profile.is_file(),
                "profile_text": profile.read_text(encoding="utf-8") if profile.is_file() else "",
                "stdout": completed.stdout,
                "wall_seconds": wall_seconds,
                "stderr": completed.stderr,
                "legacy_root": legacy_root,
                "legacy_root_exists": legacy_root.is_dir(),
                "settings": persisted_settings,
                "library": persisted_library,
                "default_profile_files": files_under(localappdata / "Nakagawa"),
                "dedicated_files": files_under(dedicated),
                "downloaded_tool_marker_exists": (
                    tool_marker.is_file() if tool_marker is not None else None
                ),
            }

    def test_compute_effective_timeout(self) -> None:
        """Effective timeout scales with scripted wait events and respects explicit bounds."""
        self.assertEqual(compute_effective_timeout([], explicit_timeout=12.5), 12.5)
        self.assertEqual(
            compute_effective_timeout([]),
            DEFAULT_BASE_TIMEOUT_SECONDS + TIMEOUT_BUFFER_SECONDS,
        )
        events = [
            "WAIT_MS=2500",
            "WAIT_VIEW=ready_library,10000",
            "WAIT_PACKAGE_STATUS=INCOMPATIBLE,3000",
            "WAIT_VIEW=error,5000",
            "QUIT",
        ]
        expected = DEFAULT_BASE_TIMEOUT_SECONDS + 2.5 + 10.0 + 3.0 + 5.0 + TIMEOUT_BUFFER_SECONDS
        self.assertEqual(compute_effective_timeout(events), expected)

    @unittest.skipUnless(sys.platform == "win32", "legacy %USERPROFILE%\\Nakagawa\\data migration warning is Windows-only")
    def test_legacy_data_root_is_reported_once_without_moving_it(self) -> None:
        run = self.run_player("empty", legacy_data=True)
        stderr = run["stderr"]
        legacy_root = run["legacy_root"]
        assert isinstance(stderr, str)
        assert isinstance(legacy_root, Path)
        expected_old_path = str(legacy_root.parent.parent) + "\\Nakagawa\\data"
        marker = f'LEGACY_DATA_DIR_FOUND: old_path="{expected_old_path}"'
        self.assertEqual(stderr.count(marker), 1, stderr)
        self.assertTrue(run["legacy_root_exists"])

    @unittest.skipUnless(sys.platform == "win32", "player-output.txt is the Windows GUI player's console stand-in")
    def test_player_output_file_holds_the_output_of_a_console_less_launch(self) -> None:
        """A double-click start has no console and no standard handles, so the player writes
        its output to logs/player-output.txt. Started like that (no console to attach to, null
        standard handles), the file must hold what the player printed."""
        with tempfile.TemporaryDirectory(prefix=".player-ui-test-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            env = os.environ.copy()
            for name in ("APPDATA", "LOCALAPPDATA", "USERPROFILE"):
                folder = scratch / name.lower()
                folder.mkdir()
                env[name] = str(folder)
            env["SDL_VIDEODRIVER"] = "dummy"
            env["SDL_AUDIO_DRIVER"] = "dummy"
            # The launcher runs detached, so there is no parent console the player could
            # attach to, and it starts the player detached with null standard handles: what
            # Explorer gives the GUI-subsystem player (the test build is console-subsystem).
            completed = subprocess.run(
                [sys.executable, "-I", "-c", CONSOLE_LESS_LAUNCHER, str(PLAYER_EXE), "--help"],
                cwd=scratch,
                env=env,
                creationflags=subprocess.DETACHED_PROCESS,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=90,
                check=False,
            )
            self.assertEqual(completed.returncode, 0)
            outputs = list(scratch.rglob("player-output.txt"))
            self.assertEqual(len(outputs), 1, outputs)
            text = outputs[0].read_text(encoding="utf-8", errors="replace")
            self.assertIn("Usage: nakagawa_player", text)

    def test_empty_library_wizard_picker_and_font_confirmation(self) -> None:
        run = self.run_player("empty", ("KEY_RETURN", "KEY_RETURN", "KEY_RETURN", "KEY_RETURN"))
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["view"], "library")
        self.assertEqual(frames[1]["view"], "wizard")
        self.assertEqual(frames[1]["wizard_step"], "welcome")
        self.assertEqual(frames[2]["wizard_step"], "online_options")
        self.assertEqual(frames[3]["wizard_step"], "select_game")
        self.assertEqual(frames[3]["online_mode"], "0", "CONTINUE moves on with both off")
        self.assertEqual(frames[4]["picker"], "1")
        self.assertNotEqual(frames[0]["pixels"], frames[1]["pixels"])

        fonts = self.run_player("wizard4", ("KEY_RETURN",))
        font_frames = fonts["frames"]
        assert isinstance(font_frames, list)
        self.assertEqual(font_frames[1]["wizard_step"], "ready_launch")
        self.assertEqual(font_frames[1]["font_confirmed"], "1")

    def test_developer_log_panel_needs_developer_mode(self) -> None:
        panel = self.run_player("developer-log", ("KEY_F12",))
        frames = panel["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["view"], "developer_log")
        self.assertNotEqual(frames[0]["dev_log_lines"], "0")
        self.assertEqual(frames[1]["view"], "library", "F12 closes the panel")
        closed = self.run_player("developer-log", ("KEY_ESCAPE",))
        frames = closed["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[1]["view"], "library", "Esc closes the panel")
        off = self.run_player("library", ("KEY_F12",))
        frames = off["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["view"], "library",
                         "without developer mode F12 does nothing")

    def test_online_settings_open_from_settings_and_back(self) -> None:
        run = self.run_player("online-settings", ("KEY_ESCAPE",))
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["view"], "online_settings")
        self.assertEqual(frames[1]["view"], "settings")

    def test_build_cache_page_leads_to_tool_removal_and_back(self) -> None:
        """#1306: Settings > Build tools and cache holds the shared-library switch, the
        clear button and tool removal; cancelling a removal returns to the page."""
        # Build speed sits between the switch and the clear button, one more Tab before
        # Remove build tools.
        run = self.run_player(
            "build-cache",
            ("KEY_TAB", "KEY_TAB", "KEY_TAB", "KEY_RETURN", "KEY_ESCAPE", "KEY_ESCAPE"),
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        views = [frame["view"] for frame in frames]
        visited = [view for i, view in enumerate(views) if i == 0 or views[i - 1] != view]
        self.assertEqual(
            visited, ["build_cache", "confirm_remove_tools", "build_cache", "settings"], views
        )

    def test_next_ui_library_card_shows_the_disc_icon_once_read(self) -> None:
        """The next UI reads ICON0.PNG from the disc in the background and shows
        it on the card; a disc that cannot be read keeps the placeholder."""
        shown = self.run_player("ready", ("WAIT_MS=1500",), art_iso="available")
        missing = self.run_player("ready", ("WAIT_MS=1500",), art_iso="unavailable")
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in shown["stdout"]:
            self.skipTest("the next UI could not start here (no UI fonts)")
        self.assertIn("[PLAYER_UI_TEST] next_ui_disc_icon index=0 width=1 height=1", shown["stdout"],
                      "the disc's 1 x 1 ICON0.PNG is decoded and shown on its card")
        self.assertNotIn("next_ui_disc_icon", missing["stdout"],
                         "an unreadable disc keeps the placeholder")

    @staticmethod
    def backdrops(run: dict[str, object]) -> list[tuple[int, str, str]]:
        """Each frame's (ticks, backdrop game, shown) on the library."""
        frames = run["frames"]
        assert isinstance(frames, list)
        return [(int(frame["ticks_ms"]), frame["backdrop"], frame["backdrop_shown"])
                for frame in frames if frame["view"] in ("library", "ready_library")]

    def test_next_ui_art_rest_shows_the_rested_cards_picture_behind_the_library(self) -> None:
        """The 0.6 s art rest: focus resting on a game's card for 0.6 s brings its
        PIC1 in behind the library; focus that moves on sooner reads no picture, and
        moving on fades the picture out. Reduce motion leaves it out altogether."""
        motion = {"reduce_motion_choice": "off"}
        # Scripted runs cut motion unless NK_UI_TEST_MOTION keeps the settings' choice.
        keep = {"NK_UI_TEST_MOTION": "1"}
        # The waits only bound how long the picture may take to be read on a busy machine.
        rest = self.run_player("ready", ("WAIT_MS=4000",), art_iso="available",
                               settings=motion, env_extra=keep, wait_background=True)
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in str(rest["stdout"]):
            self.skipTest("the next UI could not start here (no UI fonts)")
        frames = self.backdrops(rest)
        start = frames[0][0]
        self.assertTrue(all(game == "-1" for ticks, game, _ in frames if ticks - start < 550),
                        f"nothing behind the library before the rest: {frames[:6]}")
        self.assertIn(("0", "1"), [(game, shown) for _, game, shown in frames],
                      "after resting on the first card, its picture is behind the library")

        browse = self.run_player("ready", ("WAIT_MS=300", "KEY_RIGHT", "WAIT_MS=1500"),
                                 art_iso="available", settings=motion,
                                 env_extra=keep)
        self.assertNotIn("0", [game for _, game, _ in self.backdrops(browse)],
                         "focus that moved on before the rest brings no picture in")

        leave = self.run_player("ready", ("WAIT_MS=4000", "KEY_RIGHT", "WAIT_MS=1500"),
                                art_iso="available", settings=motion,
                                env_extra=keep)
        states = [(game, shown) for _, game, shown in self.backdrops(leave)]
        if ("0", "1") in states:
            # Once focus moves on the picture is never shown again (it fades out, which
            # test_backdrop_rests_and_fades times; a busy machine may draw no frame of
            # the fade), and it is gone at the end.
            last_shown = len(states) - 1 - states[::-1].index(("0", "1"))
            self.assertNotIn(("0", "1"), states[last_shown + 1:])
            self.assertEqual(states[-1], ("-1", "0"), "moving on takes the picture away")

        bare = self.run_player("ready", ("WAIT_MS=1500",), art_iso="icon-only",
                               settings=motion, env_extra=keep)
        bare_frames = self.backdrops(bare)
        self.assertEqual({game for _, game, _ in bare_frames}, {"-1"},
                         "a disc with no picture has nothing to show")
        self.assertLess(len(bare_frames), 100,
                        "after the rest on a disc with no picture the loop sleeps between frames")

        still = self.run_player("ready", ("WAIT_MS=1500",), art_iso="available",
                                settings={"reduce_motion_choice": "on"}, env_extra=keep)
        self.assertEqual({game for _, game, _ in self.backdrops(still)}, {"-1"},
                         "Reduce motion: no picture behind the library")

    def test_a_library_of_hundreds_of_titles_loads_pages_and_checks_what_it_shows(self) -> None:
        """A library of 300 titles: every one loads (the player held 64, and one
        more made the whole library load empty); the first card, not NEXT, has the
        first focus; while the first page shows, only its titles get a package
        check (a full check reads the disc, and every title was checked in turn);
        Page Down turns pages, focus keeps its place, and the page shown gets its
        disc icons (the art cache was filled by the first 64 titles)."""
        events = (("WAIT_MS=4000", "KEY_RIGHT", "WAIT_MS=300")
                  + ("KEY_PAGEDOWN", "WAIT_MS=100") * 9 + ("WAIT_MS=3000",))
        run = self.run_player("library", events, library_titles=300, timeout=180.0)
        stdout = run["stdout"]
        assert isinstance(stdout, str)
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in stdout:
            self.skipTest("the next UI could not start here (no UI fonts)")
        statuses = {int(i) for i in re.findall(r"next_ui_library_status index=(\d+)", stdout)}
        self.assertTrue(set(range(300)) <= statuses, "every saved title is in the launcher")

        first_page = stdout.split("[PLAYER_UI_TEST] wait_ms result=PASS", 1)[0]
        checked_first = {int(i) for i in re.findall(r"package_check index=(\d+)", first_page)}
        self.assertIn(0, checked_first, "the first page's titles are checked")
        self.assertLess(max(checked_first), 16,
                        "no title off the first page is checked while it shows")

        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertIn("1", {frame["selected"] for frame in frames},
                      "Right from the first card reaches the second card")
        selected = int(frames[-1]["selected"])
        self.assertGreaterEqual(selected, 64, "nine pages on, past the old 64-title limit")
        icons = {int(i) for i in re.findall(r"next_ui_disc_icon index=(\d+)", stdout)}
        self.assertIn(selected, icons, "the page turned to shows its disc icons")
        checked = {int(i) for i in re.findall(r"package_check index=(\d+)", stdout)}
        self.assertIn(selected, checked)
        self.assertNotIn(299, checked, "the last page was never shown, so never checked")

    def test_next_ui_backspace_and_the_mouse_back_button_go_back(self) -> None:
        """Backspace and the mouse's back button (X1) are Back, as Esc is: from
        Settings each returns to the library."""
        for events in (("WAIT_MS=100", "KEY_BACKSPACE", "WAIT_MS=100"),
                       ("WAIT_MS=100", "MOUSE_X1_DOWN", "MOUSE_X1_UP", "WAIT_MS=100")):
            with self.subTest(events=events[1]):
                run = self.next_ui_run("settings", events)
                self.assertEqual([view for view, _ in self.screens(run)],
                                 ["settings", "library"])

    def test_next_ui_home_and_end_reach_the_first_and_last_game(self) -> None:
        """Home and End in a library of several pages: End focuses the last game on
        the last page, Home the first game on the first."""
        run = self.run_player(
            "library",
            ("WAIT_MS=2000", "KEY_END", "WAIT_MS=200", "KEY_HOME", "WAIT_MS=200"),
            library_titles=40, timeout=120.0,
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in run["stdout"]:
            self.skipTest("the next UI could not start here (no UI fonts)")
        frames = run["frames"]
        assert isinstance(frames, list)
        selected = [frame["selected"] for frame in frames]
        self.assertIn("39", selected, "End reaches the last game")
        self.assertEqual(selected[-1], "0", "Home comes back to the first game")
        self.assertLess(selected.index("39"), len(selected) - 1)

    def test_next_ui_library_follows_the_saved_sort(self) -> None:
        """The library's grid follows the saved sort: Recently added puts the last
        game added first, where the first focus lands."""
        added = self.run_player("library", ("WAIT_MS=1000",), library_titles=12,
                                settings={"library_sort": "added"}, timeout=120.0)
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in added["stdout"]:
            self.skipTest("the next UI could not start here (no UI fonts)")
        frames = added["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["selected"], "11", "the last game added is first")
        played = self.run_player("library", ("WAIT_MS=1000",), library_titles=12,
                                 timeout=120.0)
        frames = played["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["selected"], "0",
                         "with nothing played yet, Recently played keeps the library's order")

    # The library's find box (player_ui_nk.h).
    FIND = str(0x1007)
    FIND_DONE = str(0x1008)
    FIND_KEY_Q = str(0x1080 + 10)

    def test_next_ui_find_from_the_keyboard(self) -> None:
        """'/' opens the find box on DONE; typing filters the library (D is typed,
        not Details); Enter moves to the first match and opens it."""
        run = self.run_player(
            "library",
            ("WAIT_MS=1000", "KEY_SLASH", "WAIT_MS=100", "TEXT=title 1", "KEY_D", "TEXT=1",
             "KEY_BACKSPACE", "WAIT_MS=100", "KEY_RETURN", "WAIT_MS=100", "KEY_RETURN",
             "WAIT_MS=200"),
            library_titles=12, timeout=120.0,
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in run["stdout"]:
            self.skipTest("the next UI could not start here (no UI fonts)")
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertIn(self.FIND_DONE, [frame["next_focus"] for frame in frames],
                      "'/' opens the box on DONE")
        # "title 1" + "1" then Backspace is "title 1": titles 1, 10 and 11 match, 1 first.
        self.assertEqual(frames[-1]["next_route"], "2", "Enter, Enter opens a match")
        self.assertEqual(frames[-1]["selected"], "1", "the first match is Synthetic title 1")

    def test_next_ui_find_with_a_controller_and_esc_clears_it(self) -> None:
        """The pad's west button opens the box on the on-screen keyboard's Q, and the
        east button (B) clears and closes it; Esc does too, without asking to quit."""
        pad = self.run_player(
            "library", ("WAIT_MS=1000", "PAD_WEST", "WAIT_MS=100", "PAD_EAST", "WAIT_MS=100"),
            library_titles=4, timeout=120.0,
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in pad["stdout"]:
            self.skipTest("the next UI could not start here (no UI fonts)")
        trail = self.focus_trail(pad)
        self.assertIn(self.FIND_KEY_Q, trail, "the pad opens the box on Q")
        self.assertEqual(trail[-1], self.FIND, "B closes it and focus returns to FIND")
        keyboard = self.run_player(
            "library", ("WAIT_MS=1000", "KEY_SLASH", "TEXT=zzz", "KEY_ESCAPE", "WAIT_MS=100"),
            library_titles=4, timeout=120.0,
        )
        self.assertNotIn("QUIT_DIALOG", keyboard["stdout"], "Esc closes the box, not the player")
        self.assertEqual(self.focus_trail(keyboard)[-1], self.FIND)

    @staticmethod
    def find_states(run: dict[str, object]) -> list[str]:
        """What the find box held while it was open, frame by frame, with repeats collapsed."""
        stdout = run["stdout"]
        assert isinstance(stdout, str)
        states: list[str] = []
        for text_hex in re.findall(
                r"^\[PLAYER_UI_TEST\] find_box frame=\d+ text_hex=([0-9a-f]*)\r?$", stdout, re.M):
            state = bytes.fromhex(text_hex).decode("utf-8")
            if not states or states[-1] != state:
                states.append(state)
        return states

    def test_next_ui_find_stops_at_its_cap(self) -> None:
        """#2206: what a hardware keyboard types into the find box goes through the shared
        keyboard's cap: 40 characters typed twice (the second time into a box that holds
        text), 63 bytes kept."""
        run = self.run_player(
            "library", ("WAIT_MS=1000", "KEY_SLASH", "TEXT=" + "x" * 40, "WAIT_MS=200",
                        "TEXT=" + "x" * 40, "WAIT_MS=200"),
            library_titles=4, timeout=120.0,
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in run["stdout"]:
            self.skipTest("the next UI could not start here (no UI fonts)")
        states = self.find_states(run)
        self.assertTrue(states, run["stdout"])
        self.assertEqual(states[-1], "x" * 63)

    def test_next_ui_find_types_with_a_controller_alone(self) -> None:
        """#2206: the find box's keys are the shared on-screen keyboard, so a controller
        alone searches: the d-pad moves, A types the focused key, X deletes, Start is Done
        (focus goes to the first match) and A opens it. Failing-before: the box had its own
        grid, X did nothing while it was open, and Start left the library for Settings."""
        run = self.run_player(
            "library",
            ("WAIT_MS=1000", "PAD_WEST", "WAIT_MS=100", "PAD_DPAD_UP", "PAD_SOUTH",
             "PAD_DPAD_RIGHT", "PAD_SOUTH", "WAIT_MS=100", "PAD_WEST", "WAIT_MS=100",
             "PAD_DPAD_LEFT", "PAD_SOUTH", "WAIT_MS=100", "PAD_START", "WAIT_MS=100",
             "PAD_SOUTH", "WAIT_MS=200"),
            library_titles=12, timeout=120.0,
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in run["stdout"]:
            self.skipTest("the next UI could not start here (no UI fonts)")
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertIn(self.FIND_KEY_Q, self.focus_trail(run), "the pad opens the box on q")
        # 1, 2, X deletes the 2, 1: "11" is in Synthetic title 11 and its disc ID alone.
        self.assertEqual((frames[-1]["next_route"], frames[-1]["selected"]), ("2", "11"),
                         "Start is Done, and A opens the one match")
        self.assertEqual(self.find_states(run), ["", "1", "12", "1", "11"],
                         "the keys typed 1 and 2, X deleted the 2, and 1 was typed again")

    @staticmethod
    def find_grid_trail(run: dict[str, object]) -> list[tuple[str | None, int, int, tuple[str, ...]]]:
        """The library grid for each thing the find box held, in order: (the box's text,
        or None while it is closed; the grid's games; the library's games; the disc IDs
        the page drew). Each is the grid on the last frame of that text, so a keystroke's
        own frame, which may still draw the grid of the text before, does not count."""
        stdout = run["stdout"]
        assert isinstance(stdout, str)
        finds = {int(frame): bytes.fromhex(text_hex).decode("utf-8") for frame, text_hex in
                 re.findall(r"^\[PLAYER_UI_TEST\] find_box frame=(\d+) text_hex=([0-9a-f]*)\r?$",
                            stdout, re.M)}
        grids = {int(frame): (int(matches), int(total), tuple(filter(None, shown.split(","))))
                 for frame, matches, total, shown in re.findall(
                     r"^\[PLAYER_UI_TEST\] library_grid frame=(\d+) matches=(\d+) total=(\d+)"
                     r" shown=(\S*)\r?$", stdout, re.M)}
        frames = run["frames"]
        assert isinstance(frames, list)
        trail: list[tuple[str | None, int, int, tuple[str, ...]]] = []
        grid: tuple[int, int, tuple[str, ...]] | None = None
        for frame in frames:
            number = int(frame["frame"])
            grid = grids.get(number, grid)
            if grid is None:
                continue
            text = finds.get(number)
            if trail and trail[-1][0] == text:
                trail[-1] = (text, *grid)
            else:
                trail.append((text, *grid))
        return trail

    def test_next_ui_find_filters_the_grid_with_a_controller_alone(self) -> None:
        """#2206: a controller alone searches the library by name, and the grid follows
        the box. The pad opens the box on q, the d-pad moves to e and A types it, Y types
        a space, the d-pad moves to 1 and A types it twice ("e 11"), X deletes back to an
        empty box, A types 1, and B clears and closes the box. Each step's grid is the
        games whose name or disc ID holds the text: "e 1" is in the names "Synthetic
        title 1", "10" and "11" and in no disc ID (SCAL000nn), so it shows the name
        search; the empty box and the cleared one give back the whole library."""
        run = self.run_player(
            "library",
            ("WAIT_MS=1000", "PAD_WEST", "WAIT_MS=100", "PAD_DPAD_RIGHT", "PAD_DPAD_RIGHT",
             "PAD_SOUTH", "WAIT_MS=100", "PAD_NORTH", "WAIT_MS=100", "PAD_DPAD_UP",
             "PAD_DPAD_LEFT", "PAD_DPAD_LEFT", "PAD_SOUTH", "WAIT_MS=100", "PAD_SOUTH",
             "WAIT_MS=100", "PAD_WEST", "WAIT_MS=100", "PAD_WEST", "WAIT_MS=100", "PAD_WEST",
             "WAIT_MS=100", "PAD_WEST", "WAIT_MS=100", "PAD_SOUTH", "WAIT_MS=100", "PAD_EAST",
             "WAIT_MS=200"),
            library_titles=12, timeout=120.0,
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in run["stdout"]:
            self.skipTest("the next UI could not start here (no UI fonts)")
        trail = self.find_grid_trail(run)
        self.assertEqual([text for text, *_ in trail],
                         [None, "", "e", "e ", "e 1", "e 11", "e 1", "e ", "e", "", "1", None],
                         run["stdout"][-4000:])
        full = {f"SCAL{index:05d}" for index in range(12)}
        ones = {"SCAL00001", "SCAL00010", "SCAL00011"}  # Synthetic title 1, 10 and 11
        for text, matches, total, shown in trail:
            self.assertEqual(total, 12, f"the library holds 12 games ({text!r})")
            self.assertTrue(set(shown) <= full, f"the page draws library games ({text!r})")
            if text in ("e 1", "1"):
                self.assertEqual((matches, set(shown)), (3, ones),
                                 f"{text!r} keeps titles 1, 10 and 11")
            elif text == "e 11":
                self.assertEqual((matches, shown), (1, ("SCAL00011",)), "'e 11' keeps title 11")
            else:
                self.assertEqual(matches, 12, f"{text!r} keeps every game")
                self.assertGreater(len(shown), len(ones), f"{text!r} draws a full page")
        self.assertEqual(trail[-1][3], trail[0][3],
                         "B clears the box, and the first page draws what it drew at the start")
        self.assertEqual(self.focus_trail(run)[-1], self.FIND, "B returns focus to FIND")

    def test_next_ui_is_the_only_launcher(self) -> None:
        """The owner's rule for v0.0.1: the new launcher or nothing.

        A player whose launcher cannot start (an extraction that lost
        assets/fonts/ui) says why and exits; it never draws the old renderer."""
        marker = "[PLAYER_UI_TEST] UI_NEXT_ACTIVE"
        default_ui = self.run_player("empty")
        if marker not in default_ui["stdout"]:
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        with tempfile.TemporaryDirectory(prefix=".player-ui-test-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            env = os.environ.copy()
            env.update({
                # The same headless drivers as run_player: a hosted Linux runner has no display,
                # and SDL must start for the player to reach its launcher check.
                "SDL_VIDEODRIVER": "dummy",
                "SDL_RENDER_DRIVER": "software",
                "SDL_AUDIO_DRIVER": "dummy",
                "LOCALAPPDATA": str(scratch),
                "APPDATA": str(scratch),
                "NK_UI_TEST_HIDE_UI_FONTS": "1",
            })
            completed = subprocess.run(
                [str(PLAYER_EXE), f"--user-data-root={scratch / 'data'}",
                 "--ui-test-events=WAIT_MS=100;QUIT",
                 f"--ui-test-screenshot={scratch / 'shot.bmp'}"],
                cwd=ROOT, env=env, capture_output=True, text=True, timeout=60, check=False,
            )
        self.assertEqual(completed.returncode, 4, completed.stderr)
        self.assertIn("LAUNCHER_UNAVAILABLE", completed.stderr)
        if sys.platform == "win32":
            self.assertIn("Extract the ZIP again", completed.stderr)
        else:
            # No release ZIP or nakagawa_player.exe off Windows (#306).
            self.assertNotIn("ZIP", completed.stderr)
            self.assertIn("tools/stage_install.py", completed.stderr)
        self.assertNotIn("compiled without", completed.stderr)
        # The folder is named the way this platform writes paths (#306).
        if sys.platform == "win32":
            self.assertIn("assets\\fonts\\ui", completed.stderr)
        else:
            self.assertIn("assets/fonts/ui", completed.stderr)
            self.assertNotIn("assets\\fonts", completed.stderr)
        self.assertNotIn(marker, completed.stdout)
        self.assertNotIn("frame=", completed.stdout, "nothing is drawn after the message")

    def test_removed_ui_switch_and_unknown_options_are_named_not_fatal(self) -> None:
        """Shortcuts made before the old renderer went still pass --ui=next. The
        switch is gone, so the player says it ignores it, and any other option it
        does not know, then starts the launcher as usual."""
        marker = "[PLAYER_UI_TEST] UI_NEXT_ACTIVE"
        default_ui = self.run_player("empty")
        if marker not in default_ui["stdout"]:
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        # The harness's own --ui-test-* options are taken first: none is unknown.
        self.assertNotIn("Ignoring", str(default_ui["stderr"]))
        with tempfile.TemporaryDirectory(prefix=".player-ui-test-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            env = os.environ.copy()
            env.update({
                "SDL_VIDEODRIVER": "dummy",
                "SDL_RENDER_DRIVER": "software",
                "SDL_AUDIO_DRIVER": "dummy",
                "LOCALAPPDATA": str(scratch),
                "APPDATA": str(scratch),
            })
            shot = scratch / "shot.bmp"
            completed = subprocess.run(
                [str(PLAYER_EXE), f"--user-data-root={scratch / 'data'}",
                 "--ui=legacy", "--no-such-option", "--view=library", f"--screenshot={shot}"],
                cwd=ROOT, env=env, capture_output=True, text=True, timeout=60, check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertTrue(shot.is_file(), completed.stdout + completed.stderr)
            self.assertGreater(shot.stat().st_size, 0)
        self.assertIn("Ignoring --ui=legacy", completed.stderr)
        self.assertIn("the nk launcher is the only launcher", completed.stderr)
        self.assertIn("Ignoring unknown option: --no-such-option", completed.stderr)
        self.assertNotIn("Ignoring unknown option: --ui=legacy", completed.stderr)
        self.assertNotIn("Ignoring unknown option: --view", completed.stderr)
        self.assertNotIn("Ignoring unknown option: --screenshot", completed.stderr)
        self.assertNotIn("Ignoring unknown option: --user-data-root", completed.stderr)

    def test_every_view_is_drawn_by_the_launcher(self) -> None:
        """Every PlayerView in player_state.h is drawn by the nk_ui launcher."""
        header = (ROOT / "src" / "player" / "player_state.h").read_text(encoding="utf-8")
        body = header[header.index("typedef enum {\n    VIEW_LIBRARY"):]
        body = body[:body.index("} PlayerView;")]
        body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
        declared = re.findall(r"\b((?:PLAYER_)?VIEW_[A-Z_]+)\b", body)
        run = self.run_player("library", ("WAIT_MS=50",),
                              env_extra={"NK_UI_TEST_REPORT_VIEWS": "1"})
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in run["stdout"]:
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        coverage = re.findall(r"\[PLAYER_UI_TEST\] view_coverage value=(\d+) name=(\S+) nk=(\d)",
                              str(run["stdout"]))
        self.assertEqual(len(coverage), len(declared), (coverage, declared))
        self.assertEqual([name for _, name, nk in coverage if nk != "1"], [],
                         "views the launcher does not draw")

    def test_settings_psp_fonts_imports_and_removes(self) -> None:
        """Walkthrough blocker: the PSP font import is reachable from Settings."""
        run = self.run_player("psp-fonts", ("WAIT_MS=100", "KEY_RIGHT", "WAIT_MS=100"))
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in run["stdout"]:
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["view"], "psp_fonts")
        trail = self.focus_trail(run)
        self.assertIn(str(0x452B), trail, "Get fonts from my PSP has focus first")
        self.assertIn(str(0x451E), trail, "Right reaches Choose font files")
        self.assertLess(trail.index(str(0x452B)), trail.index(str(0x451E)), trail)
        back = self.run_player("psp-fonts", ("WAIT_MS=100", "KEY_ESCAPE", "WAIT_MS=100"))
        self.assertEqual(self.screens(back)[-1][0], "settings", "Esc goes back to Settings")

    def test_settings_factory_reset_asks_then_moves_the_data_aside(self) -> None:
        """Owner 2026-10-29: Factory reset asks first, with Keep everything focused.

        Keep everything and Esc go back to Settings with nothing moved. Reset
        renames this run's scratch user data folders to "-backup-<stamp>" and
        quits; the test build skips starting the fresh copy and says so."""
        keep = self.run_player("factory-reset", ("WAIT_MS=100", "KEY_RETURN", "WAIT_MS=100"))
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in keep["stdout"]:
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        frames = keep["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["view"], "confirm_factory_reset")
        self.assertEqual(self.focus_trail(keep)[0], str(0x4517), "Keep everything has focus")
        self.assertEqual(self.screens(keep)[-1][0], "settings")
        self.assertNotIn("Factory reset: moved", keep["stdout"])
        escape = self.run_player("factory-reset", ("WAIT_MS=100", "KEY_ESCAPE", "WAIT_MS=100"))
        self.assertEqual(self.screens(escape)[-1][0], "settings", "Esc goes back to Settings")
        self.assertNotIn("Factory reset: moved", escape["stdout"])

        reset = self.run_player("factory-reset",
                                ("WAIT_MS=100", "KEY_RIGHT", "WAIT_MS=50", "KEY_RETURN",
                                 "WAIT_MS=300"))
        stdout = str(reset["stdout"])
        self.assertIn(str(0x452A), self.focus_trail(reset), "Right reaches Reset")
        moved = re.findall(r"\[PLAYER\] Factory reset: moved (.+) to (.+)$", stdout, re.M)
        self.assertTrue(moved, stdout)
        for before, after in moved:
            self.assertRegex(after, r"-backup-\d{8}-\d{6}(-\d+)?$")
            self.assertTrue(after.startswith(before), (before, after))
        self.assertIn("[PLAYER_UI_TEST] factory_reset restart=skipped", stdout)

    def test_next_ui_mouse_and_keyboard_reach_the_add_card(self) -> None:
        click = self.run_player("library", ("MOUSE_MOVE=794,257", "MOUSE_DOWN", "MOUSE_UP"))
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in click["stdout"]:
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        frames = click["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["picker"], "1", "a click on the add card opens the picker")
        # The focus graph needs one laid-out frame before arrows can move.
        keys = self.run_player("library", ("WAIT_MS=100", "KEY_RIGHT", "KEY_RIGHT", "KEY_RETURN"))
        frames = keys["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["picker"], "1", "arrow keys reach the add card")

    @staticmethod
    def screens(run: dict[str, object]) -> list[tuple[str, str]]:
        """The (view, next UI route) a run showed, with repeats collapsed."""
        frames = run["frames"]
        assert isinstance(frames, list)
        shown: list[tuple[str, str]] = []
        for frame in frames:
            screen = (frame["view"], frame["next_route"])
            if not shown or shown[-1] != screen:
                shown.append(screen)
        return shown

    def test_next_ui_about_tab_opens_by_mouse_keyboard_and_controller(self) -> None:
        """#1281: the ABOUT tab opens About, and Back returns where it came from."""
        library, about, title = ("library", "1"), ("library", "3"), ("library", "2")
        runs = {
            "mouse": ("WAIT_MS=100", "MOUSE_MOVE=400,30", "MOUSE_DOWN", "MOUSE_UP",
                      "KEY_ESCAPE"),
            "keyboard": ("WAIT_MS=100", "KEY_UP", "KEY_RIGHT", "KEY_RETURN", "KEY_ESCAPE"),
            "d-pad": ("WAIT_MS=100", "PAD_DPAD_UP", "PAD_DPAD_RIGHT", "PAD_SOUTH",
                      "PAD_EAST"),
            "shoulder": ("WAIT_MS=100", "PAD_RIGHT_SHOULDER", "PAD_LEFT_SHOULDER"),
        }
        for name, events in runs.items():
            run = self.run_player("library", events)
            if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in run["stdout"]:
                self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
            with self.subTest(input=name):
                self.assertEqual(self.screens(run), [library, about, library])
        empty = self.run_player("empty", runs["mouse"])
        self.assertEqual(self.screens(empty), [library, about, library],
                         "ABOUT opens with an empty library too")
        from_title = self.run_player(
            "library",
            ("WAIT_MS=100", "KEY_RETURN", "WAIT_MS=100", "MOUSE_MOVE=400,30", "MOUSE_DOWN",
             "MOUSE_UP", "KEY_ESCAPE", "KEY_ESCAPE"),
        )
        self.assertEqual(self.screens(from_title), [library, title, about, title, library],
                         "Back from About returns to the title page it was opened from")

    def test_next_ui_settings_tab_opens_by_mouse_keyboard_and_controller(self) -> None:
        """#1281: Settings has a visible SETTINGS tab, and the menu prompt opens it."""
        library, settings = ("library", "1"), ("settings", "1")
        runs = {
            "mouse": ("WAIT_MS=100", "MOUSE_MOVE=470,30", "MOUSE_DOWN", "MOUSE_UP",
                      "KEY_ESCAPE"),
            "keyboard": ("WAIT_MS=100", "KEY_UP", "KEY_RIGHT", "KEY_RIGHT", "KEY_RETURN",
                         "KEY_ESCAPE"),
            "menu key": ("WAIT_MS=100", "KEY_F1", "KEY_ESCAPE"),
            "d-pad": ("WAIT_MS=100", "PAD_DPAD_UP", "PAD_DPAD_RIGHT", "PAD_DPAD_RIGHT",
                      "PAD_SOUTH", "PAD_EAST"),
            "start": ("WAIT_MS=100", "PAD_START", "PAD_EAST"),
        }
        for name, events in runs.items():
            run = self.run_player("library", events)
            if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in run["stdout"]:
                self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
            with self.subTest(input=name):
                self.assertEqual(self.screens(run), [library, settings, library])
        empty = self.run_player("empty", runs["mouse"])
        self.assertEqual(self.screens(empty), [library, settings, library],
                         "the SETTINGS tab works with an empty library too")
        from_about = self.run_player(
            "library", ("WAIT_MS=100", "PAD_RIGHT_SHOULDER", "PAD_RIGHT_SHOULDER", "PAD_EAST")
        )
        self.assertEqual(
            self.screens(from_about),
            [library, ("library", "3"), ("settings", "3"), ("library", "3")],
            "R steps from About to Settings, and Back returns to About",
        )

    def next_ui_run(self, view: str, events: tuple[str, ...], **kwargs: object) -> dict[str, object]:
        run = self.run_player(view, events, **kwargs)  # type: ignore[arg-type]
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in run["stdout"]:
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        return run

    @staticmethod
    def disc_card(run: dict[str, object]) -> tuple[dict[str, str], list[dict[str, str]]]:
        """The game-found card the next UI drew first: its kind and its check rows."""
        stdout = run["stdout"]
        assert isinstance(stdout, str)
        card: dict[str, str] = {}
        rows: list[dict[str, str]] = []
        for line in stdout.splitlines():
            if line.startswith("[PLAYER_UI_TEST] next_ui_disc ") and not card:
                card = dict(part.split("=", 1) for part in line.split()[2:])
            elif line.startswith("[PLAYER_UI_TEST] next_ui_disc_row ") and card and \
                    len(rows) < int(card["rows"]):
                fields = line[len("[PLAYER_UI_TEST] next_ui_disc_row "):]
                status, rest = fields.split(" tone=", 1)
                tone, rest = rest.split(" label=", 1)
                label, message = rest.split(" message=", 1)
                rows.append({"status": status[len("status="):], "tone": tone,
                             "label": label, "message": message})
        return card, rows

    def assert_plain_rows(self, rows: list[dict[str, str]], stdout: object) -> None:
        self.assertTrue(rows, stdout)
        for row in rows:
            with self.subTest(row=row["label"]):
                self.assertTrue(row["label"] and row["message"] and row["status"], row)
                self.assertNotIn("Not supported", row["status"])
                # Developer codes and messages stay in the log (#1280, #1302).
                self.assertNotRegex(row["message"], r"\b[A-Z][A-Z_]{3,}\b:")
                self.assertNotIn(" .", row["message"])
                self.assertNotIn("PARAM.SFO", row["message"])
                self.assertNotIn("jpn0", row["message"])
                self.assertTrue(row["message"].endswith("."), row)

    def test_next_ui_game_found_cards_use_plain_words_and_offer_add_honestly(self) -> None:
        """#1302: the three discs' cards in the next UI, with their real checks."""
        supported = self.next_ui_run("supported", ("WAIT_MS=100", "KEY_ESCAPE"))
        card, rows = self.disc_card(supported)
        self.assertEqual((card.get("kind"), card.get("add")), ("1", "1"), supported["stdout"])
        self.assert_plain_rows(rows, supported["stdout"])
        self.assertNotIn("error", [row["tone"] for row in rows],
                         "nothing beside Add to library reads as an error")
        fonts = [row for row in rows if row["label"] == "Fonts"]
        self.assertEqual(len(fonts), 1, rows)
        self.assertEqual((fonts[0]["status"], fonts[0]["tone"]), ("Some missing", "warning"),
                         "missing PSP fonts are a warning the game can still be added with")
        self.assertEqual(self.screens(supported)[-1][0], "library")

        untested = self.next_ui_run("experimental", ("WAIT_MS=100", "KEY_RETURN"))
        card, rows = self.disc_card(untested)
        frames = untested["frames"]
        assert isinstance(frames, list)
        games_before = int(frames[0]["games"])
        self.assertEqual((card.get("kind"), card.get("add")), ("2", "1"), untested["stdout"])
        self.assert_plain_rows(rows, untested["stdout"])
        self.assertNotIn("error", [row["tone"] for row in rows])
        self.assertEqual(frames[-1]["view"], "library", "Add to library adds and returns")
        self.assertEqual(int(frames[-1]["games"]), games_before + 1)

        # #308: an encrypted program the user can supply a decrypted copy of
        # gets the file it needs and Check again, which reads the same disc
        # again: no picker, nothing added, the same program input.
        blocked = self.next_ui_run("experimental-blocked", ("WAIT_MS=100", "KEY_RETURN"))
        card, rows = self.disc_card(blocked)
        self.assertEqual((card.get("kind"), card.get("add"), card.get("files")), ("3", "0", "1"),
                         blocked["stdout"])
        self.assertIn("[PLAYER_UI_TEST] next_ui_disc_file name=EBOOT.elf replace=0 "
                      "source=PSP_GAME/SYSDIR/EBOOT.BIN", blocked["stdout"])
        self.assert_plain_rows(rows, blocked["stdout"])
        self.assertNotIn("Game program", [row["label"] for row in rows],
                         "the check that keeps the disc out is the card's reason, not a row")
        frames = blocked["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["picker"], "0", "Check again opens no picker")
        self.assertEqual(frames[-1]["view"], "experimental", "the card stays for its files")
        self.assertEqual(frames[-1]["input_class"], "encrypted-retail")
        self.assertEqual(frames[-1]["input_files"], "1")
        self.assertEqual(frames[-1]["games"], frames[0]["games"], "nothing is added")

        unsupported = self.next_ui_run("unsupported", ("WAIT_MS=100", "KEY_RETURN"))
        card, _ = self.disc_card(unsupported)
        self.assertEqual((card.get("kind"), card.get("add")), ("4", "0"), unsupported["stdout"])
        self.assertEqual(self.screens(unsupported)[-1][0], "library")

    def test_next_ui_checks_an_added_disc_once_on_a_worker(self) -> None:
        """#1731: a disc added from the game-found card is hashed once after the add, on the
        disc check worker (never the UI thread), and compared with the disc list the user
        imported; its library record keeps the hashes and the tier. With no list it is
        hashed and left unchecked. The worker is held for a moment: the library draws frames
        meanwhile, with the check running."""
        with tempfile.TemporaryDirectory(prefix=".disc-check-", dir=ROOT) as tmp:
            iso_path = Path(tmp) / "synthetic.iso"
            create_test_iso_with_executables(
                iso_path, build_plain_mips_elf(), disc_id="TEST00043",
                title="Synthetic Checked Disc",
            )
            iso = iso_path.read_bytes()
        sha1 = hashlib.sha1(iso).hexdigest()
        crc = f"{zlib.crc32(iso):08x}"
        disc_list = ("<?xml version=\"1.0\"?>\n<datafile>\n<header><name>Synthetic</name>"
                     "<version>1</version></header>\n<game name=\"Synthetic Checked Disc\">"
                     f"<rom name=\"synthetic.iso\" size=\"{len(iso)}\" crc=\"{crc}\" "
                     f"sha1=\"{sha1}\"/></game>\n</datafile>\n")
        events = ("WAIT_VIEW=experimental|unsupported|supported|error,15000", "WAIT_MS=100",
                  "KEY_RETURN", "WAIT_MS=2000")
        for listed, tier in ((True, "VERIFIED_RETAIL"), (False, "NOT_CHECKED")):
            with self.subTest(listed=listed):
                run = self.next_ui_run("library", events, drop_bytes=("source-owned.iso", iso),
                                       disc_list=disc_list if listed else None,
                                       env_extra={"NK_UI_TEST_DISC_CHECK_HOLD_MS": "700"})
                frames = run["frames"]
                stdout = run["stdout"]
                assert isinstance(frames, list) and isinstance(stdout, str)
                self.assertEqual(frames[-1]["view"], "library", stdout)
                checking = [frame for frame in frames
                            if frame["view"] == "library" and frame["disc_check"] == "1"]
                self.assertGreaterEqual(len(checking), 2, stdout)
                self.assertEqual(frames[-1]["disc_check"], "0", stdout)
                checks = re.findall(
                    r"^\[PLAYER\] DISC_CHECK disc=(\S+) result=(\S+) tier=(\S+)$", stdout, re.M)
                self.assertEqual(checks, [("TEST00043", "HASHED", tier)], stdout)
                library = run["library"]
                assert isinstance(library, dict)
                records = [game for game in library["games"] if game["disc_id"] == "TEST00043"]
                self.assertEqual(len(records), 1, library)
                self.assertEqual(records[0].get("content_sha1"), sha1)
                self.assertEqual(records[0].get("disc_verification", "NOT_CHECKED"), tier)

    # The next UI's Settings controls (player_ui_nk.h), as the frame line prints them.
    SETTINGS_FOCUS = {
        "resolution": str(0x4501), "pacing": str(0x4502), "done": str(0x450C),
        "online": str(0x450D), "notices": str(0x450E), "remove_tools": str(0x450F),
        "build_cache": str(0x451A), "share_libraries": str(0x451B),
        "build_speed": str(0x4555),
        "developer": str(0x4515), "back": str(0x4517), "confirm_remove": str(0x4519),
        "controller": str(0x450B), "scaling": str(0x4505),
        "launcher_fullscreen": str(0x450A), "accessibility": str(0x452C),
        "interface_size": str(0x452D), "high_contrast": str(0x452E),
        "reduce_motion": str(0x4509), "text_size": str(0x452F),
        "ace": str(0x4540), "ace_open": str(0x4541), "ace_mode": str(0x4542),
        "ui_sounds": str(0x4508),
        "preset": str(0x4510),
    }
    # From Resolution, Down runs through the Game card's seven rows (Play mode, which opens
    # Settings > Enhanced, then Graphics preset, #2150) to the button row, which it enters at
    # Third-party notices.
    TO_SETTINGS_BUTTONS = ("KEY_DOWN",) * 7

    def focus_trail(self, run: dict[str, object]) -> list[str]:
        """The controls the next UI focused, with repeats collapsed."""
        frames = run["frames"]
        assert isinstance(frames, list)
        trail: list[str] = []
        for frame in frames:
            if not trail or trail[-1] != frame["next_focus"]:
                trail.append(frame["next_focus"])
        return trail

    # The nickname editor's Save and Skip (or Cancel), and Settings > Nickname (#2197).
    NICKNAME_SAVE = str(0x4560)
    NICKNAME_SKIP = str(0x4561)
    SETTINGS_NICKNAME = str(0x4562)

    def test_next_ui_first_launch_nickname_prompt(self) -> None:
        """#2197: the first launch asks for a nickname once. Skip (Esc) keeps the neutral
        "Player" and is remembered, so the next start does not ask; a typed nickname is
        saved. --view=nickname-prompt is the one view that shows it. Failing-before: there was
        no prompt and settings.json had no nickname."""
        skip = self.next_ui_run("nickname-prompt", ("WAIT_MS=300", "KEY_ESCAPE", "WAIT_MS=200"))
        self.assertIn(self.NICKNAME_KEY_Q, self.focus_trail(skip),
                      "the prompt opens on its keyboard's first letter")
        self.assertNotIn("QUIT_DIALOG", str(skip["stdout"]), "Esc is Skip, not quit")
        settings = skip["settings"]
        assert isinstance(settings, dict)
        self.assertEqual(settings.get("nickname"), "Player")
        self.assertIs(settings.get("nickname_asked"), True)
        again = self.next_ui_run("nickname-prompt", ("WAIT_MS=300",), settings=settings)
        self.assertNotIn(self.NICKNAME_KEY_Q, self.focus_trail(again), "it is asked only once")
        typed = self.next_ui_run(
            "nickname-prompt", ("WAIT_MS=300", "TEXT=Ren 7", "KEY_RETURN", "WAIT_MS=200"))
        self.assertNotIn(self.NICKNAME_KEY_Q, self.focus_trail(self.next_ui_run(
            "library", ("WAIT_MS=300",))), "any other view keeps the prompt down")
        saved = typed["settings"]
        assert isinstance(saved, dict)
        self.assertEqual(saved.get("nickname"), "Ren 7")
        self.assertIs(saved.get("nickname_asked"), True)

    def test_next_ui_settings_edits_the_nickname(self) -> None:
        """#2197: Settings > Online and developer > Nickname opens the editor on its keyboard.
        Typing on a physical keyboard moves focus to Done, so Enter saves what was typed; an
        empty Done (Backspace on nothing, then Enter) is refused and leaves the nickname as it
        was; Cancel returns to the row. Failing-before: the page had no Nickname row."""
        run = self.next_ui_run(
            "online-settings",
            ("WAIT_MS=100", "KEY_DOWN", "KEY_RETURN", "WAIT_MS=100", "TEXT=Kai", "KEY_RETURN",
             "WAIT_MS=100", "KEY_RETURN", "WAIT_MS=100", "KEY_BACKSPACE", "KEY_RETURN",
             "WAIT_MS=100", "KEY_ESCAPE", "WAIT_MS=100"),
            settings={"nickname": "Old name", "nickname_asked": True})
        trail = self.focus_trail(run)
        self.assertEqual(trail[:4], [self.SETTINGS_FOCUS["developer"], self.SETTINGS_NICKNAME,
                                     self.NICKNAME_KEY_Q, self.NICKNAME_SAVE], trail)
        self.assertEqual(trail[-1], self.SETTINGS_NICKNAME, "Cancel returns to the row")
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["view"], "online_settings", "the editor keeps the page")
        settings = run["settings"]
        assert isinstance(settings, dict)
        self.assertEqual(settings.get("nickname"), "Kai", "Save kept it; the empty Save did not")
        # Typing stops at the 32-byte cap: 33 letters save as 32.
        capped = self.next_ui_run(
            "online-settings",
            ("WAIT_MS=100", "KEY_DOWN", "KEY_RETURN", "WAIT_MS=100", "TEXT=" + "x" * 33,
             "KEY_RETURN", "WAIT_MS=100"),
            settings={"nickname": "Old name", "nickname_asked": True})
        saved = capped["settings"]
        assert isinstance(saved, dict)
        self.assertEqual(saved.get("nickname"), "x" * 32, "the editor keeps the first 32 bytes")

    # The nickname editor's on-screen keyboard (#2197): its first letter, q, is where it opens.
    NICKNAME_KEY_BASE = 0x4570
    NICKNAME_KEY_Q = str(0x4570 + 10)

    @staticmethod
    def nickname_states(run: dict[str, object]) -> list[tuple[str, str]]:
        """What the nickname editor held while it was open, frame by frame, and the count it
        showed, with repeats collapsed."""
        stdout = run["stdout"]
        assert isinstance(stdout, str)
        states: list[tuple[str, str]] = []
        for text_hex, count in re.findall(
                r"^\[PLAYER_UI_TEST\] nickname_editor frame=\d+ text_hex=([0-9a-f]*) count=(.*?)\r?$",
                stdout, re.M):
            state = (bytes.fromhex(text_hex).decode("utf-8"), count)
            if not states or states[-1] != state:
                states.append(state)
        return states

    def test_next_ui_nickname_keyboard_types_with_a_controller(self) -> None:
        """#2197: the first-launch prompt's on-screen keyboard types a nickname with a
        controller alone: the d-pad moves, A types the focused key, L is Shift, Start is Done,
        and the setting holds what was typed. The count follows the text. Failing-before: the
        editor had no keys, so a controller could only skip or save the nickname in use."""
        right7 = ("PAD_DPAD_RIGHT",) * 7
        left7 = ("PAD_DPAD_LEFT",) * 7
        run = self.next_ui_run(
            "nickname-prompt",
            ("WAIT_MS=300", "PAD_LEFT_SHOULDER", "PAD_DPAD_DOWN") + right7 +
            ("PAD_SOUTH", "PAD_LEFT_SHOULDER") + left7 + ("PAD_SOUTH", "PAD_DPAD_UP") + right7 +
            ("PAD_SOUTH", "WAIT_MS=100", "PAD_START", "WAIT_MS=200"))
        trail = self.focus_trail(run)
        shown = [focus for focus in trail if focus != "0"]
        self.assertEqual(shown[0] if shown else None, self.NICKNAME_KEY_Q,
                         "the keyboard opens on its first letter")
        states = self.nickname_states(run)
        self.assertEqual([text for text, _ in states], ["", "K", "Ka", "Kai"], states)
        self.assertEqual(states[-1][1], "3 / 32 bytes", "the count follows the text")
        settings = run["settings"]
        assert isinstance(settings, dict)
        self.assertEqual(settings.get("nickname"), "Kai", "Done saved what the keys typed")
        self.assertIs(settings.get("nickname_asked"), True)

    def test_next_ui_nickname_keyboard_cancel_keeps_the_nickname(self) -> None:
        """#2197: Settings > Nickname: keys typed with a controller, then Cancel (B, or the
        keyboard's own Cancel key) leaves the nickname as it was before the editor opened, and
        focus returns to the row. Failing-before: the editor had no keys to type with."""
        old = {"nickname": "Old name", "nickname_asked": True}
        open_editor = ("WAIT_MS=100", "PAD_DPAD_DOWN", "PAD_SOUTH", "WAIT_MS=100")
        typed = ("PAD_SOUTH", "PAD_DPAD_RIGHT", "PAD_SOUTH")
        back = self.next_ui_run("online-settings",
                                open_editor + typed + ("PAD_EAST", "WAIT_MS=100"),
                                settings=dict(old))
        key = self.next_ui_run(
            "online-settings",
            open_editor + typed + ("PAD_DPAD_LEFT",) + ("PAD_DPAD_DOWN",) * 4 +
            ("PAD_DPAD_RIGHT",) * 3 + ("PAD_SOUTH", "WAIT_MS=100"),
            settings=dict(old))
        for name, run in (("B", back), ("the Cancel key", key)):
            states = self.nickname_states(run)
            self.assertIn(("qw", "2 / 32 bytes"), states, f"{name}: the keys typed qw")
            # Cancel saves nothing; a settings.json the run did write keeps the nickname too.
            saved = run["settings"]
            nickname = saved.get("nickname") if isinstance(saved, dict) else old["nickname"]
            self.assertEqual(nickname, "Old name",
                             f"{name}: Cancel keeps the nickname from before the editor opened")
            self.assertEqual(self.focus_trail(run)[-1], self.SETTINGS_NICKNAME,
                             f"{name}: focus returns to the row")
        self.assertIn(self.NICKNAME_SKIP, self.focus_trail(key), "the Cancel key was pressed")

    def test_next_ui_nickname_keyboard_stops_at_32_bytes(self) -> None:
        """#2197: pressing a key past the 32-byte cap types nothing: 33 presses of q leave 32
        bytes, the count reads 32 / 32 bytes, and Done saves those 32. Failing-before: the
        editor had no keys."""
        run = self.next_ui_run(
            "online-settings",
            ("WAIT_MS=100", "PAD_DPAD_DOWN", "PAD_SOUTH", "WAIT_MS=100") + ("PAD_SOUTH",) * 33 +
            ("WAIT_MS=100", "PAD_START", "WAIT_MS=100"),
            settings={"nickname": "Old name", "nickname_asked": True})
        states = self.nickname_states(run)
        self.assertEqual(states[-1], ("q" * 32, "32 / 32 bytes"), states[-3:])
        self.assertTrue(all(len(text.encode("utf-8")) <= 32 for text, _ in states), states[-3:])
        settings = run["settings"]
        assert isinstance(settings, dict)
        self.assertEqual(settings.get("nickname"), "q" * 32)

    def test_next_ui_nickname_keyboard_refuses_what_the_rule_refuses(self) -> None:
        """#2197: what is typed goes through the keyboard's filter, nk_nickname_valid's rule:
        a control character (C0, DEL) leaves the text and its count as they were, and what the
        rule takes is still typed. Failing-before: the editor showed no count and kept no
        filtered text the test could read."""
        run = self.next_ui_run(
            "online-settings",
            ("WAIT_MS=100", "PAD_DPAD_DOWN", "PAD_SOUTH", "WAIT_MS=100", "TEXT=Ab",
             "WAIT_MS=50", "TEXT=\x01", "WAIT_MS=50", "TEXT=\x7f", "WAIT_MS=50", "TEXT=\x1b",
             "WAIT_MS=50", "TEXT=c", "WAIT_MS=50", "KEY_RETURN", "WAIT_MS=100"),
            settings={"nickname": "Old name", "nickname_asked": True})
        states = self.nickname_states(run)
        self.assertEqual(states, [("", "0 / 32 bytes"), ("Ab", "2 / 32 bytes"),
                                  ("Abc", "3 / 32 bytes")], states)
        settings = run["settings"]
        assert isinstance(settings, dict)
        self.assertEqual(settings.get("nickname"), "Abc")

    def test_next_ui_nickname_done_trims_spaces_and_refuses_only_spaces(self) -> None:
        """#2197 review: Done saves the typed text without the spaces around it, and a nickname
        of spaces alone is refused (nk_nickname_valid, the runtime's rule too), so the editor
        stays open and the nickname is kept. Typed both on a physical keyboard and with the
        on-screen Space key (Y). Failing-before: "   " passed the rule and was saved, and
        "  Kai  " was saved with its spaces."""
        old = {"nickname": "Old name", "nickname_asked": True}
        open_editor = ("WAIT_MS=100", "PAD_DPAD_DOWN", "PAD_SOUTH", "WAIT_MS=100")
        trimmed = self.next_ui_run(
            "online-settings", open_editor + ("TEXT=  Kai  ", "KEY_RETURN", "WAIT_MS=100"),
            settings=dict(old))
        self.assertIn(("  Kai  ", "7 / 32 bytes"), self.nickname_states(trimmed))
        saved = trimmed["settings"]
        assert isinstance(saved, dict)
        self.assertEqual(saved.get("nickname"), "Kai", "Done trims the spaces around the name")
        self.assertEqual(self.focus_trail(trimmed)[-1], self.SETTINGS_NICKNAME,
                         "the trimmed name was saved and the editor closed")
        for name, typed in (("a physical keyboard", ("TEXT=   ", "KEY_RETURN")),
                            ("the Space key", ("PAD_NORTH",) * 3 + ("PAD_START",))):
            with self.subTest(typed=name):
                run = self.next_ui_run("online-settings",
                                       open_editor + typed + ("WAIT_MS=200",),
                                       settings=dict(old))
                states = self.nickname_states(run)
                self.assertIn(("   ", "3 / 32 bytes"), states, f"{name}: three spaces were typed")
                saved = run["settings"]
                nickname = saved.get("nickname") if isinstance(saved, dict) else old["nickname"]
                self.assertEqual(nickname, "Old name", f"{name}: spaces alone are not saved")
                self.assertEqual(states[-1], ("   ", "3 / 32 bytes"),
                                 f"{name}: the refused Done keeps the editor open as it was")
                self.assertNotEqual(self.focus_trail(run)[-1], self.SETTINGS_NICKNAME,
                                    f"{name}: the editor did not close")

    def test_next_ui_nickname_physical_tab_moves_focus_and_is_not_shift(self) -> None:
        """#2197 review N3: a physical Tab is the toolkit's focus cycle (NK_UI_EV_CYCLE) in the
        nickname editor as everywhere, not Shift: L and R (NK_UI_EV_TAB) are Shift, and the Q
        and E keys that also mean TAB elsewhere type their letters here. Tab moves focus off q,
        and A then types a lowercase letter."""
        run = self.next_ui_run(
            "online-settings",
            ("WAIT_MS=100", "PAD_DPAD_DOWN", "PAD_SOUTH", "WAIT_MS=100", "KEY_TAB", "WAIT_MS=50",
             "PAD_SOUTH", "WAIT_MS=50", "TEXT=qe", "WAIT_MS=100"),
            settings={"nickname": "Old name", "nickname_asked": True})
        trail = self.focus_trail(run)
        at_q = trail.index(self.NICKNAME_KEY_Q)
        self.assertEqual(trail[at_q + 1:at_q + 2], [str(0x4570 + 11)],
                         f"Tab moves focus from q to the next key, w: {trail}")
        states = self.nickname_states(run)
        self.assertEqual([text for text, _ in states], ["", "w", "wqe"],
                         f"Tab did not turn Shift on, and q and e typed letters: {states}")

    def bare_screenshot(self, view: str,
                        settings: dict[str, object] | None) -> tuple[bytes, object]:
        """A bare ``--screenshot=<bmp> --view=<view>`` run, as tools/capture_native_screenshots.py
        starts the player: no --ui-test-* option, on a throwaway data root that has a
        settings.json only when one is given. Its pixels, and the settings it left behind."""
        with tempfile.TemporaryDirectory(prefix=".player-ui-test-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            localappdata = scratch / "localappdata"
            runtime_root = scratch / "runtime"
            localappdata.mkdir()
            runtime_root.mkdir()
            config = (localappdata / "Nakagawa" / "config" if os.name == "nt"
                      else scratch / "config" / "nakagawa-recomp")
            if settings is not None:
                config.mkdir(parents=True)
                (config / "settings.json").write_text(
                    json.dumps({"schema_version": 2, **settings}), encoding="utf-8")
            env = os.environ.copy()
            env.update({
                "SDL_VIDEODRIVER": "dummy", "SDL_RENDER_DRIVER": "software",
                "APPDATA": str(scratch), "LOCALAPPDATA": str(localappdata),
                "USERPROFILE": str(scratch), "HOME": str(scratch),
                "XDG_CONFIG_HOME": str(scratch / "config"), "XDG_DATA_HOME": str(scratch / "data"),
                "XDG_CACHE_HOME": str(scratch / "cache"),
            })
            bmp = scratch / "shot.bmp"
            completed = subprocess.run(
                [str(PLAYER_EXE), f"--screenshot={bmp}", f"--view={view}",
                 f"--runtime-root={runtime_root}", "--width=1280", "--height=720"],
                cwd=ROOT, env=env, capture_output=True, text=True, timeout=120, check=False)
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            self.assertTrue(bmp.is_file(), completed.stdout)
            saved_path = config / "settings.json"
            saved = (json.loads(saved_path.read_text(encoding="utf-8"))
                     if saved_path.is_file() else None)
            return read_bmp(bmp)[3], saved

    def test_bare_screenshot_view_never_shows_the_nickname_prompt(self) -> None:
        """#2197 review B1: a bare --screenshot --view run on a fresh data root captures the page
        it names, never the first-launch nickname prompt over it, and records no answer;
        --view=nickname-prompt is the one view that shows it. Failing-before: the prompt
        opened over every such run, so a fresh root's frame differed from an answered one's.

        Review N1: whether the next UI runs here is the font-independent UI_NEXT_ACTIVE marker,
        as the other next-UI tests read it, never the frames themselves. On hosted CI (the
        Linux player UI job installs SDL3_ttf, and the repository ships open fonts) a missing
        marker fails instead of skipping, and with the next UI running, a prompt view that
        draws the same frame as the answered one is a failure.

        Review S1: both roots turn Reduce motion on (``reduce_motion_choice``), which holds every
        animation and transition still, so the frame a run captures does not depend on how far
        a busy host let its clock run before the capture. The fresh root is still one that has
        not answered the prompt: a settings.json without ``nickname_asked`` reads as not asked,
        as a missing file does."""
        marker = "[PLAYER_UI_TEST] UI_NEXT_ACTIVE"
        if marker not in str(self.run_player("empty")["stdout"]):
            if os.environ.get("GITHUB_ACTIONS") or os.environ.get("CI"):
                self.fail("the next UI did not start on this CI host (no UI_NEXT_ACTIVE "
                          "marker), so the nickname prompt could not be checked")
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        fresh_root = {"reduce_motion_choice": "on"}
        answered = {**fresh_root, "nickname_asked": True}
        prompt_fresh, _ = self.bare_screenshot("nickname-prompt", fresh_root)
        prompt_answered, _ = self.bare_screenshot("nickname-prompt", answered)
        self.assertTrue(prompt_fresh != prompt_answered, "the prompt view drew no prompt")
        for view in ("settings", "library"):
            with self.subTest(view=view):
                fresh, saved = self.bare_screenshot(view, fresh_root)
                reference, _ = self.bare_screenshot(view, answered)
                self.assertEqual(fresh, reference, "the fresh root's frame shows the prompt")
                self.assertFalse(isinstance(saved, dict) and saved.get("nickname_asked") is True,
                                 "holding the prompt down is not saved as an answer")

    @staticmethod
    def preset_lines(run: dict[str, object]) -> list[dict[str, str]]:
        """The graphics preset row and note the main Settings page drew, in order (#2150)."""
        stdout = run["stdout"]
        assert isinstance(stdout, str)
        prefix = "[PLAYER_UI_TEST] next_ui_settings_preset "
        lines = []
        for line in stdout.splitlines():
            if line.startswith(prefix):
                value, note = line[len(prefix):].split("|", 1)
                lines.append({"value": value[len("value="):], "note": note[len("note="):]})
        return lines

    def test_next_ui_graphics_preset_writes_its_settings(self) -> None:
        """#2150: the Game card's last row steps the graphics preset from Custom (a new
        library) to Low, then Medium. A preset is an overlay for Enhanced and Ace sessions, so
        the choice is saved and the rows keep their values, and the page's notice line says
        where the preset applies."""
        focus = self.SETTINGS_FOCUS
        run = self.next_ui_run(
            "settings",
            ("WAIT_MS=100",) + ("KEY_DOWN",) * 6 +
            ("KEY_RETURN", "WAIT_MS=100", "KEY_RETURN", "WAIT_MS=100", "KEY_ESCAPE"),
        )
        self.assertIn(focus["preset"], self.focus_trail(run), "Down reaches Graphics preset")
        lines = self.preset_lines(run)
        self.assertEqual([line["value"] for line in lines], ["Custom", "Low", "Medium"],
                         run["stdout"])
        self.assertEqual((lines[0]["note"],), ("",),
                         "a new library starts on Custom, with the page's own line")
        medium = lines[-1]
        self.assertEqual(medium["note"],
                         "Medium is used in Enhanced and Ace, with crisp text and FSR 1 on. "
                         "Authentic uses your settings.")
        settings = run["settings"]
        assert isinstance(settings, dict)
        self.assertEqual(settings.get("graphics_preset"), "medium", "two steps: Low, then Medium")
        # The rows are the new library's own, not Medium's 2x: only sessions take a preset.
        self.assertEqual(settings.get("resolution_scale"), 1, "the Resolution row is unchanged")
        self.assertEqual(settings.get("present_scaling"), "sharp")
        self.assertIs(settings.get("vsync"), True)
        self.assertIs(settings.get("fullscreen"), False)

    def test_next_ui_graphics_preset_ultra_note_and_custom_on_edit(self) -> None:
        """#2150: Ultra's row reads Ultra and its note names the Enhanced parts it turns on,
        High's the one it turns off; editing a row the preset sets (Resolution) makes it Custom
        and the note goes."""
        focus = self.SETTINGS_FOCUS
        run = self.next_ui_run(
            "settings",
            ("WAIT_MS=100",) + ("KEY_DOWN",) * 6 +
            ("KEY_RETURN", "WAIT_MS=100") * 4 + ("KEY_UP",) * 6 +
            ("KEY_RETURN", "WAIT_MS=100", "KEY_ESCAPE"),
        )
        trail = self.focus_trail(run)
        self.assertIn(focus["preset"], trail)
        self.assertIn(focus["resolution"], trail[trail.index(focus["preset"]):],
                      "Up returns to Resolution")
        lines = self.preset_lines(run)
        self.assertEqual([line["value"] for line in lines],
                         ["Custom", "Low", "Medium", "High", "Ultra", "Custom"],
                         run["stdout"])
        ultra = lines[4]
        self.assertIn("Ultra is used in Enhanced and Ace", ultra["note"])
        self.assertIn("with crisp text, FSR 1 and full 32-bit colour on", ultra["note"])
        self.assertIn("Authentic uses your settings", ultra["note"])
        self.assertIn("with crisp text on and FSR 1 off", lines[3]["note"])
        self.assertEqual((lines[-1]["note"],), ("",),
                         "an edited row makes the preset Custom and its note goes")
        settings = run["settings"]
        assert isinstance(settings, dict)
        self.assertEqual(settings.get("graphics_preset"), "custom")
        self.assertEqual(settings.get("resolution_scale"), 2, "Resolution stepped from 1x to 2x")

    def test_next_ui_settings_changes_are_saved_and_done_returns(self) -> None:
        """#1302: Settings in the next UI runs the same handlers and saves each change."""
        focus = self.SETTINGS_FOCUS
        run = self.next_ui_run(
            "settings",
            ("WAIT_MS=100", "KEY_RETURN", "KEY_DOWN", "KEY_RETURN", "WAIT_MS=100", "KEY_ESCAPE"),
        )
        self.assertEqual(self.focus_trail(run)[:2], [focus["resolution"], focus["pacing"]],
                         "Settings opens on Resolution")
        settings = run["settings"]
        assert isinstance(settings, dict)
        self.assertEqual(settings.get("schema_version"), 2)
        self.assertEqual(settings.get("resolution_scale"), 2, "Resolution steps 1x to 2x")
        self.assertEqual(settings.get("fps_cap"), 0, "Frame pacing switches to no extra cap")
        self.assertIs(settings.get("ui_sounds"), False, "launcher sounds are off by default")
        self.assertEqual(self.screens(run)[-1][0], "library")
        # Frame pacing names PSP timing or no extra cap, never a fixed frame-rate cap.
        launcher = (ROOT / "src" / "player" / "ui" / "player_ui_nk.c").read_text(
            encoding="utf-8")
        self.assertIn('"PSP timing" : "No extra cap"', launcher)
        for source in sorted((ROOT / "src" / "player").rglob("*.c")):
            text = source.read_text(encoding="utf-8").upper()
            self.assertNotIn("30 FPS CAP", text, source.name)
            self.assertNotIn("60 FPS CAP", text, source.name)

        done = self.next_ui_run(
            "settings",
            ("WAIT_MS=100",) + self.TO_SETTINGS_BUTTONS + ("KEY_LEFT", "KEY_LEFT", "KEY_RETURN"),
        )
        self.assertIn(focus["done"], self.focus_trail(done))
        self.assertEqual(self.screens(done), [("settings", "1"), ("library", "1")],
                         "Done saves and returns to the library")

        tabs = self.next_ui_run(
            "settings", ("WAIT_MS=100", "MOUSE_MOVE=400,30", "MOUSE_DOWN", "MOUSE_UP")
        )
        self.assertEqual(self.screens(tabs), [("settings", "1"), ("library", "3")],
                         "the ABOUT tab on Settings closes it and opens About")

    def test_next_ui_scaling_and_menu_sounds_are_saved(self) -> None:
        """Scaling (#1129) cycles Sharp to Integer and is saved by its name; Menu
        sounds, off by default, is switched on and saved."""
        focus = self.SETTINGS_FOCUS
        # Down from Resolution: Frame pacing, VSync, Game fullscreen, then Scaling.
        scaling = self.next_ui_run(
            "settings",
            ("WAIT_MS=100",) + ("KEY_DOWN",) * 4 + ("KEY_RETURN", "WAIT_MS=100", "KEY_ESCAPE"),
        )
        self.assertEqual(self.focus_trail(scaling)[4], focus["scaling"])
        settings = scaling["settings"]
        assert isinstance(settings, dict)
        self.assertEqual(settings.get("present_scaling"), "integer")
        self.assertIs(settings.get("ui_sounds"), False)

        # Menu sounds is on the Launcher and sound card, beside the Game card.
        sounds = self.next_ui_run(
            "settings",
            ("WAIT_MS=100", "KEY_RIGHT", "KEY_DOWN", "KEY_DOWN", "KEY_DOWN", "KEY_RETURN",
             "WAIT_MS=100", "KEY_ESCAPE"),
        )
        self.assertIn(focus["ui_sounds"], self.focus_trail(sounds))
        settings = sounds["settings"]
        assert isinstance(settings, dict)
        self.assertIs(settings.get("ui_sounds"), True)

    def test_next_ui_play_mode_is_saved_and_its_page_opens(self) -> None:
        """The play modes (#2055): Authentic by default. Settings has one Play mode row
        (#2007); it opens Settings > Enhanced, whose play mode row makes Enhanced the library's
        default and saves it."""
        focus = self.SETTINGS_FOCUS
        run = self.next_ui_run(
            "settings",
            ("WAIT_MS=100",) + ("KEY_DOWN",) * 5 +
            ("KEY_RETURN", "WAIT_MS=100", "KEY_RETURN", "WAIT_MS=100", "KEY_ESCAPE"),
        )
        self.assertIn(focus["ace_open"], self.focus_trail(run), "Down reaches the Play mode row")
        self.assertNotIn(focus["ace"], self.focus_trail(run), "there is no second Play mode row")
        self.assertEqual([view for view, _ in self.screens(run)],
                         ["settings", "ace_settings", "settings"])
        settings = run["settings"]
        assert isinstance(settings, dict)
        enhanced = settings.get("enhanced")
        assert isinstance(enhanced, dict), settings
        self.assertEqual(enhanced.get("default"), "enhanced",
                         "Enhanced chosen on its page and saved")
        self.assertEqual(enhanced.get("af"), "16", "the parts keep their defaults")
        self.assertNotIn("ace", settings, "the development builds' key is not written")
        pad = self.next_ui_run(
            "settings",
            ("WAIT_MS=100",) + ("PAD_DPAD_DOWN",) * 5 +
            ("PAD_SOUTH", "WAIT_MS=100", "PAD_SOUTH", "WAIT_MS=100", "PAD_EAST"),
        )
        pad_settings = pad["settings"]
        assert isinstance(pad_settings, dict)
        pad_enhanced = pad_settings.get("enhanced")
        assert isinstance(pad_enhanced, dict), pad_settings
        self.assertEqual(pad_enhanced.get("default"), "enhanced",
                         "a controller's d-pad and south do the same")
        page = self.next_ui_run(
            "settings",
            ("WAIT_MS=100",) + ("KEY_DOWN",) * 5 + ("KEY_RETURN", "WAIT_MS=100", "KEY_ESCAPE",
                                                     "WAIT_MS=100"),
        )
        self.assertEqual([view for view, _ in self.screens(page)],
                         ["settings", "ace_settings", "settings"])
        trail = self.focus_trail(page)
        self.assertEqual(trail[-2:], [focus["ace_mode"], focus["ace_open"]],
                         "the page opens on its play mode and Back focuses the row that opened it")

    def test_next_ui_enhanced_page_shows_the_graphics_presets_parts(self) -> None:
        """#2150, #2280: with Ultra chosen, Settings > Enhanced shows the parts the preset sets
        with its values and name, over the library's own (all off here); the other parts keep
        the library's value."""
        run = self.next_ui_run(
            "settings",
            ("WAIT_MS=100",) + ("KEY_DOWN",) * 5 + ("KEY_RETURN", "WAIT_MS=150", "KEY_ESCAPE",
                                                     "WAIT_MS=100"),
            settings={"graphics_preset": "ultra",
                      "enhanced": {"default": "enhanced", "af": "4", "text": "off",
                                   "readout": "off", "notice": "on", "smooth": "off",
                                   "upscale": "off", "twin": "off"}},
        )
        pages = [page for page in self.enhanced_pages(run) if page.get("title") == "library"]
        self.assertTrue(pages, run["stdout"])
        page = pages[-1]
        self.assertEqual(page["text"], "On (Ultra)", page)
        self.assertEqual(page["upscale"], "FSR 1 (Ultra)", page)
        self.assertEqual(page["twin"], "On (Ultra)", page)
        self.assertNotIn("(Ultra)", page["af"], "texture filtering is the library's own")
        self.assertNotIn("(Ultra)", page["smooth"])

    def test_a_dedicated_data_root_keeps_everything_out_of_the_default_profile(self) -> None:
        """--user-data-root=<folder> opens the player on a second profile, for example a
        whole collection under test: a settings change lands in <folder>/config, and
        nothing is written under the default profile. Before, only the library moved;
        settings, logs, the cache and saves still went to the default profile."""
        run = self.next_ui_run(
            "settings",
            ("WAIT_MS=100", "KEY_RETURN", "KEY_DOWN", "KEY_RETURN", "WAIT_MS=100", "KEY_ESCAPE"),
            dedicated_data_root=True,
        )
        settings = run["settings"]
        assert isinstance(settings, dict)
        self.assertEqual(settings.get("resolution_scale"), 2, "the change was saved")
        self.assertIn("config/settings.json", run["dedicated_files"])
        self.assertEqual(run["default_profile_files"], [],
                         "nothing is written under the default profile")

    def test_next_ui_accessibility_settings_are_saved_and_back_returns(self) -> None:
        """#1739: Settings > Accessibility steps each setting with the keyboard, saves it,
        and Esc returns to Settings with its row focused. The test mode starts with
        Reduce motion On, so one step turns it Off."""
        focus = self.SETTINGS_FOCUS
        run = self.next_ui_run(
            "accessibility-settings",
            ("WAIT_MS=100", "KEY_RETURN", "KEY_DOWN", "KEY_DOWN", "KEY_RETURN", "KEY_DOWN",
             "KEY_RETURN", "KEY_ESCAPE", "WAIT_MS=100"),
        )
        self.assertEqual([view for view, _ in self.screens(run)],
                         ["accessibility_settings", "settings"])
        self.assertEqual(self.focus_trail(run),
                         [focus["interface_size"], focus["text_size"], focus["high_contrast"],
                          focus["reduce_motion"], focus["accessibility"]],
                         "the page opens on its first setting and Back focuses its row")
        settings = run["settings"]
        assert isinstance(settings, dict)
        self.assertEqual(settings.get("interface_size"), 100, "Automatic steps to 100%")
        self.assertEqual(settings.get("text_size"), 100, "the text size was not stepped")
        self.assertEqual(settings.get("high_contrast_focus"), "on", "Automatic steps to On")
        self.assertEqual(settings.get("reduce_motion_choice"), "off", "On steps to Off")
        self.assertIs(settings.get("reduce_motion"), False,
                      "older players read Off as reduce_motion false")

    def test_next_ui_text_size_steps_and_is_saved(self) -> None:
        """Settings > Accessibility's Text size, under Interface size, steps Standard
        (100%), Large (125%) and Larger (150%) and saves each step as text_size in
        settings.json, without touching the interface size. The page keeps drawing
        and taking input at the larger sizes: Down still moves on to High contrast."""
        focus = self.SETTINGS_FOCUS
        run = self.next_ui_run(
            "accessibility-settings",
            ("WAIT_MS=100", "KEY_DOWN", "KEY_RETURN", "WAIT_MS=100", "KEY_RETURN",
             "WAIT_MS=100", "KEY_DOWN", "WAIT_MS=100"),
        )
        self.assertEqual(self.focus_trail(run),
                         [focus["interface_size"], focus["text_size"], focus["high_contrast"]],
                         "Text size is the second setting, and focus moves on past it")
        self.assertEqual([view for view, _ in self.screens(run)], ["accessibility_settings"])
        settings = run["settings"]
        assert isinstance(settings, dict)
        self.assertEqual(settings.get("text_size"), 150, "Standard steps to Large, then Larger")
        self.assertEqual(settings.get("interface_size"), 0,
                         "the interface size stays Automatic")

        wrapped = self.next_ui_run(
            "accessibility-settings",
            ("WAIT_MS=100", "KEY_DOWN", "KEY_RETURN", "KEY_RETURN", "KEY_RETURN",
             "WAIT_MS=100"),
        )
        settings = wrapped["settings"]
        assert isinstance(settings, dict)
        self.assertEqual(settings.get("text_size"), 100, "Larger steps back to Standard")

    def test_next_ui_settings_pages_open_and_return(self) -> None:
        """#1302: the online, notices, build tools and cache (#1306) and removal pages are
        next-UI cards too."""
        focus = self.SETTINGS_FOCUS
        online = self.next_ui_run(
            "settings",
            ("WAIT_MS=100",) + self.TO_SETTINGS_BUTTONS +
            ("KEY_LEFT", "KEY_RETURN", "WAIT_MS=100", "KEY_RETURN", "KEY_ESCAPE", "WAIT_MS=100"),
        )
        self.assertEqual([view for view, _ in self.screens(online)],
                         ["settings", "online_settings", "settings"])
        trail = self.focus_trail(online)
        self.assertEqual(trail[-3:], [focus["online"], focus["developer"], focus["online"]],
                         "the page opens on its one switch and Back focuses its button")
        frames = online["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["developer_mode"], "1", "the page's switch is developer mode")
        self.assertEqual(frames[-1]["online_mode"], "0",
                         "online mode has no switch until the feature exists (#2007)")

        notices = self.next_ui_run(
            "settings", ("WAIT_MS=100",) + self.TO_SETTINGS_BUTTONS + ("KEY_RETURN", "WAIT_MS=100"),
        )
        self.assertEqual([view for view, _ in self.screens(notices)],
                         ["settings", "prereq_about"])
        self.assertEqual(self.focus_trail(notices)[-1], focus["back"])

        # Build tools and cache opens on its switch; Tab passes Build speed and the
        # clear button and reaches Remove build tools. The Tabs wait for the page's
        # laid-out frame: on its first frame nk_ui holds a Tab one frame and runs it
        # with the next, so Build speed would be focused in no frame the trail reads
        # (a slow CI runner's WAIT_MS=100 ended before that second frame).
        # Right comes straight after the removal card opens, before its buttons are
        # laid out; nk_ui holds it a frame instead of losing it (a slow CI runner did).
        to_removal = ("KEY_RIGHT", "KEY_RETURN", "WAIT_VIEW=build_cache,5000",
                      "WAIT_LAID_OUT=5000", "KEY_TAB", "KEY_TAB", "KEY_TAB", "KEY_RETURN")
        removal = self.next_ui_run(
            "settings",
            ("WAIT_MS=100",) + self.TO_SETTINGS_BUTTONS + to_removal +
            ("WAIT_MS=1", "KEY_RIGHT", "KEY_RETURN"),
            downloaded_tool_payload=True,
        )
        self.assertEqual([view for view, _ in self.screens(removal)],
                         ["settings", "build_cache", "confirm_remove_tools", "settings"])
        trail = self.focus_trail(removal)
        self.assertIn(focus["build_cache"], trail)
        self.assertIn(focus["share_libraries"], trail, "the page opens on its switch")
        self.assertIn(focus["build_speed"], trail, "Build speed is the next stop")
        self.assertIn(focus["remove_tools"], trail)
        self.assertIn(focus["confirm_remove"], trail)
        self.assertFalse(removal["downloaded_tool_marker_exists"])

        kept = self.next_ui_run(
            "settings",
            ("WAIT_MS=100",) + self.TO_SETTINGS_BUTTONS + to_removal +
            ("WAIT_MS=100", "KEY_RETURN", "WAIT_MS=100"),
            downloaded_tool_payload=True,
        )
        self.assertTrue(kept["downloaded_tool_marker_exists"],
                        "the removal card opens on the choice that keeps the tools")
        self.assertEqual([view for view, _ in self.screens(kept)],
                         ["settings", "build_cache", "confirm_remove_tools", "build_cache"],
                         "keeping the tools returns to build tools and cache")
        self.assertEqual(self.focus_trail(kept)[-1], focus["remove_tools"],
                         "and focuses the button that opened the removal card")

        shared = self.next_ui_run(
            "settings",
            ("WAIT_MS=100",) + self.TO_SETTINGS_BUTTONS +
            ("KEY_RIGHT", "KEY_RETURN", "WAIT_MS=100", "KEY_RETURN", "KEY_ESCAPE", "WAIT_MS=100"),
        )
        self.assertEqual([view for view, _ in self.screens(shared)],
                         ["settings", "build_cache", "settings"])
        self.assertEqual(self.focus_trail(shared)[-1], focus["build_cache"],
                         "Back focuses the button that opened the page")
        settings = shared["settings"]
        assert isinstance(settings, dict)
        self.assertIs(settings.get("share_system_libraries"), False,
                      "the switch turns library sharing off and saves it")

    # The next UI's controller page (player_ui_nk.h), as the frame line prints it.
    CONTROLLER_FOCUS = {
        "select": str(0x4600), "start": str(0x4601), "rtrigger": str(0x4607),
        "stick_up": str(0x460F), "stick": str(0x4626), "calibrate": str(0x4625),
        "back": str(0x4628), "scope": str(0x4629), "calibration_cancel": str(0x462B),
    }
    # From the first binding, two steps right reach the stick choice, and three down
    # from it Calibrate stick.
    TO_STICK = ("KEY_RIGHT", "KEY_RIGHT")
    TO_CALIBRATE = TO_STICK + ("KEY_DOWN", "KEY_DOWN", "KEY_DOWN")

    @staticmethod
    def saved_profile(run: dict[str, object]) -> dict[str, object]:
        """The input profile the run left on disk: each Controls change saves it."""
        assert run["profile_exists"], "the change was saved without a Save step"
        profile = json.loads(str(run["profile_text"]))
        assert isinstance(profile, dict)
        return profile

    @staticmethod
    def keys_of(profile: dict[str, object]) -> dict[str, str]:
        entries = profile["keyboard_bindings"]
        assert isinstance(entries, list)
        return {entry["control"]: entry["primary"] for entry in entries}

    def test_next_ui_controller_rebind_calibrate_save_and_back(self) -> None:
        """Controller settings in the next UI run the input_settings handlers.

        #1299: a button another control uses moves to the chosen one, and that control
        takes the chosen one's button, so the mapping never conflicts; every change is
        written to the input profile at once."""
        focus = self.CONTROLLER_FOCUS
        bind = self.next_ui_run(
            "controller", ("WAIT_MS=100", "KEY_DOWN", "KEY_RETURN", "PAD_SOUTH", "WAIT_MS=100"),
        )
        frames = bind["frames"]
        assert isinstance(frames, list)
        self.assertEqual(self.focus_trail(bind)[:2], [focus["select"], focus["start"]],
                         "the page opens on the first binding")
        self.assertIn("1", [frame["controller_capturing"] for frame in frames],
                      "choosing a binding waits for a button")
        self.assertEqual(frames[-1]["controller_capturing"], "0",
                         "the controller's button is taken, not routed to the page")
        self.assertEqual(frames[-1]["controller_conflicts"], "0", "a taken button swaps")
        self.assertEqual(frames[-1]["key_note"], "1", "the page names the control that changed")
        self.assertNotEqual(frames[0]["start_binding"], frames[-1]["start_binding"])
        buttons = {entry["control"]: entry["primary"]
                   for entry in self.saved_profile(bind)["psp_bindings"]}  # type: ignore[union-attr]
        self.assertEqual((buttons["start"], buttons["cross"]), ("south", "start"))

        stick = self.next_ui_run(
            "controller", ("WAIT_MS=100",) + self.TO_STICK + ("KEY_RETURN", "WAIT_MS=100"),
        )
        self.assertEqual(self.focus_trail(stick)[-1], focus["stick"])
        calibration = self.saved_profile(stick)["calibration"]
        assert isinstance(calibration, dict)
        self.assertEqual((calibration["analog_x"]["host_axis"], calibration["analog_y"]["host_axis"]),
                         ("rightx", "righty"), "the PSP stick follows the right stick")

        cancel = self.next_ui_run(
            "controller", ("WAIT_MS=100", "KEY_RETURN", "KEY_ESCAPE", "WAIT_MS=100"),
        )
        frames = cancel["frames"]
        assert isinstance(frames, list)
        self.assertIn("1", [frame["controller_capturing"] for frame in frames])
        self.assertEqual(frames[-1]["view"], "controller", "Esc while waiting only cancels")
        self.assertEqual(frames[-1]["controller_capturing"], "0")
        self.assertEqual(frames[-1]["select_binding"], frames[0]["select_binding"])

        calibrate = self.next_ui_run(
            "controller",
            ("WAIT_MS=100",) + self.TO_CALIBRATE +
            ("KEY_RETURN", "WAIT_MS=100", "KEY_ESCAPE", "WAIT_MS=100"),
        )
        frames = calibrate["frames"]
        assert isinstance(frames, list)
        self.assertIn("1", [frame["calibrating"] for frame in frames])
        self.assertEqual(frames[-1]["calibrating"], "0")
        self.assertEqual(frames[-1]["view"], "controller")
        self.assertEqual(self.focus_trail(calibrate)[-3:],
                         [focus["calibrate"], focus["calibration_cancel"], focus["calibrate"]],
                         "the rest step opens on Cancel and Esc returns to Calibrate stick")

        # Down from Calibrate stick reaches Back, at the right end of the button
        # row; the mapping choice is next to it and Reset to defaults is first.
        scope = self.next_ui_run(
            "controller",
            ("WAIT_MS=100",) + self.TO_CALIBRATE + ("KEY_DOWN", "KEY_LEFT", "KEY_RETURN",
                                                     "WAIT_MS=100"),
        )
        frames = scope["frames"]
        assert isinstance(frames, list)
        self.assertEqual(self.focus_trail(scope)[-3:],
                         [focus["calibrate"], focus["back"], focus["scope"]])
        self.assertEqual(frames[0]["input_scope"], "global")
        self.assertEqual(frames[-1]["input_scope"], "TEST00005")
        self.assertEqual(frames[-1]["input_titles"], "1")
        # The same choice again goes back to the global mapping.
        unscoped = self.next_ui_run(
            "controller",
            ("WAIT_MS=100",) + self.TO_CALIBRATE + ("KEY_DOWN", "KEY_LEFT", "KEY_RETURN",
                                                     "WAIT_MS=100", "KEY_RETURN", "WAIT_MS=100"),
        )
        frames = unscoped["frames"]
        assert isinstance(frames, list)
        self.assertIn("TEST00005", [frame["input_scope"] for frame in frames])
        self.assertEqual(frames[-1]["input_scope"], "global")
        self.assertEqual(frames[-1]["input_titles"], "0")

        fallback = self.next_ui_run("controller", ("WAIT_MS=100",), invalid_profile=True)
        frames = fallback["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["profile_fallback"], "1",
                         "an unreadable profile is reported, and the defaults are used")

        back = self.next_ui_run(
            "controller",
            ("WAIT_MS=100",) + self.TO_CALIBRATE + ("KEY_DOWN", "KEY_RETURN", "WAIT_MS=100"),
        )
        self.assertEqual([view for view, _ in self.screens(back)], ["controller", "settings"])
        self.assertEqual(self.focus_trail(back)[-1], self.SETTINGS_FOCUS["controller"],
                         "Back returns to Settings on Controls")

        # A profile that cannot be written (its folder is a file): the binding's write
        # fails and says so, the first Back keeps the page to show it, and Back again
        # leaves without saving.
        unwritable = {"NK_INPUT_PROFILE": str(PLAYER_EXE / "input_profile.json")}
        rebind_and_back = (("WAIT_MS=100", "KEY_DOWN", "KEY_RETURN", "PAD_SOUTH", "WAIT_MS=100",
                            "KEY_UP") + self.TO_CALIBRATE + ("KEY_DOWN", "KEY_RETURN", "WAIT_MS=100"))
        stay = self.next_ui_run("controller", rebind_and_back, env_extra=unwritable)
        frames = stay["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["profile_save_notice"], "1", "the failed write is shown")
        self.assertEqual(frames[-1]["view"], "controller", "the first Back stays to show it")
        leave = self.next_ui_run("controller", rebind_and_back + ("KEY_RETURN", "WAIT_MS=100"),
                                 env_extra=unwritable)
        self.assertEqual([view for view, _ in self.screens(leave)], ["controller", "settings"],
                         "Back again leaves without saving")

    def test_next_ui_keyboard_keys_rebind_refuse_reset_and_save(self) -> None:
        """#1299: KEYBOARD shows the keys; a chosen button takes the next key pressed,
        a key another control uses moves to it and that control takes its old key,
        the stick's four keys rebind the same way, Reset to defaults resets the keys,
        and every change is written to the profile the game is handed."""
        # Up from the first binding is KEYBOARD; Down twice from it is Start's key.
        to_start = ("WAIT_MS=100", "KEY_UP", "KEY_RETURN", "WAIT_MS=100", "KEY_DOWN", "KEY_DOWN")
        # Down from L Trigger, the last row of the first column, then Left, is Reset to
        # defaults, the first of the buttons under the cards.
        to_reset = ("KEY_DOWN",) * 6 + ("KEY_LEFT",)
        save = self.next_ui_run(
            "controller", to_start + ("KEY_RETURN", "KEY_C", "WAIT_MS=100"),
        )
        frames = save["frames"]
        assert isinstance(frames, list)
        # Under load the two Downs can land in one drawn frame (input waits for the
        # new page's layout), so Select may not be drawn focused on the way.
        trail = self.focus_trail(save)
        self.assertEqual(trail[1], str(0x462D), "Up from the first binding is KEYBOARD")
        on_the_way = trail[2:] if trail[2:3] != [str(0x4600)] else trail[3:]
        self.assertEqual(on_the_way[:1], [str(0x4601)], "two Downs from KEYBOARD reach Start")
        self.assertEqual(frames[0]["input_device"], "controller")
        self.assertEqual(frames[-1]["input_device"], "keyboard")
        self.assertIn("1", [frame["controller_capturing"] for frame in frames],
                      "choosing Start's key waits for a key")
        self.assertEqual((frames[0]["start_key"], frames[-1]["start_key"]), ("40", "6"),
                         "the C key pressed is Start's key, not routed to the page")
        self.assertEqual(self.keys_of(self.saved_profile(save))["start"], "c")

        swap = self.next_ui_run(
            "controller",
            to_start + ("KEY_RETURN", "KEY_X", "WAIT_MS=100", "KEY_RETURN", "PAD_SOUTH",
                        "WAIT_MS=100"),
        )
        frames = swap["frames"]
        assert isinstance(frames, list)
        noted = [frame for frame in frames if frame["key_note"] == "1"]
        self.assertTrue(noted, "X, Cross's key, moves to Start with a note naming Cross")
        self.assertEqual({frame["start_key"] for frame in noted}, {"27"})
        self.assertEqual(frames[-1]["controller_capturing"], "0",
                         "a controller press stops the wait for a key")
        keys = self.keys_of(self.saved_profile(swap))
        self.assertEqual((keys["start"], keys["cross"]), ("x", "return"),
                         "Cross takes Start's old key: nothing is left without one")

        stick = self.next_ui_run(
            "controller",
            ("WAIT_MS=100", "KEY_UP", "KEY_RETURN", "WAIT_MS=100", "KEY_DOWN", "KEY_RIGHT",
             "KEY_RIGHT", "KEY_RETURN", "KEY_W", "WAIT_MS=100"),
        )
        focus = self.CONTROLLER_FOCUS
        trail = self.focus_trail(stick)
        self.assertEqual(trail[-1], focus["stick_up"],
                         "the stick's keys sit beside the buttons, on the stick card")
        self.assertLess(trail.index(focus["select"]), len(trail) - 1,
                        "reached from the buttons: Down, then Right twice")
        keys = self.keys_of(self.saved_profile(stick))
        self.assertEqual((keys["stick_up"], keys["rtrigger"]), ("w", "i"),
                         "the stick's up key is W now, and R takes I")

        reset = self.next_ui_run(
            "controller",
            to_start + ("KEY_RETURN", "KEY_C", "WAIT_MS=100") + to_reset +
            ("KEY_RETURN", "WAIT_MS=100"),
        )
        frames = reset["frames"]
        assert isinstance(frames, list)
        self.assertIn("6", [frame["start_key"] for frame in frames])
        self.assertEqual(frames[-1]["start_key"], "40", "Reset to defaults resets the keys")
        self.assertEqual(frames[-1]["start_binding"], frames[0]["start_binding"])
        self.assertEqual(frames[-1]["view"], "controller")
        self.assertEqual(self.keys_of(self.saved_profile(reset))["start"], "return",
                         "the reset is saved too")

    def test_next_ui_settings_opens_controls_and_esc_returns(self) -> None:
        """Settings > Controls opens the next UI's controller page; Esc comes back.

        Right from Scaling, the last GAME row, is Controls beside it: a settings
        row keeps Left and Right in its row (it reached the button row before)."""
        focus = self.SETTINGS_FOCUS
        run = self.next_ui_run(
            "settings",
            ("WAIT_MS=100",) + ("KEY_DOWN",) * 4 +
            ("KEY_RIGHT", "KEY_RETURN", "WAIT_MS=100", "KEY_ESCAPE", "WAIT_MS=100"),
        )
        self.assertEqual([view for view, _ in self.screens(run)],
                         ["settings", "controller", "settings"])
        self.assertEqual(self.focus_trail(run)[-4:],
                         [focus["scaling"], focus["controller"], self.CONTROLLER_FOCUS["select"],
                          focus["controller"]])
        pacing = self.next_ui_run("settings", ("WAIT_MS=100", "KEY_DOWN", "KEY_RIGHT"))
        self.assertEqual(self.focus_trail(pacing)[-1], focus["launcher_fullscreen"],
                         "Right from Frame pacing is Launcher fullscreen (F11) beside it, not "
                         "the top bar")
        mouse = self.next_ui_run(
            "settings", ("WAIT_MS=100", "MOUSE_MOVE=1010,392", "MOUSE_DOWN", "MOUSE_UP",
                         "WAIT_MS=100"),
        )
        self.assertEqual([view for view, _ in self.screens(mouse)], ["settings", "controller"])

    def test_next_ui_developer_log_buttons(self) -> None:
        """The developer log opens on Close; Copy and the layout inspector keep it open."""
        closed = self.next_ui_run("developer-log", ("WAIT_MS=100", "KEY_RETURN", "WAIT_MS=100"))
        self.assertEqual(self.focus_trail(closed)[0], str(0x4703))
        self.assertEqual([view for view, _ in self.screens(closed)], ["developer_log", "library"])
        inspected = self.next_ui_run(
            "developer-log", ("WAIT_MS=100", "KEY_LEFT", "KEY_RETURN", "WAIT_MS=100"),
        )
        self.assertEqual(self.focus_trail(inspected)[-1], str(0x4704))
        self.assertEqual([view for view, _ in self.screens(inspected)], ["developer_log"])
        copied = self.next_ui_run(
            "developer-log",
            ("WAIT_MS=100", "KEY_LEFT", "KEY_LEFT", "KEY_LEFT", "KEY_RETURN", "WAIT_MS=100"),
        )
        self.assertEqual(self.focus_trail(copied)[-1], str(0x4701))
        self.assertEqual([view for view, _ in self.screens(copied)], ["developer_log"])

    def test_next_ui_title_detail_and_mapping_follow_the_chosen_title(self) -> None:
        """The second card's Title detail, and the game's own mapping chosen from
        there, are the second title's (#1301: a frame without an action no
        longer reselects the first title)."""
        run = self.next_ui_run(
            "library",
            ("WAIT_MS=100", "KEY_RIGHT", "WAIT_MS=100", "KEY_RETURN", "WAIT_MS=200",
             "MOUSE_MOVE=483,31", "MOUSE_DOWN", "MOUSE_UP", "WAIT_MS=200",
             "MOUSE_MOVE=1010,392", "MOUSE_DOWN", "MOUSE_UP", "WAIT_MS=200") +
            self.TO_CALIBRATE + ("KEY_DOWN", "KEY_LEFT", "KEY_RETURN", "WAIT_MS=200"),
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        title = [frame for frame in frames if frame["next_route"] == "2"]
        self.assertTrue(title, "the Title detail page opened")
        self.assertEqual({frame["selected_disc"] for frame in title}, {"TEST00006"})
        self.assertEqual([view for view, _ in self.screens(run)][-1], "controller")
        self.assertEqual(frames[-1]["input_scope"], "TEST00006")

    def test_build_package_waits_for_title_preflight(self) -> None:
        """#1286: the title page must catch build refusals before BUILD PACKAGE."""
        for fixture in ("encrypted", "missing-manifest"):
            with self.subTest(fixture=fixture):
                run = self.run_player(
                    f"build-preflight-{fixture}",
                    ("WAIT_MS=100", "KEY_RETURN", "WAIT_MS=200"),
                    build_preflight=fixture,
                )
                if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in run["stdout"]:
                    self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
                frames = run["frames"]
                assert isinstance(frames, list)
                title_frames = [frame for frame in frames if frame["next_route"] == "2"]
                self.assertTrue(title_frames, run["stdout"])
                self.assertEqual(
                    title_frames[-1]["next_primary_label"],
                    "disabled:BUILD_PACKAGE",
                    run["stdout"],
                )
                self.assertEqual(title_frames[-1]["package_building"], "0")
                self.assertEqual(
                    title_frames[-1]["next_preflight_action"], "OPEN_FOLDER"
                )
                if fixture == "encrypted":
                    self.assertIn("decrypted", title_frames[-1]["next_preflight"].lower())
                    self.assertEqual(title_frames[-1]["next_preflight_code"], "EXECUTABLE")
                    self.assertIn("ENCRYPTED_EXECUTABLE",
                                  title_frames[-1]["next_preflight_detail"])
                    # #308: the title page knows the program input and the
                    # one file the build waits for.
                    self.assertEqual(title_frames[-1]["next_input_class"], "encrypted-retail")
                    self.assertEqual(title_frames[-1]["next_input_files"], "1")
                else:
                    self.assertIn("title_settings", title_frames[-1]["next_preflight"].lower())
                    self.assertEqual(title_frames[-1]["next_preflight_code"], "TITLE_PROFILE")
                    self.assertIn("TITLE_PROFILE_", title_frames[-1]["next_preflight_detail"])
                    self.assertEqual(title_frames[-1]["next_input_class"], "plain-elf")
                    self.assertEqual(title_frames[-1]["next_input_files"], "0")

    # CHECK FOR UPDATES, wait for the newer release, then Enter on its focused action
    # (Download, or the release page when Download is not offered).
    UPDATE_TO_DOWNLOAD = ("WAIT_MS=200", "KEY_RETURN",
                          f"WAIT_UPDATE=available,{UPDATE_STEP_TIMEOUT_MS}", "KEY_RETURN")

    def update_run(self, view: str, events: tuple[str, ...], fixture: dict[str, bool] | None,
                   **kwargs: object) -> tuple[dict[str, object], list[dict[str, str]], list[str]]:
        """A next-UI run whose update answers come from a synthetic fixture (None:
        an empty folder, so every request is offline)."""
        with tempfile.TemporaryDirectory(prefix=".update-fixture-", dir=ROOT) as tmp:
            folder = Path(tmp)
            if fixture is not None:
                write_update_fixture(folder, **fixture)
            env = dict(kwargs.pop("env_extra", None) or {})  # type: ignore[arg-type]
            env["NK_UI_TEST_UPDATE_DIR"] = str(folder)
            run = self.next_ui_run(view, events, env_extra=env, **kwargs)
        stdout = run["stdout"]
        assert isinstance(stdout, str)
        steps, requests = update_report(stdout)
        for url in requests:
            self.assertTrue(
                url == UPDATE_RELEASES_URL or url.startswith(UPDATE_DOWNLOAD_PREFIX),
                f"an update request left this repository's releases: {url}")
        return run, steps, requests

    def test_update_check_finds_downloads_and_installs_a_newer_release(self) -> None:
        """Owner request 2026-10-28: CHECK FOR UPDATES asks GitHub once; Download and
        Install each need their own click; the checksum file's signature is checked,
        then the ZIP against that signed SHA-256, before Install is offered, and
        Install unpacks into a new folder. Each step is waited for by its phase: a
        fixed wait once ended before the install did on a busy runner (CI run
        42312214991). The slow link (600 ms per download request, so the download
        outlasts the 1.5 s the old script gave it) keeps the script honest."""
        events = self.UPDATE_TO_DOWNLOAD
        if sys.platform == "win32":
            events += (f"WAIT_UPDATE=downloaded,{UPDATE_STEP_TIMEOUT_MS}", "KEY_RETURN",
                       f"WAIT_UPDATE=installed,{UPDATE_STEP_TIMEOUT_MS}")
        else:
            events += ("WAIT_MS=500",)
        run, steps, requests = self.update_run(
            "updates", events, {}, env_extra={"NK_UI_TEST_UPDATE_REQUEST_DELAY_MS": "600"})
        names = [step["update"] for step in steps]
        self.assertEqual(steps[0]["newest"], f"v{UPDATE_NEWER_VERSION}")
        self.assertIn("available", names)
        self.assertEqual(requests[0], UPDATE_RELEASES_URL)
        if sys.platform != "win32":
            # No native download or SHA-256 check off Windows: the release page only.
            self.assertEqual(names, ["available"], steps)
            self.assertEqual(len(requests), 1)
            self.assertIn(f"open_url=https://github.com/Jstar269/nakagawa-recomp/releases/tag/v{UPDATE_NEWER_VERSION}",
                          run["stdout"])
            return
        self.assertEqual(names, ["available", "downloaded", "installed"], steps)
        self.assertEqual(
            requests[1:],
            [UPDATE_DOWNLOAD_PREFIX + f"v{UPDATE_NEWER_VERSION}/nakagawa-recomp-{UPDATE_NEWER_VERSION}-windows-x64.zip.sha256",
             UPDATE_DOWNLOAD_PREFIX + f"v{UPDATE_NEWER_VERSION}/nakagawa-recomp-{UPDATE_NEWER_VERSION}-windows-x64.zip.sha256.minisig",
             UPDATE_DOWNLOAD_PREFIX + f"v{UPDATE_NEWER_VERSION}/nakagawa-recomp-{UPDATE_NEWER_VERSION}-windows-x64.zip"],
            "the checksum file and its signature first, then the ZIP, each once")
        installed = steps[-1]
        # The player writes its own separator, not the one of the Python running this test
        # (MSYS2's python has os.sep "/" on Windows), so compare the path's last components.
        self.assertEqual(
            re.split(r"[\\/]", installed["zip"])[-4:],
            ["Nakagawa", "data", "downloads", f"nakagawa-recomp-{UPDATE_NEWER_VERSION}-windows-x64.zip"],
            installed)
        self.assertTrue(installed["install"].endswith(f"nakagawa-recomp-{UPDATE_NEWER_VERSION}-windows-x64"), installed)
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["view"], "updates")

    @staticmethod
    def problem_report_steps(stdout: str) -> tuple[list[dict[str, str]], list[str]]:
        """The Problem report phases the player settled on, and the file names it listed."""
        steps = []
        names = []
        for line in stdout.splitlines():
            if line.startswith("[PLAYER_UI_TEST] problem_report="):
                steps.append(dict(re.findall(r"([a-z_]+)=(\S+)", line)))
            elif line.startswith("[PLAYER_UI_TEST] problem_report_file="):
                names.append(line.split("=", 1)[1])
        return steps, names

    def test_problem_report_page_previews_then_writes_one_local_bundle(self) -> None:
        """#1849: the Problem report page lists every file with a size, Create writes the
        ZIP on this PC, and the page then shows where it went."""
        # The GPU probe in the report tool takes up to ~10 s on a hosted runner, so the waits are long.
        # The report tool refuses a data folder inside the repository, where the
        # sandbox lies, so the regression build writes under a folder outside it.
        with tempfile.TemporaryDirectory(prefix="nk-report-test-") as tmp:
            env = {"NK_UI_TEST_REPORT_DATA_ROOT": tmp}
            # Wait for the previewed phase (2), press Create, then wait for done (4); 6 is
            # the no-Python page, which ends either wait and skips the test below.
            run = self.next_ui_run(
                "problem-report",
                ("WAIT_REPORT=2|6,60000", "KEY_RETURN", "WAIT_REPORT=4|6,150000"),
                env_extra=env)
            stdout = run["stdout"]
            assert isinstance(stdout, str)
            steps, names = self.problem_report_steps(stdout)
            phases = [int(step["problem_report"]) for step in steps]
            if 6 in phases:
                self.skipTest("no Python interpreter on this host, so the page shows the command")
            # 1 is listing, 2 previewed, 4 done (PlayerReportPhase).
            self.assertEqual(phases[:1], [1], stdout)
            self.assertIn(2, phases, stdout)
            self.assertEqual(phases[-1], 4, stdout)
            self.assertIn("about.txt", names)
            written = Path(steps[-1]["path"])
            self.assertEqual(written.parent, Path(tmp) / "reports", "nothing leaves this PC")
            with zipfile.ZipFile(written) as bundle:
                self.assertIn("about.txt", bundle.namelist())
                self.assertEqual(sorted(bundle.namelist()), sorted(names[-len(bundle.namelist()):]))
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["view"], "problem_report")

    def test_problem_report_that_cannot_start_its_worker_says_so_on_the_page(self) -> None:
        """#1849: if the report thread cannot start, the page shows a failed phase with the
        command to run by hand, never an empty page where Create does nothing."""
        with tempfile.TemporaryDirectory(prefix="nk-report-test-") as tmp:
            env = {"NK_UI_TEST_REPORT_DATA_ROOT": tmp, "NK_UI_TEST_FAIL_REPORT_THREAD_CREATE": "1"}
            run = self.next_ui_run(
                "problem-report", ("WAIT_REPORT=5,10000",), env_extra=env)
        stdout = run["stdout"]
        assert isinstance(stdout, str)
        steps, _ = self.problem_report_steps(stdout)
        self.assertEqual([int(step["problem_report"]) for step in steps][-1:], [5], stdout)
        self.assertIn("[PLAYER_UI_TEST] problem_report_message=The report could not be started.",
                      stdout)
        self.assertIn("nk_cli.py report", stdout)

    def test_update_check_offline_shows_one_plain_card_and_no_retries(self) -> None:
        run, steps, requests = self.update_run(
            "updates",
            ("WAIT_MS=200", "KEY_RETURN", f"WAIT_UPDATE=failed,{UPDATE_STEP_TIMEOUT_MS}"), None)
        self.assertEqual([step["update"] for step in steps], ["failed"], steps)
        self.assertEqual(steps[0]["error"], "UPDATE_OFFLINE")
        self.assertEqual(requests, [UPDATE_RELEASES_URL], "one request, no retry")
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["view"], "updates")

    def test_update_download_that_does_not_match_its_checksum_is_refused(self) -> None:
        if sys.platform != "win32":
            self.skipTest("the update download runs on Windows only")
        _, steps, _ = self.update_run(
            "updates", self.UPDATE_TO_DOWNLOAD + (f"WAIT_UPDATE=failed,{UPDATE_STEP_TIMEOUT_MS}",),
            {"wrong_checksum": True})
        self.assertEqual(steps[-1]["update"], "failed", steps)
        self.assertEqual(steps[-1]["error"], "UPDATE_HASH_MISMATCH")
        self.assertEqual(steps[-1]["zip"], "none", "nothing is kept or offered to install")

    def test_update_download_with_a_bad_signature_is_refused(self) -> None:
        """Owner request (v0.0.2): the checksum is trusted only under a valid signature
        from the player's key; a bad one stops before the ZIP is asked for."""
        if sys.platform != "win32":
            self.skipTest("the update download runs on Windows only")
        _, steps, requests = self.update_run(
            "updates", self.UPDATE_TO_DOWNLOAD + (f"WAIT_UPDATE=failed,{UPDATE_STEP_TIMEOUT_MS}",),
            {"bad_signature": True})
        self.assertEqual(steps[-1]["update"], "failed", steps)
        self.assertEqual(steps[-1]["error"], "UPDATE_SIGNATURE_INVALID")
        self.assertEqual(steps[-1]["zip"], "none", "nothing is kept or offered to install")
        self.assertEqual(
            requests[1:],
            [UPDATE_DOWNLOAD_PREFIX + f"v{UPDATE_NEWER_VERSION}/nakagawa-recomp-{UPDATE_NEWER_VERSION}-windows-x64.zip.sha256",
             UPDATE_DOWNLOAD_PREFIX + f"v{UPDATE_NEWER_VERSION}/nakagawa-recomp-{UPDATE_NEWER_VERSION}-windows-x64.zip.sha256.minisig"],
            "the ZIP is never requested")

    def test_update_without_a_signing_key_or_signature_offers_only_the_release_page(self) -> None:
        """A build with no update-signing key, or a release with no signature, still
        shows the newer version, but Download is not offered: Enter opens the release
        page, and nothing past the release list is requested."""
        for fixture in ({"player_key": False}, {"signed": False}):
            with self.subTest(fixture=fixture):
                run, steps, requests = self.update_run(
                    "updates", self.UPDATE_TO_DOWNLOAD + ("WAIT_MS=500",), fixture)
                self.assertEqual([step["update"] for step in steps], ["available"], steps)
                self.assertEqual(requests, [UPDATE_RELEASES_URL])
                self.assertIn(
                    f"open_url=https://github.com/Jstar269/nakagawa-recomp/releases/tag/v{UPDATE_NEWER_VERSION}",
                    run["stdout"])

    def test_update_install_that_loses_a_listed_file_is_refused(self) -> None:
        if sys.platform != "win32":
            self.skipTest("the update install runs on Windows only")
        _, steps, _ = self.update_run(
            "updates",
            self.UPDATE_TO_DOWNLOAD + (f"WAIT_UPDATE=downloaded,{UPDATE_STEP_TIMEOUT_MS}",
                                       "KEY_RETURN", f"WAIT_UPDATE=failed,{UPDATE_STEP_TIMEOUT_MS}"),
            {"missing_file": True})
        self.assertEqual(steps[-1]["update"], "failed", steps)
        self.assertEqual(steps[-1]["error"], "UPDATE_INSTALL_INCOMPLETE")
        self.assertEqual(steps[-1]["install"], "none")

    def test_wait_update_fails_on_timeout_and_refuses_bad_steps(self) -> None:
        """WAIT_UPDATE is a ceiling, never a pass: a phase that never comes fails the run
        and names the phase it last saw, and a malformed step stops the script before
        any wait starts."""
        run = self.next_ui_run("library", ("WAIT_UPDATE=installed,300",),
                               expect_failure=True, timeout=60)
        self.assertIn("[PLAYER_UI_TEST] wait_update result=FAIL target=installed actual=idle",
                      run["stderr"], run["stdout"])
        self.assertNotIn("result=PASS", run["stdout"])
        for step in ("WAIT_UPDATE=nonsense,1000", "WAIT_UPDATE=installed",
                     "WAIT_UPDATE=,1000", "WAIT_UPDATE=installed,0",
                     "WAIT_UPDATE=installed,-5", "WAIT_UPDATE=installed,4294967296"):
            with self.subTest(step=step):
                run = self.next_ui_run("library", (step,), expect_failure=True, timeout=60)
                self.assertIn("Invalid synthetic event script", run["stderr"], run["stdout"])
                self.assertNotIn("wait_update target=", run["stdout"])

    def test_update_check_at_start_is_off_by_default_and_quiet_when_offline(self) -> None:
        """No setting, no request: the player is offline by default (AGENTS.md section 5)."""
        run, steps, requests = self.update_run("library", ("WAIT_MS=800",), {})
        self.assertEqual((steps, requests), ([], []))
        settings = run["settings"]
        if isinstance(settings, dict):
            self.assertIs(settings.get("check_updates_at_start"), False)
        # Turned on: one request at start, and a newer version is one quiet button.
        run, steps, requests = self.update_run(
            "library", (f"WAIT_UPDATE=available,{UPDATE_STEP_TIMEOUT_MS}",), {},
            env_extra={"NK_UI_TEST_CHECK_AT_START": "1"})
        self.assertEqual(requests, [UPDATE_RELEASES_URL])
        self.assertEqual([(step["update"], step["notice"]) for step in steps],
                         [("available", "1")], steps)
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["view"], "library", "the check never takes the screen")
        # Offline at start: one request, and nothing on screen. The check starts before
        # the first frame, so the phase is checking until the failed check returns it to idle.
        run, steps, requests = self.update_run(
            "library", (f"WAIT_UPDATE=idle,{UPDATE_STEP_TIMEOUT_MS}",), None,
            env_extra={"NK_UI_TEST_CHECK_AT_START": "1"})
        self.assertEqual(requests, [UPDATE_RELEASES_URL])
        self.assertEqual(steps, [], "a failed check at start stays in the developer log")

    def test_update_page_switch_saves_and_escape_returns_to_settings(self) -> None:
        run, steps, requests = self.update_run(
            "updates", ("WAIT_MS=200", "KEY_UP", "KEY_RETURN", "WAIT_MS=200", "KEY_ESCAPE",
                        "WAIT_MS=200"), {})
        self.assertEqual((steps, requests), ([], []), "the switch asks GitHub nothing")
        settings = run["settings"]
        assert isinstance(settings, dict)
        self.assertIs(settings.get("check_updates_at_start"), True)
        self.assertEqual([view for view, _ in self.screens(run)], ["updates", "settings"])

    def test_script_can_wait_for_a_view_and_a_minimum_duration(self) -> None:
        started = time.monotonic()
        run = self.run_player(
            "settings",
            (
                "KEY_ESCAPE",
                "WAIT_VIEW=library|settings,1000",
                "WAIT_MS=100",
                "ASSERT_VIEW=library",
                "KEY_S",
            ),
        )
        elapsed = time.monotonic() - started
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertGreaterEqual(elapsed, 0.08)
        self.assertIn("[PLAYER_UI_TEST] wait_view actual=library result=PASS", run["stdout"])
        self.assertIn("[PLAYER_UI_TEST] wait_ms result=PASS", run["stdout"])
        self.assertIn("[PLAYER_UI_TEST] assert_view result=PASS actual=library", run["stdout"])
        self.assertEqual(frames[-1]["view"], "settings")

    def test_library_card_states_include_experimental_and_ready(self) -> None:
        # The ordinary library auto-discovers built showcase packages beside
        # the test executable. Use the in-memory staged-but-unbuilt card so the
        # missing-runtime state is stable whether or not `make showcase` ran.
        missing = self.run_player("ready-library", (), runtime_ready=False)
        missing_frame = missing["frames"][0]  # type: ignore[index]
        self.assertEqual(missing_frame["selected_prepared"], "0")
        self.assertEqual(missing_frame["selected_package_status"], "1")

        experimental = self.run_player("experimental-library", ())
        experimental_frame = experimental["frames"][0]  # type: ignore[index]
        self.assertEqual(experimental_frame["selected_experimental"], "1")

        ready = self.run_player(
            "ready", ("KEY_LEFT",) * 4, runtime_ready=True, wait_background=True
        )
        ready_frames = ready["frames"]
        assert isinstance(ready_frames, list)
        ready_frame = ready_frames[-1]
        self.assertEqual(ready_frame["selected_runtime"], "1")
        self.assertEqual(ready_frame["selected_prepared"], "0")
        self.assertNotEqual(missing_frame["pixels"], ready_frame["pixels"])

    def test_valid_package_with_missing_data_root_is_not_marked_ready(self) -> None:
        run = self.run_player(
            "ready", (), post_build_ready=True, data_root_present=False,
            wait_background=True,
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        ready_package_frames = [
            frame for frame in frames if frame["selected_package_status"] == "0"
        ]
        self.assertTrue(ready_package_frames, run["stdout"])
        self.assertTrue(
            all(frame["selected_runtime"] == "0" for frame in ready_package_frames),
            run["stdout"],
        )
        self.assertTrue(
            all(frame["selected_prepared"] == "0" for frame in ready_package_frames),
            run["stdout"],
        )

    def test_refused_local_title_profile_is_shown_on_the_game_card(self) -> None:
        normal = self.run_player("supported")
        refused = self.run_player("supported", profile_refusal=True)
        normal_frames = normal["frames"]
        refused_frames = refused["frames"]
        assert isinstance(normal_frames, list) and isinstance(refused_frames, list)
        self.assertEqual(refused_frames[0]["view"], "supported")
        self.assertIn("vblank_frame_counter_addr", refused["stderr"])
        self.assertNotEqual(normal_frames[0]["pixels"], refused_frames[0]["pixels"])

    def test_one_title_offline_art_and_stale_package_stay_off_frame_path(self) -> None:
        # The counters are the load-bearing gate: package validation and ISO
        # reads must never run on the UI thread, on any frame.
        # `package_validations` counts the UI-thread wrapper, so it is a proxy
        # for the whole validation path: a regression that inlined the core
        # call on the frame path would still leave it at zero, and only a
        # sanitizer or an interactive profile would see that. The wall-clock
        # bounds below are deliberately generous because the headless
        # software renderer, not the product's GPU path, sets the absolute
        # per-frame floor here; they only reject the maintainer-reported shape
        # (a first frame of seconds, steady frames of hundreds of ms).
        events = ("KEY_LEFT",) * 24
        offline = self.next_ui_run(
            "ready",
            events,
            art_iso="unavailable",
            stale_package=True,
            wait_background=True,
        )
        frames = offline["frames"]
        assert isinstance(frames, list)
        self.assertGreaterEqual(len(frames), 20)
        validation_calls = [int(frame["package_validations"]) for frame in frames]
        self.assertEqual(validation_calls, [0] * len(frames))
        elapsed = [int(frame["render_ns"]) for frame in frames]
        self.assertLess(elapsed[0], 1_000_000_000)
        self.assertLess(max(elapsed[1:]), 250_000_000)
        self.assertLess(sorted(elapsed[1:])[len(elapsed[1:]) // 2], 60_000_000)
        # Coarse end-to-end guard over process start through the last frame:
        # the maintainer-reported 3 fps interactive loop would need ~9s here.
        self.assertLess(offline["wall_seconds"], 6.0)
        self.assertEqual(frames[-1]["selected_package_status"], "3")
        self.assertEqual(frames[-1]["selected_runtime"], "0")
        # An absent disc image must show the placeholder, never a blank or a
        # blocked frame waiting for the drive.
        self.assertNotIn("next_ui_disc_icon", offline["stdout"])

        available = self.next_ui_run(
            "ready",
            # The launcher reads a card's art in the background once it is shown.
            ("KEY_LEFT",) * 8 + ("WAIT_MS=1500",),
            art_iso="available",
            stale_package=True,
            wait_background=True,
        )
        art_frames = available["frames"]
        assert isinstance(art_frames, list)
        self.assertEqual(
            [int(frame["package_validations"]) for frame in art_frames],
            [0] * len(art_frames),
        )
        self.assertEqual(art_frames[-1]["selected_package_status"], "3")
        # Synthetic icon art arrives without blocking the UI frame.
        self.assertIn("[PLAYER_UI_TEST] next_ui_disc_icon index=0 width=1 height=1",
                      available["stdout"])

    # An upgrade can leave a built package the validator no longer accepts. The
    # card then offers REBUILD PACKAGE (#1147) with the same consent, progress
    # and restore as a first build, and says in one line why.
    INCOMPATIBLE_REASON = "Built_for_another_player_version,_or_its_package_file_is_unreadable."

    def assert_rebuild_started(self, frames: list, stdout: object) -> None:
        # The dead pill left the view on the library. REBUILD PACKAGE takes the
        # first-build path, which this host answers with the build screen, the
        # build tools consent, or (no build tools on Linux) the error card.
        self.assertIn(
            frames[-1]["view"],
            ("building_package", "prereq_consent", "prereq_progress", "error"),
            stdout,
        )

    # A stand-in for tools/nk_cli.py that builds nothing: it records that it
    # started, then writes a heartbeat until it is stopped or 8 s pass.
    SLOW_BUILD_CLI = (
        "import os, pathlib, time\n"
        "witness = pathlib.Path(os.environ['NK_UI_TEST_BUILD_WITNESS'])\n"
        "(witness / 'started').write_text(str(os.getpid()), encoding='utf-8')\n"
        "deadline = time.monotonic() + 8.0\n"
        "with open(witness / 'heartbeat', 'a', encoding='utf-8') as beat:\n"
        "    while time.monotonic() < deadline:\n"
        "        beat.write('.')\n"
        "        beat.flush()\n"
        "        time.sleep(0.05)\n"
        "(witness / 'finished').write_text('outlived the player', encoding='utf-8')\n"
    )

    def test_closing_the_player_stops_a_running_package_build(self) -> None:
        # The player used to exit with a package build still running. On Linux
        # the build ran on as an orphan and kept writing into the data folder,
        # which is how the old renderer's rebuild test failed hosted CI with
        # "Directory not empty" when its scratch folder was removed. A slow
        # stand-in build makes that certain.
        with tempfile.TemporaryDirectory(
            prefix=".slow-build-", dir=ROOT, ignore_cleanup_errors=True
        ) as tmp:
            install_root = Path(tmp) / "install"
            (install_root / "tools").mkdir(parents=True)
            (install_root / "tools" / "nk_cli.py").write_text(self.SLOW_BUILD_CLI,
                                                              encoding="utf-8")
            witness = Path(tmp) / "witness"
            witness.mkdir()
            run = self.next_ui_run(
                "ready",
                # Open the title, then its primary action, REBUILD PACKAGE (launcher review B6).
                ("WAIT_PACKAGE_STATUS=INCOMPATIBLE,10000", "KEY_RETURN", "WAIT_MS=300",
                 "KEY_RETURN", "WAIT_MS=1500"),
                incompatible_package=True,
                wait_background=True,
                env_extra={
                    "NK_INSTALL_ROOT": str(install_root),
                    "NK_UI_TEST_BUILD_WITNESS": str(witness),
                },
            )
            frames = run["frames"]
            assert isinstance(frames, list)
            if frames[-1]["view"] != "building_package":
                self.skipTest("no build started: this host lacks the build tools")
            self.assertTrue((witness / "started").exists(), run["stdout"])
            heartbeat = witness / "heartbeat"
            size_at_exit = heartbeat.stat().st_size
            time.sleep(1.0)
            self.assertEqual(heartbeat.stat().st_size, size_at_exit,
                             "the package build kept running after the player closed")
            self.assertFalse((witness / "finished").exists())
            # Windows ended the build with the player's job object even before
            # the fix; the player now stops it itself, and says so.
            self.assertIn("[PLAYER] Closing: the package build was stopped.", run["stderr"])

    def test_incompatible_title_offers_rebuild_package_in_the_next_ui(self) -> None:
        shown = self.run_player(
            "ready", ("WAIT_MS=100",), incompatible_package=True, wait_background=True,
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in shown["stdout"]:
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        frames = shown["frames"]
        assert isinstance(frames, list)
        last = frames[-1]
        self.assertEqual(last["selected_package_status"], "2", shown["stdout"])
        self.assertEqual(last["next_primary_label"], "REBUILD_PACKAGE")
        self.assertEqual(last["next_detail"], self.INCOMPATIBLE_REASON)
        run = self.run_player(
            "ready",
            # Open the title: its primary action has focus (launcher review B6).
            ("WAIT_MS=300", "KEY_RETURN", "WAIT_MS=300", "KEY_RETURN", "WAIT_MS=300"),
            incompatible_package=True, wait_background=True,
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assert_rebuild_started(frames, run["stdout"])

    def test_package_status_worker_and_catalog_reload_do_not_deadlock_or_corrupt_status(
        self,
    ) -> None:
        """Two threads share the title catalog: the worker validates a package
        while the UI thread reloads the overlay 128 times. The load-bearing
        claims are that both sides finish and that the status the worker read
        is the one it stored; a data race between them is only visible to a
        sanitizer, which hosted CI does not run."""
        run = self.run_player(
            "ready",
            wait_background=True,
            catalog_reload_count=128,
        )
        self.assertRegex(
            run["stdout"],
            r"\[PLAYER_UI_TEST\] catalog_reload result=PASS "
            r"reloads=128 status_checks=128 status=1",
        )

    def test_every_queued_title_claims_a_check_before_its_first_result(self) -> None:
        """A title whose package status is not known yet must be claimed as
        checking. Leaving it unclaimed shows "BUILD PACKAGE" for a title that
        may already be prepared."""
        run = self.run_player("library")
        frame = run["frames"][0]  # type: ignore[index]
        titles = int(frame["games"])
        self.assertGreater(titles, 1, run["stdout"])
        self.assertEqual(frame["titles_unclaimed"], "0", run["stdout"])
        self.assertEqual(frame["titles_pending"], str(titles), run["stdout"])

    def test_lost_worker_events_are_reclaimed_instead_of_stalling_the_card(self) -> None:
        """A package-status worker whose completion event never reaches the
        queue must not leave its card pending for the session. With every push
        refused the launcher still applies the worker's result, so the harness
        reaches its quit instead of waiting on a check that can never finish."""
        run = self.run_player(
            "ready",
            ("WAIT_MS=1200",),
            stale_package=True,
            wait_background=True,
            env_extra={"NK_UI_TEST_DROP_WORKER_EVENT": "1"},
        )
        self.assertIn("Package status for", run["stderr"])
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["running"], "0")
        # The package job was applied from the worker result, so no title is
        # left claiming a check that will never finish.
        self.assertEqual(frames[-1]["titles_pending"], "0", run["stdout"])
        self.assertEqual(frames[-1]["titles_unclaimed"], "0", run["stdout"])
        self.assertEqual(frames[-1]["selected_package_status"], "3", run["stdout"])

    def test_package_status_thread_creation_failure_is_unresolved_and_retryable(
        self,
    ) -> None:
        """A failed worker start stays unresolved until the user retries.

        The 2-second library rescan must not turn one create failure into a
        repeating thread-creation loop. RETRY PACKAGE CHECK, the title page's
        primary action, then enters the same production queue and lets the real
        validator establish MISSING.
        """
        run = self.next_ui_run(
            "ready",
            # Open the title; its primary action has focus (launcher review B6).
            events=("WAIT_MS=2200", "KEY_RETURN", "WAIT_MS=300", "KEY_RETURN", "WAIT_MS=300"),
            wait_background=True,
            env_extra={
                "NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_AT": "1"
            },
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        failed_frames = [frame for frame in frames
                         if frame["selected_package_check_failed"] == "1"]
        self.assertTrue(failed_frames, run["stdout"])
        self.assertTrue(all(frame["selected_package_status"] == "4"
                            for frame in failed_frames), run["stdout"])
        self.assertTrue(all(frame["next_package"] == self.NEXT_PACKAGE_CHECK_FAILED
                            for frame in failed_frames), run["stdout"])
        self.assertTrue(any(frame["next_primary_label"] == "RETRY_PACKAGE_CHECK"
                            for frame in failed_frames), run["stdout"])
        self.assertTrue(all(frame["titles_failed"] == "1" and
                            frame["titles_pending"] == "0" and
                            frame["titles_unclaimed"] == "0"
                            for frame in failed_frames), run["stdout"])
        self.assertTrue(any(int(frame["ticks_ms"]) >= 2000 and
                            frame["package_status_thread_attempts"] == "1"
                            for frame in failed_frames), run["stdout"])
        self.assertTrue(any(frame["package_status_thread_attempts"] == "2"
                            for frame in frames), run["stdout"])
        self.assertEqual(frames[-1]["package_status_thread_attempts"], "2")
        self.assertEqual(frames[-1]["selected_package_check_failed"], "0")
        self.assertEqual(frames[-1]["titles_failed"], "0")
        self.assertEqual(frames[-1]["titles_pending"], "0")
        self.assertEqual(frames[-1]["titles_unclaimed"], "0")
        self.assertEqual(frames[-1]["selected_package_status"], "1")
        self.assertEqual(frames[-1]["package_building"], "0")

    # PLAYER_UI_PACKAGE_CHECK_FAILED (player_ui_nk.h): the title's package check failed.
    NEXT_PACKAGE_CHECK_FAILED = "4"

    def test_failed_post_build_worker_start_survives_cache_invalidation_and_retries(self) -> None:
        run = self.next_ui_run(
            "ready",
            events=(
                "WAIT_VIEW=library,5000",
                "INVALIDATE_PACKAGE_CACHE",
                # Open the title, then RETRY PACKAGE CHECK, its primary action.
                "WAIT_MS=100",
                "KEY_RETURN",
                "WAIT_MS=300",
                "KEY_RETURN",
                "WAIT_MS=100",
                "WAIT_VIEW=ready_library,10000",
                "ASSERT_VIEW=ready_library",
            ),
            wait_background=True,
            build_ready_test=True,
            env_extra={
                "NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_AT": "2"
            },
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertIn(
            "[PLAYER_UI_TEST] post_build_validation status=0", run["stdout"]
        )
        self.assertIn(
            "[PLAYER_UI_TEST] assert_view result=PASS actual=ready_library",
            run["stdout"],
        )
        self.assertRegex(
            run["stdout"],
            r"\[PLAYER_UI_TEST\] invalidate_package_cache generation=\d+",
        )
        failed_frames = [frame for frame in frames
                         if frame["selected_package_check_failed"] == "1"]
        self.assertTrue(failed_frames, run["stdout"])
        self.assertTrue(all(frame["next_package"] == self.NEXT_PACKAGE_CHECK_FAILED
                            for frame in failed_frames), run["stdout"])
        self.assertTrue(any(frame["next_primary_label"] == "RETRY_PACKAGE_CHECK"
                            for frame in failed_frames), run["stdout"])
        self.assertTrue(any(frame["package_status_thread_attempts"] == "2"
                            and frame["view"] == "library"
                            for frame in failed_frames), run["stdout"])
        self.assertEqual(frames[-1]["view"], "ready_library")
        self.assertGreaterEqual(
            int(frames[-1]["package_status_thread_attempts"]), 3
        )

    def test_worker_start_failure_automatically_retries_after_bounded_backoff(self) -> None:
        """The first worker start fails; once the 5 s backoff expires the title is retried on
        its own, and the retry's result replaces the failure.

        Why the old fixed 6.2 s wait could miss: the backoff runs 5 s from the failure
        (player_state.c), and only the periodic rescan (every 2 s with no check running,
        main.c) starts a due retry. The retry therefore starts between 5 s and 7 s after the
        failure, and its check then takes time of its own. A run that ended at 6.2 s saw
        either no retry yet (hosted CI: attempts 1 at the last frame, 6.4 s) or a retry in
        flight with its result not in (status 4, unknown, where 1 was expected). Both are
        correct product behaviour inside the documented bound. The run now waits for the
        event the contract is about, the retry's result (WAIT_PACKAGE_STATUS), with a
        deadline well past the 7 s bound, and asserts on the frames: the failure was shown,
        no retry started before the backoff, and the retry came."""
        run = self.run_player(
            "ready",
            events=("WAIT_PACKAGE_STATUS=MISSING,20000",),
            wait_background=True,
            env_extra={
                "NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_AT": "1"
            },
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertIn("[PLAYER_UI_TEST] wait_package_status result=PASS", run["stdout"])
        first_attempt = [frame for frame in frames
                         if frame["package_status_thread_attempts"] == "1"]
        self.assertTrue(first_attempt, run["stdout"])
        self.assertTrue(
            any(frame["selected_package_check_failed"] == "1" for frame in first_attempt),
            run["stdout"],
        )
        retried = [frame for frame in frames
                   if int(frame["package_status_thread_attempts"]) >= 2]
        self.assertTrue(retried, run["stdout"])
        # The backoff is 5 s from the failure, which is no earlier than start.
        self.assertGreaterEqual(int(retried[0]["ticks_ms"]), 5000, run["stdout"])
        self.assertGreaterEqual(
            int(frames[-1]["package_status_thread_attempts"]), 2,
            run["stdout"],
        )
        self.assertEqual(frames[-1]["selected_package_check_failed"], "0")
        self.assertEqual(frames[-1]["selected_package_status"], "1")

    def test_completed_retry_clears_failure_when_cache_generation_changes(self) -> None:
        run = self.next_ui_run(
            "ready",
            events=(
                # Open the title, then RETRY PACKAGE CHECK; the 600 ms worker delay
                # keeps the invalidation inside the retry's check.
                "WAIT_MS=100",
                "KEY_RETURN",
                "WAIT_MS=200",
                "KEY_RETURN",
                "WAIT_MS=100",
                "INVALIDATE_PACKAGE_CACHE",
                "WAIT_MS=1700",
            ),
            wait_background=True,
            env_extra={
                "NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_AT": "1",
                "NK_UI_TEST_PACKAGE_STATUS_WORKER_DELAY_MS": "600",
            },
        )
        self.assertIn("[PLAYER_UI_TEST] invalidate_package_cache", run["stdout"])
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertTrue(any(frame["next_primary_label"] == "RETRY_PACKAGE_CHECK"
                            for frame in frames), run["stdout"])
        self.assertGreaterEqual(
            int(frames[-1]["package_status_thread_attempts"]), 3,
            run["stdout"],
        )
        self.assertEqual(frames[-1]["selected_package_check_failed"], "0",
                         run["stdout"])
        self.assertEqual(frames[-1]["selected_package_status"], "1",
                         run["stdout"])

    def test_closing_during_the_startup_package_check_exits_promptly(self) -> None:
        """Launcher review B5: closing does not wait for the package check.

        NK_UI_TEST_PACKAGE_STATUS_WORKER_DELAY_MS stands in for a check that
        hashes a large package (about 4 s for each showcase demo). The run quits
        300 ms in, while that check runs. Failing-before: shutdown joined the
        check's thread, so the window stayed up until the check ended (up to
        about 25 s with three titles); now the check stops at its next read."""
        run = self.run_player(
            "ready", ("WAIT_MS=300",),
            env_extra={"NK_UI_TEST_PACKAGE_STATUS_WORKER_DELAY_MS": "5000"},
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertGreaterEqual(int(frames[-1]["package_status_thread_attempts"]), 1,
                                run["stdout"])
        wall = run["wall_seconds"]
        assert isinstance(wall, float)
        shutdown = wall - int(frames[-1]["ticks_ms"]) / 1000.0
        self.assertLess(shutdown, 2.5,
                        f"closing waited {shutdown:.1f} s for the package check")

    def test_library_card_says_checking_until_the_package_check_ends(self) -> None:
        """Launcher review B4: a card reads CHECKING, not NEEDS SETUP, while its check runs.

        Failing-before: the showcase demos read NEEDS SETUP for the first 4-9 s of
        every start, until their checks finished, and then flipped to READY."""
        # 4.5 s spans the first check (1.5 s) and the periodic re-check that starts after
        # 2 s with no check running.
        run = self.run_player(
            "ready", ("WAIT_MS=4500",), wait_background=True,
            env_extra={"NK_UI_TEST_PACKAGE_STATUS_WORKER_DELAY_MS": "1500"},
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in str(run["stdout"]):
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        statuses = [
            line.split("status=", 1)[1]
            for line in str(run["stdout"]).splitlines()
            if line.startswith("[PLAYER_UI_TEST] next_ui_library_status index=0 ")
        ]
        self.assertTrue(statuses, run["stdout"])
        self.assertEqual(statuses[0], "Checking", statuses)
        # The check then decides: this synthetic title has no package to run.
        self.assertGreaterEqual(len(statuses), 2, statuses)
        self.assertNotEqual(statuses[-1], "Checking", statuses)
        # A periodic re-check keeps the decided status: the card never blinks back to
        # CHECKING (it did on every 2 s rescan, and made this test flaky on slow hosts).
        decided = statuses.index(next(s for s in statuses if s != "Checking"))
        self.assertNotIn("Checking", statuses[decided:], statuses)

    def test_background_wait_ends_when_a_check_outlasts_the_rescan_period(self) -> None:
        """A package check slower than the 2 s rescan period no longer repeats without pause.

        The periodic rescan counted 2 s from the previous rescan, even while a check
        ran. A check of 2 s or more (a large package, or 1.5 s of test delay on a
        loaded CI runner) found the next one queued when it ended, so the launcher
        re-checked back to back and never went idle, and a run that waits for
        background work hit its timeout (the library-card test above, at 49.5 s on
        hosted Windows). Failing-before: with a 2.5 s check this run never went idle
        and timed out. Now the rescan waits for 2 s with no check running, so the run
        ends when the check under way finishes."""
        try:
            run = self.run_player(
                "ready", ("WAIT_MS=500",), wait_background=True,
                env_extra={"NK_UI_TEST_PACKAGE_STATUS_WORKER_DELAY_MS": "2500"},
                timeout=30.0,
            )
        except subprocess.TimeoutExpired:
            self.fail("the launcher never went idle: each 2.5 s package check queued the next")
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(int(frames[-1]["package_status_thread_attempts"]), 1, run["stdout"])

    def test_failed_post_build_check_does_not_suppress_later_title_build(self) -> None:
        run = self.run_player(
            "library",
            events=(
                "START_SYNTHETIC_BUILD=2",
                "WAIT_MS=2200",
                "START_SYNTHETIC_BUILD=1",
                "WAIT_VIEW=error,10000",
                "ASSERT_VIEW=error",
            ),
            wait_background=True,
            env_extra={
                "NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_AT": "2"
            },
        )
        self.assertIn(
            "[PLAYER_UI_TEST] synthetic_build result=PASS index=1 disc=TEST00006",
            run["stdout"],
        )
        self.assertIn(
            "[PLAYER_UI_TEST] synthetic_build result=PASS index=0 disc=TEST00005",
            run["stdout"],
        )
        self.assertIn(
            "Package status for TEST00006 is unresolved", run["stderr"]
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertTrue(
            any(int(frame["package_status_thread_attempts"]) >= 3
                for frame in frames),
            run["stdout"],
        )
        self.assertIn(
            "[PLAYER_UI_TEST] post_build_validation status=1", run["stdout"]
        )
        self.assertIn(
            "[PLAYER_UI_TEST] assert_view result=PASS actual=error",
            run["stdout"],
        )

    def test_failed_post_build_check_same_disc_cache_epoch_is_not_stale(self) -> None:
        run = self.run_player(
            "library",
            events=(
                "START_SYNTHETIC_BUILD=2",
                "WAIT_MS=2200",
                "INVALIDATE_AND_START_SYNTHETIC_BUILD=2",
                "WAIT_VIEW=error,10000",
                "ASSERT_VIEW=error",
            ),
            wait_background=True,
            env_extra={
                "NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_AT": "2"
            },
        )
        action_line = next(
            line for line in run["stdout"].splitlines()
            if line.startswith(
                "[PLAYER_UI_TEST] invalidate_cache_and_synthetic_build"
            )
        )
        action_match = re.search(
            r"result=PASS index=1 disc=TEST00006 generation=(\d+) "
            r"build_session_generation=(\d+)$",
            action_line,
        )
        self.assertIsNotNone(action_match, run["stdout"])
        assert action_match is not None
        self.assertGreater(int(action_match.group(1)), 0)
        self.assertGreater(int(action_match.group(2)), 0)
        post_match = re.search(
            r"\[PLAYER_UI_TEST\] post_build_validation status=1 .*?"
            r"build_session_generation=(\d+) "
            r"current_build_session_generation=(\d+)",
            run["stdout"],
        )
        self.assertIsNotNone(post_match, run["stdout"])
        assert post_match is not None
        self.assertEqual(post_match.group(1), post_match.group(2), run["stdout"])
        self.assertEqual(post_match.group(1), action_match.group(2), run["stdout"])
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertTrue(
            any(int(frame["package_status_thread_attempts"]) >= 3
                for frame in frames),
            run["stdout"],
        )
        self.assertIn(
            "[PLAYER_UI_TEST] assert_view result=PASS actual=error",
            run["stdout"],
        )

    def test_worker_start_failure_does_not_block_other_title_builds(self) -> None:
        """A's worker-start failure cannot strand the post-build latch for B.

        A fails before either B build starts, and B completes both builds. The
        failure selector is title-keyed so this does not depend on worker
        scheduling or a global create-attempt ordinal.
        """
        run = self.run_player(
            "library",
            events=(
                "START_SYNTHETIC_BUILD=1",
                "WAIT_PACKAGE_FAILURE=TEST00005,1",
                "START_SYNTHETIC_BUILD=2",
                "WAIT_VIEW=ready_library,10000",
                "START_SYNTHETIC_BUILD=2",
                "WAIT_VIEW=ready_library,10000",
                "ASSERT_VIEW=ready_library",
                # The script started the builds, not the launcher's focus, which
                # is still on A's card: select B to read its state.
                "WAIT_MS=100",
                "KEY_RIGHT",
                "WAIT_MS=100",
                "QUIT",
            ),
            runtime_ready=True,
            post_build_ready=True,
            wait_background=True,
            env_extra={
                "NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_FOR": "TEST00005"
            },
        )
        post_build_lines = [
            line for line in run["stdout"].splitlines()
            if "post_build_transition" in line and "disc=TEST00006" in line
        ]
        self.assertEqual(len(post_build_lines), 2, run["stdout"])
        unresolved = [
            line for line in run["stderr"].splitlines()
            if "is unresolved because its worker could not start" in line
        ]
        self.assertGreaterEqual(len(unresolved), 1, run["stderr"])
        self.assertTrue(all("TEST00005" in line for line in unresolved), run["stderr"])
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertTrue(
            any(frame["selected_disc"] == "TEST00005" and
                frame["selected_package_check_failed"] == "1" and
                frame["selected_package_status"] == "4"
                for frame in frames),
            run["stdout"],
        )
        self.assertEqual(frames[-1]["selected_disc"], "TEST00006", run["stdout"])
        self.assertEqual(frames[-1]["selected_package_check_failed"], "0", run["stdout"])
        self.assertEqual(frames[-1]["selected_package_status"], "0", run["stdout"])

    def test_repeated_post_build_failures_keep_prior_title_retryable(self) -> None:
        run = self.next_ui_run(
            "library",
            events=(
                "START_SYNTHETIC_BUILD=2",
                "WAIT_PACKAGE_FAILURE=TEST00006,1",
                "START_SYNTHETIC_BUILD=1",
                "WAIT_PACKAGE_FAILURE=TEST00005,1",
                # Select B, open its title, then RETRY PACKAGE CHECK.
                "WAIT_MS=100",
                "KEY_RIGHT",
                "WAIT_MS=100",
                "KEY_RETURN",
                "WAIT_MS=300",
                "KEY_RETURN",
                "WAIT_PACKAGE_FAILURE=TEST00006,2",
                "QUIT",
            ),
            runtime_ready=True,
            post_build_ready=True,
            wait_background=True,
            env_extra={
                # B fails on its post-build check, A fails on the next build,
                # and B fails again on the explicit retry. The following
                # repeated failures must stay attributed to their titles.
                "NK_UI_TEST_FAIL_PACKAGE_STATUS_THREAD_CREATE_FOR":
                    "TEST00005,TEST00006"
            },
        )
        self.assertIn(
            "Package status for TEST00006 is unresolved", run["stderr"]
        )
        self.assertIn(
            "Package status for TEST00005 is unresolved", run["stderr"]
        )
        unresolved = [
            line for line in run["stderr"].splitlines()
            if "is unresolved because its worker could not start" in line
        ]
        self.assertGreaterEqual(
            sum("TEST00005" in line for line in unresolved), 2, run["stderr"]
        )
        self.assertGreaterEqual(
            sum("TEST00006" in line for line in unresolved), 2, run["stderr"]
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertTrue(
            any(frame["selected_disc"] == "TEST00006" and
                frame["selected_package_check_failed"] == "1" and
                frame["selected_package_status"] == "4"
                for frame in frames),
            run["stdout"],
        )
        self.assertEqual(frames[-1]["selected_disc"], "TEST00006")
        self.assertEqual(frames[-1]["selected_package_check_failed"], "1")
        self.assertEqual(frames[-1]["selected_package_status"], "4")
        self.assertEqual(frames[-1]["titles_pending"], "0")

    def test_async_build_validation_enters_ready_library(self) -> None:
        run = self.run_player(
            "ready",
            ("WAIT_VIEW=ready_library,10000", "ASSERT_VIEW=ready_library"),
            build_ready_test=True,
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["view"], "building_package")
        ready_frames = [frame for frame in frames if frame["view"] == "ready_library"]
        self.assertTrue(ready_frames)
        self.assertRegex(
            run["stdout"],
            r"\[PLAYER_UI_TEST\] post_build_validation status=0 reason=Runtime package v2 is valid",
        )
        self.assertRegex(
            run["stdout"],
            r"\[PLAYER_UI_TEST\] post_build_transition view=ready_library "
            r"disc=TEST00006 found=1 prepared=1 status=\d+",
        )

    def test_experimental_build_without_selected_executable_ends_ready(self) -> None:
        """#1276: a passing build of a disc saved with no selected executable.

        The build bound EBOOT.BIN in the experimental profile; the check after
        it used the empty library value, refused the new package and showed
        "Package Build Failed". It must end on READY with a playable package
        (runtime=1 is what makes its card offer PLAY)."""
        run = self.run_player(
            "ready",
            ("WAIT_VIEW=ready_library,10000", "ASSERT_VIEW=ready_library"),
            experimental_build_ready=True,
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["view"], "building_package")
        self.assertRegex(
            run["stdout"],
            r"\[PLAYER_UI_TEST\] post_build_validation status=0 "
            r"reason=Runtime package v\d is valid for TEST00009",
        )
        self.assertRegex(
            run["stdout"],
            r"\[PLAYER_UI_TEST\] post_build_transition view=ready_library "
            r"disc=TEST00009 found=1 prepared=1 status=\d+ runtime=1",
        )
        self.assertEqual(frames[-1]["view"], "ready_library")
        self.assertFalse(any(frame["view"] == "error" for frame in frames), run["stdout"])

    def test_build_failure_card_dismisses_and_cancel_stays_away(self) -> None:
        """#1277: a finished build's outcome is shown once per session.

        The failure card was re-opened on every frame, so Back to library
        could not dismiss it, and a cancelled build pulled every later view
        back to the Library."""
        failed = self.run_player(
            "build-failed",
            ("WAIT_VIEW=error,5000", "KEY_RETURN", "WAIT_MS=300", "ASSERT_VIEW=library"),
        )
        self.assertIn("[PLAYER_UI_TEST] assert_view result=PASS actual=library", failed["stdout"])
        failed_frames = failed["frames"]
        assert isinstance(failed_frames, list)
        self.assertEqual(failed_frames[-1]["view"], "library")

        cancelled = self.run_player(
            "building",
            ("KEY_ESCAPE", "WAIT_MS=200", "KEY_S", "WAIT_MS=300", "ASSERT_VIEW=settings"),
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in cancelled["stdout"]:
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        self.assertIn(
            "[PLAYER_UI_TEST] assert_view result=PASS actual=settings", cancelled["stdout"]
        )
        cancelled_frames = cancelled["frames"]
        assert isinstance(cancelled_frames, list)
        self.assertEqual(cancelled_frames[-1]["view"], "settings")

    def test_inspection_and_support_screens_render_and_return(self) -> None:
        for view in ("inspecting", "supported", "experimental", "unsupported"):
            with self.subTest(view=view):
                run = self.run_player(view, ("KEY_ESCAPE",))
                frames = run["frames"]
                assert isinstance(frames, list)
                self.assertEqual(frames[0]["view"], view)
                self.assertEqual(frames[-1]["view"], "library")
                self.assertNotEqual(frames[0]["pixels"], frames[-1]["pixels"])

    def test_staged_card_exposes_runtime_required_status(self) -> None:
        run = self.next_ui_run("ready", ("WAIT_MS=300",), wait_background=True)
        frame = run["frames"][0]  # type: ignore[index]
        self.assertEqual(frame["selected_staged"], "1")
        self.assertEqual(frame["selected_runtime"], "0")
        statuses = [
            line.split("status=", 1)[1]
            for line in str(run["stdout"]).splitlines()
            if line.startswith("[PLAYER_UI_TEST] next_ui_library_status index=0 ")
        ]
        self.assertTrue(statuses, run["stdout"])
        self.assertEqual(statuses[-1], "Needs setup", statuses)

    def test_several_dropped_files_open_one_disc_and_say_what_happened(self) -> None:
        # The real SDL drop events through the player's event loop: two disc images and a text
        # file. The first image opens, the other waits in the queue, and the library will say
        # what was skipped (#2007).
        run = self.run_player(
            "library",
            (
                "DROP_BEGIN",
                "DROP_FILE=Z:/dropped/first.iso",
                "DROP_FILE=Z:/dropped/notes.txt",
                "DROP_FILE=Z:/dropped/second.iso",
                "DROP_COMPLETE",
                "WAIT_MS=100",
            ),
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["drop_pending"], "0")
        self.assertEqual(frames[-1]["drop_pending"], "1")
        self.assertEqual(frames[-1]["drop_notice"], "summary")
        self.assertEqual(frames[-1]["error"], "ISO_CORRUPT")  # the first image was looked at

    def test_a_lone_dropped_file_opens_and_gets_no_summary(self) -> None:
        run = self.run_player("library", ("DROP_FILE=Z:/dropped/only.iso", "WAIT_MS=100"))
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["error"], "ISO_CORRUPT")
        self.assertEqual(frames[-1]["drop_pending"], "0")
        self.assertEqual(frames[-1]["drop_notice"], "none")

    def test_files_dropped_while_a_game_is_being_set_up_are_ignored(self) -> None:
        # wizard-staging has a worker extracting. A lone file (no DROP_BEGIN, as SDL sends one
        # file) and a bracketed batch are both ignored, as a dropped file always was then:
        # nothing is queued to open later, and no error card opens over the setup.
        for events in (
            ("DROP_FILE=Z:/dropped/lone.iso", "WAIT_MS=100"),
            (
                "DROP_BEGIN",
                "DROP_FILE=Z:/dropped/a.iso",
                "DROP_FILE=Z:/dropped/b.iso",
                "DROP_COMPLETE",
                "WAIT_MS=100",
            ),
        ):
            with self.subTest(events=events):
                run = self.run_player("wizard-staging", events)
                frames = run["frames"]
                assert isinstance(frames, list)
                for frame in frames:
                    self.assertEqual(frame["view"], "wizard")
                    self.assertEqual(frame["extracting"], "1")
                    self.assertEqual(frame["error"], "NONE")
                # The queue fills as a batch's files arrive and is emptied when it completes.
                self.assertEqual(frames[-1]["drop_pending"], "0")
                self.assertEqual(frames[-1]["drop_notice"], "none")

    def test_staging_and_package_build_cancellation_render_progress(self) -> None:
        staging = self.run_player("wizard-staging", ("KEY_ESCAPE",))
        stage_frames = staging["frames"]
        assert isinstance(stage_frames, list)
        self.assertEqual(stage_frames[0]["extracting"], "1")
        self.assertEqual(stage_frames[0]["extraction_percent"], "67")
        self.assertEqual(stage_frames[1]["extraction_cancel"], "1")
        self.assertTrue(all(int(frame["pixels"], 16) != 0 for frame in stage_frames[:2]))

        building = self.run_player("building", ("KEY_ESCAPE",))
        build_frames = building["frames"]
        assert isinstance(build_frames, list)
        self.assertEqual(build_frames[0]["view"], "building_package")
        self.assertEqual(build_frames[0]["package_building"], "1")
        self.assertEqual(build_frames[1]["package_cancelled"], "1")
        self.assertEqual(build_frames[1]["view"], "library")

    def test_corrupt_disc_and_error_card_recovery(self) -> None:
        corrupt = self.run_player("library", ("KEY_RETURN",), drop_invalid_iso=True)
        error_frames = corrupt["frames"]
        assert isinstance(error_frames, list)
        self.assertEqual(error_frames[1]["view"], "error")
        self.assertEqual(error_frames[1]["error"], "ISO_CORRUPT")
        self.assertEqual(error_frames[2]["picker"], "1")

        missing_source = self.run_player("error", ("KEY_RETURN",))
        missing_source_frames = missing_source["frames"]
        assert isinstance(missing_source_frames, list)
        self.assertEqual(missing_source_frames[0]["error"], "SOURCE_NOT_FOUND")
        self.assertEqual(missing_source_frames[1]["picker"], "1")

    def test_invalid_disc_in_wizard_is_visible_without_a_selected_iso(self) -> None:
        rejected = self.run_player("wizard2", drop_invalid_iso=True)
        rejected_frames = rejected["frames"]
        assert isinstance(rejected_frames, list)
        rejected_frame = rejected_frames[-1]
        self.assertEqual(rejected_frame["view"], "wizard")
        self.assertEqual(rejected_frame["wizard_step"], "select_game")
        self.assertEqual(rejected_frame["bad_disc_notice"], "1")

        recovery_codes = (
            "EXPERIMENTAL_PROFILE_FAILED",
            "SOURCE_NOT_FOUND",
            "PROCESS_SPAWN_FAILED",
            "STAGED_EXECUTABLE_INVALID",
            "MANIFEST_MISMATCH",
            "RUNTIME_NOT_FOUND",
            "LIBRARY_WRITE_FAILED",
            "RUNTIME_PACKAGE_NOT_READY",
            "PYTHON_NOT_FOUND",
            "CLI_NOT_FOUND",
            "DATA_DIR_UNAVAILABLE",
            "SPAWN_FAILED",
            "PACKAGE_BUILD_FAILED",
            "RUNTIME_PREMATURE_EXIT",
            "RUNTIME_ERROR_EXIT",
        )
        # The launcher applies a control's action after the frame that drew it,
        # so the frame of the press still shows the card and the library is
        # drawn from the next frame; compare a frame after the wait.
        for code in recovery_codes:
            with self.subTest(code=code):
                recovered = self.run_player(
                    "library", ("KEY_RETURN", "WAIT_MS=100"), error_code=code,
                )
                frames = recovered["frames"]
                assert isinstance(frames, list)
                self.assertEqual(frames[0]["view"], "error")
                self.assertEqual(frames[0]["error"], code)
                self.assertEqual(frames[1]["view"], "library")
                self.assertEqual(frames[-1]["view"], "library")
                self.assertNotEqual(frames[0]["pixels"], frames[-1]["pixels"],
                                    "the library replaces the error card on screen")
                # Only a missing or corrupt source reopens the file picker.
                expected_picker = "1" if code in ("ISO_CORRUPT", "SOURCE_NOT_FOUND") else "0"
                self.assertEqual(frames[1]["picker"], expected_picker)

    def test_pbp_package_error_card_names_the_package_boundary(self) -> None:
        """A store package dropped as an ISO is named as a package, not a broken disc.

        Reading it as ISO9660 only ever said the volume descriptor was missing,
        which points a person at repairing a file that was never a disc. The
        card must carry the package boundary, and its recovery action must still
        reopen the file picker so another file can be chosen.
        """
        package = self.run_player("library", ("KEY_RETURN",), drop_pbp_iso=True)
        frames = package["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[1]["view"], "error")
        self.assertEqual(frames[1]["error"], "PBP_PACKAGE_UNSUPPORTED")
        self.assertEqual(frames[2]["picker"], "1")

    def test_homebrew_eboot_drop_shows_experimental_card_and_adds_to_library(self) -> None:
        """#2143: a real PSPSDK EBOOT.PBP picked in the launcher is imported, not refused.

        Failing before: PSPSDK writes the category MG for every homebrew and the package
        classifier read it as a firmware updater (FIRMWARE_OUT_OF_SCOPE). The package must
        convert to its private disc image, show the Experimental card under its own title, and
        ADD TO LIBRARY must store an experimental record under a non-retail library key.
        """
        with tempfile.TemporaryDirectory(prefix=".homebrew-eboot-", dir=ROOT) as tmp:
            eboot = build_homebrew_eboot(Path(tmp))
            package = eboot.read_bytes()
        events = ("WAIT_VIEW=experimental|unsupported|supported|error,20000", "WAIT_MS=100",
                  "KEY_RETURN", "WAIT_MS=2000")
        # The program's folder comes with it, so the package sits in a folder of its own, as
        # on a Memory Stick: the run's scratch folder holds the home and data folders, which
        # an import refuses to copy (HOMEBREW_FOLDER_UNSUPPORTED).
        run = self.next_ui_run("library", events, drop_bytes=("MYHB/EBOOT.PBP", package),
                               wait_background=True)
        frames = run["frames"]
        stdout = run["stdout"]
        assert isinstance(frames, list) and isinstance(stdout, str)
        self.assertNotIn("FIRMWARE_OUT_OF_SCOPE", stdout)
        self.assertFalse(any(frame["view"] == "error" for frame in frames), stdout)
        self.assertTrue(any(frame["view"] == "experimental" for frame in frames), stdout)
        self.assertEqual(frames[-1]["view"], "library", stdout)
        library = run["library"]
        assert isinstance(library, dict)
        self.assertEqual(len(library["games"]), 1, library)
        record = library["games"][0]
        self.assertTrue(record["is_experimental"], record)
        self.assertRegex(record["disc_id"], r"^X[A-Z]{3}[0-9]{5}$")
        self.assertEqual(record["title_name"], "Nakagawa PSP Recompilation Oracle")
        self.assertIn(f"next_ui_disc_badge disc={record['disc_id']} badge=Beta / unofficial",
                      stdout, "the Experimental card labels the program Beta / unofficial")

    def test_damaged_compressed_and_non_psp_disc_images_get_their_own_cards(self) -> None:
        """A .cso whose block index points outside the file is a damaged compressed disc
        image, and an ISO9660 image without a PSP_GAME folder is a disc image of something
        else: each gets its own plain card, not the generic "isn't a PSP disc image" card or
        an "unknown PSP title" page."""
        # A CISO header (magic, header size, uncompressed size, block size, version)
        # whose block index is all zeros: every block points into the header.
        cso = b"CISO" + struct.pack("<IQIBB2x", 0x18, 2048 * 32, 2048, 1, 0) + bytes(4096)
        # A valid ISO9660 image whose root holds only a text file.
        sector = 2048
        image = bytearray(sector * 20)

        def both32(value: int) -> bytes:
            return struct.pack("<I", value) + struct.pack(">I", value)

        def record(name: bytes, lba: int, size: int, directory: bool) -> bytes:
            length = 33 + len(name) + (1 - len(name) % 2)
            body = bytearray(length)
            body[0] = length
            body[2:10] = both32(lba)
            body[10:18] = both32(size)
            body[25] = 2 if directory else 0
            body[28:32] = struct.pack("<H", 1) + struct.pack(">H", 1)
            body[32] = len(name)
            body[33:33 + len(name)] = name
            return bytes(body)

        pvd = image[16 * sector:17 * sector]
        pvd[0] = 1
        pvd[1:6] = b"CD001"
        pvd[6] = 1
        pvd[40:72] = b"SYNTHETIC_DATA_DISC".ljust(32)
        pvd[156:156 + 34] = record(b"\x00", 18, sector, True)
        image[16 * sector:17 * sector] = pvd
        image[17 * sector] = 255
        image[17 * sector + 1:17 * sector + 6] = b"CD001"
        root = (
            record(b"\x00", 18, sector, True)
            + record(b"\x01", 18, sector, True)
            + record(b"README.TXT;1", 19, 16, False)
        )
        image[18 * sector:18 * sector + len(root)] = root
        image[19 * sector:19 * sector + 16] = b"synthetic text\n\n"
        cases = (
            ("source-owned-compressed.cso", cso, "DISC_IMAGE_INDEX_INVALID", "Damaged_disc_image"),
            ("source-owned-data-disc.iso", bytes(image), "DISC_NOT_PSP", "Not_a_PSP_disc_image"),
        )
        for name, payload, code, title in cases:
            with self.subTest(code=code):
                run = self.run_player("library", ("KEY_RETURN",), drop_bytes=(name, payload))
                frames = run["frames"]
                assert isinstance(frames, list)
                self.assertEqual(frames[1]["view"], "error", run["stdout"])
                self.assertEqual(frames[1]["error"], code)
                self.assertEqual(frames[1]["error_title"], title)
                self.assertEqual(frames[2]["picker"], "1")

    def test_compressed_disc_images_open_the_card_their_iso_opens(self) -> None:
        """A .cso, .zso or .chd is expanded once, with the wizard's progress panel, and
        then opens exactly the card its .iso opens: no refusal, the same view."""
        from test_disc_expand import BUILDERS

        with tempfile.TemporaryDirectory(prefix=".compressed-disc-", dir=ROOT) as tmp:
            iso_path = Path(tmp) / "synthetic.iso"
            create_test_iso_with_executables(
                iso_path, b"synthetic input executable\n", disc_id="TEST00042",
                title="Synthetic Compressed Disc",
            )
            iso = iso_path.read_bytes()
        wait = ("WAIT_VIEW=experimental|unsupported|supported|error,15000", "WAIT_MS=100")
        plain = self.run_player("library", wait, drop_bytes=("source-owned.iso", iso))
        plain_views = [frame["view"] for frame in plain["frames"]]
        self.assertNotIn("error", plain_views, plain["stdout"])
        for name, build in BUILDERS.items():
            with self.subTest(format=name):
                run = self.run_player("library", wait,
                                      drop_bytes=(f"source-owned.{name}", build(iso)))
                frames = run["frames"]
                assert isinstance(frames, list)
                views = [frame["view"] for frame in frames]
                self.assertNotIn("error", views, run["stdout"])
                self.assertIn("1", [frame["extracting"] for frame in frames if frame["view"] == "wizard"],
                              "the expansion shows the wizard's progress panel")
                self.assertEqual(views[-1], plain_views[-1], run["stdout"])
                self.assertEqual(frames[-1]["error"], "NONE")
                self.assertIn(f"[PLAYER] Disc image {name} expanded into TEST00042", run["stdout"])

    def test_beta_disc_is_added_and_labelled_beta_unofficial(self) -> None:
        """A disc whose own ID is a placeholder, not a retail ID, is added under the key the
        Python tools derive from its contents, and its card says Beta / unofficial."""
        from nk_core.iso_inspect import inspect_iso

        with tempfile.TemporaryDirectory(prefix=".beta-disc-", dir=ROOT) as tmp:
            iso_path = Path(tmp) / "synthetic-beta.iso"
            create_test_iso_with_executables(
                iso_path, build_plain_mips_elf(), disc_id="PROTO1234567",
                title="Synthetic Beta Disc",
            )
            iso = iso_path.read_bytes()
            key = inspect_iso(iso_path).disc_id
        self.assertRegex(key, r"^X[A-Z]{3}[0-9]{5}$")
        events = ("WAIT_VIEW=experimental|unsupported|supported|error,15000", "WAIT_MS=100",
                  "KEY_RETURN", "WAIT_MS=300")
        run = self.next_ui_run("library", events, drop_bytes=("source-owned-beta.iso", iso))
        frames = run["frames"]
        assert isinstance(frames, list)
        views = [frame["view"] for frame in frames]
        self.assertIn("experimental", views, run["stdout"])
        self.assertNotIn("error", views, run["stdout"])
        self.assertEqual(views[-1], "library", run["stdout"])
        stdout = run["stdout"]
        assert isinstance(stdout, str)
        self.assertIn(f"next_ui_disc_badge disc={key} badge=Beta / unofficial", stdout,
                      "the game-found card names the disc Beta / unofficial too")
        badges = re.findall(r"next_ui_library_badge index=\d+ disc=(\S+) badge=(.+)", stdout)
        self.assertIn((key, "Beta / unofficial"), badges, stdout)
        self.assertNotIn((key, "Your disc"), badges)

    def test_quit_from_library_runs_through_sdl_event_loop(self) -> None:
        run = self.run_player("library", ("KEY_ESCAPE",))
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["view"], "library")
        self.assertEqual(frames[-1]["running"], "0")

    # The launcher reaches every flow from the library: the setup wizard from an
    # empty library, Settings, the file picker and quit.

    def test_empty_library_starts_the_setup_wizard(self) -> None:
        run = self.run_player("empty", ("KEY_RETURN", "KEY_RETURN", "KEY_RETURN"))
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["view"], "library")
        self.assertEqual(frames[1]["view"], "wizard")
        self.assertEqual(frames[1]["wizard_step"], "welcome")
        self.assertEqual(frames[2]["wizard_step"], "online_options")
        self.assertEqual(frames[3]["wizard_step"], "select_game")

    def test_library_shortcuts_reach_settings_picker_and_quit(self) -> None:
        settings = self.run_player("library", ("KEY_S",))
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in settings["stdout"]:
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        frames = settings["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["view"], "settings")
        picker = self.run_player("library", ("KEY_O",))
        frames = picker["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["picker"], "1")
        quit_run = self.run_player("library", ("KEY_ESCAPE",))
        frames = quit_run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["running"], "0")

    # The next UI draws the setup wizard too, with the existing wizard's handlers.
    def test_next_ui_wizard_options_step_switches_and_skip(self) -> None:
        # The one switch sits above the buttons: Up from Continue reaches Developer
        # mode. Online mode has no switch until the feature exists (#2007).
        run = self.run_player(
            "wizard-options",
            ("WAIT_MS=100", "KEY_UP", "KEY_RETURN", "WAIT_MS=100"),
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in run["stdout"]:
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["online_mode"], "0")
        self.assertEqual(frames[-1]["developer_mode"], "1")
        self.assertEqual(frames[-1]["wizard_step"], "online_options")
        skipped = self.run_player(
            "wizard-options",
            # Down lands on the button row; Left twice reaches Continue
            # whichever button it lands on, and Right is Skip.
            ("WAIT_MS=100", "KEY_UP", "KEY_RETURN", "KEY_DOWN",
             "KEY_LEFT", "KEY_LEFT", "KEY_RIGHT", "KEY_RETURN"),
        )
        frames = skipped["frames"]
        assert isinstance(frames, list)
        self.assertIn("1", [frame["developer_mode"] for frame in frames])
        self.assertEqual(frames[-1]["wizard_step"], "select_game")
        self.assertEqual(frames[-1]["online_mode"], "0", "Skip keeps online mode off")
        self.assertEqual(frames[-1]["developer_mode"], "0", "Skip turns developer mode back off")

    def test_next_ui_wizard_runs_the_first_run_path(self) -> None:
        run = self.run_player("empty", ("KEY_RETURN", "KEY_RETURN", "KEY_RETURN", "KEY_RETURN"))
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[1]["view"], "wizard")
        self.assertEqual(frames[1]["wizard_step"], "welcome")
        self.assertEqual(frames[2]["wizard_step"], "online_options")
        self.assertEqual(frames[3]["wizard_step"], "select_game")
        self.assertEqual(frames[3]["online_mode"], "0", "Continue is the options step's focus")
        self.assertEqual(frames[4]["picker"], "1",
                         "Choose a disc image is the focused action on Select game")

        fonts = self.run_player("wizard4", ("KEY_RETURN",))
        font_frames = fonts["frames"]
        assert isinstance(font_frames, list)
        self.assertEqual(font_frames[1]["wizard_step"], "ready_launch")
        self.assertEqual(font_frames[1]["font_confirmed"], "1")

        for key in ("KEY_ESCAPE", "KEY_RETURN"):
            staging = self.run_player("wizard-staging", (key,))
            stage_frames = staging["frames"]
            assert isinstance(stage_frames, list)
            self.assertEqual(stage_frames[0]["extracting"], "1")
            self.assertEqual(stage_frames[1]["extraction_cancel"], "1", key)

    BUILD_ACTION = str(0x4001)

    # The next UI draws the build, build tool download and error screens, with
    # their actions and Back.
    def test_next_ui_build_screen_cancels_and_returns(self) -> None:
        building = self.run_player("building", ("KEY_ESCAPE",))
        frames = building["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["view"], "building_package")
        self.assertEqual(frames[0]["package_building"], "1")
        self.assertEqual(frames[1]["package_cancelled"], "1")
        self.assertEqual(frames[1]["view"], "library")
        # Launcher review B6: CANCEL BUILD is never the default focus, so a stray
        # Enter leaves the build running; a move reaches it.
        stray = self.run_player("building", ("KEY_RETURN",))
        frames = stray["frames"]
        assert isinstance(frames, list)
        self.assertNotEqual(frames[0]["next_focus"], self.BUILD_ACTION,
                            "CANCEL BUILD is not focused while the build runs")
        self.assertEqual(frames[1]["package_cancelled"], "0",
                         "a stray Enter does not cancel the build")
        # The build screen was drawn on that frame (-1 when it was not), and it
        # shows the build running.
        self.assertEqual(frames[0]["view"], "building_package")
        self.assertNotEqual(frames[0]["next_build_built"], "-1",
                            "the build screen was drawn")
        self.assertEqual(frames[0]["next_build_built"], "0",
                         "a running build is not shown as built")
        self.assertEqual(frames[1]["view"], "building_package")
        cancel = self.run_player("building", ("KEY_DOWN", "KEY_RETURN"))
        frames = cancel["frames"]
        assert isinstance(frames, list)
        self.assertIn(self.BUILD_ACTION, self.focus_trail(cancel),
                      "a move from no focus reaches CANCEL BUILD")
        self.assertEqual(frames[-1]["package_cancelled"], "1",
                         "Enter on CANCEL BUILD cancels the build")
        built = self.run_player("build-ready", ("KEY_RETURN",))
        frames = built["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["view"], "building_package")
        self.assertEqual(frames[0]["next_build_built"], "1",
                         "a written package reads as built")
        self.assertEqual(frames[1]["view"], "library", "BACK TO LIBRARY once the package is built")
        self.assertEqual(frames[1]["next_build_built"], "-1")
        self.assertEqual(frames[1]["package_cancelled"], "0")

    def test_next_ui_error_card_recovers_like_the_existing_card(self) -> None:
        missing_source = self.run_player("error", ("KEY_RETURN",))
        frames = missing_source["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["error"], "SOURCE_NOT_FOUND")
        self.assertEqual(frames[1]["picker"], "1")
        for code in ("PACKAGE_BUILD_FAILED", "RUNTIME_PACKAGE_NOT_READY", "PYTHON_NOT_FOUND"):
            with self.subTest(code=code):
                recovered = self.run_player("library", ("KEY_RETURN",), error_code=code)
                frames = recovered["frames"]
                assert isinstance(frames, list)
                self.assertEqual(frames[0]["view"], "error")
                self.assertEqual(frames[0]["error"], code)
                self.assertEqual(frames[1]["view"], "library")
                self.assertEqual(frames[1]["picker"], "0")

    def test_game_stop_cards_are_plain_with_restart_and_copy(self) -> None:
        """#1282: a game that ends on its own gets a plain card, not a raw code.

        Failing-before: "Child Process Terminated Early" and "Child Process Error
        Exit" were developer text, the stop card's badge fell back to the raw code
        for the two exit cards, and Back to library was the only action."""
        expected = {
            "RUNTIME_SEMANTIC_BOUNDARY": ("Game_stopped_early", "Game_stopped"),
            "RUNTIME_PREMATURE_EXIT": ("Game_couldn't_start", "Game_didn't_start"),
            "RUNTIME_ERROR_EXIT": ("Game_closed_unexpectedly", "Game_closed"),
        }
        for code, (title, badge) in expected.items():
            with self.subTest(code=code):
                run = self.run_player("library", ("WAIT_MS=50",), error_code=code)
                frames = run["frames"]
                assert isinstance(frames, list)
                self.assertEqual(frames[0]["view"], "error")
                self.assertEqual(frames[0]["error"], code)
                self.assertEqual(frames[0]["error_title"], title)
                self.assertEqual(frames[0]["error_badge"], badge)
                self.assertNotIn("Child", frames[0]["error_title"])
                self.assertEqual(frames[0]["error_retry"], "Restart_game")

        # Copy details: the third button. The report names the problem, its code
        # and the raw boundary, and stays on this computer (clipboard only).
        copied = self.run_player(
            "library", ("WAIT_MS=50", "KEY_RIGHT", "KEY_RIGHT", "KEY_RETURN", "WAIT_MS=100"),
            error_code="RUNTIME_SEMANTIC_BOUNDARY",
        )
        stdout = str(copied["stdout"])
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in stdout:
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        report = re.search(r"\[PLAYER_UI_TEST\] copied_details clipboard=(\d) report=(.*)", stdout)
        self.assertIsNotNone(report, stdout)
        assert report is not None
        self.assertEqual(report.group(1), "1", "the report did not reach the clipboard")
        for field in ("Nakagawa Recomp v", "Problem: Game stopped early",
                      "Code: RUNTIME_SEMANTIC_BOUNDARY",
                      "Reason: Stopped at unsupported-import-scenetadhocinit",
                      "tracked in issue 308", "Last boot events:",
                      "BOOT_EVENT phase=stop reason=semantic-boundary"):
            self.assertIn(field, report.group(2))
        frames = copied["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["view"], "error", "Copy details keeps the card open")
        self.assertEqual(frames[-1]["error_copied"], "1")

    def test_report_this_game_opens_only_links_the_user_clicks(self) -> None:
        """Report this game, See existing reports and a known cause's issue.

        Each is a link the user's browser opens on a click; the regression build
        prints it instead of opening a browser, and the player makes no request.
        The link carries the disc ID and the named boundary, and no path or user
        name. Failing-before: the stop card had no second row of actions, so the
        presses below opened nothing."""
        user = "Synthetic Example"
        run = self.run_player(
            "library",
            ("WAIT_MS=50", "KEY_DOWN", "KEY_RETURN", "WAIT_MS=50", "KEY_RIGHT", "KEY_RETURN",
             "WAIT_MS=50", "KEY_RIGHT", "KEY_RETURN", "WAIT_MS=100"),
            error_code="RUNTIME_SEMANTIC_BOUNDARY",
            env_extra={"USERNAME": user, "USER": user},
        )
        stdout = str(run["stdout"])
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in stdout:
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        urls = re.findall(r"\[PLAYER_UI_TEST\] open_url=(\S+)", stdout)
        frames = run["frames"]
        assert isinstance(frames, list)
        disc = frames[0]["selected_disc"]
        repo = "https://github.com/Jstar269/nakagawa-recomp"
        # A known cause (an unsupported import, #308) leads with its issue.
        self.assertEqual(len(urls), 3, stdout)
        self.assertEqual(urls[0], f"{repo}/issues/308")
        report = urlsplit(urls[1])
        self.assertEqual(f"{report.scheme}://{report.netloc}{report.path}",
                         f"{repo}/discussions/new")
        query = parse_qs(report.query)
        self.assertEqual(query["category"], ["compatibility-reports"])
        self.assertTrue(query["title"][0].endswith(
            f"({disc}): stopped at unsupported-import-scenetadhocinit"), query["title"])
        body = query["body"][0]
        for line in (f"- Disc ID: {disc}\n", "- Nakagawa: v",
                     "- Problem: stopped at unsupported-import-scenetadhocinit\n",
                     "### What I did\n", "### What I saw\n"):
            self.assertIn(line, body)
        self.assertLessEqual(len(urls[1]), 2000)
        self.assertEqual(urls[2], f"{repo}/discussions/categories/compatibility-reports"
                                  f"?discussions_q={disc}")
        for url in urls:
            # The checkout's path, not its folder name: a checkout named after the
            # repository (as hosted CI's is) shares that name with every link here.
            for leak in ("Synthetic", "%5C", ".player-ui-test-", "localappdata", "profile",
                         ROOT.as_posix(), quote(ROOT.as_posix(), safe="")):
                self.assertNotIn(leak, url)
        self.assertEqual(frames[-1]["view"], "error", "the links keep the card open")

        # A crash with no known cause: Report this game is the first link.
        crash = self.run_player(
            "library", ("WAIT_MS=50", "KEY_DOWN", "KEY_RETURN", "WAIT_MS=100"),
            error_code="RUNTIME_ERROR_EXIT",
        )
        urls = re.findall(r"\[PLAYER_UI_TEST\] open_url=(\S+)", str(crash["stdout"]))
        self.assertEqual(len(urls), 1, crash["stdout"])
        title = parse_qs(urlsplit(urls[0]).query)["title"][0]
        self.assertTrue(title.endswith(": RUNTIME_ERROR_EXIT, exit code 3"), title)

    def test_restart_game_relaunches_the_stopped_game(self) -> None:
        """#1282: Restart game runs the same launch PLAY does for that game.

        The synthetic library game has no package, so the launch answers with
        its own Game not ready card: the restart reached the launch path.

        Restart game goes back to the library and starts the launch on the
        launch worker (#1298), so the script waits for that launch's card, not
        for a fixed time. NK_UI_TEST_SLOW_START_MS holds the launch for 600 ms,
        longer than any fixed wait the script could guess: the old WAIT_MS=100
        ended on the stopped game's card whenever a slow runner took longer
        than that to reach the package check."""
        # Right-arrow and Tab both move from the primary button to Restart game: the
        # directional move must not depend on how wide the button labels are.
        for move_key in ("KEY_RIGHT", "KEY_TAB"):
            with self.subTest(move_key=move_key):
                run = self.run_player(
                    "library", ("WAIT_MS=50", move_key, "KEY_RETURN", "WAIT_VIEW=error,10000"),
                    error_code="RUNTIME_ERROR_EXIT",
                    env_extra={"NK_UI_TEST_SLOW_START_MS": "600"},
                )
                # The player exits 4 when the launcher can't start, so a run that got here
                # drew the next UI; anything else is a failure, never a skip.
                self.assertIn("[PLAYER_UI_TEST] UI_NEXT_ACTIVE", str(run["stdout"]))
                frames = run["frames"]
                assert isinstance(frames, list)
                self.assertEqual(frames[0]["error"], "RUNTIME_ERROR_EXIT")
                pending = [frame for frame in frames if frame["launch_pending"] == "1"]
                self.assertTrue(pending, "Restart game started a launch")
                self.assertEqual(pending[0]["view"], "library", "the restart left the stopped game's card")
                self.assertEqual(frames[-1]["view"], "error", run["stdout"])
                self.assertIn(frames[-1]["error"], ("RUNTIME_PACKAGE_NOT_READY",), run["stdout"])
                self.assertEqual(frames[-1]["launch_pending"], "0")
                self.assertNotEqual(frames[-1]["error_retry"], "Restart_game")

    # Enum values of PlayerUiPackageState (src/player/ui/player_ui_nk.h).
    PACKAGE_READY = "1"
    PACKAGE_STARTING = "5"
    PACKAGE_RUNNING = "6"

    # Open the ready title: its PLAY has focus (launcher review B6), so the next
    # Enter presses it.
    PLAY_READY_TITLE = ("WAIT_MS=300", "KEY_RETURN", "WAIT_MS=300")
    TITLE_PRIMARY = str(0x2002)

    def test_play_keeps_drawing_through_a_slow_start(self) -> None:
        """#1298: PLAY answers at once and the launcher keeps drawing while the game starts.

        NK_UI_TEST_SLOW_START_MS makes the launch take 1.5 s before its package
        check, standing in for a retail package whose check hashes several
        hundred MB. Failing-before: the launch ran on the UI thread, so the
        frames stopped for the whole start and PLAY showed nothing until it
        ended (about 20 s for the owner's Hot Shots Tennis package).

        The test checks that frames keep coming through the whole start, not how
        many come: a loaded runner draws fewer frames per second (#2155)."""
        slow_start_ms = 1500
        run = self.run_player(
            "ready", (*self.PLAY_READY_TITLE, "KEY_RETURN", "WAIT_MS=2500"),
            post_build_ready=True, wait_background=True,
            env_extra={"NK_UI_TEST_SLOW_START_MS": str(slow_start_ms)},
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in str(run["stdout"]):
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        frames = run["frames"]
        assert isinstance(frames, list)
        before = [frame for frame in frames if frame["next_primary_label"] == "PLAY"]
        self.assertTrue(before, "the synthetic ready package offers PLAY")
        self.assertEqual(before[-1]["next_focus"], self.TITLE_PRIMARY,
                         "opening a title focuses PLAY, not the LIBRARY tab")
        pending = [frame for frame in frames if frame["launch_pending"] == "1"]
        self.assertGreater(len(pending), 1, run["stdout"])
        # The frame that took the press drew the title before the launch began;
        # every later one shows it starting.
        for frame in pending[1:]:
            # The label ends in an ellipsis, which the harness decodes with the
            # console code page, so compare the words only.
            self.assertTrue(frame["next_primary_label"].startswith("disabled:STARTING"),
                            frame["next_primary_label"])
            self.assertEqual(frame["next_package"], self.PACKAGE_STARTING)
        first = frames.index(pending[0])
        last = frames.index(pending[-1])
        ticks = [int(frame["ticks_ms"]) for frame in frames[first:last + 2]]
        start, end = ticks[0], ticks[-1]
        self.assertGreaterEqual(end - start, slow_start_ms - 100, "the start took its 1.5 s")
        # A launcher that stops drawing leaves one gap as long as the start; one
        # that keeps drawing, however slowly, leaves none longer than half of it.
        gaps = [later - earlier for earlier, later in zip(ticks, ticks[1:], strict=False)]
        self.assertLess(max(gaps), slow_start_ms // 2,
                        f"no frame for {max(gaps)} ms during the start; frame ticks {ticks}")
        # And it drew in every third of the start, not only at its two ends.
        pending_ticks = [int(frame["ticks_ms"]) for frame in pending]
        third = (end - start) / 3
        for part, name in enumerate(("first", "middle", "last")):
            low, high = start + part * third, start + (part + 1) * third
            self.assertTrue(any(low <= tick < high for tick in pending_ticks),
                            f"no frame drawn in the {name} third of the start; "
                            f"frame ticks {ticks}")
        # The synthetic package has no staged disc and no real program, so the
        # start ends with the launch's own card, as a synchronous launch did.
        outcome = frames[-1]
        self.assertEqual(outcome["launch_pending"], "0")
        self.assertIn(outcome["error"], ("RUNTIME_NOT_FOUND", "PROCESS_SPAWN_FAILED",
                                         "RUNTIME_PREMATURE_EXIT", "RUNTIME_ERROR_EXIT"),
                      run["stdout"])

    def test_title_opened_during_the_package_check_gets_play_focus_when_it_ends(self) -> None:
        """A title opened before its package check ends focuses PLAY once PLAY turns on.

        NK_UI_TEST_PACKAGE_STATUS_WORKER_DELAY_MS stands in for a check that hashes a large
        package. Failing-before: PLAY was off on the first frame, so focus landed on
        OPEN SAVE FOLDER and stayed there when PLAY came on; Enter (or a pad's A) then
        opened the save folder instead of starting the game."""
        run = self.run_player(
            "ready", ("WAIT_MS=300", "KEY_RETURN", "WAIT_MS=4000"),
            post_build_ready=True, wait_background=True,
            env_extra={"NK_UI_TEST_PACKAGE_STATUS_WORKER_DELAY_MS": "1500"},
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in str(run["stdout"]):
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        frames = run["frames"]
        assert isinstance(frames, list)
        labels = [frame["next_primary_label"] for frame in frames]
        self.assertTrue(any(label.startswith("disabled:") for label in labels),
                        f"PLAY was never off while the check ran: {sorted(set(labels))}")
        self.assertEqual(frames[-1]["next_primary_label"], "PLAY", run["stdout"])
        self.assertEqual(frames[-1]["next_focus"], self.TITLE_PRIMARY,
                         "PLAY has focus once the package check turns it on")

    ROUTE_REBUILD = "7"
    OUTDATED_LABEL = "Built_by_an_older_Nakagawa._Rebuild_for_the_latest_fixes."

    def test_package_built_by_an_older_nakagawa_suggests_a_rebuild_and_still_plays(self) -> None:
        """Owner decision 2026-10-29: a v0.0.1-built package is outdated, not refused.

        The fixture has v0.0.1's package format and ABI epochs (they did not
        change) and records v0.0.1's runtime source digest. The player says so
        once at start (Later here), keeps a library line, labels the title, and
        a PLAY asks once before launching (Play anyway here). Failing-before:
        nothing read the package's build identity, so the package was simply
        ready, with no prompt, label or reminder."""
        run = self.run_player(
            "ready",
            ("WAIT_MS=2500",
             "KEY_ESCAPE", "WAIT_MS=300",            # the prompt: Later
             "KEY_RETURN", "WAIT_MS=400",            # open the title
             "KEY_RETURN", "WAIT_MS=300",            # PLAY: the reminder
             "KEY_ESCAPE", "WAIT_MS=2000"),          # Play anyway
            post_build_ready=True, wait_background=True,
            package_built_by="v0.0.1",
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in str(run["stdout"]):
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        frames = run["frames"]
        assert isinstance(frames, list)
        prompted = [frame for frame in frames if frame.get("rebuild_prompt", "0") == "1"]
        self.assertTrue(prompted, run["stdout"])
        self.assertEqual(prompted[0].get("outdated", "0"), "1")
        self.assertEqual(prompted[-1]["next_route"], self.ROUTE_REBUILD)
        self.assertEqual(prompted[0].get("prompt_version", "none"), "none")
        after = frames[frames.index(prompted[-1]) + 1:]
        self.assertTrue(after and all(frame.get("rebuild_prompt", "0") == "0" for frame in after),
                        "Later closes the prompt for good")
        self.assertTrue(all(frame.get("prompt_version", "none") != "none" for frame in after),
                        "the answer is kept in settings for this version")
        self.assertIn("1", [frame.get("rebuild_banner", "0") for frame in after],
                      "the library line stays")
        titled = [frame for frame in after
                  if frame["next_route"] == "2" and frame["next_primary_label"] == "PLAY"]
        self.assertTrue(titled, run["stdout"])
        self.assertEqual(titled[0]["next_detail"], self.OUTDATED_LABEL)
        self.assertEqual(titled[0]["next_package"], self.PACKAGE_READY)
        self.assertIn("next_ui_library_status index=0 status=Older build", str(run["stdout"]))
        reminded = [frame for frame in after if frame.get("rebuild_reminder", "0") == "1"]
        self.assertTrue(reminded, "PLAY on an outdated game asks first")
        self.assertEqual(reminded[-1]["next_route"], self.ROUTE_REBUILD)
        launched = frames[frames.index(reminded[-1]) + 1:]
        self.assertIn("1", [frame["launch_pending"] for frame in launched], run["stdout"])
        self.assertTrue(all(frame.get("rebuild_queue", "0") == "0" for frame in frames),
                        "nothing was queued: Later and Play anyway rebuild nothing")
        # The synthetic package has no real program: the launch ends on the
        # launch's own card, as for any ready package (see the slow-start test).
        self.assertIn(frames[-1]["error"], ("RUNTIME_NOT_FOUND", "PROCESS_SPAWN_FAILED",
                                            "RUNTIME_PREMATURE_EXIT", "RUNTIME_ERROR_EXIT"),
                      run["stdout"])

    def test_package_built_by_this_nakagawa_is_not_outdated(self) -> None:
        """The player's own build identity matches what package_cache.py records.

        The package records this checkout's identity, computed in Python
        (source_tree_digest and the tool hashes); the player computes its own
        in C from the same tree. Any disagreement would call every package
        outdated: here nothing is, no prompt shows, and PLAY launches at once."""
        run = self.run_player(
            "ready",
            ("WAIT_MS=2500", "KEY_RETURN", "WAIT_MS=400", "KEY_RETURN", "WAIT_MS=1500"),
            post_build_ready=True, wait_background=True,
            package_built_by="current",
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in str(run["stdout"]):
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        frames = run["frames"]
        assert isinstance(frames, list)
        scanned = [frame for frame in frames if frame.get("outdated_scanned", "0") == "1"]
        self.assertTrue(scanned, run["stdout"])
        self.assertTrue(all(frame.get("outdated", "0") == "0" for frame in frames), run["stdout"])
        self.assertTrue(all(frame.get("rebuild_prompt", "0") == "0" and
                            frame.get("rebuild_reminder", "0") == "0" for frame in frames))
        self.assertIn("1", [frame["launch_pending"] for frame in frames], run["stdout"])

    def title_frame_with_a_game_running(self, running: str) -> dict[str, str]:
        """The open title's last frame while NK_UI_TEST_GAME_RUNNING=`running`.

        A running game makes quitting ask first, so the script closes twice
        with the confirmation unavailable (as the close-confirmation test does)."""
        run = self.run_player(
            "ready", ("WAIT_MS=300", "KEY_RETURN", "WAIT_MS=300", "CLOSE", "CLOSE"),
            post_build_ready=True, wait_background=True,
            env_extra={"NK_UI_TEST_GAME_RUNNING": running,
                       "NK_UI_TEST_MESSAGEBOX_FAIL": "1"},
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in str(run["stdout"]):
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        frames = run["frames"]
        assert isinstance(frames, list)
        titled = [frame for frame in frames if frame["next_package"] != "-1"]
        self.assertTrue(titled, run["stdout"])
        return titled[-1]

    # The "library" view's tiles at 1280x720: TEST00005, then TEST00006.
    FIRST_TILE = "MOUSE_MOVE=175,260"
    SECOND_TILE = "MOUSE_MOVE=485,260"

    def opened_titles(self, frames: list[dict[str, str]]) -> list[str]:
        """The disc of each title page the run opened, in order."""
        opened: list[str] = []
        previous = None
        for frame in frames:
            route = frame["next_route"]
            if route == "2" and previous != "2":
                opened.append(frame["selected_disc"])
            elif route == "2" and opened and frame["selected_disc"] != opened[-1]:
                opened.append(frame["selected_disc"])
            previous = route
        return opened

    def test_clicking_a_tile_opens_that_tile(self) -> None:
        """#1301: a click opens the tile under the pointer, not the focused one.

        Failing-before: every frame without an action handed the launcher a
        NONE action whose title_index was 0, and the launcher selected that
        index, so each title page it opened showed the first game."""
        run = self.run_player(
            "library",
            ("WAIT_MS=300", self.SECOND_TILE, "MOUSE_DOWN", "MOUSE_UP", "WAIT_MS=300"),
            wait_background=True,
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in str(run["stdout"]):
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["selected_disc"], "TEST00005", "focus starts on tile 1")
        self.assertEqual(self.opened_titles(frames), ["TEST00006"], run["stdout"])
        self.assertEqual(frames[-1]["next_route"], "2")
        self.assertEqual(frames[-1]["selected_disc"], "TEST00006")

    def test_clicking_a_tile_after_a_build_opens_that_tile(self) -> None:
        """#1301: after a build, with the built title focused, a click opens the clicked tile."""
        run = self.run_player(
            "library",
            ("WAIT_MS=300", "START_SYNTHETIC_BUILD=2", "WAIT_MS=2200",
             # Back in the library, focus the built title, as the owner had it.
             "KEY_RIGHT", "WAIT_MS=200",
             self.FIRST_TILE, "MOUSE_DOWN", "MOUSE_UP", "WAIT_MS=300",
             "KEY_ESCAPE", "WAIT_MS=300",
             self.SECOND_TILE, "MOUSE_DOWN", "MOUSE_UP", "WAIT_MS=300"),
            wait_background=True, post_build_ready=True,
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in str(run["stdout"]):
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        self.assertIn("synthetic_build result=PASS index=1 disc=TEST00006", str(run["stdout"]))
        frames = run["frames"]
        assert isinstance(frames, list)
        focused_built = [frame for frame in frames
                         if frame["view"] == "ready_library" and frame["next_route"] == "1"
                         and frame["selected_disc"] == "TEST00006"]
        self.assertTrue(focused_built, "the built title had focus before the click")
        self.assertEqual(self.opened_titles(frames), ["TEST00005", "TEST00006"], run["stdout"])

    # Control IDs from src/player/ui/player_ui_nk.h, as the frame records print them.
    TITLE_REMOVE = str(0x2004)
    REMOVE_CANCEL = str(0x4401)
    REMOVE_CONFIRM = str(0x4402)
    ROUTE_REMOVE = "4"

    def library_item(self, index: int) -> str:
        return str(0x10000 + index)

    LIBRARY_ADD = str(0x1002)

    # Remove from library is the title page's lowest control, beside Report a
    # problem: Down presses reach that row whichever action-row buttons are
    # enabled. Down lands on whichever of the two lies nearest the button above
    # (the ENHANCEMENTS card's Play mode label is wide enough to lead to either),
    # so Left settles on Report a problem and Right ends on Remove.
    DOWN_TO_REMOVE = (("KEY_DOWN", "WAIT_MS=80") * 6 +
                      ("KEY_LEFT", "WAIT_MS=80", "KEY_RIGHT", "WAIT_MS=80"))

    def remove_run(self, events: tuple[str, ...]) -> list[dict[str, str]]:
        run = self.run_player("library", events, wait_background=True)
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in str(run["stdout"]):
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        frames = run["frames"]
        assert isinstance(frames, list)
        return frames

    def test_remove_from_library_asks_then_focuses_the_next_tile(self) -> None:
        """#1304: Remove from library asks first, Cancel keeps the game, and Remove
        returns to the library with the next tile focused."""
        open_first = ("WAIT_MS=300", self.FIRST_TILE, "MOUSE_DOWN", "MOUSE_UP", "WAIT_MS=300")
        frames = self.remove_run((*open_first, *self.DOWN_TO_REMOVE, "KEY_RETURN", "WAIT_MS=200",
                                  "KEY_ESCAPE", "WAIT_MS=200"))
        dialog = [frame for frame in frames if frame["next_route"] == self.ROUTE_REMOVE]
        self.assertGreater(len(dialog), 1, "Remove from library opens the dialog")
        # The title frame that took the press pushed the route; the dialog
        # draws from the next frame on, with Cancel focused.
        self.assertEqual(dialog[1]["next_focus"], self.REMOVE_CANCEL, "Cancel has the first focus")
        self.assertEqual(frames[-1]["next_route"], "2", "Back closes the dialog onto the title page")
        self.assertEqual(frames[-1]["next_focus"], self.TITLE_REMOVE)
        self.assertEqual(frames[-1]["games"], frames[0]["games"], "Cancel removes nothing")

        frames = self.remove_run((*open_first, *self.DOWN_TO_REMOVE, "KEY_RETURN", "WAIT_MS=200",
                                  "KEY_RIGHT", "WAIT_MS=100", "KEY_RETURN", "WAIT_MS=400"))
        confirm = [frame for frame in frames if frame["next_focus"] == self.REMOVE_CONFIRM]
        self.assertTrue(confirm, "the dialog's Remove button takes focus")
        last = frames[-1]
        self.assertEqual(int(last["games"]), int(frames[0]["games"]) - 1, "the title left")
        self.assertEqual(last["next_route"], "1", "Remove returns to the library")
        self.assertEqual(last["next_at_root"], "1")
        # TEST00005 was the first tile, so TEST00006 now holds its place.
        self.assertEqual(last["next_focus"], self.library_item(0))
        self.assertEqual(last["selected_disc"], "TEST00006")

    def test_removing_the_last_tile_focuses_the_add_card(self) -> None:
        """#1304: with no tile after the removed one, focus goes to the add card."""
        frames = self.remove_run(("WAIT_MS=300", self.SECOND_TILE, "MOUSE_DOWN", "MOUSE_UP",
                                  "WAIT_MS=300", *self.DOWN_TO_REMOVE, "KEY_RETURN",
                                  "WAIT_MS=200", "KEY_RIGHT", "WAIT_MS=100", "KEY_RETURN",
                                  "WAIT_MS=400"))
        last = frames[-1]
        self.assertEqual(int(last["games"]), int(frames[0]["games"]) - 1, "the title left")
        self.assertEqual(last["next_route"], "1")
        self.assertEqual(last["next_focus"], self.LIBRARY_ADD)
        self.assertEqual(last["selected_disc"], "TEST00005")

    # The title page's ENHANCEMENTS card (player_ui_nk.h), as the frame records print them.
    TITLE_PLAY_MODE = str(0x200A)
    TITLE_PLAY_OPTIONS = str(0x200B)
    ENHANCED_PAGE_PART = str(0x4543)  # Settings > Enhanced's first part row, texture filtering
    OPEN_FIRST = ("WAIT_MS=300", FIRST_TILE, "MOUSE_DOWN", "MOUSE_UP", "WAIT_MS=300")
    # From the title page's first focus, Down reaches the ENHANCEMENTS card's Play mode button;
    # Right from there is Enhanced options.
    DOWN_TO_PLAY_MODE = ("KEY_DOWN", "WAIT_MS=80")
    TO_PLAY_OPTIONS = ("KEY_RIGHT", "WAIT_MS=80")
    # A library default of Enhanced, every part named (a part the block leaves out is off).
    LIBRARY_ENHANCED = {"enhanced": {"default": "enhanced", "af": "16", "text": "on",
                                     "readout": "off", "notice": "on", "smooth": "off"}}

    @staticmethod
    def play_badges(run: dict[str, object], disc: str) -> list[tuple[str, str]]:
        """Each (library badge, title's own choice) a run showed for `disc`, in order."""
        return re.findall(r"next_ui_play_badge index=\d+ disc=" + re.escape(disc) +
                          r" badge=(\S+) chosen=(.+)", str(run["stdout"]))

    @staticmethod
    def enhanced_pages(run: dict[str, object]) -> list[dict[str, str]]:
        """Each Settings > Enhanced the run drew: whose page, its play mode and each row."""
        pages = []
        for line in str(run["stdout"]).splitlines():
            if line.startswith("[PLAYER_UI_TEST] next_ui_enhanced_page "):
                fields = line[len("[PLAYER_UI_TEST] next_ui_enhanced_page "):].split("|")
                pages.append(dict(field.split("=", 1) for field in fields))
        return pages

    @staticmethod
    def title_play(run: dict[str, object]) -> dict[str, object]:
        settings = run["settings"]
        assert isinstance(settings, dict), run["stdout"]
        title_play = settings.get("title_play", {})
        assert isinstance(title_play, dict)
        return title_play

    def test_next_ui_title_play_mode_button_steps_its_modes(self) -> None:
        """#2055: the title page's Play mode button steps Library default, Authentic, Enhanced
        and, only for a game with a package of its own, Ace; the choice is saved for that game
        alone, and the library card's badge follows it."""
        presses = ("KEY_RETURN", "WAIT_MS=120") * 4
        run = self.next_ui_run(
            "library", (*self.OPEN_FIRST, *self.DOWN_TO_PLAY_MODE, *presses),
            wait_background=True)
        self.assertIn(self.TITLE_PLAY_MODE, self.focus_trail(run), "Down reaches Play mode")
        chosen = [mode for _, mode in self.play_badges(run, "TEST00005")]
        self.assertEqual(chosen, ["Library default", "Authentic", "Enhanced", "Library default",
                                  "Authentic"],
                         "without a package of its own, Ace is skipped")
        badges = [badge for badge, _ in self.play_badges(run, "TEST00005")]
        self.assertEqual(badges, ["none", "none", "Enhanced", "none", "none"])
        self.assertEqual(self.title_play(run), {"TEST00005": {"mode": "authentic"}},
                         "the choice is saved for that game alone")
        self.assertEqual(self.play_badges(run, "TEST00006"), [("none", "Library default")],
                         "the other game keeps the library's default")

        # A sample game never has a package (player_read_title_enhancement), so the packaged
        # case is the "ready" fixture's TEST00006, opened with Enter.
        packaged = self.next_ui_run(
            "ready", ("WAIT_MS=300", "KEY_RETURN", "WAIT_MS=300", *self.DOWN_TO_PLAY_MODE,
                      *presses),
            post_build_ready=True, wait_background=True,
            enhancement_packages={"TEST00006": "display-smoke-v1"})
        self.assertIn(self.TITLE_PLAY_MODE, self.focus_trail(packaged),
                      [hex(int(focus)) for focus in self.focus_trail(packaged)])
        steps = self.play_badges(packaged, "TEST00006")
        self.assertEqual(steps, [("none", "Library default"), ("none", "Authentic"),
                                 ("Enhanced", "Enhanced"), ("Ace", "Ace"),
                                 ("none", "Library default")],
                         "with its package, Ace follows Enhanced and the badge says Ace")
        self.assertEqual(self.title_play(packaged), {}, "back to the library: nothing is kept")

    def test_next_ui_title_enhanced_options_are_that_games_and_back_returns(self) -> None:
        """#2055: Enhanced options opens Settings > Enhanced for that game: its rows show the
        game's own values and change only them, and Escape returns to its title page with
        Enhanced options focused."""
        run = self.next_ui_run(
            "library",
            (*self.OPEN_FIRST, *self.DOWN_TO_PLAY_MODE, *self.TO_PLAY_OPTIONS,
             "KEY_RETURN", "WAIT_MS=200", "KEY_DOWN", "WAIT_MS=80",
             "KEY_RETURN", "WAIT_MS=120", "KEY_RETURN", "WAIT_MS=120",
             "KEY_ESCAPE", "WAIT_MS=200"),
            wait_background=True, settings=self.LIBRARY_ENHANCED)
        trail = self.focus_trail(run)
        self.assertIn(self.TITLE_PLAY_OPTIONS, trail, "Right reaches Enhanced options")
        self.assertIn(self.ENHANCED_PAGE_PART, trail, "Down reaches the first part's row")
        self.assertIn("ace_settings", [view for view, _ in self.screens(run)])
        pages = self.enhanced_pages(run)
        self.assertTrue(pages, run["stdout"])
        self.assertEqual({page["title"] for page in pages}, {"TEST00005"},
                         "the page is that game's")
        self.assertEqual(pages[0]["af"], "Library (16x)", "a part follows the library first")
        self.assertIn("Ace needs a package made for this game.", pages[0]["sentence"],
                      "a game with no package of its own says why it has no Ace")
        self.assertEqual(pages[-1]["af"], "8x", "the row shows the game's own value")
        title_play = self.title_play(run)
        self.assertEqual(title_play, {"TEST00005": {"mode": "library", "af": "8"}})
        settings = run["settings"]
        assert isinstance(settings, dict)
        self.assertEqual(settings["enhanced"]["af"], "16", "the library's value is kept")
        last = run["frames"][-1]  # type: ignore[index]
        self.assertEqual((last["view"], last["next_route"]), ("library", "2"),
                         "Escape returns to the title page, not Settings")
        self.assertEqual(last["next_focus"], self.TITLE_PLAY_OPTIONS,
                         "with Enhanced options focused")

    def test_next_ui_enhanced_options_do_nothing_while_a_game_runs(self) -> None:
        """#2055: while a game runs, the title page's Enhanced options and Play mode change
        nothing (NK_UI_TEST_GAME_RUNNING fakes the running game)."""
        run = self.next_ui_run(
            "library",
            (*self.OPEN_FIRST, *self.DOWN_TO_PLAY_MODE, "KEY_RETURN", "WAIT_MS=120",
             *self.TO_PLAY_OPTIONS, "KEY_RETURN", "WAIT_MS=200", "CLOSE", "CLOSE"),
            wait_background=True,
            env_extra={"NK_UI_TEST_GAME_RUNNING": "TEST00006",
                       "NK_UI_TEST_MESSAGEBOX_FAIL": "1"})
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertTrue(all(frame["game_running"] == "1" for frame in frames))
        self.assertEqual({view for view, _ in self.screens(run)}, {"library"},
                         "Enhanced options opens nothing while a game runs")
        self.assertEqual([mode for _, mode in self.play_badges(run, "TEST00005")],
                         ["Library default"], "Play mode changes nothing while a game runs")

    def smooth_at_60_run(self, title: dict[str, str],
                         to_options: tuple[str, ...]) -> tuple[list[str], dict[str, str]]:
        """The packaged TEST00006 with smooth motion on in the library and `title` as its own
        play choice: its title page's play lines, then its Settings > Enhanced page, which
        `to_options` reaches from the title page's first focus."""
        settings = {"enhanced": {**self.LIBRARY_ENHANCED["enhanced"], "smooth": "x2"},
                    "title_play": {"TEST00006": title}}
        run = self.next_ui_run(
            "ready", ("WAIT_MS=300", "KEY_RETURN", "WAIT_MS=300", *to_options,
                      "KEY_RETURN", "WAIT_MS=300"),
            post_build_ready=True, wait_background=True, settings=settings,
            enhancement_packages={"TEST00006": "display-smoke-v1"})
        trail = [hex(int(focus)) for focus in self.focus_trail(run)]
        self.assertIn(hex(int(self.TITLE_PLAY_OPTIONS)), trail, trail)
        lines = re.findall(r"next_ui_title_play_line disc=TEST00006 line=(.+)",
                           str(run["stdout"]))
        pages = self.enhanced_pages(run)
        # The title page's arrows move by layout, so a path that stops on another control
        # (Play mode steps the mode instead) shows here with the focus trail.
        self.assertTrue(lines and pages, f"Enhanced options not opened; focus trail {trail}")
        self.assertEqual(pages[-1]["title"], "TEST00006", run["stdout"])
        return lines, pages[-1]

    @staticmethod
    def enhancement_tags(run: dict[str, object], disc: str) -> list[str]:
        """Each tag the ENHANCEMENTS card took for `disc`, in order, from the title model."""
        return re.findall(r"next_ui_enhancement_tag disc=" + re.escape(disc) + r" tag=(\S+)",
                          str(run["stdout"]))

    def test_next_ui_enhancement_tag_reads_optional_only_for_the_released_shipped_package(
            self) -> None:
        """Contract section 9.10 (#1732): the ENHANCEMENTS card's tag comes from where the
        title's package was found. A package in the source tree's assets/enhancements that
        declares "experimental": false reads Optional; the same package without the flag, or
        a copy in the user's data folder that declares it, reads Experimental."""
        # The "ready" fixture reads the title's package when its play mode changes, so the run
        # opens the title and steps Play mode once (to Authentic; the card stays for the package).
        events = ("WAIT_MS=300", "KEY_RETURN", "WAIT_MS=300", *self.DOWN_TO_PLAY_MODE,
                  "KEY_RETURN", "WAIT_MS=200")
        package = {"id": "fixture-60hz", "target_title_id": "display-smoke-v1"}
        released = {"experimental": False}
        own = self.next_ui_run("ready", events, post_build_ready=True, wait_background=True,
                               enhancement_packages={"TEST00006": "display-smoke-v1"},
                               enhancement_package_fields=released)
        self.assertEqual(self.enhancement_tags(own, "TEST00006")[-1:], ["Experimental"],
                         own["stdout"])
        with tempfile.TemporaryDirectory(prefix=".shipped-package-", dir=ROOT) as tmp:
            source = Path(tmp)
            (source / "tools").mkdir()
            (source / "tools" / "nk_cli.py").write_text("# fixture\n", encoding="utf-8")
            shipped = source / "assets" / "enhancements"
            shipped.mkdir(parents=True)
            leaf = shipped / "test00006-60hz.json"
            for fields, tag in ((released, "Optional"), ({}, "Experimental")):
                leaf.write_text(json.dumps({**package, **fields}), encoding="utf-8")
                run = self.next_ui_run("ready", events, post_build_ready=True,
                                       wait_background=True,
                                       env_extra={"NK_INSTALL_ROOT": str(source)})
                self.assertEqual(self.enhancement_tags(run, "TEST00006")[-1:], [tag],
                                 run["stdout"])

    def test_next_ui_smooth_motion_says_why_it_is_off_at_60_fps(self) -> None:
        """Contract 10.5 (#2085; #2074 delta review B1): a game launched with its 60 FPS game
        logic runs with smooth motion off, and its launch line says smooth=off. Its title page
        and its Settings > Enhanced page say so and why, instead of showing smooth motion on;
        with that logic off, both show the choice as it is."""
        # The "ready" fixture does not read the saved Ace's package at start, so PLAY is
        # disabled and the title page opens on Saves; Down from there reaches Enhanced options,
        # the control below it in the ENHANCEMENTS row.
        lines, page = self.smooth_at_60_run({"mode": "ace"}, ("KEY_DOWN", "WAIT_MS=80"))
        self.assertIn("Ace for this game: 60 FPS game logic", lines[-1])
        self.assertIn("Smooth motion is off while it runs at 60 FPS", lines[-1])
        self.assertEqual(page["smooth"], "Off (this game runs at 60 FPS)")

        lines, page = self.smooth_at_60_run({"mode": "ace", "logic_60": "off"},
                                            (*self.DOWN_TO_PLAY_MODE, *self.TO_PLAY_OPTIONS))
        self.assertNotIn("60 FPS game logic", lines[-1])
        self.assertIn("smooth motion", lines[-1], "smooth motion is among the parts on")
        self.assertNotIn("Smooth motion is off", lines[-1])
        self.assertEqual(page["smooth"], "Library (On)")

    EVERY_PART_ON = {"default": "enhanced", "af": "16", "text": "on", "readout": "on",
                     "notice": "on", "wide": "fill", "smooth": "x2", "sas": "sinc", "tess": "2",
                     "cull": "on", "upscale": "fsr1"}
    EVERY_PART_WORDS = ("16x texture filtering", "crisp system text", "speed readout",
                        "game badge", "widescreen", "clean pitched sound", "2x finer patches",
                        "wider culling", "sharper enlarging")

    def every_part_play_line(self, title: dict[str, str]) -> str:
        """The title page's play line for the packaged TEST00006 with every Enhanced part on in
        the library and `title` as its own play choice: the line player_ui_nk.c builds."""
        settings = {"enhanced": dict(self.EVERY_PART_ON), "title_play": {"TEST00006": title}}
        run = self.next_ui_run(
            "ready", ("WAIT_MS=300", "KEY_RETURN", "WAIT_MS=300"),
            post_build_ready=True, wait_background=True, settings=settings,
            enhancement_packages={"TEST00006": "display-smoke-v1"})
        lines = re.findall(r"next_ui_title_play_line disc=TEST00006 line=(.+?)\r?$",
                           str(run["stdout"]), re.M)
        self.assertTrue(lines, run["stdout"])
        return lines[-1]

    def test_next_ui_play_line_names_every_part(self) -> None:
        """The game page's play line with every part on names each one and ends whole: on main
        the parts list (160 bytes) cut "sharper enlarging" to "sharper e", and the Ace line (256 bytes) was
        cut mid-word, with no ellipsis for the page test to see."""
        enhanced = self.every_part_play_line({"mode": "enhanced"})
        self.assertTrue(enhanced.startswith("Enhanced for this game: "), enhanced)
        self.assertTrue(enhanced.endswith("smooth motion, clean pitched sound, 2x finer patches, "
                                          "wider culling, sharper enlarging."), enhanced)
        for words in self.EVERY_PART_WORDS:
            self.assertIn(words, enhanced)
        ace = self.every_part_play_line({"mode": "ace"})
        self.assertTrue(ace.startswith("Ace for this game: 60 FPS game logic, "), ace)
        self.assertTrue(ace.endswith("Smooth motion is off while it runs at 60 FPS: this build "
                                     "smooths only 30 FPS games."), ace)
        for words in self.EVERY_PART_WORDS:
            self.assertIn(words, ace)

    def test_compatibility_records_carry_a_player_tier(self) -> None:
        """#1297: every title record maps to a player tier; Hot Shots Tennis is Playable.

        The category -> tier table itself is pinned against docs/COMPATIBILITY.md
        by test_player_screens.c; this checks the per-title records the
        launcher really uses."""
        run = self.run_player(
            "library", ("WAIT_MS=100",),
            env_extra={"NK_UI_TEST_REPORT_COMPATIBILITY": "1"},
        )
        records = {}
        for line in str(run["stdout"]).splitlines():
            match = re.match(
                r"\[PLAYER_UI_TEST\] compatibility key=(\S+) category_tier=(.+?) tier=(.+)$",
                line,
            )
            if match:
                records[match.group(1)] = (match.group(2), match.group(3))
        self.assertIn("UCUS98701", records, run["stdout"])
        tiers = {"Perfect", "Playable", "In-game", "Menu", "Title screen", "Won't boot"}
        for key, (category_tier, tier) in records.items():
            with self.subTest(key=key):
                self.assertIn(category_tier, tiers, "the record's docs category has a tier")
                self.assertIn(tier, tiers)
        self.assertEqual(records["UCUS98701"], ("Title screen", "Playable"))
        self.assertEqual(records["UCUS-98701"], ("Title screen", "Playable"))
        self.assertEqual(records["PAC-MAN_Championship_Edition"][1], "In-game")
        self.assertEqual(records["pspdev-phase5-v1"][1], "Won't boot")

    def test_title_page_while_the_game_runs(self) -> None:
        """#1297: the running game's title page follows the package, not the STOP GAME action.

        The drawn texts are checked by test_player_screens.c; this pins the
        state the launcher hands the screen."""
        cases = {
            "TEST00006": ("STOP_GAME", self.PACKAGE_RUNNING),
            "starting:TEST00006": ("STOP_GAME", self.PACKAGE_STARTING),
        }
        for running, (label, state) in cases.items():
            with self.subTest(running=running):
                last = self.title_frame_with_a_game_running(running)
                self.assertEqual(last["selected_disc"], "TEST00006")
                self.assertEqual(last["selected_runtime"], "1", "the package is ready")
                self.assertEqual(last["next_primary_label"], label)
                self.assertEqual(last["next_package"], state)
                self.assertNotIn("boundary", last["next_expect"])
        # Another game running leaves this title's package card alone.
        other = self.title_frame_with_a_game_running("TEST00001")
        self.assertEqual(other["next_package"], self.PACKAGE_READY)

    STOP_ROUTE = "6"  # PLAYER_UI_ROUTE_STOP
    STOP_KEEP = str(0x4411)  # PLAYER_UI_ID_STOP_KEEP

    def test_stop_game_asks_first(self) -> None:
        """The walkthrough: STOP GAME ended the game at once. It asks first.

        Enter on STOP GAME opens the question with KEEP PLAYING focused; Esc,
        and Enter on KEEP PLAYING, go back with the game still running; only
        STOP GAME in the question stops it. Failing-before: the first Enter
        stopped the game, and no frame showed the question's route."""
        run = self.run_player(
            "ready",
            ("WAIT_MS=300", "KEY_RETURN", "WAIT_MS=300",  # open the running game's title page
             "KEY_RETURN", "WAIT_MS=300", "KEY_ESCAPE", "WAIT_MS=300",  # ask, then Esc
             "KEY_RETURN", "WAIT_MS=300", "KEY_RETURN", "WAIT_MS=300",  # ask, then KEEP PLAYING
             "KEY_RETURN", "WAIT_MS=300", "KEY_RIGHT", "KEY_RETURN", "WAIT_MS=300",  # STOP GAME
             "CLOSE", "CLOSE"),
            post_build_ready=True, wait_background=True,
            env_extra={"NK_UI_TEST_GAME_RUNNING": "TEST00006",
                       "NK_UI_TEST_MESSAGEBOX_FAIL": "1"},
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in str(run["stdout"]):
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        frames = run["frames"]
        assert isinstance(frames, list)
        # Each change of route, focus or running state, in order.
        steps: list[tuple[str, str, str]] = []
        for frame in frames:
            step = (frame["next_route"], frame["next_focus"], frame["game_running"])
            if not steps or steps[-1] != step:
                steps.append(step)
        asked = [i for i, (route, _focus, _running) in enumerate(steps) if route == self.STOP_ROUTE]
        self.assertTrue(asked, f"STOP GAME never asked: {steps}")
        first = steps[asked[0]]
        self.assertEqual(first[2], "1", "the game stopped before the question")
        on_keep = [step for step in steps if step[0] == self.STOP_ROUTE and step[1] == self.STOP_KEEP]
        self.assertTrue(on_keep, f"KEEP PLAYING never had the focus: {steps}")
        stopped = [i for i, step in enumerate(steps) if step[2] == "0"]
        self.assertTrue(stopped, f"STOP GAME in the question did not stop the game: {steps}")
        before_stop = steps[:stopped[0]]
        # Esc and KEEP PLAYING each went back to the title page with the game running.
        returns = [i for i in range(1, len(before_stop))
                   if before_stop[i][0] == "2" and before_stop[i - 1][0] == self.STOP_ROUTE]
        self.assertEqual(len(returns), 2, f"Esc and KEEP PLAYING did not both return: {steps}")
        self.assertTrue(all(step[2] == "1" for step in before_stop))
        # The game stopped from the question, which then closed.
        self.assertEqual(steps[stopped[0] - 1][0], self.STOP_ROUTE)
        self.assertEqual(steps[stopped[0]][0], "2")

    def game_exit_steps(self, exit_code: int,
                        before_exit: tuple[str, ...] = ()) -> list[tuple[str, str, str, str, str]]:
        """Each change of (route, focus, running, view, error) while a game run from its
        title page ends with `exit_code`, then Esc twice. `before_exit` runs on the page
        first."""
        run = self.run_player(
            "ready",
            ("WAIT_MS=300", "KEY_RETURN", "WAIT_MS=700",  # open the running game's title page
             *before_exit,
             f"GAME_EXIT={exit_code}", "WAIT_MS=300",  # the game ends on its own
             "KEY_ESCAPE", "WAIT_MS=300", "KEY_ESCAPE", "WAIT_MS=300", "CLOSE"),
            post_build_ready=True, wait_background=True,
            env_extra={"NK_UI_TEST_GAME_RUNNING": "TEST00006"},
        )
        if "[PLAYER_UI_TEST] UI_NEXT_ACTIVE" not in str(run["stdout"]):
            self.skipTest("the next UI needs SDL3_ttf, which this host lacks")
        self.assertIn(f"game_exit code={exit_code} result=PASS", str(run["stdout"]))
        frames = run["frames"]
        assert isinstance(frames, list)
        steps: list[tuple[str, str, str, str, str]] = []
        for frame in frames:
            step = (frame["next_route"], frame["next_focus"], frame["game_running"],
                    frame["view"], frame["error"])
            if not steps or steps[-1] != step:
                steps.append(step)
        return steps

    def test_a_game_that_ends_returns_to_its_title_page(self) -> None:
        """#2002: Quit to library ends the game cleanly, and the launcher is still on that
        game's page, with its focus: the run never left the page, and the exit is reaped
        without a view change (player_app_monitor_game_session). Focus is moved off the
        page's first control before the exit, so a reset to it cannot pass as kept."""
        steps = self.game_exit_steps(0, ("KEY_DOWN", "WAIT_MS=200"))
        ended = [i for i, step in enumerate(steps) if step[2] == "0"]
        self.assertTrue(ended, f"the game never ended: {steps}")
        running = steps[ended[0] - 1]
        after = steps[ended[0]]
        opened = next(step for step in steps if step[0] == "2")
        self.assertEqual(running[0], "2", f"the game was not run from its page: {steps}")
        self.assertNotEqual(running[1], opened[1], f"Down did not move the focus: {steps}")
        self.assertEqual(after[0], "2", f"the title page did not survive the exit: {steps}")
        self.assertEqual(after[1], running[1], f"the page lost its focus: {steps}")
        self.assertEqual(after[3], running[3], f"a clean exit changed the view: {steps}")
        self.assertEqual(after[4], "NONE", f"a clean exit raised a card: {steps}")
        # Esc then leaves the page for the library, and the second Esc stays there.
        later = steps[ended[0] + 1:]
        self.assertTrue(later, f"Esc did not leave the page: {steps}")
        self.assertEqual(later[0][0], "1", f"Esc did not return to the library: {steps}")
        self.assertEqual(later[-1][0], "1", f"the second Esc left the library: {steps}")
        self.assertTrue(all(step[2] == "0" for step in later), steps)

    def test_a_game_that_crashes_shows_its_card_over_its_title_page(self) -> None:
        """#2002: a game that exits with an error shows its card, and leaving the card
        lands on the same game's page."""
        steps = self.game_exit_steps(3)
        ended = [i for i, step in enumerate(steps) if step[2] == "0"]
        self.assertTrue(ended, f"the game never ended: {steps}")
        after = steps[ended[0]]
        self.assertEqual(after[3], "error", f"a crash showed no card: {steps}")
        self.assertEqual(after[4], "RUNTIME_ERROR_EXIT", f"the wrong card: {steps}")
        self.assertEqual(after[0], "2", f"the title page went under the card: {steps}")
        dismissed = [step for step in steps[ended[0]:] if step[3] != "error"]
        self.assertTrue(dismissed, f"Esc did not leave the card: {steps}")
        self.assertEqual(dismissed[0][0], "2", f"leaving the card left the page: {steps}")

    def test_preflight_refusal_is_not_a_build_failure(self) -> None:
        """#1286: a build refused at preflight never started, so its card is not
        "Package Build Failed" and its badge is not a code."""
        run = self.run_player("library", ("WAIT_MS=50",), error_code="PACKAGE_BUILD_REFUSED")
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["view"], "error")
        self.assertEqual(frames[0]["error_title"], "Build_not_started")
        self.assertEqual(frames[0]["error_badge"], "Build_not_started")
        self.assertEqual(frames[0]["error_retry"], "Try_again")
        # Every code the player raises has its own badge (#2007)...
        named = self.run_player("library", ("WAIT_MS=50",), error_code="PYTHON_NOT_FOUND")
        named_frames = named["frames"]
        assert isinstance(named_frames, list)
        self.assertEqual(named_frames[0]["error_badge"], "Build_tools_needed")
        # ...and a code it has never heard of still gets a plain badge.
        other = self.run_player("library", ("WAIT_MS=50",), error_code="SOME_UNKNOWN_CODE")
        other_frames = other["frames"]
        assert isinstance(other_frames, list)
        self.assertEqual(other_frames[0]["error_badge"], "Problem")

    def test_failed_close_confirmation_needs_a_second_explicit_close(self) -> None:
        run = self.run_player(
            "library",
            ("CLOSE", "CLOSE"),
            env_extra={
                "NK_UI_TEST_GAME_RUNNING": "1",
                "NK_UI_TEST_MESSAGEBOX_FAIL": "1",
            },
        )
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[0]["game_running"], "1")
        self.assertEqual(frames[-1]["game_running"], "1")
        self.assertEqual(frames[-1]["running"], "0")
        self.assertIn("Repeat the close request", run["stderr"])
        self.assertIn("explicit force-quit", run["stderr"])
        # The first close asked; the second, after the box failed, did not.
        dialogs = self.quit_dialogs(str(run["stdout"]))
        self.assertEqual(len(dialogs), 1, run["stdout"])
        self.assertEqual(dialogs[0]["message"],
                         "A game is still running.\nQuitting closes it, and anything "
                         "since the game's last save is lost.")

    # SDL3's Windows message box does not widen a button for a long label: it
    # showed "Close game and quit" as "ose game and qu". Labels this short fit.
    DIALOG_BUTTON_LABEL_MAX = 12

    def quit_dialogs(self, stdout: str) -> list[dict[str, object]]:
        """Each quit confirmation the player asked, as the host box would show it,
        with its buttons checked against what fits and what is the default.

        The player writes the message's line break as a backslash and an n, to
        keep each report on one line; this turns it back into a line break."""
        dialogs: list[dict[str, object]] = []
        prefix = "[PLAYER_UI_TEST] QUIT_DIALOG "
        for line in stdout.splitlines():
            if not line.startswith(prefix):
                continue
            body = line[len(prefix):]
            if body.startswith("title="):
                dialogs.append({"title": body[len("title="):], "buttons": []})
            elif body.startswith("message="):
                dialogs[-1]["message"] = body[len("message="):].replace("\\n", "\n")
            else:
                match = re.fullmatch(
                    r"button id=(\d+) return_default=([01]) escape_default=([01]) label=(.*)",
                    body)
                self.assertIsNotNone(match, line)
                assert match is not None
                buttons = dialogs[-1]["buttons"]
                assert isinstance(buttons, list)
                buttons.append(match.groups())
        for dialog in dialogs:
            self.assertEqual(dialog["title"], "Quit Nakagawa?")
            # Cancel is the Return and Escape default, so a stray Enter never
            # ends the game; Quit is button 1, which the player acts on.
            self.assertEqual(dialog["buttons"],
                             [("0", "1", "1", "Cancel"), ("1", "0", "0", "Quit")])
            buttons = dialog["buttons"]
            assert isinstance(buttons, list)
            for button in buttons:
                self.assertLessEqual(len(button[3]), self.DIALOG_BUTTON_LABEL_MAX, button)
        return dialogs

    def test_quit_confirmation_names_the_running_game(self) -> None:
        run = self.run_player(
            "library",
            ("CLOSE",),
            env_extra={
                "NK_UI_TEST_GAME_RUNNING": "TEST00005",
                "NK_UI_TEST_MESSAGEBOX_FAIL": "1",
            },
        )
        dialogs = self.quit_dialogs(str(run["stdout"]))
        self.assertEqual(len(dialogs), 1, run["stdout"])
        self.assertEqual(dialogs[0]["message"],
                         "Nakagawa Test Program is still running.\nQuitting "
                         "closes it, and anything since the game's last save is lost.")
        frames = run["frames"]
        assert isinstance(frames, list)
        self.assertEqual(frames[-1]["game_running"], "1")

def library_disc_ids(scratch: Path) -> list[str]:
    """Disc IDs in every library.json this run wrote below its isolated LOCALAPPDATA."""
    ids: list[str] = []
    for path in scratch.rglob("library.json"):
        data = json.loads(path.read_text(encoding="utf-8"))
        ids.extend(entry["disc_id"] for entry in data.get("games", []))
    return ids


class FirstRunRouteTests(unittest.TestCase):
    """First-run routes: --iso= persistence and --launch-index on an empty library.

    An inspected disc is stored only when the user chooses ADD TO LIBRARY, as the
    file picker and setup wizard already do; --launch-now is the exception, since it
    must launch and a launch needs a library record. --launch-index names a library
    entry, so an empty library is an error and bundled demo entries are named SAMPLE.
    """

    def scratch_env(self, scratch: Path) -> dict[str, str]:
        profile = scratch / "profile"
        for name in ("appdata", "localappdata", "profile"):
            (scratch / name).mkdir(exist_ok=True)
        env = os.environ.copy()
        env.update(
            {
                "SDL_VIDEODRIVER": "dummy",
                "SDL_RENDER_DRIVER": "software",
                "SDL_AUDIODRIVER": "dummy",
                "APPDATA": str(scratch / "appdata"),
                "LOCALAPPDATA": str(scratch / "localappdata"),
                "USERPROFILE": str(profile),
                "HOME": str(profile),
                "XDG_CONFIG_HOME": str(profile / "config"),
                "XDG_DATA_HOME": str(profile / "data"),
                "XDG_CACHE_HOME": str(profile / "cache"),
            }
        )
        return env

    def run_route(self, scratch: Path, args: Sequence[str]) -> subprocess.CompletedProcess:
        return subprocess.run(
            [str(PLAYER_EXE), *args, f"--runtime-root={scratch / 'runtime'}"],
            cwd=ROOT, env=self.scratch_env(scratch), capture_output=True, text=True,
            timeout=120, check=False,
        )

    def test_bad_startup_iso_opens_error_card_and_keeps_launcher_running(self) -> None:
        with tempfile.TemporaryDirectory(prefix=".first-run-invalid-iso-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            bad_iso = scratch / "not a PSP disc.iso"
            bad_iso.write_bytes(b"synthetic non-ISO input\n")
            runtime_root = scratch / "runtime"
            runtime_root.mkdir()
            screenshot = scratch / "last-frame.bmp"
            completed = subprocess.run(
                [
                    str(PLAYER_EXE), f"--iso={bad_iso}",
                    "--ui-test-events=WAIT_VIEW=error,5000;KEY_RETURN;WAIT_MS=100;QUIT",
                    f"--ui-test-screenshot={screenshot}",
                    f"--runtime-root={runtime_root}",
                ],
                cwd=ROOT, env=self.scratch_env(scratch), capture_output=True,
                text=True, timeout=30, check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            frames = parse_frames(completed.stdout)
            self.assertTrue(frames, completed.stdout + completed.stderr)
            error_frame = next((frame for frame in frames if frame["view"] == "error"), None)
            self.assertIsNotNone(error_frame, completed.stdout + completed.stderr)
            assert error_frame is not None
            self.assertEqual(error_frame["view"], "error")
            self.assertEqual(error_frame["error"], "ISO_CORRUPT")
            self.assertEqual(error_frame["bad_disc_notice"], "1")
            self.assertEqual(frames[-1]["view"], "library")
            self.assertEqual(frames[-1]["picker"], "1")
            self.assertTrue(screenshot.is_file())

    def test_iso_route_stores_nothing_until_add_to_library(self) -> None:
        sys.path.insert(0, str(ROOT / "tools"))
        from test_iso_parity import create_test_iso

        with tempfile.TemporaryDirectory(prefix=".first-run-iso-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            iso = scratch / "supported.iso"
            create_test_iso(iso, disc_id="TEST00002", title="Café® ™",
                            volume_id="TEST00001")
            completed = self.run_route(scratch, [
                f"--iso={iso}",
                "--ui-test-events=WAIT_VIEW=supported,10000;QUIT",
                f"--ui-test-screenshot={scratch / 'last-frame.bmp'}",
            ])
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            views = [frame["view"] for frame in parse_frames(completed.stdout)]
            self.assertIn("supported", views, completed.stdout)
            self.assertEqual(library_disc_ids(scratch), [],
                             "the card still offers ADD TO LIBRARY, so nothing may be stored yet")

    def test_add_to_library_on_the_iso_card_stores_the_disc(self) -> None:
        sys.path.insert(0, str(ROOT / "tools"))
        from test_iso_parity import create_test_iso

        with tempfile.TemporaryDirectory(prefix=".first-run-add-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            iso = scratch / "supported.iso"
            create_test_iso(iso, disc_id="TEST00002", title="Café® ™",
                            volume_id="TEST00001")
            completed = self.run_route(scratch, [
                f"--iso={iso}",
                "--ui-test-events=WAIT_VIEW=supported,10000;KEY_RETURN;WAIT_VIEW=library,10000;QUIT",
                f"--ui-test-screenshot={scratch / 'last-frame.bmp'}",
            ])
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            self.assertEqual(library_disc_ids(scratch), ["TEST00002"])

    def test_add_to_library_on_a_compressed_disc_stores_its_expansion(self) -> None:
        """A compressed disc given as --iso is expanded once into the per-title folder;
        ADD TO LIBRARY stores the expanded .iso for every later step and the picked file
        for the user. Adding the same unchanged file again reuses the expansion."""
        sys.path.insert(0, str(ROOT / "tools"))
        from test_disc_expand import build_cso
        from test_iso_parity import create_test_iso

        with tempfile.TemporaryDirectory(prefix=".first-run-compressed-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            iso = scratch / "supported.iso"
            create_test_iso(iso, disc_id="TEST00002", title="Compressed Disc",
                            volume_id="TEST00001")
            cso = scratch / "supported.cso"
            cso.write_bytes(build_cso(iso.read_bytes()))
            events = "--ui-test-events=WAIT_VIEW=supported,15000;KEY_RETURN;WAIT_VIEW=library,10000;QUIT"
            completed = self.run_route(scratch, [
                f"--iso={cso}", events, f"--ui-test-screenshot={scratch / 'last-frame.bmp'}",
            ])
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            self.assertIn("[PLAYER] Expanding disc image cso", completed.stdout)
            libraries = list(scratch.rglob("library.json"))
            self.assertEqual(len(libraries), 1)
            games = json.loads(libraries[0].read_text(encoding="utf-8"))["games"]
            self.assertEqual([game["disc_id"] for game in games], ["TEST00002"])
            expanded = Path(games[0]["iso_path"])
            self.assertEqual(expanded.name, "TEST00002.cso.iso")
            self.assertEqual(expanded.parent.name, "TEST00002")
            self.assertEqual(expanded.parent.parent.name, "discs")
            self.assertEqual(expanded.read_bytes(), iso.read_bytes())
            self.assertEqual(Path(games[0]["source_image_path"]), cso)

            again = self.run_route(scratch, [
                f"--iso={cso}", "--ui-test-events=WAIT_VIEW=supported,15000;QUIT",
                f"--ui-test-screenshot={scratch / 'last-frame.bmp'}",
            ])
            self.assertEqual(again.returncode, 0, again.stdout + again.stderr)
            self.assertNotIn("[PLAYER] Expanding disc image", again.stdout)
            self.assertIn("supported", [frame["view"] for frame in parse_frames(again.stdout)])

    def test_add_to_library_sets_up_an_archive_disc_before_saving_it(self) -> None:
        """ADD TO LIBRARY on an archive disc runs the wizard's staging worker.

        Failing-before: the card saved the disc without its files, so PLAY then
        found the game's data missing. Now the card hands the disc to the same
        staging transaction (progress, cancel, retry, calm messages) and the
        game reaches the library only with its files in place.
        """
        sys.path.insert(0, str(ROOT / "tools"))
        from test_iso_parity import (
            archive_title_manifest,
            build_plain_mips_elf,
            create_archive_title_iso,
        )

        with tempfile.TemporaryDirectory(prefix=".first-run-card-archive-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            iso = scratch / "archive.iso"
            create_archive_title_iso(iso, disc_id="ULUS99996", title="Synthetic Archive",
                                     executable=build_plain_mips_elf())
            user_root = scratch / "user-data"
            (user_root / "manifests").mkdir(parents=True)
            (user_root / "manifests" / "archive.json").write_text(
                json.dumps(archive_title_manifest("ULUS99996", "archive-ulus99996")),
                encoding="utf-8",
            )
            completed = subprocess.run(
                [str(PLAYER_EXE), f"--user-data-root={user_root}", f"--iso={iso}",
                 "--ui-test-events=WAIT_VIEW=supported,10000;KEY_RETURN;"
                 "WAIT_VIEW=ready_library,30000;QUIT",
                 f"--ui-test-screenshot={scratch / 'last-frame.bmp'}"],
                cwd=ROOT, env=self.scratch_env(scratch), capture_output=True, text=True,
                timeout=120, check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            views = [frame["view"] for frame in parse_frames(completed.stdout)]
            self.assertIn("supported", views, completed.stdout)
            self.assertEqual(views[-1], "ready_library", completed.stdout)
            library = json.loads((user_root / "library.json").read_text(encoding="utf-8"))
            self.assertEqual([game["disc_id"] for game in library["games"]], ["ULUS99996"])
            game = library["games"][0]
            self.assertTrue(game["assets_staged"])
            prepared = Path(game["prepared_root"])
            self.assertEqual(prepared.resolve(), (user_root / "games" / "ULUS99996").resolve())
            member = prepared / "xbdata" / "menu" / "assets.xb.d" / "data" / "raw.bin"
            self.assertEqual(member.read_bytes(), b"synthetic archive member")

    def test_launch_now_on_experimental_iso_reports_the_unavailable_boundary(self) -> None:
        sys.path.insert(0, str(ROOT / "tools"))
        from test_iso_parity import build_plain_mips_elf, create_test_iso_with_executables

        with tempfile.TemporaryDirectory(prefix=".first-run-experimental-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            iso = scratch / "experimental.iso"
            executable = build_plain_mips_elf() + bytes(i % 251 for i in range(70000))
            create_test_iso_with_executables(
                iso, executable, disc_id="ULUS99998", title="Experimental Fixture"
            )
            completed = self.run_route(scratch, [
                f"--iso={iso}",
                "--launch-now",
                "--ui-test-events=WAIT_MS=100;QUIT",
                f"--ui-test-screenshot={scratch / 'last-frame.bmp'}",
            ])

        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        frames = parse_frames(completed.stdout)
        self.assertTrue(frames, completed.stdout)
        self.assertEqual(frames[0]["view"], "error")
        self.assertEqual(frames[0]["error"], "EXPERIMENTAL_LAUNCH_UNAVAILABLE")
        self.assertIn("isn't supported yet.", completed.stderr)
        self.assertNotIn("(#308)", completed.stderr)
        self.assertEqual(library_disc_ids(scratch), [])

    def test_stage_route_runs_the_wizard_worker_for_an_archive_disc(self) -> None:
        """--stage drives the setup wizard's own staging worker.

        The disc's data ships in an XB archive below USRDIR/xbdata. The worker
        runs the shared staging transaction, the game lands in the library with
        its files promoted into the per-user data root, and the wizard ends on
        the ready library card.
        """
        sys.path.insert(0, str(ROOT / "tools"))
        from test_iso_parity import (
            archive_title_manifest,
            build_plain_mips_elf,
            create_archive_title_iso,
        )

        with tempfile.TemporaryDirectory(prefix=".first-run-archive-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            iso = scratch / "archive.iso"
            create_archive_title_iso(iso, disc_id="ULUS99996", title="Synthetic Archive",
                                     executable=build_plain_mips_elf())
            user_root = scratch / "user-data"
            (user_root / "manifests").mkdir(parents=True)
            (user_root / "manifests" / "archive.json").write_text(
                json.dumps(archive_title_manifest("ULUS99996", "archive-ulus99996")),
                encoding="utf-8",
            )
            completed = subprocess.run(
                [str(PLAYER_EXE), f"--user-data-root={user_root}", f"--iso={iso}", "--stage",
                 "--ui-test-events=WAIT_VIEW=ready_library,30000;QUIT",
                 f"--ui-test-screenshot={scratch / 'last-frame.bmp'}"],
                cwd=ROOT, env=self.scratch_env(scratch), capture_output=True, text=True,
                timeout=120, check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            frames = parse_frames(completed.stdout)
            self.assertTrue(frames, completed.stdout)
            self.assertEqual(frames[-1]["view"], "ready_library", completed.stdout)
            self.assertEqual(frames[-1]["selected_staged"], "1")
            library = json.loads((user_root / "library.json").read_text(encoding="utf-8"))
            self.assertEqual([game["disc_id"] for game in library["games"]], ["ULUS99996"])
            game = library["games"][0]
            self.assertTrue(game["assets_staged"])
            prepared = Path(game["prepared_root"])
            self.assertEqual(prepared.resolve(), (user_root / "games" / "ULUS99996").resolve())
            member = prepared / "xbdata" / "menu" / "assets.xb.d" / "data" / "raw.bin"
            self.assertEqual(member.read_bytes(), b"synthetic archive member")
            self.assertFalse((user_root / "games" / ".staging_ULUS99996").exists())
            self.assertEqual(library_disc_ids(scratch / "localappdata"), [],
                             "--user-data-root keeps the library out of the default data folder")

    def test_headless_launch_now_sets_up_archive_files_before_play(self) -> None:
        """--launch-now stands in for PLAY NOW on a disc whose files are not set up.

        The same staging transaction runs first, the game is recorded with its
        files in place, and the launch then reaches PLAY's own next boundary
        (no package has been built here) instead of a missing data folder.
        """
        sys.path.insert(0, str(ROOT / "tools"))
        from test_iso_parity import (
            archive_title_manifest,
            build_plain_mips_elf,
            create_archive_title_iso,
        )

        with tempfile.TemporaryDirectory(prefix=".first-run-launch-now-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            iso = scratch / "archive.iso"
            create_archive_title_iso(iso, disc_id="ULUS99996", title="Synthetic Archive",
                                     executable=build_plain_mips_elf())
            user_root = scratch / "user-data"
            (user_root / "manifests").mkdir(parents=True)
            (user_root / "manifests" / "archive.json").write_text(
                json.dumps(archive_title_manifest("ULUS99996", "archive-ulus99996")),
                encoding="utf-8",
            )
            completed = subprocess.run(
                [str(PLAYER_EXE), f"--user-data-root={user_root}", f"--iso={iso}",
                 "--launch-now", "--headless-launch"],
                cwd=ROOT, env=self.scratch_env(scratch), capture_output=True, text=True,
                timeout=120, check=False,
            )
            output = completed.stdout + completed.stderr
            self.assertIn("STAGING_RESULT status=PASS", completed.stdout, output)
            self.assertEqual(completed.returncode, 4, output)
            self.assertIn("--launch-now failed: Runtime package is missing", completed.stderr)
            self.assertNotIn("data folder", completed.stderr)
            library = json.loads((user_root / "library.json").read_text(encoding="utf-8"))
            self.assertEqual([game["disc_id"] for game in library["games"]], ["ULUS99996"])
            self.assertTrue(library["games"][0]["assets_staged"])
            self.assertTrue((user_root / "games" / "ULUS99996" / "xbdata" / "menu" /
                             "assets.xb.d" / "data" / "raw.bin").is_file())

    def test_launch_index_on_an_empty_library_fails_with_a_clear_message(self) -> None:
        with tempfile.TemporaryDirectory(prefix=".first-run-empty-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            completed = self.run_route(scratch, ["--launch-index=0", "--headless-launch"])
        self.assertEqual(completed.returncode, 2, completed.stdout + completed.stderr)
        self.assertIn("library is empty", completed.stderr)
        self.assertNotIn("has no prepared runtime", completed.stderr)

    def test_launch_title_names_a_library_game_by_disc_id(self) -> None:
        """#2152: --launch-title (what an Add to Steam shortcut runs) finds the game by its
        disc ID, or disc ID and image SHA-1 when two images share it, and takes the
        --launch-index route; a missing, shared or malformed value is refused by name."""
        with tempfile.TemporaryDirectory(prefix=".first-run-launch-title-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            user_root = scratch / "user data"
            user_root.mkdir()
            # A record keeps both image hashes or neither (nk_library.c).
            games = [
                {"disc_id": "ULUS10001", "title_name": "Synthetic One",
                 "iso_path": str(scratch / "one.iso"), "title_id": "synthetic-one"},
                {"disc_id": "ULUS10002", "title_name": "Synthetic Retail",
                 "iso_path": str(scratch / "retail.iso"), "title_id": "synthetic-two",
                 "content_sha1": "b" * 40, "content_crc32": "b" * 8},
                {"disc_id": "ULUS10002", "title_name": "Synthetic Translation",
                 "iso_path": str(scratch / "translation.iso"), "title_id": "synthetic-two",
                 "content_sha1": "c" * 40, "content_crc32": "c" * 8},
            ]
            (user_root / "library.json").write_text(
                json.dumps({"schema_version": 1, "games": games}), encoding="utf-8")
            base = [f"--user-data-root={user_root}", "--headless-launch"]

            found = self.run_route(scratch, [*base, "--launch-title=ULUS10001"])
            # Found, then the launch route's own check: no runtime in the scratch root.
            self.assertEqual(found.returncode, 3, found.stdout + found.stderr)
            self.assertIn("PLAY NOW is unavailable", found.stderr)
            self.assertIn("is not built for this PC yet", found.stderr)

            by_image = self.run_route(scratch, [*base, "--launch-title=ULUS10002:" + "C" * 40])
            self.assertEqual(by_image.returncode, 3, by_image.stdout + by_image.stderr)
            self.assertIn("Synthetic Translation is not built", by_image.stderr)

            shared = self.run_route(scratch, [*base, "--launch-title=ULUS10002"])
            self.assertEqual(shared.returncode, 2, shared.stdout + shared.stderr)
            self.assertIn("Several disc images in this library have disc ID ULUS10002",
                          shared.stderr)

            missing = self.run_route(scratch, [*base, "--launch-title=ULUS99999"])
            self.assertEqual(missing.returncode, 2, missing.stdout + missing.stderr)
            self.assertIn("No game with disc ID ULUS99999", missing.stderr)

            malformed = self.run_route(scratch, [*base, "--launch-title=../ULUS10001"])
            self.assertEqual(malformed.returncode, 2, malformed.stdout + malformed.stderr)
            self.assertIn("needs a game's disc ID", malformed.stderr)

            both = self.run_route(scratch, [*base, "--launch-title=ULUS10001",
                                            "--launch-index=0"])
            self.assertEqual(both.returncode, 2, both.stdout + both.stderr)
            self.assertIn("not both", both.stderr)

    def test_demo_launch_index_names_its_bundled_entry_as_a_sample(self) -> None:
        with tempfile.TemporaryDirectory(prefix=".first-run-demo-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            completed = self.run_route(scratch, ["--demo", "--launch-index=0", "--headless-launch"])
        # No runtime is built in the scratch root, so the bounded launch refuses.
        self.assertEqual(completed.returncode, 3, completed.stdout + completed.stderr)
        self.assertIn("SAMPLE", completed.stderr)


class ScreenshotDeterminismTests(unittest.TestCase):
    """#2124: two --screenshot captures of one view from one build are the same bytes.

    Failing-before: the launcher drew its one captured frame at the time since SDL
    started, so the background waves (which step with that clock at 30 Hz) and any
    progress sweep came out different from run to run. --screenshot now draws at a
    held time.

    The second capture of each view waits NK_UI_TEST_SCREENSHOT_DELAY_MS (a test-only
    hook) before it draws, so the two runs are always at least seven wave steps apart
    on the real clock. Without the held time, the test therefore fails every time,
    not only when two runs happen to land in different steps.

    Each run has its own data folder, as in the issue's repro. The Settings page
    prints that folder's path ("Saves: ..."), so there the two captures may differ
    only within one line of text."""

    VIEWS = ("library", "settings", "developer-log", "accessibility-settings", "psp-fonts",
             "updates", "wizard", "building", "empty")
    DELAY_MS = "250"  # 7.5 steps of the waves' 30 Hz clock
    TEXT_LINE_ROWS = 16

    def capture(self, scratch: Path, view: str, run: int, delay_ms: str | None) -> bytes:
        # Same-length folder names, so only the run number differs in the printed path.
        data = scratch / f"data-{run}"
        shutil.rmtree(data, ignore_errors=True)
        profile = data / "profile"
        for folder in (data / "appdata", data / "localappdata", profile):
            folder.mkdir(parents=True)
        env = os.environ.copy()
        env.update({
            "SDL_VIDEODRIVER": "dummy", "SDL_RENDER_DRIVER": "software", "SDL_AUDIODRIVER": "dummy",
            "APPDATA": str(data / "appdata"), "LOCALAPPDATA": str(data / "localappdata"),
            "USERPROFILE": str(profile), "HOME": str(profile),
            "XDG_CONFIG_HOME": str(profile / "config"), "XDG_DATA_HOME": str(profile / "data"),
            "XDG_CACHE_HOME": str(profile / "cache"),
        })
        env.pop("NK_UI_TEST_SCREENSHOT_DELAY_MS", None)
        if delay_ms is not None:
            env["NK_UI_TEST_SCREENSHOT_DELAY_MS"] = delay_ms
        shot = scratch / f"{view}-{run}.bmp"
        completed = subprocess.run(
            [str(PLAYER_EXE), f"--view={view}", f"--screenshot={shot}", "--width=1280",
             "--height=720", f"--user-data-root={data / 'user'}", f"--runtime-root={data / 'runtime'}"],
            cwd=ROOT, env=env, capture_output=True, text=True, timeout=120, check=False,
        )
        if "[PLAYER] Screenshot saved" not in completed.stdout:
            if "UI_NEXT_ACTIVE" not in completed.stdout:
                self.skipTest("the next UI could not start here (no UI fonts)")
            self.fail(f"--view={view} saved no screenshot:\n{completed.stdout}\n{completed.stderr}")
        return shot.read_bytes()

    @staticmethod
    def differing_rows(scratch: Path, view: str) -> list[int]:
        width, height, bpp, a = read_bmp(scratch / f"{view}-1.bmp")
        _, _, _, b = read_bmp(scratch / f"{view}-2.bmp")
        stride = (width * (bpp // 8) + 3) & ~3
        return [row for row in range(height)
                if a[row * stride:(row + 1) * stride] != b[row * stride:(row + 1) * stride]]

    def test_two_captures_of_each_view_are_the_same_bytes(self) -> None:
        with tempfile.TemporaryDirectory(prefix=".screenshot-twice-", dir=ROOT) as tmp:
            scratch = Path(tmp)
            for view in self.VIEWS:
                with self.subTest(view=view):
                    first = self.capture(scratch, view, 1, None)
                    second = self.capture(scratch, view, 2, self.DELAY_MS)
                    if first == second:
                        continue
                    rows = self.differing_rows(scratch, view)
                    if view == "settings" and rows and rows[-1] - rows[0] < self.TEXT_LINE_ROWS:
                        continue  # only the printed data folder's path
                    self.fail(f"--view={view}: the two captures differ in {len(rows)} rows "
                              f"({rows[0]} to {rows[-1]})" if rows else
                              f"--view={view}: the two captures differ")


if __name__ == "__main__":
    unittest.main()


class PlayerVersionIdentityTests(unittest.TestCase):
    """The About page's build date comes from SOURCE_DATE_EPOCH when it is set.

    A dry run of the player link shows the define the build passes; nothing is
    compiled. The date itself (MM-DD-YYYY from the epoch) is checked by
    tests/native/test_player_screens.c.

    `player-ui-regressions` runs this module under `make -j4 --output-sync=target`,
    and that make's MAKEFLAGS (its jobserver and its output-sync mutex handle) reach
    the dry run through the environment. The handle is a raw Windows handle number
    the child never inherits, so a dry run that used it could wait on whatever
    handle has that number in its own process: hosted runs hung for the whole
    timeout holding jobserver tokens. `-j1 --output-sync=none` makes the dry run
    a standalone make while keeping the variables the shard passes (CC, VULKAN_SDK).
    """

    # A dry run takes about 13 s on a Windows desktop, and the shard that runs both
    # of them takes 41-67 s on hosted Windows runners; 180 s is about three times
    # that whole shard, so a stuck dry run fails in minutes, not after ten.
    DRY_RUN_TIMEOUT_S = 180

    def _player_defines(self, env_extra: dict[str, str]) -> list[str]:
        make = shutil.which("mingw32-make") or shutil.which("make")
        if make is None:
            self.skipTest("make is not on PATH")
        env = dict(os.environ)
        env.update(env_extra)
        with tempfile.TemporaryDirectory(prefix="nk-player-identity-") as scratch:
            build_root = Path(scratch).as_posix()
            exe = f"{build_root}/nakagawa_player{'.exe' if os.name == 'nt' else ''}"
            run = subprocess.run(
                [make, "--no-print-directory", "-j1", "--output-sync=none", "-n", "-B",
                 f"BUILD_ROOT={build_root}", exe],
                cwd=ROOT, env=env, capture_output=True, text=True,
                timeout=self.DRY_RUN_TIMEOUT_S,
            )
        if run.returncode != 0:
            self.skipTest(f"the player dry run needs its build dependencies: {run.stderr[-400:]}")
        return re.findall(r"-DNK_PLAYER_BUILD_[A-Z]+=\S+", run.stdout)

    def test_source_date_epoch_fixes_the_build_date(self) -> None:
        defines = self._player_defines({"SOURCE_DATE_EPOCH": "1792713600"})
        self.assertIn("-DNK_PLAYER_BUILD_EPOCH=1792713600LL", defines)
        again = self._player_defines({"SOURCE_DATE_EPOCH": "1792713600"})
        self.assertEqual(defines, again, "the same epoch gives the same defines")
