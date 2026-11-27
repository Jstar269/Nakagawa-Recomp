# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Differential parity tests comparing native C core (nk_iso, nk_library, nk_launch) against Python."""

from __future__ import annotations

import contextlib
import json
import hashlib
import io
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor
from typing import ClassVar, TypedDict, cast
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import hle_manifest
from import_fixtures import BASE_VADDR, SYSLIB_EXPORT, build_module_elf
from nk_core import iso_inspect, package_cache
from nk_core.iso_inspect import (
    _classify_decrypted_elf_file,
    check_needed_modules,
    decrypted_module_dir,
    IsoInspectionError,
    inspect_compatibility_preflight,
    inspect_iso,
    list_disc_module_candidates,
    plan_guest_module_bindings,
    plan_guest_module_layout,
    read_guest_module_interface,
    runtime_registered_nids,
    runtime_serves_module,
    RuntimeRegistryUnavailableError,
    write_experimental_profile,
)
import nk_cli

# The package key asks the C compiler for its target (gcc -dumpmachine), its version
# (-dumpfullversion) and the programs it runs (-print-prog-name=cc1/as/ld) before the
# planner runs. The fakes below stand in for the planner only, so those host queries are
# passed to the real subprocess.run, captured here before any test patches it.
_REAL_SUBPROCESS_RUN = subprocess.run


def _is_compiler_target_query(command) -> bool:
    # Match the flag anywhere in the argv, not only as the last element, so the helper
    # keeps working if the query ever gains trailing arguments.
    return bool(command) and any(
        isinstance(arg, str) and (
            arg in ("-dumpmachine", "-dumpfullversion") or arg.startswith("-print-prog-name=")
        )
        for arg in command
    )


def build_param_sfo(disc_id: str, title: str, version: str = "1.00") -> bytes:
    """Construct a binary PSP PARAM.SFO buffer with given keys."""
    entries = [
        ("DISC_ID", 0x0204, disc_id.encode("utf-8") + b"\0"),
        ("DISC_VERSION", 0x0204, version.encode("utf-8") + b"\0"),
        ("TITLE", 0x0204, title.encode("utf-8") + b"\0"),
    ]
    entries.sort(key=lambda e: e[0])

    key_table = bytearray()
    data_table = bytearray()
    entry_table = bytearray()

    for key, fmt, val in entries:
        k_off = len(key_table)
        key_table.extend(key.encode("utf-8") + b"\0")

        d_off = len(data_table)
        d_len = len(val)
        data_table.extend(val)
        while len(data_table) % 4 != 0:
            data_table.append(0)

        entry_table.extend(struct.pack("<HHIII", k_off, fmt, d_len, d_len, d_off))

    header_size = 20
    key_table_start = header_size + len(entry_table)
    data_table_start = key_table_start + len(key_table)
    while data_table_start % 4 != 0:
        key_table.append(0)
        data_table_start += 1

    header = struct.pack("<4s4sIII", b"\x00PSF", b"\x01\x01\x00\x00", key_table_start, data_table_start, len(entries))
    return bytes(header + entry_table + key_table + data_table)


def build_custom_param_sfo(entries: list[tuple[str, int, bytes]]) -> bytes:
    """Construct a binary PSP PARAM.SFO buffer with arbitrary raw entries (e.g. duplicates)."""
    key_table = bytearray()
    data_table = bytearray()
    entry_table = bytearray()

    for key, fmt, val in entries:
        k_off = len(key_table)
        key_table.extend(key.encode("utf-8") + b"\0")

        d_off = len(data_table)
        d_len = len(val)
        data_table.extend(val)
        while len(data_table) % 4 != 0:
            data_table.append(0)

        entry_table.extend(struct.pack("<HHIII", k_off, fmt, d_len, d_len, d_off))

    header_size = 20
    key_table_start = header_size + len(entry_table)
    data_table_start = key_table_start + len(key_table)
    while data_table_start % 4 != 0:
        key_table.append(0)
        data_table_start += 1

    header = struct.pack("<4s4sIII", b"\x00PSF", b"\x01\x01\x00\x00", key_table_start, data_table_start, len(entries))
    return bytes(header + entry_table + key_table + data_table)


def create_test_iso(
    path: Path,
    disc_id: str = "TEST00001",
    title: str = "Test Game",
    version: str = "1.00",
    volume_id: str = "TEST_VOL",
    sfo_bytes: bytes | None = None,
    update_sfo_bytes: bytes | None = None,
) -> None:
    sector_size = 2048
    sfo_lba = 32
    update_sfo_lba = 30
    root_lba, game_lba, sysdir_lba, update_lba = 33, 34, 35, 36
    num_sectors = max(1024, sfo_lba + 2, update_lba + 2)
    data = bytearray(num_sectors * sector_size)

    pvd_off = 16 * sector_size
    data[pvd_off] = 0x01
    data[pvd_off + 1 : pvd_off + 6] = b"CD001"
    data[pvd_off + 6] = 0x01
    data[pvd_off + 40 : pvd_off + 40 + len(volume_id)] = volume_id.encode("latin-1")

    if sfo_bytes is None:
        sfo_bytes = build_param_sfo(disc_id, title, version)
    sfo_off = sfo_lba * sector_size
    data[sfo_off : sfo_off + len(sfo_bytes)] = sfo_bytes
    if update_sfo_bytes is not None:
        update_off = update_sfo_lba * sector_size
        data[update_off : update_off + len(update_sfo_bytes)] = update_sfo_bytes

    root_entries = (
        _dir_record(bytes([0]), root_lba, sector_size, True)
        + _dir_record(bytes([1]), root_lba, sector_size, True)
        + _dir_record(b"PSP_GAME", game_lba, sector_size, True)
    )
    data[root_lba * sector_size : root_lba * sector_size + len(root_entries)] = root_entries

    game_entries = (
        _dir_record(bytes([0]), game_lba, sector_size, True)
        + _dir_record(bytes([1]), root_lba, sector_size, True)
        + _dir_record(b"PARAM.SFO;1", sfo_lba, len(sfo_bytes), False)
    )
    if update_sfo_bytes is not None:
        game_entries += _dir_record(b"SYSDIR", sysdir_lba, sector_size, True)
    data[game_lba * sector_size : game_lba * sector_size + len(game_entries)] = game_entries

    if update_sfo_bytes is not None:
        sysdir_entries = (
            _dir_record(bytes([0]), sysdir_lba, sector_size, True)
            + _dir_record(bytes([1]), game_lba, sector_size, True)
            + _dir_record(b"UPDATE", update_lba, sector_size, True)
        )
        update_entries = (
            _dir_record(bytes([0]), update_lba, sector_size, True)
            + _dir_record(bytes([1]), sysdir_lba, sector_size, True)
            + _dir_record(b"PARAM.SFO;1", update_sfo_lba, len(update_sfo_bytes), False)
        )
        data[sysdir_lba * sector_size : sysdir_lba * sector_size + len(sysdir_entries)] = sysdir_entries
        data[update_lba * sector_size : update_lba * sector_size + len(update_entries)] = update_entries

    pvd_root = _dir_record(bytes([0]), root_lba, sector_size, True)[:34]
    data[pvd_off + 156 : pvd_off + 156 + 34] = pvd_root
    path.write_bytes(data)


def create_test_iso_with_executables(
    path: Path, eboot: bytes, boot: bytes | None = None,
    disc_id: str = "TEST00001", title: str = "Test Game",
    *, version: str = "1.00",
) -> None:
    """Add synthetic SYSDIR executables to the source-owned ISO fixture."""
    create_test_iso(path, disc_id=disc_id, title=title, version=version)
    sector_size = 2048
    data = bytearray(path.read_bytes())
    game_lba, sysdir_lba, eboot_lba, boot_lba = 34, 35, 36, 37
    game_entries = (
        _dir_record(bytes([0]), game_lba, sector_size, True)
        + _dir_record(bytes([1]), 33, sector_size, True)
        + _dir_record(b"PARAM.SFO;1", 32, len(build_param_sfo(disc_id, title, version)), False)
        + _dir_record(b"SYSDIR", sysdir_lba, sector_size, True)
    )
    sysdir_entries = (
        _dir_record(bytes([0]), sysdir_lba, sector_size, True)
        + _dir_record(bytes([1]), game_lba, sector_size, True)
        + _dir_record(b"EBOOT.BIN;1", eboot_lba, len(eboot), False)
    )
    if boot is not None:
        sysdir_entries += _dir_record(b"BOOT.BIN;1", boot_lba, len(boot), False)
    data[game_lba * sector_size : (game_lba + 1) * sector_size] = bytes(sector_size)
    data[sysdir_lba * sector_size : (sysdir_lba + 1) * sector_size] = bytes(sector_size)
    data[game_lba * sector_size : game_lba * sector_size + len(game_entries)] = game_entries
    data[sysdir_lba * sector_size : sysdir_lba * sector_size + len(sysdir_entries)] = sysdir_entries
    data[eboot_lba * sector_size : eboot_lba * sector_size + len(eboot)] = eboot
    if boot is not None:
        data[boot_lba * sector_size : boot_lba * sector_size + len(boot)] = boot
    path.write_bytes(data)


class _ModuleFiles(TypedDict):
    entries: bytes
    members: dict[str, bytes]


def create_test_iso_with_modules(
    path: Path,
    eboot: bytes,
    *,
    sysdir_modules: dict[str, bytes],
    usrdir_modules: dict[str, bytes],
    old_eboot: bytes | None = None,
    disc_id: str = "TEST00001",
    title: str = "Test Game",
) -> None:
    """Build a source-owned ISO with module candidates in both PSP game dirs."""
    create_test_iso(path, disc_id=disc_id, title=title)
    sector_size = 2048
    data = bytearray(path.read_bytes())
    root_lba, game_lba, sysdir_lba, usrdir_lba = 33, 34, 35, 36
    game_entries = (
        _dir_record(bytes([0]), game_lba, sector_size, True)
        + _dir_record(bytes([1]), root_lba, sector_size, True)
        + _dir_record(b"PARAM.SFO;1", 32, len(build_param_sfo(disc_id, title)), False)
        + _dir_record(b"SYSDIR", sysdir_lba, sector_size, True)
        + _dir_record(b"USRDIR", usrdir_lba, sector_size, True)
    )
    sysdir_entries = (
        _dir_record(bytes([0]), sysdir_lba, sector_size, True)
        + _dir_record(bytes([1]), game_lba, sector_size, True)
    )
    usrdir_entries = (
        _dir_record(bytes([0]), usrdir_lba, sector_size, True)
        + _dir_record(bytes([1]), game_lba, sector_size, True)
    )
    next_lba = 37
    file_records = []
    files: list[_ModuleFiles] = [
        {"entries": sysdir_entries, "members": sysdir_modules},
        {"entries": usrdir_entries, "members": usrdir_modules},
    ]
    for item in files:
        directory_entries = item["entries"]
        members = item["members"]
        for name, contents in sorted(members.items()):
            encoded_name = name.encode("ascii") + b";1"
            directory_entries += _dir_record(encoded_name, next_lba, len(contents), False)
            file_records.append((next_lba, contents))
            next_lba += max(1, (len(contents) + sector_size - 1) // sector_size)
        item["entries"] = directory_entries
    sysdir_entries, usrdir_entries = files[0]["entries"], files[1]["entries"]
    sysdir_entries += _dir_record(b"EBOOT.BIN;1", next_lba, len(eboot), False)
    file_records.append((next_lba, eboot))
    next_lba += max(1, (len(eboot) + sector_size - 1) // sector_size)
    if old_eboot is not None:
        sysdir_entries += _dir_record(b"EBOOT.OLD;1", next_lba, len(old_eboot), False)
        file_records.append((next_lba, old_eboot))
    for entries, lba in ((game_entries, game_lba), (sysdir_entries, sysdir_lba),
                         (usrdir_entries, usrdir_lba)):
        start = lba * sector_size
        data[start : start + len(entries)] = entries
    for lba, contents in file_records:
        start = lba * sector_size
        data[start : start + len(contents)] = contents
    path.write_bytes(data)


def create_test_iso_with_module_tree(
    path: Path, modules: dict[str, bytes], *,
    disc_id: str = "TEST00001", title: str = "Synthetic Module Tree",
) -> dict[tuple[str, ...], int]:
    """Build a source-owned ISO carrying files at arbitrary SYSDIR/USRDIR paths."""
    sector_size = 2048
    sfo = build_param_sfo(disc_id, title)
    directories: set[tuple[str, ...]] = {
        (), ("PSP_GAME",), ("PSP_GAME", "SYSDIR"), ("PSP_GAME", "USRDIR"),
    }
    files: dict[tuple[str, ...], bytes] = {
        ("PSP_GAME", "PARAM.SFO"): sfo,
    }
    for member, contents in modules.items():
        parts = tuple(member.split("/"))
        if len(parts) == 2 and parts[0] == "PSP_GAME" and \
                parts[1].casefold() in {"sysdir", "usrdir"}:
            directories.discard(parts)
            files[parts] = contents
            continue
        if len(parts) < 3 or parts[:2] not in {
            ("PSP_GAME", "SYSDIR"), ("PSP_GAME", "USRDIR"),
        }:
            raise ValueError(f"module fixture path is outside SYSDIR/USRDIR: {member}")
        for end in range(1, len(parts)):
            directories.add(parts[:end])
        if parts in files or parts in directories:
            raise ValueError(f"duplicate module fixture path: {member}")
        files[parts] = contents

    def pack_records(records: list[bytes]) -> bytes:
        output = bytearray()
        for record in records:
            sector_offset = len(output) % sector_size
            if sector_offset + len(record) > sector_size:
                output.extend(bytes(sector_size - sector_offset))
            output.extend(record)
        output.extend(bytes((-len(output)) % sector_size))
        if not output:
            output.extend(bytes(sector_size))
        return bytes(output)

    directory_children: dict[tuple[str, ...], list[tuple[str, tuple[str, ...], bool]]] = {
        directory: [] for directory in directories
    }
    for directory in directories:
        if directory:
            directory_children[directory[:-1]].append((directory[-1], directory, True))
    for file_path in files:
        directory_children[file_path[:-1]].append((file_path[-1], file_path, False))

    directory_sizes: dict[tuple[str, ...], int] = {}
    for directory, children in directory_children.items():
        placeholder = [
            _dir_record(bytes([0]), 1, sector_size, True),
            _dir_record(bytes([1]), 1, sector_size, True),
        ]
        for name, _child_path, is_directory in sorted(
            children, key=lambda child: child[0].casefold()
        ):
            encoded = name.encode("latin-1", "surrogateescape") if is_directory else \
                name.encode("latin-1", "surrogateescape") + b";1"
            placeholder.append(_dir_record(encoded, 1, sector_size, is_directory))
        directory_sizes[directory] = len(pack_records(placeholder))

    directory_lbas = {(): 33, ("PSP_GAME",): 34}
    next_lba = 35
    for directory in sorted(
        (item for item in directories if item not in directory_lbas),
        key=lambda item: (len(item), item),
    ):
        directory_lbas[directory] = next_lba
        next_lba += directory_sizes[directory] // sector_size

    file_lbas: dict[tuple[str, ...], int] = {("PSP_GAME", "PARAM.SFO"): 32}
    file_sizes: dict[tuple[str, ...], int] = {("PSP_GAME", "PARAM.SFO"): len(sfo)}
    for file_path, contents in sorted(files.items()):
        if file_path == ("PSP_GAME", "PARAM.SFO"):
            continue
        file_lbas[file_path] = next_lba
        file_sizes[file_path] = len(contents)
        next_lba += max(1, (len(contents) + sector_size - 1) // sector_size)

    directory_payloads: dict[tuple[str, ...], bytes] = {}
    for directory, children in directory_children.items():
        parent = directory[:-1] if directory else ()
        records = [
            _dir_record(bytes([0]), directory_lbas[directory], directory_sizes[directory], True),
            _dir_record(bytes([1]), directory_lbas[parent], directory_sizes[parent], True),
        ]
        for name, child_path, is_directory in sorted(
            children, key=lambda child: child[0].casefold()
        ):
            encoded = name.encode("latin-1", "surrogateescape") if is_directory else \
                name.encode("latin-1", "surrogateescape") + b";1"
            records.append(_dir_record(
                encoded,
                directory_lbas[child_path] if is_directory else file_lbas[child_path],
                directory_sizes[child_path] if is_directory else file_sizes[child_path],
                is_directory,
            ))
        directory_payloads[directory] = pack_records(records)

    data = bytearray(max(1024, next_lba) * sector_size)
    pvd_offset = 16 * sector_size
    data[pvd_offset] = 0x01
    data[pvd_offset + 1:pvd_offset + 6] = b"CD001"
    data[pvd_offset + 6] = 0x01
    volume_id = b"MODULE_TREE"
    data[pvd_offset + 40:pvd_offset + 40 + len(volume_id)] = volume_id
    data[pvd_offset + 156:pvd_offset + 190] = _dir_record(
        bytes([0]), 33, directory_sizes[()], True
    )[:34]
    for directory, payload in directory_payloads.items():
        start = directory_lbas[directory] * sector_size
        data[start:start + len(payload)] = payload
    for file_path, contents in files.items():
        start = file_lbas[file_path] * sector_size
        data[start:start + len(contents)] = contents
    path.write_bytes(data)
    return directory_lbas



def archive_title_manifest(disc_id: str, title_id: str) -> dict:
    """A user title manifest for a synthetic disc whose data ships in XB archives.

    The whole USRDIR is the title's loose-content root and ``xbdata`` its data
    root: the archive layout the native staging transaction extracts.
    """
    return {
        "schema_version": 1,
        "id": title_id,
        "display_name": "Synthetic archive title",
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
            "data_root": "xbdata",
            "memory_stick_root": "memstick",
            "device_prefixes": ["host0:", "ms0:"],
            "loose_content_roots": [
                {"root": ".", "mount": "", "precedence": 0, "skip_primary_root": True},
            ],
        },
        "hle_profile": "generic",
        "codegen_profile": "none",
        "feature_requirements": [],
        "verification_profile": "experimental-unverified",
    }


def create_archive_title_iso(
    path: Path, *, disc_id: str, title: str, executable: bytes
) -> None:
    """Build a source-owned disc whose game data is an XB archive below USRDIR/xbdata."""
    from test_xb_probe import _make_archive
    from xb_probe import XBCompression

    archive = _make_archive([
        ("data/raw.bin", b"synthetic archive member", XBCompression.NONE),
        ("data/sound/theme.sgd", b"synthetic sound member", XBCompression.LZS),
    ])
    create_test_iso_with_module_tree(path, {
        "PSP_GAME/SYSDIR/EBOOT.BIN": executable,
        "PSP_GAME/USRDIR/xbdata/menu/assets.xb": archive,
        "PSP_GAME/USRDIR/readme.txt": b"synthetic loose file\n",
    }, disc_id=disc_id, title=title)

def build_plain_mips_elf(
    e_type: int = 2, *, vaddr: int = 0x08800000, memsz: int = 4,
    entry: int | None = None, p_type: int = 1, p_flags: int = 5, filesz: int = 4,
) -> bytes:
    """A one-segment ELF32/MIPS image; ``entry`` defaults to ``vaddr``."""
    elf = bytearray(88)
    elf[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHI", elf, 16, e_type, 8, 1)
    struct.pack_into("<III", elf, 24, vaddr if entry is None else entry, 52, 0)
    struct.pack_into("<HHHHH", elf, 40, 52, 32, 1, 0, 0)
    struct.pack_into("<8I", elf, 52, p_type, 84, vaddr, vaddr, filesz, memsz, p_flags, 4)
    elf[84:88] = b"\x34\x12\x00\x00"
    return bytes(elf)


MODULE_START_NID = 0xD632ACDB
MODULE_BOOTSTART_NID = 0xD3744BE0


def build_module_start_elf(
    *, entry: int = 0, exports: tuple[tuple[int, int], ...] | None = None,
    p_flags: int = 5, info_offset: int | None = None, ent_end_extra: int = 0,
) -> bytes:
    """A fixed ET_EXEC whose first PT_LOAD's p_paddr names its SceModuleInfo.

    The segment at 0x08804000 holds four code words, the module info record, one
    syslib export record and its table. ``exports`` lists syslib (NID, segment offset)
    function pairs; by default module_start at the second code word.
    """
    vaddr, seg_off = 0x08804000, 0x60
    exports = ((MODULE_START_NID, 0x4),) if exports is None else exports
    segment = bytearray(0x90)
    struct.pack_into("<4I", segment, 0, 0x27BDFFF0, 0x27BDFFF0, 0x03E00008, 0x27BD0010)
    info, record, table = 0x10, 0x48, 0x58
    struct.pack_into("<HBB28sIIIII", segment, info, 0, 1, 1, b"start-entry-fixture",
                     0, vaddr + record, vaddr + record + 16 + ent_end_extra, 0, 0)
    struct.pack_into("<IHHBBHI", segment, record, 0, 0, 0x8000, 4, 0, len(exports), vaddr + table)
    for index, (nid, address) in enumerate(exports):
        struct.pack_into("<I", segment, table + 4 * index, nid)
        struct.pack_into("<I", segment, table + 4 * (len(exports) + index), vaddr + address)
    elf = bytearray(seg_off) + segment
    elf[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHI", elf, 16, 2, 8, 1)
    struct.pack_into("<III", elf, 24, entry, 52, 0)
    struct.pack_into("<HHHHH", elf, 40, 52, 32, 1, 0, 0)
    paddr = seg_off + info if info_offset is None else info_offset
    struct.pack_into("<8I", elf, 52, 1, seg_off, vaddr, paddr, len(segment), len(segment),
                     p_flags, 16)
    return bytes(elf)


def build_psp_container() -> bytes:
    container = bytearray(0x80)
    container[:4] = b"~PSP"
    container[0x27] = 1
    struct.pack_into("<I", container, 0x54, 0x1000)
    return bytes(container)


def build_overlapping_mips_elf(
    *, vaddr1: int = 0, memsz1: int = 0x2000, vaddr2: int = 0x1000, memsz2: int = 0x2000
) -> bytes:
    elf = bytearray(124)
    elf[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHI", elf, 16, 0xFFA0, 8, 1)
    struct.pack_into("<III", elf, 24, vaddr1, 52, 0)
    struct.pack_into("<HHHHH", elf, 40, 52, 32, 2, 0, 0)
    struct.pack_into("<8I", elf, 52, 1, 116, vaddr1, vaddr1, 4, memsz1, 5, 4)
    struct.pack_into("<8I", elf, 84, 1, 120, vaddr2, vaddr2, 4, memsz2, 5, 4)
    elf[116:124] = b"\x00\x00\x00\x00\x00\x00\x00\x00"
    return bytes(elf)


def build_mips_elf_with_load_segments(loads: list[tuple[int, int]]) -> bytes:
    """An executable with one PT_LOAD per (vaddr, memsz), in the order given, all backed by
    the same four file bytes; the entry is the first load's vaddr."""
    phnum = len(loads)
    data = 52 + 32 * phnum
    elf = bytearray(data + 4)
    elf[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHI", elf, 16, 2, 8, 1)
    struct.pack_into("<III", elf, 24, loads[0][0], 52, 0)
    struct.pack_into("<HHHHH", elf, 40, 52, 32, phnum, 0, 0)
    for index, (vaddr, memsz) in enumerate(loads):
        struct.pack_into("<8I", elf, 52 + 32 * index, 1, data, vaddr, vaddr, 4, memsz, 5, 4)
    elf[data:] = b"\x34\x12\x00\x00"
    return bytes(elf)


def _both_endian32(value: int) -> bytes:
    """ECMA-119 7.3.3: a 32-bit value recorded little-endian then big-endian."""
    return value.to_bytes(4, "little") + value.to_bytes(4, "big")


def _dir_record(name: bytes, extent_lba: int, size_bytes: int, is_dir: bool) -> bytes:
    """Build one ECMA-119 9.1 directory record, padded to an even length."""
    rec = bytearray(33 + len(name))
    rec[1] = 0                                          # extended attribute length
    rec[2:10] = _both_endian32(extent_lba)              # 9.1.3 extent
    rec[10:18] = _both_endian32(size_bytes)             # 9.1.4 data length
    rec[25] = 0x02 if is_dir else 0x00                  # 9.1.6 file flags
    rec[28:32] = (1).to_bytes(2, "little") + (1).to_bytes(2, "big")
    rec[32] = len(name)                                 # 9.1.10 identifier length
    rec[33:33 + len(name)] = name
    if len(rec) % 2:                                    # 9.1.12 padding
        rec.append(0)
    rec[0] = len(rec)
    return bytes(rec)


def create_custom_sfo_iso(path: Path, sfo_bytes: bytes, volume_id: str = "CUSTOM_VOL") -> None:
    create_test_iso(path, volume_id=volume_id, sfo_bytes=sfo_bytes)


def build_pbp_package(
    path: Path,
    *,
    disc_id: str = "TEST00001",
    title: str = "PBP Test Package",
    sfo_bytes: bytes | None = None,
    offsets: list[int] | None = None,
    header: bytes | None = None,
    padding: bytes = b"",
) -> None:
    """Write a synthetic PlayStation Store package (PBP).

    The default layout is a 40-byte header, PARAM.SFO in the first section and
    every other section empty. ``offsets`` or a whole ``header`` override that
    layout so a caller can build a hostile package.
    """
    if sfo_bytes is None:
        sfo_bytes = build_param_sfo(disc_id, title)
    if header is None:
        built = bytearray(40)
        built[:4] = b"\x00PBP"
        struct.pack_into("<I", built, 4, 0x00010000)
        if offsets is None:
            sfo_end = 40 + len(sfo_bytes)
            offsets = [40] + [sfo_end] * 7
        for index, value in enumerate(offsets):
            struct.pack_into("<I", built, 8 + index * 4, value)
        header = bytes(built)
    path.write_bytes(header + sfo_bytes + padding)


class IsoParityTests(unittest.TestCase):
    gcc: ClassVar[str]
    _build_dir: ClassVar[Path]
    harness_c: ClassVar[Path]
    harness_exe: ClassVar[Path]
    path_limit_exe: ClassVar[Path]

    @classmethod
    def setUpClass(cls) -> None:
        super().setUpClass()
        cls.gcc = cast(str, shutil.which("gcc"))
        if not cls.gcc:
            raise unittest.SkipTest("TOOLCHAIN_MISSING: gcc not available")

        # Compile the native test runner once.  The tests assert parser and
        # launch behaviour, not compiler freshness; each test still receives
        # its own input/output directory below.
        cls._build_dir = Path(tempfile.mkdtemp(prefix="nk_parity_build_"))
        # A class cleanup runs even when a compile failure below raises out of
        # setUpClass, which tearDownClass does not.
        cls.addClassCleanup(shutil.rmtree, cls._build_dir, True)
        cls.harness_c = cls._build_dir / "parity_harness.c"
        executable_name = "parity_harness.exe" if sys.platform == "win32" else "parity_harness"
        cls.harness_exe = cls._build_dir / executable_name
        cls.path_limit_exe = cls._build_dir / (
            "parity_path_limit.exe" if sys.platform == "win32" else "parity_path_limit"
        )

        core_srcs = [
            ROOT / "src" / "core" / "nk_shared_helpers.c",
            ROOT / "src" / "core" / "nk_iso.c",
            ROOT / "src" / "core" / "nk_library.c",
            ROOT / "src" / "core" / "nk_title_manifest.c",
            ROOT / "src" / "core" / "nk_json.c",
            ROOT / "src" / "core" / "generated" / "nk_title_catalog.c",
            # The launch modes are required: a missing nk_launch.c fails the build.
            ROOT / "src" / "core" / "nk_launch.c",
            ROOT / "src" / "core" / "nk_adhoc_relay.c",
            # nk_launch.c starts a homebrew program from its Memory Stick folder
            # (nk_pbp_homebrew.c), whose package reader reports through the PSP crypto
            # engine (nk_psp_fail), which reads the key store.
            *(ROOT / "src" / "core" / name
              for name in ("nk_pbp_homebrew.c", "nk_psp_crypto.c",
                           "nk_psp_keystore.c", "nk_psp_builtin_keys.c")),
        ]
        if sys.platform == "win32":
            core_srcs.append(ROOT / "src" / "core" / "nk_platform_win32.c")
        else:
            core_srcs.append(ROOT / "src" / "core" / "nk_platform_posix.c")

        harness_code = """#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "nk_iso.h"
#include "../rt/prx_loader.h"
#include "nk_library.h"
#include "nk_title_manifest.h"
#include "nk_launch.h"

static bool print_module_path(const char *path, const NkIsoDirEntry *entry,
                              void *userdata) {
    (void)userdata;
    size_t length = strlen(entry->name);
    if (length >= 4 &&
        tolower((unsigned char)entry->name[length - 4]) == '.' &&
        tolower((unsigned char)entry->name[length - 3]) == 'p' &&
        tolower((unsigned char)entry->name[length - 2]) == 'r' &&
        tolower((unsigned char)entry->name[length - 1]) == 'x') {
        printf("MODULE_PATH:%s\\n", path);
    } else if (length >= 4 &&
        tolower((unsigned char)entry->name[length - 4]) == '.' &&
        tolower((unsigned char)entry->name[length - 3]) == 'e' &&
        tolower((unsigned char)entry->name[length - 2]) == 'l' &&
        tolower((unsigned char)entry->name[length - 1]) == 'f') {
        printf("MODULE_PATH:%s\\n", path);
    }
    return true;
}

static bool harness_file_read(void *context, uint64_t offset, void *dst, uint32_t bytes) {
    FILE *image = (FILE *)context;
    if (offset > 0x7fffffffULL || fseek(image, (long)offset, SEEK_SET) != 0) return false;
    return fread(dst, 1, bytes, image) == bytes;
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    const char *mode = argv[1];

    if (strcmp(mode, "elf_layout") == 0) {
        /* elf_layout <file> <module 0|1>: the shared PSP ELF32/MIPS layout rule. */
        if (argc < 4) return 1;
        FILE *image = fopen(argv[2], "rb");
        if (!image) return 2;
        if (fseek(image, 0, SEEK_END) != 0) { fclose(image); return 2; }
        long image_size = ftell(image);
        if (image_size < 0) { fclose(image); return 2; }
        const char *rule = nk_elf32_mips_layout_violation(
            harness_file_read, image, (uint64_t)image_size, strcmp(argv[3], "1") == 0);
        fclose(image);
        printf("ELF_LAYOUT:%s\\n", rule ? rule : "USABLE");
        return 0;
    }

    if (strcmp(mode, "start_entry") == 0) {
        /* start_entry <file>: where the PSP starts this executable (sr_prx_start_entry in src/rt/prx_loader.h). */
        if (argc < 3) return 1;
        FILE *image = fopen(argv[2], "rb");
        if (!image) return 2;
        if (fseek(image, 0, SEEK_END) != 0) { fclose(image); return 2; }
        long image_size = ftell(image);
        if (image_size < 0) { fclose(image); return 2; }
        uint32_t entry = 0;
        const char *rule = sr_prx_start_entry(harness_file_read, image, (uint64_t)image_size,
                                              &entry);
        fclose(image);
        printf("START_ENTRY:%s:%08x\\n", rule ? rule : "OK", entry);
        return 0;
    }

    if (strcmp(mode, "module_walk") == 0) {
        if (argc < 3) return 1;
        NkIsoReader *reader = nk_iso_reader_open(argv[2]);
        if (!reader) return 2;
        NkIsoModuleWalkStatus status = nk_iso_reader_walk_module_tree(
            reader, print_module_path, NULL);
        printf("MODULE_WALK_STATUS:%d\\n", (int)status);
        nk_iso_reader_close(reader);
        return 0;
    }

    if (strcmp(mode, "inspect") == 0) {
        if (argc < 3) return 1;
        const char *iso_path = argv[2];
        NkIsoMetadata meta;
        NkResult res = nk_iso_inspect(iso_path, &meta);
        if (res != NK_OK) {
            printf("RESULT:ERROR:%d:%s\\n", (int)res, meta.error_message);
            printf("BOUNDARY:%s\\n", meta.boundary_code);
            printf("MESSAGE:%s\\n", meta.error_message);
            return 0;
        }
        printf("RESULT:OK\\n");
        printf("DISC_ID:%s\\n", meta.disc_id);
        printf("SFO_DISC_ID:%s\\n", meta.sfo_disc_id);
        printf("NON_RETAIL:%d\\n", meta.non_retail ? 1 : 0);
        printf("IDENTITY_SHA256:%s\\n", meta.identity_sha256);
        printf("NON_RETAIL_KEY_LABEL:%d\\n", nk_disc_id_is_non_retail_key(meta.disc_id) ? 1 : 0);
        printf("DISC_KEY_VERSION:%u\\n", (unsigned)meta.disc_key_version);
        printf("LEGACY_DISC_ID:%s\\n", meta.legacy_disc_id);
        printf("LEGACY_IDENTITY_SHA256:%s\\n", meta.legacy_identity_sha256);
        printf("TITLE:%s\\n", meta.title_name);
        printf("VERSION:%s\\n", meta.disc_version);
        printf("VOLUME_ID:%s\\n", meta.volume_id);
        printf("HAS_PSP_GAME:%d\\n", meta.has_psp_game_folder ? 1 : 0);
        printf("SUPPORTED:%d\\n", meta.is_supported ? 1 : 0);
        printf("PARAM_SFO_PARSED:%d\\n", meta.param_sfo_parsed ? 1 : 0);
        printf("EBOOT_KIND:%d\\n", (int)meta.executables.eboot.kind);
        printf("BOOT_KIND:%d\\n", (int)meta.executables.boot.kind);
        printf("SELECTED_EXECUTABLE:%d\\n", (int)meta.executables.selected);
        printf("BOOT_FALLBACK:%d\\n", meta.executables.boot_fallback ? 1 : 0);
        printf("MATCHED_ID:%s\\n", meta.matched_title_id[0] ? meta.matched_title_id : "NONE");
        printf("STATUS:%d\\n", (int)meta.status);
        return 0;
    }

    if (strcmp(mode, "library_key") == 0) {
        /* The key an image is added under and how each entry of a library file relates
           to it (nk_iso_library_key_for_image, nk_iso_library_key_match); then the image
           is added under that key and removed again, as the player does. */
        if (argc < 4) return 1;
        static NkLibrary lib;  /* too large for the stack (NK_MAX_GAMES) */
        static NkLibrary saved;
        static NkIsoMetadata meta;
        if (nk_library_load(&lib, argv[3]) != NK_OK) return 3;
        if (nk_iso_inspect(argv[2], &meta) != NK_OK) return 4;
        for (int i = 0; i < lib.count; i++) {
            printf("MATCH:%s:%u:%d\\n", lib.entries[i].disc_id,
                   (unsigned)lib.entries[i].disc_key_version,
                   (int)nk_iso_library_key_match(meta.disc_id, meta.disc_key_version,
                                                 meta.legacy_disc_id, lib.entries[i].disc_id,
                                                 lib.entries[i].disc_key_version));
        }
        NkGameEntry added;
        memset(&added, 0, sizeof(added));
        added.disc_key_version = nk_iso_library_key_for_image(
            lib.entries, lib.count, meta.disc_id, meta.disc_key_version, meta.legacy_disc_id,
            added.disc_id, sizeof(added.disc_id));
        printf("ADD:%s:%u\\n", added.disc_id, (unsigned)added.disc_key_version);
        snprintf(added.title_name, sizeof(added.title_name), "%s", meta.title_name);
        nk_iso_title_for_key(added.title_name, sizeof(added.title_name), meta.disc_id,
                             added.disc_id, NULL);
        printf("TITLE:%s\\n", added.title_name);
        snprintf(added.iso_path, sizeof(added.iso_path), "%s", argv[2]);
        if (nk_library_add_or_update(&lib, &added) != NK_OK) return 5;
        if (nk_library_save(&lib, argv[3]) != NK_OK) return 6;
        if (nk_library_load(&saved, argv[3]) != NK_OK) return 7;
        printf("COUNT_AFTER_ADD:%d\\n", saved.count);
        const NkGameEntry *found = nk_library_find_by_disc_id(&saved, added.disc_id);
        printf("SAVED_VERSION:%u\\n", found ? (unsigned)found->disc_key_version : 99u);
        if (nk_library_remove_by_file(&saved, added.disc_id, argv[2]) != NK_OK) return 8;
        printf("COUNT_AFTER_REMOVE:%d\\n", saved.count);
        return 0;
    }

    if (strcmp(mode, "library_test") == 0) {
        if (argc < 3) return 1;
        const char *lib_json = argv[2];
        static NkLibrary lib;  /* too large for the stack (NK_MAX_GAMES) */
        nk_library_init(&lib);

        NkGameEntry g1;
        memset(&g1, 0, sizeof(g1));
        snprintf(g1.disc_id, sizeof(g1.disc_id), "UCUS98701");
        snprintf(g1.title_name, sizeof(g1.title_name), "Hot Shots Tennis");
        snprintf(g1.title_id, sizeof(g1.title_id), "hst-ucus98701-v1");
        snprintf(g1.iso_path, sizeof(g1.iso_path), "C:/games/hst.iso");
        g1.status = NK_STATUS_VERIFIED;
        g1.is_experimental = true;
        g1.executable_eboot_kind = 2;
        g1.executable_boot_kind = 1;
        g1.executable_selection = 2;
        g1.executable_boot_fallback = true;
        snprintf(g1.selected_executable, sizeof(g1.selected_executable), "BOOT.BIN");
        g1.is_prepared = true;
        g1.iso_size_bytes = 1000000;

        assert(nk_library_add_or_update(&lib, &g1) == NK_OK);
        assert(nk_library_count(&lib) == 1);
        assert(nk_library_save(&lib, lib_json) == NK_OK);

        static NkLibrary loaded;  /* too large for the stack (NK_MAX_GAMES) */
        assert(nk_library_load(&loaded, lib_json) == NK_OK);
        assert(nk_library_count(&loaded) == 1);

        const NkGameEntry *entry = nk_library_find_by_disc_id(&loaded, "UCUS98701");
        assert(entry != NULL);
        assert(strcmp(entry->title_name, "Hot Shots Tennis") == 0);
        assert(strcmp(entry->title_id, "hst-ucus98701-v1") == 0);
        assert(entry->status == NK_STATUS_VERIFIED);
        assert(entry->is_prepared == true);
        assert(entry->is_experimental == true);
        assert(entry->executable_eboot_kind == 2);
        assert(entry->executable_boot_kind == 1);
        assert(entry->executable_selection == 2);
        assert(entry->executable_boot_fallback == true);
        assert(strcmp(entry->selected_executable, "BOOT.BIN") == 0);

        /* Test remove */
        assert(nk_library_remove(&loaded, "UCUS98701") == NK_OK);
        assert(nk_library_count(&loaded) == 0);
        assert(nk_library_save(&loaded, lib_json) == NK_OK);

        printf("LIBRARY_TEST_PASSED\\n");
        return 0;
    }

    if (strcmp(mode, "experimental_profile") == 0) {
        if (argc < 4) return 1;
        const char *iso_path = argv[2];
        const char *user_data_root = argv[3];
        NkIsoMetadata meta;
        NkResult inspect_result = nk_iso_inspect(iso_path, &meta);
        if (inspect_result != NK_OK) {
            printf("PROFILE_RESULT:ERROR:DISC:%s\\n", meta.error_message);
            return 0;
        }
        char profile_id[64] = {0};
        char error[256] = {0};
        bool ok = nk_title_manifest_write_experimental_profile(
            iso_path, meta.param_sfo_parsed, meta.disc_id, meta.title_name,
            meta.executables.selected_path, user_data_root, profile_id,
            sizeof(profile_id), error, sizeof(error));
        if (!ok) {
            printf("PROFILE_RESULT:ERROR:%s\\n", error);
            return 0;
        }
        printf("PROFILE_RESULT:OK\\n");
        printf("PROFILE_ID:%s\\n", profile_id);
        printf("PACKAGE_AVAILABLE:%d\\n",
               nk_launch_runtime_package_available(user_data_root, profile_id) ? 1 : 0);
        NkGameEntry game;
        memset(&game, 0, sizeof(game));
        snprintf(game.disc_id, sizeof(game.disc_id), "%s", meta.disc_id);
        snprintf(game.title_id, sizeof(game.title_id), "%s", profile_id);
        snprintf(game.iso_path, sizeof(game.iso_path), "%s", iso_path);
        NkLaunchSession session;
        NkResult launch_result = nk_launch_prepare_session(
            &session, &game, user_data_root);
        printf("LAUNCH_PREPARE:%s\\n",
               launch_result == NK_OK ? "OK" : "REFUSED");
        printf("LAUNCH_REASON:%s\\n", session.last_error);
        return 0;
    }

    if (strcmp(mode, "launch_test") == 0) {
        if (argc < 4) return 1;
        const char *repo_root = argv[2];
        const char *iso_path = argv[3];

        const char *want_disc = argc > 4 ? argv[4] : "UCUS98701";
        const char *want_title = argc > 5 ? argv[5] : "hst-ucus98701-v1";

        NkGameEntry g;
        memset(&g, 0, sizeof(g));
        snprintf(g.disc_id, sizeof(g.disc_id), "%s", want_disc);
        snprintf(g.title_id, sizeof(g.title_id), "%s", want_title);
        snprintf(g.iso_path, sizeof(g.iso_path), "%s", iso_path);

        NkLaunchSession session;
        NkResult res = nk_launch_prepare_session(&session, &g, repo_root);
        if (res != NK_OK) {
            printf("LAUNCH_PREPARE_ERROR:%d:%s\\n", (int)res, session.last_error);
            return 0;
        }
        printf("LAUNCH_PREPARE_OK\\n");
        printf("EXE:%s\\n", session.executable_path);
        printf("ISO:%s\\n", session.iso_path);
        printf("BASE:0x%08x\\n", session.base_address);
        printf("ENTRY:0x%08x\\n", session.entry_point);
        return 0;
    }

    if (strcmp(mode, "boot_roundtrip") == 0) {
        if (argc < 5) return 1;
        static NkLibrary lib;  /* too large for the stack (NK_MAX_GAMES) */
        NkResult load_result = nk_library_load(&lib, argv[2]);
        printf("LOAD_RESULT:%d\\n", (int)load_result);
        if (load_result != NK_OK) return 0;
        const NkGameEntry *entry = nk_library_find_by_disc_id(&lib, "TEST00001");
        if (!entry) return 2;
        printf("BOOT_LENGTH:%u\\n", (unsigned)strlen(entry->boot_executable));
        NkResult save_result = nk_library_save(&lib, argv[3]);
        printf("SAVE_RESULT:%d\\n", (int)save_result);
        if (save_result != NK_OK) return 0;
        static NkLibrary saved_lib;  /* too large for the stack (NK_MAX_GAMES) */
        NkResult saved_load_result = nk_library_load(&saved_lib, argv[3]);
        const NkGameEntry *saved_entry = saved_load_result == NK_OK
            ? nk_library_find_by_disc_id(&saved_lib, "TEST00001") : NULL;
        printf("SAVE_BOOT_MATCH:%d\\n",
               saved_entry && strcmp(entry->boot_executable,
                                     saved_entry->boot_executable) == 0);
        NkLaunchSession session;
        NkResult session_result = nk_launch_prepare_session(&session, entry, argv[4]);
        printf("SESSION_RESULT:%d\\n", (int)session_result);
        printf("SESSION_BOOT_MATCH:%d\\n",
               strcmp(entry->boot_executable, session.boot_executable) == 0);
        return 0;
    }

    if (strcmp(mode, "boot_session_unterminated") == 0) {
        NkGameEntry game;
        NkLaunchSession session;
        memset(&game, 0, sizeof(game));
        memset(&session, 0, sizeof(session));
        snprintf(game.disc_id, sizeof(game.disc_id), "TEST00001");
        snprintf(game.title_id, sizeof(game.title_id), "synthetic-allegrex-v1");
        memset(game.boot_executable, 'X', sizeof(game.boot_executable));
        NkResult result = nk_launch_prepare_session(&session, &game, ".");
        printf("SESSION_RESULT:%d\\n", (int)result);
        printf("ERROR:%s\\n", session.last_error);
        return 0;
    }

    if (strcmp(mode, "boot_save_unterminated") == 0) {
        if (argc < 3) return 1;
        static NkLibrary lib;  /* too large for the stack (NK_MAX_GAMES) */
        NkGameEntry game;
        memset(&game, 0, sizeof(game));
        nk_library_init(&lib);
        snprintf(game.disc_id, sizeof(game.disc_id), "TEST00001");
        memset(game.boot_executable, 'X', sizeof(game.boot_executable));
        assert(nk_library_add_or_update(&lib, &game) == NK_OK);
        NkResult result = nk_library_save(&lib, argv[2]);
        printf("SAVE_RESULT:%d\\n", (int)result);
        return 0;
    }

    if (strcmp(mode, "reader_test") == 0) {
        if (argc < 3) return 1;
        const char *iso_path = argv[2];
        NkIsoReader *reader = nk_iso_reader_open(iso_path);
        if (!reader) {
            printf("READER:OPEN_FAILED\\n");
            return 0;
        }
        printf("READER:OPEN_OK\\n");
        printf("VOLUME_ID:%s\\n", nk_iso_reader_volume_id(reader));
        printf("FILE_SIZE:%llu\\n", (unsigned long long)nk_iso_reader_file_size(reader));

        uint32_t lba = 0, sz = 0;
        bool is_dir = false;
        int rc = nk_iso_reader_lookup(reader, "PSP_GAME/PARAM.SFO", &lba, &sz, &is_dir);
        printf("LOOKUP_SFO:%d:%u:%u:%d\\n", rc, (unsigned)lba, (unsigned)sz, is_dir ? 1 : 0);

        rc = nk_iso_reader_lookup(reader, "PSP_GAME", &lba, &sz, &is_dir);
        printf("LOOKUP_DIR:%d:%u:%u:%d\\n", rc, (unsigned)lba, (unsigned)sz, is_dir ? 1 : 0);

        rc = nk_iso_reader_lookup(reader, "NONEXISTENT", &lba, &sz, &is_dir);
        printf("LOOKUP_MISS:%d\\n", rc);

        NkIsoDirEntry de;
        rc = nk_iso_reader_list(reader, "", 0, &de);
        printf("LIST_ROOT_0:%d:%s:%u:%u:%d\\n", rc, de.name, (unsigned)de.lba, (unsigned)de.size, de.is_directory ? 1 : 0);

        rc = nk_iso_reader_list(reader, "", 1, &de);
        printf("LIST_ROOT_1:%d\\n", rc);

        uint8_t buf[256];
        int bytes = nk_iso_reader_read(reader, 32, 0, buf, sizeof(buf));
        printf("READ_SFO:%d\\n", bytes);

        bytes = nk_iso_reader_read(reader, 2000, 0, buf, sizeof(buf));
        printf("READ_EOF:%d\\n", bytes);

        nk_iso_reader_close(reader);
        return 0;
    }

    if (strcmp(mode, "lookup") == 0) {
        if (argc < 4) return 1;
        NkIsoReader *reader = nk_iso_reader_open(argv[2]);
        if (!reader) {
            printf("READER:OPEN_FAILED\\n");
            return 0;
        }
        uint32_t lba = 0, sz = 0;
        bool is_dir = false;
        int rc = nk_iso_reader_lookup(reader, argv[3], &lba, &sz, &is_dir);
        printf("LOOKUP:%d:%u:%u:%d\\n", rc, (unsigned)lba, (unsigned)sz, is_dir ? 1 : 0);
        nk_iso_reader_close(reader);
        return 0;
    }

    if (strcmp(mode, "entries") == 0) {
        if (argc < 4) return 1;
        NkIsoReader *reader = nk_iso_reader_open(argv[2]);
        if (!reader) {
            printf("READER:OPEN_FAILED\\n");
            return 0;
        }
        for (uint32_t index = 0;; index++) {
            NkIsoDirEntry de;
            int rc = nk_iso_reader_list(reader, argv[3], index, &de);
            if (rc != 1) {
                printf("END:%d\\n", rc);
                break;
            }
            printf("ENTRY:%s:%u:%u:%d\\n", de.name, (unsigned)de.lba, (unsigned)de.size,
                   de.is_directory ? 1 : 0);
        }
        nk_iso_reader_close(reader);
        return 0;
    }

    return 2;
}
"""
        cls.harness_c.write_text(harness_code, encoding="utf-8")

        cmd = [
            cls.gcc,
            "-Wall", "-Wextra", "-Werror", "-std=c99",
            "-I", str(ROOT / "src" / "core"),
            "-I", str(ROOT / "src" / "core" / "generated"),
            str(cls.harness_c),
        ] + [str(s) for s in core_srcs] + [
            *(["-lshell32", "-lole32", "-luuid"] if sys.platform == "win32" else []),
            "-o", str(cls.harness_exe),
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode:
            raise AssertionError(f"Compilation of parity harness failed: {res.stderr}")
        path_limit_cmd = [cls.gcc, "-DNK_ISO_MODULE_TREE_MAX_PATH_BYTES=64", *cmd[1:-1],
                          str(cls.path_limit_exe)]
        res = subprocess.run(path_limit_cmd, capture_output=True, text=True)
        if res.returncode:
            raise AssertionError(f"Compilation of path-limit harness failed: {res.stderr}")

    def setUp(self) -> None:
        # Inputs and any application data remain isolated per test even though
        # the immutable native runner is shared by the class.
        self.temp_dir = Path(tempfile.mkdtemp(prefix="nk_parity_test_"))
        self.exe_path = type(self).harness_exe

    def tearDown(self) -> None:
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def _run_native_inspect(self, iso_path: Path) -> dict[str, str]:
        cmd = [str(self.exe_path), "inspect", str(iso_path)]
        res = subprocess.run(
            cmd, capture_output=True, text=True, encoding="utf-8", errors="strict"
        )
        self.assertEqual(res.returncode, 0, f"Native inspect failed: {res.stderr}")
        lines = res.stdout.strip().splitlines()
        out: dict[str, str] = {}
        for l in lines:
            if ":" in l:
                k, v = l.split(":", 1)
                out[k] = v
        return out

    def _run_native_module_walk(
        self, iso_path: Path, executable: Path | None = None
    ) -> tuple[int, list[str]]:
        result = subprocess.run(
            [str(executable or self.exe_path), "module_walk", str(iso_path)],
            capture_output=True, text=True, encoding="utf-8", errors="surrogateescape",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        status = None
        paths = []
        for line in result.stdout.splitlines():
            if line.startswith("MODULE_WALK_STATUS:"):
                status = int(line.split(":", 1)[1])
            elif line.startswith("MODULE_PATH:"):
                paths.append(line.split(":", 1)[1])
        self.assertIsNotNone(status, result.stdout)
        return cast(int, status), paths

    def test_module_discovery_covers_usrdir_module_and_nested_paths(self) -> None:
        iso_file = self.temp_dir / "module-tree.iso"
        paths = {
            "PSP_GAME/USRDIR/module/sdk.prx": build_plain_mips_elf(0xFFA0),
            "PSP_GAME/USRDIR/DATA/MODULE/MODULE/nested.prx": build_psp_container(),
            "PSP_GAME/USRDIR/Mixed/SDK_VARIANT.PrX": build_psp_container(),
        }
        create_test_iso_with_module_tree(iso_file, paths)
        candidates = list_disc_module_candidates(iso_file)
        members = {candidate["members"][0] for candidate in candidates}
        self.assertEqual(members, {tuple(path.split("/")) for path in paths})
        package_candidates = nk_cli._discover_iso_module_candidates(
            iso_file, "EBOOT.BIN"
        )
        package_members = {
            tuple(candidate["directory"]) + (candidate["entry"].name,)
            for candidate in package_candidates
        }
        self.assertEqual(package_members, members)
        copied_modules = nk_cli._copy_optional_modules(
            iso_file,
            {"modules": [{
                "name": "manifest-sdk.prx",
                "guest_path": "disc0:/PSP_GAME/USRDIR/module/sdk.prx",
                "role": "guest-prx", "required": True,
            }]},
            self.temp_dir / "manifest-package", None,
        )
        self.assertIsNotNone(copied_modules)
        self.assertEqual(
            (copied_modules / "manifest-sdk.prx").read_bytes(), paths[
                "PSP_GAME/USRDIR/module/sdk.prx"
            ],
        )
        native_status, native_paths = self._run_native_module_walk(iso_file)
        self.assertEqual(native_status, 0)
        self.assertEqual(set(native_paths), set(paths))

    def test_unusable_module_candidate_names_the_failing_rule(self) -> None:
        # p_align 3 is not a power of two: the shared layout rule refuses the module, and the
        # candidate names that rule, so the refusal says which module and which condition.
        bad_align = bytearray(build_plain_mips_elf(0xFFA0, vaddr=0, entry=0xFFFFFFFF))
        struct.pack_into("<I", bad_align, 52 + 28, 3)
        iso_file = self.temp_dir / "bad-align-module.iso"
        create_test_iso_with_module_tree(
            iso_file, {"PSP_GAME/USRDIR/module/bad-align.prx": bytes(bad_align)}
        )
        candidates = nk_cli._discover_iso_module_candidates(iso_file, "EBOOT.BIN")
        self.assertEqual([candidate["name"] for candidate in candidates], ["bad-align.prx"])
        self.assertEqual(candidates[0]["kind"], "unsupported")
        self.assertIn("segment-alignment", candidates[0]["reason"])
        self.assertEqual(
            nk_cli._unready_module_lines(candidates),
            ["MODULE bad-align.prx: not ready (" + candidates[0]["reason"] + ")"],
        )

    def test_unopenable_kernel_mode_container_is_left_out_by_name(self) -> None:
        # A disc may carry Sony kernel-mode drivers in containers the boundary does not
        # open. Their plaintext ~PSP header repeats the module attribute, so the intake
        # knows they are kernel-mode, and the runtime never runs kernel-mode code: they
        # are deferred by name instead of stopping the build. A user-mode container the
        # boundary cannot open still stops by name.
        def container(attribute: int) -> bytes:
            header = bytearray(0x150)
            header[:4] = b"~PSP"
            struct.pack_into("<H", header, 4, attribute)
            return bytes(header)  # zero segment count: not a header the boundary opens

        work = self.temp_dir / "kernel-container"
        user_root = work / "user"
        user_root.mkdir(parents=True)
        selected = work / "selected.elf"
        selected.write_bytes(build_plain_mips_elf())
        iso_file = work / "kernel.iso"
        create_test_iso_with_module_tree(
            iso_file, {"PSP_GAME/USRDIR/PRX/videocodec.prx": container(0x1006)},
            disc_id="TEST00081",
        )
        candidates = nk_cli._discover_iso_module_candidates(iso_file, "EBOOT.BIN")
        self.assertEqual([(c["name"], c["kind"]) for c in candidates],
                         [("videocodec.prx", "kernel-container")])
        declared, deferred, staged = nk_cli._declare_experimental_disc_modules(
            iso_file, user_root, "TEST00081", candidates, selected, {},
        )
        self.assertEqual(declared, [])
        self.assertIsNone(staged)
        self.assertEqual([item["name"] for item in deferred], ["videocodec.prx"])
        self.assertIn("kernel-mode", deferred[0]["reason"])

        user_iso = work / "user-mode.iso"
        create_test_iso_with_module_tree(
            user_iso, {"PSP_GAME/USRDIR/PRX/game.prx": container(0x0000)},
            disc_id="TEST00082",
        )
        candidates = nk_cli._discover_iso_module_candidates(user_iso, "EBOOT.BIN")
        self.assertEqual(candidates[0]["kind"], "unsupported")
        with self.assertRaises(nk_cli.ExperimentalModuleIntakeError) as ctx:
            nk_cli._declare_experimental_disc_modules(
                user_iso, user_root, "TEST00082", candidates, selected, {},
            )
        self.assertEqual(ctx.exception.boundary_code, "GUEST_MODULE_FORMAT_UNSUPPORTED")

    def test_container_with_an_empty_data_segment_is_an_encrypted_module(self) -> None:
        # Small user libraries declare two segments in their ~PSP header, the second
        # (data) of size 0 when they have no data. That is a container the decryption
        # step opens; only a header whose segments are all empty is refused.
        def container(sizes: tuple[int, ...]) -> bytes:
            header = bytearray(0x150)
            header[:4] = b"~PSP"
            header[0x27] = len(sizes)
            struct.pack_into(f"<{len(sizes)}I", header, 0x54, *sizes)
            return bytes(header)

        iso_file = self.temp_dir / "empty-data-segment.iso"
        create_test_iso_with_module_tree(iso_file, {
            "PSP_GAME/USRDIR/module/auth/lib_authorized.prx": container((0x118, 0)),
            "PSP_GAME/USRDIR/module/auth/empty.prx": container((0, 0)),
        })
        self.assertTrue(iso_inspect._prx_container_header_supported(container((0x118, 0))))
        self.assertFalse(iso_inspect._prx_container_header_supported(container((0, 0))))
        self.assertEqual(
            [(c["name"], c["kind"]) for c in list_disc_module_candidates(iso_file)],
            [("lib_authorized.prx", "encrypted")],
        )
        kinds = {c["name"]: c["kind"]
                 for c in nk_cli._discover_iso_module_candidates(iso_file, "EBOOT.BIN")}
        self.assertEqual(kinds, {"lib_authorized.prx": "encrypted-prx", "empty.prx": "unsupported"})

    def test_pbp_container_named_like_a_module_is_left_out_by_name(self) -> None:
        # A PBP (an EBOOT image for LoadExec or the game-sharing utility) named .prx is
        # not a module the kernel's module loader takes: it is left out by name, not a
        # format stop. A file that only starts with the magic is still unsupported.
        pbp = bytearray(0x200)
        pbp[:8] = b"\0PBP\x00\x00\x01\x00"
        struct.pack_into("<8I", pbp, 8, 40, 0x100, 0x100, 0x100, 0x100, 0x100, 0x100, 0x200)
        pbp[0x100:0x104] = b"~PSP"
        broken = bytes(pbp[:8]) + struct.pack("<8I", 40, 0x300, 0, 0, 0, 0, 0, 0) + bytes(0x60)
        work = self.temp_dir / "pbp-module"
        user_root = work / "user"
        user_root.mkdir(parents=True)
        selected = work / "selected.elf"
        selected.write_bytes(build_plain_mips_elf())
        iso_file = work / "pbp.iso"
        create_test_iso_with_module_tree(
            iso_file, {"PSP_GAME/USRDIR/share/gs_one.prx": bytes(pbp)}, disc_id="TEST00085",
        )
        candidates = nk_cli._discover_iso_module_candidates(iso_file, "EBOOT.BIN")
        self.assertEqual([(c["name"], c["kind"]) for c in candidates],
                         [("gs_one.prx", "pbp-container")])
        declared, deferred, staged = nk_cli._declare_experimental_disc_modules(
            iso_file, user_root, "TEST00085", candidates, selected, {},
        )
        self.assertEqual((declared, staged), ([], None))
        self.assertEqual([item["name"] for item in deferred], ["gs_one.prx"])
        self.assertIn("PBP executable container", deferred[0]["reason"])

        broken_iso = work / "broken-pbp.iso"
        create_test_iso_with_module_tree(
            broken_iso, {"PSP_GAME/USRDIR/share/gs_two.prx": broken}, disc_id="TEST00086",
        )
        candidates = nk_cli._discover_iso_module_candidates(broken_iso, "EBOOT.BIN")
        self.assertEqual(candidates[0]["kind"], "unsupported")

    def test_fixed_address_overlay_elf_is_a_code_overlay(self) -> None:
        # ELF type 0xFFA8: one segment at a fixed link address, no entry point, no module
        # information; the game copies it into place itself. It is found by its header
        # whatever its name, staged as it is and declared runtime-placed like an MWo3
        # overlay; one whose header breaks the form is a named unsupported candidate.
        overlay = build_plain_mips_elf(0xFFA8, vaddr=0x089AC2F0, entry=0, p_flags=7)
        work = self.temp_dir / "fixed-overlay"
        user_root = work / "user"
        user_root.mkdir(parents=True)
        selected = work / "selected.elf"
        selected.write_bytes(build_plain_mips_elf())
        iso_file = work / "fixed-overlay.iso"
        create_test_iso_with_module_tree(
            iso_file, {"PSP_GAME/USRDIR/stage.elf": overlay,
                       "PSP_GAME/USRDIR/DATA/battle.bin": overlay},
            disc_id="TEST00087",
        )
        candidates = nk_cli._discover_iso_module_candidates(iso_file, "EBOOT.BIN")
        self.assertEqual(sorted((c["name"], c["kind"]) for c in candidates),
                         [("battle.bin", "code-overlay"), ("stage.elf", "code-overlay")])
        declared, deferred, staged = nk_cli._declare_experimental_disc_modules(
            iso_file, user_root, "TEST00087", candidates, selected, {},
        )
        self.assertEqual(deferred, [])
        self.assertIn({
            "name": "stage.elf", "required": True, "role": "guest-prx",
            "placement": "runtime", "guest_path": "disc0:/PSP_GAME/USRDIR/stage.elf",
        }, declared)
        self.assertEqual((staged / "stage.elf").read_bytes(), overlay)

        entry_point = build_plain_mips_elf(0xFFA8, vaddr=0x089AC2F0, p_flags=7)
        bad_iso = work / "bad-fixed-overlay.iso"
        create_test_iso_with_module_tree(
            bad_iso, {"PSP_GAME/USRDIR/stage.elf": entry_point}, disc_id="TEST00088")
        candidates = nk_cli._discover_iso_module_candidates(bad_iso, "EBOOT.BIN")
        self.assertEqual(candidates[0]["kind"], "unsupported")
        self.assertIn("code-overlay header is malformed", candidates[0]["reason"])
        self.assertIn("names an entry point", candidates[0]["reason"])

    @staticmethod
    def _build_prx_without_module_info() -> bytes:
        """A relocatable module that declares no SceModuleInfo (a zero phdr[0].p_paddr)."""
        elf = bytearray(build_plain_mips_elf(0xFFA0))
        struct.pack_into("<I", elf, 0x34 + 12, 0)
        return bytes(elf)

    def test_plain_prx_is_found_by_its_header_whatever_its_file_name(self) -> None:
        # The kernel's module loader reads the header, not the name, so a game can load a
        # plain PRX named like data; left undeclared, its start was skipped and the first
        # call into it hit a null pointer. The same bytes under other names show what stays
        # out: the executables, a fixed-address ELF, and a file that only opens with the magic.
        prx = self._build_prx_without_module_info()
        work = self.temp_dir / "renamed-module"
        user_root = work / "user"
        user_root.mkdir(parents=True)
        selected = work / "selected.elf"
        selected.write_bytes(build_plain_mips_elf())
        iso_file = work / "renamed.iso"
        create_test_iso_with_module_tree(
            iso_file, {"PSP_GAME/USRDIR/901_menu.bin": prx,
                       "PSP_GAME/SYSDIR/BOOT.BIN": prx,
                       "PSP_GAME/USRDIR/game.dat": build_plain_mips_elf(),
                       "PSP_GAME/USRDIR/truncated.bin": prx[:0x14],
                       "PSP_GAME/USRDIR/bad_tables.bin": build_plain_mips_elf(0xFFA0),
                       "PSP_GAME/USRDIR/data/notes.bin": b"not code"},
            disc_id="TEST00084",
        )
        candidates = nk_cli._discover_iso_module_candidates(iso_file, "EBOOT.BIN")
        self.assertEqual([(c["name"], c["kind"]) for c in candidates],
                         [("901_menu.bin", "plain-elf")])
        declared, deferred, staged = nk_cli._declare_experimental_disc_modules(
            iso_file, user_root, "TEST00084", candidates, selected, {},
        )
        self.assertEqual(deferred, [])
        self.assertEqual(declared, [{
            "name": "901_menu.bin", "required": True, "role": "guest-prx",
            "placement": "runtime", "guest_path": "disc0:/PSP_GAME/USRDIR/901_menu.bin",
        }])
        self.assertEqual((staged / "901_menu.bin").read_bytes(), prx)

    def test_prx_packed_in_a_disc_file_is_declared_by_its_sector_extent(self) -> None:
        # A game can pack its modules in one archive and load each by the sectors it
        # occupies, so no file name leads to it. The section table's end is the size the
        # game passes, and that extent is the guest path the runtime matches. What only
        # looks like a module stays file data, and the firmware updater is never opened.
        section_string_table = struct.pack("<10I", 0, 3, 0, 0, 84, 4, 0, 0, 1, 0)
        elf = bytearray(self._build_prx_without_module_info())
        struct.pack_into("<I", elf, 0x20, len(elf))
        struct.pack_into("<HH", elf, 0x2E, len(section_string_table), 1)
        prx = bytes(elf) + section_string_table

        def pad(data: bytes, sectors: int = 1) -> bytes:
            return data + bytes(sectors * 2048 - len(data))

        no_sections = pad(build_plain_mips_elf(0xFFA0))
        unaligned = pad(bytes(16) + prx)
        beyond_the_file = bytearray(elf)
        struct.pack_into("<I", beyond_the_file, 0x20, 0x100000)
        archive = (bytes(2048) + pad(prx, 2) + no_sections + unaligned +
                   pad(bytes(beyond_the_file)) + b"\x7fELF" + bytes(2044) + pad(prx))
        work = self.temp_dir / "embedded-module"
        user_root = work / "user"
        user_root.mkdir(parents=True)
        selected = work / "selected.elf"
        selected.write_bytes(build_plain_mips_elf())
        iso_file = work / "embedded.iso"
        create_test_iso_with_module_tree(
            iso_file, {"PSP_GAME/USRDIR/package.rdp": archive,
                       "PSP_GAME/SYSDIR/UPDATE/DATA.BIN": archive},
            disc_id="TEST00085",
        )
        archive_lba = next(entry.lba for _directory, entry
                           in iso_inspect.walk_disc_module_entries(iso_file)
                           if entry.name == "package.rdp")
        extents = [
            f"sce_lbn0x{archive_lba + sector:x}_size0x{len(prx):x}" for sector in (1, 7)
        ]
        candidates = nk_cli._discover_iso_module_candidates(iso_file, "EBOOT.BIN")
        self.assertEqual([(c["name"], c["kind"]) for c in candidates],
                         [(f"{extent}.prx", "plain-elf") for extent in extents])
        declared, deferred, staged = nk_cli._declare_experimental_disc_modules(
            iso_file, user_root, "TEST00085", candidates, selected, {},
        )
        self.assertEqual(deferred, [])
        self.assertEqual([module["guest_path"] for module in declared],
                         [f"disc0:/{extent}" for extent in extents])
        self.assertEqual({module["placement"] for module in declared}, {"runtime"})
        self.assertEqual((staged / f"{extents[0]}.prx").read_bytes(), prx)
        copied = nk_cli._copy_optional_modules(
            iso_file, {"modules": declared}, work / "package", None,
        )
        self.assertEqual((copied / f"{extents[1]}.prx").read_bytes(), prx)

    def test_code_overlay_is_found_by_its_magic_and_declared_runtime_placed(self) -> None:
        # A game's own code overlays (MWo3) are found by their magic whatever their file
        # name, staged as they are, and declared runtime-placed so the build translates
        # them; a file with the magic and a malformed header is a named candidate.
        load = 0x08C00000
        text = struct.pack("<4I", 0x27BDFFF0, 0x03E00008, 0x27BD0010, 0)
        data = bytes(8)
        data_lo = load + 0x40 + len(text)
        overlay = (struct.pack("<4s7I", b"MWo3", 1, load, len(text), len(data), 0,
                               data_lo, data_lo) + bytes(0x20) + text + data)
        work = self.temp_dir / "code-overlay"
        user_root = work / "user"
        user_root.mkdir(parents=True)
        selected = work / "selected.elf"
        selected.write_bytes(build_plain_mips_elf())
        iso_file = work / "overlay.iso"
        create_test_iso_with_module_tree(
            iso_file, {"PSP_GAME/USRDIR/Title.ovl": overlay,
                       "PSP_GAME/USRDIR/data/notes.bin": b"not code"},
            disc_id="TEST00083",
        )
        candidates = nk_cli._discover_iso_module_candidates(iso_file, "EBOOT.BIN")
        self.assertEqual([(c["name"], c["kind"]) for c in candidates],
                         [("Title.ovl", "code-overlay")])
        declared, deferred, staged = nk_cli._declare_experimental_disc_modules(
            iso_file, user_root, "TEST00083", candidates, selected, {},
        )
        self.assertEqual(deferred, [])
        self.assertEqual(declared, [{
            "name": "Title.ovl", "required": True, "role": "guest-prx",
            "placement": "runtime", "guest_path": "disc0:/PSP_GAME/USRDIR/Title.ovl",
        }])
        self.assertEqual((staged / "Title.ovl").read_bytes(), overlay)

        bad_iso = work / "bad-overlay.iso"
        create_test_iso_with_module_tree(
            bad_iso, {"PSP_GAME/USRDIR/Bad.ovl": overlay[:-4]}, disc_id="TEST00084")
        candidates = nk_cli._discover_iso_module_candidates(bad_iso, "EBOOT.BIN")
        self.assertEqual(candidates[0]["kind"], "unsupported")
        self.assertIn("code-overlay header is malformed", candidates[0]["reason"])

    def test_non_congruent_valid_module_is_accepted(self) -> None:
        # p_offset (84) and p_vaddr (0) differ modulo p_align 0x10. The loader copies segment
        # bytes from the file offset, so this valid PSP module is accepted: no congruence rule.
        offset_module = bytearray(build_plain_mips_elf(0xFFA0, vaddr=0, entry=0xFFFFFFFF))
        struct.pack_into("<I", offset_module, 52 + 28, 0x10)
        iso_file = self.temp_dir / "non-congruent-module.iso"
        create_test_iso_with_module_tree(
            iso_file, {"PSP_GAME/USRDIR/module/offset.prx": bytes(offset_module)}
        )
        candidates = nk_cli._discover_iso_module_candidates(iso_file, "EBOOT.BIN")
        self.assertEqual(
            [(candidate["name"], candidate["kind"]) for candidate in candidates],
            [("offset.prx", "plain-elf")],
        )

    @staticmethod
    def _layout_case_images() -> list[tuple[str, bytes, bool, str | None]]:
        """(case name, image bytes, module flag, expected rule or None when usable)."""
        def patched(data: bytes, edits: list[tuple[str, int, int]]) -> bytes:
            image = bytearray(data)
            for fmt, offset, value in edits:
                struct.pack_into(fmt, image, offset, value)
            return bytes(image)

        executable = build_plain_mips_elf(2)
        prx = build_plain_mips_elf(0xFFA0, vaddr=0, entry=0xFFFFFFFF)

        def with_null_program_headers(image: bytes, total: int) -> bytes:
            # The first PT_LOAD stays; PT_NULL entries follow it, then the segment bytes.
            out = bytearray(image[:52])
            struct.pack_into("<H", out, 44, total)
            data_offset = 52 + 32 * total
            load = bytearray(image[52:84])
            struct.pack_into("<I", load, 4, data_offset)
            out += load + bytes(32 * (total - 1)) + image[84:]
            return bytes(out)

        return [
            ("many-program-headers", with_null_program_headers(executable, 182), False, None),
            ("fixed-overlay-type", patched(executable, [("<H", 16, 0xFFA8)]), True,
             "header-fields"),
            ("usable-executable", executable, False, None),
            ("usable-prx-module", prx, True, None),
            ("non-congruent-prx-module", patched(prx, [("<I", 52 + 28, 0x10)]), True, None),
            ("non-congruent-executable", patched(executable, [("<I", 52 + 28, 0x10)]), False, None),
            ("header-truncated", executable[:40], False, "header-truncated"),
            ("header-magic", patched(executable, [("<B", 0, 0)]), False, "header-magic"),
            ("header-fields", patched(executable, [("<H", 18, 3)]), False, "header-fields"),
            ("program-table-fields", patched(executable, [("<H", 42, 24)]), False,
             "program-table-fields"),
            ("program-table-empty", patched(executable, [("<H", 44, 0)]), False,
             "program-table-fields"),
            ("program-table-bounds", patched(executable, [("<I", 28, 0xFFFF0)]), False,
             "program-table-bounds"),
            # More load segments than the old 128-entry table, in descending address order:
            # both rules sort them before the overlap scan.
            ("many-load-segments-out-of-order", build_mips_elf_with_load_segments(
                [(0x08900000 - 0x100 * index, 0x100) for index in range(300)]), False, None),
            ("many-load-segments-overlap", build_mips_elf_with_load_segments(
                [(0x08900000 - 0x100 * index, 0x100) for index in range(300)]
                + [(0x088F0080, 0x100)]), False, "segment-overlap"),
            ("section-table-bounds", patched(executable, [
                ("<H", 46, 40), ("<H", 48, 1), ("<I", 32, 0x1000)]), False,
             "section-table-bounds"),
            ("section-table-stray-offset", patched(executable, [("<I", 32, 0x40)]), False,
             "section-table-bounds"),
            ("segment-file-range", patched(executable, [("<I", 52 + 16, 0x100)]), False,
             "segment-file-range"),
            ("segment-memory-below-file", patched(executable, [("<I", 52 + 20, 2)]), False,
             "segment-memory-below-file"),
            ("segment-memory-range", patched(executable, [("<I", 52 + 8, 0xFFFFFFF0),
                                                          ("<I", 52 + 20, 0x100)]), False,
             "segment-memory-range"),
            ("segment-alignment", patched(executable, [("<I", 52 + 28, 3)]), False,
             "segment-alignment"),
            ("no-load-segment", patched(executable, [("<I", 52, 4)]), False, "no-load-segment"),
            ("segment-overlap", build_overlapping_mips_elf(
                vaddr1=0, memsz1=0x2000, vaddr2=0x1000, memsz2=0x2000), True, "segment-overlap"),
            ("entry-not-executable", build_plain_mips_elf(2, p_flags=4), False,
             "entry-not-executable"),
            # The PSP starts an executable at its module_start export (uofw StartModule).
            ("module-start-entry", build_module_start_elf(entry=0), False, None),
            ("module-bootstart-entry", build_module_start_elf(
                entry=0, exports=((MODULE_BOOTSTART_NID, 0x4),)), False, None),
            ("module-start-not-executable", build_module_start_elf(entry=0, p_flags=6), False,
             "entry-not-executable"),
            ("null-entry-without-start-export", build_module_start_elf(
                entry=0, exports=((0xCEE8593C, 0x8),)), False, "entry-not-executable"),
            ("module-info-range", build_module_start_elf(entry=0, info_offset=0x1000), False,
             "module-info-range"),
            ("module-exports-range", build_module_start_elf(
                entry=0, exports=(), ent_end_extra=0x1000), False, "module-exports-range"),
            ("prx-without-code-segment", build_plain_mips_elf(
                0xFFA0, vaddr=0, entry=0xFFFFFFFF, p_flags=4), True, "no-code-segment"),
            ("prx-code-without-file-bytes", build_plain_mips_elf(
                0xFFA0, vaddr=0, entry=0xFFFFFFFF, filesz=0), True, "no-code-segment"),
        ]

    def _run_native_elf_layout(self, path: Path, module: bool) -> str | None:
        result = subprocess.run(
            [str(self.exe_path), "elf_layout", str(path), "1" if module else "0"],
            capture_output=True, text=True, encoding="utf-8", errors="strict",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        for line in result.stdout.splitlines():
            if line.startswith("ELF_LAYOUT:"):
                rule = line.split(":", 1)[1]
                return None if rule == "USABLE" else rule
        self.fail(f"the native layout check printed no verdict: {result.stdout!r}")

    def test_elf_layout_rule_is_shared_by_python_and_c(self) -> None:
        # The same synthetic headers through the Python mirror and the C definition in
        # src/core/nk_iso.c: both must accept the same inputs and refuse each one by the
        # same named rule.
        for name, image, module, expected in self._layout_case_images():
            with self.subTest(case=name):
                path = self.temp_dir / f"layout-{name}.elf"
                path.write_bytes(image)
                with path.open("rb") as stream:
                    python_rule = iso_inspect._elf32_mips_iso_violation(
                        stream, len(image), 0, len(image), module=module
                    )
                self.assertEqual(python_rule, expected, "python layout rule")
                self.assertEqual(self._run_native_elf_layout(path, module), expected,
                                 "native layout rule")

    def test_start_entry_is_shared_by_python_and_c(self) -> None:
        # module_start wins over e_entry, then module_bootstart, then e_entry; a NID listed
        # twice resolves to its last entry; an image with no module info (p_paddr ==
        # p_vaddr) starts at e_entry.
        cases = [
            ("module-start-over-e-entry", build_module_start_elf(entry=0x08804008),
             0x08804004, None),
            ("module-start-over-bootstart", build_module_start_elf(entry=0, exports=(
                (MODULE_BOOTSTART_NID, 0x8), (MODULE_START_NID, 0x4))), 0x08804004, None),
            # A table that lists module_start twice (The BIGS and The BIGS 2 list a game
            # routine first and their crt0 entry last): uofw's module manager assigns every
            # match, so the last one is entered (sr_prx_start_entry names the revision).
            ("module-start-listed-twice", build_module_start_elf(entry=0x08804008, exports=(
                (MODULE_START_NID, 0x4), (MODULE_START_NID, 0x8))), 0x08804008, None),
            ("bootstart-listed-twice", build_module_start_elf(entry=0, exports=(
                (MODULE_BOOTSTART_NID, 0x4), (MODULE_BOOTSTART_NID, 0xC))), 0x0880400C, None),
            ("bootstart-without-start", build_module_start_elf(
                entry=0x08804008, exports=((MODULE_BOOTSTART_NID, 0xC),)), 0x0880400C, None),
            ("e-entry-without-exports", build_module_start_elf(
                entry=0x08804008, exports=((0xCEE8593C, 0x8),)), 0x08804008, None),
            ("no-module-info", build_plain_mips_elf(2), 0x08800000, None),
            ("module-info-range", build_module_start_elf(entry=0, info_offset=0x1000),
             0, "module-info-range"),
        ]
        for name, image, expected_entry, expected_rule in cases:
            with self.subTest(case=name):
                path = self.temp_dir / f"start-{name}.elf"
                path.write_bytes(image)
                with path.open("rb") as stream:
                    def read(offset: int, count: int) -> bytes:
                        stream.seek(offset)
                        return stream.read(count)
                    entry, rule = iso_inspect._elf32_psp_start_entry(read, len(image))
                self.assertEqual((entry, rule), (expected_entry, expected_rule), "python")
                result = subprocess.run(
                    [str(self.exe_path), "start_entry", str(path)],
                    capture_output=True, text=True, encoding="utf-8", errors="strict",
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                native = next(line for line in result.stdout.splitlines()
                              if line.startswith("START_ENTRY:"))
                self.assertEqual(
                    native, f"START_ENTRY:{expected_rule or 'OK'}:{expected_entry:08x}", "native")

    def test_usrdir_prx_discovery_checklist_and_extraction_share_module_rule(self) -> None:
        iso_file = self.temp_dir / "usrdir-plain-prx.iso"
        member = "PSP_GAME/USRDIR/module/libfont.prx"
        prx = build_plain_mips_elf(0xFFA0, vaddr=0, entry=0xFFFFFFFF)
        create_test_iso_with_module_tree(iso_file, {member: prx})

        candidates = list_disc_module_candidates(iso_file)
        self.assertEqual([candidate["members"][0] for candidate in candidates], [
            tuple(member.split("/")),
        ])
        package_candidates = nk_cli._discover_iso_module_candidates(
            iso_file, "EBOOT.BIN"
        )
        self.assertEqual([candidate["kind"] for candidate in package_candidates], [
            "plain-elf",
        ])

        preflight = inspect_compatibility_preflight(
            iso_file, metadata=inspect_iso(iso_file), runtime_root=self.temp_dir,
        )
        module_check = next(
            check for check in preflight["checks"] if check["code"] == "GUEST_MODULES"
        )
        self.assertEqual(module_check["status"], "OK", module_check["message"])
        self.assertIn("Guest modules: 1 of 1 ready.", module_check["message"])

        copied_modules = nk_cli._copy_optional_modules(
            iso_file,
            {"modules": [{
                "name": "manifest-libfont.prx",
                "guest_path": f"disc0:/{member}",
                "role": "guest-prx", "required": True,
            }]},
            self.temp_dir / "usrdir-prx-package", None,
        )
        self.assertIsNotNone(copied_modules)
        self.assertEqual((copied_modules / "manifest-libfont.prx").read_bytes(), prx)

        native_status, native_paths = self._run_native_module_walk(iso_file)
        self.assertEqual(native_status, 0)
        self.assertEqual(native_paths, [member])
    def test_module_discovery_fails_closed_on_malformed_directory_tail(self) -> None:
        iso_file = self.temp_dir / "malformed-module-directory.iso"
        member = "PSP_GAME/USRDIR/before.prx"
        directory_lbas = create_test_iso_with_module_tree(
            iso_file, {member: build_psp_container()}
        )
        image = bytearray(iso_file.read_bytes())
        record = _dir_record(b"before.prx;1", 1, 128, False)
        malformed_offset = 2 * len(_dir_record(b"\0", 1, 2048, True)) + len(record)
        image[directory_lbas[("PSP_GAME", "USRDIR")] * 2048 + malformed_offset] = 1
        iso_file.write_bytes(image)

        with self.assertRaisesRegex(IsoInspectionError, "DISC_MODULE_TREE_INVALID"):
            list_disc_module_candidates(iso_file)
        native_status, native_paths = self._run_native_module_walk(iso_file)
        self.assertEqual(native_status, 4)
        self.assertEqual(native_paths, [member])

    def test_module_discovery_lists_a_directory_with_thousands_of_entries(self) -> None:
        # Legal and within the byte bound: a disc in the library has one with 4,647 entries.
        iso_file = self.temp_dir / "large-module-directory.iso"
        member = "PSP_GAME/USRDIR/module/sdk.prx"
        tree = {f"PSP_GAME/USRDIR/data/map/f{index:04d}.bin": b"x" for index in range(4200)}
        tree[member] = build_psp_container()
        create_test_iso_with_module_tree(iso_file, tree)

        entries = iso_inspect.list_iso_directory(iso_file, ("PSP_GAME", "USRDIR", "data", "map"))
        self.assertEqual(len(entries), 4200)
        self.assertEqual(
            [candidate["members"][0] for candidate in list_disc_module_candidates(iso_file)],
            [tuple(member.split("/"))],
        )
        native_status, native_paths = self._run_native_module_walk(iso_file)
        self.assertEqual(native_status, 0)
        self.assertEqual(native_paths, [member])

    def test_module_discovery_rejects_dot_only_directory_name(self) -> None:
        iso_file = self.temp_dir / "dot-only-module-directory.iso"
        create_test_iso_with_module_tree(iso_file, {
            "PSP_GAME/USRDIR/./sdk.prx": build_psp_container(),
        })
        with self.assertRaisesRegex(IsoInspectionError, "DISC_MODULE_TREE_INVALID"):
            list_disc_module_candidates(iso_file)
        native_status, native_paths = self._run_native_module_walk(iso_file)
        self.assertEqual(native_status, 4)
        self.assertEqual(native_paths, [])

    def test_module_discovery_fails_closed_when_a_root_is_a_file(self) -> None:
        iso_file = self.temp_dir / "module-root-file.iso"
        create_test_iso_with_module_tree(iso_file, {
            "PSP_GAME/SYSDIR": b"not a directory",
            "PSP_GAME/USRDIR/module/sdk.prx": build_psp_container(),
        })
        with self.assertRaisesRegex(IsoInspectionError, "DISC_MODULE_TREE_INVALID"):
            list_disc_module_candidates(iso_file)
        native_status, _native_paths = self._run_native_module_walk(iso_file)
        self.assertEqual(native_status, 4)

    def test_native_module_walk_reports_path_limit(self) -> None:
        iso_file = self.temp_dir / "module-path-limit.iso"
        directory_name = "d" * 50
        member = f"PSP_GAME/USRDIR/{directory_name}/nested.prx"
        create_test_iso_with_module_tree(iso_file, {member: build_psp_container()})

        # The production depth and ISO record bounds put legal paths below 1200
        # bytes. Compile this C harness with a smaller cap to execute the same
        # fail-closed branch without changing the production bound.
        native_status, native_paths = self._run_native_module_walk(
            iso_file, type(self).path_limit_exe
        )
        self.assertEqual(native_status, 3)
        self.assertEqual(native_paths, [])

    def test_copy_optional_modules_falls_back_after_stale_manifest_path(self) -> None:
        iso_file = self.temp_dir / "stale-module-path.iso"
        member = "PSP_GAME/USRDIR/module/sdk.prx"
        module_bytes = build_plain_mips_elf(0xFFA0)
        create_test_iso_with_module_tree(iso_file, {member: module_bytes})
        output = nk_cli._copy_optional_modules(
            iso_file,
            {"modules": [{
                "name": "manifest-sdk.prx",
                "role": "guest-prx",
                "required": True,
                "guest_path": "disc0:/PSP_GAME/SYSDIR/module/sdk.prx",
            }]},
            self.temp_dir / "stale-module-package", None,
        )
        self.assertIsNotNone(output)
        self.assertEqual((output / "manifest-sdk.prx").read_bytes(), module_bytes)

    def test_copy_optional_modules_keeps_only_the_modules_the_manifest_requires(self) -> None:
        # The cache's modules/ persists between builds and the package ships every file in
        # it, so a module an earlier manifest required and a leftover partial copy are
        # removed before this build's copies (audit 2026-11-17).
        iso_file = self.temp_dir / "current-modules.iso"
        module_bytes = build_plain_mips_elf(0xFFA0)
        create_test_iso_with_modules(
            iso_file, build_plain_mips_elf(),
            sysdir_modules={"libfont.prx": module_bytes},
            usrdir_modules={},
        )
        cache = self.temp_dir / "persistent-module-package"
        (cache / "modules").mkdir(parents=True)
        (cache / "modules" / "earlier.prx").write_bytes(build_plain_mips_elf(0xFFB0))
        (cache / "modules" / "libfont.prx.tmp").write_bytes(b"partial")
        (cache / "modules" / "keep-dir").mkdir()
        manifest = {"modules": [{"name": "libfont.prx", "role": "guest-prx", "required": True}]}
        output = nk_cli._copy_optional_modules(iso_file, manifest, cache, None)
        self.assertIsNotNone(output)
        assert output is not None
        self.assertEqual(
            sorted(path.name for path in output.iterdir() if path.is_file()), ["libfont.prx"]
        )
        self.assertEqual((output / "libfont.prx").read_bytes(), module_bytes)

    def test_a_disc_module_whose_copy_fails_leaves_no_partial_copy(self) -> None:
        iso_file = self.temp_dir / "failed-module-copy.iso"
        create_test_iso_with_modules(
            iso_file, build_plain_mips_elf(),
            sysdir_modules={"libfont.prx": build_plain_mips_elf(0xFFA0)},
            usrdir_modules={},
        )
        cache = self.temp_dir / "failed-module-package"
        manifest = {"modules": [{"name": "libfont.prx", "role": "guest-prx", "required": True}]}
        # The copy was already opened for writing when the read fails (a disk error); the
        # member is skipped, and the module is then refused as unavailable.
        with patch.object(nk_cli, "_read_iso_extent",
                          side_effect=OSError("synthetic read failure")):
            with self.assertRaisesRegex(nk_cli.PackageBuildError, "is unavailable"):
                nk_cli._copy_optional_modules(iso_file, manifest, cache, None)
        self.assertEqual(sorted(path.name for path in (cache / "modules").iterdir()), [])
        # A refusal of the copied bytes removes them too.
        truncated_iso = self.temp_dir / "refused-module.iso"
        create_test_iso_with_modules(
            truncated_iso, build_plain_mips_elf(),
            sysdir_modules={"libfont.prx": build_plain_mips_elf(0xFFA0)[:60]},
            usrdir_modules={},
        )
        refused_cache = self.temp_dir / "refused-module-package"
        with self.assertRaisesRegex(nk_cli.PackageBuildError, "not a usable plain MIPS ELF32"):
            nk_cli._copy_optional_modules(truncated_iso, manifest, refused_cache, None)
        self.assertEqual(sorted(path.name for path in (refused_cache / "modules").iterdir()), [])

    def test_a_failed_executable_extraction_leaves_no_partial_copy(self) -> None:
        iso_file = self.temp_dir / "short-read.iso"
        create_test_iso_with_modules(
            iso_file, build_plain_mips_elf(), sysdir_modules={}, usrdir_modules={},
        )
        destination = self.temp_dir / "short-read-package" / "selected.elf"
        # The extent leaves the image part-way: the copy was already opened for writing.
        with patch.object(nk_cli, "_read_iso_extent",
                          side_effect=IsoInspectionError("synthetic out-of-bounds extent")):
            with self.assertRaisesRegex(nk_cli.PackageBuildError, "synthetic out-of-bounds"):
                nk_cli._extract_iso_executable(iso_file, "EBOOT.BIN", destination)
        self.assertEqual(sorted(path.name for path in destination.parent.iterdir()), [])

    def test_one_module_name_in_two_disc_folders_gets_path_qualified_host_names(self) -> None:
        # A module is identified by its disc path. Two folders holding one file name
        # (ASCII case-insensitively) each get a path-qualified host name, so their
        # copies never meet in the per-title folders, while a name no other file
        # carries keeps its own spelling.
        iso_file = self.temp_dir / "duplicate-module-name.iso"
        first = "PSP_GAME/USRDIR/module/shared.prx"
        second = "PSP_GAME/USRDIR/BIN/SHARED.PRX"
        unique = "PSP_GAME/USRDIR/module/unique.prx"
        create_test_iso_with_module_tree(iso_file, {
            first: build_psp_container(), second: build_psp_container(),
            unique: build_psp_container(),
        })
        first_host = iso_inspect.disc_module_qualified_name(tuple(first.split("/")))
        second_host = iso_inspect.disc_module_qualified_name(tuple(second.split("/")))
        self.assertRegex(first_host, r"^shared\.[0-9a-f]{8}\.prx$")
        self.assertRegex(second_host, r"^SHARED\.[0-9a-f]{8}\.PRX$")
        self.assertNotEqual(first_host.casefold(), second_host.casefold())
        # The PSP matches paths without regard to case; so does the key.
        self.assertEqual(
            iso_inspect.disc_module_qualified_name(tuple(first.upper().split("/"))).casefold(),
            first_host.casefold(),
        )

        expected = {tuple(first.split("/")): first_host,
                    tuple(second.split("/")): second_host,
                    tuple(unique.split("/")): "unique.prx"}
        candidates = list_disc_module_candidates(iso_file)
        self.assertEqual(
            {candidate["members"][0]: candidate["name"] for candidate in candidates},
            expected,
        )
        package_candidates = nk_cli._discover_iso_module_candidates(iso_file, "EBOOT.BIN")
        self.assertEqual(
            {nk_cli._candidate_member_path(candidate): candidate["name"]
             for candidate in package_candidates},
            {"/".join(member): name for member, name in expected.items()},
        )
        report = inspect_compatibility_preflight(
            iso_file, metadata=inspect_iso(iso_file), runtime_root=self.temp_dir,
        )
        module_check = next(
            check for check in report["checks"] if check["code"] == "GUEST_MODULES"
        )
        self.assertNotIn("DUPLICATE_DISC_MODULE_BASENAME", module_check["message"])
        self.assertIn("0 of 3 ready", module_check["message"])

    def test_path_qualified_host_name_equal_to_a_real_name_is_refused(self) -> None:
        # The copies would share one host file: a named refusal, never a silent pick.
        first = ("PSP_GAME", "USRDIR", "A", "lib.prx")
        clash = iso_inspect.disc_module_qualified_name(first)
        iso_file = self.temp_dir / "module-host-name-clash.iso"
        create_test_iso_with_module_tree(iso_file, {
            "/".join(first): build_psp_container(),
            "PSP_GAME/USRDIR/B/lib.prx": build_psp_container(),
            f"PSP_GAME/USRDIR/C/{clash}": build_psp_container(),
        })
        with self.assertRaisesRegex(
            IsoInspectionError, f"DUPLICATE_DISC_MODULE_BASENAME.*{clash}.*A/lib.prx"
        ):
            list_disc_module_candidates(iso_file)
        with self.assertRaisesRegex(
            nk_cli.PackageBuildError, f"DUPLICATE_DISC_MODULE_BASENAME.*{clash}"
        ):
            nk_cli._discover_iso_module_candidates(iso_file, "EBOOT.BIN")

    def test_module_walk_lists_non_ascii_identifiers(self) -> None:
        # Discs carry Shift-JIS file names; the PSP's file system takes the bytes as
        # they are. The walk lists them (the native walker always has) and never
        # turns one into a module candidate or a host name.
        iso_file = self.temp_dir / "non-ascii-identifier.iso"
        module = "PSP_GAME/USRDIR/module/sdk.prx"
        create_test_iso_with_module_tree(iso_file, {
            module: build_psp_container(),
            "PSP_GAME/USRDIR/demo/fra/\x95\xe2\x91\xab.txt": b"text",
            "PSP_GAME/USRDIR/demo/\x8f\xda/inner.prx": build_psp_container(),
        })
        walked = {
            "/".join(directory + (entry.name,)).encode("ascii", "surrogateescape")
            for directory, entry in iso_inspect.walk_disc_module_entries(iso_file)
        }
        self.assertIn(b"PSP_GAME/USRDIR/demo/fra/\x95\xe2\x91\xab.txt", walked)
        self.assertEqual(
            {candidate["members"][0] for candidate in list_disc_module_candidates(iso_file)},
            {tuple(module.split("/")), ("PSP_GAME", "USRDIR", "demo", "\udc8f\udcda",
                                        "inner.prx")},
        )
        package = {
            nk_cli._candidate_member_path(candidate): candidate
            for candidate in nk_cli._discover_iso_module_candidates(iso_file, "EBOOT.BIN")
        }
        self.assertEqual(package[module]["kind"], "encrypted-prx")
        inner = package["PSP_GAME/USRDIR/demo/\udc8f\udcda/inner.prx"]
        self.assertEqual(inner["kind"], "unsupported")
        self.assertIn("not ASCII", inner["reason"])
        native_status, native_paths = self._run_native_module_walk(iso_file)
        self.assertEqual(native_status, 0)
        self.assertIn(module, native_paths)

    def test_module_walk_lists_a_directory_of_thousands_of_entries(self) -> None:
        # A speech folder of several thousand files is bounded by the directory's
        # byte size (512 KiB), not by a separate entry count.
        iso_file = self.temp_dir / "large-module-directory.iso"
        files = {
            f"PSP_GAME/USRDIR/DATA/SPEECH/line_{index:05}.at3": b"" for index in range(4200)
        }
        files["PSP_GAME/USRDIR/module/sdk.prx"] = build_psp_container()
        create_test_iso_with_module_tree(iso_file, files)
        listed = iso_inspect.list_iso_directory(
            iso_file, ("PSP_GAME", "USRDIR", "DATA", "SPEECH")
        )
        self.assertEqual(len(listed), 4200)
        self.assertEqual(
            [candidate["name"] for candidate in list_disc_module_candidates(iso_file)],
            ["sdk.prx"],
        )

    def test_nonexperimental_bringup_keeps_module_boundary_detail_and_issue(self) -> None:
        failure, issues, detail = nk_cli._bringup_import_failure(
            nk_cli.PackageBuildError(
                "DUPLICATE_DISC_MODULE_BASENAME: shared.prx occurs twice"
            ),
            {"EXECUTABLE": {"issue_numbers": [308]}},
        )
        self.assertEqual(failure, "GUEST_MODULE_DISCOVERY_FAILED")
        self.assertEqual(issues, [308])
        self.assertEqual(
            detail, "DUPLICATE_DISC_MODULE_BASENAME: shared.prx occurs twice"
        )

    def test_module_discovery_accepts_more_than_32_candidates(self) -> None:
        iso_file = self.temp_dir / "module-candidate-many.iso"
        modules = {
            f"PSP_GAME/USRDIR/MODULES/module-{index:02}.prx": build_psp_container()
            for index in range(40)
        }
        create_test_iso_with_module_tree(iso_file, modules)
        self.assertEqual(len(list_disc_module_candidates(iso_file)), 40)
        self.assertEqual(
            len(nk_cli._discover_iso_module_candidates(iso_file, "EBOOT.BIN")), 40
        )

    def test_module_discovery_fails_closed_above_documented_candidate_cap(self) -> None:
        iso_file = self.temp_dir / "module-candidate-cap.iso"
        modules = {
            f"PSP_GAME/USRDIR/module-{index:03}.prx": build_psp_container()
            for index in range(257)
        }
        create_test_iso_with_module_tree(iso_file, modules)
        with self.assertRaisesRegex(
            IsoInspectionError,
            "DISC_MODULE_CANDIDATE_LIMIT.*256.*larger module sets aren't supported yet",
        ):
            list_disc_module_candidates(iso_file)
        with self.assertRaisesRegex(
            nk_cli.PackageBuildError,
            "DISC_MODULE_CANDIDATE_LIMIT.*256.*larger module sets aren't supported yet",
        ):
            nk_cli._discover_iso_module_candidates(iso_file, "EBOOT.BIN")

    def test_module_discovery_excludes_kmodule_but_keeps_usrdir_module(self) -> None:
        iso_file = self.temp_dir / "kernel-module-exclusion.iso"
        user_module = "PSP_GAME/USRDIR/module/user.prx"
        kernel_module = "PSP_GAME/USRDIR/KMODULE/kernel.prx"
        create_test_iso_with_module_tree(iso_file, {
            user_module: build_psp_container(),
            kernel_module: build_psp_container(),
        })
        candidates = list_disc_module_candidates(iso_file)
        self.assertEqual(
            {candidate["members"][0] for candidate in candidates},
            {tuple(user_module.split("/"))},
        )
        package_candidates = nk_cli._discover_iso_module_candidates(
            iso_file, "EBOOT.BIN"
        )
        self.assertEqual(
            {candidate["directory"] + (candidate["entry"].name,)
             for candidate in package_candidates},
            {tuple(user_module.split("/"))},
        )
        native_status, native_paths = self._run_native_module_walk(iso_file)
        self.assertEqual(native_status, 0)
        self.assertEqual(native_paths, [user_module])

    def test_synthetic_iso_parity(self) -> None:
        """Verify Python and Native C inspector match on synthetic title."""
        iso_file = self.temp_dir / "synthetic.iso"
        create_test_iso(iso_file, disc_id="TEST00001", title="Synthetic Test Title", volume_id="SYNTH_VOL")

        # Python inspection
        py_meta = inspect_iso(iso_file)
        self.assertEqual(py_meta.disc_id, "TEST00001")
        self.assertTrue(py_meta.is_supported)
        self.assertIsNotNone(py_meta.matched_profile)

        # Native C inspection
        c_meta = self._run_native_inspect(iso_file)
        self.assertEqual(c_meta.get("RESULT"), "OK")
        self.assertEqual(c_meta.get("DISC_ID"), py_meta.disc_id)
        self.assertEqual(c_meta.get("TITLE"), py_meta.title)
        self.assertEqual(c_meta.get("VERSION"), py_meta.version)
        self.assertEqual(c_meta.get("SUPPORTED"), "1")
        self.assertEqual(c_meta.get("PARAM_SFO_PARSED"), "1")
        self.assertEqual(c_meta.get("MATCHED_ID"), py_meta.matched_profile.id)

    def test_game_sfo_wins_over_update_sfo(self) -> None:
        iso_file = self.temp_dir / "update-decoy.iso"
        update_sfo = build_param_sfo("MSTKUPDATE", "PSP Update ver 6.20")
        create_test_iso(
            iso_file,
            disc_id="TEST00001",
            title="Synthetic Test Title",
            volume_id="VOLUME_ID",
            update_sfo_bytes=update_sfo,
        )

        py_meta = inspect_iso(iso_file)
        c_meta = self._run_native_inspect(iso_file)
        self.assertEqual(py_meta.disc_id, "TEST00001")
        self.assertEqual(py_meta.title, "Synthetic Test Title")
        self.assertEqual(c_meta.get("DISC_ID"), py_meta.disc_id)
        self.assertEqual(c_meta.get("TITLE"), py_meta.title)
        self.assertEqual(c_meta.get("SUPPORTED"), "1")

    def test_psn_layout_uses_reachable_game_sfo(self) -> None:
        iso_file = self.temp_dir / "psn-layout.iso"
        update_sfo = build_param_sfo("MSTKUPDATE", "PSP Update ver 2.71")
        create_test_iso(
            iso_file,
            disc_id="TEST80003",
            title="Synthetic PSN Fixture",
            volume_id="TYPE_0",
            update_sfo_bytes=update_sfo,
        )

        py_meta = inspect_iso(iso_file)
        c_meta = self._run_native_inspect(iso_file)
        self.assertEqual(py_meta.disc_id, "TEST80003")
        self.assertEqual(py_meta.title, "Synthetic PSN Fixture")
        self.assertEqual(c_meta.get("DISC_ID"), "TEST80003")
        self.assertEqual(c_meta.get("TITLE"), "Synthetic PSN Fixture")

    def test_sfo_disc_id_and_utf8_title_override_volume_fallback(self) -> None:
        iso_file = self.temp_dir / "utf8-identity.iso"
        update_sfo = build_param_sfo("MSTKUPDATE", "PSP Update ver 6.20")
        create_test_iso(
            iso_file,
            disc_id="TEST00002",
            title="Café® ™",
            volume_id="TEST00001",
            update_sfo_bytes=update_sfo,
        )

        py_meta = inspect_iso(iso_file)
        c_meta = self._run_native_inspect(iso_file)
        self.assertEqual(py_meta.disc_id, "TEST00002")
        self.assertEqual(py_meta.title, "Café® ™")
        self.assertEqual(py_meta.matched_profile.id, "synthetic-title2-v1")
        self.assertEqual(c_meta.get("DISC_ID"), "TEST00002")
        self.assertEqual(c_meta.get("TITLE"), "Café® ™")
        self.assertEqual(c_meta.get("MATCHED_ID"), "synthetic-title2-v1")

    def test_catalog_match_uses_game_sfo_identity(self) -> None:
        from nk_core.title_registry import TitleRegistry
        from nk_core.types import TitleProfile

        iso_file = self.temp_dir / "catalog-identity.iso"
        update_sfo = build_param_sfo("MSTKUPDATE", "PSP Update ver 6.20")
        create_test_iso(
            iso_file,
            disc_id="TEST80003",
            title="Synthetic PSN Fixture",
            volume_id="TYPE_0",
            update_sfo_bytes=update_sfo,
        )
        registry = TitleRegistry(include_defaults=False)
        registry.register(TitleProfile(
            id="psn-fixture-v1",
            name="PSN Fixture",
            disc_ids=["TEST80003"],
            regions=["NA"],
        ))

        metadata = inspect_iso(iso_file, registry)
        self.assertTrue(metadata.is_supported)
        self.assertEqual(metadata.matched_profile.id, "psn-fixture-v1")

    def test_malformed_reachable_sfo_fails_closed(self) -> None:
        from nk_core.iso_inspect import IsoInspectionError

        iso_file = self.temp_dir / "malformed-sfo.iso"
        valid_sfo = build_param_sfo("TEST00001", "Malformed Fixture")
        create_test_iso(
            iso_file,
            volume_id="TEST00001",
            sfo_bytes=valid_sfo[:32],
        )

        with self.assertRaises(IsoInspectionError):
            inspect_iso(iso_file)
        native = self._run_native_inspect(iso_file)
        self.assertTrue(native.get("RESULT", "").startswith("ERROR"))

    def test_sfo_data_length_must_not_exceed_max_length(self) -> None:
        from nk_core.iso_inspect import IsoInspectionError

        iso_file = self.temp_dir / "bad-max-length.iso"
        malformed = bytearray(build_param_sfo("TEST00001", "Bad Max Length"))
        struct.pack_into("<I", malformed, 20 + 8, 4)
        create_test_iso(
            iso_file,
            volume_id="TEST00001",
            sfo_bytes=bytes(malformed),
        )

        with self.assertRaises(IsoInspectionError):
            inspect_iso(iso_file)
        native = self._run_native_inspect(iso_file)
        self.assertTrue(native.get("RESULT", "").startswith("ERROR"))

    def test_sfo_key_offset_outside_key_table_parity(self) -> None:
        from nk_core.iso_inspect import IsoInspectionError

        iso_file = self.temp_dir / "bad-key-offset.iso"
        malformed = bytearray(build_param_sfo("TEST00001", "Bad Key Offset"))
        struct.pack_into("<H", malformed, 20, 0xFFFF)
        create_test_iso(
            iso_file,
            volume_id="TEST00001",
            sfo_bytes=bytes(malformed),
        )

        with self.assertRaises(IsoInspectionError):
            inspect_iso(iso_file)
        native = self._run_native_inspect(iso_file)
        self.assertTrue(native.get("RESULT", "").startswith("ERROR"))

    def test_title_id_is_not_used_as_disc_id(self) -> None:
        iso_file = self.temp_dir / "title-id.iso"
        sfo = build_custom_param_sfo([
            ("TITLE_ID", 0x0204, b"UCUS99999\0"),
            ("DISC_ID", 0x0204, b"TEST00001\0"),
            ("TITLE", 0x0204, b"Identity Fixture\0"),
        ])
        create_test_iso(iso_file, volume_id="VOLUME_ID", sfo_bytes=sfo)

        py_meta = inspect_iso(iso_file)
        c_meta = self._run_native_inspect(iso_file)
        self.assertEqual(py_meta.disc_id, "TEST00001")
        self.assertEqual(c_meta.get("DISC_ID"), "TEST00001")
        self.assertEqual(c_meta.get("TITLE"), "Identity Fixture")

    def _assert_disc_sfo_ok(self, iso_file: Path, metadata) -> None:
        runtime_root = self.temp_dir / "runtime-root"
        runtime_root.mkdir(exist_ok=True)
        preflight = inspect_compatibility_preflight(
            iso_file, metadata=metadata, runtime_root=runtime_root
        )
        checks = {check["code"]: check for check in preflight["checks"]}
        self.assertEqual(checks["DISC_SFO"]["status"], "OK", checks["DISC_SFO"])

    def test_sfo_utf8_string_ends_at_its_first_nul_parity(self) -> None:
        # Format 0x0204 is a NUL-terminated string. A title written over a longer
        # one keeps the old tail (here a cut multi-byte character) after the NUL.
        iso_file = self.temp_dir / "sfo-after-nul.iso"
        sfo = build_custom_param_sfo([
            ("DISC_ID", 0x0204, b"TEST00001\0"),
            ("DISC_VERSION", 0x0204, b"1.00\0"),
            ("TITLE", 0x0204, b"Translated Title\0\x81\xaa\xe3\x81\x84\0"),
            ("PSP_SYSTEM_VER", 0x0204, b"4.05\0\x04"),
        ])
        create_test_iso(iso_file, volume_id="VOLUME_ID", sfo_bytes=sfo)

        py_meta = inspect_iso(iso_file)
        self.assertEqual(py_meta.title, "Translated Title")
        self.assertEqual(py_meta.param_sfo_facts["PSP_SYSTEM_VER"], "4.05")
        warnings = py_meta.container_metadata["format_warnings"]
        self.assertTrue(
            any("'TITLE'" in note and "81aae3818400" in note for note in warnings), warnings
        )
        self._assert_disc_sfo_ok(iso_file, py_meta)
        c_meta = self._run_native_inspect(iso_file)
        self.assertEqual(c_meta.get("RESULT"), "OK", c_meta)
        self.assertEqual(c_meta.get("TITLE"), "Translated Title")

    def test_sfo_title_that_is_not_utf8_is_shown_with_replacements_parity(self) -> None:
        # A single-byte code page dash, then cut and ill-formed sequences: each
        # ill-formed run becomes one U+FFFD and the raw bytes stay in the note.
        iso_file = self.temp_dir / "sfo-title-not-utf8.iso"
        raw_title = (
            b"Fixture \x96 Edition A\xe3\x81B\xf0\x9f\x98C\xed\xa0\x80D\xff"
        )
        sfo = build_custom_param_sfo([
            ("DISC_ID", 0x0204, b"TEST00001\0"),
            ("DISC_VERSION", 0x0204, b"1.00\0"),
            ("TITLE", 0x0204, raw_title + b"\0"),
        ])
        create_test_iso(iso_file, volume_id="VOLUME_ID", sfo_bytes=sfo)

        expected = raw_title.decode("utf-8", errors="replace")
        self.assertEqual(expected.count("�"), 7)
        py_meta = inspect_iso(iso_file)
        self.assertEqual(py_meta.disc_id, "TEST00001")
        self.assertEqual(py_meta.title, expected)
        warnings = py_meta.container_metadata["format_warnings"]
        self.assertTrue(
            any("'TITLE'" in note and raw_title.hex() in note for note in warnings), warnings
        )
        self._assert_disc_sfo_ok(iso_file, py_meta)
        c_meta = self._run_native_inspect(iso_file)
        self.assertEqual(c_meta.get("RESULT"), "OK", c_meta)
        self.assertEqual(c_meta.get("TITLE"), expected)

    def test_sfo_disc_id_and_version_are_never_repaired_parity(self) -> None:
        for key, value in (("DISC_VERSION", b"1.0\xff\0"), ("DISC_ID", b"TEST0000\xb9\0")):
            with self.subTest(key=key):
                iso_file = self.temp_dir / f"sfo-bad-{key.lower()}.iso"
                values = {"DISC_ID": b"TEST00001\0", "DISC_VERSION": b"1.00\0", key: value}
                sfo = build_custom_param_sfo([
                    ("DISC_ID", 0x0204, values["DISC_ID"]),
                    ("DISC_VERSION", 0x0204, values["DISC_VERSION"]),
                    ("TITLE", 0x0204, b"Exact Identity\0"),
                ])
                create_test_iso(iso_file, volume_id="VOLUME_ID", sfo_bytes=sfo)
                with self.assertRaisesRegex(IsoInspectionError, f"'{key}' is not valid UTF-8"):
                    inspect_iso(iso_file)
                c_meta = self._run_native_inspect(iso_file)
                self.assertTrue(c_meta.get("RESULT", "").startswith("ERROR"), c_meta)

    def test_sfo_value_past_the_end_is_skipped_unless_identity_parity(self) -> None:
        # A rewritten PARAM.SFO can be shorter than its own index table says; the
        # last value (USE_USB) then lies past the end of the file.
        entries = [
            ("DISC_ID", 0x0204, b"TEST00001\0"),
            ("DISC_VERSION", 0x0204, b"1.00\0"),
            ("TITLE", 0x0204, b"Short File\0"),
            ("USE_USB", 0x0404, struct.pack("<I", 0)),
        ]
        for index, key in ((3, "USE_USB"), (2, "TITLE")):
            with self.subTest(key=key):
                iso_file = self.temp_dir / f"sfo-{key.lower()}-past-end.iso"
                sfo = bytearray(build_custom_param_sfo(entries))
                struct.pack_into("<I", sfo, 20 + index * 16 + 12, 0x1000)
                create_test_iso(iso_file, volume_id="VOLUME_ID", sfo_bytes=bytes(sfo))
                c_meta = self._run_native_inspect(iso_file)
                if key == "TITLE":
                    with self.assertRaisesRegex(IsoInspectionError, "outside the data table"):
                        inspect_iso(iso_file)
                    self.assertTrue(c_meta.get("RESULT", "").startswith("ERROR"), c_meta)
                    continue
                py_meta = inspect_iso(iso_file)
                self.assertEqual((py_meta.disc_id, py_meta.title), ("TEST00001", "Short File"))
                warnings = py_meta.container_metadata["format_warnings"]
                self.assertTrue(any("'USE_USB'" in note for note in warnings), warnings)
                self._assert_disc_sfo_ok(iso_file, py_meta)
                self.assertEqual(c_meta.get("RESULT"), "OK", c_meta)
                self.assertEqual(c_meta.get("DISC_ID"), "TEST00001")
                self.assertEqual(c_meta.get("TITLE"), "Short File")

    def test_pvd_root_record_is_located_by_its_fixed_fields_parity(self) -> None:
        # ECMA-119 fixes the PVD root record at 34 bytes; a rebuilt image may copy
        # the length byte of a longer (system-use extended) record and no identifier.
        iso_file = self.temp_dir / "pvd-root-record.iso"
        create_test_iso(iso_file, disc_id="TEST00001", title="Root Record")
        image = bytearray(iso_file.read_bytes())
        pvd = 16 * 2048
        image[pvd + 156] = 48
        image[pvd + 188] = 0
        iso_file.write_bytes(bytes(image))

        py_meta = inspect_iso(iso_file)
        self.assertEqual((py_meta.disc_id, py_meta.title), ("TEST00001", "Root Record"))
        warnings = py_meta.container_metadata["format_warnings"]
        self.assertTrue(any("length byte 48" in note for note in warnings), warnings)
        self._assert_disc_sfo_ok(iso_file, py_meta)
        self.assertIsNotNone(iso_inspect.list_iso_directory(iso_file, ("PSP_GAME",)))
        c_meta = self._run_native_inspect(iso_file)
        self.assertEqual(c_meta.get("RESULT"), "OK", c_meta)
        self.assertEqual(c_meta.get("DISC_ID"), "TEST00001")

    def test_standard_disc_carries_no_format_warnings(self) -> None:
        iso_file = self.temp_dir / "standard.iso"
        create_test_iso(iso_file, disc_id="TEST00001", title="Standard Disc")
        self.assertNotIn("format_warnings", inspect_iso(iso_file).container_metadata)

    def test_quirky_disc_inspection_builds_a_valid_title_identity(self) -> None:
        # The inspector's container metadata goes straight into the package identity,
        # so a disc with a tolerated quirk must still yield a valid identity record.
        from nk_core import package_cache

        iso_file = self.temp_dir / "quirk-identity.iso"
        sfo = build_custom_param_sfo([
            ("DISC_ID", 0x0204, b"TEST00001\0"),
            ("DISC_VERSION", 0x0204, b"1.00\0"),
            ("TITLE", 0x0204, b"Fixture \x96 Edition\0"),
        ])
        create_test_iso(iso_file, volume_id="VOLUME_ID", sfo_bytes=sfo)
        meta = inspect_iso(iso_file)
        self.assertIn("format_warnings", meta.container_metadata)
        identity = package_cache.build_title_input_identity(
            manifest={"id": "synthetic-quirk-test", "schema_version": 1},
            executable_name="EBOOT.BIN",
            executable_sha256="2" * 64,
            modules=[],
            disc_id=meta.disc_id,
            region="OTHER",
            disc_version=meta.version,
            param_sfo=dict(meta.param_sfo_facts),
            container=dict(meta.container_metadata),
        )
        self.assertEqual(
            set(identity["container"]),
            {"format", "volume_id", "size_bytes", "pvd_sector", "sector_size"},
        )

    # A synthetic beta disc: a 12-character placeholder DISC_ID, outside the retail
    # format, and a small executable. Its key is pinned below, because a library
    # key that changed between versions would orphan the disc's packages.
    BETA_DISC_ID = "PROTO1234567"
    BETA_EXECUTABLE = b"synthetic beta executable\n"
    BETA_KEY = "XRQY66517"

    @staticmethod
    def _expected_identity(sfo: bytes, executable: bytes) -> tuple[str, str]:
        """The documented derivation, restated: SHA-256, then the key's 26^3 * 10^5 space."""
        digest = hashlib.sha256(
            b"nakagawa-non-retail-disc-key-v1\0"
            + struct.pack("<Q", len(sfo)) + sfo
            + struct.pack("<Q", len(executable)) + executable
        ).digest()
        value = int.from_bytes(digest[:8], "big") % (26 ** 3 * 10 ** 5)
        letters, digits = divmod(value, 100000)
        key = "X" + "".join(
            chr(65 + part) for part in (letters // 676, letters // 26 % 26, letters % 26)
        ) + f"{digits:05d}"
        return key, digest.hex()

    def test_non_retail_disc_is_keyed_by_its_contents_in_both_tiers(self) -> None:
        """A beta disc with a placeholder DISC_ID is accepted, labelled and keyed the same way
        by the Python and native inspectors; its own ID stays visible and the image is untouched."""
        iso_file = self.temp_dir / "beta.iso"
        create_test_iso_with_executables(
            iso_file, self.BETA_EXECUTABLE, disc_id=self.BETA_DISC_ID, title="Synthetic Beta",
        )
        image_before = hashlib.sha256(iso_file.read_bytes()).hexdigest()
        key, digest = self._expected_identity(
            build_param_sfo(self.BETA_DISC_ID, "Synthetic Beta"), self.BETA_EXECUTABLE,
        )
        self.assertEqual(key, self.BETA_KEY)

        py_meta = inspect_iso(iso_file)
        self.assertEqual(
            (py_meta.disc_id, py_meta.sfo_disc_id, py_meta.non_retail, py_meta.identity_sha256),
            (key, self.BETA_DISC_ID, True, digest),
        )
        self.assertTrue(iso_inspect.is_non_retail_key(py_meta.disc_id))
        self.assertIsNone(py_meta.matched_profile)
        self.assertEqual(py_meta.title, "Synthetic Beta")

        c_meta = self._run_native_inspect(iso_file)
        self.assertEqual(c_meta.get("RESULT"), "OK", c_meta)
        self.assertEqual(
            (c_meta.get("DISC_ID"), c_meta.get("SFO_DISC_ID"), c_meta.get("NON_RETAIL"),
             c_meta.get("IDENTITY_SHA256"), c_meta.get("NON_RETAIL_KEY_LABEL")),
            (key, self.BETA_DISC_ID, "1", digest, "1"),
        )
        self.assertEqual(c_meta.get("PARAM_SFO_PARSED"), "1")
        self.assertEqual(c_meta.get("SUPPORTED"), "0")
        self.assertEqual(hashlib.sha256(iso_file.read_bytes()).hexdigest(), image_before,
                         "inspecting a disc never changes it")

        # The same contents under another volume label keep the key; another executable
        # with the same PARAM.SFO gets another one.
        relabelled = self.temp_dir / "beta-copy.iso"
        create_test_iso_with_executables(
            relabelled, self.BETA_EXECUTABLE, disc_id=self.BETA_DISC_ID, title="Synthetic Beta",
        )
        data = bytearray(relabelled.read_bytes())
        data[16 * 2048 + 40 : 16 * 2048 + 48] = b"COPY_VOL"
        relabelled.write_bytes(bytes(data))
        self.assertEqual(inspect_iso(relabelled).disc_id, key)
        self.assertEqual(self._run_native_inspect(relabelled).get("DISC_ID"), key)
        rebuilt = self.temp_dir / "beta-rebuilt.iso"
        create_test_iso_with_executables(
            rebuilt, b"another beta executable\n", disc_id=self.BETA_DISC_ID,
            title="Synthetic Beta",
        )
        other = inspect_iso(rebuilt).disc_id
        self.assertNotEqual(other, key)
        self.assertEqual(self._run_native_inspect(rebuilt).get("DISC_ID"), other)

    def test_disc_id_classification_agrees_in_both_tiers(self) -> None:
        """Retail-format and synthetic ids keep their own key; any other printable id,
        nine characters or not, is a non-retail disc; anything else is refused."""
        cases = [
            ("ULUS-99999", "ULUS99999"),
            ("ucus99999", "UCUS99999"),
            ("TEST00001", "TEST00001"),
            ("ZZZZ99999", None),
            ("XRQY66517", None),
            ("PROTO1234567", None),
            ("BETA 01", None),
            ("Q" * 31, None),
            ("--_", "refused"),
            ("BETA\t0001", "refused"),
            ("BÊTA0001", "refused"),
            ("P" * 32, "refused"),
        ]
        executable = self.BETA_EXECUTABLE
        for index, (sfo_id, expected) in enumerate(cases):
            with self.subTest(sfo_id=sfo_id):
                iso_file = self.temp_dir / f"classify-{index}.iso"
                sfo = build_param_sfo(sfo_id, "Classified")
                create_test_iso_with_executables(
                    iso_file, executable, disc_id=sfo_id, title="Classified",
                )
                c_meta = self._run_native_inspect(iso_file)
                if expected == "refused":
                    with self.assertRaises(IsoInspectionError):
                        inspect_iso(iso_file)
                    self.assertTrue(c_meta.get("RESULT", "").startswith("ERROR:"), c_meta)
                    continue
                py_meta = inspect_iso(iso_file)
                key = expected if expected is not None else \
                    self._expected_identity(sfo, executable)[0]
                self.assertEqual(py_meta.disc_id, key)
                self.assertEqual(py_meta.non_retail, expected is None)
                self.assertEqual(py_meta.sfo_disc_id, sfo_id)
                self.assertEqual(c_meta.get("DISC_ID"), key, c_meta)
                self.assertEqual(c_meta.get("NON_RETAIL"), "0" if expected else "1")
                self.assertEqual(c_meta.get("SFO_DISC_ID"), sfo_id)

    def test_non_retail_disc_without_a_main_executable_is_refused_in_both_tiers(self) -> None:
        """Nothing else identifies a disc whose own id is not a retail id: no EBOOT.BIN, no key."""
        iso_file = self.temp_dir / "beta-no-executable.iso"
        create_test_iso(iso_file, disc_id=self.BETA_DISC_ID, title="Synthetic Beta")
        with self.assertRaises(IsoInspectionError) as caught:
            inspect_iso(iso_file)
        message = str(caught.exception)
        self.assertIn(f"'{self.BETA_DISC_ID}', which is not a retail disc ID", message)
        self.assertIn("PSP_GAME/SYSDIR/EBOOT.BIN is missing or unreadable", message)
        c_meta = self._run_native_inspect(iso_file)
        self.assertTrue(c_meta.get("RESULT", "").startswith("ERROR:"), c_meta)
        self.assertEqual(c_meta.get("MESSAGE"), message)

        empty = self.temp_dir / "beta-empty-executable.iso"
        create_test_iso_with_executables(
            empty, b"", disc_id=self.BETA_DISC_ID, title="Synthetic Beta",
        )
        with self.assertRaises(IsoInspectionError):
            inspect_iso(empty)
        self.assertEqual(self._run_native_inspect(empty).get("MESSAGE"), message)

    @staticmethod
    def _expected_boot_identity(sfo: bytes, eboot: bytes, boot: bytes) -> tuple[str, str]:
        """The v2 derivation for a disc that boots BOOT.BIN beside an empty or zero-filled
        EBOOT.BIN, restated: the v2 tag, then PARAM.SFO, EBOOT.BIN and BOOT.BIN, each after
        its length as a little-endian 64-bit integer."""
        digest = hashlib.sha256(
            b"nakagawa-non-retail-disc-key-v2\0"
            + struct.pack("<Q", len(sfo)) + sfo
            + struct.pack("<Q", len(eboot)) + eboot
            + struct.pack("<Q", len(boot)) + boot
        ).digest()
        value = int.from_bytes(digest[:8], "big") % (26 ** 3 * 10 ** 5)
        letters, digits = divmod(value, 100000)
        key = "X" + "".join(
            chr(65 + part) for part in (letters // 676, letters // 26 % 26, letters % 26)
        ) + f"{digits:05d}"
        return key, digest.hex()

    def _assert_identity_in_both_tiers(self, iso_file: Path, key: str, digest: str) -> None:
        py_meta = inspect_iso(iso_file)
        self.assertEqual(
            (py_meta.disc_id, py_meta.non_retail, py_meta.identity_sha256), (key, True, digest),
        )
        c_meta = self._run_native_inspect(iso_file)
        self.assertEqual(c_meta.get("RESULT"), "OK", c_meta)
        self.assertEqual(
            (c_meta.get("DISC_ID"), c_meta.get("NON_RETAIL"), c_meta.get("IDENTITY_SHA256")),
            (key, "1", digest),
        )

    def test_non_retail_key_covers_boot_bin_beside_a_placeholder_eboot(self) -> None:
        """Audit F-2281ebab: a prototype disc whose EBOOT.BIN is zero-filled boots its plain
        BOOT.BIN, so the key covers BOOT.BIN too (the v2 tag). Two such discs differing only
        in BOOT.BIN get two keys, not one shared package, title folder and saves."""
        sfo = build_param_sfo(self.BETA_DISC_ID, "Synthetic Beta")
        placeholder = bytes(2048)
        boots = (build_plain_mips_elf(), build_plain_mips_elf(vaddr=0x08804000))
        keys = []
        for index, boot in enumerate(boots):
            with self.subTest(boot=index):
                iso_file = self.temp_dir / f"proto-boot-{index}.iso"
                create_test_iso_with_executables(
                    iso_file, placeholder, boot, disc_id=self.BETA_DISC_ID,
                    title="Synthetic Beta",
                )
                key, digest = self._expected_boot_identity(sfo, placeholder, boot)
                self._assert_identity_in_both_tiers(iso_file, key, digest)
                keys.append(key)
        self.assertEqual(len(set(keys)), 2, "two prototype builds must not share one key")

    def test_non_retail_disc_with_an_empty_eboot_and_a_plain_boot_is_keyed(self) -> None:
        """A size-0 EBOOT.BIN beside a plain BOOT.BIN is the boot-fallback layout, keyed by
        the v2 rule rather than refused."""
        sfo = build_param_sfo(self.BETA_DISC_ID, "Synthetic Beta")
        boot = build_plain_mips_elf()
        iso_file = self.temp_dir / "proto-empty-eboot.iso"
        create_test_iso_with_executables(
            iso_file, b"", boot, disc_id=self.BETA_DISC_ID, title="Synthetic Beta",
        )
        key, digest = self._expected_boot_identity(sfo, b"", boot)
        self._assert_identity_in_both_tiers(iso_file, key, digest)

    def test_non_retail_v2_key_is_chosen_by_structure_alone(self) -> None:
        """Review F2 of PR #2855: the v2 key is chosen by the disc's structure, an empty or
        zero-filled EBOOT.BIN beside a BOOT.BIN file of any content, never by whether
        BOOT.BIN passes the executable layout rule, so tightening that rule moves no disc
        between keys, and two builds differing only in an unusable BOOT.BIN get two keys."""
        sfo = build_param_sfo(self.BETA_DISC_ID, "Synthetic Beta")
        cases = {
            "zero-eboot-unusable-boot": (bytes(2048), b"not an executable\n"),
            "zero-eboot-other-unusable-boot": (bytes(2048), b"another non-executable\n"),
            "zero-eboot-encrypted-boot": (bytes(2048), build_psp_container()),
            "empty-eboot-unusable-boot": (b"", b"not an executable\n"),
        }
        keys = set()
        for name, (eboot, boot) in cases.items():
            with self.subTest(case=name):
                iso_file = self.temp_dir / f"v2-{name}.iso"
                create_test_iso_with_executables(
                    iso_file, eboot, boot, disc_id=self.BETA_DISC_ID, title="Synthetic Beta",
                )
                key, digest = self._expected_boot_identity(sfo, eboot, boot)
                self._assert_identity_in_both_tiers(iso_file, key, digest)
                keys.add(key)
        self.assertEqual(len(keys), len(cases))

    BOOT_REFUSAL = (
        "PARAM.SFO names this disc '{disc}', which is not a retail disc ID, and its "
        "PSP_GAME/SYSDIR/EBOOT.BIN is empty or all zeros but PSP_GAME/SYSDIR/BOOT.BIN is "
        "unreadable, so the disc cannot be identified by its contents"
    )

    def _assert_refused_in_both_tiers(self, iso_file: Path, message: str) -> None:
        with self.assertRaises(IsoInspectionError) as caught:
            inspect_iso(iso_file)
        self.assertEqual(str(caught.exception), message)
        c_meta = self._run_native_inspect(iso_file)
        self.assertTrue(c_meta.get("RESULT", "").startswith("ERROR:"), c_meta)
        self.assertEqual(c_meta.get("MESSAGE"), message)

    def _with_sysdir(self, name: str, eboot: bytes, boot: bytes | None,
                     records: list[bytes]) -> Path:
        """A prototype image whose SYSDIR holds exactly these records, after its own two;
        EBOOT.BIN's bytes are at LBA 36 and BOOT.BIN's at LBA 37."""
        iso_file = self.temp_dir / f"sysdir-{name}.iso"
        create_test_iso_with_executables(
            iso_file, eboot, boot, disc_id=self.BETA_DISC_ID, title="Synthetic Beta",
        )
        data = bytearray(iso_file.read_bytes())
        entries = (_dir_record(bytes([0]), 35, 2048, True) + _dir_record(bytes([1]), 34, 2048, True)
                   + b"".join(records))
        data[35 * 2048:36 * 2048] = entries + bytes(2048 - len(entries))
        iso_file.write_bytes(data)
        return iso_file

    @staticmethod
    def _conflicting(record: bytes) -> bytes:
        """The record with the big-endian copy of its location one sector on."""
        changed = bytearray(record)
        struct.pack_into(">I", changed, 6, struct.unpack_from("<I", changed, 2)[0] + 1)
        return bytes(changed)

    OUTSIDE_LBA = 0x00FF0000

    def test_unreadable_boot_bin_is_refused_by_name_in_both_tiers(self) -> None:
        """Review F6 and R2 of PR #2855: when BOOT.BIN is what cannot be read, the refusal
        names BOOT.BIN, the same text in both tiers, not EBOOT.BIN: a BOOT.BIN record whose
        two copies of its location disagree, and a BOOT.BIN whose only record lies outside
        the image (a truncated dump), which no longer gets the v1 key quietly."""
        boot = build_plain_mips_elf()
        message = self.BOOT_REFUSAL.format(disc=self.BETA_DISC_ID)
        eboot_record = _dir_record(b"EBOOT.BIN;1", 36, 2048, False)
        boot_record = _dir_record(b"BOOT.BIN;1", 37, len(boot), False)
        outside = self._with_sysdir(
            "boot-outside", bytes(2048), boot,
            [eboot_record, _dir_record(b"BOOT.BIN;1", self.OUTSIDE_LBA, len(boot), False)])
        self._assert_refused_in_both_tiers(outside, message)
        conflicting = self._with_sysdir(
            "boot-conflicting", bytes(2048), boot, [eboot_record, self._conflicting(boot_record)])
        self._assert_refused_in_both_tiers(conflicting, message)

    def test_directory_walkers_share_one_rule(self) -> None:
        """Review R3 of PR #2855: nk_iso_find_in_dir and _lookup_iso_file_detail with
        skip_outside (the non-retail identity's reads) read a directory by one rule. Only a
        record with the wanted name has its location read; one outside
        the image is passed over for a later record of the name; a record of another name
        with disagreeing copies of its location is not read. The same for EBOOT.BIN and
        BOOT.BIN, and a disc keyed by main is keyed the same way (BETA_KEY)."""
        sfo = build_param_sfo(self.BETA_DISC_ID, "Synthetic Beta")
        boot = build_plain_mips_elf()
        zero = bytes(2048)
        v2_key, v2_digest = self._expected_boot_identity(sfo, zero, boot)
        junk = self._conflicting(_dir_record(b"JUNK.BIN;1", 38, 16, False))
        cases = {
            "boot-outside-then-inside": (zero, boot, [
                _dir_record(b"EBOOT.BIN;1", 36, len(zero), False),
                _dir_record(b"BOOT.BIN;1", self.OUTSIDE_LBA, len(boot), False),
                _dir_record(b"BOOT.BIN;1", 37, len(boot), False),
            ], (v2_key, v2_digest)),
            "bad-record-between-eboot-and-boot": (zero, boot, [
                _dir_record(b"EBOOT.BIN;1", 36, len(zero), False),
                junk,
                _dir_record(b"BOOT.BIN;1", 37, len(boot), False),
            ], (v2_key, v2_digest)),
            "eboot-outside-then-inside": (self.BETA_EXECUTABLE, None, [
                _dir_record(b"EBOOT.BIN;1", self.OUTSIDE_LBA, len(self.BETA_EXECUTABLE), False),
                _dir_record(b"EBOOT.BIN;1", 36, len(self.BETA_EXECUTABLE), False),
            ], self._expected_identity(sfo, self.BETA_EXECUTABLE)),
            "bad-record-before-eboot": (self.BETA_EXECUTABLE, None, [
                junk,
                _dir_record(b"EBOOT.BIN;1", 36, len(self.BETA_EXECUTABLE), False),
            ], self._expected_identity(sfo, self.BETA_EXECUTABLE)),
        }
        for name, (eboot, boot_bytes, records, (key, digest)) in cases.items():
            with self.subTest(case=name):
                iso_file = self._with_sysdir(name, eboot, boot_bytes, records)
                self._assert_identity_in_both_tiers(iso_file, key, digest)
        self.assertEqual(self._expected_identity(sfo, self.BETA_EXECUTABLE)[0], self.BETA_KEY)

        # A record of the wanted name whose copies of its location disagree still stops
        # the search, in both tiers: EBOOT.BIN here, so the disc has no key.
        bad_eboot = self._with_sysdir("eboot-conflicting", self.BETA_EXECUTABLE, None, [
            self._conflicting(_dir_record(b"EBOOT.BIN;1", 36, len(self.BETA_EXECUTABLE), False)),
        ])
        self._assert_refused_in_both_tiers(
            bad_eboot,
            f"PARAM.SFO names this disc '{self.BETA_DISC_ID}', which is not a retail disc ID, "
            "and PSP_GAME/SYSDIR/EBOOT.BIN is missing or unreadable, so the disc cannot be "
            "identified by its contents",
        )

    def _outside_copy(self, source: Path, name: bytes, label: str) -> Path:
        """A copy of source whose one directory record of name points outside the image."""
        data = bytearray(source.read_bytes())
        at = data.index(bytes([len(name)]) + name) - 32
        struct.pack_into("<I", data, at + 2, self.OUTSIDE_LBA)
        struct.pack_into(">I", data, at + 6, self.OUTSIDE_LBA)
        copy = self.temp_dir / f"{label}.iso"
        copy.write_bytes(data)
        return copy

    def _native_lookup(self, iso_file: Path, path: str) -> str:
        res = subprocess.run([str(self.exe_path), "lookup", str(iso_file), path],
                             capture_output=True, text=True, check=True)
        return res.stdout.strip()

    def test_a_record_outside_the_image_is_passed_over_only_by_the_identity(self) -> None:
        """Review M1 of PR #2855: a record of the wanted name whose extent leaves the image
        is passed over only where the non-retail identity reads EBOOT.BIN and BOOT.BIN
        (R3). Every other lookup gives the result it gave before this PR: a file whose only
        record lies outside the image is never read as a file the disc does not have, and a
        later record of the name does not turn such a refusal into a read."""
        runtime_root = self.temp_dir / "runtime"
        runtime_root.mkdir()
        unreadable = ("Disc directories could not be read safely; PARAM.SFO and executables "
                      "were not trusted.")
        good = self.temp_dir / "good.iso"
        create_test_iso_with_executables(good, build_plain_mips_elf())
        metadata = inspect_iso(good)

        def disc_check(iso_file: Path) -> tuple[object, object]:
            report = inspect_compatibility_preflight(
                iso_file, metadata=metadata, runtime_root=runtime_root)
            check = next(c for c in report["checks"] if c["code"] == "DISC_SFO")
            return check["status"], check["message"]

        # inspect_iso and nk_iso_inspect: PARAM.SFO, refused with one text in both tiers.
        sfo_outside = self._outside_copy(good, b"PARAM.SFO;1", "sfo-outside")
        self._assert_refused_in_both_tiers(sfo_outside, "PSP_GAME/PARAM.SFO extent is invalid")
        # inspect_compatibility_preflight: PARAM.SFO.
        self.assertEqual(disc_check(sfo_outside), ("UNSUPPORTED", unreadable))
        # A PARAM.SFO record outside the image before one inside it is refused in both tiers
        # (the PARAM.SFO lookup rule; test_param_sfo_lookup_gives_one_result_in_both_tiers).
        sfo = build_param_sfo("TEST00001", "Test Game")
        two_sfo = bytearray(good.read_bytes())
        game_dir = (_dir_record(bytes([0]), 34, 2048, True) + _dir_record(bytes([1]), 33, 2048, True)
                    + _dir_record(b"PARAM.SFO;1", self.OUTSIDE_LBA, len(sfo), False)
                    + _dir_record(b"PARAM.SFO;1", 32, len(sfo), False)
                    + _dir_record(b"SYSDIR", 35, 2048, True))
        two_sfo[34 * 2048:35 * 2048] = game_dir + bytes(2048 - len(game_dir))
        sfo_twice = self.temp_dir / "sfo-outside-then-inside.iso"
        sfo_twice.write_bytes(two_sfo)
        with self.assertRaises(IsoInspectionError) as caught:
            inspect_iso(sfo_twice)
        self.assertEqual(str(caught.exception), "PSP_GAME/PARAM.SFO extent is invalid")
        self.assertEqual(disc_check(sfo_twice), ("UNSUPPORTED", unreadable))
        self.assertEqual(self._run_native_inspect(sfo_twice).get("MESSAGE"),
                         "PSP_GAME/PARAM.SFO extent is invalid")

        # _boot_executable_facts and _classify_iso_executable (preflight and the boot
        # selection), iso_executable_sha256: EBOOT.BIN of a disc with a retail-form ID.
        eboot_outside = self._outside_copy(good, b"EBOOT.BIN;1", "eboot-outside")
        self.assertEqual(disc_check(eboot_outside), ("UNSUPPORTED", unreadable))
        self.assertIsNone(iso_inspect.select_boot_executable_source(eboot_outside))
        self.assertIsNone(iso_inspect.iso_executable_sha256(eboot_outside, "EBOOT.BIN"))
        native = self._run_native_inspect(eboot_outside)
        self.assertEqual((native.get("RESULT"), native.get("EBOOT_KIND"),
                          native.get("SELECTED_EXECUTABLE")), ("OK", "0", "0"))

        # _lookup_iso_file itself, as nk_cli.py, decrypt_tool.py, check_needed_modules and
        # write_experimental_profile call it, beside nk_iso_reader_lookup (every C caller
        # of nk_iso_find_in_dir resolves through it): a file whose only record is outside
        # the image is refused by the tool and missing to the reader, as before this PR.
        for iso_file, member in ((eboot_outside, ("PSP_GAME", "SYSDIR", "EBOOT.BIN")),
                                 (sfo_outside, ("PSP_GAME", "PARAM.SFO"))):
            with self.subTest(member="/".join(member)):
                with iso_file.open("rb") as stream:
                    size = iso_file.stat().st_size
                    with self.assertRaises(IsoInspectionError):
                        iso_inspect._lookup_iso_file(stream, size, member)
                    self.assertEqual(iso_inspect._lookup_iso_file_detail(
                        stream, size, member, skip_outside=True), (None, True))
                self.assertTrue(self._native_lookup(iso_file, "/".join(member))
                                .startswith("LOOKUP:-1:"))

        # The identity (R3): a non-retail disc whose only EBOOT.BIN record is outside the
        # image is refused naming EBOOT.BIN in both tiers, as before this PR.
        only_outside = self._with_sysdir("eboot-only-outside", self.BETA_EXECUTABLE, None, [
            _dir_record(b"EBOOT.BIN;1", self.OUTSIDE_LBA, len(self.BETA_EXECUTABLE), False)])
        self._assert_refused_in_both_tiers(
            only_outside,
            f"PARAM.SFO names this disc '{self.BETA_DISC_ID}', which is not a retail disc ID, "
            "and PSP_GAME/SYSDIR/EBOOT.BIN is missing or unreadable, so the disc cannot be "
            "identified by its contents",
        )

    # ---- The PARAM.SFO lookup rule (docs/ARCHITECTURE.md): one result in both tiers ----------

    def _param_sfo_image(self, name: str, *, root=None, game=None, sfo=None, pvd_root=None):
        """A disc made at test time from create_test_iso: root directory records (after the
        '.' and '..' ones), PSP_GAME records, the PARAM.SFO bytes and a patch of the PVD's
        root record (a callable on its bytearray). Layout: PARAM.SFO at sector 32, root 33,
        PSP_GAME 34, an image of 1024 sectors."""
        base = self.temp_dir / f"{name}-base.iso"
        create_test_iso(base)
        data = bytearray(base.read_bytes())

        def put(lba: int, records: bytes) -> None:
            records = records[:2048]   # one sector; a record may be cut by it, as in a bad disc
            data[lba * 2048:(lba + 1) * 2048] = records + bytes(2048 - len(records))

        dots = (_dir_record(bytes([0]), 33, 2048, True) + _dir_record(bytes([1]), 33, 2048, True))
        if sfo is not None:
            put(32, sfo)
        if root is not None:
            put(33, dots + b"".join(root))
        if game is not None:
            put(34, dots + b"".join(game))
        if pvd_root is not None:
            pvd = bytearray(data[16 * 2048 + 156:16 * 2048 + 190])
            pvd_root(pvd)
            data[16 * 2048 + 156:16 * 2048 + 190] = pvd
        path = self.temp_dir / f"{name}.iso"
        path.write_bytes(data)
        return path

    def test_param_sfo_lookup_gives_one_result_in_both_tiers(self) -> None:
        """Review of PR #2855 (C and Python PARAM.SFO scans): nk_iso_inspect and
        _lookup_param_sfo follow one rule, so the same synthetic disc gives the same result
        and, when it is refused, the same text. Every case is made here: no committed
        image, no game data."""
        sfo_bytes = build_param_sfo("TEST00001", "Test Game")
        n = len(sfo_bytes)
        outside = self.OUTSIDE_LBA
        rec = _dir_record
        game_rec = rec(b"PSP_GAME", 34, 2048, True)
        sfo_rec = rec(b"PARAM.SFO;1", 32, n, False)

        def bad_endian(record: bytes) -> bytes:
            damaged = bytearray(record)
            damaged[6] ^= 0x01   # the big-endian extent copy now differs
            return bytes(damaged)

        def filler(count34: int, count36: int) -> bytes:
            return rec(b"X", 33, 2048, False) * count34 + rec(b"XYZ", 33, 2048, False) * count36

        def raw_record(length: int, name_length: int) -> bytes:
            record = bytearray(length)
            record[0] = length
            if length > 32:
                record[32] = name_length
            return bytes(record)

        root_bad = "Root directory extent is invalid (its both-endian copies differ, it is empty or " \
            "too large, it is not marked as a directory, or it extends beyond the ISO file)"
        sfo_outside = "PSP_GAME/PARAM.SFO extent is invalid"
        game_outside = "PSP_GAME directory extent is invalid"
        short_root = ("Malformed ISO: root directory record at offset 68 is too short for its "
                      "identifier")
        short_game = ("Malformed ISO: PSP_GAME directory record at offset 68 is too short for its "
                      "identifier")

        def pvd_set(offset: int, value: bytes):
            def patch(record: bytearray) -> None:
                record[offset:offset + len(value)] = value
            return patch

        def both32(value: int) -> bytes:
            return value.to_bytes(4, "little") + value.to_bytes(4, "big")

        # (name, image arguments, expected): ("OK", disc_id, has_psp_game) or the refusal text.
        cases: list[tuple[str, dict, tuple | str]] = [
            ("good", {}, ("OK", "TEST00001", "1")),
            ("names-by-the-readers-rule", dict(
                root=[rec(b"psp_game", 34, 2048, True)],
                game=[rec(b"param.sfo.", 32, n, False)]), ("OK", "TEST00001", "1")),
            ("no-version-suffix", dict(game=[rec(b"PARAM.SFO", 32, n, False)]),
             ("OK", "TEST00001", "1")),
            ("no-psp-game", dict(root=[rec(b"OTHER", 34, 2048, True)]), ("OK", "UNKNOWN", "0")),
            ("no-param-sfo", dict(game=[rec(b"OTHER.BIN;1", 32, n, False)]),
             ("OK", "UNKNOWN", "1")),
            ("first-record-of-the-name-wins", dict(
                game=[sfo_rec, rec(b"PARAM.SFO;1", 32, 10, False)]), ("OK", "TEST00001", "1")),
            ("first-record-of-the-name-wins-even-when-unusable", dict(
                game=[rec(b"PARAM.SFO;1", 32, 10, False), sfo_rec]),
             "PSP_GAME/PARAM.SFO has an unsupported size"),
            ("sfo-outside-then-inside", dict(
                game=[rec(b"PARAM.SFO;1", outside, n, False), sfo_rec]), sfo_outside),
            ("sfo-outside-only", dict(game=[rec(b"PARAM.SFO;1", outside, n, False)]), sfo_outside),
            ("psp-game-outside-then-inside", dict(
                root=[rec(b"PSP_GAME", outside, 2048, True), game_rec]), game_outside),
            ("psp-game-outside-only", dict(root=[rec(b"PSP_GAME", outside, 2048, True)]),
             game_outside),
            ("root-record-with-bad-copies-before-psp-game", dict(
                root=[bad_endian(rec(b"OTHER", 34, 2048, True)), game_rec]),
             "Malformed ISO: root directory record at offset 68 has conflicting both-endian "
             "extent copies (ECMA-119 7.3.3)"),
            ("game-record-with-bad-copies-before-param-sfo", dict(
                game=[bad_endian(rec(b"OTHER", 34, 2048, True)), sfo_rec]),
             "Malformed ISO: PSP_GAME directory record at offset 68 has conflicting both-endian "
             "extent copies (ECMA-119 7.3.3)"),
            ("psp-game-with-bad-copies", dict(root=[bad_endian(game_rec)]),
             "Malformed ISO: root directory record at offset 68 has conflicting both-endian "
             "extent copies (ECMA-119 7.3.3)"),
            ("record-after-the-match-is-not-read", dict(
                root=[game_rec, raw_record(20, 1)]), ("OK", "TEST00001", "1")),
            ("root-record-too-short", dict(root=[raw_record(20, 1), game_rec]), short_root),
            ("root-record-without-identifier", dict(root=[raw_record(34, 0), game_rec]),
             short_root),
            ("root-record-identifier-past-its-length", dict(
                root=[raw_record(34, 5), game_rec]), short_root),
            ("game-record-too-short", dict(game=[raw_record(20, 1), sfo_rec]), short_game),
            ("root-record-crosses-a-sector", dict(
                root=[filler(42, 14), raw_record(100, 1)]),
             "Malformed ISO: root directory record length 100 at offset 2000 crosses 2048-byte "
             "sector boundary (ECMA-119 6.8.1.1)"),
            ("game-record-past-a-short-extent", dict(
                root=[rec(b"PSP_GAME", 34, 40, True)]),
             "Malformed ISO: PSP_GAME directory record truncated before sector end"),
            ("psp-game-is-a-file", dict(root=[rec(b"PSP_GAME", 34, 2048, False)]),
             "PSP_GAME is not a directory"),
            ("psp-game-is-empty", dict(root=[rec(b"PSP_GAME", 34, 0, True)]),
             "PSP_GAME directory is empty"),
            ("psp-game-is-too-large", dict(root=[rec(b"PSP_GAME", 100, 600 * 1024, True)]),
             "PSP_GAME directory extent is larger than 524288 bytes"),
            ("param-sfo-is-a-directory", dict(game=[rec(b"PARAM.SFO;1", 35, 2048, True)]),
             "PSP_GAME/PARAM.SFO is a directory"),
            ("param-sfo-is-empty", dict(game=[rec(b"PARAM.SFO;1", 32, 0, False)]),
             "PSP_GAME/PARAM.SFO is empty"),
            ("param-sfo-is-19-bytes", dict(game=[rec(b"PARAM.SFO;1", 32, 19, False)]),
             "PSP_GAME/PARAM.SFO has an unsupported size"),
            ("param-sfo-is-over-the-limit", dict(game=[rec(b"PARAM.SFO;1", 100, 64 * 1024 + 1, False)]),
             "PSP_GAME/PARAM.SFO has an unsupported size"),
            ("root-copies-differ", dict(pvd_root=pvd_set(6, bytes([0xAA]))), root_bad),
            ("root-is-empty", dict(pvd_root=pvd_set(10, both32(0))), root_bad),
            ("root-is-too-large", dict(pvd_root=pvd_set(10, both32(600 * 1024))), root_bad),
            ("root-is-outside-the-image", dict(pvd_root=pvd_set(2, both32(outside))), root_bad),
            ("root-is-not-marked-a-directory", dict(pvd_root=pvd_set(25, bytes([0]))), root_bad),
        ]
        for name, arguments, expected in cases:
            with self.subTest(case=name):
                image = self._param_sfo_image(name, sfo=sfo_bytes, **arguments)
                native = self._run_native_inspect(image)
                if isinstance(expected, str):
                    with self.assertRaises(IsoInspectionError) as caught:
                        inspect_iso(image)
                    self.assertEqual(str(caught.exception), expected)
                    self.assertTrue(native.get("RESULT", "").startswith("ERROR:"), native)
                    self.assertEqual(native.get("MESSAGE"), expected)
                else:
                    metadata = inspect_iso(image)
                    self.assertEqual(native.get("RESULT"), "OK", native)
                    self.assertEqual((metadata.disc_id, native.get("DISC_ID"),
                                      native.get("HAS_PSP_GAME")),
                                     (expected[1], expected[1], expected[2]))

    def test_a_disc_without_a_title_is_named_after_the_key_its_entry_holds(self) -> None:
        """Review R4 of PR #2855: a prototype disc whose PARAM.SFO has no TITLE is named
        "PSP Title (<key>)"; added again under the v1 key its entry holds, it is named after
        that key in both tiers, not after its v2 key."""
        sfo = build_param_sfo(self.BETA_DISC_ID, "")
        iso_file = self.temp_dir / "proto-untitled.iso"
        create_test_iso_with_executables(
            iso_file, bytes(2048), build_plain_mips_elf(), disc_id=self.BETA_DISC_ID, title="",
        )
        v1_key, _ = self._expected_identity(sfo, bytes(2048))
        v2_key, _ = self._expected_boot_identity(sfo, bytes(2048), build_plain_mips_elf())
        meta = inspect_iso(iso_file)
        self.assertEqual((meta.disc_id, meta.title), (v2_key, f"PSP Title ({v2_key})"))
        self.assertEqual(self._run_native_inspect(iso_file).get("TITLE"), f"PSP Title ({v2_key})")
        old_record = {"disc_id": v1_key, "iso_path": str(self.temp_dir / "elsewhere.iso")}
        keyed = iso_inspect.metadata_for_new_library_record(meta, [old_record])
        self.assertEqual((keyed.disc_id, keyed.title), (v1_key, f"PSP Title ({v1_key})"))
        library = self.temp_dir / "untitled-library.json"
        self._write_library(library, [old_record])
        lines = self._native_library_key(iso_file, library)
        self.assertEqual((lines.get("ADD"), lines.get("TITLE")),
                         ([f"{v1_key}:1"], [f"PSP Title ({v1_key})"]))
        fresh = self.temp_dir / "untitled-fresh-library.json"
        self._write_library(fresh, [])
        self.assertEqual(self._native_library_key(iso_file, fresh).get("TITLE"),
                         [f"PSP Title ({v2_key})"])

    def test_non_retail_key_stays_v1_unless_boot_bin_is_the_boot_file(self) -> None:
        """A disc that boots its EBOOT.BIN keeps its v1 key byte for byte, whatever BOOT.BIN
        holds; so does a zero-filled EBOOT.BIN without a BOOT.BIN, and an encrypted
        EBOOT.BIN (it differs per build and is hashed as stored)."""
        sfo = build_param_sfo(self.BETA_DISC_ID, "Synthetic Beta")
        cases = {
            "real-eboot-alone": (self.BETA_EXECUTABLE, None),
            "real-eboot-with-boot": (self.BETA_EXECUTABLE, build_plain_mips_elf()),
            "real-eboot-with-unusable-boot": (self.BETA_EXECUTABLE, b"not an executable\n"),
            "zero-eboot-alone": (bytes(2048), None),
            "encrypted-eboot-with-boot": (build_psp_container(), build_plain_mips_elf()),
        }
        for name, (eboot, boot) in cases.items():
            with self.subTest(case=name):
                iso_file = self.temp_dir / f"v1-{name}.iso"
                create_test_iso_with_executables(
                    iso_file, eboot, boot, disc_id=self.BETA_DISC_ID, title="Synthetic Beta",
                )
                key, digest = self._expected_identity(sfo, eboot)
                if name == "real-eboot-alone":
                    self.assertEqual(key, self.BETA_KEY)
                self._assert_identity_in_both_tiers(iso_file, key, digest)

    def _native_library_key(self, iso_file: Path, library: Path) -> dict[str, list[str]]:
        res = subprocess.run(
            [str(self.exe_path), "library_key", str(iso_file), str(library)],
            capture_output=True, text=True, encoding="utf-8", errors="strict",
        )
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        lines: dict[str, list[str]] = {}
        for line in res.stdout.splitlines():
            name, _, value = line.partition(":")
            lines.setdefault(name, []).append(value)
        return lines

    @staticmethod
    def _write_library(path: Path, records: list[dict]) -> None:
        path.write_bytes(json.dumps({"schema_version": 1, "games": records}).encode("utf-8"))

    def test_prototype_entry_made_before_v2_keeps_its_v1_key(self) -> None:
        """Owner decision of 2026-11-20 on F-2281ebab (option b): a library entry made under
        the v1 key keeps it. The image is found, opened, added again and removed under that
        key in both tiers; a library without such an entry adds the disc under its v2 key;
        and an entry's recorded key version keeps the two kinds from standing for each other.
        """
        sfo = build_param_sfo(self.BETA_DISC_ID, "Synthetic Beta")
        placeholder = bytes(2048)
        boot = build_plain_mips_elf()
        iso_file = self.temp_dir / "proto-old-entry.iso"
        create_test_iso_with_executables(
            iso_file, placeholder, boot, disc_id=self.BETA_DISC_ID, title="Synthetic Beta",
        )
        v1_key, v1_digest = self._expected_identity(sfo, placeholder)
        v2_key, v2_digest = self._expected_boot_identity(sfo, placeholder, boot)
        self.assertNotEqual(v1_key, v2_key)

        meta = inspect_iso(iso_file)
        self.assertEqual(
            (meta.disc_id, meta.identity_sha256, meta.disc_key_version, meta.legacy_disc_id,
             meta.legacy_identity_sha256),
            (v2_key, v2_digest, 2, v1_key, v1_digest),
        )
        c_meta = self._run_native_inspect(iso_file)
        self.assertEqual(
            (c_meta.get("DISC_ID"), c_meta.get("IDENTITY_SHA256"), c_meta.get("DISC_KEY_VERSION"),
             c_meta.get("LEGACY_DISC_ID"), c_meta.get("LEGACY_IDENTITY_SHA256")),
            (v2_key, v2_digest, "2", v1_key, v1_digest),
        )

        # An entry written before key versions were recorded, under the v1 key.
        old_record = {"disc_id": v1_key, "title_name": "Synthetic Beta", "iso_path": str(iso_file)}
        keyed = iso_inspect.metadata_for_library_entry(meta, v1_key, 0)
        assert keyed is not None
        self.assertEqual(
            (keyed.disc_id, keyed.identity_sha256, keyed.disc_key_version, keyed.legacy_disc_id),
            (v1_key, v1_digest, 1, ""),
        )
        self.assertEqual(
            iso_inspect.metadata_for_new_library_record(meta, [old_record]).disc_id, v1_key
        )
        library = self.temp_dir / "old-key-library.json"
        self._write_library(library, [old_record])
        lines = self._native_library_key(iso_file, library)
        self.assertEqual(lines.get("MATCH"), [f"{v1_key}:0:2"])
        self.assertEqual(lines.get("ADD"), [f"{v1_key}:1"])
        self.assertEqual(
            (lines.get("COUNT_AFTER_ADD"), lines.get("SAVED_VERSION"),
             lines.get("COUNT_AFTER_REMOVE")),
            (["1"], ["1"], ["0"]),
            "the disc added again is the entry already there, under its v1 key",
        )

        # A library without that entry adds the disc under its v2 key, and records it.
        self.assertIs(iso_inspect.metadata_for_new_library_record(meta, []), meta)
        fresh = self.temp_dir / "fresh-library.json"
        self._write_library(fresh, [])
        lines = self._native_library_key(iso_file, fresh)
        self.assertEqual(lines.get("ADD"), [f"{v2_key}:2"])
        self.assertEqual((lines.get("COUNT_AFTER_ADD"), lines.get("SAVED_VERSION")),
                         (["1"], ["2"]))

        # The recorded version decides which key an entry holds; the kinds never mix.
        cases = {
            "v1 entry": (v1_key, 1, iso_inspect.KEY_MATCH_LEGACY),
            "v2 entry": (v2_key, 2, iso_inspect.KEY_MATCH),
            "v1 key marked v2": (v1_key, 2, iso_inspect.KEY_NO_MATCH),
            "v2 key marked v1": (v2_key, 1, iso_inspect.KEY_NO_MATCH),
            "v2 key unmarked": (v2_key, 0, iso_inspect.KEY_NO_MATCH),
        }
        for name, (entry_key, version, expected) in cases.items():
            with self.subTest(case=name):
                self.assertEqual(
                    iso_inspect.key_match_for_library_entry(meta, entry_key, version), expected
                )
                record = {"disc_id": entry_key, "title_name": "Synthetic Beta",
                          "iso_path": str(self.temp_dir / "elsewhere.iso"),
                          "disc_key_version": version}
                case_library = self.temp_dir / f"key-case-{version}-{entry_key}.json"
                self._write_library(case_library, [record])
                lines = self._native_library_key(iso_file, case_library)
                self.assertEqual(lines.get("MATCH"), [f"{entry_key}:{version}:{expected}"])
                self.assertEqual(
                    lines.get("ADD"),
                    [f"{v1_key}:1"] if expected == iso_inspect.KEY_MATCH_LEGACY
                    else [f"{v2_key}:2"],
                )

        # Review F5: an entry holding the image's own v2 key wins over an unversioned
        # entry under its v1 key (one an older player or CLI may add later).
        both: list[dict] = [dict(old_record, iso_path=str(self.temp_dir / "older.iso")),
                {"disc_id": v2_key, "title_name": "Synthetic Beta",
                 "iso_path": str(self.temp_dir / "newer.iso"), "disc_key_version": 2}]
        self.assertIs(iso_inspect.metadata_for_new_library_record(meta, both), meta)
        both_library = self.temp_dir / "both-keys-library.json"
        self._write_library(both_library, both)
        lines = self._native_library_key(iso_file, both_library)
        self.assertEqual(lines.get("ADD"), [f"{v2_key}:2"])

        # A malformed version is refused by name, the record and the field named (F4).
        with self.assertRaises(iso_inspect.LibraryRecordError) as caught:
            iso_inspect.metadata_for_new_library_record(
                meta, [dict(old_record, disc_key_version=3)])
        self.assertIn("disc_key_version 3", str(caught.exception))
        self.assertIn(v1_key, str(caught.exception))

        # A disc with a v1 key (a real EBOOT.BIN) matches its entry as before.
        real = self.temp_dir / "real-eboot.iso"
        create_test_iso_with_executables(
            real, self.BETA_EXECUTABLE, disc_id=self.BETA_DISC_ID, title="Synthetic Beta",
        )
        real_meta = inspect_iso(real)
        self.assertEqual((real_meta.disc_id, real_meta.disc_key_version, real_meta.legacy_disc_id),
                         (self.BETA_KEY, 1, ""))
        for version in (0, 1):
            self.assertEqual(
                iso_inspect.key_match_for_library_entry(real_meta, self.BETA_KEY, version),
                iso_inspect.KEY_MATCH,
            )
        self.assertEqual(
            iso_inspect.key_match_for_library_entry(real_meta, self.BETA_KEY, 2),
            iso_inspect.KEY_NO_MATCH,
        )

    def test_old_key_entry_keeps_its_manifest_and_the_cli_records_key_versions(self) -> None:
        """Review F1, F3 and F4 of PR #2855, through the CLI's own steps.

        F1: a disc re-keyed under the v1 key its entry holds is looked up in the title list
        under that key, so the profile the user keeps under it goes on matching. F3: the
        bring-up write records the key version (2 in an empty library; 1 when it reuses an
        unversioned old-key record), and the package preflight compares versioned keys
        (a v1 entry passes, the v1 key marked version 2 is refused). F4: a malformed
        disc_key_version stops the bring-up as LIBRARY_RECORD_INVALID, the field named."""
        from types import SimpleNamespace
        from nk_core.title_registry import TitleRegistry
        from nk_core.types import TitleProfile

        sfo = build_param_sfo(self.BETA_DISC_ID, "Synthetic Beta")
        iso_file = self.temp_dir / "proto-cli.iso"
        create_test_iso_with_executables(
            iso_file, bytes(2048), build_plain_mips_elf(), disc_id=self.BETA_DISC_ID,
            title="Synthetic Beta",
        )
        v1_key, _ = self._expected_identity(sfo, bytes(2048))
        v2_key, _ = self._expected_boot_identity(sfo, bytes(2048), build_plain_mips_elf())
        old_record = {"disc_id": v1_key, "title_name": "Synthetic Beta",
                      "iso_path": str(iso_file)}

        # F1: a title-list profile the user keeps for the entry, under the v1 key.
        registry = TitleRegistry(include_defaults=False)
        registry.register(TitleProfile(
            id="prototype-beta", name="Synthetic Beta", disc_ids=[v1_key], regions=["OTHER"],
        ))
        profile = registry.lookup_by_disc_id(v1_key)
        assert profile is not None
        meta = inspect_iso(iso_file, registry=registry)
        self.assertEqual((meta.disc_id, meta.matched_profile), (v2_key, None))
        added = iso_inspect.metadata_for_new_library_record(meta, [old_record], registry)
        self.assertEqual((added.disc_id, added.disc_key_version), (v1_key, 1))
        self.assertIsNotNone(added.matched_profile)
        assert added.matched_profile is not None
        self.assertEqual(added.matched_profile.id, profile.id)
        opened = iso_inspect.metadata_for_library_entry(meta, v1_key, 0, registry)
        assert opened is not None and opened.matched_profile is not None
        self.assertEqual(opened.matched_profile.id, profile.id)

        # F3: the bring-up write, into an empty library and into one with the old record.
        def bringup_add(root: Path) -> list:
            keyed = iso_inspect.metadata_for_new_library_record(
                inspect_iso(iso_file), nk_cli._bringup_library_records(root))
            nk_cli._write_bringup_library(root, iso_file, keyed, "experimental-beta",
                                          "BOOT.BIN", True)
            payload = json.loads((root / "library.json").read_text(encoding="utf-8"))
            return list(payload["games"])

        empty_root = self.temp_dir / "f3-empty"
        empty_root.mkdir()
        games = bringup_add(empty_root)
        self.assertEqual([(g["disc_id"], g.get("disc_key_version")) for g in games],
                         [(v2_key, 2)])
        old_root = self.temp_dir / "f3-old"
        old_root.mkdir()
        self._write_library(old_root / "library.json", [old_record])
        games = bringup_add(old_root)
        self.assertEqual([(g["disc_id"], g.get("disc_key_version")) for g in games],
                         [(v1_key, 1)])

        # F3: the package preflight. A v1 entry gets past the identity check to the next
        # one (its recorded executable, set here to differ); the v1 key marked v2 is refused.
        def preflight(root: Path, version: int) -> str:
            self._write_library(root / "library.json", [dict(
                old_record, title_id="experimental-beta", selected_executable="EBOOT.BIN",
                is_experimental=True, disc_key_version=version)])
            st = SimpleNamespace(
                args=SimpleNamespace(user_data_root=root, iso_path=None), disc_id=v1_key,
                manifest_selected=None, module_dir=None, register_local_identity=False,
                reporter=SimpleNamespace(report=lambda *_args: None),
            )
            with self.assertRaises(nk_cli.PackageBuildError) as caught:
                nk_cli._package_preflight(st)
            return str(caught.exception)

        self.assertEqual(
            preflight(old_root, 1),
            "Selected executable changed from library entry 'EBOOT.BIN' to 'BOOT.BIN'; "
            "re-import the ISO.",
        )
        self.assertEqual(
            preflight(old_root, 2),
            f"ISO identity {v2_key} no longer matches library entry {v1_key}.",
        )

        # F4: a malformed version is named, not called a bad disc image.
        bad_root = self.temp_dir / "f4-bad"
        bad_root.mkdir()
        self._write_library(bad_root / "library.json", [dict(old_record, disc_key_version=7)])
        report = nk_cli._new_bringup_report()
        failures: list[str] = []

        def fail_stage(report_value, stage, failure_class, issues=(), duration_ms=0):
            failures.append(failure_class)
            nk_cli._fail_bringup(report_value, stage, failure_class, issues, duration_ms)

        st = SimpleNamespace(
            fail_stage=fail_stage, iso_path=iso_file, metadata=None, module_dir=None,
            preflight=None, report=report, report_path=bad_root / "report.json",
            set_stage=lambda *_args: None, user_root=bad_root,
        )
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(nk_cli._bringup_inspect(st), 1)
        self.assertEqual((failures, report["failure_class"]),
                         (["LIBRARY_RECORD_INVALID"], "LIBRARY_RECORD_INVALID"))
        self.assertIn(f"the library record '{v1_key}' has disc_key_version 7", stderr.getvalue())

    def test_cli_inspect_names_a_non_retail_disc(self) -> None:
        iso_file = self.temp_dir / "cli-beta.iso"
        create_test_iso_with_executables(
            iso_file, self.BETA_EXECUTABLE, disc_id=self.BETA_DISC_ID, title="Synthetic Beta",
        )
        runtime_root = self.temp_dir / "beta-runtime-root"
        runtime_root.mkdir()
        command = [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "inspect", str(iso_file),
                   "--root", str(runtime_root)]
        result = subprocess.run(command + ["--json"], cwd=ROOT, capture_output=True,
                                text=True, check=False)
        self.assertEqual(result.returncode, 2, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(
            (payload["disc_id"], payload["sfo_disc_id"], payload["non_retail"]),
            (self.BETA_KEY, self.BETA_DISC_ID, True),
        )
        by_code = {check["code"]: check for check in payload["compatibility_preflight"]["checks"]}
        self.assertEqual(by_code["DISC_SFO"]["status"], "OK")
        text = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=False)
        self.assertIn(f"Disc ID:    {self.BETA_KEY}", text.stdout)
        self.assertIn(f"Beta / unofficial: the disc's own ID '{self.BETA_DISC_ID}'", text.stdout)

    def test_cli_inspect_shows_structured_compatibility_preflight(self) -> None:
        iso_file = self.temp_dir / "cli-preflight.iso"
        runtime_root = self.temp_dir / "empty-runtime-root"
        runtime_root.mkdir()
        create_test_iso(iso_file, disc_id="TEST00001", title="Synthetic Test Title")
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "inspect",
             str(iso_file), "--json", "--root", str(runtime_root)],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertIn("compatibility_preflight", payload)
        checks = payload["compatibility_preflight"]["checks"]
        self.assertEqual(
            [check["code"] for check in checks],
            ["DISC_SFO", "EXECUTABLE", "RUNTIME_PACKAGE", "DATA_ROOT", "SYSTEM_FONTS", "AUDIO_OUTPUT"],
        )
        self.assertTrue(all(check["status"] in {
            "OK", "MISSING", "UNSUPPORTED", "IN_PROGRESS",
        } for check in checks))
        by_code = {check["code"]: check for check in checks}
        metadata = inspect_iso(iso_file)
        assert metadata.matched_profile is not None
        self.assertTrue(metadata.matched_profile.archive_relpath)
        checkout_data = ROOT / metadata.matched_profile.archive_relpath
        self.assertEqual(
            by_code["DATA_ROOT"]["status"], "OK" if checkout_data.is_dir() else "MISSING"
        )
        self.assertEqual(by_code["RUNTIME_PACKAGE"]["status"], "MISSING")
        self.assertEqual(by_code["AUDIO_OUTPUT"]["status"], "OK")
        original_font_status = by_code["SYSTEM_FONTS"]["status"]

        staged_data = (
            runtime_root / "games" / metadata.disc_id
            / metadata.matched_profile.archive_relpath
        )
        staged_data.mkdir(parents=True)
        ready = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "inspect",
             str(iso_file), "--json", "--root", str(runtime_root)],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(ready.returncode, 0, ready.stderr)
        ready_checks = {check["code"]: check for check in
                        json.loads(ready.stdout)["compatibility_preflight"]["checks"]}
        self.assertEqual(ready_checks["DATA_ROOT"]["status"], "OK")
        self.assertEqual(ready_checks["RUNTIME_PACKAGE"]["status"], "MISSING")
        self.assertEqual(ready_checks["SYSTEM_FONTS"]["status"], original_font_status)

        shutil.rmtree(runtime_root / "games" / metadata.disc_id)
        extracted_data_root = (
            iso_file.parent / "EXTRACTED" / "PSP_GAME" / "USRDIR" / "tests" / "fixtures" / "profile_zero"
        )
        extracted_data_root.mkdir(parents=True)
        extracted_ready = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "inspect",
             str(iso_file), "--json", "--root", str(runtime_root)],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(extracted_ready.returncode, 0, extracted_ready.stderr)
        extracted_checks = {check["code"]: check for check in
                            json.loads(extracted_ready.stdout)["compatibility_preflight"]["checks"]}
        self.assertEqual(extracted_checks["DATA_ROOT"]["status"], "OK")

    def test_preflight_validates_runtime_package_in_the_per_title_packages_folder(self) -> None:
        iso_file = self.temp_dir / "package-cache-preflight.iso"
        create_test_iso(iso_file, disc_id="TEST00001", title="Synthetic Test Title")
        metadata = inspect_iso(iso_file)
        self.assertIsNotNone(metadata.matched_profile)
        runtime_root = self.temp_dir / "package-cache-user-data"
        package_dir = runtime_root / "packages" / metadata.disc_id
        package_dir.mkdir(parents=True)

        with patch.object(iso_inspect, "package_cache", create=True) as package_cache:
            package_cache.validate_package_cache.return_value = (True, "")
            report = inspect_compatibility_preflight(
                iso_file, metadata=metadata, runtime_root=runtime_root
            )

        runtime_check = next(
            check for check in report["checks"] if check["code"] == "RUNTIME_PACKAGE"
        )
        self.assertEqual(runtime_check["status"], "OK")
        self.assertEqual(
            Path(package_cache.validate_package_cache.call_args.args[0]), package_dir
        )

    def test_boot_selection_and_preflight_share_their_on_disc_facts(self) -> None:
        iso_file = self.temp_dir / "shared-boot-facts.iso"
        create_test_iso_with_executables(
            iso_file, b"\0" * 64, build_plain_mips_elf(),
            disc_id="TEST00001", title="Synthetic Test Title",
        )
        metadata = inspect_iso(iso_file)
        self.assertEqual(iso_inspect.select_boot_executable_source(iso_file), "BOOT.BIN")
        facts_helper = getattr(iso_inspect, "_boot_executable_facts", None)
        self.assertIsNotNone(facts_helper, "boot facts must have one shared implementation")

        with patch.object(iso_inspect, "_boot_executable_facts", wraps=facts_helper) as shared:
            inspect_compatibility_preflight(
                iso_file, metadata=metadata, runtime_root=self.temp_dir / "shared-facts-root"
            )
            iso_inspect.select_boot_executable_source(iso_file)

        self.assertEqual(shared.call_count, 2)

    def test_preflight_finds_data_root_in_the_staged_title_folder(self) -> None:
        iso_file = self.temp_dir / "staged-data-root-preflight.iso"
        create_test_iso(iso_file, disc_id="TEST00001", title="Synthetic Test Title")
        metadata = inspect_iso(iso_file)
        assert metadata.matched_profile is not None
        self.assertTrue(metadata.matched_profile.archive_relpath)
        runtime_root = self.temp_dir / "staged-data-root-user-data"
        project_root = self.temp_dir / "staged-data-root-project"
        project_root.mkdir()
        staged_data = (
            runtime_root / "games" / metadata.disc_id
            / metadata.matched_profile.archive_relpath
        )
        staged_data.mkdir(parents=True)

        report = inspect_compatibility_preflight(
            iso_file, metadata=metadata, runtime_root=runtime_root, project_root=project_root
        )

        data_check = next(check for check in report["checks"] if check["code"] == "DATA_ROOT")
        self.assertEqual(data_check["status"], "OK")

    def test_preflight_uses_the_project_checkout_for_project_fonts(self) -> None:
        iso_file = self.temp_dir / "project-font-preflight.iso"
        create_test_iso(iso_file, disc_id="TEST00001", title="Synthetic Test Title")
        metadata = inspect_iso(iso_file)
        runtime_root = self.temp_dir / "project-font-user-data"
        project_root = self.temp_dir / "synthetic-project-root"
        font_dir = project_root / "font"
        font_dir.mkdir(parents=True)

        from pgf_writer import Glyph, build_pgf

        def glyph(code: int) -> Glyph:
            return Glyph(code=code, width=2, height=2, samples=(1, 2, 3, 4), row_order=1)

        (font_dir / "nkjpn.pgf").write_bytes(
            build_pgf([glyph(0x41), glyph(0x3042)], font_name="Synthetic Japanese")
        )
        (font_dir / "nkltn.pgf").write_bytes(
            build_pgf([glyph(0x41), glyph(0x61)], font_name="Synthetic Latin")
        )
        (font_dir / "nkkr.pgf").write_bytes(
            build_pgf([glyph(0x41), glyph(0xAC00)], font_name="Synthetic Korean")
        )

        with patch.object(iso_inspect, "PROJECT_ROOT", project_root, create=True):
            report = inspect_compatibility_preflight(
                iso_file, metadata=metadata, runtime_root=runtime_root
            )

        fonts_check = next(
            check for check in report["checks"] if check["code"] == "SYSTEM_FONTS"
        )
        self.assertEqual(fonts_check["status"], "SYNTHETIC")

    def test_concurrent_experimental_profile_writes_use_distinct_atomic_temporaries(self) -> None:
        from nk_core import package_cache

        iso_file = self.temp_dir / "concurrent-experimental-profile.iso"
        create_test_iso_with_executables(
            iso_file, build_plain_mips_elf(), disc_id="PROTO1234567",
            title="Synthetic Concurrent Profile",
        )
        user_root = self.temp_dir / "concurrent-profile-user-data"
        replacement_sources: list[str] = []
        original_replace = package_cache.os.replace

        def synchronized_profile_replace(source, destination):
            if Path(destination).name == "profile.json":
                replacement_sources.append(Path(source).name)
            return original_replace(source, destination)

        with patch.object(package_cache.os, "replace", side_effect=synchronized_profile_replace):
            with ThreadPoolExecutor(max_workers=2) as pool:
                futures = [
                    pool.submit(write_experimental_profile, iso_file, user_root)
                    for _ in range(2)
                ]
                paths = [future.result(timeout=10) for future in futures]

        self.assertEqual(len(replacement_sources), 2)
        self.assertEqual(len(set(replacement_sources)), 2)
        self.assertEqual(paths[0], paths[1])
        self.assertEqual(json.loads(paths[0].read_text(encoding="utf-8"))["schema_version"], 1)

    def test_cli_reports_refused_local_profile_with_shared_catalog_state(self) -> None:
        iso_file = self.temp_dir / "cli-refused-profile.iso"
        user_root = self.temp_dir / "profile-user-data"
        manifest_dir = user_root / "manifests"
        manifest_dir.mkdir(parents=True)
        create_test_iso(iso_file, disc_id="TEST00007", title="Synthetic Test Title")

        manifest = json.loads((ROOT / "assets" / "titles" / "showcase-scene.json")
                              .read_text(encoding="utf-8"))
        manifest["kind"] = "retail"
        manifest["disc"] = {
            "id": "TEST00007", "region": "OTHER",
            "revision_policy": "exact-disc-id",
        }
        manifest.pop("profile_zero", None)
        manifest["runtime_bindings"] = {
            "schema_version": 1,
            "vblank_frame_counter_addr": 0x08804000,
        }
        (manifest_dir / "retired-binding.json").write_text(
            json.dumps(manifest), encoding="utf-8"
        )
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "inspect",
             str(iso_file), "--json", "--root", str(user_root)],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertTrue(payload["catalogued"])
        self.assertTrue(payload["supported"])
        self.assertIn("vblank_frame_counter_addr", payload["profile_validation"])
        human = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "inspect",
             str(iso_file), "--root", str(user_root)],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(human.returncode, 0, human.stderr)
        self.assertIn("Catalogued: YES", human.stdout)
        self.assertIn("Supported:  YES", human.stdout)
        self.assertIn("vblank_frame_counter_addr", human.stdout)

    def test_cli_preflight_selects_plain_boot_fallback(self) -> None:
        iso_file = self.temp_dir / "cli-boot-fallback.iso"
        runtime_root = self.temp_dir / "empty-runtime-root"
        runtime_root.mkdir()
        create_test_iso_with_executables(
            iso_file, build_psp_container(), build_plain_mips_elf()
        )
        metadata = inspect_iso(iso_file)
        report = inspect_compatibility_preflight(
            iso_file, metadata=metadata, runtime_root=runtime_root
        )
        self.assertEqual(report["executables"]["EBOOT.BIN"]["classification"],
                         "PSP_ENCRYPTED_CONTAINER")
        self.assertEqual(report["executables"]["BOOT.BIN"]["classification"],
                         "PLAIN_MIPS_ELF32")
        self.assertEqual(report["selected_executable"], "BOOT.BIN")
        self.assertTrue(report["boot_fallback"])
        native = self._run_native_inspect(iso_file)
        self.assertEqual(native["PARAM_SFO_PARSED"], "1")
        self.assertEqual(native["EBOOT_KIND"], "2")
        self.assertEqual(native["BOOT_KIND"], "1")
        self.assertEqual(native["SELECTED_EXECUTABLE"], "2")
        self.assertEqual(native["BOOT_FALLBACK"], "1")
        executable_check = next(
            check for check in report["checks"] if check["code"] == "EXECUTABLE"
        )
        self.assertEqual(executable_check["status"], "OK")
        self.assertIn("BOOT.BIN selected for analysis", executable_check["message"])

    def test_zero_filled_eboot_selects_plain_boot_in_both_inspectors(self) -> None:
        """Prototype and development discs carry a zero-filled EBOOT.BIN beside a plain BOOT.BIN;
        BOOT.BIN is the executable, as for an encrypted EBOOT.BIN."""
        iso_file = self.temp_dir / "zero-eboot-boot-fallback.iso"
        runtime_root = self.temp_dir / "empty-runtime-root-zero-eboot"
        runtime_root.mkdir()
        create_test_iso_with_executables(iso_file, bytes(2048), build_plain_mips_elf())
        report = inspect_compatibility_preflight(
            iso_file, metadata=inspect_iso(iso_file), runtime_root=runtime_root
        )
        self.assertEqual(report["executables"]["EBOOT.BIN"]["classification"],
                         "EMPTY_OR_ZERO_FILLED")
        self.assertEqual(report["selected_executable"], "BOOT.BIN")
        self.assertTrue(report["boot_fallback"])
        self.assertEqual(iso_inspect.select_boot_executable_source(iso_file), "BOOT.BIN")
        executable_check = next(
            check for check in report["checks"] if check["code"] == "EXECUTABLE"
        )
        self.assertEqual(executable_check["status"], "OK")
        self.assertIn("EBOOT.BIN is zero-filled", executable_check["message"])
        native = self._run_native_inspect(iso_file)
        self.assertEqual(native["EBOOT_KIND"], "4")
        self.assertEqual(native["BOOT_KIND"], "1")
        self.assertEqual(native["SELECTED_EXECUTABLE"], "2")
        self.assertEqual(native["BOOT_FALLBACK"], "1")

    def test_zero_filled_eboot_without_boot_stays_unsupported(self) -> None:
        iso_file = self.temp_dir / "zero-eboot-alone.iso"
        runtime_root = self.temp_dir / "empty-runtime-root-zero-alone"
        runtime_root.mkdir()
        create_test_iso_with_executables(iso_file, bytes(2048))
        report = inspect_compatibility_preflight(
            iso_file, metadata=inspect_iso(iso_file), runtime_root=runtime_root
        )
        self.assertIsNone(report["selected_executable"])
        self.assertFalse(report["boot_fallback"])
        native = self._run_native_inspect(iso_file)
        self.assertEqual(native["SELECTED_EXECUTABLE"], "0")
        self.assertEqual(native["BOOT_FALLBACK"], "0")

    def test_cli_preflight_guides_encrypted_eboot_to_title_scoped_user_input(self) -> None:
        iso_file = self.temp_dir / "cli-encrypted-user-input.iso"
        user_root = self.temp_dir / "user-data"
        decrypted_dir = user_root / "titles" / "TEST00001" / "decrypted"
        create_test_iso_with_executables(
            iso_file, build_psp_container(), disc_id="TEST00001",
            title="Synthetic Test Title",
        )

        metadata = inspect_iso(iso_file)
        report = inspect_compatibility_preflight(
            iso_file, metadata=metadata, runtime_root=user_root
        )
        executable = next(check for check in report["checks"]
                          if check["code"] == "EXECUTABLE")
        self.assertEqual(executable["status"], "UNSUPPORTED")
        self.assertEqual(
            executable["message"],
            "ENCRYPTED_EXECUTABLE: EBOOT.BIN is encrypted and no decrypted EBOOT.elf is in "
            f"{decrypted_dir}. The player decrypts it from your own disc when it checks the "
            "disc; you can also place a decrypted EBOOT.elf from your own copy there.",
        )
        self.assertNotRegex(executable["message"], r"#[0-9]+")
        cli = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "inspect",
             str(iso_file), "--json", "--root", str(user_root)],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(cli.returncode, 0, cli.stderr)
        cli_checks = json.loads(cli.stdout)["compatibility_preflight"]["checks"]
        cli_executable = next(check for check in cli_checks
                              if check["code"] == "EXECUTABLE")
        self.assertIn(str(decrypted_dir), cli_executable["message"])
        self.assertIn("ENCRYPTED_EXECUTABLE:", cli_executable["message"])
        self.assertNotIn("key file", cli_executable["message"])
        self.assertNotIn("automatic decryption is in the works", cli_executable["message"].lower())
        self.assertNotRegex(cli_executable["message"], r"#[0-9]+")

        decrypted_dir.mkdir(parents=True)
        eboot = decrypted_dir / "EBOOT.elf"
        eboot.write_bytes(b"not an ELF")
        invalid = inspect_compatibility_preflight(
            iso_file, metadata=metadata, runtime_root=user_root
        )
        invalid_check = next(check for check in invalid["checks"]
                             if check["code"] == "EXECUTABLE")
        self.assertEqual(invalid_check["status"], "UNSUPPORTED")
        self.assertIn("invalid", invalid_check["message"].lower())
        self.assertNotIn("in the works", invalid_check["message"].lower())
        self.assertEqual(invalid_check["issues"], [])

        eboot.write_bytes(build_plain_mips_elf())
        valid = inspect_compatibility_preflight(
            iso_file, metadata=metadata, runtime_root=user_root
        )
        valid_check = next(check for check in valid["checks"]
                           if check["code"] == "EXECUTABLE")
        self.assertEqual(valid_check["status"], "OK")
        self.assertEqual(valid["selected_executable"], "EBOOT.elf")
        self.assertEqual(valid["decrypted_executable"], str(eboot))
        for name, wrapped in (("sce", b"~SCE" + bytes(124)),
                              ("pbp", b"\0PBP" + bytes(124))):
            wrapped_iso = self.temp_dir / f"cli-wrapped-{name}.iso"
            create_test_iso_with_executables(
                wrapped_iso, wrapped, disc_id="TEST00001",
                title="Synthetic Test Title",
            )
            wrapped_meta = inspect_iso(wrapped_iso)
            wrapped_report = inspect_compatibility_preflight(
                wrapped_iso, metadata=wrapped_meta, runtime_root=user_root
            )
            wrapped_check = next(check for check in wrapped_report["checks"]
                                 if check["code"] == "EXECUTABLE")
            self.assertEqual(wrapped_check["status"], "OK")
            self.assertEqual(wrapped_report["selected_executable"], "EBOOT.elf")

    def test_build_package_discovers_user_decrypted_elf_and_modules(self) -> None:
        iso_file = self.temp_dir / "synthetic-package-input.iso"
        user_root = self.temp_dir / "package-user-data"
        disc_id = "TEST00002"
        title_id = "synthetic-title2-v1"
        create_test_iso_with_executables(
            iso_file, build_psp_container(), disc_id=disc_id,
            title="Synthetic Title 2 Fixture",
        )
        entry = {
            "disc_id": disc_id,
            "title_id": title_id,
            "iso_path": str(iso_file),
            "selected_executable": "",
            "is_experimental": False,
        }
        user_root.mkdir()
        (user_root / "library.json").write_text(
            json.dumps({"schema_version": 1, "games": [entry]}),
            encoding="utf-8",
        )
        decrypted_dir = user_root / "titles" / disc_id / "decrypted"
        decrypted_dir.mkdir(parents=True)
        eboot_bytes = build_plain_mips_elf()
        module_bytes = build_plain_mips_elf(0xFFA0)
        (decrypted_dir / "EBOOT.elf").write_bytes(eboot_bytes)
        (decrypted_dir / "synthetic2.prx").write_bytes(module_bytes)

        manifest_path = ROOT / "assets" / "titles" / "synthetic-title2.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["modules"][0]["required"] = True
        manifest["modules"][0]["role"] = "guest-prx"
        captured_command: list[str] = []

        def fake_package_build(command, **_kwargs):
            if _is_compiler_target_query(command):
                return _REAL_SUBPROCESS_RUN(command, **_kwargs)
            captured_command.extend(command)
            build_dir = Path(command[command.index("--output-dir") + 1])
            build_dir.mkdir(parents=True, exist_ok=True)
            module_path = Path(command[command.index("--module-dir") + 1])
            identity = json.loads(Path(
                command[command.index("--title-input-identity-file") + 1]
            ).read_text(encoding="utf-8"))
            cache_key = nk_cli._current_package_cache_key(
                manifest,
                Path(command[1]),
                hashlib.sha256(eboot_bytes).hexdigest(),
                module_path,
                None,
                identity,
            )
            executable = build_dir / "synthetic.exe"
            image = build_dir / "synthetic_image.bin"
            generated = build_dir / "synthetic_recomp.o"
            executable.write_bytes(b"fake executable")
            image.write_bytes(b"fake image")
            generated.write_bytes(b"fake object")
            cache = nk_cli.package_cache.cache_metadata(
                cache_key,
                nk_cli._package_codegen_options(manifest, os.environ),
            )
            package = {
                "format": "nakagawa-aot-package",
                "schema_version": 2,
                "cache": cache,
                "title": {"id": title_id},
                "inputs": {
                    "manifest": {"sha256": cache_key["aot"]["components"]["manifest_sha256"]},
                    "executable": {"sha256": hashlib.sha256(eboot_bytes).hexdigest()},
                    "modules": [{
                        "name": "synthetic2.prx",
                        "load_address": f"0x{int(manifest['modules'][0]['load_address']):08x}",
                        "sha256": hashlib.sha256(module_bytes).hexdigest(),
                    }],
                    "psp_header": None,
                },
                "title_input_identity": identity,
                "executable": {
                    "path": executable.name,
                    "sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
                },
                "generated_objects": [{
                    "path": generated.name,
                    "sha256": hashlib.sha256(generated.read_bytes()).hexdigest(),
                }],
            }
            report = {"cache": cache}
            (build_dir / "package.json").write_text(
                json.dumps(package), encoding="utf-8"
            )
            (build_dir / "build-report.json").write_text(
                json.dumps(report), encoding="utf-8"
            )
            nk_cli.package_cache.write_completion_manifest(
                build_dir, cache_key, title_input_identity=identity
            )
            return subprocess.CompletedProcess(command, 0, "", "")

        args = type("BuildArgs", (), {
            "disc_id": disc_id,
            "user_data_root": user_root,
            "module_dir": None,
            "psp_header": None,
        })()
        with patch.object(nk_cli, "_load_entry_manifest",
                          return_value=(manifest_path, manifest, None)), \
             patch.object(nk_cli.subprocess, "run", side_effect=fake_package_build), \
             patch.object(nk_cli, "_stage_runtime_assets"):
            self.assertEqual(nk_cli.cmd_build_package(args), 0)

        game_elf = Path(captured_command[captured_command.index("--game-elf") + 1])
        module_dir = Path(captured_command[captured_command.index("--module-dir") + 1])
        self.assertEqual(game_elf.read_bytes(), eboot_bytes)
        self.assertEqual((module_dir / "synthetic2.prx").read_bytes(), module_bytes)
        captured_command.clear()
        with patch.object(nk_cli, "_load_entry_manifest",
                          return_value=(manifest_path, manifest, None)), \
             patch.object(nk_cli.subprocess, "run", side_effect=fake_package_build), \
             patch.object(nk_cli, "_stage_runtime_assets"):
            self.assertEqual(nk_cli.cmd_build_package(args), 0)
        self.assertEqual(captured_command, [])
        (decrypted_dir / "synthetic2.prx").write_bytes(b"not an ELF")
        with self.assertRaisesRegex(nk_cli.PackageBuildError, "not a decrypted ELF"):
            nk_cli._copy_optional_modules(
                iso_file, manifest, user_root / "bad-module-cache", None,
                decrypted_dir,
            )

    def _experimental_module_build(
        self, disc_id: str, usrdir_modules: dict[str, bytes],
        decrypted_modules: dict[str, bytes] | None = None,
        extra_args: dict[str, object] | None = None,
        planner_environments: list[dict[str, str]] | None = None,
    ) -> tuple[int, Path, list[str], str]:
        """Press BUILD PACKAGE for an uncatalogued disc whose USRDIR ships modules.

        The library entry and profile are what an imported uncatalogued disc gets
        (write_experimental_profile: no guest modules declared). The fake planner
        stops the build at its first codegen step, after the module intake, and
        records its command (and, given planner_environments, its environment).
        extra_args are further build-package arguments. Returns (exit code, user
        root, command, stderr).
        """
        iso_file = self.temp_dir / f"{disc_id}-experimental-modules.iso"
        user_root = self.temp_dir / f"{disc_id}-user-data"
        user_root.mkdir()
        create_test_iso_with_modules(
            iso_file, build_plain_mips_elf(e_type=2, memsz=0x40000),
            sysdir_modules={}, usrdir_modules=usrdir_modules, disc_id=disc_id,
            title="Synthetic Experimental Modules",
        )
        profile_path = write_experimental_profile(iso_file, user_root)
        profile = json.loads(profile_path.read_text(encoding="utf-8"))
        self.assertEqual(profile["manifest"]["modules"], [])
        (user_root / "library.json").write_text(json.dumps({
            "schema_version": 1,
            "games": [{
                "disc_id": disc_id,
                "title_id": profile["manifest"]["id"],
                "iso_path": str(iso_file),
                "selected_executable": "EBOOT.BIN",
                "is_experimental": True,
            }],
        }), encoding="utf-8")
        if decrypted_modules:
            decrypted_dir = user_root / "titles" / disc_id / "decrypted"
            decrypted_dir.mkdir(parents=True)
            for name, contents in decrypted_modules.items():
                (decrypted_dir / name).write_bytes(contents)
        captured: list[str] = []

        def stop_at_codegen(command, **kwargs):
            if _is_compiler_target_query(command):
                return _REAL_SUBPROCESS_RUN(command, **kwargs)
            captured.extend(command)
            if planner_environments is not None:
                planner_environments.append(dict(kwargs.get("env") or {}))
            raise nk_cli.PackageBuildError("TEST_STOP_AFTER_MODULE_INTAKE")

        args = type("BuildArgs", (), {
            "disc_id": disc_id,
            "user_data_root": user_root,
            "module_dir": None,
            "psp_header": None,
            **(extra_args or {}),
        })()
        stderr = io.StringIO()
        with patch.object(nk_cli.subprocess, "run", side_effect=stop_at_codegen), \
             patch.object(nk_cli, "_stage_runtime_assets"), \
             contextlib.redirect_stderr(stderr):
            result = nk_cli.cmd_build_package(args)
        return result, user_root, captured, stderr.getvalue()

    def test_build_package_hands_the_build_speed_to_the_planner(self) -> None:
        # Settings > Build speed reaches the planner, which sizes the Make jobs from
        # NK_PACKAGE_BUILD_SPEED: both the codegen stage's planner run and the package
        # build's (whose environment is the compile copy). Without the hand-off every
        # speed would size as Balanced while the log still named the chosen speed.
        cases: list[tuple[str, str | None, bool]] = [
            ("TEST00091", None, False),
            ("TEST00092", "background", False),
            ("TEST00093", "balanced", False),
            ("TEST00094", "fastest", False),
            ("TEST00095", "background", True),
            ("TEST00096", "fastest", True),
        ]
        speed_env = nk_cli._BUILD_SPEED_ENV
        self.assertEqual(speed_env, "NK_PACKAGE_BUILD_SPEED")
        with patch.dict(os.environ):
            os.environ.pop(speed_env, None)
            for disc_id, speed, staged in cases:
                with self.subTest(speed=speed, codegen_stage=staged):
                    extra: dict[str, object] = {}
                    if speed is not None:
                        extra["build_speed"] = speed
                    if staged:
                        extra["aot_stage_dir"] = self.temp_dir / f"{disc_id}-aot-stage"
                    environments: list[dict[str, str]] = []
                    result, _user_root, command, stderr = self._experimental_module_build(
                        disc_id, {}, extra_args=extra, planner_environments=environments,
                    )
                    self.assertEqual(result, 1)
                    self.assertIn("TEST_STOP_AFTER_MODULE_INTAKE", stderr)
                    self.assertEqual(len(environments), 1)
                    self.assertEqual("--aot-stage" in command, staged)
                    if staged:
                        # A caller's codegen stage (bring-up's codegen-stage/) is where the
                        # code is generated; the per-user stage cache never replaces it.
                        self.assertEqual(
                            command[command.index("--output-dir") + 1],
                            str(extra["aot_stage_dir"]),
                        )
                    self.assertEqual(environments[0].get(speed_env), speed or "balanced")

    def test_experimental_build_declares_usrdir_prx_as_runtime_placed_module(self) -> None:
        # The player's build route (#308): a PRX the game ships in USRDIR is declared as a
        # runtime-placed guest module, so it is translated and its module_start runs when
        # the game loads it (#299). Before, the route kept the profile's empty module list
        # and the build translated the main executable only.
        disc_id = "TEST00071"
        module_bytes = build_plain_mips_elf(e_type=0xFFA0, vaddr=0, memsz=0x1000)
        result, user_root, command, stderr = self._experimental_module_build(
            disc_id, {"libfont.prx": module_bytes},
        )
        self.assertEqual(result, 1)
        self.assertIn("TEST_STOP_AFTER_MODULE_INTAKE", stderr)
        profile = json.loads(
            (user_root / "experimental" / disc_id / "profile.json").read_text(encoding="utf-8")
        )
        declared = [{
            "name": "libfont.prx", "required": True, "role": "guest-prx",
            "placement": "runtime", "guest_path": "disc0:/PSP_GAME/USRDIR/libfont.prx",
        }]
        self.assertEqual(profile["manifest"]["modules"], declared)
        manifest = json.loads(
            (user_root / "cache" / "packages" / disc_id / "manifest.json").read_text(encoding="utf-8")
        )
        self.assertEqual(manifest["modules"], declared)
        module_dir = Path(command[command.index("--module-dir") + 1])
        self.assertEqual((module_dir / "libfont.prx").read_bytes(), module_bytes)
        # The planning copies are not left behind in the profile folder.
        self.assertEqual(
            list((user_root / "experimental" / disc_id).glob("module-stage-*")), []
        )

    def test_experimental_build_uses_the_disc_checks_decrypted_module(self) -> None:
        # An encrypted disc module resolves to the plain copy the player's disc check
        # decrypted into the per-title folder; without one the build stops by name.
        disc_id = "TEST00072"
        plain = build_plain_mips_elf(e_type=0xFFA0, vaddr=0, memsz=0x1000)
        result, user_root, command, stderr = self._experimental_module_build(
            disc_id, {"libfont.prx": build_psp_container()},
            decrypted_modules={"libfont.prx": plain},
        )
        self.assertEqual(result, 1)
        self.assertIn("TEST_STOP_AFTER_MODULE_INTAKE", stderr)
        module_dir = Path(command[command.index("--module-dir") + 1])
        self.assertEqual((module_dir / "libfont.prx").read_bytes(), plain)

        disc_id = "TEST00073"
        result, user_root, command, stderr = self._experimental_module_build(
            disc_id, {"libfont.prx": build_psp_container()},
        )
        self.assertEqual(result, 1)
        self.assertEqual(command, [])
        self.assertIn("still encrypted", stderr)
        self.assertNotIn("TEST_STOP_AFTER_MODULE_INTAKE", stderr)
        profile = json.loads(
            (user_root / "experimental" / disc_id / "profile.json").read_text(encoding="utf-8")
        )
        self.assertEqual(profile["manifest"]["modules"], [])

    def test_experimental_intake_never_turns_a_disc_module_name_into_a_host_path(self) -> None:
        # An ISO 9660 name may be a UNC or absolute path. The intake checks it as a string
        # before forming any path, so it probes, reads and rewrites nothing outside the
        # per-title folder and stops by name instead (final security pass, E1).
        work = self.temp_dir / "module-name-host-path"
        user_root = work / "user"
        user_root.mkdir(parents=True)
        outside = work / "outside"
        outside.mkdir()
        victim = outside / "b.prx"
        victim.write_bytes(build_plain_mips_elf(e_type=0xFFA0, vaddr=0))
        victim_id = os.stat(victim).st_ino
        modules = {
            "PSP_GAME/SYSDIR/EBOOT.BIN": build_plain_mips_elf(),
            "PSP_GAME/USRDIR/" + "\\\\nk-test.invalid\\share\\a.prx": b"\x7fELF-not-read",
        }
        if os.name == "nt":
            # An absolute name only stays one ISO name where the separator is a backslash.
            modules["PSP_GAME/USRDIR/" + str(victim)] = b"~SCE" + bytes(0x60)
        iso_file = work / "game.iso"
        create_test_iso_with_module_tree(iso_file, modules, disc_id="TEST00074")
        selected = work / "selected.elf"
        selected.write_bytes(build_plain_mips_elf())
        candidates = nk_cli._discover_iso_module_candidates(iso_file, "EBOOT.BIN")
        probed: list[str] = []
        real_is_file = Path.is_file

        def recording_is_file(path: Path, *args, **kwargs) -> bool:
            probed.append(str(path))
            if str(path).startswith("\\\\"):
                return False  # never reach the network
            return real_is_file(path, *args, **kwargs)

        with patch.object(Path, "is_file", recording_is_file):
            with self.assertRaises(nk_cli.ExperimentalModuleIntakeError) as ctx:
                nk_cli._declare_experimental_disc_modules(
                    iso_file, user_root, "TEST00074", candidates, selected, {},
                )
        self.assertEqual(ctx.exception.boundary_code, "GUEST_MODULE_FORMAT_UNSUPPORTED")
        self.assertEqual([p for p in probed if "nk-test.invalid" in p or str(outside) in p], [])
        self.assertEqual(os.stat(victim).st_ino, victim_id)
        self.assertEqual([p.name for p in outside.iterdir()], ["b.prx"])

    def test_package_build_public_backends_and_limits(self) -> None:
        disc_id = "TEST00004"
        user_root = self.temp_dir / "user-data-mode"
        user_root.mkdir(parents=True, exist_ok=True)
        iso_file = self.temp_dir / "mode-test.iso"
        eboot_bytes = build_plain_mips_elf()
        create_test_iso_with_executables(iso_file, build_psp_container(), disc_id=disc_id, title="Mode Test Title")

        title_id = "synthetic-title2-v1"
        entry_pub = {
            "disc_id": disc_id,
            "title_id": title_id,
            "iso_path": str(iso_file),
            "selected_executable": "",
            "is_experimental": False,
        }
        (user_root / "library.json").write_text(
            json.dumps({"schema_version": 1, "games": [entry_pub]}),
            encoding="utf-8",
        )
        decrypted_dir = user_root / "titles" / disc_id / "decrypted"
        decrypted_dir.mkdir(parents=True, exist_ok=True)
        (decrypted_dir / "EBOOT.elf").write_bytes(eboot_bytes)

        manifest_path = ROOT / "assets" / "titles" / "synthetic-title2.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

        captured_commands: list[list[str]] = []

        def fake_run(command, **_kwargs):
            if _is_compiler_target_query(command):
                return _REAL_SUBPROCESS_RUN(command, **_kwargs)
            captured_commands.append(list(command))
            build_dir = Path(command[command.index("--output-dir") + 1])
            build_dir.mkdir(parents=True, exist_ok=True)
            executable = build_dir / "synthetic.exe"
            image = build_dir / "synthetic_image.bin"
            generated = build_dir / "synthetic_recomp.o"
            executable.write_bytes(b"fake executable")
            image.write_bytes(b"fake image")
            generated.write_bytes(b"fake object")
            (build_dir / "package.json").write_text(
                json.dumps({
                    "cache": {"format": "nakagawa-aot-cache", "schema_version": 1, "key": {}, "codegen_options": {}, "runtime_abi_compatibility": {"current_epoch": 1, "generated_code_reusable": True}},
                    "title": {"id": title_id},
                    "inputs": {"manifest": {"sha256": "0" * 64}, "executable": {"sha256": hashlib.sha256(eboot_bytes).hexdigest()}, "modules": [], "psp_header": None},
                    "executable": {"path": executable.name, "sha256": hashlib.sha256(b"fake executable").hexdigest()},
                    "generated_objects": [{"path": generated.name, "sha256": hashlib.sha256(b"fake object").hexdigest()}],
                }),
                encoding="utf-8",
            )
            # Like title_codegen_plan.build_package, the report records the backend
            # mode the command line selected (the real report is covered end to end
            # in test_production_smoke).
            public = "--public-safe" in command
            (build_dir / "build-report.json").write_text(
                json.dumps({
                    "cache": {"format": "nakagawa-aot-cache", "schema_version": 1, "key": {}, "codegen_options": {}, "runtime_abi_compatibility": {"current_epoch": 1, "generated_code_reusable": True}},
                    "backends": "public" if public else "private",
                    "limits": [
                        "Fonts: import your own PSP fonts; the public PGF reader is available for supported inputs.",
                        "PGD-protected data: unavailable; broader ISO-to-Play support is in the works.",
                    ] if public else [],
                }),
                encoding="utf-8",
            )
            return subprocess.CompletedProcess(command, 0, "", "")

        args = type("BuildArgs", (), {
            "disc_id": disc_id,
            "user_data_root": user_root,
            "module_dir": None,
            "psp_header": None,
        })()

        captured_commands.clear()
        with patch.object(nk_cli, "_load_entry_manifest", return_value=(manifest_path, manifest, None)), \
             patch.object(nk_cli.subprocess, "run", side_effect=fake_run), \
             patch.object(nk_cli, "_stage_runtime_assets"), \
             patch.object(nk_cli.package_cache, "validate_package_cache", return_value=(True, "")):
            self.assertEqual(nk_cli.cmd_build_package(args), 0)
            self.assertTrue(len(captured_commands) > 0)
            self.assertIn("--public-safe", captured_commands[0])
            # build-report.json here is the fake planner's own output, so reading it
            # back proves nothing; test_production_smoke covers the real report. The
            # completion manifest is written by cmd_build_package itself.
            completion = json.loads((user_root / "packages" / disc_id / "completion-manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(completion.get("backends"), "public")
            self.assertEqual(completion.get("limits"), [
                "Fonts: import your own PSP fonts; the public PGF reader is available for supported inputs.",
                "PGD-protected data: unavailable; broader ISO-to-Play support is in the works.",
            ])


    def test_prx_format_boot_fallback_is_selected_by_both_inspectors(self) -> None:
        """Retail BOOT.BIN is often PSP PRX-format (e_type 0xFFA0), which the
        analyzer and runtime loader accept; the preflight must not call it
        unusable."""
        iso_file = self.temp_dir / "prx-boot-fallback.iso"
        runtime_root = self.temp_dir / "empty-runtime-root-prx"
        runtime_root.mkdir()
        create_test_iso_with_executables(
            iso_file, build_psp_container(), build_plain_mips_elf(0xFFA0)
        )
        report = inspect_compatibility_preflight(
            iso_file, metadata=inspect_iso(iso_file), runtime_root=runtime_root
        )
        self.assertEqual(report["executables"]["BOOT.BIN"]["classification"],
                         "PLAIN_MIPS_ELF32")
        self.assertEqual(report["selected_executable"], "BOOT.BIN")
        native = self._run_native_inspect(iso_file)
        self.assertEqual(native["BOOT_KIND"], "1")
        self.assertEqual(native["SELECTED_EXECUTABLE"], "2")

    def test_second_title_iso_parity(self) -> None:
        """Verify Python and Native C inspector match on second public synthetic disc ID."""
        iso_file = self.temp_dir / "test5_mock.iso"
        create_test_iso(iso_file, disc_id="TEST00005", title="Phase 5 Test", volume_id="TEST00005")

        py_meta = inspect_iso(iso_file)
        c_meta = self._run_native_inspect(iso_file)

        self.assertEqual(c_meta.get("RESULT"), "OK")
        self.assertEqual(c_meta.get("DISC_ID"), "TEST00005")
        self.assertEqual(c_meta.get("SUPPORTED"), "1")
        self.assertEqual(c_meta.get("MATCHED_ID"), "pspdev-phase5-v1")
        self.assertEqual(c_meta.get("MATCHED_ID"), py_meta.matched_profile.id)

    def test_uncataloged_retail_iso_parity(self) -> None:
        """Verify an uncatalogued retail disc ID is uncatalogued in public mode by both inspectors."""
        iso_file = self.temp_dir / "retail_mock.iso"
        create_test_iso(iso_file, disc_id="UCES01402", title="Retail Placeholder", volume_id="UCES01402")

        py_meta = inspect_iso(iso_file)
        c_meta = self._run_native_inspect(iso_file)

        self.assertEqual(c_meta.get("RESULT"), "OK")
        self.assertEqual(c_meta.get("DISC_ID"), "UCES01402")
        self.assertEqual(c_meta.get("SUPPORTED"), "0")
        self.assertEqual(c_meta.get("MATCHED_ID"), "NONE")
        self.assertIsNone(py_meta.matched_profile)

    def test_public_hst_manifest_is_catalogued_by_both_inspectors(self) -> None:
        """UCUS98701 resolves to the built-in hst title in both inspectors (public manifest)."""
        iso_file = self.temp_dir / "hst_mock.iso"
        create_test_iso(iso_file, disc_id="UCUS98701", title="Hot Shots Tennis", volume_id="UCUS98701")

        py_meta = inspect_iso(iso_file)
        c_meta = self._run_native_inspect(iso_file)

        self.assertEqual(c_meta.get("RESULT"), "OK")
        self.assertEqual(c_meta.get("DISC_ID"), "UCUS98701")
        self.assertEqual(c_meta.get("SUPPORTED"), "1")
        self.assertEqual(c_meta.get("MATCHED_ID"), "hst-ucus98701-v1")
        self.assertIsNotNone(py_meta.matched_profile)
        self.assertEqual(py_meta.matched_profile.id, "hst-ucus98701-v1")
        self.assertEqual(py_meta.matched_profile.codegen_profile, "hst")

    def test_uncatalogued_param_sfo_disc_is_experimental(self) -> None:
        """Unknown PSP discs get validated local identity profiles and stay launch-gated."""
        iso_file = self.temp_dir / "experimental_mock.iso"
        runtime_root = self.temp_dir / "empty-runtime-root"
        runtime_root.mkdir()
        executable_bytes = build_plain_mips_elf() + bytes((i % 251 for i in range(70000)))
        create_test_iso_with_executables(
            iso_file, executable_bytes, disc_id="ULUS99998",
            title="Experimental Fixture",
        )

        metadata = inspect_iso(iso_file)
        self.assertIsNone(metadata.matched_profile)
        preflight = inspect_compatibility_preflight(
            iso_file, metadata=metadata, runtime_root=runtime_root
        )
        self.assertTrue(preflight["is_experimental"])
        by_code = {check["code"]: check for check in preflight["checks"]}
        self.assertEqual(by_code["EXPERIMENTAL"]["issues"], [308])
        self.assertIn("Compatibility is unknown", by_code["EXPERIMENTAL"]["message"])
        self.assertEqual(by_code["RUNTIME_PACKAGE"]["status"], "MISSING")
        self.assertEqual(by_code["RUNTIME_PACKAGE"]["issues"], [308])

        python_root = self.temp_dir / "python-user-data"
        python_profile_path = write_experimental_profile(
            iso_file, python_root, metadata=metadata
        )
        python_profile = json.loads(python_profile_path.read_text(encoding="utf-8"))
        from title_manifest import validate_manifest
        manifest = validate_manifest(python_profile["manifest"])
        identity = python_profile["input_identity"]
        expected_hash = hashlib.sha256(executable_bytes).hexdigest()
        self.assertEqual(manifest["disc"]["id"], "ULUS99998")
        self.assertEqual(manifest["display_name"], "Experimental Fixture")
        self.assertEqual(manifest["executable"]["base"], 0x08800000)
        self.assertEqual(manifest["executable"]["entry"], 0x08800000)
        self.assertEqual(manifest["codegen_profile"], "none")
        self.assertEqual(manifest["modules"], [])
        self.assertEqual(identity["selected_executable"], "PSP_GAME/SYSDIR/EBOOT.BIN")
        self.assertEqual(identity["executable_sha256"], expected_hash)
        self.assertEqual(identity["elf_sha256"], expected_hash)
        self.assertTrue(python_profile_path.is_relative_to(python_root))

        native_root = self.temp_dir / "native-user-data"
        native = subprocess.run(
            [str(self.exe_path), "experimental_profile", str(iso_file), str(native_root)],
            capture_output=True, text=True,
        )
        self.assertEqual(native.returncode, 0, native.stderr)
        self.assertIn("PROFILE_RESULT:OK", native.stdout)
        self.assertIn("PACKAGE_AVAILABLE:0", native.stdout)
        self.assertIn("LAUNCH_PREPARE:REFUSED", native.stdout)
        native_profile_path = native_root / "experimental" / "ULUS99998" / "profile.json"
        native_profile = json.loads(native_profile_path.read_text(encoding="utf-8"))
        native_manifest = validate_manifest(native_profile["manifest"])
        self.assertEqual(native_manifest["id"], "experimental-ulus99998")
        self.assertEqual(native_profile["input_identity"]["executable_sha256"], expected_hash)
        self.assertEqual(native_profile["input_identity"]["elf_sha256"], expected_hash)

    def test_experimental_import_uses_documented_load_binding_for_relocatable_elf(self) -> None:
        """Relocatable main modules use the documented generic PSP load base."""
        from title_manifest import validate_manifest

        for e_type in (3, 0xFFA0):
            with self.subTest(e_type=e_type):
                iso_file = self.temp_dir / f"relocatable_{e_type:04x}.iso"
                create_test_iso_with_executables(
                    iso_file, build_plain_mips_elf(e_type=e_type),
                    disc_id="ULUS99997", title="Synthetic Relocatable Fixture",
                )
                user_root = self.temp_dir / f"relocatable-user-data-{e_type:04x}"
                metadata = inspect_iso(iso_file)
                profile_path = write_experimental_profile(
                    iso_file, user_root, metadata=metadata
                )
                profile = json.loads(profile_path.read_text(encoding="utf-8"))
                executable = validate_manifest(profile["manifest"])["executable"]
                self.assertEqual(executable["base"], 0x08804000)
                self.assertEqual(executable["load_address"], 0x08804000)
                self.assertEqual(
                    executable["load_address_evidence"], "documented-psp-default"
                )
                self.assertTrue(profile_path.is_relative_to(user_root))

    def test_experimental_fixed_address_exec_uses_lowest_load_as_image_base(self) -> None:
        """A fixed-address ET_EXEC already carries its guest address in PT_LOAD."""
        from title_manifest import validate_manifest

        base = 0x08804000
        iso_file = self.temp_dir / "fixed-address-main.iso"
        create_test_iso_with_executables(
            iso_file,
            build_plain_mips_elf(e_type=2, vaddr=base, memsz=0x2000, entry=base),
            disc_id="ULUS99996",
            title="Synthetic Fixed Address Fixture",
        )
        profile_path = write_experimental_profile(
            iso_file, self.temp_dir / "fixed-address-user-data"
        )
        profile = json.loads(profile_path.read_text(encoding="utf-8"))
        executable = validate_manifest(profile["manifest"])["executable"]
        self.assertEqual(executable["base"], base)
        self.assertEqual(executable["entry"], base)
        self.assertNotIn("load_address", executable)
        self.assertNotIn("load_address_evidence", executable)

    def test_experimental_fixed_address_exec_outside_user_ram_fails_closed(self) -> None:
        address = iso_inspect.PSP_CONVENTIONAL_USER_MEMORY_TOP - 4
        iso_file = self.temp_dir / "fixed-address-outside-user-ram.iso"
        create_test_iso_with_executables(
            iso_file,
            build_plain_mips_elf(e_type=2, vaddr=address, memsz=8, entry=address),
            disc_id="ULUS99995",
            title="Synthetic Out of Range Fixture",
        )
        user_root = self.temp_dir / "fixed-address-outside-user-ram-data"
        with self.assertRaises(IsoInspectionError) as caught:
            write_experimental_profile(iso_file, user_root)
        self.assertEqual(
            caught.exception.boundary_code,
            "FIXED_ADDRESS_EXECUTABLE_OUTSIDE_GUEST_RAM",
        )
        self.assertEqual(
            str(caught.exception),
            "This fixed-address executable loads outside guest user RAM. "
            "Building it there is not supported yet.",
        )
        self.assertFalse((user_root / "experimental").exists())

    def test_experimental_zero_based_exec_requires_a_load_binding(self) -> None:
        iso_file = self.temp_dir / "zero-based-exec.iso"
        create_test_iso_with_executables(
            iso_file,
            build_plain_mips_elf(e_type=2, vaddr=0, memsz=4, entry=0),
            disc_id="ULUS99994",
            title="Synthetic Unbound Executable Fixture",
        )
        with self.assertRaises(IsoInspectionError) as caught:
            write_experimental_profile(
                iso_file, self.temp_dir / "zero-based-exec-user-data"
            )
        self.assertEqual(
            caught.exception.boundary_code, "EXECUTABLE_LOAD_BINDING_REQUIRED"
        )
        self.assertEqual(
            str(caught.exception),
            "This ET_EXEC has no guest RAM load address. Generic rebinding for this "
            "executable type is not supported yet.",
        )

    def test_guest_modules_are_runtime_placed_and_listed_deterministically(self) -> None:
        main_elf = self.temp_dir / "placement-main.elf"
        alpha = self.temp_dir / "alpha.prx"
        beta = self.temp_dir / "beta.prx"
        main_elf.write_bytes(build_plain_mips_elf(e_type=2, memsz=0x40000))
        alpha.write_bytes(build_plain_mips_elf(e_type=0xFFA0, vaddr=0, memsz=0x21001))
        beta.write_bytes(build_plain_mips_elf(e_type=3, vaddr=0, memsz=0x17001))
        inputs = [
            ("beta.prx", beta, "disc0:/PSP_GAME/USRDIR/beta.prx"),
            ("alpha.prx", alpha, "disc0:/PSP_GAME/SYSDIR/alpha.prx"),
        ]

        first = plan_guest_module_bindings(main_elf, inputs)
        second = plan_guest_module_bindings(main_elf, list(reversed(inputs)))
        self.assertEqual(first, second)
        self.assertEqual(first, [
            {"name": "alpha.prx", "required": True, "role": "guest-prx",
             "placement": "runtime", "guest_path": "disc0:/PSP_GAME/SYSDIR/alpha.prx"},
            {"name": "beta.prx", "required": True, "role": "guest-prx",
             "placement": "runtime", "guest_path": "disc0:/PSP_GAME/USRDIR/beta.prx"},
        ])
        # The manifest validator accepts the plan exactly as written.
        from title_manifest import validate_manifest
        manifest = json.loads((ROOT / "assets" / "titles" / "synthetic.json").read_text(encoding="utf-8"))
        manifest["modules"] = first
        self.assertEqual(validate_manifest(manifest)["modules"], first)

    def test_modules_that_never_fit_together_are_still_planned(self) -> None:
        # The guest allocator places each module when it is loaded, so no concurrent
        # fit and no build-time reserve is required: two 16 MiB modules beside a main
        # image that leaves little user memory are both planned (#704).
        main_elf = self.temp_dir / "placement-full-main.elf"
        huge_a = self.temp_dir / "huge-a.prx"
        huge_b = self.temp_dir / "huge-b.prx"
        main_elf.write_bytes(build_plain_mips_elf(e_type=2, vaddr=0x09E00000, memsz=0x1F0000))
        huge_a.write_bytes(build_plain_mips_elf(e_type=0xFFA0, vaddr=0, memsz=0x01000000))
        huge_b.write_bytes(build_plain_mips_elf(e_type=0xFFA0, vaddr=0, memsz=0x01000000))
        planned = plan_guest_module_bindings(
            main_elf,
            [("a.prx", huge_a, "disc0:/PSP_GAME/USRDIR/a.prx"),
             ("b.prx", huge_b, "disc0:/PSP_GAME/USRDIR/b.prx")],
        )
        self.assertEqual([module["name"] for module in planned], ["a.prx", "b.prx"])
        self.assertTrue(all(module["placement"] == "runtime" for module in planned))

    def test_guest_module_planning_refuses_what_cannot_load(self) -> None:
        main_elf = self.temp_dir / "placement-main-checks.elf"
        main_elf.write_bytes(build_plain_mips_elf(e_type=2, vaddr=0x08804000, memsz=0x40000))
        module = self.temp_dir / "module.prx"
        cases = (
            ("a main image past user memory",
             build_plain_mips_elf(e_type=2, vaddr=0x09FF0000, memsz=0x20000), None,
             "exceeds the conventional user-memory ceiling",
             "GUEST_MODULE_LOAD_ADDRESS_LAYOUT_UNAVAILABLE"),
            # Fixed-address modules carry a real SceModuleInfo: the planner reads every
            # module's export table first (#771), and a plain ELF whose p_paddr is a
            # load address is a malformed module to that reader. An executable on the
            # main image is deferred by name, not refused (checked below).
            ("a fixed-address module past user memory", None,
             build_module_elf([SYSLIB_EXPORT], e_type=2, base_vaddr=0x09FFFFF0),
             "fixed load address 0x09fffff0 that collides with layout",
             "GUEST_MODULE_LOAD_BINDING_REQUIRED"),
            ("an image that is not a PSP module", None,
             build_plain_mips_elf(e_type=4, vaddr=0, memsz=0x100),
             "not a supported ELF/PRX", "GUEST_MODULE_FORMAT_UNSUPPORTED"),
        )
        for label, main_bytes, module_bytes, expected, boundary in cases:
            with self.subTest(case=label):
                main = main_elf
                if main_bytes is not None:
                    main = self.temp_dir / "placement-oversized-main.elf"
                    main.write_bytes(main_bytes)
                module.write_bytes(module_bytes if module_bytes is not None else
                                   build_plain_mips_elf(e_type=0xFFA0, vaddr=0, memsz=0x100))
                with self.assertRaisesRegex(IsoInspectionError, expected) as ctx:
                    plan_guest_module_bindings(
                        main, [("module.prx", module, "disc0:/PSP_GAME/USRDIR/module.prx")])
                self.assertEqual(ctx.exception.boundary_code, boundary)
        fixed = self.temp_dir / "fixed.prx"
        fixed.write_bytes(build_module_elf([SYSLIB_EXPORT], e_type=2, base_vaddr=0x09000000))
        planned = plan_guest_module_bindings(
            main_elf, [("fixed.prx", fixed, "disc0:/PSP_GAME/USRDIR/fixed.prx")])
        self.assertEqual(planned[0]["placement"], "runtime")
        # Two fixed-address modules may share addresses: only modules loaded at the
        # same time compete for memory, and the allocator decides that at load.
        twin = self.temp_dir / "twin.prx"
        twin.write_bytes(build_module_elf([SYSLIB_EXPORT], e_type=2, base_vaddr=0x09000020))
        planned = plan_guest_module_bindings(
            main_elf, [("fixed.prx", fixed, "disc0:/fixed.prx"), ("twin.prx", twin, "disc0:/twin.prx")])
        self.assertEqual([module["name"] for module in planned], ["fixed.prx", "twin.prx"])
        with self.assertRaisesRegex(IsoInspectionError, "collide") as ctx:
            plan_guest_module_bindings(
                main_elf, [("A.prx", fixed, "disc0:/a"), ("a.PRX", fixed, "disc0:/b")])
        self.assertEqual(ctx.exception.boundary_code, "GUEST_MODULE_FORMAT_UNSUPPORTED")
        # An executable on the main image is a separate program: deferred by name.
        over_main = self.temp_dir / "over-main.prx"
        over_main.write_bytes(build_module_elf([SYSLIB_EXPORT], e_type=2, base_vaddr=0x08820000))
        declared, deferred = plan_guest_module_layout(
            main_elf, [("over-main.prx", over_main, "disc0:/over-main.prx")])
        self.assertEqual(declared, [])
        self.assertEqual(deferred, [{
            "name": "over-main.prx",
            "reason": "separate executable on the main image base, not pre-placed",
        }])

    def test_guest_module_planning_with_a_main_image_that_leaves_little_memory(self) -> None:
        # A relocatable main image ending at 0x09EF4000 left no room for the former
        # fixed layout; with on-demand placement its module is still planned (the
        # allocator answers at load time), and only a main image past user memory
        # stops planning with the layout boundary.
        main_elf = self.temp_dir / "placement-huge-main.elf"
        module_a = self.temp_dir / "mod-a.prx"
        main_elf.write_bytes(build_plain_mips_elf(e_type=0xFFA0, vaddr=0, memsz=0x016F0000))
        module_a.write_bytes(build_plain_mips_elf(e_type=0xFFA0, vaddr=0, memsz=0x1000))
        planned = plan_guest_module_bindings(
            main_elf,
            [("a.prx", module_a, "disc0:/PSP_GAME/USRDIR/a.prx")],
        )
        self.assertEqual([(m["name"], m["placement"]) for m in planned], [("a.prx", "runtime")])
        main_elf.write_bytes(build_plain_mips_elf(e_type=0xFFA0, vaddr=0, memsz=0x01800000))
        with self.assertRaises(IsoInspectionError) as ctx:
            plan_guest_module_bindings(
                main_elf,
                [("a.prx", module_a, "disc0:/PSP_GAME/USRDIR/a.prx")],
            )
        self.assertEqual(ctx.exception.boundary_code, "GUEST_MODULE_LOAD_ADDRESS_LAYOUT_UNAVAILABLE")
        self.assertIn("exceeds the conventional user-memory ceiling", str(ctx.exception))

    def test_provisional_guest_module_placement_encrypted_module_raises_decryption_required(self) -> None:
        main_elf = self.temp_dir / "placement-main-enc.elf"
        enc_module = self.temp_dir / "enc.prx"
        main_elf.write_bytes(build_plain_mips_elf(e_type=2, vaddr=0x08800000, memsz=0x40000))
        enc_module.write_bytes(build_psp_container())
        with self.assertRaises(IsoInspectionError) as ctx:
            plan_guest_module_bindings(
                main_elf,
                [("enc.prx", enc_module, "disc0:/PSP_GAME/USRDIR/enc.prx")],
            )
        self.assertEqual(ctx.exception.boundary_code, "GUEST_MODULE_DECRYPTION_REQUIRED")

    def test_provisional_guest_module_placement_overlapping_segments_raises_format_unsupported(self) -> None:
        main_elf = self.temp_dir / "placement-main-ovl.elf"
        ovl_module = self.temp_dir / "ovl.prx"
        main_elf.write_bytes(build_plain_mips_elf(e_type=2, vaddr=0x08800000, memsz=0x40000))
        ovl_module.write_bytes(build_overlapping_mips_elf())
        with self.assertRaises(IsoInspectionError) as ctx:
            plan_guest_module_bindings(
                main_elf,
                [("ovl.prx", ovl_module, "disc0:/PSP_GAME/USRDIR/ovl.prx")],
            )
        self.assertEqual(ctx.exception.boundary_code, "GUEST_MODULE_FORMAT_UNSUPPORTED")
        self.assertIn("overlapping loadable segments", str(ctx.exception))

    def test_provisional_guest_module_placement_fixed_address_module_support_and_collision(self) -> None:
        main_elf = self.temp_dir / "placement-main-fixed.elf"
        fixed_ok = self.temp_dir / "fixed_ok.prx"
        fixed_bad = self.temp_dir / "fixed_bad.prx"
        main_elf.write_bytes(build_plain_mips_elf(e_type=2, vaddr=0x08800000, memsz=0x40000))
        fixed_ok.write_bytes(build_module_elf([SYSLIB_EXPORT], e_type=2, base_vaddr=0x09000000))
        placed = plan_guest_module_bindings(
            main_elf,
            [("fixed_ok.prx", fixed_ok, "disc0:/PSP_GAME/USRDIR/fixed_ok.prx")],
        )
        # A fixed-address module is translated for its link addresses and declared
        # runtime-placed: the allocator reserves exactly its link range at load.
        self.assertEqual(placed, [{
            "name": "fixed_ok.prx", "required": True, "role": "guest-prx",
            "placement": "runtime", "guest_path": "disc0:/PSP_GAME/USRDIR/fixed_ok.prx",
        }])

        # An executable on the main image's base is a separate program: it is deferred by
        # name, not pre-placed beside main and not a stop.
        fixed_bad.write_bytes(build_module_elf([SYSLIB_EXPORT], e_type=2, base_vaddr=0x08810000))
        declared, deferred = plan_guest_module_layout(
            main_elf,
            [("fixed_bad.prx", fixed_bad, "disc0:/PSP_GAME/USRDIR/fixed_bad.prx")],
        )
        self.assertEqual(declared, [])
        self.assertEqual(deferred, [{
            "name": "fixed_bad.prx",
            "reason": "separate executable on the main image base, not pre-placed",
        }])

    def test_byte_identical_copy_of_the_main_executable_is_a_recorded_duplicate(self) -> None:
        main_elf = self.temp_dir / "duplicate-main.elf"
        main_elf.write_bytes(build_plain_mips_elf(e_type=2, vaddr=0x08800000, memsz=0x40000))
        copy = self.temp_dir / "duplicate_copy.elf"
        copy.write_bytes(main_elf.read_bytes())
        declared, deferred = plan_guest_module_layout(
            main_elf,
            [("duplicate_copy.elf", copy, "disc0:/PSP_GAME/USRDIR/duplicate_copy.elf")],
        )
        self.assertEqual(declared, [])
        self.assertEqual(deferred, [{
            "name": "duplicate_copy.elf",
            "reason": "duplicate of the main executable, not loaded as a module",
        }])

    def test_codegen_step_keeps_its_output_on_disk(self) -> None:
        log = self.temp_dir / "bringup-codegen.log"
        code = (
            "import sys; print('codegen-stdout-line'); "
            "print('codegen-stderr-line', file=sys.stderr); sys.exit(3)"
        )
        returncode = nk_cli._run_codegen_step(
            [sys.executable, "-c", code], cwd=self.temp_dir, env=dict(os.environ),
            log_path=log,
        )
        self.assertEqual(returncode, 3)
        text = log.read_text(encoding="utf-8")
        self.assertIn("exit 3", text)
        self.assertIn("codegen-stdout-line", text)
        self.assertIn("codegen-stderr-line", text)

    # Synthetic NIDs for the exact HLE-served rule. The registry is injected so
    # these cases pin the rule itself, independent of what src/rt/hle.c serves.
    SERVED_A, SERVED_B, SERVED_C = 0x5EED0001, 0x5EED0002, 0x5EED0003
    UNSERVED_A, UNSERVED_B, UNSERVED_C = 0x0BAD0001, 0x0BAD0002, 0x0BAD0003

    def _plan_with_registry(self, main_elf: Path, modules: dict[str, bytes], registered=None):
        inputs = []
        for name, blob in modules.items():
            path = self.temp_dir / name
            path.write_bytes(blob)
            inputs.append((name, path, f"disc0:/PSP_GAME/USRDIR/{name}"))
        registry = frozenset(
            {self.SERVED_A, self.SERVED_B, self.SERVED_C} if registered is None else registered
        )
        with patch.object(iso_inspect, "runtime_registered_nids", return_value=registry):
            return plan_guest_module_bindings(main_elf, inputs)

    def _main_importing(self, imports: list[tuple[str, list[int]]]) -> Path:
        main_elf = self.temp_dir / "placement-main-imports.elf"
        main_elf.write_bytes(
            build_module_elf([SYSLIB_EXPORT], imports=imports, e_type=2, base_vaddr=BASE_VADDR)
        )
        return main_elf

    def test_module_the_runtime_serves_completely_is_not_placed(self) -> None:
        main_elf = self.temp_dir / "placement-main-served.elf"
        main_elf.write_bytes(build_plain_mips_elf(e_type=2, vaddr=0x08800000, memsz=0x40000))
        placed = self._plan_with_registry(main_elf, {
            # user-mode library: every exported function registered
            "served_user.prx": build_module_elf(
                [SYSLIB_EXPORT, ("SynthAudio", 0x0001, [self.SERVED_A, self.SERVED_B], [])]
            ),
            # kernel-mode driver: its syscall-exported library is fully registered;
            # the kernel-only library is not callable from user mode
            "served_driver.prx": build_module_elf(
                [
                    SYSLIB_EXPORT,
                    ("SynthCodec", 0x4001, [self.SERVED_C], []),
                    ("SynthCodec_driver", 0x0001, [self.UNSERVED_A], []),
                ],
                module_attributes=0x1006,
            ),
            # one callable function the runtime lacks keeps the module translated
            "partial_user.prx": build_module_elf(
                [SYSLIB_EXPORT, ("SynthVideo", 0x0001, [self.SERVED_A, self.UNSERVED_B], [])]
            ),
            # the runtime registers functions only, so an exported variable keeps it
            "variable_user.prx": build_module_elf(
                [SYSLIB_EXPORT, ("SynthData", 0x0001, [self.SERVED_B], [self.UNSERVED_C])]
            ),
            # an overlay that only runs from module_start exports nothing callable
            "overlay.prx": build_module_elf([SYSLIB_EXPORT]),
        })
        self.assertEqual(
            [module["name"] for module in placed],
            ["overlay.prx", "partial_user.prx", "variable_user.prx"],
        )

    def test_firmware_kernel_module_the_main_executable_does_not_need_is_left_out(self) -> None:
        """A kernel driver used only by a firmware library never blocks the title."""
        main_elf = self._main_importing([("SynthVideo", [self.SERVED_A])])
        placed = self._plan_with_registry(main_elf, {
            "video_library.prx": build_module_elf(
                [SYSLIB_EXPORT, ("SynthVideo", 0x0001, [self.SERVED_A, self.UNSERVED_B], [])],
                imports=[("SynthVideoDriver", [self.UNSERVED_A])],
            ),
            "video_driver.prx": build_module_elf(
                [
                    SYSLIB_EXPORT,
                    ("SynthVideoDriver", 0x4001, [self.UNSERVED_A, self.UNSERVED_C], []),
                    ("SynthVideoDriver_driver", 0x0001, [0x0BAD0010], []),
                ],
                module_attributes=0x1006,
            ),
            "lifecycle_only_driver.prx": build_module_elf([SYSLIB_EXPORT], module_attributes=0x1006),
        })
        self.assertEqual([module["name"] for module in placed], ["video_library.prx"])

    def test_kernel_module_the_main_executable_needs_raises_format_unsupported(self) -> None:
        main_elf = self._main_importing(
            [("SynthDriver", [self.UNSERVED_A]), ("SynthAudio", [self.SERVED_A])]
        )
        driver = build_module_elf(
            [SYSLIB_EXPORT, ("SynthDriver", 0x4001, [self.UNSERVED_A, self.UNSERVED_B], [])],
            module_attributes=0x1006,
        )
        with self.assertRaises(IsoInspectionError) as ctx:
            self._plan_with_registry(main_elf, {"title_driver.prx": driver})
        self.assertEqual(ctx.exception.boundary_code, "GUEST_MODULE_FORMAT_UNSUPPORTED")
        self.assertIn("requires PSP kernel mode: title_driver.prx", str(ctx.exception))
        self.assertIn(f"0x{self.UNSERVED_A:08x}", str(ctx.exception))
        self.assertNotIn(f"0x{self.UNSERVED_B:08x}", str(ctx.exception))

    def test_kernel_module_import_is_not_a_need_when_something_else_provides_it(self) -> None:
        main_elf = self._main_importing([("SynthDriver", [self.UNSERVED_A])])
        driver = build_module_elf(
            [SYSLIB_EXPORT, ("SynthDriver", 0x4001, [self.UNSERVED_A], [])],
            module_attributes=0x1006,
        )
        # the runtime registers the imported NID
        placed = self._plan_with_registry(
            main_elf, {"driver.prx": driver},
            registered={self.SERVED_A, self.UNSERVED_A},
        )
        self.assertEqual(placed, [])
        # a placed user-mode module exports the imported NID
        placed = self._plan_with_registry(main_elf, {
            "driver.prx": driver,
            "user_impl.prx": build_module_elf(
                [SYSLIB_EXPORT, ("SynthDriver", 0x0001, [self.UNSERVED_A], [])]
            ),
        })
        self.assertEqual([module["name"] for module in placed], ["user_impl.prx"])
        # the import names a kernel-only library, which user code cannot link to
        kernel_only = build_module_elf(
            [SYSLIB_EXPORT, ("SynthDriver", 0x0001, [self.UNSERVED_A], [])],
            module_attributes=0x1006,
        )
        self.assertEqual(self._plan_with_registry(main_elf, {"driver.prx": kernel_only}), [])

    def test_unreadable_main_import_table_names_the_analyzer_boundary(self) -> None:
        blob = bytearray(
            build_module_elf(
                [SYSLIB_EXPORT], imports=[("SynthDriver", [self.UNSERVED_A])],
                e_type=2, base_vaddr=BASE_VADDR,
            )
        )
        data_off = 0x1000  # import_fixtures.DATA_FILE_OFF: the segment's file offset
        stub_top = struct.unpack_from("<I", blob, data_off + 44)[0]
        struct.pack_into("<I", blob, data_off + (stub_top - BASE_VADDR) + 12, 0)  # nidData = 0
        main_elf = self.temp_dir / "placement-main-bad-imports.elf"
        main_elf.write_bytes(bytes(blob))
        driver = build_module_elf(
            [SYSLIB_EXPORT, ("SynthDriver", 0x4001, [self.UNSERVED_A], [])],
            module_attributes=0x1006,
        )
        with self.assertRaises(IsoInspectionError) as ctx:
            self._plan_with_registry(main_elf, {"driver.prx": driver})
        self.assertEqual(ctx.exception.boundary_code, "ANALYZER_IMPORT_NID_TABLE_MISSING")

    def test_malformed_export_table_raises_format_unsupported(self) -> None:
        main_elf = self.temp_dir / "placement-main-bad-exports.elf"
        main_elf.write_bytes(build_plain_mips_elf(e_type=2, vaddr=0x08800000, memsz=0x40000))
        broken = build_module_elf(
            [SYSLIB_EXPORT, ("SynthAudio", 0x0001, [self.SERVED_A], [])], corrupt="entry_overrun"
        )
        with self.assertRaises(IsoInspectionError) as ctx:
            self._plan_with_registry(main_elf, {"broken.prx": broken})
        self.assertEqual(ctx.exception.boundary_code, "GUEST_MODULE_FORMAT_UNSUPPORTED")
        self.assertIn("invalid module or export table: broken.prx", str(ctx.exception))

    def test_runtime_registry_failure_stops_planning_with_a_named_boundary(self) -> None:
        runtime_registered_nids.cache_clear()
        self.addCleanup(runtime_registered_nids.cache_clear)
        main_elf = self.temp_dir / "placement-main-registry.elf"
        main_elf.write_bytes(build_plain_mips_elf(e_type=2, vaddr=0x08800000, memsz=0x40000))
        library = self.temp_dir / "library.prx"
        library.write_bytes(
            build_module_elf([SYSLIB_EXPORT, ("SynthAudio", 0x0001, [self.SERVED_A], [])])
        )
        overlay = self.temp_dir / "overlay.prx"
        overlay.write_bytes(build_module_elf([SYSLIB_EXPORT]))
        failures = (
            hle_manifest.ManifestError("registration the extractor cannot account for"),
            OSError("hle.c is missing"),
        )
        for failure in failures:
            with self.subTest(failure=type(failure).__name__):
                with patch.object(hle_manifest, "registered_nids", side_effect=failure):
                    with self.assertRaises(RuntimeRegistryUnavailableError) as ctx:
                        plan_guest_module_bindings(
                            main_elf, [("library.prx", library, "disc0:/library.prx")]
                        )
                    self.assertEqual(
                        ctx.exception.boundary_code, "RUNTIME_HLE_REGISTRY_UNAVAILABLE"
                    )
                    self.assertIn(str(failure), str(ctx.exception))
                    # A module with nothing to compare never consults the registry.
                    placed = plan_guest_module_bindings(
                        main_elf, [("overlay.prx", overlay, "disc0:/overlay.prx")]
                    )
                    self.assertEqual([module["name"] for module in placed], ["overlay.prx"])

    def test_live_runtime_registry_drives_the_served_decision(self) -> None:
        live = runtime_registered_nids()
        self.assertEqual(live, hle_manifest.registered_nids())
        some_registered = sorted(live)[:3]
        unregistered = next(nid for nid in range(0x0BAD0000, 0x0BAE0000) if nid not in live)
        served = read_guest_module_interface(
            "driver.prx",
            build_module_elf(
                [SYSLIB_EXPORT, ("SynthCodec", 0x4001, some_registered, [])],
                module_attributes=0x1006,
            ),
        )
        self.assertTrue(served.requires_kernel)
        self.assertTrue(runtime_serves_module(served, live))
        partial = read_guest_module_interface(
            "driver.prx",
            build_module_elf(
                [SYSLIB_EXPORT, ("SynthCodec", 0x4001, [*some_registered, unregistered], [])],
                module_attributes=0x1006,
            ),
        )
        self.assertFalse(runtime_serves_module(partial, live))

    def test_provisional_guest_module_placement_corrupt_elf_header_raises_format_unsupported(self) -> None:
        main_elf = self.temp_dir / "placement-main-corrupt.elf"
        corrupt_module = self.temp_dir / "corrupt.prx"
        main_elf.write_bytes(build_plain_mips_elf(e_type=2, vaddr=0x08800000, memsz=0x40000))
        corrupt_module.write_bytes(b"\x7fELF\x01\x01\x01\x00" + b"\x00" * 30)
        with self.assertRaises(IsoInspectionError) as ctx:
            plan_guest_module_bindings(
                main_elf,
                [("corrupt.prx", corrupt_module, "disc0:/PSP_GAME/USRDIR/corrupt.prx")],
            )
        self.assertEqual(ctx.exception.boundary_code, "GUEST_MODULE_FORMAT_UNSUPPORTED")

    def test_non_psp_iso_without_directory_reachable_sfo_is_refused(self) -> None:
        """An embedded but unreferenced PARAM.SFO does not authorize experimental import."""
        iso_file = self.temp_dir / "non-psp.iso"
        create_test_iso(iso_file, disc_id="ULUS99997", title="Orphaned SFO")
        data = bytearray(iso_file.read_bytes())
        root_off = 33 * 2048
        original_root = bytes(data[root_off:root_off + 2048])
        data[root_off:root_off + 2048] = bytes(2048)
        # Keep only the ISO root's self and parent records, removing PSP_GAME.
        data[root_off:root_off + 68] = original_root[:68]
        iso_file.write_bytes(data)

        metadata = inspect_iso(iso_file)
        self.assertEqual(metadata.disc_id, "UNKNOWN")
        self.assertEqual(metadata.title, "Unknown PSP Title")
        runtime_root = self.temp_dir / "empty-runtime-root-non-psp"
        runtime_root.mkdir()
        report = inspect_compatibility_preflight(
            iso_file, metadata=metadata, runtime_root=runtime_root
        )
        self.assertFalse(report["is_experimental"])
        self.assertEqual(report["checks"][0]["code"], "DISC_SFO")
        with self.assertRaisesRegex(ValueError, "requires PARAM.SFO"):
            write_experimental_profile(iso_file, self.temp_dir / "refused-user-data",
                                       metadata=metadata)

        native_root = self.temp_dir / "native-refused-user-data"
        native = subprocess.run(
            [str(self.exe_path), "experimental_profile", str(iso_file), str(native_root)],
            capture_output=True, text=True,
        )
        self.assertEqual(native.returncode, 0, native.stderr)
        self.assertIn("PROFILE_RESULT:ERROR:Experimental profiles require", native.stdout)
        self.assertFalse((native_root / "experimental").exists())

    def test_pbp_package_is_named_identically_in_both_tiers(self) -> None:
        """A PBP package is refused by boundary code and one plain sentence."""
        package = self.temp_dir / "store-package.iso"
        build_pbp_package(package, disc_id="TEST00424", title="Store Package")

        with self.assertRaises(IsoInspectionError) as caught:
            inspect_iso(package)
        message = str(caught.exception)
        self.assertEqual(caught.exception.boundary_code, "PBP_PACKAGE_UNSUPPORTED")
        self.assertEqual(
            message,
            "This is a PlayStation Store package (PBP), not a disc image. "
            "The player converts PSN UMD games. Title: Store Package (ID: TEST00424)",
        )
        self.assertNotRegex(message, r"#\d+")
        self.assertNotIn("ISO9660", message)

        native = self._run_native_inspect(package)
        self.assertEqual(native.get("BOUNDARY"), caught.exception.boundary_code)
        self.assertEqual(native.get("MESSAGE"), message)
        self.assertTrue(native.get("RESULT", "").startswith("ERROR:-3:"), native)

    def test_pbp_hostile_packages_fail_closed_in_both_tiers(self) -> None:
        """Every hostile package gets the same boundary code and sentence in both tiers."""
        oversized_sfo = build_param_sfo("TEST00424", "Oversized") + bytes(70000)
        past_eof = 40 + len(build_param_sfo("TEST00424", "Past EOF"))
        cases: list[tuple[str, dict]] = [
            ("header-truncated", {"header": b"\x00PBP\x00\x00\x01\x00", "sfo_bytes": b""}),
            ("offsets-descending", {"offsets": [40, 16, 40, 40, 40, 40, 40, 40]}),
            ("offset-past-eof", {"offsets": [40, past_eof, past_eof, past_eof,
                                             past_eof, past_eof, past_eof, past_eof + 64]}),
            ("empty-sfo", {"offsets": [40] * 8, "sfo_bytes": b""}),
            ("sfo-bad-magic", {"sfo_bytes": bytes(32)}),
            ("sfo-oversized", {"sfo_bytes": oversized_sfo}),
            ("disc-id-invalid", {"disc_id": "NODISCID"}),
            ("title-absent", {"sfo_bytes": build_custom_param_sfo(
                [("DISC_ID", 0x0204, b"TEST00424\0")])}),
            ("title-controls", {"title": "Line\tBreak\nTitle"}),
        ]
        for name, kwargs in cases:
            with self.subTest(name=name):
                package = self.temp_dir / f"{name}.iso"
                build_pbp_package(package, **kwargs)
                with self.assertRaises(IsoInspectionError) as caught:
                    inspect_iso(package)
                message = str(caught.exception)
                self.assertIsNotNone(caught.exception.boundary_code)
                self.assertRegex(caught.exception.boundary_code or "", r"^PBP_[A-Z_]+$")
                self.assertNotRegex(message, r"#\d+")
                native = self._run_native_inspect(package)
                self.assertEqual(
                    native.get("BOUNDARY"), caught.exception.boundary_code, native
                )
                self.assertEqual(native.get("MESSAGE"), message, native)

    def test_unsupported_iso_parity(self) -> None:
        """Verify Python and Native C inspector match on unsupported disc ID."""
        iso_file = self.temp_dir / "unsupported.iso"
        create_test_iso(iso_file, disc_id="ULUS99999", title="Random Unsupported Game", volume_id="ULUS99999")

        py_meta = inspect_iso(iso_file)
        c_meta = self._run_native_inspect(iso_file)

        self.assertEqual(c_meta.get("RESULT"), "OK")
        self.assertEqual(c_meta.get("DISC_ID"), "ULUS99999")
        self.assertEqual(c_meta.get("SUPPORTED"), "0")
        self.assertEqual(c_meta.get("MATCHED_ID"), "NONE")
        self.assertIsNone(py_meta.matched_profile)

    def test_native_library_crud(self) -> None:
        """Verify native C library persistence (add, save, load, lookup, remove)."""
        lib_json = self.temp_dir / "library.json"
        cmd = [str(self.exe_path), "library_test", str(lib_json)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"Library test failed: {res.stderr}")
        self.assertIn("LIBRARY_TEST_PASSED", res.stdout)

    def test_native_boot_executable_byte_limit_round_trip_and_overflow_refusal(self) -> None:
        root = self.temp_dir / "boot-limit-root"
        root.mkdir()

        def write_library(name: str, boot_executable: str) -> Path:
            path = self.temp_dir / name
            path.write_text(json.dumps({
                "schema_version": 1,
                "games": [{
                    "disc_id": "TEST00001",
                    "title_name": "Synthetic",
                    "title_id": "synthetic-allegrex-v1",
                    "iso_path": "synthetic.iso",
                    "selected_executable": "EBOOT.BIN",
                    "boot_executable": boot_executable,
                }],
            }, ensure_ascii=False), encoding="utf-8")
            return path

        overlong = write_library("boot-overlong.json", "X" * 256)
        rejected_save = self.temp_dir / "boot-rejected-save.json"
        rejected_save_run = subprocess.run(
            [str(self.exe_path), "boot_save_unterminated", str(rejected_save)],
            capture_output=True, text=True,
        )
        self.assertEqual(rejected_save_run.returncode, 0, rejected_save_run.stderr)
        self.assertIn("SAVE_RESULT:-12", rejected_save_run.stdout)
        self.assertFalse(rejected_save.exists())

        rejected = subprocess.run(
            [str(self.exe_path), "boot_roundtrip", str(overlong),
             str(self.temp_dir / "must-not-be-saved.json"), str(root)],
            capture_output=True, text=True,
        )
        self.assertEqual(rejected.returncode, 0, rejected.stderr)
        self.assertIn("LOAD_RESULT:-12", rejected.stdout)

        valid_name = '"' * 255
        self.assertEqual(len(valid_name.encode("utf-8")), 255)
        valid = write_library("boot-valid.json", valid_name)
        saved = self.temp_dir / "boot-valid-saved.json"
        roundtrip = subprocess.run(
            [str(self.exe_path), "boot_roundtrip", str(valid), str(saved), str(root)],
            capture_output=True, text=True,
        )
        self.assertEqual(roundtrip.returncode, 0, roundtrip.stderr)
        self.assertIn("LOAD_RESULT:0", roundtrip.stdout)
        self.assertIn("BOOT_LENGTH:255", roundtrip.stdout)
        self.assertIn("SAVE_BOOT_MATCH:1", roundtrip.stdout)
        self.assertIn("SESSION_BOOT_MATCH:1", roundtrip.stdout)
        self.assertEqual(json.loads(saved.read_text(encoding="utf-8"))["games"][0]["boot_executable"], valid_name)

        utf8_name = "é" * 127 + "A"
        self.assertEqual(len(utf8_name.encode("utf-8")), 255)
        utf8_input = write_library("boot-valid-utf8.json", utf8_name)
        utf8_saved = self.temp_dir / "boot-valid-utf8-saved.json"
        utf8_roundtrip = subprocess.run(
            [str(self.exe_path), "boot_roundtrip", str(utf8_input),
             str(utf8_saved), str(root)],
            capture_output=True, text=True,
        )
        self.assertEqual(utf8_roundtrip.returncode, 0, utf8_roundtrip.stderr)
        self.assertIn("BOOT_LENGTH:255", utf8_roundtrip.stdout)
        self.assertIn("SAVE_BOOT_MATCH:1", utf8_roundtrip.stdout)
        self.assertIn("SESSION_BOOT_MATCH:1", utf8_roundtrip.stdout)
        self.assertEqual(
            json.loads(utf8_saved.read_text(encoding="utf-8"))["games"][0]["boot_executable"],
            utf8_name,
        )

        unterminated_session = subprocess.run(
            [str(self.exe_path), "boot_session_unterminated"],
            capture_output=True, text=True,
        )
        self.assertEqual(unterminated_session.returncode, 0, unterminated_session.stderr)
        self.assertIn("SESSION_RESULT:-12", unterminated_session.stdout)
        self.assertIn("boot_executable", unterminated_session.stdout)

    def test_native_launch_plan(self) -> None:
        """Native launch resolution is identity-bound to the selected title.

        Failing-before contract: this fixture used to PROVE the defect --
        a stale build/<retail>/<retail> runtime satisfied an unrelated
        selected title (display-smoke-v1) and the session paired that binary
        with another title's session data. The stale artifact now must be
        irrelevant, and only the selected title's own build may resolve.
        """
        mock_root = self.temp_dir / "mock_repo"
        stale_dir = mock_root / "build" / "hst"
        stale_dir.mkdir(parents=True, exist_ok=True)
        exe_name = "hst.exe" if sys.platform == "win32" else "hst"
        stale_exe = stale_dir / exe_name
        stale_exe.write_bytes(b"MZfake")
        (stale_dir / "hst_image.bin").write_bytes(b"image")

        mock_iso = self.temp_dir / "game.iso"
        create_test_iso(mock_iso)

        # The native harness resolves application data from the environment, so
        # every launch below runs with it pointed at this test's temp directory.
        # Built once and passed to both invocations: the refusal case further
        # down must not escape the isolation either.
        env = {
            **os.environ,
            "LOCALAPPDATA": str(self.temp_dir),
            "APPDATA": str(self.temp_dir),
            "USERPROFILE": str(self.temp_dir),
            "HOME": str(self.temp_dir),
        }

        # A selected title the catalog describes must NOT resolve through the
        # stale retail-shaped build: only its own game_name layout counts, and
        # an absent own runtime is an honest missing-runtime error.
        cmd = [str(self.exe_path), "launch_test", str(mock_root), str(mock_iso),
               "TEST00006", "display-smoke-v1"]
        res = subprocess.run(cmd, capture_output=True, text=True, env=env)
        self.assertEqual(res.returncode, 0, f"Launch plan test failed: {res.stderr}")
        self.assertIn("LAUNCH_PREPARE_ERROR", res.stdout)
        self.assertNotIn("LAUNCH_PREPARE_OK", res.stdout)
        self.assertIn("Runtime binary not found", res.stdout)
        self.assertNotIn("hst.exe", res.stdout)

        # With the selected title's own runtime present, the session resolves
        # it -- still ignoring the stale sibling -- and takes its addresses
        # from the catalog rather than from a constant.
        own_dir = mock_root / "build" / "display-smoke"
        own_dir.mkdir(parents=True, exist_ok=True)
        own_exe = own_dir / ("display-smoke.exe" if sys.platform == "win32" else "display-smoke")
        own_exe.write_bytes(b"MZfake")
        (own_dir / "display-smoke_image.bin").write_bytes(b"image")

        cmd = [str(self.exe_path), "launch_test", str(mock_root), str(mock_iso),
               "TEST00006", "display-smoke-v1"]
        missing_data = subprocess.run(cmd, capture_output=True, text=True, env=env)
        self.assertEqual(missing_data.returncode, 0, missing_data.stderr)
        self.assertIn("LAUNCH_PREPARE_ERROR", missing_data.stdout)
        self.assertIn("data folder", missing_data.stdout.lower())
        self.assertNotIn("LAUNCH_PREPARE_OK", missing_data.stdout)

        (mock_root / "tests" / "fixtures" / "display_smoke").mkdir(parents=True)

        res = subprocess.run(cmd, capture_output=True, text=True, env=env)
        self.assertEqual(res.returncode, 0, f"Launch plan test failed: {res.stderr}")
        self.assertIn("LAUNCH_PREPARE_OK", res.stdout)
        self.assertIn(Path(own_exe).name, res.stdout)
        self.assertNotIn("hst.exe", res.stdout)
        self.assertIn(Path(mock_iso).name, res.stdout)
        self.assertIn("BASE:0x08810000", res.stdout)
        self.assertIn("ENTRY:0x08810000", res.stdout)

        # A title it does not describe is refused. This used to "succeed" by
        # launching the guest at a hard-coded 0x0029a060 with a zero base: an
        # address belonging to no public title, so the runtime was started at a
        # constant rather than at anything the catalog knew. A public-safe tree
        # has no addresses for an uncatalogued retail identity and must say so.
        cmd = [str(self.exe_path), "launch_test", str(mock_root), str(mock_iso),
               "UCES01402", "private-retail-lookalike-v1"]
        res = subprocess.run(cmd, capture_output=True, text=True, env=env)
        self.assertEqual(res.returncode, 0, f"Launch plan test failed: {res.stderr}")
        self.assertIn("LAUNCH_PREPARE_ERROR", res.stdout)
        self.assertNotIn("LAUNCH_PREPARE_OK", res.stdout)
        self.assertIn("No catalog entry", res.stdout)

        # The public HST manifest is catalogued, so its identity resolves, but a
        # tree without its runtime still refuses by name rather than starting at
        # any address of its own.
        cmd = [str(self.exe_path), "launch_test", str(mock_root), str(mock_iso),
               "UCUS98701", "hst-ucus98701-v1"]
        res = subprocess.run(cmd, capture_output=True, text=True, env=env)
        self.assertEqual(res.returncode, 0, f"Launch plan test failed: {res.stderr}")
        self.assertIn("LAUNCH_PREPARE_ERROR", res.stdout)
        self.assertNotIn("LAUNCH_PREPARE_OK", res.stdout)
        self.assertIn("Runtime binary not found for title hst-ucus98701-v1", res.stdout)

    def test_duplicate_sfo_keys_parity(self) -> None:
        """Verify identical duplicate SFO keys accepted and conflicting rejected by both Python and C."""
        from nk_core.iso_inspect import IsoInspectionError

        # 1. Conflicting keys: Python and Native C must reject
        sfo_conflict = build_custom_param_sfo([
            ("DISC_ID", 0x0204, b"UCUS98701\0"),
            ("DISC_ID", 0x0204, b"TEST80004\0"),
            ("TITLE", 0x0204, b"Test Conflict\0"),
        ])
        iso_conflict = self.temp_dir / "conflict.iso"
        create_custom_sfo_iso(iso_conflict, sfo_conflict)

        with self.assertRaises(IsoInspectionError):
            inspect_iso(iso_conflict)

        c_conflict = self._run_native_inspect(iso_conflict)
        self.assertTrue(c_conflict.get("RESULT", "").startswith("ERROR"))
        valid_iso = self.temp_dir / "preflight-valid.iso"
        create_test_iso(valid_iso)
        preflight_meta = inspect_iso(valid_iso)
        preflight = inspect_compatibility_preflight(
            iso_conflict, metadata=preflight_meta, runtime_root=self.temp_dir
        )
        disc_check = next(check for check in preflight["checks"]
                          if check["code"] == "DISC_SFO")
        self.assertEqual(disc_check["status"], "UNSUPPORTED")

        # 2. Byte-identical keys: Python and Native C must accept
        sfo_identical = build_custom_param_sfo([
            ("DISC_ID", 0x0204, b"UCUS98701\0"),
            ("DISC_ID", 0x0204, b"UCUS98701\0"),
            ("TITLE", 0x0204, b"Test Identical\0"),
        ])
        iso_identical = self.temp_dir / "identical.iso"
        create_custom_sfo_iso(iso_identical, sfo_identical)

        py_identical = inspect_iso(iso_identical)
        self.assertEqual(py_identical.disc_id, "UCUS98701")

        c_identical = self._run_native_inspect(iso_identical)
        self.assertEqual(c_identical.get("RESULT"), "OK")
        self.assertEqual(c_identical.get("DISC_ID"), "UCUS98701")

    def test_identity_parameter_format_parity(self) -> None:
        """An identity field in a non-string format is refused by both parsers.

        The native reader used to ignore the parameter format at entry offset
        +2 and decode the bytes as text regardless, while the Python inspector
        yielded the decoded integer. A disc whose DISC_ID declared an integer
        format could therefore be matched to a catalog title by one parser and
        not the other -- the differential contract these two are meant to keep.
        """
        from nk_core.iso_inspect import IsoInspectionError

        # DISC_ID declared as uint32 (0x0404) while carrying string bytes.
        sfo_int_id = build_custom_param_sfo([
            ("DISC_ID", 0x0404, b"UCUS98701\0"),
            ("TITLE", 0x0204, b"Wrong Format\0"),
        ])
        iso_int_id = self.temp_dir / "int_disc_id.iso"
        create_custom_sfo_iso(iso_int_id, sfo_int_id)

        with self.assertRaises(IsoInspectionError):
            inspect_iso(iso_int_id)

        c_int_id = self._run_native_inspect(iso_int_id)
        self.assertTrue(c_int_id.get("RESULT", "").startswith("ERROR"))

        # An unrecognised format is refused the same way, not silently ignored.
        sfo_unknown = build_custom_param_sfo([
            ("DISC_ID", 0x0101, b"UCUS98701\0"),
            ("TITLE", 0x0204, b"Unknown Format\0"),
        ])
        iso_unknown = self.temp_dir / "unknown_fmt.iso"
        create_custom_sfo_iso(iso_unknown, sfo_unknown)

        with self.assertRaises(IsoInspectionError):
            inspect_iso(iso_unknown)

        c_unknown = self._run_native_inspect(iso_unknown)
        self.assertTrue(c_unknown.get("RESULT", "").startswith("ERROR"))

        # A non-identity key in a non-string format is NOT a reason to reject:
        # only identity decides whether a disc matches a catalog title.
        sfo_other = build_custom_param_sfo([
            ("DISC_ID", 0x0204, b"UCUS98701\0"),
            ("TITLE", 0x0204, b"Fine\0"),
            ("PARENTAL_LEVEL", 0x0404, bytes([1, 0, 0, 0])),
        ])
        iso_other = self.temp_dir / "other_int_key.iso"
        create_custom_sfo_iso(iso_other, sfo_other)

        py_other = inspect_iso(iso_other)
        self.assertEqual(py_other.disc_id, "UCUS98701")

        c_other = self._run_native_inspect(iso_other)
        self.assertEqual(c_other.get("RESULT"), "OK")
        self.assertEqual(c_other.get("DISC_ID"), "UCUS98701")

    def test_reader_parity_inspect_and_lookup(self) -> None:
        iso_path = self.temp_dir / "reader_test.iso"
        create_test_iso(iso_path, disc_id="UCUS98701", title="Reader Test", volume_id="READER_VOL")

        # Python inspect
        py_meta = inspect_iso(iso_path)
        self.assertEqual(py_meta.volume_id.rstrip("\x00 "), "READER_VOL")

        # Native reader test
        cmd = [str(self.exe_path), "reader_test", str(iso_path)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        lines = dict(line.split(":", 1) for line in res.stdout.strip().splitlines() if ":" in line)
        self.assertEqual(lines.get("READER"), "OPEN_OK")
        self.assertEqual(lines.get("VOLUME_ID"), "READER_VOL")
        self.assertEqual(lines.get("FILE_SIZE"), str(1024 * 2048))

        sfo_parts = lines.get("LOOKUP_SFO", "").split(":")
        self.assertEqual(sfo_parts[0], "0")
        self.assertEqual(sfo_parts[1], "32")
        self.assertEqual(sfo_parts[3], "0")

        dir_parts = lines.get("LOOKUP_DIR", "").split(":")
        self.assertEqual(dir_parts[0], "0")
        self.assertEqual(dir_parts[1], "34")
        self.assertEqual(dir_parts[3], "1")

        self.assertEqual(lines.get("LOOKUP_MISS"), "-1")

        list_parts = lines.get("LIST_ROOT_0", "").split(":")
        self.assertEqual(list_parts[0], "1")
        self.assertEqual(list_parts[1], "PSP_GAME")
        self.assertEqual(list_parts[2], "34")
        self.assertEqual(list_parts[4], "1")

        self.assertEqual(lines.get("LIST_ROOT_1"), "0")
        self.assertGreater(int(lines.get("READ_SFO", "0")), 0)
        self.assertEqual(lines.get("READ_EOF"), "0")

    def test_inspect_finds_psp_game_and_param_sfo_by_the_reader_name_rules(self) -> None:
        """Audit F-074a12a4: the identifiers PSP_GAME and PARAM.SFO match case-insensitively,
        without their ';1' version and without one trailing separator '.', in the C inspect as
        in the C reader, the stager and the Python inspect."""
        sfo = build_param_sfo("TEST00001", "Name Rules")
        sector = 2048
        cases = {
            "lower-case": (b"psp_game", b"param.sfo;1"),
            "mixed-case": (b"Psp_Game", b"Param.Sfo"),
            "separator-dot": (b"PSP_GAME.", b"PARAM.SFO.;1"),
        }
        for name, (game_name, sfo_name) in cases.items():
            with self.subTest(case=name):
                iso_path = self.temp_dir / f"name-rules-{name}.iso"
                create_test_iso(iso_path, sfo_bytes=sfo)
                data = bytearray(iso_path.read_bytes())
                root = (
                    _dir_record(bytes([0]), 33, sector, True)
                    + _dir_record(bytes([1]), 33, sector, True)
                    + _dir_record(game_name, 34, sector, True)
                )
                game = (
                    _dir_record(bytes([0]), 34, sector, True)
                    + _dir_record(bytes([1]), 33, sector, True)
                    + _dir_record(sfo_name, 32, len(sfo), False)
                )
                data[33 * sector : 35 * sector] = bytes(2 * sector)
                data[33 * sector : 33 * sector + len(root)] = root
                data[34 * sector : 34 * sector + len(game)] = game
                iso_path.write_bytes(bytes(data))

                py_meta = inspect_iso(iso_path)
                self.assertEqual((py_meta.disc_id, py_meta.title), ("TEST00001", "Name Rules"))
                c_meta = self._run_native_inspect(iso_path)
                self.assertEqual(c_meta.get("RESULT"), "OK", c_meta)
                self.assertEqual(
                    (c_meta.get("DISC_ID"), c_meta.get("TITLE"), c_meta.get("HAS_PSP_GAME"),
                     c_meta.get("PARAM_SFO_PARSED")),
                    ("TEST00001", "Name Rules", "1", "1"),
                )
                res = subprocess.run(
                    [str(self.exe_path), "lookup", str(iso_path), "PSP_GAME/PARAM.SFO"],
                    capture_output=True, text=True, encoding="utf-8", errors="strict",
                )
                self.assertEqual(res.returncode, 0, res.stderr)
                fields = next(
                    line for line in res.stdout.splitlines() if line.startswith("LOOKUP:")
                ).split(":")
                self.assertEqual(fields[1:], ["0", "32", str(len(sfo)), "0"])

    def test_non_retail_disc_with_lower_case_names_keeps_its_content_key(self) -> None:
        """Audit F-074a12a4: a beta disc whose directories and files are named in lower case
        gets, in the C inspect too, the key the Python inspect already gave it. Names do not
        enter the key, so it is the pinned key of the same contents under upper-case names."""
        iso_file = self.temp_dir / "beta-lower-case.iso"
        create_test_iso_with_executables(
            iso_file, self.BETA_EXECUTABLE, disc_id=self.BETA_DISC_ID, title="Synthetic Beta",
        )
        data = iso_file.read_bytes()
        for name in (b"PSP_GAME", b"PARAM.SFO;1", b"SYSDIR", b"EBOOT.BIN;1"):
            # Each identifier follows its one-byte length in exactly one directory record.
            record_name = bytes([len(name)]) + name
            self.assertEqual(data.count(record_name), 1, name)
            data = data.replace(record_name, bytes([len(name)]) + name.lower())
        iso_file.write_bytes(data)
        key, digest = self._expected_identity(
            build_param_sfo(self.BETA_DISC_ID, "Synthetic Beta"), self.BETA_EXECUTABLE,
        )
        self.assertEqual(key, self.BETA_KEY)

        py_meta = inspect_iso(iso_file)
        self.assertEqual(
            (py_meta.disc_id, py_meta.sfo_disc_id, py_meta.non_retail, py_meta.identity_sha256),
            (key, self.BETA_DISC_ID, True, digest),
        )
        c_meta = self._run_native_inspect(iso_file)
        self.assertEqual(c_meta.get("RESULT"), "OK", c_meta)
        self.assertEqual(
            (c_meta.get("DISC_ID"), c_meta.get("SFO_DISC_ID"), c_meta.get("NON_RETAIL"),
             c_meta.get("IDENTITY_SHA256"), c_meta.get("HAS_PSP_GAME")),
            (key, self.BETA_DISC_ID, "1", digest, "1"),
        )

    def test_semicolon_member_names_resolve_the_same_in_python_and_c(self) -> None:
        # A ';' inside a member name is part of the name. Only a trailing ';' and digits is a
        # version, so "A;1.BIN" and "A;2.BIN" are two files in both implementations.
        iso_path = self.temp_dir / "semicolon_members.iso"
        create_test_iso_with_module_tree(iso_path, {
            "PSP_GAME/USRDIR/DATA/A;1.BIN": b"first-bytes",
            "PSP_GAME/USRDIR/DATA/A;2.BIN": b"second-bytes",
            "PSP_GAME/USRDIR/DATA/FOO.BIN": b"plain",
        })
        py_entries = iso_inspect.list_iso_directory(iso_path, ("PSP_GAME", "USRDIR", "DATA"))
        self.assertIsNotNone(py_entries)
        py_by_name = {entry.name: entry for entry in py_entries}
        self.assertEqual(sorted(py_by_name), ["A;1.BIN", "A;2.BIN", "FOO.BIN"])

        listing = subprocess.run(
            [str(self.exe_path), "entries", str(iso_path), "PSP_GAME/USRDIR/DATA"],
            capture_output=True, text=True, encoding="utf-8", errors="strict",
        )
        self.assertEqual(listing.returncode, 0, listing.stderr)
        native_names = []
        for line in listing.stdout.splitlines():
            if line.startswith("ENTRY:"):
                name, lba, size, _is_dir = line[len("ENTRY:"):].rsplit(":", 3)
                native_names.append(name)
                self.assertEqual((int(lba), int(size)),
                                 (py_by_name[name].lba, py_by_name[name].size), name)
        self.assertEqual(sorted(native_names), sorted(py_by_name))

        for name in ("A;1.BIN", "A;2.BIN", "FOO.BIN", "A;3.BIN"):
            res = subprocess.run(
                [str(self.exe_path), "lookup", str(iso_path), "PSP_GAME/USRDIR/DATA/" + name],
                capture_output=True, text=True, encoding="utf-8", errors="strict",
            )
            self.assertEqual(res.returncode, 0, res.stderr)
            fields = next(
                line for line in res.stdout.splitlines() if line.startswith("LOOKUP:")
            ).split(":")
            if name in py_by_name:
                entry = py_by_name[name]
                self.assertEqual(fields[1:], ["0", str(entry.lba), str(entry.size), "0"], name)
            else:
                self.assertEqual(fields[1], "-1", name)

    def test_reader_invalid_iso_fails_open(self) -> None:
        bad_iso = self.temp_dir / "bad.iso"
        bad_iso.write_bytes(b"garbage data not an iso" * 100)
        cmd = [str(self.exe_path), "reader_test", str(bad_iso)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0)
        self.assertIn("READER:OPEN_FAILED", res.stdout)


class InspectRuntimePackageTests(unittest.TestCase):
    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory(prefix="nk-inspect-package-")
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        self.user_data = self.root / "user"
        self.user_data.mkdir()
        self.elf = build_plain_mips_elf()

    def _write_package(
        self, user_data: Path, iso: Path, *, profile_id: str | None = None,
        codegen_options: dict[str, str | int] | None = None,
    ) -> Path:
        metadata = inspect_iso(iso)
        disc_id: str = metadata.disc_id
        profile = metadata.matched_profile
        title_id = profile_id or (profile.id if profile is not None else f"experimental-{disc_id.lower()}")
        folder = user_data / "packages" / disc_id
        folder.mkdir(parents=True)
        executable = folder / "fixture.exe"
        executable.write_bytes(b"synthetic host executable")
        (folder / "fixture_image.bin").write_bytes(b"synthetic runtime image")

        executable_hash = hashlib.sha256(self.elf).hexdigest()
        empty_digest = "0" * 64
        inputs: dict[str, object] = {
            "manifest": {"sha256": empty_digest},
            "executable": {"sha256": executable_hash},
            "modules": [], "psp_header": None,
        }
        identity = package_cache.build_title_input_identity(
            manifest={"id": title_id, "schema_version": 1},
            executable_name="EBOOT.BIN", executable_sha256=executable_hash, modules=[],
            disc_id=disc_id, disc_version=metadata.version, param_sfo=metadata.param_sfo_facts,
            source_media={
                "executable": {"path": "PSP_GAME/SYSDIR/EBOOT.BIN", "sha256": executable_hash},
                "modules": [],
            },
        )
        options = codegen_options or {}
        key = package_cache.build_cache_key(
            input_hashes=inputs, title_input_identity=identity, codegen_options=options,
            analyzer_sha256=empty_digest, codegen_sha256=empty_digest,
            compiler="synthetic-fixture", target="synthetic", runtime_source_digest=empty_digest,
        )
        cache = package_cache.cache_metadata(key, options)
        package = {
            "format": "nakagawa-aot-package", "schema_version": 3,
            "title": {"id": title_id, "manifest_sha256": empty_digest},
            "cache": cache, "inputs": inputs, "title_input_identity": identity,
            "executable": {"path": executable.name, "sha256": package_cache.sha256_file(executable)},
        }
        (folder / "package.json").write_text(package_cache.canonical_json(package), encoding="utf-8")
        (folder / "build-report.json").write_text(
            package_cache.canonical_json({"cache": cache}), encoding="utf-8",
        )
        package_cache.write_local_title_input_identity(user_data, identity)
        package_cache.write_completion_manifest(folder, key)
        valid, reason = package_cache.validate_package_cache(folder)
        self.assertTrue(valid, reason)
        return folder

    def _inspect(
        self, iso: Path, *, supported: bool = False, user_data: Path | None = None,
    ) -> dict:
        result = subprocess.run(
            [sys.executable, "-B", str(ROOT / "tools" / "nk_cli.py"), "inspect",
             str(iso), "--json", "--user-data-root", str(user_data or self.user_data)],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 0 if supported else 2, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["supported"], supported)
        self.assertEqual(payload["catalogued"], supported)
        report = payload["compatibility_preflight"]
        self.assertEqual(report["is_experimental"], not supported)
        checks: dict[str, dict] = {check["code"]: check for check in report["checks"]}
        if not supported:
            self.assertEqual(checks["EXPERIMENTAL"]["status"], "IN_PROGRESS")
        return checks["RUNTIME_PACKAGE"]

    def test_cli_experimental_package_status_checks_the_selected_input(self) -> None:
        iso = self.root / "experimental.iso"
        disc_id = "ULUS99998"
        create_test_iso_with_executables(iso, self.elf, disc_id=disc_id)
        write_experimental_profile(iso, self.user_data, metadata=inspect_iso(iso))
        self.assertEqual(self._inspect(iso)["status"], "MISSING")

        package = self._write_package(self.user_data, iso)
        self.assertEqual(self._inspect(iso)["status"], "OK")
        for name, elf, version, reason in (
            ("changed-executable", self.elf + b"fixture variant", "1.00", "bytes differ"),
            ("changed-revision", self.elf, "2.00", "disc revision"),
        ):
            with self.subTest(name=name):
                other = self.root / f"{name}.iso"
                create_test_iso_with_executables(other, elf, disc_id=disc_id, version=version)
                check = self._inspect(other)
                self.assertEqual(check["status"], "UNSUPPORTED")
                self.assertIn(reason, check["message"])

        local_identity = package_cache.local_title_input_identity_path(self.user_data, disc_id)
        original_identity = local_identity.read_bytes()
        changed_identity = json.loads(original_identity)
        changed_identity["main_executable"]["sha256"] = "f" * 64
        local_identity.write_text(package_cache.canonical_json(changed_identity), encoding="utf-8")
        check = self._inspect(iso)
        self.assertEqual(check["status"], "UNSUPPORTED")
        self.assertIn("local record", check["message"])
        local_identity.unlink()
        check = self._inspect(iso)
        self.assertEqual(check["status"], "UNSUPPORTED")
        self.assertIn("missing or unreadable", check["message"])
        local_identity.write_bytes(original_identity)

        with (package / "build-report.json").open("ab") as report:
            report.write(b"\n")
        check = self._inspect(iso)
        self.assertEqual(check["status"], "UNSUPPORTED")
        self.assertIn("validation failed", check["message"])

    def test_cli_experimental_package_rejects_a_valid_other_disc_package(self) -> None:
        selected_disc_id = "ULUS99998"
        other_disc_id = "ULUS99997"
        selected = self.root / "selected.iso"
        other = self.root / "other.iso"
        create_test_iso_with_executables(selected, self.elf, disc_id=selected_disc_id)
        create_test_iso_with_executables(other, self.elf, disc_id=other_disc_id)
        other_root = self.root / "other-user"
        other_package = self._write_package(other_root, other)
        selected_package = self.user_data / "packages" / selected_disc_id
        shutil.copytree(other_package, selected_package)
        self.assertTrue(package_cache.validate_package_cache(selected_package)[0])
        other_identity = package_cache.local_title_input_identity_path(other_root, other_disc_id)
        selected_identity = package_cache.local_title_input_identity_path(self.user_data, selected_disc_id)
        selected_identity.parent.mkdir(parents=True)
        shutil.copyfile(other_identity, selected_identity)
        check = self._inspect(selected)
        self.assertEqual(check["status"], "UNSUPPORTED")
        self.assertIn("different disc", check["message"])

    def test_cli_supported_title_keeps_its_package_and_exit_status(self) -> None:
        iso = self.root / "supported.iso"
        create_test_iso_with_executables(iso, self.elf)
        self.assertEqual(self._inspect(iso, supported=True)["status"], "MISSING")
        self._write_package(self.user_data, iso)
        self.assertEqual(self._inspect(iso, supported=True)["status"], "OK")

    def test_cli_experimental_package_rejects_the_wrong_profile_or_variant(self) -> None:
        iso = self.root / "experimental.iso"
        create_test_iso_with_executables(iso, self.elf, disc_id="ULUS99998")
        self._write_package(self.user_data, iso, profile_id="other-experimental-profile")
        check = self._inspect(iso)
        self.assertEqual(check["status"], "UNSUPPORTED")
        self.assertIn("different experimental profile", check["message"])

        for option, value in (
            ("enhancement_package_id", "fixture-60hz"),
            ("enhancement_package_epoch", 1),
            ("code_hook_package_id", "fixture-hook"),
        ):
            with self.subTest(option=option):
                user_data = self.root / option
                self._write_package(user_data, iso, codegen_options={option: value})
                check = self._inspect(iso, user_data=user_data)
                self.assertEqual(check["status"], "UNSUPPORTED")
                self.assertIn("enhanced build", check["message"])

    def test_cli_inspect_failure_keeps_its_error_exit(self) -> None:
        result = subprocess.run(
            [sys.executable, "-B", str(ROOT / "tools" / "nk_cli.py"), "inspect",
             str(self.root / "absent.iso"), "--json", "--user-data-root", str(self.user_data)],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertIn("Error inspecting ISO", result.stderr)


class PbpBoundaryCodeParityTests(unittest.TestCase):
    """The PBP identify codes are one set in the Python emitter and the C emitter.

    This runs without gcc: it reads the C emitter's source rather than
    executing it, so a new code added only on the C side fails here even on a
    machine that cannot build the native harness.
    """

    C_EMITTER = ROOT / "src" / "core" / "nk_iso.c"
    # A C string literal naming a PBP boundary code. Only quoted literals match,
    # so identifiers such as PBP_MAGIC or PBP_HEADER_SIZE are not counted.
    C_CODE_LITERAL = re.compile(r'"(PBP_[A-Z0-9_]+)"')

    def test_c_emitter_emits_exactly_the_python_boundary_codes(self) -> None:
        source = self.C_EMITTER.read_text(encoding="utf-8")
        emitted = set(self.C_CODE_LITERAL.findall(source))
        self.assertTrue(emitted, "no PBP boundary code literal found in nk_iso.c")
        self.assertEqual(emitted, set(iso_inspect.PBP_BOUNDARY_CODES))


class PackageBuildFailureMessageTests(unittest.TestCase):
    """The generic compile/link failure branch names the first real diagnostic line."""

    def setUp(self) -> None:
        self.temp_dir = Path(tempfile.mkdtemp(prefix="nk_build_fail_test_"))

    def tearDown(self) -> None:
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def _build_with_failing_compile(self, disc_id: str, stderr: str, returncode: int,
                                    with_log: bool, stdout: str = "") -> tuple[int, str, Path]:
        user_root = self.temp_dir / f"user-{disc_id}"
        user_root.mkdir(parents=True, exist_ok=True)
        iso_file = self.temp_dir / f"{disc_id}.iso"
        create_test_iso_with_executables(iso_file, build_psp_container(), disc_id=disc_id,
                                         title="Build Fail Title")
        title_id = "synthetic-title2-v1"
        entry = {
            "disc_id": disc_id,
            "title_id": title_id,
            "iso_path": str(iso_file),
            "selected_executable": "",
            "is_experimental": False,
        }
        (user_root / "library.json").write_text(
            json.dumps({"schema_version": 1, "games": [entry]}), encoding="utf-8")
        decrypted_dir = user_root / "titles" / disc_id / "decrypted"
        decrypted_dir.mkdir(parents=True, exist_ok=True)
        (decrypted_dir / "EBOOT.elf").write_bytes(build_plain_mips_elf())
        manifest_path = ROOT / "assets" / "titles" / "synthetic-title2.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

        progress = self.temp_dir / f"{disc_id}-progress.jsonl"
        log_file = self.temp_dir / f"{disc_id}-build.log"
        args = type("BuildArgs", (), {
            "disc_id": disc_id,
            "user_data_root": user_root,
            "module_dir": None,
            "psp_header": None,
            "progress_json": progress,
            "log_file": log_file if with_log else None,
        })()

        def fake_compile(command, **_kwargs):
            if _is_compiler_target_query(command):
                return _REAL_SUBPROCESS_RUN(command, **_kwargs)
            return subprocess.CompletedProcess(list(command), returncode, stdout=stdout, stderr=stderr)

        with patch.object(nk_cli, "_load_entry_manifest", return_value=(manifest_path, manifest, None)), \
             patch.object(nk_cli.subprocess, "run", side_effect=fake_compile), \
             patch.object(nk_cli, "_stage_runtime_assets"), \
             patch.object(nk_cli.package_cache, "validate_package_cache", return_value=(True, "")):
            rc = nk_cli.cmd_build_package(args)

        events = [json.loads(line) for line in progress.read_text(encoding="utf-8").splitlines()]
        compile_failures = [e for e in events if e["stage"] == "compile" and e["status"] == "FAIL"]
        self.assertEqual(len(compile_failures), 1)
        return rc, compile_failures[0]["message"], log_file

    def test_linker_failure_names_first_diagnostic_and_exit_code(self) -> None:
        stderr = (
            "make: *** [Makefile:12: atrac3p.o] Error 1\n"
            "ld.exe: cannot find D:/games/packages/.build-package-xyz/atrac3p: No such file or directory\n"
            "collect2.exe: error: ld returned 1 exit status\n"
            "make: *** [Makefile:88: all] Error 1\n"
        )
        rc, message, log_file = self._build_with_failing_compile(
            "TEST00101", stderr, returncode=2, with_log=True)
        self.assertEqual(rc, 2)
        self.assertEqual(
            message,
            "PACKAGE_BUILD_FAILED: ld.exe: cannot find D:/games/packages/.build-package-xyz/atrac3p: "
            f"No such file or directory (exit code 2; full log: {log_file})",
        )

    def test_diagnostic_on_stdout_without_log_file(self) -> None:
        stdout = (
            "gcc -c src/rt/hle.c -o build/hle.o\n"
            "src/rt/hle.c:12:5: error: 'missing' undeclared (first use in this function)\n"
        )
        rc, message, _log_file = self._build_with_failing_compile(
            "TEST00103", "make: *** [Makefile:88: all] Error 2\n", returncode=2,
            with_log=False, stdout=stdout)
        self.assertEqual(rc, 2)
        self.assertEqual(
            message,
            "PACKAGE_BUILD_FAILED: src/rt/hle.c:12:5: error: 'missing' undeclared "
            "(first use in this function) (exit code 2)",
        )

    def test_diagnostic_markers_skip_make_noise_and_warnings(self) -> None:
        first = nk_cli._first_build_diagnostic
        self.assertEqual(
            first("a.c:1:1: warning: unused variable 'x'\n"
                  "ld: game.o: in function `main': undefined reference to `missing_fn'\n"),
            "ld: game.o: in function `main': undefined reference to `missing_fn'",
        )
        self.assertEqual(first("make: *** [Makefile:1: x] error: 1\nb.c:2:3: error: bad\n"),
                         "b.c:2:3: error: bad")
        self.assertIsNone(first("make: *** [Makefile:1: all] Error 2\nwarning: nothing\n"))
        self.assertEqual(len(first("x.c:1:1: error: " + "y" * 1000)), 300)

    def test_fallback_names_log_file_when_one_was_requested(self) -> None:
        rc, message, log_file = self._build_with_failing_compile(
            "TEST00104", "make: *** [Makefile:88: all] Error 4\n", returncode=4, with_log=True)
        self.assertEqual(rc, 4)
        self.assertEqual(
            message, f"PACKAGE_BUILD_FAILED: build exited with code 4 (full log: {log_file})")

    def test_failure_without_diagnostic_uses_fallback(self) -> None:
        stderr = "make: *** [Makefile:88: all] Error 3\n"
        rc, message, _log_file = self._build_with_failing_compile(
            "TEST00102", stderr, returncode=3, with_log=False)
        self.assertEqual(rc, 3)
        self.assertEqual(message, "PACKAGE_BUILD_FAILED: build exited with code 3")

class GuestModuleElfRuleTests(unittest.TestCase):
    """Issue #729: a PSP PRX guest module is usable by its real contract (an
    executable PT_LOAD with code bytes), not by an executable e_entry.  The
    module checklist and package extraction apply the same rule, while the
    executable rule keeps its e_entry requirement."""

    def setUp(self) -> None:
        self.temp_dir = Path(tempfile.mkdtemp(prefix="nk_prx_rule_"))
        self.addCleanup(shutil.rmtree, self.temp_dir, True)

    def _write(self, name: str, data: bytes) -> Path:
        path = self.temp_dir / name
        path.write_bytes(data)
        return path

    def test_prx_with_unset_entry_and_code_segment_is_usable_module(self) -> None:
        prx = self._write(
            "libfont.prx", build_plain_mips_elf(0xFFA0, vaddr=0, entry=0xFFFFFFFF)
        )
        self.assertEqual(_classify_decrypted_elf_file(prx, module=True), "PLAIN_MIPS_ELF32")

    def test_module_candidates_reject_prx_segments_above_the_player_limit(self) -> None:
        oversized_header = bytearray(build_psp_container())
        struct.pack_into("<I", oversized_header, 0x54, 100 * 1024 * 1024)
        iso = self.temp_dir / "oversized-prx-segment.iso"
        create_test_iso_with_module_tree(
            iso, {"PSP_GAME/USRDIR/oversized.prx": bytes(oversized_header)}
        )

        self.assertEqual(list_disc_module_candidates(iso), [])

    def test_module_walk_stops_by_name_when_import_analysis_is_unreadable(self) -> None:
        iso = self.temp_dir / "unreadable-module-imports.iso"
        plain_module = build_module_elf([], imports=[("sceAudio", [0x12345678])])
        create_test_iso_with_module_tree(
            iso, {"PSP_GAME/USRDIR/module/unknown-imports.prx": plain_module}
        )

        with patch("imports.parse_imports", side_effect=ValueError("synthetic malformed table")):
            with self.assertRaises(IsoInspectionError) as caught:
                list_disc_module_candidates(iso)

        self.assertEqual(caught.exception.boundary_code, "ANALYZER_IMPORT_TABLE_INVALID")

    def test_module_lookup_round_trips_surrogateescaped_directory_bytes(self) -> None:
        directory = b"\x83\x58".decode("ascii", "surrogateescape")
        iso = self.temp_dir / "shift-jis-module-directory.iso"
        create_test_iso_with_module_tree(
            iso,
            {f"PSP_GAME/USRDIR/{directory}/MOD.PRX": build_psp_container()},
            disc_id="TEST00043",
        )

        candidates = list_disc_module_candidates(iso)
        self.assertEqual(len(candidates), 1)
        checklist = check_needed_modules(
            iso, user_data_root=self.temp_dir / "user-data", disc_id="TEST00043",
            modules=candidates,
        )

        self.assertEqual(checklist["results"][0]["reason"], "encrypted")

    def test_module_walk_reads_each_directory_from_one_open_iso_stream(self) -> None:
        iso = self.temp_dir / "single-stream-module-walk.iso"
        create_test_iso_with_module_tree(iso, {
            "PSP_GAME/SYSDIR/sys.prx": build_psp_container(),
            "PSP_GAME/USRDIR/A/B/sdk.prx": build_psp_container(),
        })
        original_open = Path.open
        original_read_directory = iso_inspect._read_iso_directory_entries
        opened_images = 0
        read_directory_count = 0

        def counted_open(path, *args, **kwargs):
            nonlocal opened_images
            if path == iso:
                opened_images += 1
            return original_open(path, *args, **kwargs)

        def counted_read_directory(*args, **kwargs):
            nonlocal read_directory_count
            read_directory_count += 1
            return original_read_directory(*args, **kwargs)

        with patch.object(Path, "open", new=counted_open), patch.object(
            iso_inspect, "_read_iso_directory_entries", side_effect=counted_read_directory
        ):
            walked = list(iso_inspect.walk_disc_module_entries(iso))

        self.assertEqual(len(walked), 2)
        self.assertEqual(opened_images, 1)
        self.assertEqual(read_directory_count, 6)

    def test_executables_with_entry_in_code_segment_stay_usable(self) -> None:
        for e_type in (2, 3):
            with self.subTest(e_type=e_type):
                path = self._write(f"plain_{e_type}.elf", build_plain_mips_elf(e_type))
                self.assertEqual(_classify_decrypted_elf_file(path), "PLAIN_MIPS_ELF32")
                self.assertEqual(
                    _classify_decrypted_elf_file(path, module=True), "PLAIN_MIPS_ELF32"
                )

    def test_prx_without_load_segment_is_rejected(self) -> None:
        path = self._write(
            "noload.prx", build_plain_mips_elf(0xFFA0, vaddr=0, entry=0xFFFFFFFF, p_type=4)
        )
        self.assertEqual(_classify_decrypted_elf_file(path, module=True), "UNKNOWN")

    def test_prx_whose_only_load_segment_is_not_executable_is_rejected(self) -> None:
        path = self._write(
            "data.prx", build_plain_mips_elf(0xFFA0, vaddr=0, entry=0xFFFFFFFF, p_flags=4)
        )
        self.assertEqual(_classify_decrypted_elf_file(path, module=True), "UNKNOWN")

    def test_prx_code_segment_without_file_bytes_is_rejected(self) -> None:
        path = self._write(
            "bss.prx",
            build_plain_mips_elf(0xFFA0, vaddr=0, entry=0xFFFFFFFF, filesz=0),
        )
        self.assertEqual(_classify_decrypted_elf_file(path, module=True), "UNKNOWN")

    def test_truncated_prx_is_rejected(self) -> None:
        prx = build_plain_mips_elf(0xFFA0, vaddr=0, entry=0xFFFFFFFF)
        for length in (40, 60):
            with self.subTest(length=length):
                path = self._write(f"truncated_{length}.prx", prx[:length])
                self.assertEqual(_classify_decrypted_elf_file(path, module=True), "UNKNOWN")

    def test_unset_entry_stays_rejected_for_executables(self) -> None:
        executable = self._write(
            "unset.elf", build_plain_mips_elf(2, entry=0xFFFFFFFF)
        )
        self.assertEqual(_classify_decrypted_elf_file(executable), "UNKNOWN")
        self.assertEqual(_classify_decrypted_elf_file(executable, module=True), "UNKNOWN")
        # A PRX-format main image still needs its e_entry in a code segment:
        # the launcher starts at e_entry, so the module rule does not apply.
        main_prx = self._write(
            "main_prx.elf", build_plain_mips_elf(0xFFA0, vaddr=0, entry=0xFFFFFFFF)
        )
        self.assertEqual(_classify_decrypted_elf_file(main_prx), "UNKNOWN")

    def test_encrypted_disc_fails_closed_and_never_reads_a_key_file(self) -> None:
        """The command-line check decrypts nothing: key files are ignored, nothing is written.

        A key file at the old default location and the old override pointing at
        another existing file are both present. The encrypted executable and the
        encrypted module fail closed by name, no file is touched or written, and
        neither the override nor either key file is ever looked up.
        """
        import builtins

        iso = self.temp_dir / "encrypted-modules.iso"
        create_test_iso_with_modules(
            iso, build_psp_container(),
            sysdir_modules={"alpha.prx": build_psp_container()},
            usrdir_modules={},
            disc_id="TEST00001", title="Synthetic Encrypted Disc",
        )
        user_root = self.temp_dir / "encrypted-user"
        default_key = user_root / "keys" / "psp-keyfile.json"
        default_key.parent.mkdir(parents=True)
        default_key.write_text("synthetic test marker", encoding="utf-8")
        override_key = self.temp_dir / "override-keys" / "psp-keyfile.json"
        override_key.parent.mkdir()
        override_key.write_text("synthetic test marker", encoding="utf-8")
        key_paths = {str(default_key.resolve()), str(override_key.resolve())}
        touched: list[str] = []
        env_reads: list[str] = []

        def note(path) -> None:
            try:
                resolved = str(Path(path).resolve())
            except (OSError, TypeError, ValueError):
                return
            if resolved in key_paths or "keys" in Path(resolved).parts[-2:-1]:
                touched.append(resolved)

        real_open = builtins.open
        real_path_open = Path.open
        real_stat = Path.stat

        def recording_open(file, *args, **kwargs):
            if isinstance(file, (str, os.PathLike)):
                note(file)
            return real_open(file, *args, **kwargs)

        def recording_path_open(self_path, *args, **kwargs):
            note(self_path)
            return real_path_open(self_path, *args, **kwargs)

        def recording_stat(self_path, *args, **kwargs):
            note(self_path)
            return real_stat(self_path, *args, **kwargs)

        def recording_os_path(real):
            def wrapper(path, *args, **kwargs):
                note(path)
                return real(path, *args, **kwargs)
            return wrapper

        class RecordingEnviron(dict):
            def get(self, key, default=None):
                env_reads.append(key)
                return super().get(key, default)

            def __getitem__(self, key):
                env_reads.append(key)
                return super().__getitem__(key)

            def __contains__(self, key):
                env_reads.append(key)
                return super().__contains__(key)

        environ = RecordingEnviron(os.environ)
        environ["NAKAGAWA_PSP_KEY_FILE"] = str(override_key)
        metadata = inspect_iso(iso)
        with patch.object(os, "environ", environ), \
                patch.object(builtins, "open", recording_open), \
                patch.object(Path, "open", recording_path_open), \
                patch.object(Path, "stat", recording_stat), \
                patch.object(os, "stat", recording_os_path(os.stat)), \
                patch.object(os.path, "isfile", recording_os_path(os.path.isfile)), \
                patch.object(os.path, "exists", recording_os_path(os.path.exists)):
            report = inspect_compatibility_preflight(
                iso, metadata=metadata, runtime_root=user_root
            )
        self.assertEqual(touched, [], "a key file was looked up")
        self.assertNotIn("NAKAGAWA_PSP_KEY_FILE", env_reads)

        decrypted_dir = user_root / "titles" / "TEST00001" / "decrypted"
        checks = {check["code"]: check for check in report["checks"]}
        self.assertEqual(checks["EXECUTABLE"]["status"], "UNSUPPORTED")
        self.assertEqual(
            checks["EXECUTABLE"]["message"],
            "ENCRYPTED_EXECUTABLE: EBOOT.BIN is encrypted and no decrypted EBOOT.elf is in "
            f"{decrypted_dir}. The player decrypts it from your own disc when it checks the "
            "disc; you can also place a decrypted EBOOT.elf from your own copy there.",
        )
        self.assertEqual(checks["GUEST_MODULES"]["status"], "MISSING")
        self.assertEqual(
            checks["GUEST_MODULES"]["message"],
            "Guest modules: 0 of 1 ready; ENCRYPTED_MODULE: alpha.prx is encrypted. "
            "The player decrypts it from your own disc when it checks the disc; you can "
            f"also place a decrypted alpha.prx from your own copy in {decrypted_dir}.",
        )
        results = report["module_decryption"]["results"]
        self.assertEqual([(r["name"], r["status"], r["reason"]) for r in results],
                         [("alpha.prx", "failed", "encrypted")])
        self.assertIsNone(report["selected_executable"])
        self.assertNotIn("key_file", report)
        self.assertNotIn("built_in_boundary", report)
        for check in report["checks"]:
            self.assertNotIn("key file", check["message"])
        # Nothing decrypted, staged or rewritten.
        self.assertFalse(decrypted_dir.exists())
        self.assertFalse((user_root / "cache").exists())
        self.assertEqual(default_key.read_text(encoding="utf-8"), "synthetic test marker")
        self.assertEqual(override_key.read_text(encoding="utf-8"), "synthetic test marker")

        # The build route names both spellings and the same folder.
        manifest = {"modules": [{
            "name": "sceAlpha_library.prx", "role": "guest-prx", "required": True,
            "guest_path": "disc0:/PSP_GAME/SYSDIR/alpha.prx",
        }]}
        with patch.dict(os.environ, {"NAKAGAWA_PSP_KEY_FILE": str(override_key)}):
            with self.assertRaises(nk_cli.PackageBuildError) as raised:
                nk_cli._copy_optional_modules(
                    iso, manifest, self.temp_dir / "encrypted-cache", None,
                    decrypted_module_dir(user_root, "TEST00001"),
                )
        self.assertEqual(
            str(raised.exception),
            "ENCRYPTED_MODULE: Required guest PRX alpha.prx (sceAlpha_library.prx) is "
            "encrypted. The player decrypts it from your own disc when it checks the disc; "
            "you can also place a decrypted alpha.prx (or sceAlpha_library.prx) from your "
            f"own copy in {decrypted_module_dir(user_root, 'TEST00001')}.",
        )
        self.assertFalse(
            (self.temp_dir / "encrypted-cache" / "modules" / "sceAlpha_library.prx").exists()
        )

        # A plain copy the user supplies under the disc name makes it ready.
        decrypted_dir.mkdir(parents=True)
        plain = build_plain_mips_elf(0xFFA0, vaddr=0, entry=0xFFFFFFFF)
        (decrypted_dir / "alpha.prx").write_bytes(plain)
        (decrypted_dir / "EBOOT.elf").write_bytes(build_plain_mips_elf())
        ready = inspect_compatibility_preflight(iso, metadata=metadata, runtime_root=user_root)
        ready_checks = {check["code"]: check for check in ready["checks"]}
        self.assertEqual(ready_checks["EXECUTABLE"]["status"], "OK")
        self.assertEqual(ready["selected_executable"], "EBOOT.elf")
        self.assertEqual(ready_checks["GUEST_MODULES"]["status"], "OK")
        self.assertIn("1 of 1 ready", ready_checks["GUEST_MODULES"]["message"])
        output = nk_cli._copy_optional_modules(
            iso, manifest, self.temp_dir / "plain-cache", None,
            decrypted_module_dir(user_root, "TEST00001"),
        )
        self.assertEqual((output / "sceAlpha_library.prx").read_bytes(), plain)

    def test_checklist_and_extraction_agree_on_prx_modules(self) -> None:
        prx = build_plain_mips_elf(0xFFA0, vaddr=0, entry=0xFFFFFFFF)
        truncated = prx[:60]
        iso = self.temp_dir / "guest-modules.iso"
        create_test_iso_with_modules(
            iso, build_plain_mips_elf(),
            sysdir_modules={"libfont.prx": prx, "truncated.prx": truncated},
            usrdir_modules={},
        )
        user_root = self.temp_dir / "user-data"

        def spec(name: str) -> dict:
            return {"names": [name], "members": [("PSP_GAME", "SYSDIR", name)]}

        # Checklist, disc copies: the valid PRX is ready, the truncated one is not.
        checklist = check_needed_modules(
            iso, user_data_root=user_root, disc_id="TEST00001",
            modules=[spec("libfont.prx"), spec("truncated.prx")],
        )
        status = {result["name"]: result["status"] for result in checklist["results"]}
        self.assertEqual(status["libfont.prx"], "skipped")
        self.assertNotEqual(status["truncated.prx"], "skipped")

        # Extraction, disc copies: the same two modules, the same verdicts.
        for name, expect_ok in (("libfont.prx", True), ("truncated.prx", False)):
            with self.subTest(source="disc", module=name):
                manifest = {"modules": [{"name": name, "role": "guest-prx", "required": True}]}
                cache = self.temp_dir / f"disc-cache-{name}"
                if expect_ok:
                    output = nk_cli._copy_optional_modules(iso, manifest, cache, None)
                    self.assertEqual((output / name).read_bytes(), prx)
                else:
                    with self.assertRaisesRegex(nk_cli.PackageBuildError, "not a usable plain MIPS ELF32"):
                        nk_cli._copy_optional_modules(iso, manifest, cache, None)
                    self.assertFalse((cache / "modules" / name).exists())

        # Checklist and extraction, user-supplied decrypted copies.
        decrypted = decrypted_module_dir(user_root, "TEST00001")
        decrypted.mkdir(parents=True)
        for name, data, expect_ok in (("libfont.prx", prx, True),
                                      ("truncated.prx", truncated, False)):
            with self.subTest(source="user", module=name):
                (decrypted / name).write_bytes(data)
                checklist = check_needed_modules(
                    iso, user_data_root=user_root, disc_id="TEST00001",
                    modules=[spec(name)],
                )
                result = checklist["results"][0]
                manifest = {"modules": [{"name": name, "role": "guest-prx", "required": True}]}
                cache = self.temp_dir / f"user-cache-{name}"
                if expect_ok:
                    self.assertEqual((result["status"], result["reason"]), ("skipped", "user-supplied"))
                    output = nk_cli._copy_optional_modules(
                        iso, manifest, cache, decrypted
                    )
                    self.assertEqual((output / name).read_bytes(), data)
                else:
                    self.assertEqual(result["status"], "failed")
                    self.assertEqual(result["reason"], "user-supplied-invalid")
                    with self.assertRaisesRegex(nk_cli.PackageBuildError, "not a usable plain MIPS ELF32"):
                        nk_cli._copy_optional_modules(iso, manifest, cache, decrypted)
                    self.assertFalse((cache / "modules" / name).exists())


# nk_cli.py writes titles as UTF-8 whatever the host pipe encoding is (#732). On Windows a
# redirected Python stdout uses the ANSI code page (cp1252), so trademark and registered
# signs became single bytes and a UTF-8 reader saw U+FFFD. The child is forced to a cp1252
# pipe so the failure reproduces on any host.
CONSOLE_ENCODING_TITLE = "Café® ™"


class NkCliConsoleEncodingTests(unittest.TestCase):
    def run_inspect(self, iso: Path, pipe_encoding: str) -> bytes:
        env = os.environ.copy()
        env["PYTHONIOENCODING"] = pipe_encoding
        completed = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "inspect", str(iso)],
            cwd=str(ROOT), env=env, capture_output=True, timeout=120,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr.decode("utf-8", "replace"))
        return completed.stdout

    def test_piped_title_with_trademark_and_registered_signs_is_utf8(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk_cli_encoding_") as temp:
            iso = Path(temp) / "sign-title.iso"
            create_test_iso(iso, disc_id="TEST00002", title=CONSOLE_ENCODING_TITLE, volume_id="TEST00001")
            # A legacy ANSI pipe is what a Windows parent process gets by default.
            raw = self.run_inspect(iso, "cp1252")

        text = raw.decode("utf-8")  # strict: a stray cp1252 byte must fail here
        self.assertNotIn("�", text)
        title_line = next(line for line in text.splitlines() if line.startswith("Title:"))
        self.assertEqual(title_line, "Title:      " + CONSOLE_ENCODING_TITLE)

    def test_output_written_to_a_file_is_utf8_too(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk_cli_encoding_file_") as temp:
            temp_root = Path(temp)
            iso = temp_root / "sign-title.iso"
            create_test_iso(iso, disc_id="TEST00002", title=CONSOLE_ENCODING_TITLE, volume_id="TEST00001")
            out_path = temp_root / "inspect.txt"
            env = os.environ.copy()
            env["PYTHONIOENCODING"] = "cp1252"
            with out_path.open("wb") as out:
                completed = subprocess.run(
                    [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "inspect", str(iso)],
                    cwd=str(ROOT), env=env, stdout=out, stderr=subprocess.PIPE, timeout=120,
                )
            self.assertEqual(completed.returncode, 0, completed.stderr.decode("utf-8", "replace"))
            text = out_path.read_bytes().decode("utf-8")
        self.assertIn("Title:      " + CONSOLE_ENCODING_TITLE, text)


if __name__ == "__main__":
    unittest.main()
