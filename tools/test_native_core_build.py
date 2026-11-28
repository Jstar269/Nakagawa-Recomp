# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Coverage contract for ``native-core-tests`` (#735 items 3 and 7).

The target shares compiled core objects and expands its test-binary recipes as macros. These
checks read the target's dry run, which lists every command Make would execute, so they hold
without building anything: every test binary the dry run shows being linked is run after its
link, every binary run on its own line is linked, and the core sources compile once per flag
set. A recipe that names its binary through a shell variable is not seen.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from makefile_text import read_makefile  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]

# Every dry run names a scratch BUILD_ROOT: parsing the Makefile writes beneath BUILD_ROOT (the
# SDL3 discovery fragment, a named title's stamps), so the checkout's build/ is never touched.
# Commands are compared after that root is folded back to the literal `build/` spelling.
_SCRATCH_ROOT = tempfile.TemporaryDirectory(prefix="nk-native-core-dry-")
unittest.addModuleCleanup(_SCRATCH_ROOT.cleanup)
_SCRATCH = Path(_SCRATCH_ROOT.name).as_posix()


# Linked to prove a link, never run: the Ace UI selftest's first build shows that the UI pass,
# the keyboard model and nk_ui link without the rest of the renderer (mk/native_ui_tests.mk).
_LINK_ONLY = frozenset({"ace_ui_selftest_s1"})


def _make_program() -> str | None:
    for name in ("mingw32-make", "make"):
        found = shutil.which(name)
        if found:
            return found
    return None


def _commands(lines: list[str]) -> list[str]:
    """Join backslash-continued lines so each entry is one shell command."""
    commands: list[str] = []
    pending = ""
    for line in lines:
        stripped = line.strip()
        if stripped.endswith("\\"):
            pending += stripped[:-1] + " "
            continue
        commands.append((pending + stripped).strip())
        pending = ""
    if pending.strip():
        commands.append(pending.strip())
    return commands


def _dry_run(*goals: str) -> list[str]:
    make = _make_program()
    if make is None:
        raise unittest.SkipTest("no make program on PATH")
    result = subprocess.run(
        [make, "--no-print-directory", "-n", "CC=gcc", f"BUILD_ROOT={_SCRATCH}", *goals],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(f"make -n {' '.join(goals)} failed:\n{result.stderr}")
    return [_fold_root(c) for c in _commands(result.stdout.splitlines())]


def _fold_root(command: str) -> str:
    """Spell the scratch BUILD_ROOT as `build` so expectations read like the default layout."""
    return command.replace(_SCRATCH + "/", "build/").replace("./build/", "build/")


def _test_runs(commands: list[str]) -> list[str]:
    return [
        # A run may carry a shell tail (the SDL leg maps exit 77 to SKIP); keep the binary part.
        command.split(";")[0].replace(".exe", "")
        for command in commands
        if command.startswith("build/") and " -o " not in command
    ]


def _link_outputs(commands: list[str]) -> set[str]:
    outputs = set()
    for command in commands:
        tokens = command.split()
        if tokens and tokens[0] == "gcc" and "-o" in tokens:
            target = tokens[tokens.index("-o") + 1]
            outputs.add(os.path.basename(target).replace(".exe", ""))
    return outputs


class NativeCoreTestsContractTests(unittest.TestCase):
    commands: list[str]

    @classmethod
    def setUpClass(cls) -> None:
        cls.commands = _dry_run("native-core-tests")

    def test_every_test_binary_the_target_links_is_run(self) -> None:
        """A selftest that is built and never run reports nothing. A run is the binary in
        command position: first on a line, or after a shell operator. An argument naming
        it (``--exe build/...``) is not one, and neither is a longer name that starts
        with it."""
        checked = 0
        for index, command in enumerate(self.commands):
            for link in re.finditer(r'\bgcc\b[^;&|]*?\s-o\s+"?(build/[^\s";&|]+)', command):
                target = link.group(1)
                name = os.path.basename(target).replace(".exe", "")
                if target.endswith(".o") or not (name.startswith("test_") or "_selftest" in name):
                    continue  # an object file, or a helper program a test starts itself
                if name in _LINK_ONLY:
                    continue
                checked += 1
                run = re.compile(r'(?:^|[;&|(]\s*)"?' + re.escape(target) + r'(?=$|[\s";&|)])')
                candidates = [command[link.end():], *self.commands[index + 1:]]
                with self.subTest(binary=os.path.basename(target)):
                    self.assertTrue(
                        any(run.search(later) for later in candidates),
                        f"{target} is linked by native-core-tests and nothing after the link runs it",
                    )
        self.assertGreater(checked, 50, "the dry run names too few test-binary links to check")

    def test_nk_ui_core_is_compiled_and_run_by_native_core_tests(self) -> None:
        compile_commands = [command for command in self.commands
                            if command.startswith("gcc") and "-o build/test_nk_ui_core" in command]
        self.assertEqual(len(compile_commands), 1, "nk_ui core test compile command")
        self.assertIn("-Isrc/ui", compile_commands[0])
        self.assertIn("-Ithird_party/clay", compile_commands[0])
        self.assertIn("tests/native/test_nk_ui_core.c", compile_commands[0])
        self.assertIn("src/ui/nk_ui_clay.c", compile_commands[0])
        self.assertIn("build/test_nk_ui_core", _test_runs(self.commands))

    def test_a_test_that_can_exit_77_is_run_so_that_77_is_a_skip(self) -> None:
        """test_player_state names a case a host cannot run NOT_RUN (a checkout with no working
        git) and exits 77; a bare run would stop native-core-tests there and no later test
        would run."""
        for binary in ("build/test_player_state",):
            run = [c for c in self.commands if c.replace(".exe", "").startswith(binary + ";")]
            self.assertEqual(len(run), 1, binary)
            self.assertIn("77", run[0], f"{binary}: exit 77 must be reported as SKIP")

    def test_nk_ui_focus_selftest_has_an_sdl_free_build_and_an_sdl_leg(self) -> None:
        plain = [c for c in self.commands if c.startswith("gcc") and " -o build/test_nk_ui_focus" in c
                 and "_sdl" not in c.split(" -o ", 1)[1]]
        sdl = [c for c in self.commands if c.startswith("gcc") and " -o build/test_nk_ui_focus_sdl" in c]
        self.assertEqual(len(plain), 1, "SDL-free focus test compile")
        self.assertEqual(len(sdl), 1, "SDL translation leg compile")
        self.assertNotIn("NK_UI_WITH_SDL3", plain[0])
        for command in (plain[0], sdl[0]):
            self.assertIn("tests/native/test_nk_ui_focus.c", command)
            self.assertIn("src/ui/nk_ui_sdl_input.c", command)
            self.assertIn("-DSR_SELFTEST_BUILD_ROOT=", command)
        run = [c for c in self.commands if c.startswith("build/test_nk_ui_focus_sdl")]
        self.assertEqual(len(run), 1)
        self.assertIn("--sdl-input", run[0])
        self.assertIn("77", run[0], "exit 77 must be reported as SKIP, not swallowed or failed")

    def test_core_sources_compile_once_per_flag_set(self) -> None:
        # Two shared object sets: the core flags, and the core flags plus -Isrc/player.
        compiles = [c for c in self.commands if " -c src/core/nk_font.c " in c]
        self.assertEqual(len(compiles), 2, "\n".join(compiles))

    def test_binaries_that_share_the_core_objects_link_them(self) -> None:
        linked = [c for c in self.commands
                  if c.startswith("gcc") and "build/native-core/" in c and " -o build/test_" in c]
        names = {os.path.basename(c.split(" -o ", 1)[1].split()[0]).replace(".exe", "")
                 for c in linked}
        for binary in ("test_core_catalog", "test_parsers_hostile", "test_manifest_parser",
                       "test_launch_resolution", "test_input_profile", "test_input_settings",
                       "test_package_builder", "test_xb_parser", "test_fuzz_parsers"):
            with self.subTest(binary=binary):
                self.assertIn(binary, names)

    def test_every_binary_the_target_runs_is_linked_by_it(self) -> None:
        outputs = _link_outputs(self.commands)
        runs = _test_runs(self.commands)
        self.assertTrue(runs, "native-core-tests runs no test binary")
        for run in runs:
            binary = os.path.basename(run.split()[0])
            with self.subTest(binary=binary):
                self.assertIn(binary, outputs)

    def test_target_does_not_recurse_into_make(self) -> None:
        recursive = [c for c in self.commands if "--no-print-directory" in c]
        self.assertEqual(recursive, [], "every sub-make re-parses the whole Makefile")


class StandaloneTestBinTargetTests(unittest.TestCase):
    """The *-test-bin targets stay buildable on their own; other tests invoke them directly."""

    def test_input_settings_and_package_builder_build_from_the_shared_objects(self) -> None:
        for goal, binary in (("input-settings-test-bin", "test_input_settings"),
                             ("package-builder-test-bin", "test_package_builder")):
            with self.subTest(goal=goal):
                commands = _dry_run(goal)
                self.assertTrue(any("build/native-core/player/nk_font.o" in c for c in commands))
                self.assertIn(binary, _link_outputs(commands))

    def test_only_the_package_builder_test_shortens_the_python_probe_bound(self) -> None:
        # The hung-wrapper case waits out the probe bound once per probe. The test
        # binary sets a shorter bound; nothing else does, so the player keeps 10 s.
        commands = _dry_run("package-builder-test-bin")
        self.assertTrue(any("-DPYTHON_VERSION_PROBE_TIMEOUT_MS=3000" in c and "src/player/package_builder.c" in c
                            for c in commands))
        makefile = read_makefile(ROOT)
        self.assertEqual(makefile.count("PYTHON_VERSION_PROBE_TIMEOUT_MS"), 1)
        source = (ROOT / "src" / "player" / "package_builder.c").read_text(encoding="utf-8")
        self.assertIn("#ifndef PYTHON_VERSION_PROBE_TIMEOUT_MS\n#define PYTHON_VERSION_PROBE_TIMEOUT_MS 10000\n#endif",
                      source)

    def test_player_state_keeps_its_seam_variant(self) -> None:
        commands = _dry_run("player-state-test-bin")
        self.assertTrue(any("-DNK_TITLE_MANIFEST_TEST_SEAMS" in c and "src/core/nk_font.c" in c
                            for c in commands))
        # The saves fault seams (#1883) compile into nk_zip.c and player_state.c here only.
        self.assertTrue(any("-DNK_SAVES_TEST_SEAMS" in c and "src/core/nk_zip.c" in c
                            and "src/player/player_state.c" in c for c in commands))
        # The monitor's live-read seam (#306) compiles into player_state.c here only.
        self.assertTrue(any("-DNK_PLAYER_STATE_TEST_SEAMS" in c
                            and "src/player/player_state.c" in c for c in commands))


class NkUiInventoryTests(unittest.TestCase):
    """mk/nk_ui.mk and CMakeLists.txt keep one nk_ui source inventory, and every path in it is
    a file the build can compile: a list naming a missing file breaks the first rule wired to it."""

    @staticmethod
    def _make_paths() -> dict[str, list[str]]:
        text = (ROOT / "mk" / "nk_ui.mk").read_text(encoding="utf-8").replace("\\\n", " ")
        return {match.group(1): [w for w in match.group(2).split() if w.endswith(".c")]
                for match in re.finditer(r"^(NK_UI_\w+)\s*:?=(.*)$", text, re.M)}

    @staticmethod
    def _cmake_paths() -> dict[str, list[str]]:
        text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        return {match.group(1): [w for w in match.group(2).split() if w.endswith(".c")]
                for match in re.finditer(r"^set\((NK_UI_\w+)\s([^)]*)\)", text, re.M)}

    def test_every_listed_nk_ui_source_exists(self) -> None:
        for origin, lists in (("mk/nk_ui.mk", self._make_paths()),
                              ("CMakeLists.txt", self._cmake_paths())):
            self.assertTrue(lists, origin)
            for name, paths in lists.items():
                for path in paths:
                    with self.subTest(origin=origin, list=name, path=path):
                        self.assertTrue((ROOT / path).is_file(), f"{origin} {name} names {path}")


if __name__ == "__main__":
    unittest.main()
