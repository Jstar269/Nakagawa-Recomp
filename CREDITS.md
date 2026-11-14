# Credits and upstream lineage

Nakagawa Recomp began as a fork of [sal063's PSP Recompilation Project](https://github.com/sal063/PSP-recompilation-project), a GPL-2.0-or-later PSP static-recompiler toolkit. That project is the immediate upstream for substantial portions of Nakagawa's recompiler, runtime, renderer, reference interpreter, and tooling.

Nakagawa has since substantially extended and modified that codebase. This repository does **not** claim clean-room or independently originated recompiler lineage.

## Primary upstreams

- **PSP Recompilation Project — sal063 / psp-recomp authors** — GPL-2.0-or-later. Immediate upstream and original basis of this repository. The upstream attribution document is preserved at [`third_party/licenses/SAL063_CREDITS.txt`](third_party/licenses/SAL063_CREDITS.txt).
- **PPSSPP — Henrik Rydgård and contributors** — GPL-2.0-or-later plus its own third-party notices. A significant part of the immediate upstream itself derives from, ports, or models PPSSPP subsystems; Nakagawa also retains and extends some of that downstream lineage. The per-file lineage is in [`src/rt/PROVENANCE.md`](src/rt/PROVENANCE.md).

## Other upstreams

- **FFmpeg n4.4 — the FFmpeg developers** — LGPL-2.1-or-later. The ATRAC3 and ATRAC3+ decoder subsets in [`src/rt/atrac3p/`](src/rt/atrac3p/) and the H.264 video decoder subset in [`src/rt/h264_ffmpeg_sub/`](src/rt/h264_ffmpeg_sub/); two imported ATRAC files also carry a MIPS Technologies, Inc. BSD-3-Clause notice.
- **pspdecrypt — John Kelley (John-K) and contributors** — GNU GPL version 3, pinned at
  `c156627db7634d395c380c0a9589130f603307fc`. The ported `~PSP` container, KIRK, AES, SHA-1 and KL4E/KL3E code of the
  built-in game decryption in [`src/core/`](src/core/PROVENANCE.md); its KIRK engine is Draan's KIRK-ENGINE
  (GPL-3.0-or-later).
- **PPSSPP game key tables** — the built-in game-decryption tables in
  [`src/core/nk_psp_builtin_keys.c`](src/core/PROVENANCE.md) are taken from PPSSPP at
  `b4386554795949491a26e51b7d74082bb9b2470f` (`Core/ELF/PrxDecrypter.cpp`, GPL-2.0-or-later, and
  `ext/libkirk/kirk_engine.c`, KIRK-ENGINE by Draan, GPL-3.0-or-later), firmware entries left out.
- **PPSSPP PSN image reader** — the PSN UMD image (NPUMDIMG) reader in [`src/core/`](src/core/PROVENANCE.md) is
  adapted from PPSSPP at `b4386554795949491a26e51b7d74082bb9b2470f`: the DRM BB MAC and cipher from
  `ext/libkirk/amctrl.c` (amctrl.prx reverse engineering by tpu), the LZRC decoder from
  `Core/FileSystems/tlzrc.cpp` (based on benhur's code, rewritten by tpu; Copyright (c) 2012- PPSSPP Project,
  GPL-2.0-or-later) and the block reader from `Core/FileSystems/BlockDevices.cpp` (GPL-2.0-or-later).
- **PPSSPP SAS envelope** — the sceSasCore voice envelope (ADSR) model in [`src/rt/sas_envelope.h`](src/rt/PROVENANCE.md) is adapted from PPSSPP at `3bc60270eae3069f13dfbfdb7292a7e1375de6f6` (`Core/HW/SasAudio.cpp`, `Core/HW/SasAudio.h` and `Core/HLE/sceSas.cpp`, Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later).
- **JPCSP — the jpcsp authors** — GPL-3.0-or-later. The Media Engine video boundary (sceVideocodec, sceMpegbase) in [`src/rt/hle_videocodec.c`](src/rt/PROVENANCE.md) is adapted from JPCSP at `00ac4d6daedd89c0492093ae83fb0d3d819af414` (`sceVideocodec.java`, `sceMpegbase.java`, `SceMp4AvcCscStruct.java`, `mediaengine/MEMemory.java` and `mediaengine/MEEmulator.java`); none of JPCSP's Media Engine code emulation is used. The sceAudiocodec handlers in the same file also take the ATRAC3plus work-area words from JPCSP's `sceMeAudio.java` and `sceAudiocodec.java`.
- **uOFW — the uOFW team** — MIT. The sceAudiocodec wrapper checks in [`src/rt/hle_videocodec.c`](src/rt/PROVENANCE.md) (codec range, EDRAM words and error codes, the output-size rule) are adapted from uOFW at `28306bb5bd4fcdcc0c1d469bf1f3bdb885dde3e9` (`src/kd/avcodec/audiocodec.c`, Copyright (c) 2011, 2012 The uOFW team).
- **PPSSPP texture replacement** — the quick texture hash and the replacement-pack key and lookup order in [`src/rt/texrepl.c`](src/rt/texrepl.c) are translated from PPSSPP at `3bc60270eae3069f13dfbfdb7292a7e1375de6f6` (`GPU/Common/TextureDecoder.cpp`, `GPU/Common/TextureReplacer.cpp` and `GPU/Common/TextureCacheCommon.cpp`, Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later); the texture pack layout is PPSSPP's.
- **PPSSPP ad hoc server** — the lobby of the in-process ad hoc medium in [`src/core/nk_adhoc_net.c`](src/core/PROVENANCE.md) is translated from PPSSPP at `3bc60270eae3069f13dfbfdb7292a7e1375de6f6` (`Core/HLE/proAdhocServer.cpp`, Copyright (c) 2014- PPSSPP Project, GPL-2.0-or-later; PPSSPP's port of Coldbird's aemu ad hoc server), and the relay codec's login checks and port offset in `src/core/nk_adhoc_relay.c` come from the same file and `Core/HLE/sceNetAdhoc.cpp` (Copyright (c) 2013- PPSSPP Project) at that revision.
- **aemu_postoffice — Kethen** — the ad hoc relay protocol that `src/core/nk_adhoc_relay.c` encodes, at commit `02abf18fd161a1639d9f93c48c4c46638c7c6d15` (GPL-3.0-only for its packet headers and server, GPL-3.0-or-later for its PRO ONLINE files); the codec follows its packet layouts and limits, and no code is imported. The codec's login checks, the product-code check included, are PPSSPP's (`login_user_data`), not aemu_postoffice's.
- **minimp3 — lieff (Lion) and contributors** — CC0-1.0 public-domain dedication. The MPEG audio decoder header in [`src/rt/minimp3/`](src/rt/minimp3/), pinned to upstream commit `ea99364f61c14656440e8d77e9c233ccf3124633` and kept byte-identical; it backs the `sceMp3` library.
- **OpenXR-SDK headers — The Khronos Group Inc.** — Apache-2.0 OR MIT. The OpenXR C headers (`openxr.h`, `openxr_platform.h`, `openxr_platform_defines.h`) in [`src/rt/xr/openxr/`](src/rt/xr/openxr/PROVENANCE.md), pinned to tag `release-1.1.65` (commit `24a1f2b92d212501a2b1b8d30ffd53abf55fcfa8`) and kept byte-identical; they type the optional VR module, which loads the OpenXR loader at run time.
- **Clay — Nic Barker** — zlib. The layout library in [`third_party/clay/`](third_party/clay/).
- **Portable Snippets — Evan Nemerson** — CC0-1.0 (public domain). The overflow-checked arithmetic header in [`third_party/portable-snippets/`](third_party/portable-snippets/).
- **AMD FidelityFX SDK (FSR 1) — Advanced Micro Devices, Inc.** — MIT. EASU and RCAS, the optional presentation upscaler (`SR_PRESENT_UPSCALE=fsr1`), in [`third_party/fidelityfx_sdk/`](third_party/fidelityfx_sdk/): FSR 1 and the `ffx_core` headers from FidelityFX SDK v2.4.0 (commit `7539ae8b6568ad29ed78a753404351daf3b5cca7`), and the GLSL portability header `ffx_core_glsl.h` from FidelityFX SDK v1.1.4 (commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`), the last release that ships one. Every file is imported byte-identical.
- **Anime4K — bloc97** — MIT. The CNN x2 (M) upscaler, the optional movie upscaler (`SR_MOVIE_UPSCALE=anime4k`), in [`third_party/anime4k/`](third_party/anime4k/), pinned to upstream commit `4029bf701ecaa15f163cdc49cffe5501c1acf410` (v4.0.1) and kept byte-identical; `tools/anime4k_port.py` wraps its passes as the compute shaders `src/rt/gpu_sdl3vk/shaders/anime4k_cnn_x2_m_*.comp`.
- **dr_libs (dr_wav) — David Reid** — public domain (Unlicense), elected from its Unlicense or MIT-0 choice. The WAV writer selftests use, in [`third_party/dr_libs/`](third_party/dr_libs/).
- **libchdr — Romain Tisserand and contributors** — BSD-3-Clause, pinned at
  `47d794151b2b64f6aba60304e6480f4f5a2bc24b`, with the codecs it carries: miniz 3.1.2 (RAD Game Tools, Valve Software,
  Rich Geldreich; MIT), the Zstandard 1.5.7 decompressor (Meta Platforms; BSD-3-Clause), the LZMA SDK decoder
  (Igor Pavlov; public domain) and dr_flac (David Reid; public domain, Unlicense elected). It reads CHD disc images
  when the player expands them at import, in [`third_party/libchdr/`](third_party/libchdr/PROVENANCE.md).
- **xBRZ 1.9 — Zenju** — GPL-3.0, with the author's linking exception kept unchanged. The pixel-art scaler behind
  Ace Mode's texture scaling, translated from C++ to C (a LOCAL DELTA, checked against the upstream release's output)
  in [`third_party/xbrz/`](third_party/xbrz/PROVENANCE.md).
- **AMD FidelityFX SDK (FSR 3) — Advanced Micro Devices, Inc.** — MIT. The FSR 3.1.6 upscaler's eight compute passes,
  frame generation's seventeen (frame interpolation and optical flow) and the headers they include, from FidelityFX
  SDK v2.4.0 (commit `7539ae8b6568ad29ed78a753404351daf3b5cca7`), in
  [`third_party/fidelityfx_sdk/`](third_party/fidelityfx_sdk/PROVENANCE.md): each file is on the SDK's MIT list, which
  [`LICENSE-AMD.md`](third_party/fidelityfx_sdk/LICENSE-AMD.md) carries, and is imported byte-identical, except three
  headers with recorded local changes. The C host in `src/rt/fsr3_host.c`, `src/rt/fsr3_jitter.c`,
  `src/rt/fsr3_fg_host.c`, `src/rt/gpu_sdl3vk/fsr3_pass.c` and `src/rt/gpu_sdl3vk/fsr3_fg_pass.c` is translated from
  the SDK's C++ and keeps AMD's notice.
- **Monocypher 4.0.3 — Loup Vaillant, Michael Savage, Fabio Scotoni and contributors** — dual BSD-2-Clause or
  CC0-1.0; the project uses it under CC0-1.0. The Ed25519 signature check in
  [`third_party/monocypher/`](third_party/monocypher/).
- **rcheevos 12.5.0 — RetroAchievements.org and contributors** — MIT. RetroAchievements' C library for opt-in
  achievements ([#1316](https://github.com/Jstar269/nakagawa-recomp/issues/1316)), in
  [`third_party/rcheevos/`](third_party/rcheevos/PROVENANCE.md). Its MD5 (`src/rhash/md5.c`, `md5.h`) is
  L. Peter Deutsch's (Aladdin Enterprises), under the zlib licence.
- **PSPSDK / PSPDEV contributors** — BSD-family notices for public ABI definitions and source-owned fixtures ([`third_party/licenses/PSPSDK.txt`](third_party/licenses/PSPSDK.txt)).
- **PSPSDK sample programs — Jesper Svennevid, Donour Sizemore, James Forshaw, John Kelley, Marcus R. Brown, Frank Buss and weltall** — BSD-3-Clause. The public homebrew corpus ([#2144](https://github.com/Jstar269/nakagawa-recomp/issues/2144)) names four PSPSDK samples, pinned to the PSPDEV release; nothing is built or copied into the repository ([`fixtures/homebrew_corpus/PROVENANCE.md`](fixtures/homebrew_corpus/PROVENANCE.md)).
- **SDL, SDL_ttf and their dependencies** — staged beside the player with their licence texts (listed in [`NOTICE.md`](NOTICE.md)).
- **stb_image 2.30 — Sean Barrett** — public domain (Unlicense), the alternative to MIT that upstream offers. The PNG
  decoder in [`third_party/stb/`](third_party/stb/), for the player's prompt glyphs and the disc art read from your own disc.
- **stb_vorbis 1.24 — Sean Barrett** — public domain (Unlicense), the alternative to MIT that upstream offers. The Ogg
  Vorbis decoder for the optional launcher music, in [`third_party/stb/`](third_party/stb/PROVENANCE.md).
- **"Chill lofi inspired" — omfgdude ([OpenGameArt](https://opengameart.org/content/chill-lofi-inspired))** — CC0-1.0.
  The optional launcher music, `ChillLofi.ogg` as its author uploaded it, in
  [`assets/music/launcher/`](assets/music/launcher/PROVENANCE.md).
- **Kenney "Input Prompts" and "Interface Sounds" — Kenney Vleugels ([kenney.nl](https://kenney.nl))** — CC0-1.0. The
  launcher's button prompt glyphs and its optional launcher sounds, in [`third_party/kenney/`](third_party/kenney/).
- **Redump PSP datfile — the Redump project ([redump.info](https://redump.info/))** — the list of PSP disc dumps
  (names, disc IDs, sizes and CRC32, MD5 and SHA-1 hashes) the disc check compares images with, in
  [`assets/redump/`](assets/redump/PROVENANCE.md): Redump's datfile of 2026-11-12 16-02-53, fetched 2026-11-12 and
  kept byte for byte. Redump's About page (<https://redump.info/about>, read 2026-11-12) says of its data: "This
  information is considered public domain to be used however people see fit - whether you are a researcher,
  academic, organization, or simply an individual verifying the data for your own media."
- **Inno Setup 6.7.3 — Jordan Russell and Martijn Laan** — the Inno Setup License (a permissive, no-copyleft licence; text pinned by hash in [`assets/upstream/innosetup.lock.json`](assets/upstream/innosetup.lock.json)). A build tool for the Windows installer, not vendored: `tools/build_installer.py` runs the user's own pinned copy. The installer's `Setup.exe` embeds Inno's unmodified loader, whose copyright notice and web addresses stay intact.
- **LLVM 21.1.8 — the LLVM Project** — Apache-2.0 WITH LLVM-exception. clang, lld, compiler-rt builtins, libunwind,
  libc++abi and libc++, built by `tools/android_toolchain.py` from the pinned release tarball for the on-device C
  toolchain of the Android build ([`android/toolchain/`](android/toolchain/PROVENANCE.md)). Not vendored.
- **GNU make 4.4.1 — the Free Software Foundation** — GPL-3.0-or-later. Built by `tools/android_toolchain.py` from the
  pinned release tarball for the same toolchain. Not vendored.
- **CPython 3.15.2 — the Python Software Foundation** — PSF-2.0. The official Android release's `libpython3.15.so`,
  its bundled OpenSSL 3.5.9 (Apache-2.0) and SQLite 3.53.4 (public domain) libraries, the bzip2 1.0.8 (bzip2-1.0.6
  licence), XZ Utils 5.4.6 (liblzma, public domain) and Zstandard 1.5.7 (BSD-3-Clause) code in its `_bz2`, `_lzma` and
  `_zstd` modules, and its standard library, shipped unchanged in the Android build's toolchain, with the licences listed in
  [`android/toolchain/PROVENANCE.md`](android/toolchain/PROVENANCE.md). Not vendored.
- **LLVM, MSYS2 UCRT64 build — the LLVM Project** — Apache-2.0 WITH LLVM-exception in MSYS2's metadata. The opt-in
  LLVM IR backend (`CODEGEN_BACKEND=llvm`, [#330](https://github.com/Jstar269/nakagawa-recomp/issues/330)) runs the
  developer's own `clang` and `llvm-link`; nothing from LLVM is vendored, linked or shipped
  ([`docs/LLVM_BACKEND.md`](docs/LLVM_BACKEND.md#third-party-toolchain-licence-provenance-and-security)).
- **Android Open Source Project, `android-16.0.0_r4`** — bionic's headers, crt sources and symbol files (BSD-style
  and Apache-2.0; the kernel uapi headers GPL-2.0-only WITH Linux-syscall-note), and the liblog, libandroid and
  libvulkan symbol files and `android/log.h` (Apache-2.0), read by `tools/android_sysroot.py` to build that
  toolchain's sysroot. **Khronos Vulkan-Headers v1.4.363** — Apache-2.0 OR MIT, the sysroot's Vulkan headers. None is
  vendored; the pins and SHA-256 values are in [`android/toolchain/`](android/toolchain/PROVENANCE.md).
- **Font fixtures** — Gudea (Agustina Mingote), Gowun Dodum (The Gowun Dodum Project Authors), M+ 1p (The M+ Project Authors) and Liberation Sans and Liberation Serif 2.1.5 (Google Corporation and Red Hat, Inc.; Reserved Font Names Arimo, Tinos, Cousine and Liberation), all SIL Open Font License 1.1 ([`fixtures/fonts/`](fixtures/fonts/)). The build converts Liberation (Latin), Gowun Dodum (Korean) and Noto Sans JP with M+ 1p (Japanese) into the project's stand-in PSP fonts; Gudea is used only by tests.

The authoritative, component-by-component provenance records are [`NOTICE.md`](NOTICE.md) and the
[`public provenance ledger`](assets/public_provenance_ledger.json).

This credits file is a discoverability aid, not a replacement for `LICENSE`, `NOTICE.md`, per-file license notices, or the third-party license files that govern redistribution. The project is dedicated to Blitzen; see the [Dedication](README.md#dedication) in `README.md`.

## Project UI fonts

- **Chakra Petch** — Cadson Demak — SIL Open Font License 1.1. Static 600 and 700 files come from the pinned [Google Fonts source](https://github.com/google/fonts/tree/45e3d5269e4a17d823f6d84470bc084dc5ae46f6/ofl/chakrapetch).
- **Geist and Geist Mono** — Andrés Briganti, Mateo Zaragoza, Guillermo Rauch, Evil Rabbit, José Rago, and Facundo Santana — SIL Open Font License 1.1. The shipped static weights are instances of the pinned [Google Fonts Geist source](https://github.com/google/fonts/tree/45e3d5269e4a17d823f6d84470bc084dc5ae46f6/ofl/geist) and [Geist Mono source](https://github.com/google/fonts/tree/45e3d5269e4a17d823f6d84470bc084dc5ae46f6/ofl/geistmono).
- **Noto Sans JP** — Google; upstream copyright notice credits Adobe — SIL Open Font License 1.1. The static 400 file is an instance of the pinned [Google Fonts source](https://github.com/google/fonts/tree/45e3d5269e4a17d823f6d84470bc084dc5ae46f6/ofl/notosansjp). The upstream Reserved Font Name `Source` is not used as the shipped family name.

The exact file and license hashes are in [`docs/provenance/FONT_ORIGINS.md`](docs/provenance/FONT_ORIGINS.md). These fonts are project UI assets, not game data.
