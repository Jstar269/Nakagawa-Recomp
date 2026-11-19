# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for the relocatable compilation database tool (issue #368)."""

from __future__ import annotations

import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import compdb_relocate  # noqa: E402

OLD = "/old/checkout"
BS = chr(92)


def _entry() -> dict:
    return {
        "directory": OLD,
        "file": OLD + "/src/rt/a.c",
        "arguments": ["gcc", "-I" + OLD + "/src", "-c", OLD + "/src/rt/a.c",
                      "-o", OLD + "/build/a.o", "-I/usr/include"],
    }


class RelocateTests(unittest.TestCase):
    def test_paths_under_root_become_relative(self) -> None:
        (out,) = compdb_relocate.relocate([_entry()], OLD, "/new/place")
        self.assertEqual(out["directory"], "/new/place")
        self.assertEqual(out["file"], "src/rt/a.c")
        self.assertEqual(out["arguments"], ["gcc", "-Isrc", "-c", "src/rt/a.c",
                                            "-o", "build/a.o", "-I/usr/include"])

    def test_no_old_root_survives_anywhere(self) -> None:
        (out,) = compdb_relocate.relocate([_entry()], OLD, "/new/place")
        self.assertNotIn(OLD, json.dumps(out))

    def test_backslash_roots_and_command_strings(self) -> None:
        root = "C:" + BS + "w" + BS + "nk"
        entry = {"directory": root, "file": root + BS + "x.c",
                 "command": "gcc -IC:/w/nk/src -c C:/w/nk/x.c"}
        (out,) = compdb_relocate.relocate([entry], root, "D:/moved")
        self.assertEqual(out["file"], "x.c")
        self.assertEqual(out["command"], "gcc -Isrc -c x.c")

    def test_sibling_prefix_is_not_rewritten(self) -> None:
        entry = {"directory": OLD, "file": OLD + "-other/x.c", "arguments": []}
        (out,) = compdb_relocate.relocate([entry], OLD, "/n")
        self.assertEqual(out["file"], OLD + "-other/x.c")

    def test_glued_and_quoted_path_options_leave_no_checkout_path(self) -> None:
        entry = {"directory": OLD, "file": OLD + "/a.c",
                 "arguments": ["gcc", "-isystem" + OLD + "/inc", "-iquote" + OLD + "/q", "--sysroot=" + OLD + "/s",
                               "-MF" + OLD + "/build/a.d", "-isystem/usr/include", "--sysroot=/opt/sdk"]}
        (out,) = compdb_relocate.relocate([entry], OLD, "/n")
        self.assertEqual(out["arguments"], ["gcc", "-isysteminc", "-iquoteq", "--sysroot=s", "-MFbuild/a.d",
                                            "-isystem/usr/include", "--sysroot=/opt/sdk"])
        spaced = {"directory": "C:/a b", "file": "C:/a b/x.c",
                  "command": 'gcc  -I"C:/a b/src" -isystemC:/a/inc "C:/a b/sub dir/x.c" -o "C:/a b/x.o"'}
        (out,) = compdb_relocate.relocate([spaced], "C:/a b", "D:/m")
        self.assertEqual(out["command"], 'gcc  -Isrc -isystemC:/a/inc "sub dir/x.c" -o x.o')
        self.assertNotIn("C:/a b", json.dumps(out))

    def test_a_root_that_is_no_checkout_folder_is_refused(self) -> None:
        # With no directory (or --old-root /) every absolute path used to lose its leading slash.
        with tempfile.TemporaryDirectory() as tmp:
            db = Path(tmp) / "compile_commands.json"
            original = json.dumps([{"file": "/usr/include/x.h", "arguments": ["-I/usr/include"]}])
            db.write_text(original, encoding="utf-8")
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(compdb_relocate.main([str(db), "--root", tmp]), 1)
                self.assertEqual(compdb_relocate.main([str(db), "--old-root", "/", "--root", tmp]), 1)
            self.assertEqual(db.read_text(encoding="utf-8"), original)
        for root in ("", "/", "C:/", "C:" + BS):
            with self.subTest(root=root), self.assertRaisesRegex(ValueError, "COMPDB_ROOT_INVALID"):
                compdb_relocate.relocate([_entry()], root, "/n")

    def test_each_entry_keeps_its_own_folder_under_the_new_root(self) -> None:
        entries = [{"directory": OLD + "/build", "file": "../src/a.c", "arguments": ["gcc", "-I../src", "-c", "../src/a.c"]},
                   {"directory": OLD, "file": OLD + "/src/b.c", "arguments": ["gcc", "-c", OLD + "/src/b.c"]},
                   {"directory": "/elsewhere", "file": "c.c", "arguments": []}]
        first, second, third = compdb_relocate.relocate(entries, OLD, "/new")
        self.assertEqual((first["directory"], first["file"]), ("/new/build", "../src/a.c"))
        self.assertEqual((second["directory"], second["file"]), ("/new", "src/b.c"))
        self.assertEqual(third["directory"], "/elsewhere")
        # The default old root is the folder every entry is in, not the first entry's folder.
        self.assertEqual(compdb_relocate.default_old_root(entries[:2]), OLD)
        with tempfile.TemporaryDirectory() as tmp:
            db = Path(tmp) / "compile_commands.json"
            db.write_text(json.dumps(entries[:2]), encoding="utf-8")
            self.assertEqual(compdb_relocate.main([str(db), "--root", tmp]), 0)
            first, second = json.loads(db.read_text(encoding="utf-8"))
            self.assertEqual(first["directory"], Path(tmp).resolve().as_posix() + "/build")
            self.assertEqual(second["file"], "src/b.c")

    def test_main_rewrites_file_in_place(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            db = Path(tmp) / "compile_commands.json"
            db.write_text(json.dumps([_entry()]), encoding="utf-8")
            self.assertEqual(compdb_relocate.main([str(db), "--root", tmp]), 0)
            (out,) = json.loads(db.read_text(encoding="utf-8"))
            self.assertEqual(out["directory"], Path(tmp).resolve().as_posix())
            self.assertEqual(out["file"], "src/rt/a.c")


if __name__ == "__main__":
    unittest.main()
