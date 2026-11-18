# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Machine-checkable parity test between GNU Make (Makefile) and CMake (CMakeLists.txt).

Compares as sets the player's core sources, its own src/player sources and its include
search paths; checks that the named core sources and platform drivers are in both, that both
build and run the named native test targets, and that both link the same libraries. Also
verifies that _CRT_SECURE_NO_WARNINGS stays MSVC compiler noise suppression, never a switch
that selects code.
"""

from __future__ import annotations

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from makefile_text import read_makefile  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent

# The player's own target and the libraries whose PUBLIC include directories it inherits.
CMAKE_PLAYER_TARGET = "nakagawa_player"
CMAKE_PLAYER_LIBRARIES = ("nakagawa_core", "nk_update_sig")
# CMake adds this folder only for the generated version resource script; the Makefile
# passes it to windres as --include-dir, not to the C compiler.
CMAKE_RESOURCE_INCLUDE = "assets/brand"
CMAKE_SOURCE_PREFIX = "${CMAKE_CURRENT_SOURCE_DIR}/"


def make_player_sources(makefile_text: str) -> set[str]:
    """The src/player files the Makefile's PLAYER_SRCS names literally."""
    match = re.search(r"^PLAYER_SRCS\s*:=\s*(.+)$", makefile_text, re.MULTILINE)
    if match is None:
        raise AssertionError("PLAYER_SRCS not found in the Makefile text")
    return {word for word in match.group(1).split() if word.startswith("src/player/")}


def cmake_player_sources(cmake_text: str) -> set[str]:
    """The src/player files CMake's set(PLAYER_SOURCES ...) names literally."""
    match = re.search(r"set\(PLAYER_SOURCES\s+(.*?)\)", cmake_text, re.DOTALL)
    if match is None:
        raise AssertionError("set(PLAYER_SOURCES ...) not found in CMakeLists.txt")
    return {word for word in match.group(1).split() if word.startswith("src/player/")}


def make_player_includes(makefile_text: str) -> set[str]:
    """The repository directories PLAYER_INCLUDES passes as -I flags."""
    match = re.search(r"^PLAYER_INCLUDES\s*:=\s*(.+)$", makefile_text, re.MULTILINE)
    if match is None:
        raise AssertionError("PLAYER_INCLUDES not found in the Makefile text")
    return {word[2:] for word in match.group(1).split() if word.startswith("-I") and "$" not in word}


def cmake_player_includes(cmake_text: str) -> set[str]:
    """The repository directories the CMake player compiles with: its own include
    directories and the PUBLIC ones of the libraries it links."""
    dirs: set[str] = set()
    for target in (CMAKE_PLAYER_TARGET, *CMAKE_PLAYER_LIBRARIES):
        blocks = re.findall(rf"target_include_directories\({target}\s+(PUBLIC|PRIVATE)\s+(.*?)\)",
                            cmake_text, re.DOTALL)
        if not blocks:
            raise AssertionError(f"target_include_directories({target} ...) not found in CMakeLists.txt")
        for scope, body in blocks:
            if target != CMAKE_PLAYER_TARGET and scope != "PUBLIC":
                continue
            for word in body.split():
                word = word.strip('"')
                if word.startswith(CMAKE_SOURCE_PREFIX):
                    dirs.add(word[len(CMAKE_SOURCE_PREFIX):])
    dirs.discard(CMAKE_RESOURCE_INCLUDE)
    return dirs


class BuildSystemParityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.makefile_text = read_makefile(ROOT)
        self.cmake_text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

    def test_core_sources_parity(self) -> None:
        """Verify identical core sources in Makefile and CMakeLists.txt."""
        expected_core = [
            "src/core/nk_shared_helpers.c",
            "src/core/nk_iso.c",
            "src/core/nk_library.c",
            "src/core/nk_launch.c",
            "src/core/nk_title_manifest.c",
            "src/core/generated/nk_title_catalog.c",
        ]
        for src in expected_core:
            self.assertIn(src, self.makefile_text, f"Missing {src} in Makefile")
            self.assertIn(src, self.cmake_text, f"Missing {src} in CMakeLists.txt")

    def test_platform_drivers_parity(self) -> None:
        """Verify Win32 and POSIX platform drivers present in both build systems."""
        expected_plat = [
            "src/core/nk_platform_win32.c",
            "src/core/nk_platform_posix.c",
        ]
        for src in expected_plat:
            self.assertIn(src, self.makefile_text, f"Missing platform driver {src} in Makefile")
            self.assertIn(src, self.cmake_text, f"Missing platform driver {src} in CMakeLists.txt")

    def test_player_sources_parity(self) -> None:
        """Make's PLAYER_SRCS and CMake's PLAYER_SOURCES name the same src/player sources, so a
        file added to one list only fails here rather than at the other system's link."""
        make_set = make_player_sources(self.makefile_text)
        self.assertEqual(make_set, cmake_player_sources(self.cmake_text),
                         "the player's src/player sources differ between make and CMake")
        expected_player = [
            "src/player/main.c",
            "src/player/player_host.c",
            "src/player/player_state.c",
            "src/player/input_settings.c",
            "src/player/iso_reader.c",
            "src/player/package_builder.c",
            "src/player/update_check.c",
            "src/player/discord_presence.c",
            "src/player/cover_art.c",
        ]
        self.assertLessEqual(set(expected_player), make_set)

    def test_player_source_comparison_sees_a_one_sided_file(self) -> None:
        """The comparison above can fail: a source in one list only makes the sets differ."""
        mutated = re.sub(r"^(PLAYER_SRCS\s*:=)", r"\1 src/player/only_in_make.c", self.makefile_text,
                         count=1, flags=re.MULTILINE)
        self.assertNotEqual(make_player_sources(mutated), cmake_player_sources(self.cmake_text))

    def test_player_core_sources_are_one_set(self) -> None:
        """The player's core library is one list in both systems: make's PLAYER_CORE_SOURCES and
        CMake's CORE_SOURCES name the same files. A file the runtime also builds (such as the
        texture pack reader, #1880) appears elsewhere in both texts, so only the lists themselves
        say whether the player links it."""
        make = re.search(r"^PLAYER_CORE_SOURCES\s*:=\s*(.+)$", self.makefile_text, re.MULTILINE)
        cmake = re.search(r"^set\(CORE_SOURCES\s+(.*?)\)", self.cmake_text, re.MULTILINE | re.DOTALL)
        if make is None:
            self.fail("PLAYER_CORE_SOURCES not found in the Makefile text")
        if cmake is None:
            self.fail("set(CORE_SOURCES ...) not found in CMakeLists.txt")
        make_set = set(make.group(1).split())
        cmake_set = set(cmake.group(1).split())
        self.assertEqual(make_set, cmake_set, "the player's core sources differ between make and CMake")
        self.assertIn("src/rt/texrepl.c", make_set, "the player does not link the texture pack reader")

    def test_include_paths_parity(self) -> None:
        """The player compiles with the same repository include directories in both systems:
        the -I flags of Make's PLAYER_INCLUDES, and CMake's player include directories plus the
        PUBLIC ones of the libraries it links. A source file path that merely contains a
        directory name does not count."""
        make_set = make_player_includes(self.makefile_text)
        self.assertEqual(make_set, cmake_player_includes(self.cmake_text),
                         "the player's include directories differ between make and CMake")
        self.assertLessEqual({"src/core", "src/core/generated", "src/player"}, make_set)

    def test_include_comparison_sees_a_dropped_flag(self) -> None:
        """The comparison above can fail: dropping -Isrc/core/generated makes the sets differ."""
        mutated = re.sub(r"^(PLAYER_INCLUDES\s*:=.*?) -Isrc/core/generated\b", r"\1", self.makefile_text,
                         count=1, flags=re.MULTILINE)
        self.assertNotEqual(mutated, self.makefile_text)
        self.assertNotEqual(make_player_includes(mutated), cmake_player_includes(self.cmake_text))

    def test_library_dependencies_parity(self) -> None:
        """Verify SDL3 and platform libraries are linked in both systems."""
        self.assertIn("SDL3", self.makefile_text)
        self.assertIn("SDL3", self.cmake_text)
        self.assertIn("shell32", self.makefile_text)
        self.assertIn("shell32", self.cmake_text)
        for library in ("ole32", "uuid"):
            self.assertIn(library, self.makefile_text)
            self.assertIn(library, self.cmake_text)

    def test_update_signature_sources_and_key_parity(self) -> None:
        """Both build systems link the update signature check into the player and its
        test, and read the update-signing key from the same one committed file."""
        for src in (
            "src/core/nk_sig.c",
            "third_party/monocypher/monocypher.c",
            "third_party/monocypher/monocypher-ed25519.c",
        ):
            self.assertIn(src, self.makefile_text, f"Missing {src} in Makefile")
            self.assertIn(src, self.cmake_text, f"Missing {src} in CMakeLists.txt")
        for text in (self.makefile_text, self.cmake_text):
            self.assertIn("assets/update_signing_key.pub", text)
            self.assertIn("NK_UPDATE_SIGNING_KEY", text)
        self.assertIn("test_nk_sig", self.cmake_text)
        # The Makefile passes the key to the player, the regression player and the test.
        self.assertEqual(self.makefile_text.count("$(NK_UPDATE_KEY_DEFS)"), 3)
        self.assertIn("nk_update_sig", self.cmake_text.split("add_executable(nakagawa_player", 1)[1])

    def test_update_signing_key_reader(self) -> None:
        """The Makefile's key reader takes a minisign public key file or its bare line,
        and prints nothing (which stops the build) for anything else."""
        match = re.search(
            r'^NK_UPDATE_KEY_LINE := .*?"\$\(PYTHON\)" -c "(.*?)" "\$\(NK_UPDATE_KEY_FILE\)"',
            self.makefile_text, re.MULTILINE,
        )
        self.assertIsNotNone(match, "the Makefile reads the key with one Python command")
        assert match is not None
        code = match.group(1)
        python = shutil.which("python") or shutil.which("python3")
        if not python:
            self.skipTest("Python is required for the behavioural check")
        # The TEST-ONLY minisign public key of tests/native/test_nk_sig.c; it signs
        # nothing outside the tests.
        line = "RWSeDEzFopmm0LdwefFMjtRkQtZckksdhfycG9vMHzmHiu5pl16/wA81"
        comment = "untrusted comment: minisign public key D0A699A2C54C0C9E"
        cases = {
            f"{comment}\r\n{line}\r\n": line,
            f"{comment}\n{line}": line,
            f"{line}\n": line,
            f"{comment}\n": "",
            f"{comment}\n{line[:-1]}\n": "",
            f"{comment}\n{line}\n{line}\n": "",
            f"another comment\n{line}\n": "",
            f"{comment}\nRU{line[2:]}\n": "",
            f"{comment}\n{line[:-1]}!\n": "",
        }
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "update_signing_key.pub"
            for content, expected in cases.items():
                with self.subTest(content=content):
                    path.write_bytes(content.encode("ascii"))
                    proc = subprocess.run([python, "-c", code, str(path)],
                                          capture_output=True, text=True, timeout=60)
                    self.assertEqual(proc.stdout.strip(), expected, proc.stderr)
            # A malformed committed key stops the build at parse time, even for help.
            path.write_bytes(f"{comment}\nnot a key\n".encode("ascii"))
            make = shutil.which("mingw32-make") or shutil.which("make")
            if not make:
                self.skipTest("GNU Make is required for the build-stop check")
            proc = subprocess.run(
                [make, "--no-print-directory", "help", f"NK_UPDATE_KEY_FILE={path.as_posix()}",
                 f"BUILD_DIR={(Path(tmp) / 'help-build').as_posix()}"],
                cwd=ROOT, capture_output=True, text=True, timeout=120,
            )
            self.assertNotEqual(proc.returncode, 0, proc.stdout)
            self.assertIn("is not a minisign public key", proc.stdout + proc.stderr)

    def test_test_targets_parity(self) -> None:
        """Verify all native core test targets exist in both build systems."""
        test_targets = [
            "test_core_catalog",
            "test_parsers_hostile",
            "test_manifest_parser",
            "test_win32_process",
            "test_pgf_public",
        ]
        for target in test_targets:
            # Built and run by each system, not merely named somewhere in its text.
            self.assertIn(f"-o $(BUILD_ROOT)/{target}$(EXE_EXT)", self.makefile_text,
                          f"the Makefile builds no {target}")
            self.assertIn(f"$(BUILD_ROOT_RUN)/{target}$(EXE_EXT)", self.makefile_text,
                          f"the Makefile runs no {target}")
            self.assertRegex(self.cmake_text, rf"add_executable\({target}\b",
                             f"CMakeLists.txt builds no {target}")
            self.assertRegex(self.cmake_text, rf"add_test\(NAME \S+ COMMAND {target}\b",
                             f"CMakeLists.txt runs no {target}")

    def test_public_pgf_backend_and_test_sources(self) -> None:
        """Public Make builds and both test systems compile the same reader test."""
        self.assertIn("PGF_BACKEND_SRC := src/rt/pgf_public.c", self.makefile_text)
        self.assertIn("src/rt/pgf_public.c", self.makefile_text)
        self.assertIn("src/rt/pgf_public.c", self.cmake_text)
        self.assertIn("tests/native/test_pgf_public.c", self.makefile_text)
        self.assertIn("tests/native/test_pgf_public.c", self.cmake_text)

    def test_compiler_noise_suppression_classification(self) -> None:
        """_CRT_SECURE_NO_WARNINGS is MSVC compiler noise suppression, not portable architecture:
        CMake defines it only for MSVC, and no source tests it to choose code (each use is the
        #ifndef/#define guard that silences the CRT deprecation warnings)."""
        # Architectural rule: Nakagawa uses portable abstractions (nk_platform.h),
        # not Windows-specific CRT suppressions disguised as portability.
        msvc_block = re.search(r"^if\(MSVC\)\n(.*?)^(?:else\(\)|endif\(\))", self.cmake_text,
                               re.MULTILINE | re.DOTALL)
        self.assertIsNotNone(msvc_block, "CMakeLists.txt has no top-level if(MSVC) block")
        assert msvc_block is not None
        self.assertIn("add_compile_definitions(_CRT_SECURE_NO_WARNINGS)", msvc_block.group(1))
        self.assertEqual(self.cmake_text.count("_CRT_SECURE_NO_WARNINGS"), 1,
                         "CMake must define _CRT_SECURE_NO_WARNINGS for MSVC only")
        selectors: list[str] = []
        for path in sorted((ROOT / "src").rglob("*.[ch]")):
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            for number, line in enumerate(lines):
                if "_CRT_SECURE_NO_WARNINGS" not in line:
                    continue
                stripped = line.strip()
                if stripped.startswith("#define _CRT_SECURE_NO_WARNINGS"):
                    continue
                following = lines[number + 1].strip() if number + 1 < len(lines) else ""
                if stripped == "#ifndef _CRT_SECURE_NO_WARNINGS" and \
                        following.startswith("#define _CRT_SECURE_NO_WARNINGS"):
                    continue
                selectors.append(f"{path.relative_to(ROOT).as_posix()}:{number + 1}: {stripped}")
        self.assertEqual(selectors, [], "_CRT_SECURE_NO_WARNINGS must never select code")

    def test_verification_targets_present(self) -> None:
        """Verify make test and make check exist in Makefile (O-05)."""
        self.assertTrue(
            bool(re.search(r"^test:", self.makefile_text, re.MULTILINE)),
            "Missing test target in Makefile",
        )
        self.assertTrue(
            bool(re.search(r"^check:", self.makefile_text, re.MULTILINE)),
            "Missing check target in Makefile",
        )

    def test_public_target_catalog_covers_phony_targets(self) -> None:
        """`make help` must expose every public target exactly once (O-16)."""
        catalog = re.search(
            r"(?ms)^PUBLIC_TARGETS := \\\n(.*?)(?=^INTERNAL_TARGETS :=)",
            self.makefile_text,
        )
        self.assertIsNotNone(catalog, "Makefile must define the public target catalog")
        targets = re.findall(
            r"(?m)^\s*([A-Za-z0-9_.%/+?-]+)\s*\\?$",
            catalog.group(1),
        )
        self.assertEqual(len(targets), len(set(targets)), "public target catalog contains a duplicate")
        self.assertIn("help", targets)
        self.assertIn("psp-oracle", targets)
        self.assertIn("psp-oracle-nakagawa", targets)
        self.assertIn(".PHONY: $(PUBLIC_TARGETS) $(INTERNAL_TARGETS)", self.makefile_text)

        defined_targets = set(re.findall(
            r"(?m)^([A-Za-z0-9_.%/+?-]+):",
            self.makefile_text,
        ))
        defined_targets.difference_update({".PHONY", ".SECONDARY"})
        internal = set(
            self.makefile_text.split("INTERNAL_TARGETS :=", 1)[1]
            .splitlines()[0].split()
        )
        self.assertEqual(
            defined_targets,
            set(targets) | internal,
            "public and internal target catalogs must cover every named target",
        )

        descriptions = dict(re.findall(
            r"(?m)^HELP_DESCRIPTION_([A-Za-z0-9_.%/+?-]+)\s*:=\s*(.+)$",
            self.makefile_text,
        ))
        self.assertEqual(set(targets), set(descriptions), "every public target needs one description")
        self.assertTrue(all(description.strip() for description in descriptions.values()))

    def test_provenance_refresh_wrapper_keeps_authority_explicit(self) -> None:
        self.assertRegex(self.makefile_text, r"(?m)^provenance-refresh:")
        self.assertIn("tools/provenance_refresh.py", self.makefile_text)
        self.assertIn("NK_TRUSTED_LEDGER", self.makefile_text)
        self.assertIn("PROVENANCE_REFRESH_APPLY_POLICY", self.makefile_text)

    def test_audited_public_controls_stay_diff_visible(self) -> None:
        """The provenance controls are audited bytes, so PR diffs must show them.

        GitHub collapses diffs of paths marked ``linguist-generated``; marking the
        ledger or export that way would hide exactly the bytes a reviewer must see.
        """
        attributes = (ROOT / ".gitattributes").read_text(encoding="utf-8")
        for path in (
            "/PUBLIC_EXPORT.json",
            "/assets/public_provenance_ledger.json",
            "/assets/public_source_profile.json",
        ):
            with self.subTest(path=path):
                self.assertNotRegex(
                    attributes,
                    rf"(?m)^{re.escape(path)}\s+.*linguist-generated",
                )

    def test_help_skips_parse_time_side_effects(self) -> None:
        """`make help` must not create or invalidate profile stamps or build dirs."""
        self.assertRegex(self.makefile_text, r"(?m)^NK_INFO_ONLY_GOALS := help$")
        self.assertIn(
            "ifeq ($(strip $(filter clean distclean,$(MAKECMDGOALS))$(NK_INFO_ONLY)),)",
            self.makefile_text,
        )
        make = shutil.which("mingw32-make") or shutil.which("make")
        python = shutil.which("python") or shutil.which("python3")
        if not make or not python:
            self.skipTest("GNU Make and Python are required for the behavioural check")
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp) / "help-build"
            proc = subprocess.run(
                [make, "--no-print-directory", "help", f"BUILD_DIR={build_dir.as_posix()}"],
                cwd=ROOT, capture_output=True, text=True, timeout=120,
            )
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertIn("help - list every public Make target", proc.stdout)
            self.assertFalse(
                build_dir.exists(),
                "make help created build state: "
                + ", ".join(sorted(p.name for p in build_dir.rglob("*"))),
            )

if __name__ == "__main__":
    unittest.main()
