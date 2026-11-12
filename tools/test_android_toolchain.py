# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""The on-device toolchain's own code (#360): the build driver and the ld launcher.

tools/android_toolchain.py's LLVM build takes about an hour; the tests cover the decisions
around it: the pinned input, the configure options and the NDK check.

android/toolchain/nk_ld_launcher.c is built with the host compiler and started next to a
stand-in libnk_lld.so, a small C program that records the argv it was given and exits with
the status the test asks for. The launcher reads /proc/self/exe, so this runs on Linux (WSL
included) and is skipped elsewhere.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LAUNCHER = ROOT / "android" / "toolchain" / "nk_ld_launcher.c"

sys.path.insert(0, str(ROOT / "tools"))

import android_toolchain as toolchain  # noqa: E402

STAND_IN = r"""
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    FILE *record = fopen(getenv("NK_LD_RECORD"), "w");
    if (record == NULL) return 99;
    for (int i = 0; i < argc; i++) fprintf(record, "%s\n", argv[i]);
    fclose(record);
    return atoi(getenv("NK_LD_STATUS"));
}
"""


@unittest.skipUnless(sys.platform.startswith("linux"), "the launcher reads /proc/self/exe")
class LdLauncherTests(unittest.TestCase):
    _tmp: tempfile.TemporaryDirectory[str]
    folder: Path
    launcher: Path
    stand_in: Path

    @classmethod
    def setUpClass(cls) -> None:
        compiler = shutil.which(os.environ.get("CC", "cc")) or shutil.which("gcc")
        if compiler is None:
            raise unittest.SkipTest("no C compiler")
        cls._tmp = tempfile.TemporaryDirectory(prefix="nk-ld-launcher-")
        root = Path(cls._tmp.name)
        cls.folder = root / "lib" / "arm64"
        cls.folder.mkdir(parents=True)
        cls.launcher = cls.folder / "libnk_ld.so"
        flags = ["-std=c11", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror"]
        subprocess.run([compiler, *flags, "-o", str(cls.launcher), str(LAUNCHER)], check=True)
        (root / "stand_in.c").write_bytes(STAND_IN.encode())
        cls.stand_in = root / "stand_in"
        subprocess.run([compiler, *flags, "-o", str(cls.stand_in), str(root / "stand_in.c")], check=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls._tmp.cleanup()

    def setUp(self) -> None:
        self.record = self.folder / "record.txt"
        self.record.unlink(missing_ok=True)
        self.lld = self.folder / "libnk_lld.so"
        self.lld.unlink(missing_ok=True)

    def install_stand_in(self) -> None:
        shutil.copy2(self.stand_in, self.lld)

    def run_launcher(self, *args: str, status: int = 0, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
        env = dict(os.environ, NK_LD_RECORD=str(self.record), NK_LD_STATUS=str(status))
        return subprocess.run(
            [str(self.launcher), *args], env=env, cwd=cwd, capture_output=True, text=True, check=False
        )

    def recorded(self) -> list[str]:
        return self.record.read_text().split("\n")[:-1]

    def test_lld_starts_as_ld_lld_with_every_argument_unchanged(self) -> None:
        self.install_stand_in()
        args = ("-o", "out file", "--version-script=a.map", "", "x.o")
        result = self.run_launcher(*args)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.recorded(), ["ld.lld", *args])

    def test_lld_is_found_beside_the_launcher_from_any_working_directory(self) -> None:
        self.install_stand_in()
        result = self.run_launcher("x.o", cwd=Path(self._tmp.name))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.recorded(), ["ld.lld", "x.o"])

    def test_the_exit_status_is_lld_s(self) -> None:
        self.install_stand_in()
        self.assertEqual(self.run_launcher("x.o", status=3).returncode, 3)

    def test_a_missing_lld_returns_127_and_names_it(self) -> None:
        result = self.run_launcher("x.o")
        self.assertEqual(result.returncode, 127)
        self.assertIn(f"libnk_ld: cannot start {self.lld}", result.stderr)
        self.assertFalse(self.record.exists())


def _config(work: Path) -> toolchain.Config:
    return toolchain.Config(
        llvm_tarball=work / "llvm.tar.xz", bionic=work / "bionic", extra=work / "extra",
        work=work, cmake="cmake", ninja="ninja", jobs=3, host_cc="gcc", host_cxx="g++")


class PinTests(unittest.TestCase):
    def test_the_tarball_must_match_the_pinned_sha256(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-tc-test-") as tmp:
            tarball = Path(tmp) / "llvm.tar.xz"
            with self.assertRaisesRegex(toolchain.ToolchainError, "missing"):
                toolchain.verify_tarball(tarball)
            tarball.write_bytes(b"not llvm")
            with self.assertRaisesRegex(toolchain.ToolchainError, "pinned"):
                toolchain.verify_tarball(tarball)
            toolchain.verify_tarball(tarball, hashlib.sha256(b"not llvm").hexdigest())

    def test_the_source_is_extracted_only_from_a_checked_tarball(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-tc-test-") as tmp:
            cfg = _config(Path(tmp))
            cfg.llvm_tarball.write_bytes(b"not llvm")
            with self.assertRaisesRegex(toolchain.ToolchainError, "pinned"):
                toolchain.extract_source(cfg)
            self.assertFalse(cfg.src.exists())

    def test_the_pin_names_one_release(self) -> None:
        self.assertRegex(toolchain.LLVM_SHA256, r"^[0-9a-f]{64}$")
        self.assertIn(f"llvmorg-{toolchain.LLVM_VERSION}/", toolchain.LLVM_URL)
        self.assertTrue(toolchain.LLVM_URL.endswith(toolchain.LLVM_TARBALL))


class ConfigureTests(unittest.TestCase):
    def setUp(self) -> None:
        self.cfg = _config(Path("/w"))

    def test_every_llvm_build_is_aarch64_only_and_links_one_at_a_time(self) -> None:
        for command in (toolchain.host_configure(self.cfg), toolchain.device_configure(self.cfg)):
            self.assertIn("-DLLVM_TARGETS_TO_BUILD=AArch64", command)
            self.assertIn("-DLLVM_PARALLEL_LINK_JOBS=1", command)
            self.assertIn("-DLLVM_ENABLE_ZLIB=OFF", command)

    def test_build_paths_are_mapped_out_of_the_binaries(self) -> None:
        maps = self.cfg.prefix_maps()
        self.assertIn("=/llvm-src", maps)
        self.assertIn("=/llvm-build", maps)
        for command in (toolchain.host_configure(self.cfg), toolchain.runtimes_configure(self.cfg),
                        toolchain.device_configure(self.cfg)):
            self.assertTrue(any(maps in option for option in command if option.startswith("-DCMAKE_C_FLAGS=")))

    def test_the_source_and_build_folders_do_not_nest(self) -> None:
        # Nested folders would match both prefix maps, and which one clang applies would then
        # decide the paths in the binaries.
        self.assertFalse(self.cfg.build.is_relative_to(self.cfg.src))
        self.assertFalse(self.cfg.src.is_relative_to(self.cfg.build))
        for folder in (self.cfg.sysroot, self.cfg.resource, self.cfg.host_bin):
            self.assertTrue(folder.is_relative_to(self.cfg.build), folder)

    def test_the_device_build_uses_only_the_project_runtimes(self) -> None:
        command = toolchain.device_configure(self.cfg)
        link = next(o for o in command if o.startswith("-DCMAKE_EXE_LINKER_FLAGS="))
        for flag in ("-rtlib=compiler-rt", "-unwindlib=libunwind", "-stdlib=libc++", "-static-libstdc++"):
            self.assertIn(flag, link)
        self.assertIn(f"-DCMAKE_SYSROOT={self.cfg.sysroot}", command)
        self.assertIn(f"-DLLVM_NATIVE_TOOL_DIR={self.cfg.host_bin}", command)
        compilers = [o for o in command if o.startswith("-DCMAKE_C") and "COMPILER=" in o]
        self.assertTrue(compilers)
        for option in compilers:
            self.assertIn(str(self.cfg.host_bin), option)
        self.assertFalse(any("ndk" in option.lower() for option in command))

    def test_the_runtimes_are_static(self) -> None:
        command = toolchain.runtimes_configure(self.cfg)
        for library in ("LIBUNWIND", "LIBCXXABI", "LIBCXX"):
            self.assertIn(f"-D{library}_ENABLE_SHARED=OFF", command)
        self.assertIn("-DLIBCXX_STATICALLY_LINK_ABI_IN_STATIC_LIBRARY=ON", command)

    def test_libraries_android_lacks_are_not_linked(self) -> None:
        # A static-library try-compile finds every library, so libc++.a would carry
        # dependent-library entries for pthread and rt, which no Android link can resolve.
        command = toolchain.runtimes_configure(self.cfg)
        for option in ("LIBCXX_HAS_PTHREAD_LIB", "LIBCXX_HAS_RT_LIB", "LIBCXX_HAS_ATOMIC_LIB",
                       "LIBCXXABI_HAS_PTHREAD_LIB", "LIBUNWIND_HAS_PTHREAD_LIB",
                       "COMPILER_RT_HAS_GCC_LIB"):
            self.assertIn(f"-D{option}=OFF", command)


class NdkCheckTests(unittest.TestCase):
    def test_a_binary_with_the_ndk_libcxx_is_refused(self) -> None:
        data = b"\x7fELF...NSt3__112system_errorE\0...NSt6__ndk112bad_weak_ptrE\0"
        with self.assertRaisesRegex(toolchain.ToolchainError, "1 names from the NDK"):
            toolchain.check_no_ndk_runtime(data, "clang")

    def test_a_binary_without_the_project_libcxx_is_refused(self) -> None:
        with self.assertRaisesRegex(toolchain.ToolchainError, "no std::__1"):
            toolchain.check_no_ndk_runtime(b"\x7fELF...main\0", "clang")

    def test_a_binary_with_the_project_libcxx_passes(self) -> None:
        toolchain.check_no_ndk_runtime(b"\x7fELF...NSt3__112system_errorE\0", "clang")

    def test_the_device_binaries_are_linked_without_a_symbol_table(self) -> None:
        # lld writes .symtab differently from link to link; see DEVICE_LINK.
        self.assertIn("-Wl,--strip-all", toolchain.DEVICE_LINK)
        self.assertIn("-Wl,--build-id=sha1", toolchain.DEVICE_LINK)


class ThirdPartyNoticeTests(unittest.TestCase):
    def write_sources(self, src: Path, *, drop: str | None = None) -> None:
        for relative, _first, last, marker in toolchain.LLVM_THIRD_PARTY_EXCERPTS:
            lines = [f"line {n}" for n in range(1, last + 6)]
            if relative != drop:
                lines[last - 1] = f"line {last} {marker}"
            (src / relative).parent.mkdir(parents=True, exist_ok=True)
            (src / relative).write_text("\n".join(lines) + "\n", encoding="utf-8")

    def test_each_header_notice_is_copied_with_its_source_path_and_lines(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-tc-test-") as tmp:
            src = Path(tmp)
            self.write_sources(src)
            text = toolchain.third_party_notices(src)
            for relative, first, last, marker in toolchain.LLVM_THIRD_PARTY_EXCERPTS:
                self.assertIn(f"==== {relative}, lines {first}-{last} ====", text)
                self.assertIn(f"line {last} {marker}", text)
                self.assertNotIn(f"line {last + 1}\n", text.split(f"==== {relative},")[1].split("====")[1])

    def test_a_notice_that_moved_is_refused(self) -> None:
        relative = toolchain.LLVM_THIRD_PARTY_EXCERPTS[0][0]
        with tempfile.TemporaryDirectory(prefix="nk-tc-test-") as tmp:
            self.write_sources(Path(tmp), drop=relative)
            with self.assertRaisesRegex(toolchain.ToolchainError, "no longer hold"):
                toolchain.third_party_notices(Path(tmp))

    def test_the_known_third_party_code_is_covered(self) -> None:
        covered = {path for path, *_ in toolchain.LLVM_THIRD_PARTY_EXCERPTS}
        covered.update(toolchain.LLVM_THIRD_PARTY_FILES)
        for path in ("llvm/lib/Support/COPYRIGHT.regex", "llvm/lib/Support/BLAKE3/LICENSE",
                     "llvm/lib/Support/xxhash.cpp", "llvm/lib/Support/ConvertUTF.cpp",
                     "llvm/lib/Support/MD5.cpp", "libcxx/src/ryu/d2s.cpp"):
            self.assertIn(path, covered)


class StampTests(unittest.TestCase):
    def test_a_finished_stage_is_not_run_again(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-tc-test-") as tmp:
            cfg = _config(Path(tmp))
            (cfg.build / "host").mkdir(parents=True)
            toolchain._mark(cfg.build / "host")
            # cmake is never started: a run would fail, as "cmake" here names nothing built.
            toolchain.stage_host(toolchain.Config(**{**cfg.__dict__, "cmake": str(Path(tmp) / "none")}))


if __name__ == "__main__":
    unittest.main()
