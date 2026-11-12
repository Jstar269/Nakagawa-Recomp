# PROVENANCE.md — `android/toolchain/` (the on-device C toolchain)

The Android build of Nakagawa compiles a game's generated C on the device (issue #360), so the APK ships a C compiler,
a linker, `make`, a Python interpreter and a sysroot for `aarch64-linux-android29`. Nothing in them comes from the
Android NDK. Python is the official CPython release for Android, shipped unchanged; every other part is built by the
project from a pinned upstream source. This repository holds only the pins, the build tools and their tests. No upstream
source file is vendored here.

## Project-authored files

Written for this project with Claude Code (Claude Opus 5.5) from the documented formats, GPL-3.0-or-later:

- `nk_ld_launcher.c`: starts `libnk_lld.so` as `ld.lld`. Android runs only `lib*.so` files from an app's native library
  folder, and lld chooses its role from its program name.
- `nk_python_launcher.c`: `libnk_python.so`, the `python` program, which the CPython release for Android does not
  include. It runs Python's own command line (`Py_BytesMain`).
- `sysroot-inputs.json`: the pins below.
- `tools/android_stubgen.py`, `tools/android_sysroot.py` and `tools/android_toolchain.py`, with their tests: the stub
  generator for AOSP symbol files (`*.map.txt`), the sysroot builder and the toolchain build driver.

## Sysroot (built from pinned AOSP and Khronos files)

`tools/android_sysroot.py` builds one tar archive with fixed timestamps, owners and modes. Two builds from the same
inputs give the same bytes. Each input is checked before use: bionic by its commit, and every other file by the
SHA-256 in `sysroot-inputs.json` (which also records each file's git blob ID).

| Source | Revision | Used | Licence |
| --- | --- | --- | --- |
| [AOSP bionic](https://android.googlesource.com/platform/bionic) | `android-16.0.0_r4`, `f22516cbc67e81c13cf943ce02f01b8929a98726` | `libc/include`, `libc/kernel/uapi` (arm64 and generic), `libc/kernel/android/uapi`, the crt sources in `libc/arch-common/bionic`, and the libc, libm and libdl symbol files | BSD-style and Apache-2.0 (`libc/NOTICE`); the kernel uapi headers are GPL-2.0-only WITH Linux-syscall-note |
| [AOSP system/logging](https://android.googlesource.com/platform/system/logging) | `android-16.0.0_r4`, `58dac317ea08b8f275ebd01ce864acab2de30b93` | `liblog/include/android/log.h`, `liblog/liblog.map.txt` | Apache-2.0 |
| [AOSP frameworks/base](https://android.googlesource.com/platform/frameworks/base) | `android-16.0.0_r4`, `45034f0663f960d9ee5fb0a101a4732b71f6e2f4` | `native/android/libandroid.map.txt` | Apache-2.0 |
| [AOSP frameworks/native](https://android.googlesource.com/platform/frameworks/native) | `android-16.0.0_r4`, `fcbde2bcff56ca1d7bec9cabf8bdca5e288bf103` | `vulkan/libvulkan/libvulkan.map.txt` | Apache-2.0 |
| [Khronos Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | `v1.4.363`, `6802bb4733b63ed5efd3adb308a6c885ef180ea1` | `include/vulkan/*.h`, `include/vk_video/*.h` | Apache-2.0 OR MIT |
| [SPDX license-list-data](https://github.com/spdx/license-list-data) | `v3.29.0`, `31ba1a50e5397e00a304dbadc76531740e89ee48` | `text/GPL-2.0-only.txt`, `text/Linux-syscall-note.txt` | the licence texts themselves |

What the archive holds:

- headers, laid out as the NDK lays them out: bionic's, the kernel's (`asm-arm64/asm` under
  `aarch64-linux-android/asm`), `android/log.h`, and the Vulkan headers;
- crt objects (`crtbegin_dynamic.o`, `crtbegin_so.o`, `crtend_android.o`, `crtend_so.o`, `crt_pad_segment.o`)
  compiled from bionic's sources by the project's clang;
- stub shared libraries generated from the symbol files: `libc.so`, `libm.so` and `libdl.so` versioned, and `liblog.so`,
  `libandroid.so` and `libvulkan.so` unversioned, as the NDK ships them. A stub holds only symbol definitions; the
  device's own libraries run.
- `NOTICES/`: bionic's `libc/NOTICE`, `libm/NOTICE` and `libdl/NOTICE`, each AOSP project's `NOTICE`, Vulkan-Headers'
  `LICENSE.md` and `LICENSES/`, and the GPL-2.0-only and Linux-syscall-note texts for the kernel headers.

The stubs were checked against the NDK r30 stubs as an oracle, not copied from them: for arm64 they match in every
exported name, symbol type and binding at each API level from 21 (libvulkan from 24) to 36.

## Compiler, linker and runtimes

| Source | Revision | Built | Licence |
| --- | --- | --- | --- |
| [LLVM](https://github.com/llvm/llvm-project) | `llvmorg-21.1.8`, release tarball `llvm-project-21.1.8.src.tar.xz`, SHA-256 `4633a23617fa31a3ea51242586ea7fb1da7140e426bd62fc164261fe036aa142` | clang and lld (AArch64 target only); compiler-rt builtins, libunwind, libc++abi and libc++, all static | Apache-2.0 WITH LLVM-exception |

`tools/android_toolchain.py` checks the tarball's SHA-256 and builds the same source three times. First it builds a
host clang and lld with the build machine's C++ compiler (MSYS2 GCC on Windows). That host clang then builds the
runtimes for `aarch64-linux-android29`. Last, it builds the device's clang and lld, linked against those runtimes. The
build refuses a device binary that holds the NDK's libc++, whose symbols are in `std::__ndk1`. The device clang needs
only `libc.so`, `libm.so` and `libdl.so`.

What the APK carries, from `tools/android_toolchain.py`'s staging step:

- `jniLibs/arm64-v8a/libnk_clang.so` and `libnk_lld.so` (the device binaries, linked without a symbol table so that
  two builds give the same bytes; a crash trace from them therefore shows only addresses, and symbolizing one needs a
  relink of the same build folder without `--strip-all`) and `libnk_ld.so` (the launcher);
- `assets/toolchain/sysroot.tar` (above) and `clang-resource.tar` (clang's own headers, the builtins and libunwind);
- `assets/toolchain/NOTICES/llvm/<project>/LICENSE.TXT` for llvm, clang, lld, compiler-rt, libunwind, libcxxabi and
  libcxx, copied from the checked tarball;
- the notices of the third-party code compiled into those binaries, from the same tarball:
  `NOTICES/llvm/llvm/lib/Support/COPYRIGHT.regex` (Henry Spencer's regex library, BSD-3-Clause),
  `NOTICES/llvm/llvm/lib/Support/BLAKE3/LICENSE` (BLAKE3, CC0-1.0 or Apache-2.0), and
  `NOTICES/llvm/THIRD-PARTY-NOTICES.txt`, the licence headers of the files that carry their own notice: libc++'s
  Ryu and `charconv.cpp` (Ulf Adams, Boost Software License 1.0; Microsoft, Apache-2.0 WITH LLVM-exception),
  `ConvertUTF.cpp` and `UnicodeNameToCodepointGenerated.cpp` (Unicode licence), `MD5.cpp` (Alexander Peslyak, public
  domain or a cut-down BSD licence), `regstrlcpy.c` (Todd C. Miller, ISC) and `xxhash.cpp` (Yann Collet,
  BSD-2-Clause). The build refuses a tarball whose pinned lines no longer hold those notices;
- `jniLibs/arm64-v8a/libnk_make.so` (GNU make, below) and `assets/toolchain/NOTICES/make/COPYING`;
- `toolchain-manifest.json`, the SHA-256 of every staged file.

## GNU make

| Source | Revision | Built | Licence |
| --- | --- | --- | --- |
| [GNU make](https://www.gnu.org/software/make/) | 4.4.1, release tarball `make-4.4.1.tar.gz` from `https://ftp.gnu.org/gnu/make/`, SHA-256 `dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3` | `make`, for `aarch64-linux-android29` | GPL-3.0-or-later |

`tools/android_toolchain.py` checks the tarball's SHA-256, runs make's `configure` for the device with the host clang
and the sysroot above, and compiles make with make's own `build.sh`, so the build needs no make of its own. The options
are make's own: no NLS, Guile or `load`, and no `posix_spawn`, because bionic has no `confstr()` and make's
`posix_spawn` path calls it to find the default `PATH`, so make uses fork and exec instead. The binary is linked
position-independent and stripped, and the build refuses one that is not an AArch64 PIE or that names the build
folder. Two builds in different folders give the same bytes. make needs only `libc.so` and `libdl.so`.

make is GPL-3.0-or-later, so the release that ships the APK also carries its corresponding source: the pinned
`make-4.4.1.tar.gz` and `tools/android_toolchain.py` at the release's revision, which holds make's configure and build
settings. make is the toolchain's only GPL program. The APK itself carries `NOTICES/make/SOURCE.txt` beside make's
`COPYING`: the tarball's name, URL and SHA-256, the configure options, and the repository and commit of
`tools/android_toolchain.py` and `tools/android_sysroot.py`.

## Python

| Source | Revision | Used | Licence |
| --- | --- | --- | --- |
| [CPython](https://www.python.org/downloads/android/) | 3.15.2, the official release `python-3.15.2-aarch64-linux-android.tar.gz` from `https://www.python.org/ftp/python/3.15.2/`, SHA-256 `cfd4f0fdaff465ba62ebf3c3df2c8c410d0802fef3f98897082460df704cfef6` | `prefix/lib/libpython3.15.so` and the `lib*_python.so` libraries it bundles (OpenSSL 3.5.9 and SQLite 3.53.4), unchanged; the standard library `prefix/lib/python3.15` without `__pycache__`; the headers, to compile the launcher | PSF-2.0; OpenSSL Apache-2.0; SQLite public domain |
| [CPython](https://github.com/python/cpython) | `v3.15.2`, `5e58eaad7c58df58dfa8ea549c87996e01dd1077` | `Doc/license.rst`, SHA-256 `0d4c04850f7320219853c3c9d20cf9d2c6916524030e147f220f2b96b5ae8aeb`: the licences of the software incorporated into CPython (expat, libffi, libmpdec, mimalloc, zlib and the rest) | the licence texts themselves |
| [OpenSSL](https://github.com/openssl/openssl) | `openssl-3.5.9`, `45e844fa2a14ec92d146bd8f5778ac130b6625fb` | `LICENSE.txt`, SHA-256 `7d5450cb2d142651b8afa315b5f238efc805dad827d91ba367d8516bc9d49e7a` | Apache-2.0 |
| [bzip2](https://sourceware.org/git/bzip2.git) | `bzip2-1.0.8`, `6a8690fc8d26c815e798c588f796eabe9d684cf0` | `LICENSE`, SHA-256 `c6dbbf828498be844a89eaa3b84adbab3199e342eb5cb2ed2f0d4ba7ec0f38a3` (the `_bz2` module) | bzip2-1.0.6 |
| [XZ Utils](https://github.com/tukaani-project/xz) | `v5.4.6`, `6e8732c5a317a349986a4078718f1d95b67072c5` | `COPYING`, SHA-256 `29a1e305b2e34eefe5d4602d00cde1d528b71c5d9f2eec5106972cf6ddb6f73f` (the `_lzma` module) | public domain (liblzma) |
| [Zstandard](https://github.com/facebook/zstd) | `v1.5.7`, `f8745da6ff1ad1e7bab384bd1f9d742439278e99` | `LICENSE`, SHA-256 `7055266497633c9025b777c78eb7235af13922117480ed5c674677adc381c9d8` (the `_zstd` module) | BSD-3-Clause |

The library versions are the ones the release's binaries report. `tools/android_toolchain.py` checks every SHA-256
above, reads the release tarball without extracting it, and refuses one whose files are not where the release puts
them or are links. It builds `libnk_python.so` from `nk_python_launcher.c` with the host clang against the release's
headers and libraries. A program the app starts is linked by the system's default search path, not the app's native
library folder, so the launcher has `DT_RUNPATH` `$ORIGIN`. It also names every `lib*_python.so` as needed: the extension
modules in `lib-dynload` need them but have no search path of their own, and the dynamic linker finds an already-loaded
library by its name. Besides those, it needs only `libc.so`. The standard library becomes one tar with fixed metadata,
and two builds give the same bytes. The APK carries these libraries byte for byte as staged: Gradle keeps the NDK's strip
away from them.

What the APK carries for Python:

- `jniLibs/arm64-v8a/libnk_python.so`, `libpython3.15.so`, `libssl_python.so`, `libcrypto_python.so` and
  `libsqlite3_python.so`;
- `assets/toolchain/python-stdlib.tar` (`lib/python3.15/`, with its extension modules in `lib-dynload/`), which the
  app unpacks; the caller sets `PYTHONHOME` to the folder that holds it;
- `assets/toolchain/NOTICES/python/`: the release's `LICENSE.txt` and the licence files in the table.
