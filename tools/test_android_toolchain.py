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
import io
import os
import shutil
import subprocess
import sys
import tarfile
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
        work=work, cmake="cmake", ninja="ninja", jobs=3, host_cc="gcc", host_cxx="g++",
        make_tarball=work / "make.tar.gz", sh="sh", python_tarball=work / "python.tar.gz")


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


def _elf(machine: int = 183, kind: int = 3) -> bytes:
    return b"ELF" + bytes(9) + kind.to_bytes(2, "little") + machine.to_bytes(2, "little") + bytes(44)


class MakeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.cfg = _config(Path(tempfile.gettempdir()) / "nk-tc-make")

    def test_make_is_built_from_its_pinned_release(self) -> None:
        self.assertRegex(toolchain.MAKE_SHA256, r"^[0-9a-f]{64}$")
        self.assertTrue(toolchain.MAKE_URL.startswith("https://ftp.gnu.org/gnu/make/"))
        self.assertTrue(toolchain.MAKE_URL.endswith(f"/make-{toolchain.MAKE_VERSION}.tar.gz"))

    def test_an_unchecked_make_tarball_is_refused_before_extraction(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-tc-test-") as tmp:
            cfg = _config(Path(tmp))
            cfg.make_tarball.write_bytes(b"not make")
            with self.assertRaisesRegex(toolchain.ToolchainError, "pinned"):
                toolchain.stage_make(cfg)
            self.assertFalse(cfg.make_src.exists())

    def test_make_is_configured_for_the_device_with_the_project_clang(self) -> None:
        command, env = toolchain.make_configure(self.cfg)
        self.assertEqual(command[0], "sh")
        self.assertEqual(command[1], "../../src/make-4.4.1/configure")
        cc = next(arg for arg in command if arg.startswith("CC="))
        self.assertIn(Path(self.cfg.tool("clang")).as_posix(), cc)
        self.assertIn(f"--target={toolchain.TRIPLE}", cc)
        self.assertIn(f"--sysroot={self.cfg.sysroot.as_posix()}", cc)
        self.assertNotIn("\\", cc)
        self.assertIn("--host=aarch64-linux-android", command)
        self.assertIn("CFLAGS=-O2 -ffile-prefix-map=../../src/make-4.4.1=/make-src", command)
        self.assertIn("-Wl,--strip-all", next(arg for arg in command if arg.startswith("LDFLAGS=")))
        self.assertEqual(env, {"MSYS2_ARG_CONV_EXCL": "*"})

    def test_configure_runs_with_the_tools_beside_the_given_shell(self) -> None:
        shell = Path(tempfile.gettempdir()) / "posix" / "bin" / "sh"
        cfg = toolchain.Config(**{**self.cfg.__dict__, "sh": str(shell)})
        _command, env = toolchain.make_configure(cfg)
        self.assertTrue(env["PATH"].startswith(f"{shell.parent}{os.pathsep}"))

    def test_make_does_not_use_posix_spawn(self) -> None:
        # Bionic has no confstr(), which make's posix_spawn path needs.
        self.assertIn("--disable-posix-spawn", toolchain.MAKE_CONFIGURE)

    def test_the_device_make_must_be_a_pie_aarch64_elf(self) -> None:
        work = Path(tempfile.gettempdir()) / "nk-tc-work"
        toolchain.check_device_executable(_elf(), "make", work)
        for bad in (_elf(machine=62), _elf(kind=2), b"MZ" + bytes(62)):
            with self.assertRaisesRegex(toolchain.ToolchainError, "AArch64"):
                toolchain.check_device_executable(bad, "make", work)

    def test_the_device_make_must_not_name_the_build_folder(self) -> None:
        work = Path(tempfile.gettempdir()) / "nk-tc-work"
        with self.assertRaisesRegex(toolchain.ToolchainError, "build path"):
            toolchain.check_device_executable(_elf() + work.as_posix().encode() + b"/x.c", "make", work)


def _python_release(path: Path, *, extra: dict[str, bytes] | None = None, link: str | None = None) -> str:
    """A tarball in the CPython Android release layout; returns its SHA-256."""
    files = {
        "prefix/lib/libpython3.15.so": b"libpython",
        "prefix/lib/libpython3.so": b"stable abi stub",
        "prefix/lib/libssl_python.so": b"ssl",
        "prefix/lib/libcrypto.so": b"plain crypto",
        "prefix/lib/pkgconfig/python3.pc": b"pc",
        "prefix/include/python3.15/Python.h": b"/* Python.h */",
        "prefix/lib/python3.15/LICENSE.txt": b"PSF licence",
        "prefix/lib/python3.15/os.py": b"# os",
        "prefix/lib/python3.15/__pycache__/os.cpython-315.pyc": b"pyc",
        "prefix/lib/python3.15/lib-dynload/_ssl.cpython-315-aarch64-linux-android.so": b"elf",
        "README.md": b"readme",
        **(extra or {}),
    }
    with tarfile.open(path, "w:gz") as tar:
        for name, data in files.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
        if link is not None:
            info = tarfile.TarInfo(link)
            info.type = tarfile.SYMTYPE
            info.linkname = "elsewhere"
            tar.addfile(info)
    return hashlib.sha256(path.read_bytes()).hexdigest()


class PythonTests(unittest.TestCase):
    def test_python_is_the_pinned_official_release(self) -> None:
        self.assertRegex(toolchain.PYTHON_SHA256, r"^[0-9a-f]{64}$")
        self.assertEqual(toolchain.PYTHON_URL, "https://www.python.org/ftp/python/3.15.2/"
                         "python-3.15.2-aarch64-linux-android.tar.gz")
        for _name, digest, url in toolchain.PYTHON_NOTICES:
            self.assertRegex(digest, r"^[0-9a-f]{64}$")
            self.assertTrue(url.startswith("https://"))

    def test_an_unchecked_python_release_is_refused(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-tc-test-") as tmp:
            tarball = Path(tmp) / "python.tar.gz"
            _python_release(tarball)
            with self.assertRaisesRegex(toolchain.ToolchainError, "pinned"):
                toolchain.read_python_package(tarball)

    def test_the_apk_takes_libpython_its_libraries_and_the_stdlib(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-tc-test-") as tmp:
            tarball = Path(tmp) / "python.tar.gz"
            package = toolchain.read_python_package(tarball, _python_release(tarball))
        self.assertEqual(sorted(package.libraries), ["libpython3.15.so", "libssl_python.so"])
        self.assertEqual(list(package.headers), ["include/python3.15/Python.h"])
        self.assertEqual(sorted(package.stdlib), [
            "lib/python3.15/LICENSE.txt",
            "lib/python3.15/lib-dynload/_ssl.cpython-315-aarch64-linux-android.so",
            "lib/python3.15/os.py"])
        self.assertTrue(package.stdlib[
            "lib/python3.15/lib-dynload/_ssl.cpython-315-aarch64-linux-android.so"].executable)
        self.assertFalse(package.stdlib["lib/python3.15/os.py"].executable)
        self.assertEqual(package.license, b"PSF licence")

    def test_a_link_where_a_file_belongs_is_refused(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-tc-test-") as tmp:
            tarball = Path(tmp) / "python.tar.gz"
            digest = _python_release(tarball, link="prefix/lib/python3.15/site.py")
            with self.assertRaisesRegex(toolchain.ToolchainError, "not a regular file"):
                toolchain.read_python_package(tarball, digest)

    def test_a_different_layout_is_refused(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-tc-test-") as tmp:
            tarball = Path(tmp) / "python.tar.gz"
            with tarfile.open(tarball, "w:gz") as tar:
                info = tarfile.TarInfo("prefix/lib/libpython3.14.so")
                tar.addfile(info, io.BytesIO(b""))
            digest = hashlib.sha256(tarball.read_bytes()).hexdigest()
            with self.assertRaisesRegex(toolchain.ToolchainError, "release layout"):
                toolchain.read_python_package(tarball, digest)

    def test_the_launcher_finds_the_release_libraries_beside_itself(self) -> None:
        cfg = _config(Path(tempfile.gettempdir()) / "nk-tc-py")
        command = toolchain.python_launcher_command(cfg, ["libcrypto_python.so", "libpython3.15.so"])
        self.assertIn("-Wl,-rpath,$ORIGIN", command)
        # Each prefix map is its own argument; joined, clang reads one map with a garbage value.
        self.assertIn(f"-ffile-prefix-map={cfg.src}=/llvm-src", command)
        self.assertIn(f"-ffile-prefix-map={cfg.build}=/llvm-build", command)
        self.assertFalse([option for option in command if option.count("-ffile-prefix-map=") > 1])
        # Every library is linked, used or not, so the extension modules find it loaded.
        start = command.index("-Wl,--no-as-needed")
        self.assertEqual(command[start + 1:start + 4],
                         ["-l:libcrypto_python.so", "-l:libpython3.15.so", "-Wl,--as-needed"])

    def test_a_licence_file_that_changed_is_refused(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-tc-test-") as tmp:
            path = Path(tmp) / "LICENSE"
            path.write_bytes(b"changed")
            with self.assertRaisesRegex(toolchain.ToolchainError, "pinned"):
                toolchain.read_pinned(path, hashlib.sha256(b"original").hexdigest(), "https://example.invalid/")
            self.assertEqual(toolchain.read_pinned(path, hashlib.sha256(b"changed").hexdigest(), "u"), b"changed")


class MakeNoticeTests(unittest.TestCase):
    """make's notices in the APK: its licence and where its corresponding source is."""

    reference = {"repository": "https://example.invalid/nakagawa", "commit": "0123abc", "local_changes": False}

    def test_the_staged_notices_say_where_make_s_source_is(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-tc-test-") as tmp:
            source = Path(tmp) / "make-src"
            source.mkdir()
            (source / "COPYING").write_bytes(b"GPL-3.0 text")
            toolchain.stage_make_notices(source, Path(tmp) / "NOTICES", self.reference)
            folder = Path(tmp) / "NOTICES" / "make"
            self.assertEqual(sorted(path.name for path in folder.iterdir()), ["COPYING", "SOURCE.txt"])
            self.assertEqual((folder / "COPYING").read_bytes(), b"GPL-3.0 text")
            text = (folder / "SOURCE.txt").read_text(encoding="utf-8")
        for expected in (toolchain.MAKE_TARBALL, toolchain.MAKE_URL, toolchain.MAKE_SHA256,
                         "tools/android_toolchain.py", "tools/android_sysroot.py",
                         "https://example.invalid/nakagawa", "0123abc", *toolchain.MAKE_CONFIGURE):
            self.assertIn(expected, text)
        self.assertNotIn("local changes", text)

    def test_a_build_with_local_changes_says_so(self) -> None:
        text = toolchain.make_source_notice({**self.reference, "local_changes": True})
        self.assertIn("local changes", text)

    def test_stage_output_stages_them(self) -> None:
        # stage_output needs a full toolchain build; it must use this step, not copy COPYING alone.
        source = Path(toolchain.__file__).read_text(encoding="utf-8")
        body = source[source.index("def stage_output("):source.index("def main(")]
        self.assertIn('stage_make_notices(cfg.make_src, assets / "NOTICES"', body)


class GradleWiringTests(unittest.TestCase):
    """The app bundles the staged toolchain only when nk.toolchain names a staged folder."""

    gradle = (Path(__file__).resolve().parent.parent / "android" / "app" / "build.gradle.kts").read_text(
        encoding="utf-8")

    def test_the_staged_folders_are_the_app_s_sources(self) -> None:
        self.assertIn('findProperty("nk.toolchain")', self.gradle)
        # stage_output writes jniLibs/arm64-v8a/ and assets/toolchain/ under --out.
        self.assertIn('jniLibs.directories.add(File(toolchain, "jniLibs").path)', self.gradle)
        self.assertIn('assets.directories.add(File(toolchain, "assets").path)', self.gradle)

    def test_a_folder_that_was_not_staged_is_refused(self) -> None:
        self.assertIn('File(dir, "toolchain-manifest.json").isFile', self.gradle)

    def test_the_toolchain_is_bundled_for_arm64_only(self) -> None:
        self.assertIn('require(abis == listOf("arm64-v8a"))', self.gradle)

    def test_native_libraries_stay_extracted_so_they_can_run(self) -> None:
        self.assertIn("useLegacyPackaging = true", self.gradle)

    def test_the_apk_ships_the_staged_bytes(self) -> None:
        # AGP's NDK strip rewrites .shstrtab even in a binary linked --strip-all.
        self.assertIn('File(dir, "jniLibs/arm64-v8a").listFiles()?.forEach { keepDebugSymbols += "**/${it.name}" }',
                      self.gradle)


JAVA_SOURCES = Path(__file__).resolve().parent.parent / "android" / "app" / "src" / "main" / "java"
ARCHIVE_JAVA = JAVA_SOURCES / "io" / "github" / "jstar269" / "nakagawa" / "ToolchainArchive.java"
#: Runs ToolchainArchive.unpack(<archive>, <folder>) and prints the file count, or REFUSED and why.
ARCHIVE_MAIN = """package io.github.jstar269.nakagawa;
public final class ArchiveMain {
    public static void main(String[] args) throws Exception {
        try (java.io.InputStream in = new java.io.FileInputStream(args[0])) {
            System.out.println("FILES " + ToolchainArchive.unpack(in, new java.io.File(args[1])));
        } catch (java.io.IOException e) {
            System.out.println("REFUSED " + e.getMessage());
            System.exit(3);
        }
    }
}
"""


@unittest.skipIf(shutil.which("javac") is None or shutil.which("java") is None, "needs a JDK")
class ToolchainArchiveTests(unittest.TestCase):
    """The app's unpacker for assets/toolchain/*.tar, run on archives the staging step writes."""

    classes: Path
    _tmp: tempfile.TemporaryDirectory[str]

    @classmethod
    def setUpClass(cls) -> None:
        cls._tmp = tempfile.TemporaryDirectory(prefix="nk-tc-java-")
        root = Path(cls._tmp.name)
        main = root / "src" / "io" / "github" / "jstar269" / "nakagawa" / "ArchiveMain.java"
        main.parent.mkdir(parents=True)
        main.write_text(ARCHIVE_MAIN, encoding="utf-8")
        cls.classes = root / "classes"
        subprocess.run(["javac", "--release", "17", "-Xlint:all", "-Werror", "-d", str(cls.classes),
                        str(ARCHIVE_JAVA), str(main)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls._tmp.cleanup()

    def unpack(self, archive: Path, target: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(["java", "-cp", str(self.classes), "io.github.jstar269.nakagawa.ArchiveMain",
                               str(archive), str(target)], capture_output=True, text=True, check=False)

    def test_a_staged_archive_unpacks_to_the_same_files(self) -> None:
        long_name = "lib/python3.15/" + "/".join(["deep"] * 30) + "/module.py"  # past ustar's 100 bytes
        entries = {
            "usr/include/stdio.h": toolchain.android_sysroot.Entry(b"/* stdio */\n"),
            "lib/python3.15/lib-dynload/_ssl.so": toolchain.android_sysroot.Entry(b"\x7fELF", executable=True),
            long_name: toolchain.android_sysroot.Entry(b"x = 1\n" * 300),
            "empty.txt": toolchain.android_sysroot.Entry(b""),
        }
        with tempfile.TemporaryDirectory(prefix="nk-tc-test-") as tmp:
            archive = Path(tmp) / "a.tar"
            toolchain.android_sysroot.write_archive(entries, archive)
            result = self.unpack(archive, Path(tmp) / "out")
            self.assertEqual(result.stdout.strip(), "FILES 4", result.stdout + result.stderr)
            for name, entry in entries.items():
                self.assertEqual((Path(tmp) / "out" / name).read_bytes(), entry.data, name)
            if os.name != "nt":
                self.assertTrue(os.access(Path(tmp) / "out" / "lib/python3.15/lib-dynload/_ssl.so", os.X_OK))
                self.assertFalse(os.access(Path(tmp) / "out" / "usr/include/stdio.h", os.X_OK))

    def refused(self, build: object) -> str:
        with tempfile.TemporaryDirectory(prefix="nk-tc-test-") as tmp:
            archive = Path(tmp) / "a.tar"
            assert callable(build)
            build(archive)
            result = self.unpack(archive, Path(tmp) / "out" / "target")
            self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
            self.assertFalse((Path(tmp) / "out" / "evil").exists())
            return result.stdout

    @staticmethod
    def _tar(archive: Path, *infos: tuple[tarfile.TarInfo, bytes]) -> None:
        with tarfile.open(archive, "w", format=tarfile.PAX_FORMAT) as tar:
            for info, data in infos:
                info.size = len(data) if info.isreg() else 0
                tar.addfile(info, io.BytesIO(data) if info.isreg() else None)

    def test_a_link_is_refused(self) -> None:
        link = tarfile.TarInfo("usr/lib/libc.so")
        link.type, link.linkname = tarfile.SYMTYPE, "/system/lib64/libc.so"
        self.assertIn("is not a folder or a file", self.refused(lambda a: self._tar(a, (link, b""))))

    def test_a_path_out_of_the_folder_is_refused(self) -> None:
        for name in ("../evil", "usr/../../evil", "/evil", "usr//evil", "./evil"):
            with self.subTest(name=name):
                self.assertIn("path", self.refused(lambda a, n=name: self._tar(a, (tarfile.TarInfo(n), b"x"))))

    def test_a_damaged_header_is_refused(self) -> None:
        def build(archive: Path) -> None:
            self._tar(archive, (tarfile.TarInfo("usr/a.h"), b"a"))
            data = bytearray(archive.read_bytes())
            data[0] ^= 0x01
            archive.write_bytes(bytes(data))
        self.assertIn("checksum", self.refused(build))

    def test_an_archive_cut_short_is_refused(self) -> None:
        def build(archive: Path) -> None:
            self._tar(archive, (tarfile.TarInfo("usr/a.h"), b"a" * 2000))
            archive.write_bytes(archive.read_bytes()[:1024])
        self.assertIn("ends inside a file", self.refused(build))

    def test_a_pax_record_length_that_overflows_is_refused(self) -> None:
        # The second record's length, added to its offset, passes Integer.MAX_VALUE; the bound
        # check must refuse it with an IOException, not index the array with the wrapped sum.
        def header(name: bytes, size: int, kind: bytes) -> bytes:
            block = bytearray(512)
            block[0:len(name)] = name
            block[100:124] = b"0000644\0" b"0000000\0" b"0000000\0"
            block[124:136] = b"%011o\0" % size
            block[136:148] = b"00000000000\0"
            block[156:157] = kind
            block[257:265] = b"ustar\x0000"
            block[148:156] = b" " * 8
            block[148:156] = b"%06o\0 " % sum(block)
            return bytes(block)

        def build(archive: Path) -> None:
            data = b"6 a=b\n" b"2147483647 path=x\n"
            archive.write_bytes(header(b"pax", len(data), b"x") + data + bytes(512 - len(data))
                                + header(b"f", 0, b"0") + bytes(1024))
        self.assertIn("bad PAX record", self.refused(build))


#: Runs one ToolchainArchive.Install scenario on a.tar and b.tar in <archives> with <work> as the
#: files folder, and prints what it saw as "key value" lines.
INSTALL_MAIN = """package io.github.jstar269.nakagawa;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InterruptedIOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicInteger;
public final class InstallMain {
    static final String[][] ARCHIVES = {{"a.tar", "a"}, {"b.tar", "b"}};
    static File archives;
    static File work;
    static final AtomicInteger opens = new AtomicInteger();
    static final Set<String> threads = ConcurrentHashMap.newKeySet();
    /** b.tar's open number (1 counts, 2 unpacks) that gets bad.tar instead; 0 for none. */
    static volatile int badOpen;
    /** b.tar's open number at which the process dies, as one killed part way does; 0 for none. */
    static volatile int dieOpen;
    static final CountDownLatch release = new CountDownLatch(1);
    static volatile boolean gated;
    static final AtomicInteger bOpens = new AtomicInteger();

    static InputStream open(String name) throws IOException {
        opens.incrementAndGet();
        threads.add(Thread.currentThread().getName());
        if (gated) {
            try {
                release.await();
            } catch (InterruptedException e) {
                throw new InterruptedIOException();
            }
        }
        String file = name;
        int bOpen = name.equals("b.tar") ? bOpens.incrementAndGet() : 0;
        if (bOpen != 0 && bOpen == dieOpen) Runtime.getRuntime().halt(9);
        if (bOpen != 0 && bOpen == badOpen) file = "bad.tar";
        return new FileInputStream(new File(archives, file));
    }

    static ToolchainArchive.Install install(String stamp) {
        return new ToolchainArchive.Install(InstallMain::open, ARCHIVES, new File(work, "toolchain"),
            new File(work, ".toolchain-unpacked"), new File(work, ".toolchain.lock"), stamp);
    }

    static void print(String key, Object value) {
        System.out.println(key + " " + value);
    }

    /** What LauncherActivity.main() does: awaitReady(set the variables, hand them to native code). */
    static Thread waiter(ToolchainArchive.Install install, List<String> actions, String key) {
        Thread waiter = new Thread(() -> {
            try {
                boolean ready = install.awaitReady(
                    () -> actions.add("set " + install.status().state + " complete=" + install.isComplete()),
                    () -> actions.add("import"));
                print(key, ready + " " + actions);
            } catch (InterruptedException e) {
                print(key, "interrupted");
            }
        });
        waiter.start();
        while (waiter.isAlive() && waiter.getState() != Thread.State.WAITING) Thread.onSpinWait();
        return waiter;
    }

    public static void main(String[] args) throws Exception {
        archives = new File(args[0]);
        work = new File(args[1]);
        ExecutorService executor = Executors.newSingleThreadExecutor(r -> new Thread(r, "unpack-worker"));
        List<ToolchainArchive.Status> seen = Collections.synchronizedList(new ArrayList<>());
        try {
            switch (args[2]) {
                case "background": {
                    gated = true;
                    ToolchainArchive.Install install = install("v1");
                    install.setListener(seen::add);
                    print("started", install.start(executor));
                    // The unpack is held at its first open, so start() has returned while it runs.
                    print("state-after-start", install.status().state);
                    print("second-start", install.start(executor));
                    release.countDown();
                    print("end", install.await());
                    List<String> actions = Collections.synchronizedList(new ArrayList<>());
                    waiter(install, actions, "ready").join();
                    print("skip-when-done", install.skip());
                    print("third-start", install.start(executor));
                    print("threads", threads);
                    print("opens", opens.get());
                    print("complete", install.isComplete());
                    boolean ordered = true;
                    int last = 0;
                    for (ToolchainArchive.Status s : seen) {
                        if (s.done > 0 && (s.total == 0 || s.done > s.total || s.done < last)) ordered = false;
                        last = s.done;
                    }
                    print("progress-ordered", ordered);
                    print("last-seen", seen.get(seen.size() - 1));
                    break;
                }
                case "failure": {
                    badOpen = 2;  // b.tar counts clean, then is damaged when unpacked, after a/
                    ToolchainArchive.Install install = install("v1");
                    print("started", install.start(executor));
                    ToolchainArchive.Status failed = install.await();
                    print("failed", failed);
                    print("folder-after-failure", new File(work, "toolchain").exists());
                    print("stamp-after-failure", new File(work, ".toolchain-unpacked").exists());
                    print("complete-after-failure", install.isComplete());
                    print("complete-for-a-new-start", install("v1").isComplete());
                    List<String> actions = Collections.synchronizedList(new ArrayList<>());
                    Thread waiter = waiter(install, actions, "waiter");
                    print("waiter-waits-on-failed", waiter.getState() + " " + install.status().state);
                    print("retry", install.start(executor));
                    waiter.join();
                    print("after-retry", install.status());
                    print("complete-after-retry", install.isComplete());
                    break;
                }
                case "die": {
                    dieOpen = 2;  // after a/ is unpacked, while b/ is about to be
                    ToolchainArchive.Install install = install("v1");
                    install.start(executor);
                    install.await();
                    print("survived", install.status());
                    break;
                }
                case "redo": {
                    // What a run that died part way leaves: some files and no matching stamp.
                    ToolchainArchive.Install install = install("v1");
                    print("complete-before", install.isComplete());
                    print("started", install.start(executor));
                    print("end", install.await());
                    print("threads", threads);
                    print("leftover", new File(work, "toolchain/a/leftover.h").exists());
                    print("complete", install.isComplete());
                    break;
                }
                case "complete": {
                    ToolchainArchive.Install install = install("v1");
                    print("complete-before", install.isComplete());
                    print("started", install.start(executor));
                    print("end", install.await());
                    print("opens", opens.get());
                    break;
                }
                case "skip": {
                    badOpen = 2;
                    ToolchainArchive.Install install = install("v1");
                    install.start(executor);
                    print("failed", install.await().state);
                    List<String> actions = Collections.synchronizedList(new ArrayList<>());
                    Thread waiter = waiter(install, actions, "player");
                    print("waiter-waits-on-failed", waiter.getState() + " " + install.status().state);
                    print("skip", install.skip());  // Continue without the build tools
                    waiter.join();
                    print("skip-again", install.skip());
                    print("state", install.status().state);
                    print("folder", new File(work, "toolchain").exists());
                    print("stamp", new File(work, ".toolchain-unpacked").exists());
                    print("complete-after-skip", install.isComplete());
                    // The next start, a new process: the unpack runs again.
                    ToolchainArchive.Install next = install("v1");
                    print("next-complete-before", next.isComplete());
                    print("next-started", next.start(executor));
                    print("next-end", next.await());
                    print("complete", next.isComplete());
                    break;
                }
                case "unknown-version": {
                    ToolchainArchive.Install install = install(null);
                    print("complete-before", install.isComplete());
                    install.start(executor);
                    ToolchainArchive.Status end = install.await();
                    print("end", end.state);
                    print("error", end.error);
                    print("opens", opens.get());
                    print("leftover", new File(work, "toolchain").exists());
                    break;
                }
                default:
                    throw new IllegalArgumentException(args[2]);
            }
        } finally {
            executor.shutdown();
        }
    }
}
"""


@unittest.skipIf(shutil.which("javac") is None or shutil.which("java") is None, "needs a JDK")
class ToolchainInstallTests(unittest.TestCase):
    """The app's whole-toolchain unpack (ToolchainArchive.Install), which runs off the UI thread."""

    classes: Path
    _tmp: tempfile.TemporaryDirectory[str]

    @classmethod
    def setUpClass(cls) -> None:
        cls._tmp = tempfile.TemporaryDirectory(prefix="nk-tc-java-")
        root = Path(cls._tmp.name)
        main = root / "src" / "io" / "github" / "jstar269" / "nakagawa" / "InstallMain.java"
        main.parent.mkdir(parents=True)
        main.write_text(INSTALL_MAIN, encoding="utf-8")
        cls.classes = root / "classes"
        subprocess.run(["javac", "--release", "17", "-Xlint:all", "-Werror", "-d", str(cls.classes),
                        str(ARCHIVE_JAVA), str(main)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls._tmp.cleanup()

    def run_scenario(self, scenario: str, prepare: object = None) -> dict[str, str]:
        with tempfile.TemporaryDirectory(prefix="nk-tc-test-") as tmp:
            archives, work = Path(tmp) / "archives", Path(tmp) / "files"
            archives.mkdir()
            work.mkdir()
            entry = toolchain.android_sysroot.Entry
            toolchain.android_sysroot.write_archive({"a.h": entry(b"a"), "sub/a2.h": entry(b"a2")},
                                                    archives / "a.tar")
            toolchain.android_sysroot.write_archive({"b.h": entry(b"b"), "c.h": entry(b"c"),
                                                     "sub/d.h": entry(b"d")}, archives / "b.tar")
            bad = bytearray((archives / "b.tar").read_bytes())
            bad[0] ^= 0x01
            (archives / "bad.tar").write_bytes(bytes(bad))
            if prepare is not None:
                assert callable(prepare)
                prepare(work)
            result = subprocess.run(["java", "-cp", str(self.classes), "io.github.jstar269.nakagawa.InstallMain",
                                     str(archives), str(work), scenario],
                                    capture_output=True, text=True, check=False, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            seen: dict[str, str] = {}
            for line in result.stdout.splitlines():
                key, _, value = line.partition(" ")
                seen[key] = value
            if seen.get("complete") == "true" or seen.get("complete-after-retry") == "true":
                for name, data in (("a/a.h", b"a"), ("a/sub/a2.h", b"a2"), ("b/b.h", b"b"), ("b/sub/d.h", b"d")):
                    self.assertEqual((work / "toolchain" / name).read_bytes(), data, name)
                self.assertEqual((work / ".toolchain-unpacked").read_text(encoding="utf-8"), "v1")
            return seen

    def test_the_unpack_runs_on_the_worker_and_once(self) -> None:
        seen = self.run_scenario("background")
        self.assertEqual(seen["started"], "true")
        self.assertEqual(seen["state-after-start"], "RUNNING")   # start() did not wait for it
        self.assertEqual(seen["second-start"], "false")          # a second start runs nothing
        self.assertEqual(seen["third-start"], "false")           # nor one after it is done
        self.assertEqual(seen["end"], "DONE 5/5")
        # Once done: the variables are set (with the unpack complete), then handed to native code.
        self.assertEqual(seen["ready"], "true [set DONE complete=true, import]")
        self.assertEqual(seen["skip-when-done"], "false")
        self.assertEqual(seen["threads"], "[unpack-worker]")     # never on the caller's thread
        self.assertEqual(seen["opens"], "4")                     # one count and one unpack each
        self.assertEqual(seen["complete"], "true")
        self.assertEqual(seen["progress-ordered"], "true")       # the total is known before any file
        self.assertEqual(seen["last-seen"], "DONE 5/5")

    def test_a_failed_unpack_leaves_no_stamp_and_can_be_retried(self) -> None:
        seen = self.run_scenario("failure")
        # It failed after writing a/'s two files, and deleted them.
        self.assertEqual(seen["failed"], "FAILED 2/5 tar header checksum does not match")
        self.assertEqual(seen["folder-after-failure"], "false")
        self.assertEqual(seen["stamp-after-failure"], "false")
        self.assertEqual(seen["complete-after-failure"], "false")
        self.assertEqual(seen["complete-for-a-new-start"], "false")
        self.assertEqual(seen["waiter-waits-on-failed"], "WAITING FAILED")  # a failure does not release main()
        self.assertEqual(seen["retry"], "true")
        self.assertEqual(seen["waiter"], "true [set DONE complete=true, import]")
        self.assertEqual(seen["after-retry"], "DONE 5/5")
        self.assertEqual(seen["complete-after-retry"], "true")

    def test_continuing_without_the_toolchain_leaves_it_unset_and_the_next_start_retries(self) -> None:
        seen = self.run_scenario("skip")
        self.assertEqual(seen["failed"], "FAILED")
        self.assertEqual(seen["waiter-waits-on-failed"], "WAITING FAILED")
        self.assertEqual(seen["skip"], "true")
        self.assertEqual(seen["player"], "false []")             # the player starts; nothing is set
        self.assertEqual(seen["skip-again"], "false")
        self.assertEqual(seen["state"], "SKIPPED")
        self.assertEqual(seen["folder"], "false")                # the partial folder is gone
        self.assertEqual(seen["stamp"], "false")
        self.assertEqual(seen["complete-after-skip"], "false")
        self.assertEqual(seen["next-complete-before"], "false")
        self.assertEqual(seen["next-started"], "true")
        self.assertEqual(seen["next-end"], "DONE 5/5")
        self.assertEqual(seen["complete"], "true")

    def test_a_run_that_died_part_way_is_done_again(self) -> None:
        for stamp in (None, "v0"):
            with self.subTest(stamp=stamp):
                def prepare(work: Path, stamp: str | None = stamp) -> None:
                    (work / "toolchain" / "a").mkdir(parents=True)
                    (work / "toolchain" / "a" / "leftover.h").write_bytes(b"partial")
                    if stamp is not None:
                        (work / ".toolchain-unpacked").write_text(stamp, encoding="utf-8")
                seen = self.run_scenario("redo", prepare)
                self.assertEqual(seen["complete-before"], "false")
                self.assertEqual(seen["started"], "true")
                self.assertEqual(seen["end"], "DONE 5/5")
                self.assertEqual(seen["threads"], "[unpack-worker]")
                self.assertEqual(seen["leftover"], "false")
                self.assertEqual(seen["complete"], "true")

    def test_a_process_killed_part_way_leaves_no_stamp_and_the_next_start_redoes_it(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-tc-test-") as tmp:
            archives, work = Path(tmp) / "archives", Path(tmp) / "files"
            archives.mkdir()
            work.mkdir()
            entry = toolchain.android_sysroot.Entry
            toolchain.android_sysroot.write_archive({"a.h": entry(b"a")}, archives / "a.tar")
            toolchain.android_sysroot.write_archive({"b.h": entry(b"b")}, archives / "b.tar")
            (work / ".toolchain-unpacked").write_text("v0", encoding="utf-8")   # an earlier APK's

            def run(scenario: str) -> subprocess.CompletedProcess[str]:
                return subprocess.run(["java", "-cp", str(self.classes), "io.github.jstar269.nakagawa.InstallMain",
                                       str(archives), str(work), scenario],
                                      capture_output=True, text=True, check=False, timeout=120)
            died = run("die")
            self.assertEqual(died.returncode, 9, died.stdout + died.stderr)
            self.assertTrue((work / "toolchain" / "a" / "a.h").is_file())        # it died part way
            self.assertFalse((work / ".toolchain-unpacked").exists())             # the stamp went first
            again = run("redo")
            self.assertIn("complete-before false", again.stdout, again.stdout + again.stderr)
            self.assertIn("complete true", again.stdout)
            self.assertEqual((work / ".toolchain-unpacked").read_text(encoding="utf-8"), "v1")

    def test_a_finished_unpack_is_not_done_again(self) -> None:
        seen = self.run_scenario("complete", lambda work: (work / ".toolchain-unpacked").write_text(
            "v1", encoding="utf-8"))
        self.assertEqual(seen["complete-before"], "true")
        self.assertEqual(seen["end"], "DONE 0/0")
        self.assertEqual(seen["opens"], "0")

    def test_an_unknown_app_version_fails(self) -> None:
        def prepare(work: Path) -> None:
            (work / ".toolchain-unpacked").write_text("unknown", encoding="utf-8")
            (work / "toolchain" / "a").mkdir(parents=True)
            (work / "toolchain" / "a" / "leftover.h").write_bytes(b"partial")
        seen = self.run_scenario("unknown-version", prepare)
        self.assertEqual(seen["complete-before"], "false")
        self.assertEqual(seen["end"], "FAILED")
        self.assertEqual(seen["error"], "the app's version cannot be read")
        self.assertEqual(seen["opens"], "0")
        self.assertEqual(seen["leftover"], "false")              # its space comes back too


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
