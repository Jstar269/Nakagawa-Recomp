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

from collections.abc import Mapping
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


def _make_exported_names(text: str) -> set[str]:
    """Every variable the makefile text ``export``s, alone or after a target (``goal: export X``)."""
    return set(re.findall(r"(?m)^(?:[^\s#][^:\n]*:\s*)?export\s+([A-Za-z_][A-Za-z0-9_]*)", text))


def _isolated_make_environment(environ: Mapping[str, str]) -> dict[str, str]:
    """``environ`` without what an outer make hands a child make, for a plan of this repository.

    Run under make (``make contrib-check``, ``make test``), this process inherits every variable
    the makefiles export with the outer make's value, and MAKEFLAGS, which carries the outer
    command line's variable overrides. A child ``make -n`` would take each of those over its own
    ``?=`` default: a Windows outer make exports ``H264_FFMPEG=0``, and the Android plan then
    drops the vendored FFmpeg sources. NK_TARGET, CC and CFLAGS are the overrides the plans set.
    """
    caller_only = {"NK_TARGET", "CC", "CFLAGS", "MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKEOVERRIDES"}
    caller_only |= _make_exported_names(read_makefile())
    return {key: value for key, value in environ.items() if key.upper() not in caller_only}


def _compiled_sources(dry_run: str) -> set[str]:
    """The ``.c`` files a ``make -n`` plan compiles, recipes spanning lines joined."""
    commands = dry_run.replace("\\\n", " ").splitlines()
    return {m.group(1) for c in commands for m in [re.search(r" -c (\S+\.c) -o \S+\.o\b", c)] if m}


def _cmake_list(text: str, name: str) -> list[str]:
    match = re.search(r"^set\(" + re.escape(name) + r"\b(.*?)\)", text, re.S | re.M)
    if not match:
        raise AssertionError(f"CMakeLists.txt has no set({name} ...)")
    return [w for w in match.group(1).split() if not w.startswith("#")]


def _unconditional_definitions(text: str, variable: str) -> set[str]:
    """The ``-DNAME`` tokens assigned to ``variable`` outside an ifeq/ifdef/ifndef block.

    A definition a Makefile adds only for one profile is deliberately absent here: the
    parity check is about the macros every normal build of the variable carries.
    """
    definitions: set[str] = set()
    depth = 0
    assignment = re.compile(r"^(?:override\s+)?" + re.escape(variable) + r"\s*(?::=|\?=|\+=|=)\s*(.*)$")
    for line in text.replace("\\\n", " ").splitlines():
        stripped = line.strip()
        if re.match(r"^(?:ifeq|ifneq|ifdef|ifndef)\b", stripped):
            depth += 1
            continue
        if re.match(r"^endif\b", stripped):
            depth = max(0, depth - 1)
            continue
        if depth:
            continue
        match = assignment.match(stripped)
        if match:
            definitions.update(token[2:] for token in match.group(1).split()
                               if token.startswith("-D") and len(token) > 2)
    return definitions


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

    def test_only_the_imported_h264_units_are_compiled_with_w(self) -> None:
        # nk_h264_api.c is project-authored (src/rt/h264_ffmpeg_sub/PROVENANCE.md), so, as in
        # the Makefile's h264sub_nk_h264_api.o rule, it keeps the project warning set; only
        # the imported FFmpeg units, kept byte-identical, take -w.
        def options(name: str) -> list[str]:
            match = re.search(r"target_compile_options\(\$\{target\}_" + name + r" PRIVATE(.*?)\)",
                              self.cmake, re.S)
            self.assertIsNotNone(match, f"no compile options for ${{target}}_{name}")
            assert match is not None
            return match.group(1).split()
        for line in ('list(FILTER h264sub EXCLUDE REGEX "/nk_h264_api[.]c$")',
                     'list(FILTER h264api INCLUDE REGEX "/nk_h264_api[.]c$")',
                     "add_library(${target}_h264_api OBJECT ${h264api})",
                     "$<TARGET_OBJECTS:${target}_h264_api>"):
            self.assertTrue(line in self.cmake, f"CMakeLists.txt lacks {line}")
        self.assertIn("-w", options("h264"))
        self.assertNotIn("-w", options("h264_api"))
        self.assertIn("-Wall", options("h264_api"))

    def test_ge_compile_definitions_match_the_makefile(self) -> None:
        """The `${target}_ge` object is the Makefile's RT_GE_O (GE_CFLAGS). A macro the
        Makefile always passes to ge.c must reach the Android title build too, or ge.c
        compiles against the header's no-op stubs (for example the flight recorder's
        sr_flight_record() and SR_WATCH_ACTIVE() when SR_FLIGHT_RECORDER_LINKED is absent)."""
        smoke = (ROOT / "mk" / "smoke_gates.mk").read_text(encoding="utf-8")
        make_defs = _unconditional_definitions(smoke, "GE_CFLAGS")
        self.assertIn("SR_SDL3VK", make_defs)
        self.assertIn("SR_FLIGHT_RECORDER_LINKED", make_defs)
        target = re.search(r"target_compile_definitions\(\$\{target\}_ge PRIVATE ([^)]*)\)", self.cmake)
        self.assertIsNotNone(target, "CMakeLists.txt must define ${target}_ge's macros explicitly")
        assert target is not None
        cmake_defs = set(target.group(1).split())
        missing = sorted(make_defs - cmake_defs)
        self.assertEqual(
            missing, [],
            "GE_CFLAGS macros the Android ${target}_ge object does not define:\n  " + "\n  ".join(missing),
        )


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
            assert ci_pin is not None and android_pin is not None
            self.assertEqual(android_pin.group(1), ci_pin.group(1), cmake_name)

    def test_manifest_requests_no_permissions(self) -> None:
        manifests = sorted(ANDROID.glob("app/src/*/AndroidManifest.xml"))
        # An empty glob (a moved source set) would otherwise pass without reading one manifest.
        self.assertIn(ANDROID / "app" / "src" / "main" / "AndroidManifest.xml", manifests)
        for manifest in manifests:
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
        self.assertIn("private static native void nativeImportEnvironment();", launcher)
        self.assertIn("JNICALL Java_io_github_jstar269_nakagawa_LauncherActivity_nativeImportEnvironment(\n"
                      "    JNIEnv *env, jclass cls) {\n    (void)cls;\n    android_import_environment(env);", posix)

    def test_the_player_starts_after_the_toolchain_variables_reach_native_code(self) -> None:
        """Under an ARM translation layer the player library has its own libc, which took the
        environment when it loaded; variables a toolchain unpack sets later reach it only through
        nativeImportEnvironment, which must run after they are set and before the player's main()."""
        java = ANDROID / "app" / "src" / "main" / "java" / "io" / "github" / "jstar269" / "nakagawa"
        launcher = (java / "LauncherActivity.java").read_text(encoding="utf-8")
        main = launcher[launcher.index("    protected void main() {"):]
        main = main[:main.index("\n    }\n") + 6]
        ready = re.search(r"install\.awaitReady\(\(\) -> NakagawaHost\.setToolchainEnvironment\(this\),\s+"
                          r"LauncherActivity::nativeImportEnvironment\)", main)
        self.assertIsNotNone(ready, main)
        assert ready is not None
        self.assertLess(ready.end(), main.index("super.main();"))
        self.assertEqual(main.count("super.main();"), 1)

    def test_title_process_ends_without_the_framework_handlers(self) -> None:
        """exit() in the app process would run the framework's static destructors while its render
        threads still run (FORTIFY abort). The runtime's atexit() and exit() go through the shim,
        which runs only the runtime's handlers, drains the log copy and ends with _exit()."""
        cmake = (ANDROID / "app" / "CMakeLists.txt").read_text(encoding="utf-8")
        shim = (ANDROID / "app" / "src" / "main" / "cpp" / "nk_android_title.c").read_text(encoding="utf-8")
        for symbol in ("exit", "_exit", "_Exit", "atexit"):
            self.assertIn(f"-Wl,--wrap={symbol}", cmake)
        self.assertIn("int __wrap_atexit(void (*handler)(void))", shim)
        self.assertNotIn("__real_exit(", shim)
        self.assertIn("__wrap_exit(main(argc, argv));", shim)
        header = (ROOT / "src" / "core" / "nk_platform.h").read_text(encoding="utf-8")
        self.assertIn("void nk_platform_host_shutdown(void);", header)
        for source in ("nk_platform_posix.c", "nk_platform_win32.c"):
            text = (ROOT / "src" / "core" / source).read_text(encoding="utf-8")
            self.assertIn("void nk_platform_host_shutdown(void)", text)

    @unittest.skipUnless(sys.platform.startswith("linux") and shutil.which("cc"), "needs a Linux C compiler")
    def test_the_title_shim_ends_the_process_as_exit_would(self) -> None:
        """The shim built for Linux with the Android link's --wrap flags, around a stub runtime:
        only the runtime's handlers run (one registered with the C library itself does not), last
        registered first and once, a handler's own exit() goes on with the rest,
        a second thread's exit() waits for the first, and the exit file is written after the log
        is drained, with the status the process ends with."""
        shim = ANDROID / "app" / "src" / "main" / "cpp" / "nk_android_title.c"
        with tempfile.TemporaryDirectory(prefix="nk-title-shim-") as tmp:
            work = Path(tmp)
            (work / "harness.c").write_text(_TITLE_SHIM_HARNESS, encoding="utf-8")
            program = work / "title"
            wraps = [f"-Wl,--wrap={symbol}" for symbol in ("exit", "_exit", "_Exit", "atexit")]
            subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror", "-I", str(ROOT / "src" / "core"),
                            "-Dmain=nk_shim_runtime_main", "-c", str(shim), "-o", str(work / "shim.o")],
                           check=True)
            subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror", "-c",
                            str(work / "harness.c"), "-o", str(work / "harness.o")], check=True)
            subprocess.run(["cc", str(work / "harness.o"), str(work / "shim.o"), "-pthread", *wraps,
                            "-o", str(program)], check=True)
            cases = {
                "order": (3, "b a shutdown(no-exit-file) "),
                "nested": (5, "b nested a shutdown(no-exit-file) "),
                "_exit": (4, "shutdown(no-exit-file) "),
                "race": (2, "slow racer slow-done a shutdown(no-exit-file) "),
                "foreign": (6, "a shutdown(no-exit-file) "),
            }
            for mode, (status, trace) in cases.items():
                with self.subTest(mode=mode):
                    env = dict(os.environ, NK_ANDROID_EXIT_FILE=str(work / f"exit-{mode}"),
                               NK_TEST_TRACE=str(work / f"trace-{mode}"))
                    env.pop("NK_ANDROID_PID_FILE", None)
                    ran = subprocess.run([str(program), mode], env=env, check=False, timeout=60)
                    self.assertEqual(ran.returncode, status)
                    self.assertEqual((work / f"exit-{mode}").read_text(encoding="utf-8"), f"{status}\n")
                    self.assertEqual((work / f"trace-{mode}").read_text(encoding="utf-8"), trace)

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
        self.assertIsNone(self.paths.map(str(self.tmp / "src-other" / "a")))
        self.assertEqual(self.paths.map_value("--check"), "--check")

    def test_the_part_below_a_tree_keeps_its_case(self) -> None:
        # The device's file system is case-sensitive; the files are synced under their real names.
        font = self.source / "assets" / "fonts" / "UI" / "NotoSans-Regular.ttf"
        self.assertEqual(str(self.paths.map(str(font))),
                         f"{DEVICE}/src/assets/fonts/UI/NotoSans-Regular.ttf")
        self.assertEqual(self.paths.map_value(str(self.binary / "build" / "Out.bin")),
                         f"{DEVICE}/work/build/Out.bin")
        if os.name == "nt":
            # Windows paths are case-insensitive: a root spelled in another case is still the tree.
            self.assertEqual(str(self.paths.map(os.path.join(str(self.source).upper(), "assets", "Fonts"))),
                             f"{DEVICE}/src/assets/Fonts")

    def test_remote_script_maps_cwd_args_and_test_environment(self) -> None:
        env = {"NK_TEST_SCRATCH_DIR": str(self.binary / "build"), "SR_GESTAT": "1",
               "NK_ADB": "C:/sdk/adb.exe", "PATH": "C:/Windows"}
        script = runner.remote_run_script(self.device, self.paths, str(self.source), "test_x",
                                          ["--input", str(self.source / "fixtures" / "a b")], env)
        self.assertEqual(
            script,
            f"mkdir -p {DEVICE}/src && cd {DEVICE}/src && "
            f"env TMPDIR={DEVICE}/tmp NK_TEST_SCRATCH_DIR={DEVICE}/work/build SR_GESTAT=1 "
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
        # src/rt/sr_coro.c does not build on macOS (ELF-only assembly, no _XOPEN_SOURCE for
        # Apple's ucontext), so neither coroutine target may be defined there.
        self.assertRegex(cmake, r"if\(NOT APPLE\)\n\s*add_executable\(test_sr_coro ")
        self.assertRegex(cmake, r"if\(NOT WIN32 AND NOT ANDROID AND NOT APPLE\n"
                                r"\s*AND CMAKE_SYSTEM_PROCESSOR [^\n]*\n"
                                r"(?:\s*#.*\n)*\s*add_executable\(test_sr_coro_asm ")


class AndroidMakeTargetTests(unittest.TestCase):
    """NK_TARGET=android: the package route's Make build links a title as the library
    TitleActivity loads, on the device or cross from a PC (#360). A dry run (make -n) of the
    compile goal over an empty pregenerated title needs no compiler or SDK."""

    make: str | None
    root: Path
    sdl: Path
    build_dir: Path
    android_out: str

    @classmethod
    def setUpClass(cls) -> None:
        cls.make = shutil.which("mingw32-make") or shutil.which("make")
        if not cls.make:
            raise unittest.SkipTest("GNU Make is required")
        scratch = tempfile.TemporaryDirectory(prefix="nk-android-make-")
        cls.addClassCleanup(scratch.cleanup)
        cls.root = Path(scratch.name)
        cls.sdl = cls.root / "sdl"
        (cls.sdl / "include" / "SDL3").mkdir(parents=True)
        (cls.sdl / "include" / "SDL3" / "SDL_version.h").write_text(
            "#define SDL_MAJOR_VERSION   3\n#define SDL_MINOR_VERSION   4\n"
            "#define SDL_MICRO_VERSION   18\n", encoding="ascii")
        (cls.sdl / "lib").mkdir()
        (cls.sdl / "lib" / "libSDL3.so").write_bytes(b"")
        cls.build_dir = cls.root / "build" / "demo"
        cls.build_dir.mkdir(parents=True)
        for name in ("demo_recomp.c", "demo_recomp_0.c", "demo_recomp_funcs.h"):
            (cls.build_dir / name).write_bytes(b"")
        # One Android dry run serves every test that reads it (each run parses the whole
        # Makefile, about 15 s).
        proc = cls._dry_run("compile", *cls._android_overrides())
        if proc.returncode != 0:
            raise AssertionError(proc.stdout[-2000:] + proc.stderr[-2000:])
        cls.android_out = proc.stdout

    @classmethod
    def _android_overrides(cls) -> tuple[str, ...]:
        return ("NK_TARGET=android", "CC=aarch64-linux-android29-clang",
                f"ANDROID_SDL3_INCLUDE={(cls.sdl / 'include').as_posix()}",
                f"ANDROID_SDL3_LIBDIR={(cls.sdl / 'lib').as_posix()}")

    @classmethod
    def _dry_run(cls, goal: str, *overrides: str) -> subprocess.CompletedProcess[str]:
        env = _isolated_make_environment(os.environ)
        return subprocess.run(
            [cast_str(cls.make), "-n", "--no-print-directory", goal,
             f"PYTHON={Path(sys.executable).as_posix()}", "GAME_NAME=demo",
             "NK_AOT_PREGENERATED=1", f"BUILD_ROOT={(cls.root / 'build').as_posix()}",
             f"LOG_DIR={(cls.root / 'logs').as_posix()}",
             f"BUILD_DIR={cls.build_dir.as_posix()}", *overrides],
            cwd=ROOT, env=env, capture_output=True, text=True, timeout=300, check=False)

    def _android_dry_run(self) -> str:
        return self.android_out

    def test_the_title_links_as_the_library_title_activity_loads(self) -> None:
        out = self._android_dry_run()
        library = f"{self.build_dir.as_posix()}/libdemo.so"
        link = next((line for line in out.splitlines() if f"-o {library}" in line), None)
        self.assertIsNotNone(link, "the compile goal links no libdemo.so")
        link_words = str(link).split()
        # The same four wraps as android/app/CMakeLists.txt: without atexit, the runtime's handlers
        # would register with the C library, and the shim's _exit() would drop every one.
        for flag in ("-shared", "-Wl,--wrap=exit", "-Wl,--wrap=_exit", "-Wl,--wrap=_Exit", "-Wl,--wrap=atexit"):
            self.assertIn(flag, link_words)
        rsp = (out.split(f"-o {library}", 1)[1]).split("\n", 3)[:3]
        self.assertIn("-lSDL3 -lvulkan", " ".join(rsp))
        self.assertIn("-lm -llog -landroid", " ".join(rsp))
        self.assertIn(f'-L"{(self.sdl / "lib").as_posix()}"', link_words)
        # The entry point TitleActivity calls is compiled and linked.
        self.assertRegex(out, r"-c android/app/src/main/cpp/nk_android_title\.c -o \S+/nk_android_title\.o")

    def test_every_object_is_position_independent_and_posix(self) -> None:
        out = self._android_dry_run()
        # A recipe may span lines; join each command before reading its flags.
        commands = out.replace("\\\n", " ").splitlines()
        compiles = [c for c in commands if re.search(r" -c \S+\.c -o \S+\.o\b", c)]
        self.assertGreater(len(compiles), 150)
        self.assertEqual([c for c in compiles if "-fPIC" not in c.split()], [])
        sources = {m.group(1) for c in compiles for m in [re.search(r" -c (\S+\.c) ", c)] if m}
        self.assertIn("src/core/nk_platform_posix.c", sources)
        self.assertNotIn("src/core/nk_platform_win32.c", sources)
        self.assertIn("src/rt/h264_ffmpeg.c", sources)
        self.assertFalse([c for c in commands if "windres" in c or "copy_build_assets" in c])

    def test_the_plan_ignores_what_an_outer_make_exports(self) -> None:
        # Run from inside a Windows make, this process inherits H264_FFMPEG=0 (Makefile's
        # `export H264_FFMPEG`) and MAKEFLAGS; the Android plan must not change with them.
        self.assertIn("H264_FFMPEG", _make_exported_names(read_makefile()))
        outer = {"H264_FFMPEG": "0", "MAKEFLAGS": "-- H264_FFMPEG=0", "MAKELEVEL": "1"}
        with mock.patch.dict(os.environ, outer):
            proc = self._dry_run("compile", *self._android_overrides())
        self.assertEqual(proc.returncode, 0, proc.stdout[-2000:] + proc.stderr[-2000:])
        # The first plan wrote its profile stamps, so the text differs; what it compiles may not.
        compiled = _compiled_sources(proc.stdout)
        self.assertIn("src/rt/h264_ffmpeg.c", compiled)
        self.assertEqual(compiled, _compiled_sources(self.android_out))

    def test_the_host_build_is_unchanged(self) -> None:
        proc = self._dry_run("compile", "CC=gcc", "SDL3_DIR=", "VULKAN_SDK=")
        out = proc.stdout + proc.stderr
        if proc.returncode != 0 and "SDL3 discovery failed" in proc.stderr:
            self.skipTest("this host's SDL3 cannot be discovered, so its compile goal cannot be planned: "
                          + proc.stderr.strip()[-500:])
        # A dry run that stopped (a parse error, any $(error)) prints none of the Android names
        # either, so the host plan must be there before their absence means anything.
        self.assertEqual(proc.returncode, 0, out[-2000:])
        self.assertRegex(proc.stdout, rf"-o {re.escape(self.build_dir.as_posix())}/demo(?:\.exe)?(?:\s|$)",
                         "the host compile goal links no title executable")
        self.assertNotIn("libdemo.so", out)
        self.assertNotIn("nk_android_title", out)
        self.assertNotIn("-shared", out.split())

    def test_an_unknown_target_or_missing_sdl3_is_refused(self) -> None:
        proc = self._dry_run("compile", "NK_TARGET=ios")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("NK_TARGET 'ios' is not a target this Makefile builds", proc.stderr)
        for missing in ("ANDROID_SDL3_INCLUDE", "ANDROID_SDL3_LIBDIR"):
            overrides = [o for o in self._android_overrides() if not o.startswith(missing)]
            proc = self._dry_run("sdl3-check", *overrides, f"{missing}={(self.root / 'missing').as_posix()}")
            self.assertIn(f"NK_TARGET=android needs {missing}", proc.stdout)


class AndroidPackageTargetTests(unittest.TestCase):
    """The package route's target choice (tools/title_codegen_plan.py, nk_cli build-package)."""

    def test_the_package_names_the_file_make_links(self) -> None:
        import title_codegen_plan as plan
        self.assertEqual(plan.package_executable_name("demo", "android"), "libdemo.so")
        self.assertEqual(plan.package_executable_name("demo", "host"),
                         "demo.exe" if os.name == "nt" else "demo")
        self.assertEqual(plan.PACKAGE_TARGETS, ("host", "android"))

    def test_android_is_the_default_only_on_android(self) -> None:
        import title_codegen_plan as plan
        with mock.patch.object(plan.sys, "platform", "android"):
            self.assertEqual(plan.default_package_target(), "android")
        with mock.patch.object(plan.sys, "platform", "linux"):
            self.assertEqual(plan.default_package_target(), "host")

    def test_an_unknown_target_is_refused(self) -> None:
        import title_codegen_plan as plan
        with self.assertRaises(plan.PackageRouteError) as caught:
            plan.build_package({}, manifest_path=Path("m.json"), game_elf=Path("e.elf"),
                               output_dir=Path("out"), target="ios")
        self.assertEqual(caught.exception.code, "PACKAGE_INVALID_TARGET")

    def test_the_cli_passes_the_target_to_the_planner(self) -> None:
        help_text = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "nk_cli.py"), "build-package", "--help"],
            cwd=ROOT, capture_output=True, text=True, timeout=120, check=True).stdout
        self.assertRegex(help_text, r"--target \{host,android\}")
        source = (ROOT / "tools" / "nk_cli.py").read_text(encoding="utf-8")
        self.assertIn('planner.extend(("--target", package_target))', source)


_TITLE_LIBRARY_PROBE = """package io.github.jstar269.nakagawa;

public final class TitleLibraryProbe {
    public static void main(String[] args) {
        if (args[0].equals("--launch-file")) {
            for (int i = 2; i < args.length; i++) {
                System.out.println(TitleLibrary.launchFile(args[i], args[1]) == null ? "REFUSED" : "OK");
            }
            return;
        }
        if (args[0].equals("--write-status")) {
            boolean recorded = TitleLibrary.writeStatus(args[1], Integer.parseInt(args[2]));
            System.out.println(recorded ? "RECORDED" : "FAILED");
            return;
        }
        for (int i = 2; i < args.length; i++) {
            TitleLibrary.Choice choice = TitleLibrary.resolve(args[i], args[0], args[1]);
            System.out.println(choice == null ? "REFUSED" : choice.path + "|" + choice.bundledName);
        }
    }
}
"""


class AndroidTitleLibraryTests(unittest.TestCase):
    """TitleActivity loads a library the APK ships, or one built in the app's files folder, and
    nothing else (TitleLibrary.java, #360). The rule is plain Java, run here with the host JDK."""

    work: Path
    java: list[str]
    lib: Path
    files: Path
    outside: Path
    links: bool

    @classmethod
    def setUpClass(cls) -> None:
        home = os.environ.get("JAVA_HOME", "")
        javac = shutil.which("javac") or (shutil.which("javac", path=str(Path(home) / "bin")) if home else None)
        if not javac:
            raise unittest.SkipTest("a JDK (javac) is required")
        java = shutil.which("java", path=str(Path(javac).parent)) or shutil.which("java")
        scratch = tempfile.TemporaryDirectory(prefix="nk-title-library-")
        cls.addClassCleanup(scratch.cleanup)
        cls.work = Path(scratch.name).resolve()
        package = cls.work / "src" / "io" / "github" / "jstar269" / "nakagawa"
        package.mkdir(parents=True)
        (package / "TitleLibraryProbe.java").write_text(_TITLE_LIBRARY_PROBE, encoding="utf-8")
        classes = cls.work / "classes"
        subprocess.run([javac, "-d", str(classes), "-encoding", "UTF-8",
                        str(ANDROID / "app" / "src" / "main" / "java" / "io" / "github" / "jstar269" /
                            "nakagawa" / "TitleLibrary.java"),
                        str(package / "TitleLibraryProbe.java")],
                       check=True, capture_output=True, text=True, timeout=300)
        cls.java = [cast_str(java), "-cp", str(classes), "io.github.jstar269.nakagawa.TitleLibraryProbe"]
        # The app's two folders, as Android lays them out, and a folder outside both.
        cls.lib = cls.work / "app" / "lib" / "arm64"
        cls.files = cls.work / "data" / "files"
        cls.outside = cls.work / "outside"
        for folder in (cls.lib / "sub", cls.files / "Nakagawa" / "packages" / "ULUS00000", cls.outside):
            folder.mkdir(parents=True)
        for path in (cls.lib / "libnk_title_demo.so", cls.lib / "sub" / "libnested.so",
                     cls.files / "Nakagawa" / "packages" / "ULUS00000" / "libULUS00000.so",
                     cls.files / "notes.txt", cls.outside / "libother.so"):
            path.write_bytes(b"\x7fELF")
        (cls.files / "dir.so").mkdir()
        cls.links = True
        try:
            (cls.files / "escape.so").symlink_to(cls.outside / "libother.so")
            (cls.files / "inside.so").symlink_to(
                cls.files / "Nakagawa" / "packages" / "ULUS00000" / "libULUS00000.so")
        except OSError:
            cls.links = False   # Windows without the symbolic-link privilege

    def _resolve(self, *libraries: str) -> list[str]:
        out = subprocess.run([*self.java, self.lib.as_posix(), self.files.as_posix(), *libraries],
                             check=True, capture_output=True, text=True, timeout=120).stdout
        return out.splitlines()

    def _abs(self, path: Path) -> str:
        # Java's canonical form: on Windows the drive path with backslashes, else the POSIX path.
        # Built here, not taken from str(): an MSYS2 Python on Windows prints paths with "/".
        resolved = str(path.resolve())
        return resolved.replace("/", "\\") if os.name == "nt" else resolved

    def test_a_library_the_apk_ships_loads_by_name_or_path(self) -> None:
        bundled = f"{self._abs(self.lib / 'libnk_title_demo.so')}|nk_title_demo"
        self.assertEqual(self._resolve("nk_title_demo", "libnk_title_demo.so",
                                       (self.lib / "libnk_title_demo.so").as_posix()),
                         [bundled, bundled, bundled])

    def test_a_built_title_loads_by_its_path_in_the_files_folder(self) -> None:
        built = self.files / "Nakagawa" / "packages" / "ULUS00000" / "libULUS00000.so"
        self.assertEqual(self._resolve(built.as_posix()), [f"{self._abs(built)}|"])
        if self.links:
            self.assertEqual(self._resolve((self.files / "inside.so").as_posix()), [f"{self._abs(built)}|"])

    def test_every_other_name_is_refused(self) -> None:
        refused = [
            (self.outside / "libother.so").as_posix(),               # outside both folders
            (self.files / ".." / ".." / "outside" / "libother.so").as_posix(),   # a .. escape
            (self.lib / "sub" / "libnested.so").as_posix(),          # not directly in the lib folder
            "sub/libnested.so", "Nakagawa/packages/ULUS00000/libULUS00000.so",   # relative paths
            (self.files / "notes.txt").as_posix(),                   # not a library
            (self.files / "dir.so").as_posix(),                      # a folder
            (self.files / "missing.so").as_posix(), "nk_title_missing", "lib.so", "",
        ]
        if self.links:
            refused.append((self.files / "escape.so").as_posix())    # a link that resolves out
        self.assertEqual(self._resolve(*refused), ["REFUSED"] * len(refused))

    def _probe(self, *args: str) -> list[str]:
        out = subprocess.run([*self.java, *args], check=True, capture_output=True, text=True,
                             timeout=120).stdout
        return out.splitlines()

    def test_only_the_launchers_status_files_are_accepted(self) -> None:
        """A debug build exports TitleActivity, so its env extra may come from any app: the exit
        and pid files it names are written by renaming over them, and only the launcher's
        title-launch-<pid>-<serial>.<exit|pid> names are taken."""
        cache = (self.files / "Nakagawa" / "cache").as_posix()
        self.assertEqual(self._probe("--launch-file", "exit", f"{cache}/title-launch-4242-1.exit"), ["OK"])
        self.assertEqual(self._probe("--launch-file", "pid", f"{cache}/title-launch-4242-17.pid"), ["OK"])
        refused = [
            (self.files / "Nakagawa" / "settings.json").as_posix(),
            f"{cache}/title-launch-4242-1.pid",          # the other status file
            f"{cache}/title-launch-4242-1.exit.tmp",
            f"{cache}/title-launch-x-1.exit",
            f"{cache}/title-launch-1-1.exit/../settings.json",
            "",
        ]
        self.assertEqual(self._probe("--launch-file", "exit", *refused), ["REFUSED"] * len(refused))

    def test_a_status_is_recorded_only_when_written_in_full(self) -> None:
        """As nk_android_title.c's write_number_file: the temporary file is renamed over the
        status file only after the whole number is written, so a failed write publishes
        nothing the launcher could read as a status."""
        folder = self.work / "status"
        folder.mkdir(exist_ok=True)
        good = folder / "title-launch-1-1.exit"
        self.assertEqual(self._probe("--write-status", str(good), "126"), ["RECORDED"])
        self.assertEqual(good.read_text(encoding="ascii"), "126\n")
        self.assertFalse((folder / "title-launch-1-1.exit.tmp").exists())
        bad = folder / "title-launch-1-2.exit"
        (folder / "title-launch-1-2.exit.tmp").mkdir()   # the temporary file cannot be written
        self.assertEqual(self._probe("--write-status", str(bad), "126"), ["FAILED"])
        self.assertFalse(os.path.lexists(bad), "a failed write must not be renamed into place")

    def test_title_activity_loads_only_what_title_library_allows(self) -> None:
        source = (ANDROID / "app" / "src" / "main" / "java" / "io" / "github" / "jstar269" /
                  "nakagawa" / "TitleActivity.java").read_text(encoding="utf-8")
        self.assertIn("TitleLibrary.resolve(\n                    library, "
                      "getApplicationInfo().nativeLibraryDir, getFilesDir().getPath());", source)
        self.assertRegex(source, r"if \(choice == null\) \{[^}]*endProcess\(EXIT_REFUSED\);")
        self.assertIn("static final int EXIT_REFUSED = 126;", source)
        self.assertIn("Process.killProcess(Process.myPid());", source)
        # The env is applied before the library is chosen, so a refusal can record its status.
        self.assertLess(source.index("NakagawaHost.setenv(name, value);"),
                        source.index("TitleLibrary.resolve("))

    def test_title_activity_takes_only_the_launchers_status_files(self) -> None:
        source = (ANDROID / "app" / "src" / "main" / "java" / "io" / "github" / "jstar269" /
                  "nakagawa" / "TitleActivity.java").read_text(encoding="utf-8")
        check = source.index("TitleLibrary.launchFile(value, suffix) == null")
        self.assertIn('name.equals("NK_ANDROID_EXIT_FILE") ? "exit"', source)
        self.assertIn('name.equals("NK_ANDROID_PID_FILE") ? "pid"', source)
        self.assertLess(check, source.index("NakagawaHost.setenv(name, value);"))
        self.assertIn("continue;", source[check:source.index("NakagawaHost.setenv(name, value);")])
        # The exit status is written only through TitleLibrary.writeStatus.
        self.assertIn("TitleLibrary.writeStatus(mExitFile, status)", source)
        self.assertNotIn("renameTo(", source)

    def test_a_title_that_never_starts_records_a_status(self) -> None:
        """nk_android_title_main ends the process itself, so SDL's main returning, or SDL failing
        to load a library, means the runtime never ran: the launcher gets status 127 instead of
        waiting forever for a pid or exit file."""
        source = (ANDROID / "app" / "src" / "main" / "java" / "io" / "github" / "jstar269" /
                  "nakagawa" / "TitleActivity.java").read_text(encoding="utf-8")
        self.assertIn("static final int EXIT_NOT_STARTED = 127;", source)
        main = re.search(r"protected void main\(\) \{(.*?)\n    \}", source, re.S)
        self.assertIsNotNone(main)
        assert main is not None
        self.assertLess(main.group(1).index("super.main();"),
                        main.group(1).index("endProcess(EXIT_NOT_STARTED);"))
        created = source[source.index("super.onCreate(savedInstanceState);"):]
        self.assertRegex(created, r"^super\.onCreate\(savedInstanceState\);\s+if \(mBrokenLibraries\) \{"
                                  r"[^}]*endProcess\(EXIT_NOT_STARTED\);")
        shim = (ANDROID / "app" / "src" / "main" / "cpp" / "nk_android_title.c").read_text(encoding="utf-8")
        self.assertIn("    __wrap_exit(main(argc, argv));\n}", shim)

    def test_a_failed_bundled_file_unpack_is_shown(self) -> None:
        """A failed copy of the bundled files is shown by the launcher, and does not also hide
        the toolchain unpack (its flag is set before the copy)."""
        java = ANDROID / "app" / "src" / "main" / "java" / "io" / "github" / "jstar269" / "nakagawa"
        host = (java / "NakagawaHost.java").read_text(encoding="utf-8")
        self.assertLess(host.index("sBundlesToolchain = bundlesToolchain;"),
                        host.index('copyAssetTree(context.getAssets(), "app/" + child'))
        failed = host[host.index('Log.e(TAG, "BUNDLED_FILES_UNPACK_FAILED dir="'):]
        self.assertLess(failed.index("sBundledFilesError = "), failed.index("return;"))
        launcher = (java / "LauncherActivity.java").read_text(encoding="utf-8")
        created = launcher[launcher.index("    protected void onCreate("):]
        created = created[:created.index("\n    }\n")]
        self.assertIn("NakagawaHost.bundledFilesError()", created)
        self.assertIn("showBundledFilesFailure(", created)
        strings = (ANDROID / "app" / "src" / "main" / "res" / "values" / "strings.xml").read_text(encoding="utf-8")
        self.assertRegex(strings, r'<string name="bundled_files_failed">[^<]*%1\$s')


def cast_str(value: str | None) -> str:
    assert value is not None
    return value


#: A stand-in for git, for AndroidPinnedDependencyTests: it logs each command line to
#: NK_TEST_GIT_LOG, keeps HEAD in .git/HEAD, and fails `submodule update` while
#: NK_TEST_SUBMODULE_FAIL is set, as a network drop would.
_FAKE_GIT = r"""
import os
import sys

args = sys.argv[1:]
with open(os.environ["NK_TEST_GIT_LOG"], "a", encoding="utf-8") as log:
    log.write(" ".join(args) + "\n")
while args[:1] == ["-c"]:
    args = args[2:]
if args[:1] == ["--version"]:
    print("git version 2.45.0")
    sys.exit(0)
folder = "."
if args[:1] == ["-C"]:
    folder, args = args[1], args[2:]
command = args[0]
if command == "init":
    os.makedirs(os.path.join(args[-1], ".git"), exist_ok=True)
elif command == "fetch":
    with open(os.path.join(folder, ".git", "FETCH_HEAD"), "w", encoding="utf-8") as out:
        out.write(args[-1])
elif command == "checkout":
    with open(os.path.join(folder, ".git", "FETCH_HEAD"), encoding="utf-8") as src:
        commit = src.read()
    with open(os.path.join(folder, ".git", "HEAD"), "w", encoding="utf-8") as out:
        out.write(commit)
elif command == "submodule":
    if os.environ.get("NK_TEST_SUBMODULE_FAIL"):
        sys.exit(1)
elif command == "rev-parse":
    try:
        with open(os.path.join(folder, ".git", "HEAD"), encoding="utf-8") as src:
            print(src.read())
    except OSError:
        sys.exit(128)
"""


class AndroidPinnedDependencyTests(unittest.TestCase):
    """android/app/deps.cmake reuses a pinned checkout only when its fetch completed: one left
    at the pinned commit by a failed or killed submodule fetch is fetched again."""

    def setUp(self) -> None:
        self.cmake = shutil.which("cmake")
        if not self.cmake:
            self.skipTest("CMake is required")
        scratch = tempfile.TemporaryDirectory(prefix="nk-android-deps-")
        self.addCleanup(scratch.cleanup)
        self.work = Path(scratch.name)
        script = self.work / "fake_git.py"
        script.write_text(_FAKE_GIT, encoding="utf-8")
        if os.name == "nt":
            self.git = self.work / "git.cmd"
            self.git.write_text(f'@"{sys.executable}" "{script}" %*\r\n', encoding="utf-8")
        else:
            self.git = self.work / "git"
            self.git.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{script}" "$@"\n', encoding="utf-8")
            self.git.chmod(0o755)
        self.log = self.work / "git.log"
        self.deps = self.work / "deps"

    def _configure(self, *, submodules_fail: bool) -> tuple[int, list[str]]:
        self.log.write_text("", encoding="utf-8")
        env = dict(os.environ, NK_TEST_GIT_LOG=str(self.log))
        env.pop("NK_TEST_SUBMODULE_FAIL", None)
        if submodules_fail:
            env["NK_TEST_SUBMODULE_FAIL"] = "1"
        ran = subprocess.run([cast_str(self.cmake), f"-DNK_ANDROID_DEPS_DIR={self.deps.as_posix()}",
                              f"-DGIT_EXECUTABLE={self.git.as_posix()}", "-P",
                              str(ANDROID / "app" / "deps.cmake")],
                             env=env, capture_output=True, text=True, timeout=300, check=False)
        fetches = [line for line in self.log.read_text(encoding="utf-8").splitlines() if " fetch " in line]
        return ran.returncode, fetches

    def test_a_checkout_left_by_a_failed_submodule_fetch_is_fetched_again(self) -> None:
        status, _ = self._configure(submodules_fail=True)
        self.assertNotEqual(status, 0, "a failed submodule fetch must stop the configure")
        status, fetches = self._configure(submodules_fail=False)
        self.assertEqual(status, 0)
        # SDL completed on the first run and is reused; SDL_ttf stopped after its checkout.
        self.assertEqual(len(fetches), 1, fetches)
        self.assertIn("SDL_ttf", fetches[0])
        status, fetches = self._configure(submodules_fail=False)
        self.assertEqual((status, fetches), (0, []), "a completed checkout is reused with no fetch")


#: A stub runtime for the title shim (nk_android_title.c), built on Linux by
#: test_the_title_shim_ends_the_process_as_exit_would. The shim's main() is renamed to
#: nk_shim_runtime_main; each mode registers handlers and ends the process one way.
_TITLE_SHIM_HARNESS = r"""
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int nk_android_title_main(int argc, char **argv);
int __real_atexit(void (*handler)(void));

static void say(const char *text) {
    int fd = open(getenv("NK_TEST_TRACE"), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return;
    if (write(fd, text, strlen(text)) < 0) {}
    close(fd);
}

bool nk_platform_host_startup(const char *log_tag) { (void)log_tag; return true; }
void nk_platform_host_shutdown(void) {
    say(access(getenv("NK_ANDROID_EXIT_FILE"), F_OK) == 0 ? "shutdown(exit-file) " : "shutdown(no-exit-file) ");
}

static void handler_a(void) { say("a "); }
static void handler_b(void) { say("b "); }
static void handler_foreign(void) { say("foreign "); }   /* registered with the C library itself */
static void handler_nested(void) { say("nested "); exit(5); }
static atomic_int s_racer_started;
static void *racer(void *arg) {
    (void)arg;
    say("racer ");
    atomic_store(&s_racer_started, 1);
    exit(9);   /* must wait for the exit the main thread owns */
    return NULL;
}
static void handler_slow(void) {
    say("slow ");
    pthread_t thread;
    if (pthread_create(&thread, NULL, racer, NULL) == 0) {
        while (!atomic_load(&s_racer_started)) sched_yield();
        usleep(200000);   /* time for the racer to reach the exit lock */
    }
    say("slow-done ");
}

int nk_shim_runtime_main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    if (strcmp(mode, "order") == 0) { atexit(handler_a); atexit(handler_b); return 3; }
    if (strcmp(mode, "nested") == 0) { atexit(handler_a); atexit(handler_nested); atexit(handler_b); exit(1); }
    if (strcmp(mode, "_exit") == 0) { atexit(handler_a); _exit(4); }
    if (strcmp(mode, "race") == 0) { atexit(handler_a); atexit(handler_slow); exit(2); }
    if (strcmp(mode, "foreign") == 0) { __real_atexit(handler_foreign); atexit(handler_a); exit(6); }
    return 99;
}

int main(int argc, char **argv) { return nk_android_title_main(argc, argv); }
"""


if __name__ == "__main__":
    unittest.main()
