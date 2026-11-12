# PROVENANCE.md — `android/toolchain/` (the on-device C toolchain)

The Android build of Nakagawa compiles a game's generated C on the device (issue #360), so the APK ships a C compiler,
a linker and a sysroot for `aarch64-linux-android29`. Nothing in them comes from the Android NDK: each part is built by
the project from a pinned upstream source, and this repository holds only the pins, the build tools and their tests. No
upstream source file is vendored here.

## Project-authored files

Written for this project with Claude Code (Claude Opus 5.5) from the documented formats, GPL-3.0-or-later:

- `nk_ld_launcher.c`: starts `libnk_lld.so` as `ld.lld`. Android runs only `lib*.so` files from an app's native library
  folder, and lld chooses its role from its program name.
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
- `toolchain-manifest.json`, the SHA-256 of every staged file.
