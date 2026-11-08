# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Static checks of the Android build (android/, #360) against the rest of the repository.

The Android app is built with Gradle and the NDK, which hosted CI does not run, so the contracts
it shares with the desktop build are checked here, without an SDK:

- the runtime inventory nk_add_title_runtime() compiles (CMakeLists.txt) is the Makefile's
  runtime link (RT_SRCS with the public backends and the POSIX platform, RT_GE_O, the ATRAC3+
  import and bridge, the FFmpeg H.264 subset), so a source added to one is not missing from the
  other;
- SDL3 and SDL3_ttf are pinned to the commits hosted CI builds;
- the app asks for no permission at all (the product is offline, AGENTS.md section 5);
- the names the native and Java halves use to find each other agree;
- machine-local Android state stays out of the repository.
"""

from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import android_test_runner as runner  # noqa: E402
from makefile_text import read_makefile  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
ANDROID = ROOT / "android"


def _make_assignments(*texts: str) -> dict[str, list[str]]:
    """Every `NAME :=|?=|= value` in the texts, continuation lines joined, in order."""
    out: dict[str, list[str]] = {}
    for text in texts:
        text = text.replace("\\\n", " ")
        for line in text.splitlines():
            match = re.match(r"^([A-Za-z0-9_]+)\s*(?::=|\?=|=)\s*(.*)$", line)
            if match:
                out.setdefault(match.group(1), []).append(match.group(2))
    return out


def _expand(value: str, assignments: dict[str, list[str]], choose: dict[str, str]) -> list[str]:
    words: list[str] = []
    for word in value.split():
        match = re.fullmatch(r"\$\(([A-Za-z0-9_]+)\)", word)
        if not match:
            words.append(word)
            continue
        name = match.group(1)
        candidates = assignments.get(name, [])
        chosen = choose.get(name)
        if chosen is not None:
            candidates = [c for c in candidates if chosen in c] or candidates
        words += _expand(candidates[0], assignments, choose) if candidates else []
    return words


def _cmake_list(text: str, name: str) -> list[str]:
    match = re.search(r"^set\(" + re.escape(name) + r"\b(.*?)\)", text, re.S | re.M)
    if not match:
        raise AssertionError(f"CMakeLists.txt has no set({name} ...)")
    return [w for w in match.group(1).split() if not w.startswith("#")]


class AndroidRuntimeInventoryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.make = _make_assignments(read_makefile(ROOT))
        # The Android runtime is a POSIX build: the POSIX platform backend, and the H.264 subset
        # (H264_FFMPEG=1 off Windows) rather than the empty list.
        self.choose = {"PLAYER_PLATFORM_SRC": "posix", "H264_SUB_SRCS": "nk_h264_api.c"}

    def test_runtime_sources_match_the_makefile_link(self) -> None:
        make_rt = _expand(self.make["RT_SRCS"][0], self.make, self.choose)
        self.assertIn("src/core/nk_platform_posix.c", make_rt)
        ui = (_cmake_list(self.cmake, "NK_UI_CORE_SOURCES") +
              _cmake_list(self.cmake, "NK_UI_EDGE_SOURCES") +
              ["src/ui/backend/nk_ui_sdlr.c", "src/ui/backend/nk_ui_vk.c"] +
              _cmake_list(self.cmake, "NK_UI_OVERLAY_C9C_SOURCES") +
              ["src/rt/overlay/overlay_host.c"])
        cmake_rt = _cmake_list(self.cmake, "NK_RUNTIME_SOURCES") + ui
        self.assertEqual(sorted(cmake_rt), sorted(make_rt))
        self.assertEqual(len(cmake_rt), len(set(cmake_rt)), "a runtime source is listed twice")

    def test_ge_atrac_and_h264_sources_match(self) -> None:
        self.assertIn("$(RT_GE_O): src/rt/ge.c", read_makefile(ROOT))
        self.assertEqual(_cmake_list(self.cmake, "NK_RUNTIME_GE_SOURCE"), ["src/rt/ge.c"])
        self.assertEqual(_cmake_list(self.cmake, "NK_RUNTIME_ATRAC3P_SOURCES"),
                         _expand(self.make["ATRAC3P_SRCS"][0], self.make, self.choose))
        self.assertEqual(_cmake_list(self.cmake, "NK_RUNTIME_ATRAC3P_BRIDGE_SOURCE"),
                         ["src/rt/atrac3p_bridge.c"])
        self.assertEqual(_cmake_list(self.cmake, "NK_RUNTIME_H264_SUB_SOURCES"),
                         _expand("$(H264_SUB_SRCS)", self.make, self.choose))

    def test_every_listed_source_exists(self) -> None:
        for name in ("NK_RUNTIME_SOURCES", "NK_RUNTIME_ATRAC3P_SOURCES",
                     "NK_RUNTIME_H264_SUB_SOURCES"):
            for source in _cmake_list(self.cmake, name):
                self.assertTrue((ROOT / source).is_file(), f"{name}: {source} does not exist")


class AndroidAppContractTests(unittest.TestCase):
    def test_sdl_pins_match_hosted_ci(self) -> None:
        ci = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        deps = (ANDROID / "app" / "deps.cmake").read_text(encoding="utf-8")
        for ci_name, cmake_name in (("SDL3_COMMIT", "NK_SDL3_COMMIT"),
                                    ("SDL3_TTF_COMMIT", "NK_SDL3_TTF_COMMIT")):
            ci_pin = re.search(ci_name + r"=([0-9a-f]{40})", ci)
            android_pin = re.search(r"set\(" + cmake_name + r" ([0-9a-f]{40})\)", deps)
            self.assertIsNotNone(ci_pin, ci_name)
            self.assertIsNotNone(android_pin, cmake_name)
            assert android_pin is not None
            self.assertEqual(android_pin.group(1), ci_pin.group(1), cmake_name)

    def test_manifest_requests_no_permissions(self) -> None:
        for manifest in ANDROID.glob("app/src/*/AndroidManifest.xml"):
            text = manifest.read_text(encoding="utf-8")
            self.assertNotIn("uses-permission", text, f"{manifest} asks for a permission")

    def test_title_activity_runs_in_its_own_process(self) -> None:
        text = (ANDROID / "app" / "src" / "main" / "AndroidManifest.xml").read_text(encoding="utf-8")
        block = re.search(r'<activity\s+android:name="\.TitleActivity"(.*?)/>', text, re.S)
        self.assertIsNotNone(block)
        assert block is not None
        self.assertIn('android:process=":title"', block.group(1))
        assert block is not None
        self.assertIn('android:exported="false"', block.group(1))
        # One runtime per process: a second start must not run main() again in it.
        assert block is not None
        self.assertIn('android:launchMode="singleTask"', block.group(1))

    def test_native_and_java_names_agree(self) -> None:
        java = ANDROID / "app" / "src" / "main" / "java" / "io" / "github" / "jstar269" / "nakagawa"
        launcher = (java / "LauncherActivity.java").read_text(encoding="utf-8")
        title = (java / "TitleActivity.java").read_text(encoding="utf-8")
        posix = (ROOT / "src" / "core" / "nk_platform_posix.c").read_text(encoding="utf-8")
        shim = (ANDROID / "app" / "src" / "main" / "cpp" / "nk_android_title.c").read_text(encoding="utf-8")
        self.assertIn('FindClass(env, "io/github/jstar269/nakagawa/LauncherActivity")', posix)
        self.assertIn('"startTitle"', posix)
        self.assertRegex(launcher, r"public static boolean startTitle\(String library, String\[\] args, "
                                   r"String\[\] env, String cwd,\s+String logPath\)")
        # (String, String[], String[], String, String) -> boolean
        self.assertIn('"(Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;"\n'
                      '        "Ljava/lang/String;)Z"', posix)
        self.assertIn('return "nk_android_title_main";', title)
        self.assertIn("int nk_android_title_main(int argc, char **argv)", shim)

    def test_data_folder_has_the_name_every_host_uses(self) -> None:
        """The user data folder is called Nakagawa on every host; on Android it is in the
        app-private files folder and reaches the native layer as NK_ANDROID_DATA_DIR."""
        java = ANDROID / "app" / "src" / "main" / "java" / "io" / "github" / "jstar269" / "nakagawa"
        host = (java / "NakagawaHost.java").read_text(encoding="utf-8")
        self.assertIn('return new File(context.getFilesDir(), "Nakagawa");', host)
        self.assertIn('setenv("NK_ANDROID_DATA_DIR", data.getAbsolutePath());', host)

    def test_local_android_state_is_ignored(self) -> None:
        ignore = (ROOT / ".gitignore").read_text(encoding="utf-8")
        for entry in ("/android/local.properties", "/android/.gradle/", "/android/app/.cxx/",
                      "/android/.idea/", "/android/app/build/"):
            self.assertIn(entry, ignore.splitlines())


# The device scratch folder, kept out of literals: the publication audit reads a literal
# temporary-folder path as a local path.
SCRATCH = runner.DEVICE_SCRATCH_ROOT
DEVICE = f"{SCRATCH}/nk-test"


class AndroidDeviceTestRunnerTests(unittest.TestCase):
    """tools/android_test_runner.py, the CTest emulator that runs the native tests on a device."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.source = self.tmp / "src"
        self.binary = self.source / "build" / "android-arm64-ctest"
        self.binary.mkdir(parents=True)
        self.device = runner.checked_device_dir(DEVICE)
        self.paths = runner.PathMap(self.device, str(self.source), str(self.binary))

    def test_device_dir_must_be_under_the_shell_scratch_area(self) -> None:
        self.assertEqual(str(self.device), DEVICE)
        for bad in (str(SCRATCH), f"{SCRATCH}/../app", "/sdcard/nk", f"{SCRATCH}/nk"[1:],
                    # What Git Bash makes of a device path without MSYS_NO_PATHCONV=1.
                    f"C:/Program Files/Git{SCRATCH}/nk"):
            with self.subTest(bad=bad), self.assertRaises(runner.RunnerError):
                runner.checked_device_dir(bad)

    def test_trees_map_to_their_device_copies_binary_tree_first(self) -> None:
        self.assertEqual(str(self.paths.map(str(self.binary))), f"{DEVICE}/work")
        self.assertEqual(str(self.paths.map(str(self.binary / "build" / "x.bin"))),
                         f"{DEVICE}/work/build/x.bin")
        self.assertEqual(str(self.paths.map(str(self.source / "assets" / "fonts"))),
                         f"{DEVICE}/src/assets/fonts")
        self.assertIsNone(self.paths.map(str(self.tmp / "elsewhere")))
        self.assertEqual(self.paths.map_value("--check"), "--check")

    def test_remote_script_maps_cwd_args_and_test_environment(self) -> None:
        env = {"NK_TEST_SCRATCH_DIR": str(self.binary / "build"), "SR_GESTAT": "1",
               "NK_ADB": "C:/sdk/adb.exe", "PATH": "C:/Windows"}
        script = runner.remote_run_script(self.device, self.paths, str(self.source), "test_x",
                                          ["--input", str(self.source / "fixtures" / "a b")], env)
        self.assertEqual(
            script,
            f"mkdir -p {DEVICE}/src && cd {DEVICE}/src && "
            f"env NK_TEST_SCRATCH_DIR={DEVICE}/work/build SR_GESTAT=1 "
            f"{DEVICE}/bin/test_x --input '{DEVICE}/src/fixtures/a b'")

    def test_a_working_directory_outside_both_trees_is_refused(self) -> None:
        with self.assertRaises(runner.RunnerError):
            runner.remote_run_script(self.device, self.paths, str(self.tmp), "test_x", [], {})

    def test_run_pushes_runs_and_returns_the_device_exit_status(self) -> None:
        exe = self.binary / "test_x"
        exe.write_bytes(b"\x7fELF")
        calls: list[list[str]] = []

        def fake_run(cmd, check=False, **kwargs):  # noqa: ANN001
            calls.append(list(cmd))
            rc = 77 if cmd[1] == "shell" and "cd " in cmd[2] else 0
            return subprocess.CompletedProcess(cmd, rc, stdout="", stderr="")

        cwd = os.getcwd()
        os.chdir(self.binary)
        try:
            with mock.patch.dict(os.environ, {"NK_ADB": "adb-test"}), \
                    mock.patch.object(runner.subprocess, "run", fake_run):
                rc = runner.main(["run", "--device-dir", str(self.device), "--source-root",
                                  str(self.source), "--binary-root", str(self.binary), "--",
                                  str(exe), "--check"])
        finally:
            os.chdir(cwd)
        self.assertEqual(rc, 77)  # a device SKIP stays a SKIP; nothing is turned into a pass
        self.assertEqual(calls[0], ["adb-test", "push", str(exe), f"{DEVICE}/bin/test_x"])
        self.assertEqual(calls[1], ["adb-test", "shell", f"chmod 755 {DEVICE}/bin/test_x"])
        self.assertTrue(calls[2][2].endswith(f"{DEVICE}/bin/test_x --check"))
        self.assertIn(f"cd {DEVICE}/work &&", calls[2][2])

    def test_cmake_wires_the_runner_as_the_cross_compiling_emulator(self) -> None:
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('"${CMAKE_CURRENT_SOURCE_DIR}/tools/android_test_runner.py" run', cmake)
        self.assertIn('set(NK_TEST_BUILD_ROOT "${NK_ANDROID_TEST_DIR}/work/build")', cmake)
        self.assertIn("FIXTURES_SETUP nk_android_device", cmake)
        self.assertIn("FIXTURES_REQUIRED nk_android_device", cmake)
        # Compiled-in scratch paths follow the device copy instead of naming the host tree.
        self.assertNotIn('SR_SELFTEST_BUILD_ROOT="${CMAKE_CURRENT_BINARY_DIR}/build"', cmake)
        # Linking the core on Android needs liblog (nk_platform_posix.c's log redirection).
        self.assertRegex(cmake, r"elseif\(ANDROID\)\s*\n(?:\s*#.*\n)*\s*target_link_libraries\("
                                r"nakagawa_core PUBLIC log\)")
        # Release-type configurations would otherwise compile every assert() away.
        self.assertIn("target_compile_options(${nk_target} PRIVATE -UNDEBUG)", cmake)
        for test in ("sr_coro_test", "fp_convert_test", "ge_float24_test", "ge_raster_ref_test"):
            self.assertIn(f"add_test(NAME {test} ", cmake)


if __name__ == "__main__":
    unittest.main()
