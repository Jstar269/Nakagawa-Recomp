# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Contract tests for tools/isolated_run.py, on a synthetic Memory Stick tree and stub commands."""

from __future__ import annotations

import contextlib
import hashlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import isolated_run  # noqa: E402

# Writes argv[2] under the directory named by the environment variable argv[1].
STUB = (
    "import os, sys, pathlib\n"
    "t = pathlib.Path(os.environ[sys.argv[1]]) / sys.argv[2]\n"
    "t.parent.mkdir(parents=True, exist_ok=True)\n"
    "t.write_text('x')\n"
)


class IsolatedRunTests(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        base = Path(tmp.name)
        self.ms = base / "ms"
        (self.ms / "PSP" / "SAVEDATA").mkdir(parents=True)
        (self.ms / "PSP" / "SAVEDATA" / "A.BIN").write_bytes(b"save")
        self.settings = base / "settings.ini"
        self.settings.write_text("a=1")
        self.work = base / "work"
        self.work.mkdir()

    def _main(self, *tail: str, settings: bool = False) -> tuple[int, str]:
        argv = ["--memstick", str(self.ms), "--work-dir", str(self.work)]
        if settings:
            argv += ["--settings", str(self.settings), "--settings-env", "SR_TEST_SETTINGS"]
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            code = isolated_run.main([*argv, "--", sys.executable, *tail])
        return code, out.getvalue()

    def test_write_to_copy_passes(self) -> None:
        code, out = self._main("-c", STUB, "SR_MEMSTICK", "PSP/SAVEDATA/NEW.BIN")
        self.assertEqual(code, 0, out)
        self.assertIn("ORIGINALS_UNCHANGED", out)
        self.assertFalse((self.ms / "PSP" / "SAVEDATA" / "NEW.BIN").exists())
        self.assertEqual(
            len(list(self.work.glob("isolated-*/memstick/PSP/SAVEDATA/NEW.BIN"))), 1)

    def test_settings_write_to_copy_passes(self) -> None:
        # The variable names the copied settings file, so write beside it, not under it.
        stub = ("import os, sys, pathlib\n"
                "pathlib.Path(os.environ[sys.argv[1]]).with_name(sys.argv[2]).write_text('x')\n")
        code, out = self._main("-c", stub, "SR_TEST_SETTINGS", "extra.ini", settings=True)
        self.assertEqual(code, 0, out)
        self.assertEqual(self.settings.read_text(), "a=1")
        # The write must land beside the copy: an exported original path would put
        # extra.ini beside the original and leave settings.ini itself unchanged.
        self.assertFalse((self.settings.parent / "extra.ini").exists())
        self.assertEqual(len(list(self.work.glob("isolated-*/settings0/extra.ini"))), 1)

    def test_write_to_original_fails_and_names_file(self) -> None:
        stub = "import pathlib; pathlib.Path(%r).write_bytes(b'bad')" % str(
            self.ms / "PSP" / "SAVEDATA" / "A.BIN")
        code, out = self._main("-c", stub)
        self.assertEqual(code, isolated_run.ORIGINAL_CHANGED_STATUS)
        self.assertIn("ORIGINAL_CHANGED", out)
        self.assertIn("PSP/SAVEDATA/A.BIN", out)

    def test_created_file_in_original_is_named(self) -> None:
        stub = "import pathlib; pathlib.Path(%r).write_text('x')" % str(self.ms / "NEW.TXT")
        code, out = self._main("-c", stub)
        self.assertEqual(code, isolated_run.ORIGINAL_CHANGED_STATUS)
        self.assertIn("NEW.TXT", out)

    def test_changed_settings_original_fails(self) -> None:
        stub = "import pathlib; pathlib.Path(%r).write_text('b=2')" % str(self.settings)
        code, out = self._main("-c", stub, settings=True)
        self.assertEqual(code, isolated_run.ORIGINAL_CHANGED_STATUS)
        self.assertIn("settings0", out)

    def test_work_dir_inside_original_refused(self) -> None:
        self.work = self.ms / "work"
        self.work.mkdir()
        code, _ = self._main("-c", "pass")
        self.assertEqual(code, isolated_run.SETUP_FAILED_STATUS)

    def test_command_status_passes_through(self) -> None:
        code, _ = self._main("-c", "raise SystemExit(7)")
        self.assertEqual(code, 7)

    def test_wrapper_codes_never_collide_with_a_commands_own_status(self) -> None:
        # A gate's 3 (no verdict) and argparse's 2 pass through and cannot be mistaken for
        # the wrapper's "original changed" or "setup failed".
        self.assertEqual(len({2, 3, isolated_run.SETUP_FAILED_STATUS,
                              isolated_run.ORIGINAL_CHANGED_STATUS}), 4)
        for own in (2, 3):
            with self.subTest(status=own):
                code, out = self._main("-c", f"raise SystemExit({own})")
                self.assertEqual(code, own)
                self.assertIn(f"ORIGINALS_UNCHANGED: command_status={own}", out)

    def test_a_copy_failure_is_a_setup_failure(self) -> None:
        def refuse(*_args, **_kwargs):
            raise OSError("synthetic copy failure")

        with mock.patch.object(isolated_run.shutil, "copytree", refuse):
            code, out = self._main("-c", "raise SystemExit(0)")
        self.assertEqual(code, isolated_run.SETUP_FAILED_STATUS)
        self.assertIn("setup failed", out)
        self.assertIn("synthetic copy failure", out)
        self.assertNotIn("ORIGINALS_UNCHANGED", out)

    def test_a_command_that_cannot_start_still_rechecks_the_originals(self) -> None:
        argv = ["--memstick", str(self.ms), "--work-dir", str(self.work), "--",
                str(self.work / "no-such-command")]
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            code = isolated_run.main(argv)
        self.assertEqual(code, isolated_run.SETUP_FAILED_STATUS, out.getvalue())
        self.assertIn("could not be started", out.getvalue())
        self.assertIn("ORIGINALS_UNCHANGED: command_status=not-started", out.getvalue())

    def test_snapshot_hashes_in_chunks_not_whole_files(self) -> None:
        # A multi-GiB ISO on the stick must never be read into memory in one piece.
        big = self.ms / "ISO" / "BIG.ISO"
        big.parent.mkdir()
        big.write_bytes(b"Z" * (3 * isolated_run._HASH_CHUNK + 7))

        def whole_file_read(_self, *_args, **_kwargs):
            raise AssertionError("snapshot read a whole file into memory")

        with mock.patch.object(Path, "read_bytes", whole_file_read):
            tree = isolated_run.snapshot(self.ms)
            single = isolated_run.snapshot(big)
        expected = hashlib.sha256(big.read_bytes()).hexdigest()
        self.assertEqual(tree["ISO/BIG.ISO"], expected)
        self.assertEqual(single, {".": expected})


if __name__ == "__main__":
    unittest.main()
