# PROVENANCE.md — `third_party/stb/`

Sean Barrett's single-header PNG decoder, vendored for the native player's next UI: `src/ui/nk_ui_image.c` builds
it for PNG only, from memory, with no file access, to decode three inputs: the project's embedded prompt glyphs; at
run time, the `ICON0.PNG` and `PIC1.PNG` the player reads from the user's own disc image, which are never written to
disk or stored in this repository; and, when the user turns on the opt-in cover art
([#2632](https://github.com/Jstar269/nakagawa-recomp/issues/2632)), the cover the player downloaded from
`thumbnails.libretro.com` (`src/player/cover_art.h`) and cached on disk, read back through
`player_ui_art_decode_cover` (`src/player/ui/player_ui_art.c`) with at most 8 MB and 2048 pixels a side
(`PLAYER_UI_ART_COVER_MAX_BYTES`, `PLAYER_UI_ART_COVER_MAX_SIDE`). The cover comes from a remote host, so stb_image's
input is not only local and user-owned (corrected 2026-11-18: this paragraph named only the first two inputs).

`stb_vorbis.c`, the Ogg Vorbis decoder from the same repository, is vendored for the player's optional launcher
music ([#2207](https://github.com/Jstar269/nakagawa-recomp/issues/2207)): `src/player/ui/player_ui_vorbis.c` builds
it, into the player only, to decode the one track the player ships (`assets/music/launcher/`, see its
`PROVENANCE.md`) from memory.

## Upstream source

- Project: stb (`stb_image`, v2.34)
- Repository: `https://github.com/nothings/stb`
- Pin: commit `6b5799bae8a1fe3792691ef4b72ae26434c3e799` (`master` on 2026-11-19, "update stb_image_write version",
  committed 2026-11-19T18:39:41Z). `stb_image.h` last changed in commit `1da9ade1e66195252114f5da497f244704a6c58d`
  (2026-11-19, "bump stb_image version number", v2.34), so the file is the same at both. Updated 2026-11-20 from
  v2.30 (pin `003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5`, file last changed in `013ac3beddff3dbffafd5177e7972067cd2b5083`,
  2024-05-31) under the maintainer's standing decision to keep vendored code at upstream's latest release; upstream
  has no release tags, so the version is the one the header names. The 67 upstream commits to `stb_image.h` between the
  two pins include PNG fixes (an uninitialised palette that could expose stack bytes, `298550a`; thread-safe handling of
  invalid chunks, `796dd17`; `stbi__getn` no longer calls `memcpy` for zero bytes and compares lengths without pointer
  overflow, `51c22cc`; a leak in the 8/16-bit conversions on allocation failure, `de23427`) and many fixes to formats
  this build leaves out (BMP, GIF, TGA, PNM, JPEG, HDR; PIC support removed).
- License: upstream offers a choice of MIT or public domain (Unlicense wording), in `LICENSE` and at the end of
  `stb_image.h`. The project uses it under the public-domain alternative (`Unlicense`). Both texts are kept unchanged;
  the licence text at the end of `stb_image.h` is the same at both pins.
- Import method: `stb_image.h` was downloaded from the pinned commit `6b5799bae8a1fe3792691ef4b72ae26434c3e799`. Its
  Git blob ID matches that commit's file (checked against the GitHub contents API and a second download from
  `raw.githubusercontent.com` on 2026-11-20), and the SHA-256 below was calculated from the local bytes (corrected
  2026-11-18: this bullet sat in the `stb_vorbis.c` section).
- `LICENSE`: upstream reworded its opening lines in commit `aae9fee018211439eec93986e4dd2d9201e9b82c`
  (2026-10-31, "update license since libraries are inconsistent"): the libraries are in the public domain and most are
  also available under MIT. The two alternatives below the opening are unchanged. On 2026-11-20, with the
  maintainer's approval of the new wording, `LICENSE` was downloaded again from that commit, which is still the last
  to change it on `master` (the same file as at the `stb_image.h` pin above); its Git blob ID matches that commit's
  file, and the SHA-256 below was calculated from the local bytes. The `LICENSE` at the earlier pin
  `003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5` had blob `a77ae91f3ec0abb0028c9d9bd8d34d880b454d38`.

### `stb_vorbis.c`

- Project: stb (`stb_vorbis`, v1.24)
- Pin: commit `9f755b604db3492df2cbeb39bf84b81e3e3932e3` (`master` on 2026-11-12, "update version numbers",
  committed 2026-11-12T17:33:26Z), the commit that last changed `stb_vorbis.c`.
- License: the file's own header says "public domain" and its end offers a choice of two licences, "ALTERNATIVE A -
  MIT License" (Copyright (c) 2017 Sean Barrett) or "ALTERNATIVE B - Public Domain (www.unlicense.org)". The project
  uses it under the public-domain alternative (`Unlicense`), as it does `stb_image.h`. Both texts are kept unchanged
  at the end of the file. The repository's `LICENSE` here is the one recorded above (commit
  `aae9fee018211439eec93986e4dd2d9201e9b82c`); the file carries its own licence text.
- Import method: downloaded from `raw.githubusercontent.com` at the pinned commit. Its Git blob ID matches the pinned
  commit's file (checked against the GitHub contents API on 2026-11-12), and the SHA-256 below was calculated from
  the local bytes.

## File manifest

| File | Kind | Upstream blob SHA | SHA-256 |
| --- | --- | --- | --- |
| `stb_image.h` | imported byte-identical | `9e65b834cfd7e34d9f8f8bd81d1c5e6eae128fc7` | `dfc2aadda6295f67f677587773f25e5bf8c8ad427ed0db7c518525c91e72e020` |
| `stb_vorbis.c` | imported byte-identical (pin `9f755b604db3492df2cbeb39bf84b81e3e3932e3`) | `f5e80a0f27cbbe78405d01803bcc5e3edb98c2bd` | `b52cf518355f7b2d181f18b97b812f361ffeb26299d935c8cf29e842401d60a6` |
| `LICENSE` | imported byte-identical (pin `aae9fee018211439eec93986e4dd2d9201e9b82c`) | `48b774af49c4a552d5593e005da685af27be2f20` | `eb36e45c704dd0b59730115523e089b051852625b4db0a6fe76c991900af20e7` |
| `PROVENANCE.md` | project-authored manifest | n/a | n/a |

## Build configuration

`src/ui/nk_ui_image.c` includes the header with `STBI_ONLY_PNG`, `STBI_NO_STDIO`, `STBI_NO_LINEAR`, `STBI_NO_HDR`,
`STBI_NO_FAILURE_STRINGS`, `STBI_NO_THREAD_LOCALS`, `STBI_MAX_DIMENSIONS 8192` and `STB_IMAGE_STATIC`. Before stb
reads anything past the header, the caller's byte and dimension limits are applied, so an oversized image is refused
without being decoded. It also defines `STBI_MALLOC`, `STBI_REALLOC` and `STBI_FREE` as wrappers that, while a decode
runs, refuse any request larger than an image of the header's size and a file of that size can need: stb accepts image
data longer than the image and grows its zlib buffer for as long as the data inflates, so a small file of highly
compressible data would otherwise allocate gigabytes (v2.34 still grows that buffer the same way). A PNG whose first
`IDAT` chunk is empty is refused before stb reads it: v2.30 copied that chunk into a buffer it had not yet allocated (a
`memcpy` into `NULL`); v2.34 skips a zero-byte copy (`51c22cc`) but still adds the chunk's offset to that `NULL`
buffer pointer, which C leaves undefined, so the refusal stays.

A second translation unit builds the header for a test only: `tests/native/texrepl_selftest.c` (`texrepl-selftest`)
uses it as an independent PNG decoder to check the texture-replacement pack reader's own decoder on a seeded
corpus. It defines `STBI_ONLY_PNG`, `STBI_NO_STDIO`, `STBI_NO_LINEAR`, `STBI_NO_HDR`, `STBI_NO_FAILURE_STRINGS`,
`STBI_NO_THREAD_LOCALS` and `STB_IMAGE_STATIC`, but not `STBI_MAX_DIMENSIONS`, so stb's default limit of 2^24 pixels
a side applies there. It is not part of the player or the runtime (corrected 2026-11-18: this section named only
`src/ui/nk_ui_image.c`).

`src/player/ui/player_ui_vorbis.c` is the only translation unit that includes `stb_vorbis.c`. It defines
`STB_VORBIS_NO_STDIO` (the player reads the file itself and hands the decoder memory) and
`STB_VORBIS_NO_PUSHDATA_API` (only the pull API is used), and silences, around the include only, the warnings the
project's flags raise in the upstream code. It is compiled into the player and its tests, nowhere else. A stream is
decoded only when it is mono or stereo at 8 to 192 kHz and declares 1 frame to 600 seconds, so the decoded buffer has
a fixed upper size. The declared length is the 64-bit `stb_vorbis_stream_length_in_samples64`, checked before it is
narrowed: the 32-bit `stb_vorbis_stream_length_in_samples` keeps only the low 32 bits of a last-page granule of 2^63
or more, which is a negative length to the decoder. The decoder is opened with an `stb_vorbis_alloc` arena of
`PLAYER_UI_VORBIS_ARENA_BYTES` (8 MB; the shipped track needs about 0.25 MB), so every allocation stb_vorbis makes
comes from it: a setup header that declares many large codebooks (each at most 2^18 entries, `CODEBOOK_LIMIT`) runs
out of the arena and is refused instead of allocating up to about 0.9 GB. `tests/native/test_player_ui_music.c`
checks both refusals.
