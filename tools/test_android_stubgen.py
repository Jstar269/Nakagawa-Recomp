# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""tools/android_stubgen.py: which symbols a sysroot stub exports, and how.

The symbol files here are synthetic, written for these tests in the documented format.
The pinned AOSP files are checked by the sysroot build itself.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import android_stubgen as stubgen  # noqa: E402

SAMPLE = """\
# A synthetic symbol file in the AOSP format.
LIBX { # introduced=21
  global:
    x_open;
    x_errno; # var
    x_hook; # weak
    x_arm_only; # arm
    x_arm64_later; # arm64 introduced-arm64=30
    x_both; # arm arm64
    x_platform; # platform-only
    x_apex; # apex
  local:
    *;
};

LIBX_Q { # introduced=Q
  global:
    x_q;
    x_systemapi; # systemapi
} LIBX;

LIBX_R { # introduced=R
  global:
    x_r;
} LIBX_Q;

LIBX_SYMBOL_EARLIER { # introduced=31
  global:
    x_backported; # introduced-arm64=28
} LIBX_R;

LIBX_PRIVATE {
  global:
    x_private;
} LIBX_Q;

LIBX_LATE_VERSION { # versioned=30
  global:
    x_late;
} LIBX;
"""


def _names(versions: list[stubgen.StubVersion]) -> dict[str | None, list[str]]:
    return {version.name: [symbol.name for symbol in version.symbols] for version in versions}


class SelectTests(unittest.TestCase):
    def setUp(self) -> None:
        self.blocks = stubgen.parse_symbol_file(SAMPLE, "sample.map.txt")

    def test_api_29_on_arm64_keeps_only_the_public_symbols_of_that_level(self) -> None:
        versions = stubgen.select(self.blocks, 29, "arm64")
        self.assertEqual(
            _names(versions),
            {
                "LIBX": ["x_open", "x_errno", "x_hook", "x_both"],
                "LIBX_Q": ["x_q"],
                None: ["x_late"],
            },
        )

    def test_a_later_api_adds_its_blocks_and_symbols(self) -> None:
        names = _names(stubgen.select(self.blocks, 30, "arm64"))
        self.assertIn("x_arm64_later", names["LIBX"])
        self.assertEqual(names["LIBX_R"], ["x_r"])
        self.assertEqual(names["LIBX_LATE_VERSION"], ["x_late"])
        self.assertNotIn(None, names)

    def test_the_architecture_filters_symbols_and_arch_introduced_wins(self) -> None:
        arm = _names(stubgen.select(self.blocks, 29, "arm"))
        self.assertEqual(arm["LIBX"], ["x_open", "x_errno", "x_hook", "x_arm_only", "x_both"])
        self.assertNotIn("LIBX_SYMBOL_EARLIER", arm)

    def test_a_block_introduced_later_hides_even_symbols_tagged_earlier(self) -> None:
        self.assertNotIn("LIBX_SYMBOL_EARLIER", _names(stubgen.select(self.blocks, 30, "arm64")))
        self.assertEqual(
            _names(stubgen.select(self.blocks, 31, "arm64"))["LIBX_SYMBOL_EARLIER"], ["x_backported"]
        )

    def test_platform_blocks_and_tags_never_reach_a_stub(self) -> None:
        exported = [
            symbol.name
            for version in stubgen.select(self.blocks, stubgen.FUTURE_API, "arm64")
            for symbol in version.symbols
        ]
        for name in ("x_platform", "x_apex", "x_systemapi", "x_private"):
            self.assertNotIn(name, exported)

    def test_a_symbol_tagged_versioned_is_unversioned_below_that_level(self) -> None:
        text = (
            "LIBY {\n  global:\n    y_old;\n};\n"
            "LIBY_N { # introduced=24\n  global:\n    y_sdk; # versioned=29\n    y_new;\n} LIBY;\n"
        )
        blocks = stubgen.parse_symbol_file(text)
        self.assertEqual(
            _names(stubgen.select(blocks, 28, "arm64")),
            {"LIBY": ["y_old"], "LIBY_N": ["y_new"], None: ["y_sdk"]},
        )
        self.assertEqual(
            _names(stubgen.select(blocks, 29, "arm64")),
            {"LIBY": ["y_old"], "LIBY_N": ["y_sdk", "y_new"]},
        )
        script = stubgen.render_version_script(stubgen.select(blocks, 28, "arm64"))
        self.assertNotIn("y_sdk", script)

    def test_unversioned_exports_the_same_symbols_without_versions(self) -> None:
        versions = stubgen.select(self.blocks, 29, "arm64", unversioned=True)
        self.assertEqual({version.name for version in versions}, {None})
        self.assertEqual(
            [symbol.name for version in versions for symbol in version.symbols],
            ["x_open", "x_errno", "x_hook", "x_both", "x_q", "x_late"],
        )
        self.assertEqual(stubgen.render_version_script(versions), "")

    def test_extern_cxx_patterns_in_a_platform_block_are_skipped(self) -> None:
        text = (
            "LIBZ {\n  global:\n    z_open;\n};\n"
            "LIBZ_PLATFORM { # platform-only\n  global:\n    z_internal;\n"
            '    extern "C++" {\n        Z_register*;\n        Z_other*;\n    };\n} LIBZ;\n'
        )
        self.assertEqual(
            _names(stubgen.select(stubgen.parse_symbol_file(text), 29, "arm64")), {"LIBZ": ["z_open"]}
        )

    def test_an_exported_extern_cxx_pattern_is_refused(self) -> None:
        text = 'LIBZ {\n  global:\n    extern "C++" {\n        Z_register*;\n    };\n};\n'
        with self.assertRaisesRegex(stubgen.SymbolFileError, "a C stub cannot define it"):
            stubgen.select(stubgen.parse_symbol_file(text), 29, "arm64")

    def test_an_unclosed_extern_cxx_block_is_refused(self) -> None:
        text = 'LIBZ {\n  global:\n    extern "C++" {\n        Z_register*;\n'
        with self.assertRaisesRegex(stubgen.SymbolFileError, "block 'LIBZ' is not closed"):
            stubgen.parse_symbol_file(text)

    def test_var_and_weak_shape_the_c_stub(self) -> None:
        text = stubgen.render_stub_c(stubgen.select(self.blocks, 29, "arm64"), "libx.so")
        self.assertIn("void x_open(void) {}", text)
        self.assertIn("int x_errno = 0;", text)
        self.assertIn("__attribute__((weak)) void x_hook(void) {}", text)

    def test_the_version_script_chains_exported_bases_only(self) -> None:
        script = stubgen.render_version_script(stubgen.select(self.blocks, 29, "arm64"))
        self.assertEqual(
            script,
            "LIBX {\n  global:\n    x_open;\n    x_errno;\n    x_hook;\n    x_both;\n};\n"
            "LIBX_Q {\n  global:\n    x_q;\n} LIBX;\n",
        )
        self.assertNotIn("local:", script)

    def test_release_names_and_numbers_are_the_same_level(self) -> None:
        self.assertEqual(stubgen.parse_api("Q", "t"), 29)
        self.assertEqual(stubgen.parse_api("29", "t"), 29)
        self.assertEqual(stubgen.parse_api("Baklava", "t"), 36)
        with self.assertRaisesRegex(stubgen.SymbolFileError, "unknown API level 'Zed'"):
            stubgen.parse_api("Zed", "t")


class FormatErrorTests(unittest.TestCase):
    def assert_refused(self, text: str, message: str) -> None:
        with self.assertRaisesRegex(stubgen.SymbolFileError, message):
            stubgen.select(stubgen.parse_symbol_file(text, "bad.map.txt"), 29, "arm64")

    def test_an_unclosed_block_is_refused(self) -> None:
        self.assert_refused("LIBX {\n  global:\n    a;\n", "block 'LIBX' is not closed")

    def test_a_symbol_outside_a_global_scope_is_refused(self) -> None:
        self.assert_refused("LIBX {\n    a;\n};\n", r"bad.map.txt:2: symbol 'a' is outside")

    def test_a_named_local_symbol_is_refused(self) -> None:
        self.assert_refused("LIBX {\n  local:\n    a;\n};\n", "may only hide everything")

    def test_a_symbol_exported_twice_is_refused(self) -> None:
        text = "LIBX {\n  global:\n    a;\n};\nLIBY {\n  global:\n    a;\n} LIBX;\n"
        self.assert_refused(text, "'a' is already exported by LIBX")

    def test_a_block_defined_twice_is_refused(self) -> None:
        self.assert_refused("LIBX {\n};\nLIBX {\n};\n", "defined twice: LIBX")

    def test_an_unknown_level_in_a_tag_is_refused(self) -> None:
        self.assert_refused("LIBX { # introduced=Zed\n};\n", "unknown API level 'Zed'")

    def test_an_unknown_architecture_is_refused(self) -> None:
        with self.assertRaisesRegex(stubgen.SymbolFileError, "unknown architecture 'mips'"):
            stubgen.select([], 29, "mips")


class CommandLineTests(unittest.TestCase):
    def test_main_writes_both_outputs_with_lf_endings(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-stubgen-") as tmp:
            root = Path(tmp)
            (root / "libx.map.txt").write_bytes(SAMPLE.encode("utf-8"))
            code = stubgen.main(
                [
                    str(root / "libx.map.txt"),
                    "--api", "29", "--arch", "arm64", "--library", "libx.so",
                    "--out-c", str(root / "libx.c"),
                    "--out-version-script", str(root / "libx.map"),
                ]
            )
            self.assertEqual(code, 0)
            self.assertNotIn(b"\r\n", (root / "libx.c").read_bytes())
            self.assertTrue((root / "libx.map").read_bytes().startswith(b"LIBX {\n"))

    def test_main_reports_a_bad_file_and_fails(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-stubgen-") as tmp:
            root = Path(tmp)
            (root / "bad.map.txt").write_bytes(b"LIBX {\n")
            code = stubgen.main(
                [
                    str(root / "bad.map.txt"),
                    "--api", "29", "--arch", "arm64", "--library", "libx.so",
                    "--out-c", str(root / "x.c"), "--out-version-script", str(root / "x.map"),
                ]
            )
            self.assertEqual(code, 1)
            self.assertFalse((root / "x.c").exists())


def _llvm_tool(name: str) -> str | None:
    """An LLVM tool from NK_ANDROID_LLVM_BIN (the NDK's or ours), else PATH."""
    folder = os.environ.get("NK_ANDROID_LLVM_BIN")
    if folder:
        for candidate in (name, name + ".exe"):
            path = Path(folder) / candidate
            if path.is_file():
                return str(path)
        return None
    return shutil.which(name)


class LinkTests(unittest.TestCase):
    """Build the stub as an AArch64 Android shared library and read its dynamic symbols."""

    def test_the_stub_links_and_exports_the_versioned_symbols(self) -> None:
        clang, readelf = _llvm_tool("clang"), _llvm_tool("llvm-readelf")
        if clang is None or readelf is None:
            self.skipTest("no clang and llvm-readelf (set NK_ANDROID_LLVM_BIN to an LLVM bin folder)")
        versions = stubgen.select(stubgen.parse_symbol_file(SAMPLE), 29, "arm64")
        with tempfile.TemporaryDirectory(prefix="nk-stubgen-") as tmp:
            root = Path(tmp)
            (root / "libx.c").write_bytes(stubgen.render_stub_c(versions, "libx.so").encode())
            (root / "libx.map").write_bytes(stubgen.render_version_script(versions).encode())
            link = subprocess.run(
                [
                    clang, "--target=aarch64-linux-android29", "-shared", "-nostdlib",
                    "-fuse-ld=lld", "-Wl,-soname,libx.so",
                    f"-Wl,--version-script={root / 'libx.map'}",
                    "-o", str(root / "libx.so"), str(root / "libx.c"),
                ],
                capture_output=True, text=True, check=False,
            )
            if link.returncode != 0 and "unable to find library" in link.stderr + link.stdout:
                self.skipTest("this clang cannot link for Android: " + link.stderr.strip()[:200])
            self.assertEqual(link.returncode, 0, link.stderr)
            dyn = subprocess.run(
                [readelf, "--dyn-syms", "--wide", str(root / "libx.so")],
                capture_output=True, text=True, check=True,
            ).stdout
        self.assertIn("x_open@@LIBX", dyn)
        self.assertIn("x_q@@LIBX_Q", dyn)
        self.assertRegex(dyn, r"OBJECT\s+GLOBAL\s+DEFAULT\s+\S+\s+x_errno@@LIBX")
        self.assertRegex(dyn, r"FUNC\s+WEAK\s+DEFAULT\s+\S+\s+x_hook@@LIBX")
        self.assertRegex(dyn, r"(?m)FUNC\s+GLOBAL\s+DEFAULT\s+\S+\s+x_late$")  # unversioned below 30
        for hidden in ("x_platform", "x_private", "x_r", "x_backported"):
            self.assertNotIn(hidden, dyn)


if __name__ == "__main__":
    unittest.main()
