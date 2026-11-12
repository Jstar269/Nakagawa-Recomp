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
            assert ci_pin is not None and android_pin is not None
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
        env = {k: v for k, v in os.environ.items() if k not in ("NK_TARGET", "CC", "CFLAGS")}
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
        for flag in ("-shared", "-Wl,--wrap=exit", "-Wl,--wrap=_exit", "-Wl,--wrap=_Exit"):
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

    def test_the_host_build_is_unchanged(self) -> None:
        proc = self._dry_run("compile", "CC=gcc", "SDL3_DIR=", "VULKAN_SDK=")
        out = proc.stdout + proc.stderr
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

    def test_title_activity_loads_only_what_title_library_allows(self) -> None:
        source = (ANDROID / "app" / "src" / "main" / "java" / "io" / "github" / "jstar269" /
                  "nakagawa" / "TitleActivity.java").read_text(encoding="utf-8")
        self.assertIn("TitleLibrary.resolve(\n                    library, "
                      "getApplicationInfo().nativeLibraryDir, getFilesDir().getPath());", source)
        self.assertRegex(source, r"if \(choice == null\) \{\s*refuse\(library, env\);")
        self.assertIn("static final int EXIT_REFUSED = 126;", source)
        self.assertIn('entry.startsWith("NK_ANDROID_EXIT_FILE=")', source)
        self.assertIn("Process.killProcess(Process.myPid());", source)
        # The env is applied before the library is chosen, so a refusal can record its status.
        self.assertLess(source.index("NakagawaHost.setenv(entry.substring(0, eq)"),
                        source.index("TitleLibrary.resolve("))


def cast_str(value: str | None) -> str:
    assert value is not None
    return value


if __name__ == "__main__":
    unittest.main()
