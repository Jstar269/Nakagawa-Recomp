# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""nk_core.disc_verify and the bring-up's library records keyed by disc ID plus hash.

Synthetic data, a datafile in Redump's layout naming made-up images, for every case but the
bundled list's: ``BundledListTests`` reads the real list the project ships (assets/redump),
checks it against its pin and uses it as the default, and ``ConformanceTests`` cuts the
mutations' starting points from it. ``ConformanceTests`` holds this reader to
tests/native/disc_dat_conformance.txt, which tests/native/test_disc_verify.c holds the C reader
(src/core/nk_disc_verify.c) to: one table of inputs and results for both. Regenerate the table
with ``python tools/test_disc_verify.py --write-corpus`` after a deliberate change to the format.
"""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import re
import struct
import sys
import tempfile
import tracemalloc
import unittest
from typing import Callable, Iterable, Optional, cast
from unittest import mock
import zipfile
import zlib

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from nk_core import disc_verify  # noqa: E402

RETAIL = bytes((index * 31 + 7) & 0xFF for index in range(8192))
MODIFIED = RETAIL[:4096] + bytes([RETAIL[4096] ^ 0x5A]) + RETAIL[4097:]
OTHER = bytes((index * 17 + 3) & 0xFF for index in range(8192))


def digest(data: bytes) -> disc_verify.DiscDigest:
    return disc_verify.DiscDigest(sha1=hashlib.sha1(data).hexdigest(),
                                  crc32=f"{zlib.crc32(data):08x}", size_bytes=len(data))


def datafile(serials: bool = True) -> str:
    retail = digest(RETAIL)
    serial = "\t\t<serial>TEST-70001, TEST-70001GA</serial>\n" if serials else ""
    return (
        '<?xml version="1.0"?>\n'
        '<!DOCTYPE datafile PUBLIC "-//Logiqx//DTD ROM Management Datafile//EN" '
        '"http://www.logiqx.com/Dats/datafile.dtd">\n'
        "<datafile>\n\t<header>\n\t\t<name>Synthetic &amp; Test</name>\n"
        "\t\t<version>2026-10-31</version>\n\t</header>\n"
        '\t<game name="Retail Disc (USA)">\n\t\t<category>Games</category>\n'
        f"{serial}"
        f'\t\t<rom name="Retail Disc (USA).iso" size="{retail.size_bytes}" crc="{retail.crc32}" '
        f'md5="0" sha1="{retail.sha1}"/>\n\t</game>\n'
        '\t<game name="Bad Row">\n\t\t<rom name="Bad.iso" size="12" crc="zz" sha1="not-a-hash"/>\n'
        "\t</game>\n"
        f'\t<game name="Repeated">\n\t\t<rom name="Again.iso" size="{retail.size_bytes}" '
        f'crc="{retail.crc32}" sha1="{retail.sha1}"/>\n\t</game>\n'
        '\t<game name="Elsewhere (Japan)">\n\t\t<rom name="Elsewhere.iso" size="4" crc="00000001" '
        'sha1="00112233445566778899aabbccddeeff00112233"/>\n\t</game>\n'
        "</datafile>\n"
    )


class DatafileTests(unittest.TestCase):
    def test_reader_takes_hashes_sizes_and_serials(self) -> None:
        dat = disc_verify.parse_dat_text(datafile())
        self.assertEqual((dat.name, dat.version), ("Synthetic & Test", "2026-10-31"))
        self.assertEqual((dat.games, dat.rejected_roms, len(dat)), (4, 1, 2))
        self.assertEqual(dat.serials, frozenset({"TEST70001"}))
        rom = dat.roms_by_sha1[digest(RETAIL).sha1]
        self.assertEqual((rom.name, rom.size_bytes, rom.serials),
                         ("Retail Disc (USA).iso", 8192, ("TEST70001",)))

    def test_the_reader_follows_the_c_reader_on_edge_cases(self) -> None:
        retail = digest(RETAIL)
        rom = f'<rom name="A &amp; B &#38; C" size="{{size}}" crc="{retail.crc32}" sha1="{retail.sha1}"/>'
        many = ", ".join(f"TEST-7{index:04d}" for index in range(16))
        text = (f"<datafile><game name=\"x\"><serial>{many}, TEST&#45;79999</serial>"
                f"{rom.format(size=len(RETAIL))}</game>"
                f"<game name=\"y\">{rom.format(size=2**64).replace(retail.sha1, 'f' * 40)}"
                "</game></datafile>")
        dat = disc_verify.parse_dat_text(text)
        # Only the five predefined entities are decoded, as in C.
        self.assertEqual(dat.roms_by_sha1[retail.sha1].name, "A & B &#38; C")
        # Sixteen disc IDs, and a numeric reference is not decoded into one.
        self.assertEqual(len(dat.serials), 16)
        self.assertNotIn("TEST79999", dat.serials)
        # A size past 64 bits is a malformed row.
        self.assertEqual((len(dat), dat.rejected_roms), (1, 1))
        # Seventeen IDs in one <serial>, or a second <serial>, are refused (rule 9).
        for refused in (many + ", TEST-79998", "TEST-70000</serial><serial>TEST-70001"):
            with self.assertRaises(disc_verify.DiscVerifyError) as raised:
                disc_verify.parse_dat_text(text.replace(many + ", TEST&#45;79999", refused))
            self.assertEqual(raised.exception.boundary_code, "DISC_DAT_NOT_DATAFILE")

    def test_disc_ids_match_after_ascii_upper_casing_only(self) -> None:
        dat = disc_verify.parse_dat_text(datafile())
        changed = digest(MODIFIED)
        self.assertEqual(disc_verify.classify("test70001", changed, dat), disc_verify.MODIFIED)
        # U+017F (long s) upper-cases to "S" in Unicode, not in ASCII: no match.
        self.assertEqual(disc_verify.classify("TE\u017fT70001", changed, dat), disc_verify.UNKNOWN)

    def test_serials_normalize_to_the_disc_id_param_sfo_holds(self) -> None:
        self.assertEqual(
            disc_verify.normalize_serials("TEST-71002, TEST-71002GA, test 71003,BAD,TEST-7100"),
            ("TEST71002", "TEST71003"))

    def test_refusals_are_named(self) -> None:
        with self.assertRaises(disc_verify.DiscVerifyError) as raised:
            disc_verify.parse_dat_text("<html>nothing</html>")
        self.assertEqual(raised.exception.boundary_code, "DISC_DAT_NOT_DATAFILE")
        with self.assertRaises(disc_verify.DiscVerifyError) as raised:
            disc_verify.parse_dat_text('<datafile><game name="x"><rom size="1"/></game></datafile>')
        self.assertEqual(raised.exception.boundary_code, "DISC_DAT_EMPTY")
        # A list cut short is refused, at a game's end or inside one, as the C reader does.
        whole = datafile()
        last_game = whole.rindex("<game")
        for cut in (whole[:last_game], whole[:whole.rindex("<rom")] + "</datafile>\n",
                    whole[:whole.rindex("</datafile>") + len("</datafile")]):
            with self.assertRaises(disc_verify.DiscVerifyError) as raised:
                disc_verify.parse_dat_text(cut)
            self.assertEqual(raised.exception.boundary_code, "DISC_DAT_NOT_DATAFILE")
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "big.dat"
            path.write_text(datafile(), encoding="utf-8")
            with mock.patch.object(disc_verify, "MAX_DAT_BYTES", 64):
                with self.assertRaises(disc_verify.DiscVerifyError) as raised:
                    disc_verify.load_dat(path)
            self.assertEqual(raised.exception.boundary_code, "DISC_DAT_TOO_LARGE")
            with self.assertRaises(disc_verify.DiscVerifyError) as raised:
                disc_verify.load_dat(Path(folder) / "missing.dat")
            self.assertEqual(raised.exception.boundary_code, "DISC_DAT_READ_FAILED")


class TierTests(unittest.TestCase):
    def setUp(self) -> None:
        self.dat = disc_verify.parse_dat_text(datafile())

    def test_verified_modified_unknown_and_not_checked(self) -> None:
        classify = disc_verify.classify
        self.assertEqual(classify("TEST70001", digest(RETAIL), self.dat),
                         disc_verify.VERIFIED_RETAIL)
        self.assertEqual(classify("TEST70001", digest(MODIFIED), self.dat), disc_verify.MODIFIED)
        self.assertEqual(classify("TEST70009", digest(OTHER), self.dat), disc_verify.UNKNOWN)
        self.assertEqual(classify("TEST70009", digest(OTHER), self.dat, {"TEST70009"}),
                         disc_verify.MODIFIED)
        self.assertEqual(classify("TEST70001", digest(RETAIL), None), disc_verify.NOT_CHECKED)
        self.assertEqual(classify("TEST70001", None, self.dat), disc_verify.NOT_CHECKED)

    def test_a_matching_sha1_with_another_crc_or_size_is_not_verified(self) -> None:
        good = digest(RETAIL)
        bent_crc = disc_verify.DiscDigest(good.sha1, "00000000", good.size_bytes)
        bent_size = disc_verify.DiscDigest(good.sha1, good.crc32, good.size_bytes + 2048)
        for bent in (bent_crc, bent_size):
            self.assertEqual(disc_verify.classify("TEST70009", bent, self.dat),
                             disc_verify.UNKNOWN)

    def test_a_serial_less_list_learns_retail_ids_from_verified_images(self) -> None:
        dat = disc_verify.parse_dat_text(datafile(serials=False))
        images = {"translation": ("TEST70001", digest(MODIFIED)),
                  "retail": ("TEST70001", digest(RETAIL)),
                  "other": ("TEST70009", digest(OTHER)),
                  "unread": ("TEST70002", None)}
        expected = {"translation": disc_verify.MODIFIED, "retail": disc_verify.VERIFIED_RETAIL,
                    "other": disc_verify.UNKNOWN, "unread": disc_verify.NOT_CHECKED}
        self.assertEqual(disc_verify.classify_all(images, dat), expected)
        # The order images arrive in never changes a tier.
        self.assertEqual(disc_verify.classify_all(dict(reversed(images.items())), dat), expected)
        self.assertEqual(disc_verify.classify("TEST70001", digest(MODIFIED), dat),
                         disc_verify.UNKNOWN)


class HashAndImportTests(unittest.TestCase):
    def test_whole_image_hash_matches_sha1_and_crc32(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            image = Path(folder) / "disc.iso"
            data = RETAIL * 300  # several read chunks, not a whole number of them
            image.write_bytes(data)
            seen: list[tuple[int, int]] = []
            result = disc_verify.hash_image(image, on_progress=lambda done, total:
                                            seen.append((done, total)))
            self.assertEqual(result, digest(data))
            self.assertEqual(seen[-1], (len(data), len(data)))
            with self.assertRaises(disc_verify.DiscVerifyError) as raised:
                disc_verify.hash_image(Path(folder) / "missing.iso")
            self.assertEqual(raised.exception.boundary_code, "DISC_IMAGE_READ_FAILED")
        # The CRC-32 check value, as the C reader's test checks it.
        self.assertEqual(f"{zlib.crc32(b'123456789'):08x}", "cbf43926")

    def test_nk_cli_refuses_a_list_with_control_characters_and_prints_none(self) -> None:
        import argparse
        import contextlib
        import io

        import nk_cli

        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "chosen.dat"
            source.write_text(datafile().replace("<version>2026-10-31</version>",
                                                 "<version>1\x1b[31m\x07red</version>"),
                              encoding="utf-8")
            shown, errors = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(shown), contextlib.redirect_stderr(errors):
                self.assertEqual(nk_cli.cmd_redump_import(argparse.Namespace(
                    datafile=source, user_data_root=root / "data")), 1)
            # Rule 3: a control byte anywhere refuses the list, named by its value.
            self.assertIn("[DISC_DAT_NOT_DATAFILE]", errors.getvalue())
            self.assertIn("(byte 0x1B)", errors.getvalue())
            for output in (shown.getvalue(), errors.getvalue()):
                self.assertNotIn("\x1b", output)
                self.assertNotIn("\x07", output)
            self.assertFalse(disc_verify.imported_dat_path(root / "data").exists())

    def test_import_saves_the_bytes_it_checked(self) -> None:
        # The file is read once: a source that changes after the check can't put
        # unchecked bytes in the data folder.
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "chosen.dat"
            checked = datafile().encode("utf-8")
            source.write_bytes(checked)
            real_parse = disc_verify.parse_dat_bytes

            def parse_then_change(data: bytes) -> disc_verify.RedumpDat:
                dat = real_parse(data)
                source.write_bytes(b"not a datafile, written after the check")
                return dat

            with mock.patch.object(disc_verify, "parse_dat_bytes", parse_then_change):
                disc_verify.import_dat(source, root / "data")
            self.assertEqual((root / "data" / "verification" / "redump.dat").read_bytes(),
                             checked)

    def test_import_keeps_a_whole_copy_in_the_data_folder(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "chosen.dat"
            source.write_text(datafile(), encoding="utf-8")
            data = root / "data"
            self.assertIsNone(disc_verify.load_imported_dat(data))
            self.assertEqual(len(disc_verify.import_dat(source, data)), 2)
            imported = data / "verification" / "redump.dat"
            self.assertEqual(imported.read_bytes(), source.read_bytes())
            bad = root / "bad.dat"
            bad.write_text("hello", encoding="utf-8")
            with self.assertRaises(disc_verify.DiscVerifyError):
                disc_verify.import_dat(bad, data)
            self.assertEqual(imported.read_bytes(), source.read_bytes())
            source.write_text(datafile(serials=False), encoding="utf-8")
            disc_verify.import_dat(source, data)
            # A verification folder that links elsewhere is refused; the file there is kept.
            outside = root / "outside"
            outside.mkdir()
            (outside / "redump.dat").write_text("kept", encoding="utf-8")
            linked = root / "linked"
            linked.mkdir()
            try:
                (linked / "verification").symlink_to(outside, target_is_directory=True)
            except OSError:
                pass  # this host cannot make a symlink without privilege
            else:
                with self.assertRaises(disc_verify.DiscVerifyError):
                    disc_verify.import_dat(source, linked)
                self.assertEqual((outside / "redump.dat").read_text(encoding="utf-8"), "kept")
                # Nor is a link left where an earlier import named its temporary file.
                planted = data / "verification" / f".redump.dat.{os.getpid()}.tmp"
                planted.symlink_to(outside / "redump.dat")
                disc_verify.import_dat(source, data)
                self.assertEqual((outside / "redump.dat").read_text(encoding="utf-8"), "kept")
                self.assertTrue(planted.is_symlink())
                planted.unlink()
            self.assertEqual(disc_verify.load_imported_dat(data).serials, frozenset())
            self.assertEqual(sorted(path.name for path in imported.parent.iterdir()),
                             ["redump.dat"])


class BringupLibraryTests(unittest.TestCase):
    """nk_cli's library write: the same rules as src/core/nk_library.c."""

    def setUp(self) -> None:
        import nk_cli

        self.nk_cli = nk_cli
        self._tmp = tempfile.TemporaryDirectory(prefix="disc-verify-library-")
        self.root = Path(self._tmp.name)
        (self.root / "chosen.dat").write_text(datafile(serials=False), encoding="utf-8")
        disc_verify.import_dat(self.root / "chosen.dat", self.root)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def write(self, iso: str, data: bytes | None, disc_id: str = "TEST70001", *,
              hashed: bool = True) -> None:
        """A bring-up adding the image `data` at `iso`; hashed=False is one whose hash failed
        (or a record an older Nakagawa wrote, which never hashed)."""
        path = self.root / iso
        if data is not None:
            path.write_bytes(data)
        metadata = mock.Mock(disc_id=disc_id, title=f"Title {iso}", version="1.00")
        self.nk_cli._write_bringup_library_locked(
            self.root, path, metadata, f"{disc_id}-id", "EBOOT.BIN", True, "",
            digest(path.read_bytes()) if hashed else None)

    def games(self) -> list[dict]:
        return cast(list[dict], json.loads(
            (self.root / "library.json").read_text(encoding="utf-8")
        )["games"])

    def by_file(self) -> dict[str, dict]:
        return {Path(game["iso_path"]).name: game for game in self.games()}

    def test_a_fan_translation_and_its_retail_disc_are_both_kept(self) -> None:
        self.write("translation.iso", MODIFIED)
        self.assertEqual([game["disc_verification"] for game in self.games()],
                         [disc_verify.UNKNOWN])
        self.write("retail.iso", RETAIL)
        games = self.by_file()
        self.assertEqual(sorted(games), ["retail.iso", "translation.iso"])
        self.assertEqual(games["retail.iso"]["disc_verification"], disc_verify.VERIFIED_RETAIL)
        self.assertEqual(games["translation.iso"]["disc_verification"], disc_verify.MODIFIED)
        self.assertEqual(games["retail.iso"]["content_sha1"], digest(RETAIL).sha1)
        self.assertEqual(games["retail.iso"]["content_crc32"], digest(RETAIL).crc32)
        self.assertEqual(games["retail.iso"]["iso_size_bytes"], len(RETAIL))
        # The same image again replaces its own record only.
        self.write("retail.iso", RETAIL)
        self.assertEqual(len(self.games()), 2)
        # The build picks the record of the image it was given, and refuses to guess.
        entry = self.nk_cli._load_library_entry(self.root, "TEST70001",
                                                self.root / "translation.iso")
        self.assertEqual(entry["content_sha1"], digest(MODIFIED).sha1)
        with self.assertRaisesRegex(self.nk_cli.PackageBuildError, "--iso-path"):
            self.nk_cli._load_library_entry(self.root, "TEST70001")
        with self.assertRaisesRegex(self.nk_cli.PackageBuildError, "--iso-path"):
            self.nk_cli._load_library_entry(self.root, "TEST70001", self.root / "other.iso")

    def test_a_library_built_before_hashing_keeps_its_retail_record(self) -> None:
        # Resuming an older build: the retail record has no hash, then its fan translation
        # (once a duplicate) is built. The retail file is hashed now and both are kept.
        self.write("retail.iso", RETAIL, hashed=False)
        self.assertNotIn("content_sha1", self.games()[0])
        self.write("translation.iso", MODIFIED)
        games = self.by_file()
        self.assertEqual(sorted(games), ["retail.iso", "translation.iso"])
        self.assertEqual(games["retail.iso"]["content_sha1"], digest(RETAIL).sha1)
        self.assertEqual(games["retail.iso"]["disc_verification"], disc_verify.VERIFIED_RETAIL)
        self.assertEqual(games["translation.iso"]["disc_verification"], disc_verify.MODIFIED)

    def test_an_older_record_of_the_same_image_elsewhere_is_replaced(self) -> None:
        self.write("old.iso", RETAIL, hashed=False)
        self.write("copy.iso", RETAIL)
        self.assertEqual(sorted(self.by_file()), ["copy.iso"])

    def test_an_older_record_whose_file_is_gone_is_kept(self) -> None:
        self.write("gone.iso", RETAIL, hashed=False)
        (self.root / "gone.iso").unlink()
        self.write("new.iso", OTHER)
        self.assertEqual(sorted(self.by_file()), ["gone.iso", "new.iso"])

    def test_a_failed_hash_replaces_one_record_at_most(self) -> None:
        self.write("retail.iso", RETAIL)
        self.write("translation.iso", MODIFIED)
        # Its own file again, unhashed this time: that record only.
        self.write("translation.iso", MODIFIED, hashed=False)
        games = self.by_file()
        self.assertEqual(sorted(games), ["retail.iso", "translation.iso"])
        self.assertEqual(games["retail.iso"]["content_sha1"], digest(RETAIL).sha1)
        self.assertNotIn("content_sha1", games["translation.iso"])
        # A new file, unhashed, beside two images of its disc ID: added, nothing dropped.
        self.write("third.iso", OTHER, hashed=False)
        self.assertEqual(sorted(self.by_file()), ["retail.iso", "third.iso", "translation.iso"])

    def test_a_failed_hash_never_replaces_a_hashed_record_of_another_file(self) -> None:
        self.write("retail.iso", RETAIL)
        self.write("unread.iso", MODIFIED, hashed=False)
        self.assertEqual(sorted(self.by_file()), ["retail.iso", "unread.iso"])
        # Beside a record that was never hashed either, it replaces it, as before hashing.
        self.write("old.iso", OTHER, disc_id="TEST70009", hashed=False)
        self.write("moved.iso", OTHER, disc_id="TEST70009", hashed=False)
        self.assertNotIn("old.iso", self.by_file())

    def test_the_file_another_image_now_fills_is_replaced(self) -> None:
        self.write("disc.iso", RETAIL)
        self.write("disc.iso", OTHER)  # e.g. a compressed image expanded over the same path
        self.assertEqual([game["content_sha1"] for game in self.games()], [digest(OTHER).sha1])


def zip_of(entries: dict[str, bytes], method: int = zipfile.ZIP_DEFLATED) -> bytes:
    """A zip of the given files, as Redump serves its datfile (one .dat, deflated)."""
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", compression=method) as archive:
        for name, data in entries.items():
            archive.writestr(name, data)
    return buffer.getvalue()


class ZipAndEntityTests(unittest.TestCase):
    """Redump's zip is read as the .dat inside it; anything else is refused by name."""

    def refused(self, data: bytes, code: str, *, max_bytes: int | None = None) -> None:
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "list.zip"
            path.write_bytes(data)
            patch = (mock.patch.object(disc_verify, "MAX_DAT_BYTES", max_bytes)
                     if max_bytes is not None else contextlib.nullcontext())
            with patch, self.assertRaises(disc_verify.DiscVerifyError) as raised:
                disc_verify.load_dat(path)
            self.assertEqual(raised.exception.boundary_code, code)
            # The importer refuses it the same way and keeps nothing.
            with patch, self.assertRaises(disc_verify.DiscVerifyError) as raised:
                disc_verify.import_dat(path, Path(folder) / "data")
            self.assertEqual(raised.exception.boundary_code, code)
            self.assertFalse(disc_verify.imported_dat_path(Path(folder) / "data").exists())

    def test_the_zip_as_served_is_read_and_imported_as_its_dat(self) -> None:
        text = datafile().encode("utf-8")
        for method in (zipfile.ZIP_DEFLATED, zipfile.ZIP_STORED):
            with self.subTest(method=method), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                source = root / "Sony - PlayStation Portable - Datfile (2) (2026-10-31).zip"
                source.write_bytes(zip_of({"Synthetic - Datfile (2) (2026-10-31).dat": text}, method))
                self.assertEqual(len(disc_verify.load_dat(source)), 2)
                dat = disc_verify.import_dat(source, root / "data")
                self.assertEqual((len(dat), dat.source), (2, disc_verify.LIST_IMPORTED))
                # The .dat inside is what is kept, so the C reader and later loads read text.
                self.assertEqual(disc_verify.imported_dat_path(root / "data").read_bytes(), text)

    def test_nk_cli_imports_the_zip(self) -> None:
        import argparse

        import nk_cli

        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "redump.zip"
            source.write_bytes(zip_of({"list.dat": datafile().encode("utf-8")}))
            shown = io.StringIO()
            with contextlib.redirect_stdout(shown):
                self.assertEqual(nk_cli.cmd_redump_import(argparse.Namespace(
                    datafile=source, user_data_root=root / "data")), 0)
            self.assertIn("Imported a Redump list of 2 discs (version 2026-10-31)", shown.getvalue())
            self.assertIn("in place of the list bundled with Nakagawa", shown.getvalue())

    def test_a_zip_that_is_not_one_dat_is_refused_by_name(self) -> None:
        text = datafile().encode("utf-8")
        self.refused(zip_of({"a.dat": text, "b.dat": text}), "DISC_DAT_ZIP_INVALID")
        self.refused(zip_of({"list.txt": text}), "DISC_DAT_ZIP_INVALID")
        # An empty zip has no file header to be told by, so it is read as text, and refused.
        self.refused(zip_of({}), "DISC_DAT_NOT_DATAFILE")
        whole = zip_of({"list.dat": text}, zipfile.ZIP_STORED)
        self.refused(whole[:-10], "DISC_DAT_ZIP_INVALID")  # cut short: no end record
        # A changed byte in the stored data fails its CRC-32.
        at = whole.index(b"<datafile>")
        bent = whole[:at] + b"<DATAFILE>" + whole[at + len(b"<datafile>"):]
        self.refused(bent, "DISC_DAT_ZIP_INVALID")
        # The encrypted flag, in the local and central headers.
        local, central = whole.index(b"PK\x03\x04"), whole.index(b"PK\x01\x02")
        flagged = bytearray(whole)
        flagged[local + 6] |= 1
        flagged[central + 8] |= 1
        self.refused(bytes(flagged), "DISC_DAT_ZIP_INVALID")

    def test_the_zip_rules_are_the_c_readers(self) -> None:
        text = datafile().encode("utf-8")
        # A folder entry counts: the zip must hold the one .dat alone, as the C reader requires.
        self.refused(zip_of({"d/": b"", "a.dat": text}), "DISC_DAT_ZIP_INVALID")
        self.refused(zip_of({"d.dat/": b""}), "DISC_DAT_ZIP_INVALID")
        # A ZIP64 block in the entry's central-directory extra field is refused, as the C
        # reader refuses it (an empty block: header ID 0x0001, length 0).
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            info = zipfile.ZipInfo("list.dat")
            info.compress_type = zipfile.ZIP_DEFLATED
            info.extra = b"\x01\x00\x00\x00"
            archive.writestr(info, text)
        self.refused(buffer.getvalue(), "DISC_DAT_ZIP_INVALID")

    def test_an_oversized_list_in_a_zip_is_refused_before_it_is_read_whole(self) -> None:
        big = datafile().encode("utf-8")
        # The header's size is over the cap ...
        self.refused(zip_of({"list.dat": big}), "DISC_DAT_TOO_LARGE", max_bytes=len(big) - 1)
        # ... and a header that understates it is caught by reading at most the cap plus one.
        whole = bytearray(zip_of({"list.dat": big}))
        central = whole.index(b"PK\x01\x02")
        local = whole.index(b"PK\x03\x04")
        for header, offset in ((central, 24), (local, 22)):
            whole[header + offset:header + offset + 4] = (16).to_bytes(4, "little")
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "list.zip"
            path.write_bytes(bytes(whole))
            with mock.patch.object(disc_verify, "MAX_DAT_BYTES", 64), \
                    self.assertRaises(disc_verify.DiscVerifyError) as raised:
                disc_verify.load_dat(path)
            self.assertIn(raised.exception.boundary_code,
                          ("DISC_DAT_TOO_LARGE", "DISC_DAT_ZIP_INVALID"))

    def test_entity_bearing_lists_are_refused_by_name(self) -> None:
        good = datafile()
        external = ('<!DOCTYPE datafile PUBLIC "-//Logiqx//DTD ROM Management Datafile//EN" '
                    '"http://www.logiqx.com/Dats/datafile.dtd">')
        self.assertIn(external, good)
        cases = {
            "internal subset with an entity": good.replace(
                external, '<!DOCTYPE datafile [<!ENTITY big "x">]>').replace(
                "Synthetic &amp; Test", "&big;"),
            "empty internal subset": good.replace(external, "<!DOCTYPE datafile [ ]>"),
            "external entity": good.replace(
                "<datafile>", '<datafile><!ENTITY xxe SYSTEM "file:///etc/passwd">'),
            "undeclared reference": good.replace("Synthetic &amp; Test", "&xxe;"),
            "bare ampersand": good.replace("Synthetic &amp; Test", "A & B"),
            "lowercase entity": good.replace("<datafile>", '<datafile><!entity a "b">'),
            "second DOCTYPE": good.replace(external, external + external),
            "DOCTYPE after the root": good.replace("<datafile>", "<datafile>" + external),
            "markup inside the DOCTYPE": good.replace(
                external, '<!DOCTYPE datafile <!ENTITY a "b">>'),
            "comment": good.replace("<datafile>", "<datafile><!-- a -->"),
            "CDATA": good.replace("Synthetic &amp; Test", "<![CDATA[x]]>"),
        }
        for name, text in cases.items():
            with self.subTest(name), self.assertRaises(disc_verify.DiscVerifyError) as raised:
                disc_verify.parse_dat_text(text)
            self.assertEqual(raised.exception.boundary_code, "DISC_DAT_ENTITY_REFUSED", name)
        # The external DOCTYPE Redump's lists carry is skipped unread; character references stay.
        dat = disc_verify.parse_dat_text(good.replace("Synthetic &amp; Test", "A &#38; B &#x26;"))
        self.assertEqual(dat.name, "A &#38; B &#x26;")


#: The most bytes the reader may look at for each byte of its input. The reader counts each
#: one every time it looks at it (the UTF-8 and control-byte passes, the scan, and each
#: <serial>'s splitting), so this bounds the work of any input (#1731 reviews B1, S1 and S2:
#: lists that once cost time quadratic in their size, and a serial whose splitting went
#: uncounted). nk_disc_dat_parse_counted counts the same way, to the same bound.
COST_PER_CHARACTER = 8


def hostile_lists(repeats: int) -> dict[str, str]:
    """Datafiles of about ``repeats`` repetitions of a shape that once cost quadratic time, or
    could: each is refused by name or read, never a hang."""
    good = datafile()
    game = good.index("\t<game")
    retail = digest(RETAIL)
    rom = f'<rom name="x" size="8192" crc="{retail.crc32}" sha1="{retail.sha1}"/>'
    return {
        # B1: DOCTYPEs after the root, once rescanned from each to the next ">".
        "DOCTYPE after the root": good[:game] + "<!DOCTYPE" * repeats + good[game:],
        "DOCTYPE before the root": good.replace("<datafile>", "<!DOCTYPE" * repeats + "<datafile>"),
        # S1: header tags, once matched by three backtracking patterns.
        "header tags": "<datafile>" + "<header" * repeats + good[game:],
        "header elements": "<datafile>" + "<header>" * repeats + good[game:],
        "unclosed games": good[:game] + '<game name="a">' * repeats + good[game:],
        "game openers": good[:game] + "<game" * repeats + good[game:],
        "a long attribute name": good[:game] + "<game>" + "<rom " + "a" * repeats + "/></game>"
        + good[game:],
        "many roms": good[:game] + "<game>" + rom * (repeats // 20) + "</game>" + good[game:],
        "references": good.replace("Synthetic &amp; Test", "&amp;" * repeats),
        "long reference": good.replace("Synthetic &amp; Test", "&#" + "1" * repeats + ";"),
        "a long serial": good.replace("TEST-70001, TEST-70001GA", "TEST-70001," * repeats),
        # S2: serials at the length bound, all commas, in game after game.
        "serial comma floods": good[:game] + ("<game><serial>" + "," * disc_verify.MAX_SERIAL_BYTES
                                              + "</serial></game>") * (repeats // 64) + good[game:],
        "many serials": good.replace("\t\t<serial>", "<serial>TEST-70002</serial>" * (repeats // 10)
                                     + "\t\t<serial>"),
        "an open declaration": "<datafile><?" + "x" * repeats,
        "tags": good[:game] + "<a>" * repeats + good[game:],
        "less-than signs": good[:game] + "<" * repeats + good[game:],
        "ampersands": good.replace("Synthetic &amp; Test", "&" * repeats),
        "no closing tag": good.replace("</datafile>", "") + "<x>" * repeats,
    }


class LinearCostTests(unittest.TestCase):
    """The reader's work is linear in its input: counted, not timed (a time limit would be
    flaky). tests/native/test_disc_verify.c holds the C reader to the same bound."""

    def cost(self, text: str) -> int:
        data = text.encode("utf-8")
        scanner = disc_verify._DatScanner(data)
        try:
            examined = disc_verify._check_text(data, 0)
            scanner.parse()
        except disc_verify.DiscVerifyError as exc:
            self.assertTrue(exc.boundary_code.startswith("DISC_DAT_"), exc.boundary_code)
            examined = 2 * len(data)
        # int(): mypy sees nk_core (imported through sys.path) as Any (S4).
        return int(examined + scanner.examined)

    def test_hostile_lists_cost_a_fixed_multiple_of_their_size(self) -> None:
        for repeats in (20_000, 40_000):
            for name, text in hostile_lists(repeats).items():
                with self.subTest(name, repeats=repeats):
                    self.assertLessEqual(self.cost(text), COST_PER_CHARACTER * len(text))

    def test_the_review_inputs_are_refused_by_name(self) -> None:
        lists = hostile_lists(200_000)
        for name, code in (("DOCTYPE after the root", "DISC_DAT_ENTITY_REFUSED"),
                           ("DOCTYPE before the root", "DISC_DAT_ENTITY_REFUSED"),
                           ("header tags", "DISC_DAT_NOT_DATAFILE")):
            with self.subTest(name), self.assertRaises(disc_verify.DiscVerifyError) as raised:
                disc_verify.parse_dat_text(lists[name])
            self.assertEqual(raised.exception.boundary_code, code)

    def test_the_bundled_list_costs_the_same_bound(self) -> None:
        with zipfile.ZipFile(disc_verify.bundled_dat_path()) as archive:
            text = archive.read(archive.infolist()[0]).decode("utf-8-sig")
        self.assertLessEqual(self.cost(text), COST_PER_CHARACTER * len(text))

    def test_a_serial_is_split_once_and_counted(self) -> None:
        # S2: the splitting's three passes are counted, and a serial past the bound is refused
        # before it is split at all.
        good = datafile()
        at_bound = good.replace("TEST-70001, TEST-70001GA", "," * disc_verify.MAX_SERIAL_BYTES)
        self.assertEqual(self.cost(at_bound) - self.cost(good.replace("TEST-70001, TEST-70001GA", "")),
                         4 * disc_verify.MAX_SERIAL_BYTES + 3 * disc_verify.MAX_SERIAL_BYTES)
        over = good.replace("TEST-70001, TEST-70001GA", "," * (disc_verify.MAX_SERIAL_BYTES + 1))
        with self.assertRaises(disc_verify.DiscVerifyError) as raised:
            disc_verify.parse_dat_text(over)
        self.assertIn("longer than 256 bytes", str(raised.exception))


#: What the reader may hold at once (S5: it once held every attribute of a <rom>, so a 14 MB
#: list peaked near 180 MiB): a few times the bytes it reads (the file, and the .dat inflated
#: from a zip), plus each rom it keeps (its DatRom, its SHA-1, CRC-32 and name strings, its
#: place in the dict, its game's disc IDs). Counted by tracemalloc, which sees every
#: allocation Python makes; the limits (MAX_DAT_BYTES, MAX_DAT_ROMS) bound both terms.
MEMORY_PER_BYTE = 3
MEMORY_PER_ROM = 512


class MemoryTests(unittest.TestCase):
    def peak(self, read: Callable[[], object]) -> int:
        tracemalloc.start()
        try:
            read()
            return tracemalloc.get_traced_memory()[1]
        finally:
            tracemalloc.stop()

    def bound(self, size: int, roms: int) -> int:
        return MEMORY_PER_BYTE * size + MEMORY_PER_ROM * roms

    def test_the_bundled_list_is_held_once(self) -> None:
        path = disc_verify.bundled_dat_path()
        packed = path.read_bytes()
        text = disc_verify._dat_from_zip(packed)
        peak = self.peak(lambda: disc_verify.load_dat(path))
        self.assertLessEqual(peak, self.bound(len(packed) + len(text), BUNDLED_DISCS))
        peak = self.peak(lambda: disc_verify.parse_dat_bytes(text))
        self.assertLessEqual(peak, self.bound(len(text), BUNDLED_DISCS))

    def test_many_games_fit_the_same_bound(self) -> None:
        # Ten thousand games of one rom and one disc ID each (the bound is per byte and per rom).
        games = "".join(f'<game name="Game {index} (USA)"><serial>ULUS-{index:05d}</serial>'
                        f'<rom name="Game {index} (USA).iso" size="1" crc="0badf00d" '
                        f'sha1="{index:040x}"/></game>' for index in range(10_000))
        data = f"<datafile>{games}</datafile>".encode("ascii")
        peak = self.peak(lambda: disc_verify.parse_dat_bytes(data))
        self.assertLessEqual(peak, self.bound(len(data), 10_000))

    def test_attributes_a_rom_does_not_use_are_not_held(self) -> None:
        # The review's c03 shape (one <rom> with 1.4 million empty attributes) at a seventh of
        # its size: nothing in proportion to them is held, the peak is a few KiB.
        attributes = "".join(f' a{index:x}=""' for index in range(200_000))
        data = (f'<datafile><game><rom{attributes} size="1" crc="0badf00d" sha1="{"a" * 40}"/>'
                "</game></datafile>").encode("ascii")
        peak = self.peak(lambda: disc_verify.parse_dat_bytes(data))
        self.assertLessEqual(peak, 64 * 1024)


#: The list bundled with Nakagawa (assets/redump/PROVENANCE.md). A refreshed list changes these
#: with its pin; tests/native/test_disc_verify.c checks the same counts with the C reader.
BUNDLED_DATE = "2026-11-12 16-02-53"
BUNDLED_DISCS = 3546
BUNDLED_SERIALS = 3907
BUNDLED_SHA256 = "7e4afb96fe3a02b3e43e4138b1ebba911bdb1d96c73fa61f294166bb14fbebaf"
BUNDLED_SIZE = 346932


class BundledListTests(unittest.TestCase):
    """The Redump PSP list shipped in assets/redump: pinned, used by default, overridden by an
    import. The only real list the tests read; the cases above use synthetic ones."""

    def test_the_bundled_list_matches_its_pin(self) -> None:
        path = disc_verify.bundled_dat_path()
        self.assertEqual(path, disc_verify.PROJECT_ROOT / "assets" / "redump" / "redump-psp.zip")
        data = path.read_bytes()
        self.assertEqual((len(data), hashlib.sha256(data).hexdigest()),
                         (BUNDLED_SIZE, BUNDLED_SHA256))
        manifest = json.loads((disc_verify.PROJECT_ROOT / "assets" / "release_manifest.json")
                              .read_text(encoding="utf-8"))
        pin = next(item for item in manifest["components"] if item["id"] == "redump-psp-datfile")
        self.assertEqual((pin["source_path"], pin["hashes"]["sha256"], pin["size_bytes"]),
                         ("assets/redump/redump-psp.zip", BUNDLED_SHA256, BUNDLED_SIZE))
        provenance = (path.parent / "PROVENANCE.md").read_text(encoding="utf-8")
        row = next(line for line in provenance.splitlines() if line.startswith("| `redump-psp.zip`"))
        self.assertIn("imported byte-identical", row)
        self.assertIn(f"`{BUNDLED_SHA256}`", row)
        self.assertIn(pin["fetched_utc"], row)
        self.assertIn("`https://redump.info/datfile/PSP`", row)

    def test_the_bundled_list_loads_with_its_counts_and_a_known_entry_verifies(self) -> None:
        dat = disc_verify.load_dat(disc_verify.bundled_dat_path())
        self.assertEqual((dat.name, dat.version, dat.date),
                         ("Sony - PlayStation Portable", BUNDLED_DATE, BUNDLED_DATE))
        self.assertEqual((dat.games, len(dat), dat.rejected_roms, len(dat.serials)),
                         (BUNDLED_DISCS, BUNDLED_DISCS, 0, BUNDLED_SERIALS))
        known = disc_verify.DiscDigest(sha1="3ba8af1354c8ae1e5119dbff19730793c21ad56e",
                                       crc32="b71764f4", size_bytes=1207468032)
        self.assertEqual(disc_verify.classify("ULJM06340", known, dat), disc_verify.VERIFIED_RETAIL)
        changed = disc_verify.DiscDigest(known.sha1, "b71764f5", known.size_bytes)
        # The list names disc IDs, so a changed copy of a listed disc is Modified on its own.
        self.assertEqual(disc_verify.classify("ULJM06340", changed, dat), disc_verify.MODIFIED)
        self.assertEqual(disc_verify.classify("TEST70009", changed, dat), disc_verify.UNKNOWN)
        self.assertIn("ULUS10020", dat.serials)

    def test_the_bundled_list_is_the_default_and_an_import_overrides_it(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            data = Path(folder) / "data"
            dat = disc_verify.load_verification_dat(data)
            assert dat is not None
            self.assertEqual((dat.source, len(dat)), (disc_verify.LIST_BUNDLED, BUNDLED_DISCS))
            self.assertEqual(dat.describe(), "the Redump list bundled with Nakagawa "
                                             f"(dated {BUNDLED_DATE}, {BUNDLED_DISCS} discs)")
            source = Path(folder) / "chosen.dat"
            source.write_text(datafile(), encoding="utf-8")
            disc_verify.import_dat(source, data)
            dat = disc_verify.load_verification_dat(data)
            assert dat is not None
            self.assertEqual((dat.source, len(dat)), (disc_verify.LIST_IMPORTED, 2))
            # The import is older than the bundled list: it is still used (the user chose it),
            # and the output names both dates and how to use the bundled list again.
            self.assertEqual(dat.newer_bundled_date, BUNDLED_DATE)
            self.assertEqual(dat.describe(), (
                "your imported Redump list (dated 2026-10-31, 2 discs), older than the list "
                f"bundled with Nakagawa (dated {BUNDLED_DATE}): import a newer list, or delete "
                "verification/redump.dat from your data folder to use the bundled one"))
            # A newer import gets no such note.
            source.write_text(datafile().replace("2026-10-31", "2027-01-02 03-04-05"),
                              encoding="utf-8")
            disc_verify.import_dat(source, data)
            dat = disc_verify.load_verification_dat(data)
            assert dat is not None
            self.assertEqual(dat.newer_bundled_date, "")
            self.assertEqual(dat.describe(),
                             "your imported Redump list (dated 2027-01-02 03-04-05, 2 discs)")
            # A damaged import is reported, never replaced by the bundled list.
            disc_verify.imported_dat_path(data).write_text("damaged", encoding="utf-8")
            with self.assertRaises(disc_verify.DiscVerifyError) as raised:
                disc_verify.load_verification_dat(data)
            self.assertEqual(raised.exception.boundary_code, "DISC_DAT_NOT_DATAFILE")
            # Neither list: None, so the tier is Not checked.
            self.assertIsNone(disc_verify.load_verification_dat(
                Path(folder) / "empty", bundled=Path(folder) / "missing.zip"))

    def test_bringup_says_which_list_it_used(self) -> None:
        import nk_cli

        with tempfile.TemporaryDirectory() as folder:
            data = Path(folder) / "data"
            shown = io.StringIO()
            with contextlib.redirect_stdout(shown):
                dat = nk_cli._verification_dat(data)
            assert dat is not None and dat.source == disc_verify.LIST_BUNDLED
            self.assertIn("Disc check: compared with the Redump list bundled with Nakagawa "
                          f"(dated {BUNDLED_DATE}, {BUNDLED_DISCS} discs).", shown.getvalue())
            # An image not in the list is Unknown, never verified.
            self.assertEqual(disc_verify.classify("TEST70001", digest(RETAIL), dat),
                             disc_verify.UNKNOWN)
            disc_verify.imported_dat_path(data).parent.mkdir(parents=True)
            disc_verify.imported_dat_path(data).write_text("damaged", encoding="utf-8")
            shown = io.StringIO()
            with contextlib.redirect_stdout(shown):
                self.assertIsNone(nk_cli._verification_dat(data))
            self.assertIn("The imported Redump list was not used [DISC_DAT_NOT_DATAFILE]",
                          shown.getvalue())

    def test_a_damaged_bundled_list_is_named_as_the_bundled_one(self) -> None:
        import nk_cli

        with tempfile.TemporaryDirectory() as folder:
            damaged = Path(folder) / "redump-psp.zip"
            damaged.write_bytes(b"PK\x03\x04 not a zip")
            with mock.patch.object(disc_verify, "bundled_dat_path", return_value=damaged):
                with self.assertRaises(disc_verify.DiscVerifyError) as raised:
                    disc_verify.load_verification_dat(Path(folder) / "data")
                self.assertEqual((raised.exception.boundary_code, raised.exception.list_source),
                                 ("DISC_DAT_ZIP_INVALID", disc_verify.LIST_BUNDLED))
                shown = io.StringIO()
                with contextlib.redirect_stdout(shown):
                    self.assertIsNone(nk_cli._verification_dat(Path(folder) / "data"))
            self.assertIn("The Redump list bundled with Nakagawa was not used [DISC_DAT_ZIP_INVALID]",
                          shown.getvalue())
            self.assertNotIn("imported", shown.getvalue())

    def test_list_dates_compare_in_redump_form_only(self) -> None:
        older = disc_verify.list_date_is_older
        self.assertTrue(older("2026-10-31", BUNDLED_DATE))
        self.assertTrue(older("2026-11-12 16-02-52", BUNDLED_DATE))
        self.assertFalse(older(BUNDLED_DATE, BUNDLED_DATE))
        self.assertFalse(older("2026-11-12", BUNDLED_DATE))  # same day, no time: not called older
        self.assertFalse(older("2027-01-01", BUNDLED_DATE))
        self.assertFalse(older("version 3", BUNDLED_DATE))


# ---- one table of cases for both readers ----------------------------------------------------
#
# tests/native/disc_dat_conformance.txt holds inputs and the result each must give, and both
# test suites read it: this one generates it (``python tools/test_disc_verify.py
# --write-corpus``) and checks that the file is what the generator and this reader give; the C
# suite builds the same inputs and checks that the C reader gives the same results. A reader
# that changes alone fails its suite. The file's own header says how it is written.

CORPUS = disc_verify.PROJECT_ROOT / "tests" / "native" / "disc_dat_conformance.txt"
MUTATION_SEED = 0x1731
MUTATION_COUNT = 4096
MUTATION_WINDOWS = 8
MUTATION_WINDOW_GAMES = 8

Piece = tuple  # ("t", bytes) | ("r", count, bytes) | ("h", bytes) | ("seq", count, bytes)
#                | ("file", path) | ("reg", name) | ("out",) | ("put", name) | ("deflate", name)

CORPUS_HEADER = """\
# The Redump datafile format, as both readers must read it: tools/nk_core/disc_verify.py
# (its module documentation writes the format down) and src/core/nk_disc_verify.c.
# Generated by `python tools/test_disc_verify.py --write-corpus`; do not edit by hand.
#
# A case builds one input from pieces, in order, then names the result:
#   t TEXT          the text (escapes: \\\\ \\n \\t \\r \\xHH; every other byte is itself)
#   r COUNT TEXT    the text COUNT times
#   h HEX           bytes in hex
#   seq COUNT TEXT  the text COUNT times, i from 0: %H is i in 40 hex digits, %X in hex,
#                   %D is i mod 100000 in five digits, %% is %
#   file PATH       a file's bytes (PATH from the repository's root)
#   reg NAME        the pieces that follow fill register NAME (emptied first) ...
#   out             ... until this, when they fill the input again
#   put NAME        a register's bytes
#   deflate NAME    a register's bytes as one fixed-Huffman deflate block: each byte a
#                   literal, a run of 259 or more of one byte a literal and as many
#                   length-258 distance-1 copies as fit, then the end of the block
#   expect RESULT   OK GAMES ROMS REJECTED DISC-IDS SHA1, the SHA-1 of the list read (its
#                   counts, header and disc IDs, and each rom's SHA-1, CRC-32, size and
#                   place, one per line, sorted); or the refusal's code and message
#
# The mutations section names a seed and a count: mutation i is made by a splitmix64
# generator seeded with (seed << 32) | i from one of eight windows of the bundled list (its
# header, eight games, </datafile>): XML changes (i % 16 < 10), a stored zip of the window
# with changed bytes (10, 11), a fixed-Huffman zip of it with flipped bits or a changed
# size (12-14), or the bundled zip with flipped bits in its deflate data (15). Each
# result's line is listed as the first 8 hex digits of its SHA-1, eight to a line.
"""


def _escape(data: bytes) -> str:
    out = []
    for index, byte in enumerate(data):
        if byte == 0x5C:
            out.append("\\\\")
        elif byte == 0x0A:
            out.append("\\n")
        elif byte == 0x09:
            out.append("\\t")
        elif byte == 0x0D:
            out.append("\\r")
        elif byte < 0x20 or byte >= 0x7F or (byte == 0x20 and index == len(data) - 1):
            out.append(f"\\x{byte:02x}")
        else:
            out.append(chr(byte))
    return "".join(out)


_SEQ_FIELDS = {b"%H": "{0:040x}", b"%X": "{0:x}", b"%D": "{1:05d}", b"%%": "%"}


def _seq(count: int, template: bytes) -> bytes:
    """A ``seq`` piece: the template ``count`` times, its fields filled from i."""
    parts = re.split(rb"(%[HXD%])", template)
    pattern = "".join(_SEQ_FIELDS[part] if index % 2 else
                      part.decode("ascii").replace("{", "{{").replace("}", "}}")
                      for index, part in enumerate(parts))
    return "".join(pattern.format(index, index % 100000) for index in range(count)).encode("ascii")


_RUNS = [re.compile(re.escape(bytes([byte])) + b"+") for byte in range(256)]
_FIXED_CODES = []
for _symbol in range(288):
    if _symbol < 144:
        _code, _length = 0x30 + _symbol, 8
    elif _symbol < 256:
        _code, _length = 0x190 + _symbol - 144, 9
    elif _symbol < 280:
        _code, _length = _symbol - 256, 7
    else:
        _code, _length = 0xC0 + _symbol - 280, 8
    # Huffman codes go into the stream most significant bit first.
    _FIXED_CODES.append((int(f"{_code:0{_length}b}"[::-1], 2), _length))


def fixed_deflate(data: bytes) -> bytes:
    """One final fixed-Huffman block (RFC 1951 3.2.6) of ``data``, as the corpus's ``deflate``
    piece and the C suite's fixed_deflate write it."""
    out = bytearray()
    acc = nbits = 0

    def put(value: int, count: int) -> None:
        nonlocal acc, nbits
        acc |= value << nbits
        nbits += count
        while nbits >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nbits -= 8

    put(1, 1)  # BFINAL
    put(1, 2)  # BTYPE 01, fixed codes
    at = 0
    while at < len(data):
        match = _RUNS[data[at]].match(data, at)
        assert match is not None
        run = match.end() - at
        if run < 259:
            for byte in data[at:at + run]:
                put(*_FIXED_CODES[byte])
            at += run
            continue
        put(*_FIXED_CODES[data[at]])
        copies = (run - 1) // 258
        for _ in range(copies):
            put(*_FIXED_CODES[285])  # length 258
            put(0, 5)  # distance code 0: distance 1
        at += 1 + copies * 258
    put(*_FIXED_CODES[256])
    if nbits:
        out.append(acc & 0xFF)
    return bytes(out)


def build_pieces(pieces: Iterable[Piece]) -> bytes:
    """The input a case's pieces build (the C suite's build_case does the same)."""
    output = bytearray()
    registers: dict[str, bytearray] = {}
    target = output
    for piece in pieces:
        kind = piece[0]
        if kind == "t" or kind == "h":
            target += piece[1]
        elif kind == "r":
            target += piece[2] * piece[1]
        elif kind == "seq":
            target += _seq(piece[1], piece[2])
        elif kind == "file":
            target += (disc_verify.PROJECT_ROOT / piece[1]).read_bytes()
        elif kind == "reg":
            target = registers[piece[1]] = bytearray()
        elif kind == "out":
            target = output
        elif kind == "put":
            target += registers[piece[1]]
        elif kind == "deflate":
            target += fixed_deflate(bytes(registers[piece[1]]))
        else:
            raise ValueError(kind)
    return bytes(output)


def piece_lines(pieces: Iterable[Piece]) -> list[str]:
    lines = []
    for piece in pieces:
        kind = piece[0]
        if kind == "t":
            lines.append(f"t {_escape(piece[1])}" if piece[1] else "t")
        elif kind == "h":
            lines.append(f"h {piece[1].hex()}")
        elif kind in ("r", "seq"):
            lines.append(f"{kind} {piece[1]} {_escape(piece[2])}")
        elif kind == "out":
            lines.append("out")
        else:
            lines.append(f"{kind} {piece[1]}")
    return lines


def outcome(data: bytes) -> str:
    """The result line of one input: what the reader made of it, or its refusal."""
    try:
        dat = disc_verify.dat_from_bytes(data)
    except disc_verify.DiscVerifyError as exc:
        return f"{exc.boundary_code} {exc}"
    lines = [str(dat.games), str(len(dat)), str(dat.rejected_roms), dat.name, dat.version,
             dat.date]
    lines += sorted(f"serial {disc_id}" for disc_id in dat.serials)
    lines += sorted(f"rom {rom.sha1} {rom.crc32} {rom.size_bytes} {rom.listed}"
                    for rom in dat.roms_by_sha1.values())
    canonical = hashlib.sha1("".join(line + "\n" for line in lines).encode("ascii"))
    return (f"OK {dat.games} {len(dat)} {dat.rejected_roms} {len(dat.serials)} "
            f"{canonical.hexdigest()}")


# -- the cases -----------------------------------------------------------------------------

SHA_A, SHA_B, SHA_C = "a" * 40, "b" * 40, "c" * 40
DOCTYPE = ('<!DOCTYPE datafile PUBLIC "-//Logiqx//DTD ROM Management Datafile//EN" '
           '"http://www.logiqx.com/Dats/datafile.dtd">')
HEADER = ("\t<header>\n\t\t<name>Sony - PlayStation Portable</name>\n"
          "\t\t<version>2026-11-12 16-02-53</version>\n\t\t<date>2026-11-12 16-02-53</date>\n"
          "\t</header>\n")


def c_rom(sha: str, crc: str = "0badf00d", size: str = "1024", name: str = "x.iso") -> str:
    return f'<rom name="{name}" size="{size}" crc="{crc}" sha1="{sha}"/>'


def c_game(body: str, serial: Optional[str] = None, name: str = "G") -> str:
    s = f"<serial>{serial}</serial>" if serial is not None else ""
    return f'\t<game name="{name}">\n\t\t{s}\n\t\t{body}\n\t</game>\n'


def c_dat(games: str, header: Optional[str] = None, pre: str = "", post: str = "") -> str:
    header = HEADER if header is None else header
    return f'<?xml version="1.0"?>\n{DOCTYPE}\n{pre}<datafile>\n{header}{games}</datafile>\n{post}'


GOOD_GAMES = c_game(c_rom(SHA_A), "ULUS-10041") + c_game(c_rom(SHA_B), "ULES-00151, ULES-00152")
GOOD = c_dat(GOOD_GAMES)


def T(text: str | bytes) -> Piece:
    return ("t", text.encode("utf-8") if isinstance(text, str) else text)


def R(count: int, text: str | bytes) -> Piece:
    return ("r", count, text.encode("utf-8") if isinstance(text, str) else text)


def SEQ(count: int, text: str) -> Piece:
    return ("seq", count, text.encode("utf-8"))


def text_case(text: str | bytes) -> list[Piece]:
    return [T(text)]


def around(text: str, count: int, unit: str) -> list[Piece]:
    """The text with its one "@@" replaced by ``unit`` ``count`` times."""
    before, after = text.split("@@")
    return [T(before), R(count, unit), T(after)]


def _local_header(name: bytes, method: int, crc: int, csize: int, usize: int, flags: int = 0,
                  extra: bytes = b"") -> bytes:
    return struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, flags, method, 0, 0x21, crc, csize, usize,
                       len(name), len(extra)) + name + extra


def _central(name: bytes, method: int, crc: int, csize: int, usize: int, offset: int,
             flags: int = 0, extra: bytes = b"") -> bytes:
    return struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, flags, method, 0, 0x21, crc,
                       csize, usize, len(name), len(extra), 0, 0, 0, 0, offset) + name + extra


def _eocd(count: int, cd_size: int, cd_offset: int, comment: bytes = b"",
          count2: Optional[int] = None, disk: int = 0) -> bytes:
    return struct.pack("<IHHHHIIH", 0x06054B50, disk, 0, count, count if count2 is None else count2,
                       cd_size, cd_offset, len(comment)) + comment


def zip_bytes(payload: bytes, method: int) -> bytes:
    """The zip the mutations start from (and the C suite's zip_bytes): one entry, list.dat."""
    packed = fixed_deflate(payload) if method == 8 else payload
    crc = zlib.crc32(payload)
    local = _local_header(b"list.dat", method, crc, len(packed), len(payload))
    central = _central(b"list.dat", method, crc, len(packed), len(payload), 0)
    body = local + packed
    return body + central + _eocd(1, len(central), len(body))


_zip_registers = iter(range(1_000_000))


def zip_case(payload: list[Piece], *, method: int = 8, packed: Optional[list[Piece]] = None,
             crc: Optional[int] = None, usize: Optional[int] = None,
             csize: Optional[int] = None, flags: int = 0, lflags: Optional[int] = None,
             extra: bytes = b"", lextra: bytes = b"", name: bytes = b"list.dat",
             lname: Optional[bytes] = None, lmethod: Optional[int] = None, count: int = 1,
             count2: Optional[int] = None, comment: bytes = b"", disk: int = 0,
             offset: int = 0, after_data: bytes = b"", after_eocd: bytes = b"") -> list[Piece]:
    """A one-entry zip of a payload, each field overridable."""
    serial = next(_zip_registers)
    payload_register, packed_register = f"p{serial}", f"q{serial}"
    payload_bytes = build_pieces(payload)
    pieces: list[Piece] = [("reg", payload_register), *payload]
    if packed is not None:
        packed_bytes = build_pieces(packed)
        pieces += [("reg", packed_register), *packed]
        data: Piece = ("put", packed_register)
    elif method == 8:
        packed_bytes = fixed_deflate(payload_bytes)
        data = ("deflate", payload_register)
    else:
        packed_bytes = payload_bytes
        data = ("put", payload_register)
    crc = zlib.crc32(payload_bytes) if crc is None else crc
    usize = len(payload_bytes) if usize is None else usize
    csize = len(packed_bytes) if csize is None else csize
    local = _local_header(name if lname is None else lname, method if lmethod is None else lmethod,
                          crc, csize, usize, flags if lflags is None else lflags, lextra)
    central = _central(name, method, crc, csize, usize, offset, flags, extra)
    body_length = len(local) + len(packed_bytes) + len(after_data)
    tail = after_data + central + _eocd(count, len(central), body_length, comment, count2, disk)
    return pieces + [("out",), ("h", local), data, ("h", tail + after_eocd)]


def corpus_cases() -> list[tuple[str, list[Piece]]]:
    """The cases: the shapes of the reviews of #2530 (hostile datafiles and zips), each
    rule's edges, and the bundled list."""
    cases: list[tuple[str, list[Piece]]] = []

    def add(name: str, pieces: list[Piece] | str | bytes) -> None:
        cases.append((name, pieces if isinstance(pieces, list) else text_case(pieces)))

    good = GOOD
    add("good", good)
    add("bundled zip", [("file", "assets/redump/redump-psp.zip")])
    # Whitespace is space, tab, CR and LF only (review B1).
    one = c_dat(c_game(c_rom(SHA_A), "ULUS-10041"))
    for label, space in (("VT", "\x0b"), ("NBSP", "\u00a0"), ("FF", "\x0c"), ("tab", "\t"),
                         ("ideographic space", "\u3000"), ("NEL", "\u0085")):
        add(f"closing tag with {label}", one.replace(
            "</datafile>\n", f"</datafile {space}>\n" + c_game(c_rom(SHA_C), "ULUS-99999")
            + "</datafile>\n"))
    add("closing tag with a tab only", good.replace("</datafile>", "</datafile\t>"))
    add("closing tag, CRLF", good.replace("</datafile>", "</datafile\r\n>"))
    # Disc IDs (review B2).
    for label, serial in (("VT", "ULUS\x0b10041"), ("NBSP", "ULUS\u00a010041"),
                          ("long s", "\u017fLUS10041"), ("dotless i", "\u0131LUS10041"),
                          ("fullwidth digits", "ULUS\uff11\uff10\uff10\uff14\uff11"),
                          ("lower case", "ulus-10041"),
                          ("suffixes and spaces", "ULUS-10041GA, ULUS10041-X, UL US 1 0 0 4 2"),
                          ("references", "ULUS-10041&amp;, ULUS&amp;10042, ULUS&#45;10043"),
                          ("CR LF tab inside", "UL\r\nUS-1\t0041"),
                          ("empty parts", ",,,ULUS-10041,,"),
                          ("one ID twenty times", ", ".join(["ULUS-10041"] * 20)),
                          ("sixteen IDs", ", ".join(f"ULUS-1{i:04d}" for i in range(16))),
                          ("seventeen IDs", ", ".join(f"ULUS-1{i:04d}" for i in range(17))),
                          ("sixteen IDs, repeats between", ",".join(
                              f"ULUS1{i % 16:04d}" for i in range(23))),
                          ("256 bytes", "ULUS-10041," + "," * 245),
                          ("257 bytes", "ULUS-10041," + "," * 246),
                          ("non-ASCII", "ULUS-10041, \u00dcLUS-10042")):
        add(f"serial: {label}", c_dat(c_game(c_rom(SHA_A), serial)))
    add("serial: self-closing", c_dat(c_game(c_rom(SHA_A)).replace("\t\t\n", "\t\t<serial/>\n", 1)))
    add("serial: sixteen repeats in separate elements", c_dat(
        '\t<game name="G">' + "<serial>ULUS-10041</serial>" * 16 + "<serial>ULUS-10042</serial>"
        + c_rom(SHA_A) + "</game>\n"))
    add("serial: element inside", c_dat(c_game(c_rom(SHA_A), "ULUS<b>10041</b>")))
    add("serial: CDATA", c_dat(c_game(c_rom(SHA_A), "<![CDATA[ULUS10041]]>")))
    add("serial: in the header", good.replace("<date>", "<serial>ULUS-55555</serial><date>"))
    add("serial: in the root", good.replace("\t<game", "<serial>ULUS-55555</serial>\t<game", 1))
    add("serial: inside a rom's sibling element", c_dat(c_game(
        "<x><serial>ULUS-55555</serial></x>" + c_rom(SHA_A))))
    # Header fields (review S1).
    add("date: leading space", good.replace("<date>2026", "<date> 2026"))
    add("date: on its own lines", good.replace("<date>2026-11-12 16-02-53</date>",
                                               "<date>\n\t\t\t2026-11-12 16-02-53\n\t\t</date>"))
    add("date: NUL", good.replace("<date>2026", "<date>\x002026"))
    add("date: NBSP after", good.replace("16-02-53</date>", "16-02-53\u00a0</date>"))
    add("date: control characters", good.replace("<date>2026-11-12 16-02-53",
                                                 "<date>" + "\x01" * 80 + "2026-11-12"))
    add("date: 63 bytes", good.replace("<date>2026-11-12 16-02-53", "<date>" + "d" * 63))
    add("date: 64 bytes", good.replace("<date>2026-11-12 16-02-53", "<date>" + "d" * 64))
    add("date: 63 bytes after references", good.replace(
        "<date>2026-11-12 16-02-53", "<date>\t " + "&amp;" * 3 + "d" * 60 + " \t"))
    add("date: escape sequence", good.replace("<date>2026", "<date>\x1b[31m2026"))
    add("date: empty", good.replace("<date>2026-11-12 16-02-53</date>", "<date/>"))
    add("date: element inside", good.replace("<date>2026", "<date><b/>2026"))
    add("date: twice", good.replace("\t</header>", "\t\t<date>2027-01-01</date>\n\t</header>"))
    add("name: references", good.replace("Sony - PlayStation", "Sony &amp; &lt;PlayStation&gt;"))
    add("name: character references", good.replace(
        "Sony - PlayStation", "&#65;&#x41;&#0;&#xD800;&#1114111;&#x10FFFF;"))
    add("name: non-ASCII", good.replace("Sony", "S\u00f6ny"))
    add("name: astral text", around(good.replace("Sony", "@@"), 3_000_000, "\U0001F600"))
    add("header: second", good.replace("\t<game", HEADER + "\t<game", 1))
    add("header: after a game", c_dat(GOOD_GAMES + HEADER))
    add("header: fifty thousand", around(c_dat(GOOD_GAMES, header="@@"), 50_000,
                                         "<header><date>2026-01-01</date></header>"))
    add("header: none", c_dat(GOOD_GAMES, header=""))
    add("header: unknown fields kept out", good.replace(
        "\t</header>", "\t\t<author>a &amp; b</author><comment/>\n\t</header>"))
    # Markup.
    add("attribute: '>' in a value", c_dat(c_game('<rom name="a>b" size="1024" crc="0badf00d" sha1="'
                                                   + SHA_C + '"/>' + c_rom(SHA_A))))
    add("attribute: single quotes", c_dat(c_game(
        "<rom name='x' size='1024' crc='0badf00d' sha1='" + SHA_C + "'/>" + c_rom(SHA_A))))
    add("attribute: apostrophe in a value", c_dat(c_game(
        '<rom name="it\'s" size="1024" crc="0badf00d" sha1="' + SHA_C + '"/>' + c_rom(SHA_A))))
    add("attribute: no space before", c_dat(c_game(
        '<rom name="x"size="1024" crc="0badf00d" sha1="' + SHA_C + '"/>')))
    add("attribute: no value", c_dat(c_game('<rom name size="1024"/>' + c_rom(SHA_A))))
    add("attribute: unquoted", c_dat(c_game(c_rom(SHA_A)).replace('name="G"', "name=G")))
    add("attribute: spaces around =", c_dat(c_game(
        f'<rom name = "x"\tsize =\n"1024" crc= "0badf00d" sha1 ="{SHA_A}" />')))
    add("attribute: repeated, first kept", c_dat(c_game(
        f'<rom size="1024" crc="0badf00d" sha1="{SHA_A}" sha1="{SHA_C}" size="9"/>')))
    add("attribute: a long value", around(c_dat(c_game(
        f'<rom name="@@" size="1024" crc="0badf00d" sha1="{SHA_C}"/>' + c_rom(SHA_A))),
        8_000_000, "n"))
    add("attribute: a long name", around(c_dat(c_game(
        f'<rom @@="x" size="1024" crc="0badf00d" sha1="{SHA_C}"/>' + c_rom(SHA_A))),
        8_000_000, "n"))
    add("attribute: two hundred thousand", [
        T(c_dat("").split("</datafile>")[0] + '\t<game name="G"><rom'),
        SEQ(200_000, ' a%X=""'),
        T(f' size="1024" crc="0badf00d" sha1="{SHA_A}"/></game>\n</datafile>\n')])
    add("name: non-ASCII element", c_dat(c_game(c_rom(SHA_A)) + "<\u00e9l\u00e9ment/>"))
    add("name: element ROM", c_dat(c_game(
        f'<ROM name="x" size="1024" crc="0badf00d" sha1="{SHA_C}"/>' + c_rom(SHA_A))))
    add("name: element romx", c_dat(c_game(
        f'<romx name="x" size="1024" crc="0badf00d" sha1="{SHA_C}"/>' + c_rom(SHA_A))))
    add("name: root DATAFILE", good.replace("datafile>", "DATAFILE>"))
    add("end tag: space before >", good.replace("</game>", "</game >"))
    add("end tag: space after <", good.replace("</game>", "</ game>"))
    add("end tag: attributes", good.replace("</game>", '</game a="b">'))
    add("end tag: not matching", good.replace("</game>", "</gam>", 1))
    add("end tag: after the root", good + "</datafile>")
    add("end tag: before the root", good.replace("<datafile>", "</x><datafile>"))
    add("tag: '<' inside", good.replace("<game ", "<game <", 1))
    add("tag: '<' after the root", good.replace("</datafile>\n", "</datafile>\n<"))
    add("tag: '<' at the end", good.replace("</datafile>\n", "<"))
    add("tag: cut short", good.replace("</datafile>\n", '<rom sha1="' + SHA_C))
    add("tag: empty", good.replace("\t<game", "<>\t<game", 1))
    add("tag: '< game'", good.replace("<game", "< game", 1))
    add("tag: '/' in the middle", good.replace('<game name="G">', '<game / name="G">', 1))
    add("references: allowed forms", good.replace(
        "Sony - PlayStation", "&#1234567;&#x10FFFF;&#x123456;&quot;&apos;"))
    add("references: eight digits", good.replace("Sony - PlayStation", "&#12345678;"))
    add("references: hex seven digits", good.replace("Sony - PlayStation", "&#x1100000;"))
    add("references: empty", good.replace("Sony - PlayStation", "&#;"))
    add("references: upper X", good.replace("Sony - PlayStation", "&#X41;"))
    add("references: named", good.replace("Sony - PlayStation", "&nbsp;"))
    add("references: in an attribute", c_dat(c_game(c_rom(SHA_A, name="a&bogus;b"))))
    add("references: bare &", good.replace("Sony - PlayStation", "A & B"))
    add("references: cut at the end", good.replace("</datafile>\n", "</datafile>&am"))
    add("references: cut inside the root", good.replace("</datafile>\n", "&am"))
    add("references: in text outside the root", good.replace("<datafile>", "&amp;<datafile>"))
    # Declarations and DOCTYPE.
    add("DOCTYPE: lower case", good.replace("<!DOCTYPE", "<!doctype"))
    add("DOCTYPE: '>' in a quoted value", good.replace(DOCTYPE, '<!DOCTYPE datafile SYSTEM "a>b">'))
    add("DOCTYPE: internal subset", good.replace(DOCTYPE, '<!DOCTYPE datafile [<!ENTITY b "x">]>'))
    add("DOCTYPE: '[' alone", good.replace(DOCTYPE, "<!DOCTYPE datafile [ ]>"))
    add("DOCTYPE: twice", good.replace(DOCTYPE, DOCTYPE + DOCTYPE))
    add("DOCTYPE: inside the root", good.replace("<datafile>", "<datafile>" + DOCTYPE))
    add("DOCTYPE: cut short", "<!DOCTYPE datafile")
    add("DOCTYPE: '<' inside", good.replace(DOCTYPE, '<!DOCTYPE datafile <!ENTITY a "b">>'))
    add("DOCTYPE: none", good.replace(DOCTYPE + "\n", ""))
    add("comment: in the root", good.replace("<datafile>", "<datafile><!-- <game><rom/></game> -->"))
    add("comment: before the root", good.replace("<datafile>", "<!-- x --><datafile>"))
    add("comment: after the root", good + "<!-- x -->")
    add("entity: after the root", good + "<!ENTITY x 'y'>&bogus;")
    add("declaration: inside the root", c_dat(c_game(c_rom(SHA_A)) + "<?x " + c_game(c_rom(SHA_C))
                                              + " ?>"))
    add("declaration: cut short", "<?xml version")
    add("declaration: many before the root", good.replace("<datafile>", "<?a?><?b c?>\n<datafile>"))
    add("declaration: none", good.replace('<?xml version="1.0"?>\n', ""))
    # Encoding and control bytes.
    add("encoding: UTF-16", ("\ufeff" + good).encode("utf-16-le"))
    add("encoding: byte-order mark", b"\xef\xbb\xbf" + good.encode())
    add("encoding: two byte-order marks", b"\xef\xbb\xbf\xef\xbb\xbf" + good.encode())
    add("encoding: byte-order mark only", b"\xef\xbb\xbf")
    add("encoding: Latin-1", good.replace("Sony", "S\u00f6ny").encode("latin-1"))
    for label, raw in (("overlong", b"\xc0\xaf"), ("surrogate", b"\xed\xa0\x80"),
                       ("past U+10FFFF", b"\xf4\x90\x80\x80"), ("lone continuation", b"\x80"),
                       ("cut short", b"\xe3\x80")):
        add(f"encoding: {label}", good.encode().replace(b"Sony", b"S" + raw + b"ony"))
    add("encoding: cut at the end", good.encode() + b"\xe3\x80")
    add("control: NUL before a game", good.replace("<game", "\x00<game", 1))
    add("control: DEL in a name", c_dat(c_game(c_rom(SHA_A, name="a\x7fb"))))
    add("control: VT in text", good.replace("\t<game", "\x0b<game", 1))
    add("control: CR only lines", good.replace("\n", "\r"))
    add("control: CRLF lines", good.replace("\n", "\r\n"))
    add("text: empty", b"")
    add("text: whitespace only", " \t\r\n")
    add("text: not a datafile", "<html>nothing</html>")
    add("text: plain words", "hello")
    add("text: before the root", good.replace("<datafile>", "x<datafile>"))
    add("text: after the root", good + "x")
    add("text: whitespace after the root", good + " \t\r\n\n")
    add("text: a game outside the root", good.replace(
        "<datafile>", c_game(c_rom(SHA_C), "ULUS-77777") + "<datafile>"))
    add("text: no closing tag", good.replace("</datafile>\n", ""))
    add("text: closing tag cut", good.replace("</datafile>\n", "</datafile"))
    add("text: cut inside the last game", good[:good.rindex("<rom")])
    # Structure.
    add("structure: nested game", c_dat('<game name="a"><game name="b">' + c_rom(SHA_A)
                                        + "</game></game>\n"))
    add("structure: self-closing game", c_dat("<game/>" + c_game(c_rom(SHA_A))))
    add("structure: game with no rom", c_dat(c_game("", "ULUS-12345") + c_game(c_rom(SHA_A))))
    add("structure: rom outside a game", c_dat(c_rom(SHA_C) + c_game(c_rom(SHA_A))))
    add("structure: rom in the header", good.replace("<date>", c_rom(SHA_C) + "<date>"))
    add("structure: rom with content", c_dat(c_game(c_rom(SHA_A)[:-2] + "></rom>")))
    add("structure: second datafile inside", c_dat(c_game(c_rom(SHA_A)) + "<datafile/>"))
    add("structure: self-closing root", '<?xml version="1.0"?>\n<datafile/>\n')
    add("structure: unknown elements kept out", c_dat(c_game(
        "<x a=\"1\"><y>text &amp; more</y><z/></x>" + c_rom(SHA_A))))
    add("structure: eight deep", c_dat(c_game("<a><b><c><d><e><f/></e></d></c></b></a>"
                                              + c_rom(SHA_A))))
    add("structure: nine deep", c_dat(c_game("<a><b><c><d><e><f><g/></f></e></d></c></b></a>"
                                             + c_rom(SHA_A))))
    deep = c_dat(GOOD_GAMES + "@@").split("@@")
    add("structure: a million tags deep", [T(deep[0]), R(500_000, "<a>"), R(500_000, "</a>"),
                                           T(deep[1])])
    add("structure: references in tags", around(c_dat(GOOD_GAMES + "@@"), 300_000,
                                                '<a b="&amp;&amp;&amp;">'))
    # Roms.
    add("rom: upper-case hashes", c_dat(c_game(c_rom(SHA_A.upper(), crc="0BADF00D"))))
    add("rom: malformed hashes", c_dat(c_game(c_rom("a" * 39) + c_rom("a" * 41) + c_rom("g" * 40)
                                               + c_rom(SHA_B))))
    add("rom: malformed CRCs", c_dat(c_game(c_rom(SHA_A, crc="0BADF00D") + c_rom(SHA_B, crc="0badf00")
                                            + c_rom(SHA_C, crc="0badf00dd"))))
    add("rom: sizes at the edge", c_dat(c_game(
        c_rom(SHA_A, size="18446744073709551615") + c_rom(SHA_B, size="18446744073709551616")
        + c_rom(SHA_C, size="+1") + c_rom("d" * 40, size=" 1")
        + c_rom("e" * 40, size="00000000000000000000001") + c_rom("f" * 40, size="0"))))
    add("rom: missing attributes", c_dat(c_game(f'<rom sha1="{SHA_A}" crc="0badf00d"/>'
                                                + c_rom(SHA_B))))
    add("rom: references in a hash", c_dat(c_game(c_rom("&#97;" + "a" * 39) + c_rom(SHA_B))))
    add("rom: one SHA-1 twice", c_dat(c_game(c_rom(SHA_A, size="1")) + c_game(c_rom(SHA_A, size="2"))
                                      + c_game(c_rom(SHA_B))))
    add("rom: none with a SHA-1", c_dat(c_game('<rom name="x" size="1"/>')))
    add("rom: forty thousand", [T(c_dat("").split("</datafile>")[0] + "<game>"),
                                SEQ(40_000, '<rom size="1" crc="0badf00d" sha1="%H"/>'),
                                T("</game>\n</datafile>\n")])
    add("rom: forty thousand and one", [T(c_dat("").split("</datafile>")[0] + "<game>"),
                                        SEQ(40_001, '<rom size="1" crc="0badf00d" sha1="%H"/>'),
                                        T("</game>\n</datafile>\n")])
    add("rom: forty thousand and one games", [T(c_dat("").split("</datafile>")[0]),
                                      SEQ(40_001, '\t<game name="G"><serial>ULUS%D</serial>'
                                          '<rom size="1024" crc="0badf00d" sha1="%H"/></game>\n'),
                                      T("</datafile>\n")])
    # Limits and cost (review S2: a comma flood once went uncounted).
    add("serial: comma flood", around(c_dat(c_game(c_rom(SHA_A), "@@")), 15_000_000, ","))
    add("serial: short parts flood", around(c_dat(c_game(c_rom(SHA_A), "@@")), 7_500_000, "x,"))
    add("serial: comma floods at the bound", [
        T(c_dat("").split("</datafile>")[0] + c_game(c_rom(SHA_A))),
        R(30_000, "<game><serial>" + "," * 256 + "</serial></game>"),
        T("</datafile>\n")])
    add("serial: eighty thousand and one IDs", [
        T(c_dat("").split("</datafile>")[0] + c_game(c_rom(SHA_A))),
        SEQ(5_001, "<game><serial>" + ", ".join(
            f"AA{chr(65 + j // 26)}{chr(65 + j % 26)}%D" for j in range(16)) + "</serial></game>"),
        T("</datafile>\n")])
    before = c_dat("").split("</datafile>")[0] + "<game>" + c_rom(SHA_A)
    after = "</game>\n</datafile>\n"
    fill = disc_verify.MAX_DAT_TAGS - (before + after).count("<")
    add("tags: at the bound", [T(before), R(fill, "<a/>"), T(after)])
    add("tags: one more", [T(before), R(fill + 1, "<a/>"), T(after)])
    before = '<?xml version="1.0"?>\n<?pad '
    after = "?><datafile>" + HEADER + GOOD_GAMES + "</datafile>\n"
    fill = disc_verify.MAX_DAT_BYTES - len(before) - len(after)
    add("size: 16 MiB", [T(before), R(fill, "p"), T(after)])
    add("size: one byte more", [T(before), R(fill + 1, "p"), T(after)])
    # Zips (review B3: zips whose structure lies).
    payload = [T(good)]
    other = [T(c_dat(c_game(c_rom(SHA_C), "ULUS-99999")))]
    good_bytes, other_bytes = good.encode(), build_pieces(other)
    add("zip: deflated", zip_case(payload))
    add("zip: stored", zip_case(payload, method=0))
    deflater = zlib.compressobj(9, zlib.DEFLATED, -15)
    add("zip: zlib's dynamic codes", zip_case(
        payload, packed=[("h", deflater.compress(good_bytes) + deflater.flush())]))
    add("zip: UTF-8 name flag", zip_case(payload, flags=0x0800))
    add("zip: deflate option flags", zip_case(payload, flags=0x0006))
    add("zip: flag 0x0010", zip_case(payload, flags=0x0010))
    longer = good_bytes + b"<!-- trailing -->" + b"x" * 1000
    add("zip: declared size too small, deflated", zip_case(payload, packed=[("h", fixed_deflate(longer))]))
    add("zip: declared size too small, stored", zip_case(
        payload, method=0, packed=[("h", longer)], usize=len(good_bytes)))
    add("zip: declared size too large, deflated", zip_case(payload, usize=len(good_bytes) + 5000))
    add("zip: declared size too large, stored", zip_case(
        payload, method=0, usize=len(good_bytes) + 10, csize=len(good_bytes)))
    add("zip: declared size 0", zip_case(payload, usize=0))
    add("zip: compressed size short", zip_case(payload, csize=len(fixed_deflate(good_bytes)) - 1))
    add("zip: stored data into the directory", zip_case(
        payload, method=0, csize=len(good_bytes) + 30, usize=len(good_bytes)))
    add("zip: count two", zip_case(payload, count=2))
    add("zip: counts differ", zip_case(payload, count2=2))
    add("zip: count zero", zip_case(payload, count=0))
    local_a = _local_header(b"a.dat", 0, zlib.crc32(good_bytes), len(good_bytes), len(good_bytes))
    local_b = _local_header(b"b.dat", 0, zlib.crc32(other_bytes), len(other_bytes), len(other_bytes))
    offset_b = len(local_a) + len(good_bytes)
    central_a = _central(b"a.dat", 0, zlib.crc32(good_bytes), len(good_bytes), len(good_bytes), 0)
    central_b = _central(b"b.dat", 0, zlib.crc32(other_bytes), len(other_bytes), len(other_bytes),
                         offset_b)
    body = local_a + good_bytes + local_b + other_bytes
    add("zip: two files", [("h", body + central_a + central_b
                                 + _eocd(2, len(central_a) + len(central_b), len(body)))])
    add("zip: two directories, the end record naming the first", [
        ("h", body + central_a + central_b + _eocd(1, len(central_a), len(body)))])
    add("zip: an end record in the comment", [
        ("h", body + central_a + central_b + _eocd(
            1, len(central_a), len(body),
            _eocd(1, len(central_b), len(body) + len(central_a)) + b"tail-bytes"))])
    add("zip: a comment", zip_case(payload, comment=b"Redump"))
    add("zip: bytes after the end record", zip_case(payload, after_eocd=b"\0"))
    add("zip: bytes between the data and the directory", zip_case(payload, after_data=b"\0\0"))
    add("zip: path in the name", zip_case(payload, name=b"../../../../evil.dat"))
    add("zip: drive in the name", zip_case(payload, name=b"C:/Windows/evil.dat"))
    add("zip: NUL then .txt", zip_case(payload, name=b"x.dat\x00.txt"))
    add("zip: NUL then .dat", zip_case(payload, name=b"x.txt\x00.dat"))
    add("zip: upper-case .DAT", zip_case(payload, name=b"LIST.DAT"))
    add("zip: only .dat", zip_case(payload, name=b".dat"))
    add("zip: a .txt", zip_case(payload, name=b"list.txt"))
    add("zip: a folder named .dat", zip_case([T(b"")], method=0, name=b"d.dat/"))
    add("zip: local name differs", zip_case(payload, lname=b"other.dat"))
    add("zip: local method differs", zip_case(payload, method=0, lmethod=8))
    add("zip: local flags differ", zip_case(payload, lflags=0x0800))
    add("zip: ZIP64 extra block", zip_case(payload, extra=b"\x01\x00\x00\x00"))
    add("zip: ZIP64 extra block second", zip_case(
        payload, extra=b"\x55\x54\x01\x00\x00\x01\x00\x00\x00"))
    add("zip: extra field whole", zip_case(payload, extra=b"\x55\x54\x01\x00\x00",
                                           lextra=b"\x55\x54\x01\x00\x00"))
    add("zip: extra field cut", zip_case(payload, extra=b"\x55\x54\x05\x00\x00"))
    add("zip: local extra cut", zip_case(payload, lextra=b"\x55\x54\x05\x00\x00"))
    add("zip: local ZIP64 extra", zip_case(payload, lextra=b"\x01\x00\x00\x00"))
    add("zip: ZIP64 sizes", zip_case(payload, csize=0xFFFFFFFF))
    add("zip: ZIP64 offset", zip_case(payload, offset=0xFFFFFFFF))
    add("zip: not at offset 0", zip_case(payload, offset=4))
    add("zip: encrypted", zip_case(payload, flags=0x1))
    add("zip: strong encryption", zip_case(payload, flags=0x40))
    packed = fixed_deflate(good_bytes)
    descriptor = struct.pack("<IIII", 0x08074B50, zlib.crc32(good_bytes), len(packed), len(good_bytes))
    local = _local_header(b"list.dat", 8, 0, 0, 0, flags=0x8)
    central = _central(b"list.dat", 8, zlib.crc32(good_bytes), len(packed), len(good_bytes), 0,
                       flags=0x8)
    add("zip: data descriptor", [("reg", "dd"), *payload, ("out",), ("h", local), ("deflate", "dd"),
                                 ("h", descriptor + central + _eocd(
                                     1, len(central), len(local) + len(packed) + len(descriptor)))])
    add("zip: bzip2", zip_case(payload, method=12, packed=[T(b"BZh9garbage")]))
    add("zip: garbage deflate data", zip_case(payload, packed=[("h", b"\xff" * 100)]))
    add("zip: deflate data cut", zip_case(payload, packed=[("h", packed[:-5])]))
    add("zip: bytes after the deflate data", zip_case(payload, packed=[("h", packed + b"\0")]))
    add("zip: wrong CRC", zip_case(payload, crc=zlib.crc32(good_bytes) ^ 1))
    add("zip: empty file", zip_case([T(b"")], method=0))
    add("zip: empty file, deflated", zip_case([T(b"")]))
    add("zip: stored, near 16 MiB", zip_case([
        T('<?xml version="1.0"?>\n<?pad '), R(disc_verify.MAX_DAT_BYTES - 2000, "p"),
        T("?><datafile>" + HEADER + GOOD_GAMES + "</datafile>\n")], method=0))
    add("zip: declared over 16 MiB", zip_case(payload, usize=16 * 1024 * 1024 + 1))
    bomb = [T(good.replace("</datafile>\n", "")), R(17 << 20, " "), T("</datafile>\n")]
    add("zip: bomb declared at the cap", zip_case(bomb, crc=0, usize=16 * 1024 * 1024))
    add("zip: bomb declared small", zip_case(bomb, crc=0, usize=1000))
    add("zip: a zip in it", zip_case([("h", zip_bytes(good_bytes, 8))], name=b"inner.dat"))
    whole = zip_bytes(good_bytes, 8)
    for cut in (4, 30, 60, len(whole) - 22, len(whole) - 1):
        add(f"zip: cut at {cut}", [("h", whole[:cut])])
    add("zip: magic only", [("h", b"PK\x03\x04")])
    add("zip: disk number", zip_case(payload, disk=1))
    add("zip: empty, no file header", [("h", _eocd(0, 0, 0))])
    return cases


# -- mutations -------------------------------------------------------------------------------

_MASK64 = (1 << 64) - 1
INTERESTING_BYTES = (b"\x00\x09\x0a\x0b\x0c\x0d\x20\x21\x22\x26\x27\x2c\x2d\x2f\x3c\x3d\x3e\x3f"
                     b"\x5b\x5d\x7f\x80\xa0\xc2\xe3\xef\xff")
INSERTIONS = (b"\xc2\xa0", b"\xe3\x80\x80", b"\xc5\xbf", b"\xc4\xb1", b"\xef\xbb\xbf", b"&amp;",
              b"&#65;", b"&#x41;", b"&bogus;", b"<!--", b"-->", b"<?x?>", b"<!DOCTYPE x>",
              b"<![CDATA[", b"</datafile>", b"<datafile>", b"<header>", b"</header>", b"<game>",
              b"</game>", b"<serial>", b"</serial>", b"<rom/>",
              b'<rom sha1="0123456789abcdef0123456789abcdef01234567" crc="0badf00d" size="1"/>',
              b"<a>", b"</a>", b",", b"ULUS-10041", b"\xf0\x9f\x98\x80", b"\xed\xa0\x80",
              b"\xc0\xaf", b"\xf4\x90\x80\x80")
WHITESPACE_SWAPS = (b"\x0b", b"\x0c", b"\xc2\xa0", b"\xe3\x80\x80", b"\xc2\x85", b"\xe2\x80\x8b",
                    b"\x00", b"\x1f", b"", b"  ")


class SplitMix64:
    def __init__(self, seed: int) -> None:
        self.state = seed & _MASK64

    def next(self) -> int:
        self.state = (self.state + 0x9E3779B97F4A7C15) & _MASK64
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & _MASK64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & _MASK64
        return z ^ (z >> 31)

    def below(self, bound: int) -> int:
        return self.next() % bound


def bundled_windows() -> tuple[bytes, list[bytes]]:
    """The bundled zip, and eight short lists cut from its .dat: its header, eight games from
    spread places, </datafile>."""
    data = disc_verify.bundled_dat_path().read_bytes()
    text = disc_verify._dat_from_zip(data)
    header = text[:text.index(b"</header>\n") + len(b"</header>\n")]
    starts = []
    at = text.find(b"\t<game ")
    while at >= 0:
        starts.append(at)
        at = text.find(b"\t<game ", at + 1)
    ends = starts[1:] + [text.index(b"</datafile>")]
    windows = []
    for window in range(MUTATION_WINDOWS):
        first = window * (len(starts) - MUTATION_WINDOW_GAMES) // (MUTATION_WINDOWS - 1)
        last = first + MUTATION_WINDOW_GAMES - 1
        windows.append(header + text[starts[first]:ends[last]] + b"</datafile>\n")
    return data, windows


def _mutate_text(rng: SplitMix64, base: bytes) -> bytes:
    data = bytearray(base)
    for _ in range(1 + rng.below(4)):
        op = rng.below(8)
        size = len(data)
        if op == 2:
            at = rng.below(size + 1)
            data[at:at] = INSERTIONS[rng.below(len(INSERTIONS))]
        elif size == 0:
            continue
        elif op == 0:
            at = rng.below(size)
            data[at] ^= 1 << rng.below(8)
        elif op == 1:
            at = rng.below(size)
            data[at] = INTERESTING_BYTES[rng.below(len(INTERESTING_BYTES))]
        elif op == 3:
            at = rng.below(size)
            del data[at:at + 1 + rng.below(16)]
        elif op == 4:
            at = rng.below(size)
            span = data[at:at + 1 + rng.below(64)]
            data[at:at] = span
        elif op == 5:
            del data[rng.below(size + 1):]
        elif op == 6:
            at = rng.below(size)
            if 0x41 <= (data[at] & ~0x20) <= 0x5A:
                data[at] ^= 0x20
        else:
            at = rng.below(size)
            while at < size and data[at] not in b" \t\r\n":
                at += 1
            if at < size:
                data[at:at + 1] = WHITESPACE_SWAPS[rng.below(len(WHITESPACE_SWAPS))]
    return bytes(data)


def mutation(index: int, bundled: bytes, windows: list[bytes]) -> bytes:
    rng = SplitMix64((MUTATION_SEED << 32) | index)
    kind = index % 16
    if kind < 10:
        return _mutate_text(rng, windows[rng.below(len(windows))])
    if kind < 12:
        data = bytearray(zip_bytes(windows[rng.below(len(windows))], 0))
        for _ in range(1 + rng.below(3)):
            if not data:
                break
            region = rng.below(3)
            if region == 0:
                at = rng.below(min(38, len(data)))
            elif region == 1:
                at = len(data) - 1 - rng.below(min(76, len(data)))
            else:
                at = rng.below(len(data))
            change = rng.below(3)
            if change == 0:
                data[at] ^= 1 << rng.below(8)
            elif change == 1:
                data[at] = rng.below(256)
            else:
                del data[at:]
        return bytes(data)
    if kind < 15:
        data = bytearray(zip_bytes(windows[rng.below(len(windows))], 8))
        packed_size = int.from_bytes(data[18:22], "little")
        central = 38 + packed_size
        for _ in range(1 + rng.below(4)):
            if rng.below(4) < 3:
                at = 38 + rng.below(packed_size)
                data[at] ^= 1 << rng.below(8)
            else:
                size = (int.from_bytes(data[22:26], "little") + rng.below(17) - 8) & 0xFFFFFFFF
                data[22:26] = size.to_bytes(4, "little")
                data[central + 24:central + 28] = size.to_bytes(4, "little")
        return bytes(data)
    data = bytearray(bundled)
    start = 30 + int.from_bytes(data[26:28], "little") + int.from_bytes(data[28:30], "little")
    packed_size = int.from_bytes(data[18:22], "little")
    for _ in range(1 + rng.below(2)):
        at = start + rng.below(packed_size)
        data[at] ^= 1 << rng.below(8)
    return bytes(data)


def generate_corpus() -> str:
    lines = [CORPUS_HEADER.rstrip("\n"), ""]
    for name, pieces in corpus_cases():
        lines.append(f"case {name}")
        lines += piece_lines(pieces)
        lines.append(f"expect {outcome(build_pieces(pieces))}")
        lines.append("end")
        lines.append("")
    bundled, windows = bundled_windows()
    lines.append(f"mutations {MUTATION_SEED:x} {MUTATION_COUNT}")
    prefixes = [hashlib.sha1(outcome(mutation(index, bundled, windows)).encode("ascii"))
                .hexdigest()[:8] for index in range(MUTATION_COUNT)]
    for at in range(0, len(prefixes), 8):
        lines.append(" ".join(prefixes[at:at + 8]))
    lines.append("end")
    return "\n".join(lines) + "\n"


class ConformanceTests(unittest.TestCase):
    """This reader against the table both readers are held to (rules 1-10; the module
    documentation of nk_core/disc_verify.py)."""

    def test_the_table_is_what_the_generator_and_this_reader_give(self) -> None:
        expected = CORPUS.read_text(encoding="ascii").split("\n\n")
        actual = generate_corpus().split("\n\n")
        # Not strict: the first block that differs names the case (or the
        # mutations) before the lengths are compared.
        for want, got in zip(expected, actual, strict=False):
            self.assertEqual(got, want)
        self.assertEqual(len(actual), len(expected))

    def test_the_table_covers_every_refusal(self) -> None:
        text = CORPUS.read_text(encoding="ascii")
        results = re.findall(r"^expect (\S+)", text, re.M)
        self.assertGreaterEqual(len(results), 230)
        self.assertEqual(set(results), {
            "OK", "DISC_DAT_NOT_DATAFILE", "DISC_DAT_ENTITY_REFUSED", "DISC_DAT_EMPTY",
            "DISC_DAT_TOO_LARGE", "DISC_DAT_ZIP_INVALID"})
        mutations = re.search(r"^mutations (\S+) (\d+)\n(.*?)\nend$", text, re.M | re.S)
        assert mutations is not None
        self.assertEqual((int(mutations.group(1), 16), int(mutations.group(2))),
                         (MUTATION_SEED, MUTATION_COUNT))
        prefixes = mutations.group(3).split()
        self.assertEqual(len(prefixes), MUTATION_COUNT)
        # The mutations reach many results, not one refusal over and over.
        self.assertGreaterEqual(len(set(prefixes)), MUTATION_COUNT // 8)


if __name__ == "__main__":
    if sys.argv[1:] == ["--write-corpus"]:
        CORPUS.write_text(generate_corpus(), encoding="ascii", newline="\n")
    else:
        unittest.main()
