#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for tools/hle_focus_check.py (issue #307), on small compiled objects."""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hle_focus_check  # noqa: E402

CC = shutil.which("gcc") or shutil.which("cc")
NM = shutil.which("nm")
SOURCES = ["fake/focus_a.c", "fake/focus_b.c", "fake/focus_c.c"]


@unittest.skipUnless(CC and NM, "needs a C compiler and nm")
class FocusCheckTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def compile(self, name: str, body: str) -> Path:
        source = self.dir / f"{name}.c"
        source.write_text(body, encoding="utf-8")
        obj = self.dir / f"{name}.o"
        subprocess.run([str(CC), "-c", str(source), "-o", str(obj)], check=True)
        return obj

    def link(self, *objects: Path) -> Path:
        exe = self.dir / "t.exe"
        subprocess.run([str(CC), "-o", str(exe), *map(str, objects)], check=True)
        return exe

    def build(self) -> tuple[Path, Path, Path, Path]:
        a = self.compile("focus_a", "int a_only(void) { return 1; }\nint shared(void) { return 2; }\n")
        b = self.compile("focus_b", "int b_only(void) { return 3; }\n")
        c = self.compile("focus_c", "int c_only(void) { return 4; }\n")
        main = self.compile("main", "extern int a_only(void);\nint main(void) { return a_only() - 1; }\n")
        return a, b, c, main

    def run_check(self, exe: Path, include: list[str], required: list[str] | None = None) -> list[str]:
        problems: list[str] = hle_focus_check.check(exe, self.dir, include, [], required or [], NM, SOURCES)
        return problems

    def test_focused_executable_passes(self) -> None:
        a, _, _, main = self.build()
        self.assertEqual(self.run_check(self.link(main, a), ["fake/focus_a.c"], ["a_only"]), [])

    def test_linked_excluded_file_is_reported(self) -> None:
        a, b, _, main = self.build()
        problems = self.run_check(self.link(main, a, b), ["fake/focus_a.c"])
        self.assertEqual(len(problems), 1)
        self.assertIn("focus_b.c is linked", problems[0])
        self.assertIn("b_only", problems[0])

    def test_stub_redefining_included_symbol_is_reported(self) -> None:
        a, _, _, main = self.build()
        stub = self.compile("stub", "int shared(void) { return 9; }\n")
        problems = hle_focus_check.check(self.link(main, a), self.dir, ["fake/focus_a.c"], [stub], [], NM, SOURCES)
        self.assertEqual(len(problems), 1)
        self.assertIn("stub stub.o redefines", problems[0])
        self.assertIn("shared", problems[0])

    def test_missing_required_symbol_is_reported(self) -> None:
        a, _, _, main = self.build()
        problems = self.run_check(self.link(main, a), ["fake/focus_a.c"], ["not_there"])
        self.assertEqual(problems, ["required symbol not_there is not defined in t.exe"])

    def test_missing_excluded_object_fails_closed(self) -> None:
        a, b, _, main = self.build()
        exe = self.link(main, a)
        b.unlink()
        with self.assertRaises(hle_focus_check.FocusError):
            self.run_check(exe, ["fake/focus_a.c"])

    def test_unknown_included_file_fails_closed(self) -> None:
        a, _, _, main = self.build()
        with self.assertRaises(hle_focus_check.FocusError):
            self.run_check(self.link(main, a), ["fake/focus_zzz.c"])


SYNTHETIC_NM_SAMPLE = "\n".join(
    [
        "",
        "   ",
        "file.o:",
        "0000000000000000 T global_func",
        "0000000000000010 t local_func",
        "0000000000000020 D global_data",
        "0000000000000024 d local_data",
        "                 U undef_func",
        "0000000000000030 W weak_defined",
        "0000000000000038 V weak_obj",
        "                 w weak_undef",
        "                 A no_addr_sym",
        "0000000000000040 R .refptr.imported_var",
        "0000000000000048 T .refptr.imported_fn",
        "0000000000000050 T ver_fn@VER_1",
        "0000000000000058 T default_ver_fn@@GLIBC_2.14",
        "0000000000000060 0000000000000010 T four_field_sym",
        "0000000000000070 TT multi_char_type",
    ]
) + "\n"


class NmParsingPinTests(unittest.TestCase):
    """Pin hle_focus_check.defined_symbols output on synthetic nm samples."""

    def _run_with_sample(self, *, local_too: bool, text_only: bool) -> tuple[list[str], set[str]]:
        captured: list[list[str]] = []

        def fake_run(cmd: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
            captured.append(list(cmd))
            return subprocess.CompletedProcess(cmd, 0, stdout=SYNTHETIC_NM_SAMPLE, stderr="")

        with mock.patch("subprocess.run", side_effect=fake_run):
            symbols = hle_focus_check.defined_symbols(
                Path("file.o"), "fake-nm", local_too=local_too, text_only=text_only
            )
        return captured[0], symbols

    def test_defined_symbols_all_types_pins_current_behavior(self) -> None:
        cmd, symbols = self._run_with_sample(local_too=False, text_only=False)
        self.assertEqual(cmd, ["fake-nm", "-g", "--defined-only", "file.o"])
        self.assertEqual(
            symbols,
            {
                "global_func",
                "local_func",
                "global_data",
                "local_data",
                "weak_defined",
                "weak_obj",
                "ver_fn@VER_1",
                "default_ver_fn@@GLIBC_2.14",
                "multi_char_type",
            },
        )

    def test_defined_symbols_text_only_pins_current_behavior(self) -> None:
        cmd, symbols = self._run_with_sample(local_too=True, text_only=True)
        self.assertEqual(cmd, ["fake-nm", "--defined-only", "file.o"])
        self.assertEqual(
            symbols,
            {
                "global_func",
                "local_func",
                "ver_fn@VER_1",
                "default_ver_fn@@GLIBC_2.14",
            },
        )

    def test_defined_symbols_errors_raise_focus_error(self) -> None:
        fail = subprocess.CompletedProcess(["fake-nm"], 1, stdout="", stderr="bad object")
        with mock.patch("subprocess.run", return_value=fail):
            with self.assertRaises(hle_focus_check.FocusError):
                hle_focus_check.defined_symbols(Path("file.o"), "fake-nm")
        with mock.patch("subprocess.run", side_effect=OSError("no such binary")):
            with self.assertRaises(hle_focus_check.FocusError):
                hle_focus_check.defined_symbols(Path("file.o"), "fake-nm")


class SharedNmParserTests(unittest.TestCase):
    """Unit tests for the shared nm parser (F-5f35396b)."""

    def test_parse_nm_defined_symbols_refptr_and_version(self) -> None:
        canned = "\n".join(
            [
                "file.o:",
                "",
                "0000000000000000 T foo",
                "0000000000000008 t bar",
                "0000000000000010 D baz",
                "0000000000000018 R .refptr.hidden_import",
                "0000000000000020 T sym@VER_1",
                "0000000000000028 T sym2@@VER_2",
                "0000000000000030 0000000000000004 T sized_sym",
                "                 U undef_sym",
            ]
        )
        self.assertEqual(
            hle_focus_check.parse_nm_defined_symbols(canned),
            {"foo", "bar", "baz", "sym@VER_1", "sym2@@VER_2"},
        )
        self.assertEqual(
            hle_focus_check.parse_nm_defined_symbols(canned, text_only=True),
            {"foo", "bar", "sym@VER_1", "sym2@@VER_2"},
        )
        self.assertEqual(
            hle_focus_check.parse_nm_defined_symbols(canned, strip_version=True, strict_columns=False),
            {"foo", "bar", "baz", "sym", "sym2", "sized_sym"},
        )

    def test_nm_defined_symbols_raises_on_nonzero_exit(self) -> None:
        fail = subprocess.CompletedProcess(["fake-nm"], 2, stdout="", stderr="invalid format")
        with mock.patch("subprocess.run", return_value=fail):
            with self.assertRaisesRegex(RuntimeError, "failed"):
                hle_focus_check.nm_defined_symbols(Path("bad.o"), "fake-nm")


if __name__ == "__main__":
    unittest.main()
