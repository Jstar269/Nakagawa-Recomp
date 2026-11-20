# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Compressed disc images reach the Python tools through the C wrapper.

``tools/nk_core/disc_expand_tool.py`` builds ``nk_disc_expand`` from the same C
code the player runs (``src/core/nk_disc_expand.c``) and ``iso_inspect`` and
``nk_cli.py bringup`` expand a .cso, .zso or .chd through it before reading
the plain .iso. These tests build every image here from a synthetic ISO (no
game data): a CSO with real DEFLATE blocks from ``zlib``, a ZSO with LZ4 run
blocks, and a CHD v5 DVD image with an uncompressed map. The block decoders
themselves are covered in depth by ``tests/native/test_disc_expand.c``.

The writers are importable (``build_cso``, ``build_zso``, ``build_chd``) so
the player UI test makes its compressed image the same way.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest
from types import SimpleNamespace
from typing import Callable, ClassVar
from unittest import mock
import zlib

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from nk_core import disc_expand_tool, package_cache  # noqa: E402
from nk_core.iso_inspect import (
    IsoInspectionError, inspect_compatibility_preflight, inspect_iso,
)  # noqa: E402
from test_iso_parity import build_plain_mips_elf, create_test_iso_with_executables  # noqa: E402

SECTOR = 2048
PLAIN_FLAG = 0x80000000


def build_cso(iso: bytes, *, version: int = 1, align: int = 0) -> bytes:
    """A CISO image: raw DEFLATE blocks, stored blocks where DEFLATE does not shrink."""
    blocks = len(iso) // SECTOR
    out = bytearray(b"CISO" + struct.pack("<IQIBB2x", 24, len(iso), SECTOR, version, align))
    index_at = len(out)
    out += bytes((blocks + 1) * 4)
    entries = []
    for i in range(blocks + 1):
        while len(out) % (1 << align):
            out.append(0)
        entry = len(out) >> align
        if i < blocks:
            block = iso[i * SECTOR:(i + 1) * SECTOR]
            packer = zlib.compressobj(9, zlib.DEFLATED, -15)
            packed = packer.compress(block) + packer.flush()
            if len(packed) < SECTOR:
                out += packed
            else:
                out += block
                entry |= PLAIN_FLAG if version < 2 else 0
        entries.append(entry)
    struct.pack_into(f"<{blocks + 1}I", out, index_at, *entries)
    return bytes(out)


def _lz4_runs(block: bytes) -> bytes:
    """One LZ4 block: runs of a repeated byte become offset-1 matches."""
    out = bytearray()
    anchor = 0
    pos = 0
    end = len(block)

    def length_bytes(value: int) -> bytes:
        return b"\xff" * (value // 255) + bytes([value % 255])

    def sequence(literals: bytes, match: int) -> None:
        token = (min(len(literals), 15) << 4) | (min(match - 4, 15) if match else 0)
        out.append(token)
        if len(literals) >= 15:
            out.extend(length_bytes(len(literals) - 15))
        out.extend(literals)
        if match:
            out.extend(struct.pack("<H", 1))
            if match - 4 >= 15:
                out.extend(length_bytes(match - 19))

    while pos + 12 <= end:
        run = 0
        limit = end - 5 - (pos + 1)
        while run < limit and block[pos + 1 + run] == block[pos]:
            run += 1
        if run >= 4:
            sequence(block[anchor:pos + 1], run)
            pos += 1 + run
            anchor = pos
        else:
            pos += 1
    sequence(block[anchor:], 0)
    return bytes(out)


def build_zso(iso: bytes) -> bytes:
    """A ZISO image: LZ4 blocks, stored blocks where LZ4 does not shrink."""
    blocks = len(iso) // SECTOR
    out = bytearray(b"ZISO" + struct.pack("<IQIBB2x", 24, len(iso), SECTOR, 1, 0))
    index_at = len(out)
    out += bytes((blocks + 1) * 4)
    entries = []
    for i in range(blocks + 1):
        entry = len(out)
        if i < blocks:
            block = iso[i * SECTOR:(i + 1) * SECTOR]
            packed = _lz4_runs(block)
            if len(packed) < SECTOR:
                out += packed
            else:
                out += block
                entry |= PLAIN_FLAG
        entries.append(entry)
    struct.pack_into(f"<{blocks + 1}I", out, index_at, *entries)
    return bytes(out)


def build_chd(iso: bytes, *, metadata_tag: bytes = b"DVD ") -> bytes:
    """A CHD v5 image with an uncompressed map; all-zero hunks are left unstored."""
    hunk = 4096
    hunks = (len(iso) + hunk - 1) // hunk
    header = bytearray(124)
    header[0:8] = b"MComprHD"
    struct.pack_into(">II", header, 8, 124, 5)
    struct.pack_into(">QQQII", header, 32, len(iso), 0, 124, hunk, SECTOR)
    meta_text = b"synthetic\x00"
    meta = metadata_tag + struct.pack(">IQ", len(meta_text), 0) + meta_text
    map_offset = 124 + len(meta)
    struct.pack_into(">Q", header, 40, map_offset)
    out = bytearray(header + meta + bytes(hunks * 4))
    out += bytes(-len(out) % hunk)
    for index in range(hunks):
        data = iso[index * hunk:(index + 1) * hunk].ljust(hunk, b"\x00")
        if index and not any(data):
            continue
        struct.pack_into(">I", out, map_offset + index * 4, len(out) // hunk)
        out += data
    return bytes(out)


BUILDERS = {"cso": build_cso, "zso": build_zso, "chd": build_chd}


def _have_compiler() -> bool:
    return bool(os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc"))


def expanded_name_pattern(extension: str) -> str:
    """The per-title expansion's name: <DISC_ID>.<16 hex digits of the source's
    fingerprint>.<extension>.iso (src/core/nk_disc_expand.h)."""
    return rf"^TEST00042\.[0-9a-f]{{16}}\.{extension}\.iso$"


@unittest.skipUnless(_have_compiler(), "no C compiler to build nk_disc_expand (SKIP, not a pass)")
class DiscExpandToolTests(unittest.TestCase):
    _tmp: ClassVar[tempfile.TemporaryDirectory[str]]
    root: ClassVar[Path]
    iso_path: ClassVar[Path]
    iso: ClassVar[bytes]
    images: ClassVar[dict[str, Path]]

    @classmethod
    def setUpClass(cls) -> None:
        cls._tmp = tempfile.TemporaryDirectory(prefix="nk-disc-expand-")
        cls.root = Path(cls._tmp.name)
        cls.iso_path = cls.root / "synthetic.iso"
        create_test_iso_with_executables(
            cls.iso_path, b"synthetic input executable\n", disc_id="TEST00042",
            title="Synthetic Compressed Disc",
        )
        cls.iso = cls.iso_path.read_bytes()
        cls.images = {}
        for name, build in BUILDERS.items():
            image = cls.root / f"synthetic.{name}"
            image.write_bytes(build(cls.iso))
            cls.images[name] = image
        disc_expand_tool.find_or_build_tool()

    @classmethod
    def tearDownClass(cls) -> None:
        cls._tmp.cleanup()

    def test_magic_names_each_format_and_nothing_else(self) -> None:
        for name, image in self.images.items():
            with self.subTest(name=name):
                self.assertEqual(disc_expand_tool.compressed_format(image), name)
        self.assertIsNone(disc_expand_tool.compressed_format(self.iso_path))
        self.assertIsNone(disc_expand_tool.compressed_format(self.root / "missing.cso"))

    def test_probe_reports_the_expanded_size(self) -> None:
        for name, image in self.images.items():
            with self.subTest(name=name):
                result = disc_expand_tool.run_tool("probe", str(image))
                self.assertEqual(result, {"status": "OK", "format": name,
                                          "expanded_bytes": len(self.iso)})

    def test_import_expands_once_into_the_per_title_folder(self) -> None:
        for name, image in self.images.items():
            with self.subTest(name=name):
                data_root = self.root / f"data-{name}"
                first = disc_expand_tool.import_image(image, data_root)
                expected = Path(first["iso_path"])
                self.assertEqual(expected.parent, data_root / "discs" / "TEST00042")
                self.assertRegex(expected.name, expanded_name_pattern(name))
                self.assertEqual(first["disc_id"], "TEST00042")
                self.assertFalse(first["reused"])
                self.assertEqual(expected.read_bytes(), self.iso)
                again = disc_expand_tool.import_image(image, data_root)
                self.assertTrue(again["reused"])
                self.assertEqual(again["iso_path"], first["iso_path"])

    def test_iso_inspect_reads_each_format_as_its_iso(self) -> None:
        plain = inspect_iso(self.iso_path)
        for name, image in self.images.items():
            with self.subTest(name=name):
                data_root = self.root / f"inspect-{name}"
                info = inspect_iso(image, user_data_root=data_root)
                self.assertEqual((info.disc_id, info.title, info.version, info.size_bytes),
                                 (plain.disc_id, plain.title, plain.version, plain.size_bytes))
                self.assertEqual(info.container_metadata["format"], "iso9660")
                self.assertEqual(info.container_metadata["source_format"], name)
                identity = package_cache.build_title_input_identity(
                    manifest={"id": "synthetic-expand-test", "schema_version": 1},
                    executable_name="EBOOT.BIN",
                    executable_sha256="2" * 64,
                    modules=[],
                    container=dict(info.container_metadata),
                )
                self.assertNotIn("source_format", identity["container"])
                found = sorted((data_root / "discs" / "TEST00042").glob(f"TEST00042.*.{name}.iso"))
                self.assertEqual(len(found), 1, found)
                self.assertRegex(found[0].name, expanded_name_pattern(name))
                self.assertEqual(found[0].read_bytes(), self.iso)

    def test_preflight_reads_the_expanded_iso_for_each_compressed_input(self) -> None:
        for name, image in self.images.items():
            with self.subTest(name=name):
                data_root = self.root / f"preflight-{name}"
                metadata = inspect_iso(image, user_data_root=data_root)
                report = inspect_compatibility_preflight(
                    image, metadata=metadata, runtime_root=data_root
                )
                disc_check = next(
                    check for check in report["checks"] if check["code"] == "DISC_SFO"
                )
                self.assertEqual(disc_check["status"], "OK")

    def test_default_data_root_failure_is_a_named_iso_inspection_error(self) -> None:
        from nk_core import prereq_fetcher
        from nk_core.prereq_fetcher import PrerequisiteFetchError

        with mock.patch.object(
            prereq_fetcher, "default_data_root",
            side_effect=PrerequisiteFetchError("DATA_DIR_UNAVAILABLE: synthetic"),
        ):
            with self.assertRaises(IsoInspectionError) as caught:
                inspect_iso(self.images["cso"])

        self.assertEqual(caught.exception.boundary_code, "DATA_DIR_UNAVAILABLE")

    def test_a_refused_image_keeps_its_named_boundary(self) -> None:
        bad = bytearray(self.images["cso"].read_bytes())
        bad[20] = 3  # CSO format version 3
        path = self.root / "bad-version.cso"
        path.write_bytes(bytes(bad))
        with self.assertRaises(IsoInspectionError) as caught:
            inspect_iso(path, user_data_root=self.root / "data-bad")
        self.assertEqual(caught.exception.boundary_code, "DISC_IMAGE_VERSION_UNSUPPORTED")
        self.assertFalse((self.root / "data-bad" / "discs" / "TEST00042").exists())

    def test_a_cd_type_chd_is_refused_by_name(self) -> None:
        path = self.root / "cd-layout.chd"
        path.write_bytes(build_chd(self.iso, metadata_tag=b"CHT2"))
        with self.assertRaises(disc_expand_tool.DiscImageRefused) as caught:
            disc_expand_tool.import_image(path, self.root / "data-cd")
        self.assertEqual(caught.exception.code, "DISC_IMAGE_CHD_CD_UNSUPPORTED")

    def test_a_plain_iso_passes_through_unchanged(self) -> None:
        self.assertEqual(disc_expand_tool.ensure_plain_iso(self.iso_path, self.root / "unused"),
                         self.iso_path)
        self.assertFalse((self.root / "unused").exists())


class DiscExpandBuildCacheTests(unittest.TestCase):
    def test_libchdr_header_change_rebuilds_the_cached_tool(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-disc-expand-cache-") as tmpdir:
            root = Path(tmpdir)
            inputs = (
                disc_expand_tool.CORE_SOURCES
                + (disc_expand_tool._platform_source(),)
                + disc_expand_tool.CORE_HEADERS
                + disc_expand_tool.LIBCHDR_SOURCES
            )
            for relative in inputs:
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(relative.encode("utf-8"))
            header = root / "third_party" / "libchdr" / "include" / "libchdr" / "cache-test.h"
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_bytes(b"header revision one")
            builds: list[list[str]] = []

            def fake_compile(command, **_options):
                builds.append(command)
                output = Path(command[command.index("-o") + 1])
                output.parent.mkdir(parents=True, exist_ok=True)
                output.write_bytes(b"synthetic tool")
                return SimpleNamespace(returncode=0, stdout="", stderr="")

            with (
                mock.patch.object(disc_expand_tool, "_compiler", return_value="synthetic-cc"),
                mock.patch.object(disc_expand_tool.subprocess, "run", side_effect=fake_compile),
            ):
                disc_expand_tool.find_or_build_tool(root)
                first_build_count = len(builds)
                header.write_bytes(b"header revision two")
                disc_expand_tool.find_or_build_tool(root)

            self.assertGreater(len(builds), first_build_count)


@unittest.skipUnless(_have_compiler(), "no C compiler to build nk_disc_expand (SKIP, not a pass)")
class BringupAcceptsCompressedImagesTests(unittest.TestCase):
    def setUp(self) -> None:
        import nk_cli

        self.nk_cli = nk_cli
        tmp = tempfile.TemporaryDirectory(prefix="nk-disc-bringup-")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.iso_path = self.root / "synthetic.iso"
        create_test_iso_with_executables(
            self.iso_path, b"synthetic input executable\n", disc_id="TEST00042",
            title="Synthetic Compressed Disc",
        )
        self.iso = self.iso_path.read_bytes()

    def _bringup(self, image: Path, case: str):
        report_path = self.root / case / "bringup.json"
        args = argparse.Namespace(
            iso=str(image), work_dir=str(self.root / case / "work"), report=str(report_path),
            launch_timeout=1, instruction_trace=False,
        )
        seen = []
        # The inspect stage passes the compressed handling, then stops at a
        # refused executable, so no package is built.
        preflight = {
            "checks": [
                {"code": "DISC_SFO", "status": "OK", "issues": []},
                {"code": "EXECUTABLE", "status": "UNSUPPORTED", "issues": [308]},
            ],
            "selected_executable": None,
            "modified_dump_cfw_loader": False,
        }

        def fake_preflight(iso_path, **_kwargs):
            seen.append(Path(iso_path))
            return preflight

        with (
            mock.patch.object(self.nk_cli, "inspect_compatibility_preflight",
                              side_effect=fake_preflight),
            mock.patch("builtins.print"),
        ):
            status = self.nk_cli.cmd_bringup(args)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.nk_cli.validate_bringup_report(report)
        return status, report, seen

    def test_bringup_reads_the_expanded_iso_of_each_format(self) -> None:
        for name, build in BUILDERS.items():
            with self.subTest(name=name):
                image = self.root / f"synthetic.{name}"
                image.write_bytes(build(self.iso))
                status, report, seen = self._bringup(image, f"case-{name}")
                self.assertEqual(status, 1)
                self.assertEqual(report["failure_class"], "EXECUTABLE_UNSUPPORTED")
                self.assertEqual(len(seen), 1)
                self.assertRegex(seen[0].name, expanded_name_pattern(name))
                self.assertEqual(seen[0].read_bytes(), self.iso)
                user_data = (self.root / f"case-{name}" / "work" / "user-data").resolve()
                self.assertTrue(seen[0].resolve().is_relative_to(user_data))

    def test_bringup_names_a_refused_image(self) -> None:
        bad = bytearray(build_cso(self.iso))
        struct.pack_into("<Q", bad, 8, 1 << 62)  # an expanded size no disc has
        image = self.root / "bad-size.cso"
        image.write_bytes(bytes(bad))
        status, report, seen = self._bringup(image, "case-bad")
        self.assertEqual(status, 1)
        self.assertEqual(report["reached_stage"], "inspect")
        self.assertEqual(report["failure_class"], "DISC_IMAGE_REFUSED")
        self.assertEqual(seen, [])


class PrototypeExpansionFollowsItsEntryKeyTests(unittest.TestCase):
    """Review R1 of PR #2855: the command-line route (``nk_disc_expand import``, reached
    through ``ensure_plain_iso`` by ``nk_cli.py inspect`` and the bring-up) keys an
    expansion by the library in its data folder, as the player's route does. A prototype
    disc whose entry holds its v1 key is expanded into ``discs/<v1 key>``, and an
    expansion recorded earlier under its v2 key moves there. Removing the title removes
    ``discs/<the entry's key>`` (the player's removal plan, ``player_state.c``), so
    nothing is left under ``discs/`` but the records in ``discs/expanded``."""

    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory(prefix="nk-disc-proto-")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        iso_path = self.root / "prototype.iso"
        create_test_iso_with_executables(
            iso_path, bytes(2048), build_plain_mips_elf(), disc_id="PROTO1234567",
            title="Synthetic Beta",
        )
        meta = inspect_iso(iso_path)
        self.v2_key, self.v1_key = meta.disc_id, meta.legacy_disc_id
        self.assertEqual(meta.disc_key_version, 2)
        self.image = self.root / "prototype.cso"
        self.image.write_bytes(build_cso(iso_path.read_bytes()))

    def _user_root(self, name: str, records: list[dict] | None) -> Path:
        root = self.root / name
        root.mkdir(parents=True)
        if records is not None:
            (root / "library.json").write_bytes(
                json.dumps({"schema_version": 1, "games": records}).encode("utf-8"))
        return root

    def _old_entry(self) -> dict:
        return {"disc_id": self.v1_key, "title_name": "Synthetic Beta",
                "iso_path": str(self.root / "elsewhere.iso")}

    @staticmethod
    def _title_folders(root: Path) -> list[str]:
        discs = root / "discs"
        return sorted(p.name for p in discs.iterdir() if p.name != "expanded") \
            if discs.is_dir() else []

    def _remove_title(self, root: Path, key: str) -> list[str]:
        """The player's removal of the title's disc copy: discs/<the entry's key>."""
        shutil.rmtree(root / "discs" / key, ignore_errors=False)
        return self._title_folders(root)

    def test_cli_inspect_expands_under_the_entry_key(self) -> None:
        root = self._user_root("inspect", [self._old_entry()])
        meta = inspect_iso(self.image, user_data_root=root)
        self.assertEqual(meta.legacy_disc_id, self.v1_key)
        self.assertEqual(self._title_folders(root), [self.v1_key],
                         "the expansion goes into the folder of the key the entry holds")
        self.assertEqual(self._remove_title(root, self.v1_key), [])

    def test_bringup_expands_under_the_entry_key(self) -> None:
        import nk_cli

        work = self.root / "bringup" / "work"
        user_root = work / "user-data"
        user_root.mkdir(parents=True)
        (user_root / "library.json").write_bytes(json.dumps(
            {"schema_version": 1, "games": [self._old_entry()]}).encode("utf-8"))
        args = argparse.Namespace(
            iso=str(self.image), work_dir=str(work), report=str(self.root / "bringup.json"),
            launch_timeout=1, instruction_trace=False,
        )
        preflight = {
            "checks": [{"code": "DISC_SFO", "status": "OK", "issues": []},
                       {"code": "EXECUTABLE", "status": "UNSUPPORTED", "issues": [308]}],
            "selected_executable": None, "modified_dump_cfw_loader": False,
        }
        with (
            mock.patch.object(nk_cli, "inspect_compatibility_preflight", return_value=preflight),
            mock.patch("builtins.print"),
        ):
            self.assertEqual(nk_cli.cmd_bringup(args), 1)
        self.assertEqual(self._title_folders(user_root), [self.v1_key])
        self.assertEqual(self._remove_title(user_root, self.v1_key), [])

    def test_an_earlier_expansion_moves_to_the_entry_key(self) -> None:
        root = self._user_root("moves", None)
        first = disc_expand_tool.import_image(self.image, root)
        self.assertEqual((first["disc_id"], first["reused"]), (self.v2_key, False))
        self.assertEqual(self._title_folders(root), [self.v2_key])
        (root / "library.json").write_bytes(json.dumps(
            {"schema_version": 1, "games": [self._old_entry()]}).encode("utf-8"))
        again = disc_expand_tool.import_image(self.image, root)
        self.assertEqual((again["disc_id"], again["reused"]), (self.v1_key, True))
        self.assertTrue(Path(again["iso_path"]).is_file())
        self.assertEqual(Path(again["iso_path"]).parent.name, self.v1_key)
        self.assertEqual(self._title_folders(root), [self.v1_key])
        self.assertEqual(self._remove_title(root, self.v1_key), [])

    def test_a_v2_entry_keeps_the_v2_folder(self) -> None:
        root = self._user_root("v2", [self._old_entry(), {
            "disc_id": self.v2_key, "iso_path": str(self.root / "newer.iso"),
            "disc_key_version": 2}])
        result = disc_expand_tool.import_image(self.image, root)
        self.assertEqual(result["disc_id"], self.v2_key)
        self.assertEqual(self._title_folders(root), [self.v2_key])

    def _expanded_under_v2(self, name: str) -> tuple[Path, Path, dict]:
        """An expansion recorded under the v2 key, then a library whose entry holds v1."""
        root = self._user_root(name, None)
        first = disc_expand_tool.import_image(self.image, root)
        self.assertEqual(first["disc_id"], self.v2_key)
        (root / "library.json").write_bytes(json.dumps(
            {"schema_version": 1, "games": [self._old_entry()]}).encode("utf-8"))
        return root, Path(str(first["iso_path"])), first

    def _destination(self, root: Path, iso: Path, identity: str | None = None) -> Path:
        """The name the move takes: the short one, or with identity the long one."""
        key = str(self.v1_key)
        parts = iso.name.split(".")
        parts[0] = key
        if identity is not None:
            parts[1] = identity
        return root / "discs" / key / ".".join(parts)

    def _assert_move_failed_and_left_usable(self, root: Path, iso: Path, code: str,
                                            run: Callable[[], object]) -> None:
        """The failed move named its error and left the earlier expansion where its record
        names it: an import without the library reuses it there."""
        before = iso.read_bytes()
        with self.assertRaises(disc_expand_tool.DiscImageRefused) as caught:
            run()
        self.assertEqual(caught.exception.code, code, str(caught.exception))
        self.assertTrue(iso.is_file(), "the earlier expansion is still there")
        self.assertEqual(iso.read_bytes(), before)
        self.assertTrue(Path(f"{iso}.source").is_file(), "and so is its record")
        (root / "library.json").unlink()
        again = disc_expand_tool.import_image(self.image, root)
        self.assertEqual((again["reused"], Path(str(again["iso_path"]))), (True, iso))

    def test_a_move_whose_name_is_being_placed_elsewhere_keeps_the_expansion(self) -> None:
        """Review S1 of PR #2855: the name lock of the destination is held by another
        import, so nothing moves."""
        root, iso, _first = self._expanded_under_v2("move-busy")
        lock_path = root / "discs" / "expanded" / (self._destination(root, iso).name[:-4] + ".lock")
        with lock_path.open("a+b") as held:
            held.seek(0)
            if sys.platform == "win32":
                import msvcrt
                msvcrt.locking(held.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(held.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            self._assert_move_failed_and_left_usable(
                root, iso, "DISC_IMAGE_IMPORT_BUSY",
                lambda: disc_expand_tool.import_image(self.image, root))
            held.seek(0)
            if sys.platform == "win32":
                msvcrt.locking(held.fileno(), msvcrt.LK_UNLCK, 1)

    def test_a_move_whose_folder_cannot_be_made_keeps_the_expansion(self) -> None:
        root, iso, _first = self._expanded_under_v2("move-no-folder")
        (root / "discs" / self.v1_key).write_bytes(b"not a folder")
        self._assert_move_failed_and_left_usable(
            root, iso, "DISC_IMAGE_WRITE_FAILED",
            lambda: disc_expand_tool.import_image(self.image, root))

    def test_a_move_that_cannot_rename_keeps_the_expansion(self) -> None:
        """Both names the image could take are folders in use, so the rename fails."""
        root, iso, first = self._expanded_under_v2("move-rename")
        for target in (self._destination(root, iso),
                       self._destination(root, iso, str(first["identity"]))):
            target.mkdir(parents=True)
            (target / "in-use").write_bytes(b"x")
        self._assert_move_failed_and_left_usable(
            root, iso, "DISC_IMAGE_WRITE_FAILED",
            lambda: disc_expand_tool.import_image(self.image, root))

    def test_a_move_whose_record_cannot_be_written_is_put_back(self) -> None:
        """The image moves, its record cannot be written at the new name, so it is moved
        back and its own record written again."""
        root, iso, first = self._expanded_under_v2("move-record")
        short = self._destination(root, iso)
        short.mkdir(parents=True)
        (short / "in-use").write_bytes(b"x")
        long_name = self._destination(root, iso, str(first["identity"]))
        blocked = Path(f"{long_name}.source")
        blocked.mkdir()
        (blocked / "in-use").write_bytes(b"x")
        self._assert_move_failed_and_left_usable(
            root, iso, "DISC_IMAGE_WRITE_FAILED",
            lambda: disc_expand_tool.import_image(self.image, root))
        self.assertFalse(long_name.exists(), "nothing is left at the name it was moved to")

    def test_an_old_folder_that_holds_another_file_is_left_alone(self) -> None:
        """Review S2 of PR #2855: the folder the expansion leaves is removed only when it
        is empty; a file another import placed there stays."""
        root, iso, _first = self._expanded_under_v2("move-sibling")
        sibling = iso.parent / f"{self.v2_key}.0000000000000000.cso.iso"
        sibling.write_bytes(b"another import")
        moved = disc_expand_tool.import_image(self.image, root)
        self.assertEqual((moved["disc_id"], moved["reused"]), (self.v1_key, True))
        self.assertFalse(iso.exists())
        self.assertEqual(sibling.read_bytes(), b"another import")
        self.assertEqual(self._title_folders(root), sorted([self.v1_key, self.v2_key]))

    def test_a_library_a_newer_nakagawa_wrote_refuses_by_name(self) -> None:
        root = self._user_root("newer", None)
        (root / "library.json").write_bytes(
            json.dumps({"schema_version": 99, "games": []}).encode("utf-8"))
        with self.assertRaises(disc_expand_tool.DiscImageRefused) as caught:
            disc_expand_tool.import_image(self.image, root)
        self.assertEqual(caught.exception.code, "IO_FAILURE")
        self.assertIn("library could not be read", str(caught.exception))
        self.assertEqual(self._title_folders(root), [])


if __name__ == "__main__":
    unittest.main()
