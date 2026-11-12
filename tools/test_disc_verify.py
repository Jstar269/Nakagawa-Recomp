# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""nk_core.disc_verify and the bring-up's library records keyed by disc ID plus hash.

Synthetic data, a datafile in Redump's layout naming made-up images, for every case but one:
``BundledListTests`` reads the real list the project ships (assets/redump), checks it against
its pin and uses it as the default. The C reader (src/core/nk_disc_verify.c) has the same cases
in tests/native/test_disc_verify.c.
"""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from typing import cast
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
        many = ", ".join(f"TEST-7{index:04d}" for index in range(20))
        text = (f"<datafile><game name=\"x\"><serial>{many}</serial>"
                f"<serial>TEST&#45;79999</serial>{rom.format(size=len(RETAIL))}</game>"
                f"<game name=\"y\">{rom.format(size=2**64).replace(retail.sha1, 'f' * 40)}"
                "</game></datafile>")
        dat = disc_verify.parse_dat_text(text)
        # Only the five predefined entities are decoded, as in C.
        self.assertEqual(dat.roms_by_sha1[retail.sha1].name, "A & B &#38; C")
        # At most 16 serials per game, and a numeric entity is not decoded into one.
        self.assertEqual(len(dat.serials), 16)
        self.assertNotIn("TEST79999", dat.serials)
        # A size past 64 bits is a malformed row.
        self.assertEqual((len(dat), dat.rejected_roms), (1, 1))

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

    def test_nk_cli_prints_the_list_version_without_control_characters(self) -> None:
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
            shown = io.StringIO()
            with contextlib.redirect_stdout(shown):
                self.assertEqual(nk_cli.cmd_redump_import(argparse.Namespace(
                    datafile=source, user_data_root=root / "data")), 0)
            self.assertIn("(version 1[31mred)", shown.getvalue())
            self.assertNotIn("\x1b", shown.getvalue())
            self.assertNotIn("\x07", shown.getvalue())

    def test_import_saves_the_bytes_it_checked(self) -> None:
        # The file is read once: a source that changes after the check can't put
        # unchecked bytes in the data folder.
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "chosen.dat"
            checked = datafile().encode("utf-8")
            source.write_bytes(checked)
            real_parse = disc_verify._parse_dat_bytes

            def parse_then_change(data: bytes) -> disc_verify.RedumpDat:
                dat = real_parse(data)
                source.write_bytes(b"not a datafile, written after the check")
                return dat

            with mock.patch.object(disc_verify, "_parse_dat_bytes", parse_then_change):
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


#: The most characters the reader may look at for each character of its input. The scanner
#: counts each one every time it looks at it, so this bounds the work of any input (#1731
#: review B1, S1: lists that once cost time quadratic in their size).
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
        scanner = disc_verify._DatScanner(text)
        try:
            scanner.parse()
        except disc_verify.DiscVerifyError as exc:
            self.assertTrue(exc.boundary_code.startswith("DISC_DAT_"), exc.boundary_code)
        return scanner.examined

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


if __name__ == "__main__":
    unittest.main()
