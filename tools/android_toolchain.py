# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Build the on-device C toolchain from pinned LLVM and AOSP sources (#360).

The APK ships clang, lld and a sysroot for aarch64-linux-android29, all built by the project;
nothing comes from the Android NDK (android/toolchain/PROVENANCE.md). One LLVM release tarball,
checked against its pinned SHA-256, is built three times:

1. host: clang, lld and the LLVM binary tools for the build machine, by its C++ compiler;
2. sysroot: tools/android_sysroot.py, with the host clang compiling the crt objects and stubs;
3. runtimes: compiler-rt builtins, libunwind, libc++abi and libc++, static, by the host clang;
4. device: clang and lld for Android, by the host clang, linked against those runtimes. The
   build refuses a binary that holds the NDK's libc++ (its inline namespace is `__ndk1`).
5. make: GNU make 4.4.1 from its pinned release tarball, configured for the device and
   compiled by the host clang with make's own build.sh;
6. python: the official CPython 3.15.2 Android release, checked by SHA-256 and shipped
   unchanged, and libnk_python.so (android/toolchain/nk_python_launcher.c) built against it.

`stage` then writes what the APK carries (Android runs only lib*.so files from an app's native
library folder):

- `jniLibs/arm64-v8a/`: `libnk_clang.so`, `libnk_lld.so`, `libnk_make.so`, `libnk_python.so`
  with CPython's libraries, and `libnk_ld.so` (android/toolchain/nk_ld_launcher.c);
- `assets/toolchain/`: `sysroot.tar`, `clang-resource.tar` and `python-stdlib.tar`, and under
  `NOTICES/` the LICENSE.TXT of each LLVM project the binaries hold, the notices of the
  third-party code inside them (LLVM_THIRD_PARTY_FILES and _EXCERPTS), make's COPYING and
  Python's licences (PYTHON_NOTICES);
- `toolchain-manifest.json`, listing every file's SHA-256.

Each stage writes into its own folder under --work and runs only when that folder has no
`.done` stamp, so an interrupted build resumes. `--jobs` caps the compile jobs; links run one
at a time, as one LLVM link can take several GB.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import android_sysroot  # noqa: E402

LLVM_VERSION = "21.1.8"
LLVM_TARBALL = f"llvm-project-{LLVM_VERSION}.src.tar.xz"
LLVM_URL = f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{LLVM_VERSION}/{LLVM_TARBALL}"
LLVM_SHA256 = "4633a23617fa31a3ea51242586ea7fb1da7140e426bd62fc164261fe036aa142"
CLANG_MAJOR = LLVM_VERSION.split(".")[0]

MAKE_VERSION = "4.4.1"
MAKE_TARBALL = f"make-{MAKE_VERSION}.tar.gz"
MAKE_URL = f"https://ftp.gnu.org/gnu/make/{MAKE_TARBALL}"
MAKE_SHA256 = "dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3"
#: GNU make's own switches for a cross build with no optional parts. Bionic has no confstr(),
#: which make's posix_spawn path calls to find the default PATH, so make uses fork and exec.
MAKE_CONFIGURE = (
    "--host=aarch64-linux-android", "--disable-nls", "--without-guile", "--disable-load",
    "--disable-dependency-tracking", "--disable-posix-spawn",
)

PYTHON_VERSION = "3.15.2"
PYTHON_MINOR = ".".join(PYTHON_VERSION.split(".")[:2])
PYTHON_TARBALL = f"python-{PYTHON_VERSION}-aarch64-linux-android.tar.gz"
PYTHON_URL = f"https://www.python.org/ftp/python/{PYTHON_VERSION}/{PYTHON_TARBALL}"
PYTHON_SHA256 = "cfd4f0fdaff465ba62ebf3c3df2c8c410d0802fef3f98897082460df704cfef6"
#: The licences of the libraries the CPython release bundles, which its LICENSE.txt does not
#: hold: (file under --extra, SHA-256, upstream URL at the pinned tag). Doc/license.rst lists
#: the software incorporated into CPython itself (expat, libffi, libmpdec, mimalloc, zlib and
#: the rest); SQLite 3.53.4 is in the public domain.
PYTHON_NOTICES = (
    ("pylic/cpython-license.rst", "0d4c04850f7320219853c3c9d20cf9d2c6916524030e147f220f2b96b5ae8aeb",
     "https://raw.githubusercontent.com/python/cpython/v3.15.2/Doc/license.rst"),
    ("pylic/openssl-LICENSE.txt", "7d5450cb2d142651b8afa315b5f238efc805dad827d91ba367d8516bc9d49e7a",
     "https://raw.githubusercontent.com/openssl/openssl/openssl-3.5.9/LICENSE.txt"),
    ("pylic/bzip2-LICENSE", "c6dbbf828498be844a89eaa3b84adbab3199e342eb5cb2ed2f0d4ba7ec0f38a3",
     "https://sourceware.org/git/?p=bzip2.git;a=blob_plain;f=LICENSE;hb=bzip2-1.0.8"),
    ("pylic/xz-COPYING", "29a1e305b2e34eefe5d4602d00cde1d528b71c5d9f2eec5106972cf6ddb6f73f",
     "https://raw.githubusercontent.com/tukaani-project/xz/v5.4.6/COPYING"),
    ("pylic/zstd-LICENSE", "7055266497633c9025b777c78eb7235af13922117480ed5c674677adc381c9d8",
     "https://raw.githubusercontent.com/facebook/zstd/v1.5.7/LICENSE"),
)
PYTHON_LAUNCHER = Path(__file__).resolve().parent.parent / "android" / "toolchain" / "nk_python_launcher.c"

API = 29
TRIPLE = f"aarch64-unknown-linux-android{API}"
SYSROOT_LIB = f"usr/lib/aarch64-linux-android/{API}"
EXE = ".exe" if os.name == "nt" else ""
LAUNCHER = Path(__file__).resolve().parent.parent / "android" / "toolchain" / "nk_ld_launcher.c"

#: Options every LLVM configure here shares: AArch64 only, no optional libraries, no tests.
LLVM_COMMON = (
    "-DCMAKE_BUILD_TYPE=Release", "-DLLVM_ENABLE_ASSERTIONS=OFF",
    "-DLLVM_ENABLE_PROJECTS=clang;lld", "-DLLVM_TARGETS_TO_BUILD=AArch64",
    f"-DLLVM_DEFAULT_TARGET_TRIPLE={TRIPLE}",
    "-DLLVM_INCLUDE_TESTS=OFF", "-DLLVM_INCLUDE_BENCHMARKS=OFF", "-DLLVM_INCLUDE_EXAMPLES=OFF",
    "-DLLVM_INCLUDE_DOCS=OFF", "-DLLVM_ENABLE_ZLIB=OFF", "-DLLVM_ENABLE_ZSTD=OFF",
    "-DLLVM_ENABLE_LIBXML2=OFF", "-DLLVM_ENABLE_PLUGINS=OFF", "-DCLANG_ENABLE_ARCMT=OFF",
    "-DCLANG_ENABLE_STATIC_ANALYZER=OFF", "-DCLANG_PLUGIN_SUPPORT=OFF",
    "-DLLVM_PARALLEL_LINK_JOBS=1",
)
HOST_TOOLS = ("clang", "lld", "clang-resource-headers", "llvm-ar", "llvm-ranlib", "llvm-readobj",
              "llvm-objcopy", "llvm-nm", "llvm-strip", "llvm-readelf")
RUNTIMES = (
    "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY",
    "-DLLVM_ENABLE_RUNTIMES=compiler-rt;libunwind;libcxxabi;libcxx",
    f"-DLLVM_DEFAULT_TARGET_TRIPLE={TRIPLE}", "-DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF",
    "-DLLVM_INCLUDE_TESTS=OFF", "-DLLVM_INCLUDE_DOCS=OFF",
    "-DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON", "-DCOMPILER_RT_INCLUDE_TESTS=OFF",
    "-DCOMPILER_RT_BUILD_BUILTINS=ON", "-DCOMPILER_RT_BUILD_CRT=OFF",
    "-DCOMPILER_RT_BUILD_SANITIZERS=OFF", "-DCOMPILER_RT_BUILD_XRAY=OFF",
    "-DCOMPILER_RT_BUILD_LIBFUZZER=OFF", "-DCOMPILER_RT_BUILD_PROFILE=OFF",
    "-DCOMPILER_RT_BUILD_MEMPROF=OFF", "-DCOMPILER_RT_BUILD_ORC=OFF",
    "-DCOMPILER_RT_BUILD_CTX_PROFILE=OFF", "-DCOMPILER_RT_BUILD_GWP_ASAN=OFF",
    "-DLIBUNWIND_ENABLE_SHARED=OFF", "-DLIBUNWIND_USE_COMPILER_RT=ON",
    "-DLIBUNWIND_INCLUDE_TESTS=OFF",
    "-DLIBCXXABI_ENABLE_SHARED=OFF", "-DLIBCXXABI_USE_COMPILER_RT=ON",
    "-DLIBCXXABI_USE_LLVM_UNWINDER=ON", "-DLIBCXXABI_INCLUDE_TESTS=OFF",
    "-DLIBCXX_ENABLE_SHARED=OFF", "-DLIBCXX_USE_COMPILER_RT=ON", "-DLIBCXX_INCLUDE_TESTS=OFF",
    "-DLIBCXX_INCLUDE_BENCHMARKS=OFF", "-DLIBCXX_STATICALLY_LINK_ABI_IN_STATIC_LIBRARY=ON",
    "-DLIBCXX_HAS_MUSL_LIBC=OFF", "-DLIBCXX_HAS_PTHREAD_API=ON",
    # Bionic's libc holds pthread, rt and the atomics; Android has no libpthread, librt,
    # libatomic or libgcc. The runtimes' checks cannot tell: a static-library try-compile
    # never links, so each check would pass and libc++.a would ask the linker for them.
    "-DLIBCXX_HAS_PTHREAD_LIB=OFF", "-DLIBCXX_HAS_RT_LIB=OFF", "-DLIBCXX_HAS_ATOMIC_LIB=OFF",
    "-DLIBCXXABI_HAS_PTHREAD_LIB=OFF", "-DLIBUNWIND_HAS_PTHREAD_LIB=OFF",
    "-DCOMPILER_RT_HAS_GCC_LIB=OFF",
)
#: The LLVM projects whose code the shipped binaries hold; each LICENSE.TXT ships with them.
LLVM_LICENSED = ("llvm", "clang", "lld", "compiler-rt", "libunwind", "libcxxabi", "libcxx")
#: Third-party code inside those binaries that keeps its own licence: whole licence files,
#: shipped under the same relative path.
LLVM_THIRD_PARTY_FILES = (
    "llvm/lib/Support/COPYRIGHT.regex",  # Henry Spencer's regex (regcomp/regexec/regerror/regfree)
    "llvm/lib/Support/BLAKE3/LICENSE",  # BLAKE3: CC0-1.0, or Apache-2.0
)
#: ... and notices that live only in a compiled source file's header: (path, first line,
#: last line, text the excerpt must hold). They are joined into THIRD-PARTY-NOTICES.txt.
LLVM_THIRD_PARTY_EXCERPTS = (
    ("libcxx/src/charconv.cpp", 29, 33, "Copyright (c) Microsoft Corporation."),
    ("libcxx/src/ryu/d2fixed.cpp", 9, 37, "Copyright 2018 Ulf Adams"),
    ("libcxx/src/ryu/d2s.cpp", 9, 37, "Copyright 2018 Ulf Adams"),
    ("libcxx/src/ryu/f2s.cpp", 9, 37, "Copyright 2018 Ulf Adams"),
    ("llvm/lib/Support/ConvertUTF.cpp", 9, 44, "Unicode, Inc."),
    ("llvm/lib/Support/MD5.cpp", 1, 38, "Alexander Peslyak"),
    ("llvm/lib/Support/regstrlcpy.c", 1, 17, "Todd C. Miller"),
    ("llvm/lib/Support/UnicodeNameToCodepointGenerated.cpp", 16, 63, "Unicode, Inc."),
    ("llvm/lib/Support/xxhash.cpp", 1, 33, "Yann Collet"),
)
#: The device binaries are linked stripped. lld 21.1.8 does not write .symtab the same way twice:
#: in each link, one local `$x` mapping symbol beside the Cortex-A53 erratum 843419 patches
#: gets a different st_shndx (SHN_UNDEF or .text), with one thread or many. Nothing else
#: differs, but the build ID hashes the whole output, so a later strip would ship different
#: bytes. Linked without .symtab, two links give the same bytes, and the build ID names
#: exactly what ships.
DEVICE_LINK = ("-fuse-ld=lld -rtlib=compiler-rt -unwindlib=libunwind -stdlib=libc++ "
               "-static-libstdc++ -Wl,--build-id=sha1 -Wl,--strip-all")


class ToolchainError(RuntimeError):
    """An input is not the pinned one, or a stage failed; the message says which."""


@dataclass(frozen=True)
class Config:
    llvm_tarball: Path
    bionic: Path
    extra: Path
    work: Path
    cmake: str
    ninja: str
    jobs: int
    host_cc: str
    host_cxx: str
    make_tarball: Path
    sh: str
    python_tarball: Path

    @property
    def src(self) -> Path:
        return self.work / "src" / f"llvm-project-{LLVM_VERSION}.src"

    @property
    def make_src(self) -> Path:
        return self.work / "src" / f"make-{MAKE_VERSION}"

    @property
    def build(self) -> Path:
        """Everything built; apart from the source, so the two prefix maps never nest."""
        return self.work / "build"

    @property
    def host_bin(self) -> Path:
        return self.build / "host" / "bin"

    @property
    def sysroot(self) -> Path:
        return self.build / "sysroot"

    @property
    def resource(self) -> Path:
        return self.build / "resource"

    def tool(self, name: str) -> str:
        return str(self.host_bin / f"{name}{EXE}")

    def prefix_map_args(self) -> list[str]:
        """Build paths out of the binaries: the source and work folders get fixed names."""
        return [f"-ffile-prefix-map={self.src}=/llvm-src", f"-ffile-prefix-map={self.build}=/llvm-build"]

    def prefix_maps(self) -> str:
        """prefix_map_args() as one string, for a CMake flags variable (not for an argv list)."""
        return " ".join(self.prefix_map_args())


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def verify_tarball(path: Path, expected: str = LLVM_SHA256, url: str = LLVM_URL) -> None:
    if not path.is_file():
        raise ToolchainError(f"{path}: missing; fetch {url}")
    actual = sha256_file(path)
    if actual != expected:
        raise ToolchainError(f"{path}: sha256 {actual}, pinned {expected}")


def _done(folder: Path) -> bool:
    return (folder / ".done").is_file()


def _mark(folder: Path) -> None:
    (folder / ".done").write_text("done\n", encoding="utf-8")


def _run(command: list[str], log: Path, what: str, cwd: Path | None = None,
         env: dict[str, str] | None = None) -> None:
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("wb") as handle:
        result = subprocess.run(command, stdout=handle, stderr=subprocess.STDOUT, check=False,
                                cwd=cwd, env=None if env is None else {**os.environ, **env})
    if result.returncode != 0:
        tail = log.read_bytes()[-3000:].decode(errors="replace")
        raise ToolchainError(f"{what} failed ({result.returncode}); see {log}:\n{tail}")


def extract_source(cfg: Config) -> None:
    if _done(cfg.src):
        return
    verify_tarball(cfg.llvm_tarball)
    target = cfg.src.parent
    target.mkdir(parents=True, exist_ok=True)
    with tarfile.open(cfg.llvm_tarball) as tar:
        tar.extractall(target, filter="data")
    if not (cfg.src / "llvm" / "CMakeLists.txt").is_file():
        raise ToolchainError(f"{cfg.llvm_tarball}: no llvm/CMakeLists.txt under {cfg.src.name}")
    _mark(cfg.src)


def host_configure(cfg: Config) -> list[str]:
    maps = cfg.prefix_maps()
    return [
        cfg.cmake, "-G", "Ninja", "-S", str(cfg.src / "llvm"), "-B", str(cfg.build / "host"),
        f"-DCMAKE_MAKE_PROGRAM={cfg.ninja}", f"-DCMAKE_C_COMPILER={cfg.host_cc}",
        f"-DCMAKE_CXX_COMPILER={cfg.host_cxx}", *LLVM_COMMON,
        f"-DCMAKE_C_FLAGS={maps}", f"-DCMAKE_CXX_FLAGS={maps}",
        # MSYS2's GCC runtime DLLs stay out of the host tools.
        *(["-DCMAKE_EXE_LINKER_FLAGS=-static-libgcc -static-libstdc++"] if os.name == "nt" else []),
    ]


def cross_common(cfg: Config) -> list[str]:
    """CMake options for building aarch64-linux-android code with the host clang."""
    return [
        "-DCMAKE_SYSTEM_NAME=Linux", "-DCMAKE_SYSTEM_PROCESSOR=aarch64",
        f"-DCMAKE_C_COMPILER={cfg.tool('clang')}", f"-DCMAKE_CXX_COMPILER={cfg.tool('clang++')}",
        f"-DCMAKE_ASM_COMPILER={cfg.tool('clang')}",
        f"-DCMAKE_C_COMPILER_TARGET={TRIPLE}", f"-DCMAKE_CXX_COMPILER_TARGET={TRIPLE}",
        f"-DCMAKE_ASM_COMPILER_TARGET={TRIPLE}", f"-DCMAKE_SYSROOT={cfg.sysroot}",
        f"-DCMAKE_AR={cfg.tool('llvm-ar')}", f"-DCMAKE_RANLIB={cfg.tool('llvm-ranlib')}",
        f"-DCMAKE_NM={cfg.tool('llvm-nm')}", f"-DCMAKE_STRIP={cfg.tool('llvm-strip')}",
        f"-DCMAKE_OBJCOPY={cfg.tool('llvm-objcopy')}", "-DCMAKE_LINKER_TYPE=LLD",
        f"-DCMAKE_MAKE_PROGRAM={cfg.ninja}",
    ]


def cross_flags(cfg: Config) -> str:
    return f"--target={TRIPLE} --sysroot={cfg.sysroot} -resource-dir={cfg.resource} {cfg.prefix_maps()}"


def runtimes_configure(cfg: Config) -> list[str]:
    flags = cross_flags(cfg)
    return [
        cfg.cmake, "-G", "Ninja", "-S", str(cfg.src / "runtimes"), "-B", str(cfg.build / "rt"),
        *cross_common(cfg), *RUNTIMES,
        f"-DCMAKE_C_FLAGS={flags}", f"-DCMAKE_CXX_FLAGS={flags}", f"-DCMAKE_ASM_FLAGS={flags}",
        f"-DCMAKE_INSTALL_PREFIX={cfg.build / 'rt-install'}",
    ]


def device_configure(cfg: Config) -> list[str]:
    flags = cross_flags(cfg)
    return [
        cfg.cmake, "-G", "Ninja", "-S", str(cfg.src / "llvm"), "-B", str(cfg.build / "android"),
        *cross_common(cfg), *LLVM_COMMON,
        f"-DCMAKE_C_FLAGS={flags}", f"-DCMAKE_CXX_FLAGS={flags} -stdlib=libc++",
        f"-DCMAKE_EXE_LINKER_FLAGS={DEVICE_LINK}", f"-DCMAKE_SHARED_LINKER_FLAGS={DEVICE_LINK}",
        f"-DLLVM_HOST_TRIPLE={TRIPLE}", f"-DLLVM_NATIVE_TOOL_DIR={cfg.host_bin}",
        "-DLLVM_ENABLE_LIBCXX=ON",
    ]


def check_no_ndk_runtime(data: bytes, binary: str) -> None:
    """Refuse a binary holding the NDK's libc++, whose inline namespace is std::__ndk1.

    The binaries have no symbol table, but libc++'s type names stay in .rodata: the
    project's are `NSt3__1...`, the NDK's `NSt6__ndk1...`.
    """
    ndk = data.count(b"__ndk1")
    if ndk:
        raise ToolchainError(f"{binary}: {ndk} names from the NDK's libc++ (std::__ndk1)")
    if b"NSt3__1" not in data:
        raise ToolchainError(f"{binary}: no std::__1 type names, so not linked with the project's libc++")


def stage_host(cfg: Config) -> None:
    folder = cfg.build / "host"
    if _done(folder):
        return
    _run(host_configure(cfg), cfg.work / "logs" / "host-configure.log", "host configure")
    _run([cfg.ninja, "-C", str(folder), f"-j{cfg.jobs}", *HOST_TOOLS],
         cfg.work / "logs" / "host-build.log", "host build")
    _mark(folder)


def stage_sysroot(cfg: Config) -> None:
    if _done(cfg.sysroot):
        return
    tar_path = cfg.build / "sysroot.tar"
    files = android_sysroot.read_bionic(cfg.bionic)
    extra = android_sysroot.read_extra(cfg.extra, android_sysroot.load_pins())
    headers = android_sysroot.header_entries(files, android_sysroot.extra_header_entries(extra))
    scratch = cfg.build / "sysroot-scratch"
    if scratch.exists():
        shutil.rmtree(scratch)
    scratch.mkdir(parents=True)
    objects = android_sysroot.build_objects(files, headers, Path(cfg.tool("clang")), API, scratch,
                                            android_sysroot.stub_inputs(files, extra))
    shutil.rmtree(scratch)
    entries = {**headers, **objects, **android_sysroot.notice_entries(files, extra)}
    android_sysroot.write_archive(entries, tar_path)
    # The build reads an unpacked copy. A case-insensitive file system keeps one of each
    # case-only pair of kernel headers; none of them is read here.
    skip = android_sysroot.case_collisions(list(entries))
    for path, entry in entries.items():
        if path in skip:
            continue
        target = cfg.sysroot / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(entry.data)
    _mark(cfg.sysroot)


def stage_runtimes(cfg: Config) -> None:
    folder = cfg.build / "rt"
    if _done(folder):
        return
    if cfg.resource.exists():
        shutil.rmtree(cfg.resource)
    shutil.copytree(cfg.build / "host" / "lib" / "clang" / CLANG_MAJOR / "include",
                    cfg.resource / "include")
    _run(runtimes_configure(cfg), cfg.work / "logs" / "rt-configure.log", "runtimes configure")
    _run([cfg.ninja, "-C", str(folder), f"-j{cfg.jobs}", "install"],
         cfg.work / "logs" / "rt-build.log", "runtimes build")
    install = cfg.build / "rt-install"
    builtins = sorted(folder.rglob("libclang_rt.builtins*aarch64*.a"))
    if len(builtins) != 1:
        raise ToolchainError(f"expected one builtins library under {folder}, found {len(builtins)}")
    linux = cfg.resource / "lib" / "linux"
    (linux / "aarch64").mkdir(parents=True)
    shutil.copyfile(builtins[0], linux / "libclang_rt.builtins-aarch64-android.a")
    shutil.copyfile(install / "lib" / "libunwind.a", linux / "aarch64" / "libunwind.a")
    # libc++ goes where clang's Android driver looks for it: the sysroot. Only the device
    # build reads it; the shipped sysroot tar is C only.
    shutil.copytree(install / "include" / "c++" / "v1", cfg.sysroot / "usr" / "include" / "c++" / "v1")
    shutil.copyfile(install / "lib" / "libc++.a", cfg.sysroot / SYSROOT_LIB / "libc++.a")
    _mark(folder)


def stage_device(cfg: Config) -> None:
    folder = cfg.build / "android"
    if _done(folder):
        return
    _run(device_configure(cfg), cfg.work / "logs" / "device-configure.log", "device configure")
    _run([cfg.ninja, "-C", str(folder), f"-j{cfg.jobs}", "clang", "lld", "clang-resource-headers"],
         cfg.work / "logs" / "device-build.log", "device build")
    for name in ("clang", "lld"):
        binary = (folder / "bin" / name).resolve()
        check_no_ndk_runtime(binary.read_bytes(), str(binary))
    _mark(folder)


def make_configure(cfg: Config) -> tuple[list[str], dict[str, str]]:
    """GNU make's configure, run in build/make, compiling with the host clang for the device."""
    folder = cfg.build / "make"
    source = Path(os.path.relpath(cfg.make_src, folder)).as_posix()
    cc = (f"{Path(cfg.tool('clang')).as_posix()} --target={TRIPLE} --sysroot={cfg.sysroot.as_posix()} "
          f"-resource-dir={cfg.resource.as_posix()}")
    command = [cfg.sh, f"{source}/configure", *MAKE_CONFIGURE, f"CC={cc}",
               # configure is run by a relative path, so the compiler sees relative sources.
               f"CFLAGS=-O2 -ffile-prefix-map={source}=/make-src",
               "LDFLAGS=-fuse-ld=lld -pie -Wl,--build-id=sha1 -Wl,--strip-all"]
    # MSYS2's sh would turn the POSIX paths configure writes for the device, such as
    # /usr/local/include, into Windows paths; elsewhere the variable means nothing.
    env = {"MSYS2_ARG_CONV_EXCL": "*"}
    shell = Path(cfg.sh)
    if shell.parent != Path("."):
        # configure's expr, sed and the rest must be the ones beside that shell: another
        # POSIX layer's tools on PATH (Git for Windows', say) misparse its options.
        env["PATH"] = f"{shell.parent}{os.pathsep}{os.environ.get('PATH', '')}"
    return command, env


def check_device_executable(data: bytes, binary: str, work: Path) -> None:
    """A position-independent AArch64 ELF that names no build-machine path."""
    if data[:5] != b"\x7fELF\x02" or int.from_bytes(data[16:18], "little") != 3 \
            or int.from_bytes(data[18:20], "little") != 183:
        raise ToolchainError(f"{binary}: not a position-independent AArch64 ELF")
    for path in {work.as_posix(), str(work)}:
        if path.encode() in data:
            raise ToolchainError(f"{binary}: holds the build path {path}")


def stage_make(cfg: Config) -> None:
    """GNU make for the device: configure, then make's own build.sh, which needs no make."""
    folder = cfg.build / "make"
    if _done(folder):
        return
    if not _done(cfg.make_src):
        verify_tarball(cfg.make_tarball, MAKE_SHA256, MAKE_URL)
        cfg.make_src.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(cfg.make_tarball) as tar:
            tar.extractall(cfg.make_src.parent, filter="data")
        if not (cfg.make_src / "configure").is_file():
            raise ToolchainError(f"{cfg.make_tarball}: no configure under {cfg.make_src.name}")
        _mark(cfg.make_src)
    folder.mkdir(parents=True, exist_ok=True)
    command, env = make_configure(cfg)
    _run(command, cfg.work / "logs" / "make-configure.log", "make configure", folder, env)
    _run([cfg.sh, "./build.sh"], cfg.work / "logs" / "make-build.log", "make build", folder, env)
    check_device_executable((folder / "make").read_bytes(), str(folder / "make"), cfg.work)
    _mark(folder)


def make_source_notice(reference: dict[str, object]) -> str:
    """NOTICES/make/SOURCE.txt: where make's complete corresponding source is (GPL-3.0 section 6).

    *reference* is package_notices.source_reference(): the repository and commit that hold the
    scripts controlling make's build, as the desktop packages' SOURCE.txt names them.
    """
    commit = reference.get("commit") or "unknown (built without git metadata)"
    lines = [
        "GNU MAKE: COMPLETE CORRESPONDING SOURCE",
        "",
        f"libnk_make.so in this app is GNU make {MAKE_VERSION}, licensed under the GNU General",
        "Public License version 3 or later (COPYING, beside this file). It is built, unmodified,",
        "from this release tarball:",
        "",
        f"  File:    {MAKE_TARBALL}",
        f"  From:    {MAKE_URL}",
        f"  SHA-256: {MAKE_SHA256}",
        "",
        "It is configured with make's own configure and these options:",
        "",
        *(f"  {option}" for option in MAKE_CONFIGURE),
        "",
        "and compiled with make's own build.sh. The scripts that control the build are part of",
        "Nakagawa Recomp: tools/android_toolchain.py (the configure options, the compiler and its",
        "CC, CFLAGS and LDFLAGS) and tools/android_sysroot.py (the sysroot make is compiled and",
        "linked against), at:",
        "",
        f"  Repository: {reference.get('repository')}",
        f"  Commit:     {commit}",
    ]
    if reference.get("local_changes"):
        lines.extend([
            "",
            "This build was made from a working tree with local changes to tracked files; its",
            "scripts are the commit above plus those changes.",
        ])
    return "\n".join(lines) + "\n"


def stage_make_notices(make_src: Path, notices: Path, reference: dict[str, object]) -> None:
    """make's licence and source statement under *notices*/make/."""
    folder = notices / "make"
    folder.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(make_src / "COPYING", folder / "COPYING")
    (folder / "SOURCE.txt").write_text(make_source_notice(reference), encoding="utf-8", newline="\n")


@dataclass(frozen=True)
class PythonPackage:
    """What the APK takes from the CPython release, by path."""
    libraries: dict[str, bytes]  # for the native library folder: libpython and the *_python libraries
    headers: dict[str, bytes]  # include/python3.15/..., to compile the launcher
    stdlib: dict[str, android_sysroot.Entry]  # lib/python3.15/..., without __pycache__
    license: bytes


def read_python_package(tarball: Path, expected: str = PYTHON_SHA256) -> PythonPackage:
    """The CPython release's parts, read from the checked tarball without extracting it."""
    verify_tarball(tarball, expected, PYTHON_URL)
    libraries: dict[str, bytes] = {}
    headers: dict[str, bytes] = {}
    stdlib: dict[str, android_sysroot.Entry] = {}
    stdlib_root = f"lib/python{PYTHON_MINOR}/"
    with tarfile.open(tarball) as tar:
        for member in tar.getmembers():
            name = member.name.removeprefix("./")
            if not name.startswith("prefix/") or member.isdir():
                continue
            path = name.removeprefix("prefix/")
            folder, _, base = path.rpartition("/")
            is_library = folder == "lib" and (base == f"libpython{PYTHON_MINOR}.so"
                                              or (base.startswith("lib") and base.endswith("_python.so")))
            is_header = path.startswith(f"include/python{PYTHON_MINOR}/")
            is_stdlib = path.startswith(stdlib_root) and "__pycache__" not in path.split("/")
            if not (is_library or is_header or is_stdlib):
                continue
            if not member.isfile():
                raise ToolchainError(f"{tarball}: {name} is not a regular file")
            handle = tar.extractfile(member)
            if handle is None:
                raise ToolchainError(f"{tarball}: cannot read {name}")
            data = handle.read()
            if is_library:
                libraries[base] = data
            elif is_header:
                headers[path] = data
            else:
                stdlib[path] = android_sysroot.Entry(data, executable=path.endswith(".so"))
    license_path = f"{stdlib_root}LICENSE.txt"
    if f"libpython{PYTHON_MINOR}.so" not in libraries or license_path not in stdlib \
            or f"include/python{PYTHON_MINOR}/Python.h" not in headers:
        raise ToolchainError(f"{tarball}: not the CPython {PYTHON_VERSION} Android release layout")
    return PythonPackage(libraries, headers, stdlib, stdlib[license_path].data)


def read_pinned(path: Path, expected: str, url: str) -> bytes:
    if not path.is_file():
        raise ToolchainError(f"{path}: missing; fetch {url}")
    data = path.read_bytes()
    actual = hashlib.sha256(data).hexdigest()
    if actual != expected:
        raise ToolchainError(f"{path}: sha256 {actual}, pinned {expected}")
    return data


def python_launcher_command(cfg: Config, libraries: list[str]) -> list[str]:
    """The link of libnk_python.so against the release's libpython and the libraries it bundles.

    A program the app starts is linked by the system's default search path, not the app's
    native library folder, so the launcher looks beside itself ($ORIGIN). It also needs every
    lib*_python.so: the extension modules in lib-dynload name them but have no search path of
    their own, and the linker finds a library that is already loaded by its name.
    """
    folder = cfg.build / "python"
    return [cfg.tool("clang"), f"--target={TRIPLE}", f"--sysroot={cfg.sysroot}",
            f"-resource-dir={cfg.resource}", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            f"-I{folder / 'include' / f'python{PYTHON_MINOR}'}", "-fPIE", "-pie", "-fuse-ld=lld",
            "-Wl,--build-id=sha1", "-s", "-Wl,-rpath,$ORIGIN", *cfg.prefix_map_args(),
            "-o", str(folder / "libnk_python.so"), str(PYTHON_LAUNCHER), f"-L{folder / 'lib'}",
            "-Wl,--no-as-needed", *(f"-l:{name}" for name in libraries), "-Wl,--as-needed"]


def stage_python(cfg: Config) -> None:
    """The CPython release's libraries and standard library, and the launcher built against them."""
    folder = cfg.build / "python"
    if _done(folder):
        return
    package = read_python_package(cfg.python_tarball)
    files = [*((f"lib/{name}", data) for name, data in package.libraries.items()),
             *package.headers.items(), ("LICENSE.txt", package.license)]
    for relative, data in files:
        (folder / relative).parent.mkdir(parents=True, exist_ok=True)
        (folder / relative).write_bytes(data)
    android_sysroot.write_archive(package.stdlib, folder / "python-stdlib.tar")
    _run(python_launcher_command(cfg, sorted(package.libraries)),
         cfg.work / "logs" / "python-launcher.log", "building the python launcher")
    check_device_executable((folder / "libnk_python.so").read_bytes(), "libnk_python.so", cfg.work)
    _mark(folder)


def _tree_entries(root: Path, prefix: str) -> dict[str, android_sysroot.Entry]:
    return {f"{prefix}/{path.relative_to(root).as_posix()}": android_sysroot.Entry(path.read_bytes())
            for path in sorted(root.rglob("*")) if path.is_file()}


def third_party_notices(src: Path) -> str:
    """The licence notices held only in compiled files' headers, each under its source path."""
    parts = [f"Third-party notices inside the LLVM {LLVM_VERSION} code in libnk_clang.so, libnk_lld.so\n"
             "and the runtimes, copied from the headers of the files that hold that code.\n"]
    for relative, first, last, marker in LLVM_THIRD_PARTY_EXCERPTS:
        lines = (src / relative).read_text(encoding="utf-8").splitlines()
        excerpt = "\n".join(lines[first - 1:last])
        if last > len(lines) or marker not in excerpt:
            raise ToolchainError(f"{relative} lines {first}-{last} no longer hold the notice {marker!r}")
        parts.append(f"\n==== {relative}, lines {first}-{last} ====\n\n{excerpt}\n")
    return "".join(parts)


def stage_output(cfg: Config, out: Path) -> dict[str, str]:
    """The APK's files under *out*; returns {path: sha256}."""
    libs = out / "jniLibs" / "arm64-v8a"
    assets = out / "assets" / "toolchain"
    libs.mkdir(parents=True, exist_ok=True)
    assets.mkdir(parents=True, exist_ok=True)
    device = cfg.build / "android"
    for name, target in (("clang", "libnk_clang.so"), ("lld", "libnk_lld.so")):
        shutil.copyfile((device / "bin" / name).resolve(), libs / target)  # linked stripped
    shutil.copyfile(cfg.build / "make" / "make", libs / "libnk_make.so")
    python = cfg.build / "python"
    for library in sorted((python / "lib").glob("*.so")):
        shutil.copyfile(library, libs / library.name)  # the CPython release's own binaries
    shutil.copyfile(python / "libnk_python.so", libs / "libnk_python.so")
    shutil.copyfile(python / "python-stdlib.tar", assets / "python-stdlib.tar")
    (assets / "NOTICES" / "python").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(python / "LICENSE.txt", assets / "NOTICES" / "python" / "LICENSE.txt")
    for relative, digest, url in PYTHON_NOTICES:
        name = relative.removeprefix("pylic/")
        (assets / "NOTICES" / "python" / name).write_bytes(read_pinned(cfg.extra / relative, digest, url))
    _run([cfg.tool("clang"), f"--target={TRIPLE}", f"--sysroot={cfg.sysroot}",
          f"-resource-dir={cfg.resource}", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
          "-fPIE", "-pie", "-fuse-ld=lld", "-Wl,--build-id=sha1", "-s", *cfg.prefix_map_args(),
          "-o", str(libs / "libnk_ld.so"), str(LAUNCHER)],
         cfg.work / "logs" / "launcher.log", "building the ld launcher")
    shutil.copyfile(cfg.build / "sysroot.tar", assets / "sysroot.tar")
    import package_notices  # the desktop packages' source reference; imported only to stage
    stage_make_notices(cfg.make_src, assets / "NOTICES", package_notices.source_reference())
    notices = assets / "NOTICES" / "llvm"
    for project in LLVM_LICENSED:
        (notices / project).mkdir(parents=True, exist_ok=True)
        shutil.copyfile(cfg.src / project / "LICENSE.TXT", notices / project / "LICENSE.TXT")
    for relative in LLVM_THIRD_PARTY_FILES:
        (notices / relative).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(cfg.src / relative, notices / relative)
    (notices / "THIRD-PARTY-NOTICES.txt").write_text(third_party_notices(cfg.src),
                                                    encoding="utf-8", newline="\n")
    resource = _tree_entries(device / "lib" / "clang" / CLANG_MAJOR / "include", "include")
    resource.update(_tree_entries(cfg.resource / "lib", "lib"))
    android_sysroot.write_archive(resource, assets / "clang-resource.tar")
    manifest = {path.relative_to(out).as_posix(): sha256_file(path)
                for path in sorted(out.rglob("*")) if path.is_file()
                and path.name != "toolchain-manifest.json"}
    document = {"llvm": LLVM_VERSION, "llvm_sha256": LLVM_SHA256,
                "make": MAKE_VERSION, "make_sha256": MAKE_SHA256,
                "python": PYTHON_VERSION, "python_sha256": PYTHON_SHA256,
                "bionic": android_sysroot.BIONIC_COMMIT, "api": API, "files": manifest}
    (out / "toolchain-manifest.json").write_text(json.dumps(document, indent=2) + "\n",
                                                 encoding="utf-8", newline="\n")
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--llvm-tarball", required=True, type=Path,
                        help=f"{LLVM_TARBALL} from {LLVM_URL}")
    parser.add_argument("--bionic", required=True, type=Path,
                        help=f"a clone of {android_sysroot.BIONIC_URL} holding {android_sysroot.BIONIC_TAG}")
    parser.add_argument("--extra", required=True, type=Path,
                        help="the files android/toolchain/sysroot-inputs.json pins")
    parser.add_argument("--work", required=True, type=Path, help="build folder (several GB)")
    parser.add_argument("--out", required=True, type=Path, help="where the APK's files go")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--ninja", default="ninja")
    parser.add_argument("--host-cc", default="gcc")
    parser.add_argument("--host-cxx", default="g++")
    parser.add_argument("--make-tarball", required=True, type=Path,
                        help=f"{MAKE_TARBALL} from {MAKE_URL}")
    parser.add_argument("--python-tarball", required=True, type=Path,
                        help=f"{PYTHON_TARBALL} from {PYTHON_URL}")
    parser.add_argument("--sh", default="sh", help="the POSIX shell that runs make's configure")
    parser.add_argument("--jobs", type=int, default=4, help="compile jobs (each can take 1 GB)")
    args = parser.parse_args(argv)
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")
    cfg = Config(args.llvm_tarball.resolve(), args.bionic.resolve(), args.extra.resolve(),
                 args.work.resolve(), args.cmake, args.ninja, args.jobs, args.host_cc, args.host_cxx,
                 args.make_tarball.resolve(), args.sh, args.python_tarball.resolve())
    try:
        extract_source(cfg)
        stage_host(cfg)
        stage_sysroot(cfg)
        stage_runtimes(cfg)
        stage_device(cfg)
        stage_make(cfg)
        stage_python(cfg)
        manifest = stage_output(cfg, args.out.resolve())
    except (OSError, subprocess.CalledProcessError, ToolchainError, android_sysroot.SysrootError,
            android_sysroot.android_stubgen.SymbolFileError) as error:
        print(f"android_toolchain: {error}", file=sys.stderr)
        return 1
    for path, digest in manifest.items():
        print(f"android_toolchain: {digest}  {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
