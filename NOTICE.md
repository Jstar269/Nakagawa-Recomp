# Notices, provenance, and disclosures

The project is GPL-3.0-or-later; the full text is in [LICENSE](LICENSE).
Individual files keep their own SPDX identifiers and upstream terms. This file
lists where code came from and the notices that go with it. It is not legal advice.

## Legal and contact

Nakagawa Recomp is an independent project. It is not affiliated with or endorsed by
Sony Interactive Entertainment, Clap Hanz, or any game publisher. PlayStation and
PSP are trademarks of Sony Interactive Entertainment Inc., and game titles are
trademarks of their owners; they are named only to say what the project works
with. The repository contains no game files, firmware or Sony system fonts. Users
supply their own game; Nakagawa decrypts it locally with the public game key
tables PPSSPP ships, never downloads games, and never decrypts firmware.

Rights holders with a concern about anything in this repository can contact the
maintainer privately through
[GitHub private reporting](https://github.com/Jstar269/nakagawa-recomp/security/advisories/new)
or the contact listed on the [maintainer's GitHub profile](https://github.com/Jstar269).
We aim to respond promptly and will remove material where appropriate.

## Upstream families

- **sal063 PSP Recompilation Project** — GPL-2.0-or-later, public revision
  `da17b0e1db209206a407d097d132201e516e3855`. Nakagawa began from this toolkit
  and retains substantial modified runtime and pipeline code. See the
  per-path [public provenance ledger](assets/public_provenance_ledger.json).
- **PPSSPP** — Copyright (c) 2012- PPSSPP Project and contributors,
  GPL-2.0-or-later, with its own third-party notices. Several HLE, GE, VFPU and
  recompiler structures are translated or adapted through the sal063 lineage;
  the files that say so in their headers are `src/rt/hle.c`, the files split from it (`hle_audio.c`, `hle_audio_codec.c`, `hle_cache_dma.c`, `hle_data.c`, `hle_display.c`, `hle_font.c`, `hle_ge.c`, `hle_host.c`, `hle_input.c`, `hle_internal.h`, `hle_io.c`, `hle_kernel_interrupt.c`, `hle_kernel_memory.c`, `hle_kernel_module.c`, `hle_kernel_sync.c`, `hle_kernel_thread.c`, `hle_kernel_time.c`, `hle_kernel_utils.c`, `hle_mpeg.c`, `hle_network.c`, `hle_npdrm.c`, `hle_route.c`, `hle_rtc.c`, `hle_system.c`, `hle_umd.c`, `hle_utility.c`), `sce_kernel_errors.h`, `hle_power.c/.h`,
  `ge.c`, `mpeg.c`, `recomp.c/.h`, `savedata.c`, `vfpu_interp.c` and `evf.h`. PPSSPP-origin data is retained only where the profile records
  its exact path, revision and notice. The per-file lineage, the pin status (the exact PPSSPP
  revision of the inherited files is unknown) and the GPL-2.0-or-later files under `src/` are
  listed in [`src/rt/PROVENANCE.md`](src/rt/PROVENANCE.md).
- **PSP game decryption (PR #1494)** — `src/core/nk_psp_*`. The KIRK engine
  (`nk_psp_kirk.*`, GPL-3.0-or-later per its own notice), the AES and SHA-1 cores
  (`nk_psp_aes.*`, `nk_psp_sha1.*`, public-domain Rijndael and SHA-1 notices
  retained), KL4E/KL3E decompression (`nk_psp_kle.*`) and the `~PSP` container
  logic (`nk_psp_prx.*`, which also descends from PPSSPP's PrxDecrypter) are
  ported from John-K/pspdecrypt at `c156627db7634d395c380c0a9589130f603307fc`,
  whose licence is GPL version 3 with no later-version grant, so every file ported
  through it except the KIRK engine is GPL-3.0-only. The built-in game key tables
  (`nk_psp_builtin_keys.*`) come from PPSSPP at
  `b4386554795949491a26e51b7d74082bb9b2470f` (GPL-2.0-or-later) and its bundled
  KIRK-ENGINE (GPL-3.0-or-later), with every firmware entry left out; Nakagawa
  decrypts a game the user owns, locally, and never decrypts firmware. The
  PSN UMD image reader is adapted from the same PPSSPP revision:
  `nk_psp_amctrl.*` from `ext/libkirk/amctrl.c` (written by tpu, shipped with
  no notice of its own; conveyed as GPL-3.0-or-later), `nk_psp_lzrc.*` from
  `Core/FileSystems/tlzrc.cpp` (Copyright (c) 2012- PPSSPP Project,
  GPL-2.0-or-later, kept GPL-2.0-or-later) and `nk_psp_npumdimg.*` from
  `Core/FileSystems/BlockDevices.cpp` (GPL-2.0-or-later, conveyed as
  GPL-3.0-or-later). The keystore, inflate, elliptic-curve and
  container-framing files are project-authored. Per-file lineage: [`src/core/PROVENANCE.md`](src/core/PROVENANCE.md).
- **Ad hoc transport (#1139)** — the lobby of the in-process ad hoc medium
  (`src/core/nk_adhoc_net.c`) is translated from PPSSPP's
  `Core/HLE/proAdhocServer.cpp` at `3bc60270eae3069f13dfbfdb7292a7e1375de6f6`
  (Copyright (c) 2014- PPSSPP Project, GPL-2.0-or-later, PPSSPP's port of
  Coldbird's aemu server), conveyed as GPL-3.0-or-later. The relay codec
  (`src/core/nk_adhoc_relay.c`) encodes the aemu_postoffice wire format, and no
  aemu_postoffice code is imported; all its login checks, the product-code
  check included, are translated from `login_user_data` in PPSSPP's
  `proAdhocServer.cpp`, and its port offset from
  PPSSPP's `Core/HLE/sceNetAdhoc.cpp` (Copyright (c) 2013- PPSSPP Project) at the
  same revision, and it is conveyed as GPL-3.0-or-later. The opt-in TCP relay
  client (`src/core/nk_adhoc_relay_client.c`) adapts its listener backlog and
  connect states from PPSSPP's `Core/HLE/AdhocTransportRelay.cpp` (Copyright (c)
  2012- PPSSPP Project, GPL-2.0-or-later), and the opt-in online ad hoc tier of
  `src/rt/hle_network.c` is adapted from PPSSPP's `Core/HLE` ad hoc files
  (`proAdhoc`, `sceNetAdhoc`, `sceNetAdhocMatching`; Copyright (c) 2013- and
  (c) 2025- PPSSPP Project, with Coldbird's aemu notice kept in the file), all at the same
  revision and conveyed as GPL-3.0-or-later; both are PPSSPP-derived, not
  PSP-measured. Per-file lineage: [`src/core/PROVENANCE.md`](src/core/PROVENANCE.md),
  [`src/rt/PROVENANCE.md`](src/rt/PROVENANCE.md).
- **Cheat files (#2639)** — the CWCheat reader, the decode of each code form and
  the applier (`src/core/nk_cheats.c`, `src/core/nk_cheats.h`) are translated
  from PPSSPP's `Core/CwCheat.cpp` and `Core/CwCheat.h` at
  `c437463bc5cb78a0236312ad23f9c3b56fb9ae67` (GPL-2.0-or-later), conveyed as
  GPL-3.0-or-later. Neither upstream file carries a notice of its own; the files
  here carry the notice of PPSSPP's source files (Copyright (c) 2012- PPSSPP
  Project, as `Core/Core.cpp` has it at that revision). Per-file lineage: [`src/core/PROVENANCE.md`](src/core/PROVENANCE.md).
- **PSPSDK / PSPDEV** — permissive per-component declarations and BSD-family
  notices for public ABI definitions and source-owned fixtures. The applicable
  notices are in `third_party/licenses/PSPSDK.txt` and `assets/upstream/`.
- **FFmpeg n4.4 ATRAC3 and ATRAC3+ subsets** — LGPL-2.1-or-later. The imported
  decoders, the project-authored API layer over them (`atrac3p_api.c`/`.h`) and their
  license/provenance file are under `src/rt/atrac3p/`. The provenance ledger lists all of
  these under the FFmpeg decoder-import record; the HLE bridge that drives them,
  `src/rt/atrac3p_bridge.c`, has its own project-authored record. Two byte-identical imports,
  `libavcodec/fft_init_table.c` and `libavcodec/fft_table.h`, also carry a MIPS
  Technologies, Inc. BSD-3-Clause notice. Its verbatim text is in
  `third_party/licenses/FFMPEG_MIPS_FFT.txt` and is included in the generated
  native-package third-party notices bundle alongside the FFmpeg LGPL text.
- **minimp3** — CC0-1.0 public-domain dedication. `src/rt/minimp3/minimp3.h` and
  its `LICENSE` are imported from `https://github.com/lieff/minimp3` at commit
  `ea99364f61c14656440e8d77e9c233ccf3124633` (the `LICENSE` byte-identical, the
  header with two later upstream lines as LOCAL DELTA); the manifest is
  `src/rt/minimp3/PROVENANCE.md`. It is statically linked into the runtime through
  the project wrapper `src/rt/mp3_bridge.c` and decodes for `sceMp3`. CC0 asks for no
  notice; it is listed here so the lineage stays discoverable.
- **Clay layout library** (upstream `main` at `e6cc36941ab2`) — `Zlib` (the upstream
  `LICENSE.md` calls it the "zlib/libpng license"). The header (upstream plus four
  documented LOCAL DELTAs), complete license, pinned source manifest, and SHA-256
  values are recorded under `third_party/clay/`. Clay is linked into the native
  player, where the launcher and the in-game overlay lay out through `nk_ui`.
- **Portable Snippets `safe-math`** — `CC0-1.0` (public domain dedication). The
  byte-identical header, upstream `COPYING.md`, pinned source manifest, and
  SHA-256 values are under `third_party/portable-snippets/`. It is compiled into
  the native player through `src/core/nk_iso.c` and `src/core/nk_xb.c`, and into
  every title runtime through `src/core/nk_xb.c` (`mk/runtime_sources.mk`).
- **dr_libs `dr_wav`** — the Unlicense (public domain), elected from upstream's
  Unlicense or `MIT-0` choice; both texts are kept. The header (pinned, with four
  later upstream fixes as LOCAL DELTA), upstream `LICENSE`, pinned source manifest,
  and SHA-256 values are under
  `third_party/dr_libs/`. Only selftests compile it; it is not in the player or
  the runtime.
- **libchdr** — `BSD-3-Clause`, Copyright Romain Tisserand; the MAME-derived CHD, CD-ROM,
  Huffman, bitstream, FLAC and A/V Huffman sources are Copyright Aaron Giles, under the
  BSD-3-Clause text at the top of `third_party/libchdr/src/libchdr_chd.c`. The CHD reader,
  pinned at
  `47d794151b2b64f6aba60304e6480f4f5a2bc24b`, with the codecs it carries: miniz 3.1.2
  (`MIT`), the Zstandard 1.5.7 single-file decompressor (`BSD-3-Clause`, elected from
  upstream's `BSD-3-Clause OR GPL-2.0-only`), the LZMA SDK decoder (public domain) and
  the dr_flac v0.13.4 development snapshot libchdr carries at that pin (the Unlicense, elected from its Unlicense or `MIT-0` choice). Every file
  keeps its upstream bytes apart from LF line endings, one added final newline and small code fixes in six
  files: the empty-hunk check in `src/libchdr_codec_flac.c` and the Huffman lookup-table prefill (2026-11-17), and allocation
  checks on the HUFF codec path (2026-11-20) in `src/libchdr_huffman.c`, `src/libchdr_codec_huff.c`,
  `src/libchdr_bitstream.c`, `src/libchdr_chd.c` and `include/libchdr/huffman.h`, all recorded as LOCAL DELTA in
  `third_party/libchdr/PROVENANCE.md`, which lists each file; the
  licence texts and the pinned manifest are under `third_party/libchdr/`. It is compiled into the native player, which uses it only
  to expand a CHD disc image into a plain `.iso` when the image is added; it is not in the
  runtime.
- **xBRZ 1.9** — `GPL-3.0-only`, Copyright (C) Zenju, with the author's linking exception for
  MAME, FreeFileSync, Snes9x and ePSXe kept unchanged. The pixel-art scaler is a C translation
  of the upstream C++ release (LOCAL DELTA), pinned to release 1.9 (`xBRZ_1.9.zip`, 2026-01-24;
  zip SHA-256 `b2dff73b3abd24a18a7cde78d5ff5ed8f0922296dce6ed734dce2264cd0a0fc9`). The licence,
  the manifest and the SHA-256 values are under `third_party/xbrz/`. It is statically linked
  into the runtime, where the Vulkan GE renderer's Ace Mode texture scaling uses it; it is not
  in `nk_decrypt`.
- **Monocypher 4.0.3 Ed25519 signature check** — upstream offers `BSD-2-Clause OR CC0-1.0`;
  the project uses it under CC0-1.0. Four source files (`monocypher.c` with one later upstream
  fix as LOCAL DELTA, the other three byte-identical), the complete upstream
  `LICENCE.md` and the pinned manifest are under `third_party/monocypher/`. It is compiled into
  the `ed25519-verify-selftest` (RFC 8032 vectors) and into the player, where `src/core/nk_sig.c`
  uses it to check the signature of an update download.
- **FFmpeg n4.4 H.264 decoder subset** — LGPL-2.1-or-later, the same pin as the ATRAC3+
  subset. The imported libavcodec/libavutil files (generic C only, no `--enable-gpl` or
  `--enable-nonfree` component), their license text and the byte-exact manifest are under
  `src/rt/h264_ffmpeg_sub/`. A few imported files are modified: each carries fixes from named
  upstream FFmpeg commits as a dated LOCAL DELTA, recorded in the file header and the
  manifest; the project API layer and configuration stand-ins there are
  identified in `src/rt/h264_ffmpeg_sub/PROVENANCE.md`. It is the default H.264 decoder where no platform decoder
  exists (`H264_FFMPEG=1`, the default off Windows); Windows uses Media Foundation, an operating
  system component, and links none of it. Build notes are in `docs/PLATFORM_PORTABILITY.md`.
- **SDL3** — zlib; used by the optional host renderer/audio integration. The
  first public profile excludes the sal063-derived `src/rt/audio.c` backend and
  links `src/rt/audio_sdl3.c` instead.
- **Vulkan** — Apache-2.0 loader/header ecosystem; this repository does not
  redistribute a Vulkan SDK or loader binary.
- **Android on-device C toolchain (#360)** — nothing vendored; the repository holds the
  pins, the build tools and their tests ([`android/toolchain/PROVENANCE.md`](android/toolchain/PROVENANCE.md)).
  `tools/android_toolchain.py` builds clang, lld, compiler-rt builtins, libunwind,
  libc++abi and libc++ from the LLVM 21.1.8 release tarball (Apache-2.0 WITH
  LLVM-exception), and `tools/android_sysroot.py` builds the sysroot from AOSP
  `android-16.0.0_r4` (bionic: BSD-style and Apache-2.0, its kernel uapi headers
  GPL-2.0-only WITH Linux-syscall-note; liblog, libandroid and libvulkan symbol files
  and `android/log.h`: Apache-2.0) and Khronos Vulkan-Headers v1.4.363 (Apache-2.0 OR
  MIT). Every input is checked by commit or SHA-256. The sysroot archive ships each of
  those licence texts under `NOTICES/`, including the GPL-2.0-only and
  Linux-syscall-note texts. No Android NDK file is shipped. The toolchain assets ship
  each LLVM project's `LICENSE.TXT` and the notices of the third-party code inside the
  binaries: Henry Spencer's regex (BSD-3-Clause), BLAKE3 (CC0-1.0 or Apache-2.0), Ryu
  (Boost Software License 1.0), the Unicode conversion code and name tables (Unicode
  licence), MD5 (public domain or cut-down BSD), `strlcpy` (ISC) and xxHash
  (BSD-2-Clause). The same tool builds GNU make 4.4.1 (GPL-3.0-or-later) from its
  pinned release tarball and ships its `COPYING` and a `SOURCE.txt` that names the
  tarball, its SHA-256 and the build scripts' commit; a release that ships the APK
  carries that tarball, and `tools/android_toolchain.py` at the release's revision
  (it holds make's configure and build settings), as make's corresponding source.
  It also stages the official CPython 3.15.2 Android release unchanged (PSF-2.0,
  with OpenSSL 3.5.9 under Apache-2.0 and SQLite 3.53.4 in the public domain), with
  the release's `LICENSE.txt`, CPython's `Doc/license.rst`, and the licence files
  of OpenSSL 3.5.9, bzip2 1.0.8, XZ Utils 5.4.6 and Zstandard 1.5.7.
- **Opt-in LLVM IR backend toolchain (#330)** — nothing vendored, linked or shipped.
  `CODEGEN_BACKEND=llvm` runs the developer's own `clang` and `llvm-link` (MSYS2 UCRT64
  `mingw-w64-ucrt-x86_64-clang` and `mingw-w64-ucrt-x86_64-llvm-tools`, which MSYS2's
  metadata lists as Apache-2.0 WITH LLVM-exception) on IR that the project-authored
  `tools/codegen_llvm.py` writes. The executable is linked by the C toolchain with GCC's
  runtime libraries, as a C build is. Nothing the project ships selects the backend; the
  player's package builder uses it only when the user's own environment selects it
  (for example `CODEGEN_BACKEND=llvm`). The facts and the security notes are in
  `docs/LLVM_BACKEND.md`.
- **VFPU tables** — PPSSPP-origin lookup data with the exact source and checksums
  recorded in `assets/vfpu/PROVENANCE.json`; inclusion is a provenance decision,
  not a claim about PSP firmware ownership.
- **Gudea font fixture (issue #313)** — SIL Open Font License 1.1,
  Copyright (c) 2011, Agustina Mingote, with Reserved Font Name "Gudea". The
  exact upstream bytes, the licence text, and the `font-pin/v1` provenance
  record are pinned in `tests/fixtures/fonts/gudea/`; they are input to the
  deterministic converter `tools/ttf2pgf.py` for test fixtures only. The font
  is not packaged, generated PGFs stay in temporary or `build/` paths, and the
  output name is project-owned so the Reserved Font Name is never claimed.
- **Stand-in PSP font sources (issues #313, #2005 and #1300)** — SIL Open Font License 1.1:
  Liberation Sans and Liberation Serif 2.1.5 (Digitized data copyright (c) 2010
  Google Corporation, with Reserved Font Names Arimo, Tinos and Cousine; Copyright
  (c) 2012 Red Hat, Inc., with Reserved Font Name Liberation) in
  `tests/fixtures/fonts/liberation/`; Geist Regular and Geist SemiBold (Copyright 2024
  The Geist Project Authors; no Reserved Font Name), the project UI font in
  `assets/fonts/ui/geist/` (static instances of `google/fonts` at
  `45e3d5269e4a17d823f6d84470bc084dc5ae46f6`, see Project UI fonts below); Gowun Dodum
  (Copyright 2021 The Gowun Dodum Project Authors) in `tests/fixtures/fonts/gowundodum/`;
  and M+ 1p (Copyright 2016 The M+ Project Authors) in `tests/fixtures/fonts/mplus1p/`.
  Each is pinned with its licence text and a `font-pin/v1` record, and Geist by its
  record in `assets/fonts/ui/manifest.json`. Unlike Gudea, these
  are build inputs: `tools/ttf2pgf.py` converts them (to PGF, scaled, emboldened and
  cut to each glyph's ink) into the stand-in PSP fonts that a build puts in its `font/`
  folder and the release ships (`font/README.md`). Geist makes the regular and bold
  sans-serif Latin fonts (`nkltn.pgf` and `nkltns.pgf` from Regular, `nkltn4.pgf` and
  `nkltn12.pgf` from SemiBold), with Liberation Sans Regular and Bold drawing the 110
  characters of their set Geist lacks; Liberation makes the other twelve `nkltn*.pgf` fonts, and Gowun Dodum makes
  `nkkr.pgf`. `nkjpn.pgf` is made from Noto Sans JP (see Project UI fonts below), with
  M+ 1p drawing the 341 characters of its set Noto Sans JP lacks (U+2252 among them).
  Each generated font is shipped with its conversion manifest and the licence text of
  every font it draws from; `nkjpn-OFL.txt` carries both Noto Sans JP's and M+ 1p's,
  and the four Geist fonts' licence copies both Geist's and Liberation's. The generated
  fonts carry project-owned names (`Nakagawa Open Latin`, `Nakagawa Open Japanese`,
  `Nakagawa Open Korean`), so no Reserved Font Name is used.
- **stb_image 2.34** — Sean Barrett; upstream offers `MIT OR Unlicense`, and the
  project uses it under the public-domain (Unlicense) alternative. The byte-identical
  header, upstream `LICENSE` and the pinned manifest are under `third_party/stb/`. It is
  built for PNG only into the `nk_ui` core and the player, where it decodes the
  embedded prompt glyphs and the `ICON0.PNG` / `PIC1.PNG` read at run time from the
  user's own disc image; those images are kept in memory only.
- **stb_vorbis 1.24** — Sean Barrett; upstream offers `MIT OR Unlicense`, and the
  project uses it under the public-domain (Unlicense) alternative. The byte-identical
  source, with both licence texts at its end, and the pinned manifest are under
  `third_party/stb/`. It is built into the player only, where it decodes the optional
  launcher music.
- **"Chill lofi inspired" (`ChillLofi.ogg`)** — CC0-1.0, by omfgdude, from OpenGameArt
  (`https://opengameart.org/content/chill-lofi-inspired`). The optional launcher music,
  byte-identical to the upload, under `assets/music/launcher/` with its manifest; it ships
  beside the player.
- **Kenney "Input Prompts" 1.5 and "Interface Sounds" 1.0** — CC0-1.0, by Kenney
  (kenney.nl). The 32 prompt glyphs and three UI sounds the player uses, each pack's
  `License.txt` and the pinned manifest are under `third_party/kenney/`;
  `tools/gen_ui_assets.py` embeds them in `src/ui/nk_ui_assets.c`. The controller
  glyphs only show which button to press on the pad that is connected; no console
  maker is affiliated with or endorses the project.

## Generated third-party notices

Release notices are generated, never hand-written. One machine-readable source
of truth, `assets/third_party_components.json` (schema:
`assets/third_party_components.schema.json`), records every component that can
enter a release artifact: its SPDX identifier, its disposition, the packaging
route that copies it, and the license texts copied verbatim into
`third_party/licenses/`. The native package, the
SBOM, and the release gate all read that one file.

- The native package route (`tools/package_notices.py`, invoked by the package
  build and by `tools/copy_build_assets.ps1`) emits `THIRD_PARTY_NOTICES.txt`,
  `THIRD_PARTY_NOTICES/index.json`, `SOURCE.txt`, and `RELINK.md` beside the
  package, and fails closed when a bundled binary has no license record or no
  license text.
- `tools/generate_sbom.py` lists the shipped DLLs and the recorded Python tool
  licenses from the same file, so the SBOM cannot drift from the notices.
- `tools/test_third_party_notices.py` is the gate: it fails when a DLL a
  packaging route can copy, or any component in the source of truth, lacks a
  license record or a license text.

## Staged host runtime libraries (#421)

The Windows player and every built package stage `SDL3.dll`, `SDL3_ttf.dll`,
and the mechanically resolved SDL3_ttf dependency closure beside the
executable (`tools/stage_runtime_dlls.py`, PE import-table walk stopping at
Windows system DLLs). The launcher's fonts are the project's own, under
`assets/fonts/ui/` (see Project UI fonts below). Every staged DLL ships its upstream
license text, copied verbatim from the MSYS2 UCRT64 toolchain packages
(`ucrt64/share/licenses/`, read-only), or from the upstream source at the
packaged tag for a text the package does not install, and recorded with its
SHA-256 in `assets/third_party_components.json`:

| DLL | License | License text |
| --- | --- | --- |
| `SDL3.dll` | Zlib AND BSD-3-Clause | `third_party/licenses/sdl3/LICENSE.txt`, `third_party/licenses/sdl3/yuv2rgb-LICENSE.txt`, `third_party/licenses/sdl3/hidapi-LICENSE.txt` |
| `SDL3_ttf.dll` | Zlib | `third_party/licenses/SDL3_ttf.txt` |
| `libfreetype-6.dll` | FTL AND MIT | `third_party/licenses/FreeType-FTL.txt`, `third_party/licenses/freetype/bdf-README.txt`, `third_party/licenses/freetype/pcf-README.txt` |
| `libharfbuzz-0.dll` | MIT-Modern-Variant AND MIT | `third_party/licenses/HarfBuzz.txt`, `third_party/licenses/harfbuzz/ms-use-COPYING.txt` |
| `libgraphite2.dll` | MIT (chosen) | `third_party/licenses/Graphite2.txt` |
| `libpng16-16.dll` | libpng-2.0 | `third_party/licenses/libpng.txt` |
| `zlib1.dll` | Zlib | `third_party/licenses/zlib.txt` |
| `libbz2-1.dll` | bzip2-1.0.6 | `third_party/licenses/bzip2.txt` |
| `libbrotlidec.dll`, `libbrotlicommon.dll` | MIT | `third_party/licenses/Brotli.txt` |
| `libglib-2.0-0.dll` | LGPL-2.1-or-later | `third_party/licenses/GLib.txt` |
| `libintl-8.dll` | LGPL-2.1-or-later | `third_party/licenses/libintl.txt` |
| `libiconv-2.dll` | LGPL-2.1-or-later | `third_party/licenses/libiconv.txt` |
| `libpcre2-8-0.dll` | BSD-3-Clause WITH PCRE2-exception | `third_party/licenses/PCRE2.txt` |
| `libgcc_s_seh-1.dll`, `libstdc++-6.dll` | GPL-3.0-or-later WITH GCC-exception-3.1 | `third_party/licenses/gcc-libs/COPYING3.txt`, `third_party/licenses/gcc-libs/COPYING.RUNTIME.txt` |
| `libwinpthread-1.dll` | MIT AND BSD-3-Clause | `third_party/licenses/winpthreads/COPYING.txt` |

License choices and combination notes:

- **FreeType** is dual-licensed FTL or GPL-2.0-or-later; this distribution uses
  the **FTL** option, which the FSF lists as GPL-compatible. As the FTL asks of
  a binary distribution: portions of this software are copyright (C) 1996-2026
  The FreeType Project (<https://freetype.org>), and this software is based in
  part on the work of the FreeType Team. The package's `THIRD_PARTY_NOTICES.txt`
  repeats that statement. FreeType's `LICENSE.TXT` gives the contributed BDF and
  PCF drivers, and `src/base/fthash.c` with its header, an X11-style (MIT)
  licence that asks for its notice in all copies. The MSYS2 build compiles all
  three into `libfreetype-6.dll` but installs only `FTL.TXT`, so the package
  ships `src/bdf/README` and `src/pcf/README` from upstream at the packaged tag,
  and its notices repeat `fthash.c`'s copyright lines.
- **SDL3** is zlib, but `SDL3.dll` also compiles `src/video/yuv2rgb`
  (BSD-3-Clause, Adrien Descamps) and, on Windows, HIDAPI, which may be used
  under GPL-3.0, a BSD-style licence or its original licence; this distribution
  uses the **BSD-style** option. Both binary clauses ask for the text in the
  documentation, and the MSYS2 package installs only `LICENSE.txt`, so the
  package ships `src/video/yuv2rgb/LICENSE` and `src/hidapi/LICENSE-bsd.txt`
  from upstream at the packaged tag.
- **Graphite2** is granted under four licenses (LGPL-2.1+, MPL-2.0, GPL-2.0+,
  or MIT) by its upstream `LICENSE`; this distribution uses the **MIT** option.
- **HarfBuzz**'s `COPYING` refers readers to `COPYING` files in subdirectories.
  At the packaged tag, `src/ms-use/COPYING` (MIT, Microsoft Corporation) is the
  licence of the data from which the Universal Shaping Engine and vowel-constraint
  tables compiled into `libharfbuzz-0.dll` are generated, so the package ships it
  beside `HarfBuzz.txt`. The other subdirectory `COPYING` files cover benchmark
  and test data that the MSYS2 build does not compile.
- **GLib, libintl and libiconv** are LGPL-2.1-or-later and are dynamically
  linked as separate replaceable DLLs. LGPL-2.1 section 3 permits use under
  GPL-3 terms, so they combine with this GPL-3 distribution.
- The **MinGW runtime** licenses (`gcc-libs`, `winpthreads`) were already
  covered by the existing records; no new text was needed for them.

The FFmpeg ATRAC3 and ATRAC3+ subsets and, in builds with `H264_FFMPEG=1`, the FFmpeg H.264
subset (LGPL-2.1-or-later) are statically linked. The whole
program is distributed under the GNU GPL version 3 (the project's own files are
GPL-3.0-or-later; the player and `nk_decrypt` also link the GPL-3.0-only decryption files
listed above, and the player links the GPL-3.0-only xBRZ scaler) with its complete corresponding source, and `SOURCE.txt` names the exact repository commit or tag of each
package, which is what lets a user modify and relink it (issue #421).

## Public-safe exclusions

The public source profile excludes the legacy PGF parser, all firmware font data
and any PGD implementation; the project-authored PGF reader `src/rt/pgf_public.c`
is public and reads only fonts the user supplies. It also excludes the sal063-
derived ISO/VFS and SDL audio implementations until their separate public review
is complete. These exclusions are enforced by
`assets/public_source_profile.json`, not by a disclaimer. The unavailable seams
reject the capability and do not fabricate fonts, disc reads, playback, keys, or
success.

Private engineering/review documents are also outside the active public source tree. The
HST title manifest (addresses only, no game data), synthetic title manifests and generic
schemas are retained; no game executable, extracted asset, save, key, capture, oracle trace,
or generated retail translation unit is part of the public source boundary.

## Attribution contract

Preserve each file's SPDX header and any upstream notice when copying or
modifying source. The public provenance ledger assigns every included path an
explicit class and evidence reference. A missing or unresolved
record is a publication failure. Generated files remain generated and must not be
hand-edited.

New project-authored source files use `SPDX-License-Identifier: GPL-3.0-or-later`.
Files classified as upstream-derived retain the SPDX identifier and notices
for their recorded source lineage.

## Upstream authors

Nakagawa Recomp is not endorsed by PPSSPP, sal063, FFmpeg, SDL, Khronos, or any
other upstream author.

## Private inputs and history

Users must supply any external inputs locally. Private inputs, keys, saves,
captures, extracted assets and generated output must not be committed, attached
to issues, or included in a release.

## Presentation clip of Hot Shots Tennis

The MP4 file docs/media/hst-rally.mp4 is a 28.5-second presentation clip of Hot Shots Tennis: Get a Grip running in
Nakagawa Recomp on Windows (a build of main e3a407da, recorded 2026-10-27), with the game's own sound, and
docs/media/hst-rally-poster.png is one of its frames. The game, its logo, artwork, music, sound and on-screen text belong to
their owners and are not licensed under this repository's GPL-3.0-or-later declaration; the clip is shown
only to identify the game. The repository contains no game data files, and users need their own copy of the
game. Both files are listed with the SHA-256 of their exact bytes in assets/release_manifest.json.

## Launcher screenshots

The PNG files docs/media/launcher-library.png and docs/media/launcher-building.png are presentation
screenshots of the Nakagawa player from the v0.0.1 release build. The library shows the disc art of Hot Shots
Tennis: Get a Grip and LocoRoco, and the build screen names Hot Shots Tennis; those games, their art and
names belong to their publishers and are shown only to identify them. The repository contains no game data
files, and users need their own copy of a game. Both files are listed with the SHA-256 of their exact bytes in
assets/release_manifest.json.

## Presentation screenshots of Hot Shots Tennis

The PNG file docs/media/hst-title-screen.png is a presentation capture of Hot Shots Tennis: Get a Grip
running in Nakagawa Recomp on Windows. The game, its logo, artwork and on-screen text belong to their
owners and are not licensed under this repository's GPL-3.0-or-later declaration; the image is shown
only to identify the game. docs/media/hst-title-screen-960.png is the same capture reduced to 960 by 540
pixels for the project website, under the same terms. The repository contains no game data files, and
users need their own copy of the game. Both files are listed with the SHA-256 of their exact bytes in
assets/release_manifest.json.

## Project website share card

The PNG file docs/images/brand/og-card.png is the project-authored link-preview image of the project website:
the N1 mark, a short MIPS listing written for the card, and text set in Chakra Petch, Geist and Geist Mono
(SIL Open Font License 1.1, assets/fonts/ui/chakra-petch/OFL.txt, assets/fonts/ui/geist/OFL.txt and
assets/fonts/ui/geist-mono/OFL.txt). It shows no game imagery and no game code. It is distributed under GPL-3.0-or-later and
listed with the SHA-256 of its exact bytes in assets/release_manifest.json.

## Project-authored Nakagawa brand artwork

The files docs/images/brand/nakagawa-mark-light.svg and
docs/images/brand/nakagawa-mark-dark.svg are the project-authored N1 Block & Branch
mark, rendered in dark ink for light page themes and light ink for dark page themes.
The file assets/brand/nakagawa.ico is the app icon drawn from that mark by
tools/make_app_icon.py. The artwork is distributed under GPL-3.0-or-later and listed
with the SHA-256 of each file's exact bytes in assets/release_manifest.json.

## Project UI fonts

The fonts under assets/fonts/ui/ are Chakra Petch, Geist, Geist Mono, and Noto Sans JP,
distributed under the SIL Open Font License 1.1. Each family's OFL text is beside its font
files; only whitespace normalization was applied, with all license wording retained. Exact
upstream revisions, file hashes, static weights, and license hashes are recorded in
docs/provenance/FONT_ORIGINS.md and assets/fonts/ui/manifest.json. These are project UI
assets, not game data. The build also converts the Noto Sans JP file into the Japanese
stand-in PSP font `nkjpn.pgf` (#2005); `tests/fixtures/fonts/notosansjp/PIN.json` pins it for
the converter, and its Reserved Font Name `Source` is not used. It converts Geist Regular and
SemiBold into the regular and bold sans-serif Latin stand-in fonts (#1300); the converter checks
them against their records in assets/fonts/ui/manifest.json, and Geist reserves no font name.
