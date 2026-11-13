# Android development

Status: the launcher and the source-owned showcase demos build into one APK for arm64-v8a and
x86_64 and run in the Android emulator. Retail games, importing a disc on the device and a
supported-device list are not available ([#360](https://github.com/Jstar269/nakagawa-recomp/issues/360)).

This page covers building and running the Android app from source. The plan it belongs to is in
[`PLATFORM_PORTABILITY.md`](PLATFORM_PORTABILITY.md#3-android).

## What runs where

- **The launcher** is the player (`src/player/`), built as `libmain.so` and started by
  `LauncherActivity` through SDL's Android Java layer.
- **A title** runs in its own Android process, `:title`, as `TitleActivity`. The desktop player
  starts one child process per title; an Android app may not execute programs from its own files
  (API 29 and later; a tool the app ships in its native library folder is the one exception, see
  [native tests on a device](#native-tests-on-a-device)) and cannot fork itself safely, so the launcher asks Android to start that
  activity instead, with the same arguments and environment a desktop child gets
  (`nk_platform_spawn_process`, Android section of `src/core/nk_platform_posix.c`). The activity
  loads the title's runtime library, `libnk_title_<id>.so`, runs the runtime's `main()`, and its
  process ends when the run ends, as a desktop child's does. The guest sees no difference.
- **A title's runtime library** is the package's generated C compiled with the runtime for the
  device's ABI (`nk_add_title_runtime` in `CMakeLists.txt`). Packages are built on a PC by the
  ordinary route; the APK contains no C compiler, Make or CPython
  ([#360](https://github.com/Jstar269/nakagawa-recomp/issues/360)).
- **A title built by the package route** is the same library, linked by Make with
  `NK_TARGET=android` ([below](#a-title-library-from-the-package-route)). `TitleActivity` loads a
  library the APK ships by its name, or a built one by its absolute path in the app's files
  folder (`TitleLibrary.java`); it refuses any other library, and its process ends with exit
  status 126.

## Prerequisites

Command-line tools from the Android SDK are enough; Android Studio is not needed. The versions
below are the ones this was built and tested with.

- Android SDK with platform `android-37.2`, build-tools 37.0.0, platform-tools, CMake 4.1.2 and
  NDK 30.0.16248370 (`android sdk install` from cmdline-tools 23, or the SDK manager).
- Gradle 9.8.2 (`android/gradle/wrapper/gradle-wrapper.properties` pins it; run
  `gradle -p android wrapper` once to create a local `gradlew`, which stays untracked).
- A JDK 17 or later (Android Studio's bundled JBR works).
- Git, for the pinned SDL3 3.4.20 and SDL3_ttf 3.2.2 sources the build fetches into
  `build/android-deps` (the commits hosted CI builds for Linux; `android/app/deps.cmake`).
- For the showcase demos: PSPDEV, or prebuilt demo PRXs, as for the desktop showcase
  ([`SHOWCASE.md`](SHOWCASE.md)).

Point Gradle at the SDK with `ANDROID_HOME`, or with an `android/local.properties` file
(`sdk.dir=...`), which is ignored and never committed.

## Build

The Gradle project is `android/settings.gradle.kts` and `android/build.gradle.kts`, with the app module in
`android/app/build.gradle.kts`. Its native build, `android/app/CMakeLists.txt`, builds the launcher from the
repository's top-level CMake project and links each bundled title with `nk_add_title_runtime()`. A debug build
merges `android/app/src/debug/AndroidManifest.xml`, which lets `adb shell am start` open `TitleActivity` for
device tests; a release build keeps that activity private.

1. Build the showcase packages on the PC, outside the repository:

   ```bash
   NK_SHOWCASE_BUILD_ROOT=<scratch>/showcase-build \
   NK_SHOWCASE_DEMO_ROOT=<scratch>/showcase-demo \
   python fixtures/showcase/showcase.py build
   ```

2. Build the APK with those packages bundled:

   ```bash
   gradle -p android assembleDebug \
     "-Pnk.titles=showcase-scene-v1=<scratch>/showcase-demo/packages/TEST00007;showcase-breakout-v1=<scratch>/showcase-demo/packages/TEST00008"
   ```

   The APK is `android/app/build/outputs/apk/debug/app-debug.apk`. `-Pnk.abis=arm64-v8a` builds
   one ABI only. Without `nk.titles` the APK carries the launcher alone.

Hosted CI's Android launcher build job builds that launcher-only APK for arm64-v8a from a
clean checkout, with the versions listed under [Prerequisites](#prerequisites), on every change
that runs the native gates ([`CI.md`](CI.md#android-launcher-build)). It builds and packages;
it does not run the APK. It keeps the APK for 14 days as the artifact
`nakagawa-android-arm64-debug`, for [testing on a phone](#test-on-a-phone).

### A title library from the package route

The Makefile builds a title for Android when `NK_TARGET=android`. The compile goal then links
`<BUILD_DIR>/lib<GAME_NAME>.so`, a shared library holding `nk_android_title.c`, with every object
built `-fPIC`, the exit wrappers that `nk_android_title.c` provides, and the libraries `-lSDL3
-lvulkan -lm -llog -landroid`. `CC` is an Android Clang. `ANDROID_SDL3_INCLUDE` names the folder
that holds `SDL3/SDL.h`, and `ANDROID_SDL3_LIBDIR` the folder that holds the APK's `libSDL3.so`.
The conditionals about the built program (platform source, libraries, H.264 backend) follow
`NK_TARGET`; the ones about the host's own shell follow the host. From a PC with the NDK:

```bash
mingw32-make all NK_TARGET=android \
  CC=<ndk>/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android29-clang \
  ANDROID_SDL3_INCLUDE=<deps>/SDL/include ANDROID_SDL3_LIBDIR=<folder with libSDL3.so> \
  GAME_NAME=... GAME_ELF=... BUILD_DIR=...
```

The package route passes the same choice through: `python tools/nk_cli.py build-package <disc
id> --target android` (the default when nk_cli itself runs on Android). The package then records
`lib<id>.so` as its executable.

A build on the phone also needs a C compiler, GNU Make and CPython in the APK. Android lets an
app run only the programs it ships in its native library folder, so those would ship there as
`lib*.so` files. `nk_platform_spawn_tool` starts only programs in that folder, and on Android the
package builder's Python is the APK's `libnk_python.so` there. The APK bundles none of the three
([#360](https://github.com/Jstar269/nakagawa-recomp/issues/360)).

## Run in the emulator

Create an x86_64 Google APIs AVD (API 37.2 was used) and start it headless:

```bash
emulator -avd <name> -no-window -no-audio -gpu swiftshader_indirect
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n io.github.jstar269.nakagawa/.LauncherActivity
```

`swiftshader_indirect` gives the guest Vulkan 1.4 through gfxstream on SwiftShader
(`adb shell cmd gpu vkjson`), with no host GPU. Debug builds let `adb` start a bundled title
directly, as the launcher does:

```bash
adb shell am start -n io.github.jstar269.nakagawa/.TitleActivity \
  --es library nk_title_showcase-scene-v1 \
  --esa args "--image,<app files>/titles/showcase-scene-v1/showcase-scene-v1_image.bin,0x08804000,0x088040ec,none,none,--gui"
adb logcat -s nakagawa nakagawa-title SDL
```

`<app files>` is `/data/user/0/io.github.jstar269.nakagawa/files`. Add
`--esa env "SR_GESTAT=1,SR_EXIT_AT_VBLANK=600"` for the runtime's frame counters and a bounded
run. The runtime's and the player's standard output and error go to the system log under the
tags `nakagawa-title` and `nakagawa`.

`adb install --abi arm64-v8a` installs the arm64 libraries instead, which an x86_64 Google APIs
image runs through its ARM translation layer: a functional check of the arm64 build, not a
measurement of a real device.

## Test on a phone

A physical arm64 phone (Android 10 or later) can run the debug APK. Two routes:

| Route | Needs | Gives |
| --- | --- | --- |
| A. Install the CI build on the phone | A GitHub sign-in on the phone | The launcher only |
| B. Install from the PC with `adb` | A USB cable, and the PC build above for the showcase demos | The launcher, the showcase demos, and a log to attach |

The debug APK is signed with a throwaway key, a different one for every build. To install a
newer APK from another build, uninstall the old one first. Uninstalling deletes the app's
`Nakagawa/` folder, which holds nothing yet that you would miss.

### Route A: install the CI build, no PC

1. On the phone, open the repository's **Actions** tab, workflow **CI**, and the latest run on
   `main` (or on the pull request you are testing). On the run's summary page, under
   **Artifacts**, download `nakagawa-android-arm64-debug`. It downloads as a `.zip`.
2. Open the `.zip` in the **Files** app and extract it. You get `app-debug.apk`.
3. Tap `app-debug.apk`. The first time, Android asks you to allow installs from Files: follow
   **Settings**, turn on **Allow from this source** (under **Apps** > **Special app access** >
   **Install unknown apps**), then go back and install. If Play Protect warns about an unknown
   developer, choose **More details** > **Install anyway**.
4. Open **Nakagawa** from the app drawer.

This build has no titles in it, and the launcher does not list bundled titles. It tests the
launcher itself, and a log needs route B.

### Route B: install from the PC with adb

1. On the phone, turn on **Developer options**: **Settings** > **About phone**, tap **Build
   number** seven times. Then, in **Settings** > **System** > **Developer options**, turn on
   **USB debugging**.
2. Connect the phone by USB and accept the debugging prompt on the phone. `adb devices` lists it
   as `device`.
3. Build the APK with the showcase demos bundled ([Build](#build), with `-Pnk.abis=arm64-v8a`),
   or use the CI artifact from route A for the launcher alone. Install it:

   ```bash
   adb install -r android/app/build/outputs/apk/debug/app-debug.apk
   ```

4. Start the launcher from the app drawer. Start a showcase demo with the `adb shell am start`
   command under [Run in the emulator](#run-in-the-emulator). On a phone, `<app files>` is the
   same `/data/user/0/io.github.jstar269.nakagawa/files`.

### What to try

- **Launcher start.** It opens, draws the library screen and responds to touch or a gamepad.
- **A showcase demo** (route B). It starts in landscape and animates, with sound.
- **Rotate** the phone while the demo runs.
- **Fold and unfold** while the demo runs. The screen size changes, so the presenter has to
  recreate its swapchain.
- **Background and return.** Press Home while the demo runs, wait ten seconds, and switch back.
  In the emulator the demo kept running unseen and did not draw again after returning (the
  Lifecycle row below). Report what the phone does.
- The same rotate, fold and background steps on the launcher.

### What to report

For each item: what you did, what you saw (a short screen recording helps), and whether it
recovered. Add the phone model, its Android version (**Settings** > **About phone**), and which
build you installed (the CI run's link, or the commit you built).

With route B, capture the app's own log. Clear the log first, run the steps, then save it:

```bash
adb logcat -c
adb logcat -d -v time -s nakagawa nakagawa-title SDL > nakagawa-android.log
```

The `-s` filter keeps only the launcher's, the title runtime's and SDL's lines, so other apps'
lines and the rest of the system log stay out. Do not attach a full `adb logcat` or a bug report
from Developer options: those carry other apps' data and account details.

## Native tests on a device

The native CTest set (the core, launcher-state and UI tests, and the runtime's coroutine, FP
conversion and GE reference selftests) builds for arm64-v8a with the NDK and runs on a phone over
`adb`, without Gradle or the APK. Configure the top-level CMake project with the NDK's toolchain
file and `NK_ANDROID_TEST_DIR`, a folder under `/data/local/tmp`:

```bash
cmake -S . -B build/android-arm64-ctest -G Ninja \
  -DCMAKE_MAKE_PROGRAM=$ANDROID_HOME/cmake/4.1.2/bin/ninja.exe \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_HOME/ndk/30.0.16248370/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DBUILD_PLAYER=OFF \
  -DNK_ANDROID_TEST_DIR=/data/local/tmp/<folder>
cmake --build build/android-arm64-ctest
ANDROID_SERIAL=<serial> ctest --test-dir build/android-arm64-ctest --output-on-failure
```

The NDK, CMake and minimum API are the ones the Gradle build pins
([Prerequisites](#prerequisites)); on a Linux host the SDK's Ninja is `bin/ninja`. With
`NK_ANDROID_TEST_DIR` set, CMake uses
`tools/android_test_runner.py` as its cross-compiling emulator. A first setup test copies the
repository's tracked files to `<dir>/src`. Then each test executable is pushed to `<dir>/bin` and
run from `<dir>/work` or `<dir>/src`, whichever matches its CTest working directory, so every
PASS, FAIL and SKIP (exit 77) is the device's own exit status. adb comes from `PATH` or `NK_ADB`,
and `ANDROID_SERIAL` picks the device. Each test runs with `TMPDIR` set to `<dir>/tmp`, so the
tests' temporary folders are in the device folder too, and the next run's setup test empties it.
The tool writes only under `NK_ANDROID_TEST_DIR`; it needs USB debugging and no root. In Git Bash, set `MSYS_NO_PATHCONV=1` first, or the shell rewrites
the device path into a Windows path, which the configure step refuses.

A title cannot start from `adb shell`: on Android it starts in its own process through
`TitleActivity`, and a program started from `adb shell` has no Java VM, so the title spawn fails
closed there and the subtests that need it report SKIP with the reason. Tools are different:
`nk_platform_spawn_tool` starts a program directly, but only one that is a regular file directly
in the folder holding the app's native libraries (`nativeLibraryDir` in the app, the test's own
folder under `adb shell`), because Android lets an app run programs it ships there and nothing it
wrote itself. `posix_process_test` runs its child processes through that route. On Android the
package builder's Python is only `libnk_python.so` in that folder, and the test folder has none.
`package_builder_test` passes there. `player_state_test` asserts the Android toolchain card
(it names `libnk_python.so` and gives no `PATH` or PPA advice), runs every other subtest, and skips
only its package-rebuild steps, which need a runnable Python: it prints a `SKIP` line naming
[#360](https://github.com/Jstar269/nakagawa-recomp/issues/360) and exits 77, which CTest reports as
skipped.

## On-device toolchain

To compile a title's generated C on the phone, the APK can carry a C toolchain: clang and lld
21.1.8, GNU make 4.4.1, CPython 3.15.2 and a sysroot for `aarch64-linux-android29`. None of it comes
from the NDK. `tools/android_toolchain.py` builds clang, lld, the runtimes, the sysroot and make
from pinned sources, and stages the official CPython Android release unchanged
([`android/toolchain/PROVENANCE.md`](../android/toolchain/PROVENANCE.md) has every pin and licence).
The app unpacks it on the first start after an install or update, but nothing runs it yet; this
section covers building it and putting it in the APK.

1. Build and stage it on the PC, outside the repository. The inputs are the files that
   `PROVENANCE.md` and `android/toolchain/sysroot-inputs.json` pin: the LLVM, make and CPython
   tarballs, a bionic clone, and the folder of the other pinned files (`--extra`). With MSYS2
   UCRT64's `bin` on `PATH`:

   ```bash
   python -I tools/android_toolchain.py --llvm-tarball <dl>/llvm-project-21.1.8.src.tar.xz \
     --bionic <bionic clone> --extra <pinned files> \
     --make-tarball <dl>/make-4.4.1.tar.gz \
     --python-tarball <dl>/python-3.15.2-aarch64-linux-android.tar.gz \
     --sh C:/msys64/usr/bin/sh.exe --cmake <sdk>/cmake/4.1.2/bin/cmake.exe \
     --ninja <sdk>/cmake/4.1.2/bin/ninja.exe --work <scratch>/tc-work --out <scratch>/tc-out --jobs 3
   ```

   The first build takes about three hours at `--jobs 3`; each stage keeps a `.done` stamp, so an
   interrupted build resumes. Two builds give the same bytes.

2. Bundle it with `-Pnk.toolchain=<scratch>/tc-out -Pnk.abis=arm64-v8a` on the Gradle command
   line ([Build](#build)). The app gets `libnk_clang.so`, `libnk_lld.so`, `libnk_ld.so`,
   `libnk_make.so`, `libnk_python.so` and CPython's libraries in its native library folder, and
   `assets/toolchain/` with the sysroot, clang's resource files, Python's standard library and
   the notices. The toolchain is arm64 only, so Gradle refuses it with any other ABI, and also
   refuses a folder without the `toolchain-manifest.json` that staging writes.

Android runs only the `lib*.so` files in an app's native library folder, so each program is
named that way. lld picks its role from its program name, so `libnk_ld.so` starts `libnk_lld.so`
as `ld.lld`; clang takes it with `-fuse-ld=<native library folder>/libnk_ld.so`.

The app (`NakagawaHost`, with `ToolchainArchive` reading the archives) unpacks
`assets/toolchain/` into `toolchain/` in its files folder: `sysroot/` (clang's `--sysroot`),
`clang-resource/` (its `-resource-dir`) and `python/` (Python's standard library). It sets
`NK_ANDROID_TOOLCHAIN_DIR` to that folder, `PYTHONHOME` to `python/` inside it, and `TMPDIR`
to the app's cache folder (clang's temporary files), and every program the app starts inherits
them. It sets them only once the unpack is complete: after a failed one they are absent, and a
consumer must read that as "no usable toolchain", not guess the folder. The unpack (about 4,300
files) runs on a worker thread, once per process, on the first start after an install or update.
Meanwhile the launcher shows the files done of the total, and the player starts once the unpack
is complete. Before the player starts, the launcher sets the variables and applies them to the
player library's own C library too (`nativeImportEnvironment`), which is a separate one when an
arm64 player runs under an ARM translation layer; the programs the player starts then inherit
them on either kind of device.

A failed unpack deletes what it had written and shows its reason, a note that building on the
device is unavailable until an unpack succeeds, and two buttons: **Try again**, and **Continue
without the build tools**, which starts the player with the three variables unset (logged as
`TOOLCHAIN_UNPACK_SKIPPED`). A touch presses either button; the arrow keys, Tab or a
controller's D-pad move between them, and Enter or the A button presses the focused one. A file
lock keeps a second process from unpacking at once, and every file is synced before the stamp
that marks the unpack complete is written last; a failed, skipped or interrupted unpack leaves
no stamp, so the next start unpacks again. The title
process never unpacks the toolchain: it sets the variables only when a complete unpack is
already there. The unpacker takes only folders and regular files, and refuses links, paths that
leave the folder, damaged headers and archives that end early.

## Storage

The player's data, settings, caches, logs and saves are in `Nakagawa/` in the app's private
files folder (`NK_ANDROID_DATA_DIR`, set by the activities), the folder name every host uses. The files the APK bundles (`assets/`
with the UI fonts, `titles/` with the bundled titles) are unpacked into the files folder itself
on the first start after an install or update, and it is the working directory: SDL resolves a
relative path against that folder, so `SDL_GetBasePath()` (`./` on Android) finds them.

## Limits

| Area | Status |
| --- | --- |
| Launcher library | The launcher does not list bundled titles; they start from `adb` (above). |
| Disc import | Picking the user's own ISO through the system document picker (SAF) is not wired. |
| Packages from a PC | A package can be built for Android on a PC (`NK_TARGET=android`), and `TitleActivity` loads a built library from the files folder. Moving the package to the phone, and the launcher starting it, are not wired. |
| Building on the phone | The package route and its process rules are in place. The compiler, Make and Python it needs are not in the APK. |
| Lifecycle | Measured in the emulator: with the title in the background the guest keeps running and every present fails (`swapchain acquire failed`), because the presenter does not recreate its Vulkan surface and swapchain after Android destroys the window surface. Pausing the guest in the background and recreating the swapchain on return are not done. |
| Input | Touch controls are not built; SDL gamepads reach the runtime's ordinary input path. |
| Video | The FFmpeg H.264 subset builds for Android; it has not been run on a device. |
| Devices | The APK has run only in the emulator. The native test set has run on one physical arm64 device, an Android 16 handheld with a Snapdragon 7 Gen 4-class SoC ([Native tests on a device](#native-tests-on-a-device)). Memory and API floors have not been measured. |

<!-- issue-links: #360 -->
