# PROVENANCE.md — `src/rt/atrac3p/`

Standalone ATRAC3+ (ATRAC3plus) and plain ATRAC3 (AT3) audio decoder subset for
the Nakagawa recomp runtime (Hot Shots Tennis: Get a Grip, PSP). Both decoder
cores are imported from FFmpeg; thin C APIs written for this project wrap them.

## Upstream source

- Project: FFmpeg
- Repository: `https://git.ffmpeg.org/ffmpeg.git` (upstream canonical)
- Pin: tag `n4.4`. The tag object SHA is `09c358362008e2d04cec8239526c6827543da4cf`
  and it peels to commit `dc91b913b6260e85e1304c74ff7bb3c22a8c9fb1` (2021-04-08,
  "RELEASE_NOTES: Based on the version from 4.3"). All blob SHAs in the
  manifest below are resolved as `n4.4:<path>` against that commit's tree.
  Vintage note: the PPSSPP standalone fork era that motivated the pin is dated
  ~2021-11-19 (pre-av_tx, classic `FFTContext`). The n4.4 tree is not that
  fork's exact vintage: FFmpeg `master` holds commits to the imported files
  that the n4.4 tag lacks (corrected 2026-11-17, measured over every
  `imported` row of the manifest below with
  `git log n4.4..origin/master --until=2021-11-20 -- <paths>`; dates are
  commit dates). On the imported `libavcodec` files that is 71 commits: 62
  touch `libavcodec/version.h` (version bumps, one of them also below), and
  these ten touch the other codec files:
  `e7cbbd9026` ("avcodec/put_bits: Don't set size_in_bits, fix overflow",
  2021-03-30; `put_bits.h`), `11ff9cb5e9` ("avcodec/put_bits: Add functions
  for amount of bytes written/left", 2021-03-30; `put_bits.h`),
  `c197e3fe12` ("avcodec/bitstream: Remove avpriv PutBits API functions",
  2021-04-27; `bitstream.c`, `put_bits.h`, `version.h`), `a247ac640d`
  ("avcodec: Constify AVCodecs", 2021-04-27; `atrac3.c`,
  `atrac3plusdec.c`), `c28823ff97` ("lavc/get_bits: avoid avcodec.h
  dependency", 2021-06-10; `get_bits.h`), `25c8507818` ("Remove/replace
  some unnecessary avcodec.h inclusions", 2021-07-22; `atrac.c`,
  `bitstream.c`), `763c501432` ("avcodec/fft-internal: Remove unused
  macros", 2021-08-05; `fft-internal.h`), `211619ad7f` ("avcodec: Remove the
  FFT_FIXED_32 define", 2021-08-05; `fft.h`, `fft_float.c`,
  `fft_template.c`, `fft-internal.h`, `mdct_float.c`, `mdct_template.c`),
  `a449fb35ca` ("avcodec/fft-internal: Make it a standalone header",
  2021-08-05; `fft-internal.h`) and `1ea3650823` ("Replace all occurences of
  av_mallocz_array() by av_calloc()", 2021-09-20; `atrac3.c`,
  `atrac3plusdec.c`). The imported `libavutil` files changed too: 43
  commits in the same range touch them (`mem.c`, `common.h`, `log.h` and
  others); they are not listed here. None of these commits is imported:
  every file is taken from the n4.4 tree, and the byte verification below
  rests on the n4.4 blob SHAs alone.
- License: LGPL-2.1-or-later (see `LICENSE.LGPLv2.1.txt`, imported from
  `COPYING.LGPLv2.1` at the same pin; upstream blob
  `58af0d3787aec7d6f8d833535d64879156e7ec23`)
- Import method: files extracted byte-exact via `git archive n4.4` /
  `git cat-file blob n4.4:<path>`; every imported blob SHA in the manifest
  below is verified against the upstream tree (`git rev-parse n4.4:<path>`)
  and the local working-tree content hash (`git hash-object`).

## File manifest

Legend:

- `imported byte-identical` — working-tree content hash equals the upstream
  n4.4 blob hash; no local modification.
- `imported + LOCAL DELTA` — upstream blob, modified as documented in the
  file itself (search for `LOCAL DELTA`); delta descriptions below.
- `AUTHORED stand-in` — written for this project. Two classes:
  - `n/a` upstream counterpart — no such file exists in the n4.4 tree
    (configure-generated headers and the API layer).
  - Upstream SHA listed — upstream has a file of the same name, but the
    content here is a locally authored subset/stand-in, not a copy.

| File | Kind | Upstream n4.4 blob SHA |
| --- | --- | --- |
| `atrac3p_api.c` | AUTHORED stand-in | n/a |
| `atrac3p_api.h` | AUTHORED stand-in | n/a |
| `libavcodec/atrac.c` | imported byte-identical | `bf9878be453d86823bff1c98694ab38365972600` |
| `libavcodec/atrac.h` | imported byte-identical | `05208bbee69aaafa4d9dadb736dd93f14176b3ce` |
| `libavcodec/atrac3.c` | imported + LOCAL DELTA | `37ccbcc946cbc394cbb426a09cc6776f8db107bf` |
| `libavcodec/atrac3data.h` | imported byte-identical | `d050c0f3806a95d79fec38d1482fdea07c06110b` |
| `libavcodec/atrac3plus.c` | imported + LOCAL DELTA | `3a0a0d5f360827d8bc1daf6bc3d4d84cd5557ace` |
| `libavcodec/atrac3plus.h` | imported byte-identical | `a588436e2adb86fb32e27b22ec7f96d9fca09038` |
| `libavcodec/atrac3plus_data.h` | imported byte-identical | `05ae2b78a9ba8736583a5cac078c5d7538afd23d` |
| `libavcodec/atrac3plusdec.c` | imported + LOCAL DELTA | `c024ab6bded112d5ff298045fed106858da55303` |
| `libavcodec/atrac3plusdsp.c` | imported byte-identical | `e32c5c81705d5a6d133c0230758346084f7c788d` |
| `libavcodec/avcodec.h` | AUTHORED stand-in (upstream name exists) | `8a71c042308e37a89dbf357d8f9b276dd4ebcbf9` |
| `libavcodec/avfft.h` | imported byte-identical | `0c0f9b8d8dae13c14a8cd91a1c4234b07821e916` |
| `libavcodec/bitstream.c` | imported byte-identical | `e425ffdc96473034c78c4844ba8be3234189f93c` |
| `libavcodec/bytestream.h` | imported byte-identical | `d0033f14f3690d6cedf1320b82d42f4d59124770` |
| `libavcodec/config.h` | AUTHORED stand-in (configure output) | n/a |
| `libavcodec/fft.h` | imported byte-identical | `e03ca01abfde80e4d36d591028d94dbddcff490d` |
| `libavcodec/fft_float.c` | imported byte-identical | `73cc98d0d4b1b137662240cbfb8c1b4b72460ba3` |
| `libavcodec/fft_init_table.c` | imported byte-identical | `83e35ffb7c844d777935bebad4decb9ed45dd966` |
| `libavcodec/fft_table.h` | imported byte-identical | `09df49f2b8ef8d5d2dc0a0216d46df6ea54389c1` |
| `libavcodec/fft_template.c` | imported byte-identical | `3012372a74be92b98b794502cea8188a560a68d2` |
| `libavcodec/fft-internal.h` | imported byte-identical | `3bd5a1123d84efcc527c47a689f84eb246150c51` |
| `libavcodec/get_bits.h` | imported byte-identical | `66fb8775994b3a9fee3fac9125981bccfce7aa68` |
| `libavcodec/internal.h` | AUTHORED stand-in (upstream name exists) | `b57b9968166f9032baa06ff84e9426b493a8fee5` |
| `libavcodec/mathops.h` | imported byte-identical | `1c35664318f8ba8753d7586c876a4611f6f213cd` |
| `libavcodec/mdct_float.c` | imported byte-identical | `cff2d211c4bbfbebeec9a6e105a1290147899709` |
| `libavcodec/mdct_template.c` | imported byte-identical | `e0ad9f1e5312991a23f9fefc6f600870d60a0820` |
| `libavcodec/put_bits.h` | imported byte-identical | `f07944a8fbee01053e761916bb82e5ac756bf8c6` |
| `libavcodec/sinewin.c` | imported + LOCAL DELTA | `1fa0e953f015027831296c9ddd7621350c56d8e5` |
| `libavcodec/sinewin.h` | imported byte-identical | `fc4e69a58fe378934086e56a19d69c19bd461ec3` |
| `libavcodec/sinewin_tablegen.h` | imported byte-identical | `6887d59cfe9ba6bffe664b661c97ef53009d34d6` |
| `libavcodec/version.h` | imported byte-identical | `cfdde469600bd30a6d60cfe8816747dba6edbd2b` |
| `libavcodec/vlc.h` | imported byte-identical | `6879c3ca6a7c29a3b1e8a87f13a849aac1aa8b0a` |
| `libavutil/attributes.h` | imported byte-identical | `5cb9fe345288e1e5a892a454cc6fbc0ae7883cf5` |
| `libavutil/avassert.h` | imported byte-identical | `9abeadea4a2319832491e57d6b4ab16da2e738f5` |
| `libavutil/avconfig.h` | AUTHORED stand-in (configure output) | n/a |
| `libavutil/avutil.h` | AUTHORED stand-in (upstream name exists) | `4d633156d14df32518281d0a9750de52bf0c69fb` |
| `libavutil/bswap.h` | imported byte-identical | `91cb79538dc2fb5979df88ce5e8279ec80170600` |
| `libavutil/channel_layout.h` | imported byte-identical | `d39ae1177afea3e58e2396711b5d02aa8985094e` |
| `libavutil/common.h` | imported byte-identical | `aee353d3993d1ef725eeb865dde33141eb183cbe` |
| `libavutil/config.h` | AUTHORED stand-in (configure output) | n/a |
| `libavutil/dynarray.h` | imported byte-identical | `3a7e146422a13ffacaa539646f69a0aa59da5ef9` |
| `libavutil/error.h` | imported byte-identical | `71df4da353b9cd95e45e86c14acc0c285854f0af` |
| `libavutil/float_dsp.c` | imported byte-identical | `6e28d71b570c7988346e2fcaeb209ed781952b37` |
| `libavutil/float_dsp.h` | imported byte-identical | `9c664592bd550ec18c09ce9d0406f399bf97f1af` |
| `libavutil/intmath.c` | imported byte-identical | `b0c00e1cadd918c10564a54584cd6807a996752f` |
| `libavutil/intmath.h` | imported byte-identical | `9573109e9d1b910496cc280815883a011e96feb2` |
| `libavutil/intreadwrite.h` | imported byte-identical | `4c8413a536868b9a272d1030a20d0d776880b1d8` |
| `libavutil/libm.h` | AUTHORED stand-in (upstream name exists) | `a8199623912d50d3439cb11e59c0c9a7712d125d` |
| `libavutil/log.h` | imported byte-identical | `8edd6bbf2b8bdd21dd711565fc1b4ac51e160d44` |
| `libavutil/log2_tab.c` | imported byte-identical | `0dbf07d74c5e2fbbb2acb0fc99196b62753af8d3` |
| `libavutil/macros.h` | imported byte-identical | `2007ee5619871558dd07cd516ca77324dcf75940` |
| `libavutil/mathematics.h` | AUTHORED stand-in (upstream name exists) | `54901800ba6ad22fd33d6c8595400d378c7429ca` |
| `libavutil/mem.c` | imported byte-identical | `cfb6d8ab8ffa6c6d51f325d4d8bbe7dc0922871f` |
| `libavutil/mem.h` | imported + LOCAL DELTA | `e21a1feaaeab14d21ef93e3053e2de6b04b4097b` |
| `libavutil/mem_internal.h` | imported byte-identical | `ee2575c85f9cd04a1d9c4f483dfc02fb598f09f4` |
| `libavutil/qsort.h` | imported byte-identical | `39b7a088520a61a87c1565474cb4b32ea6ce766a` |
| `libavutil/reverse.c` | imported byte-identical | `105eb03dda4c30952541347c6e6f7f2feb4f2f30` |
| `libavutil/reverse.h` | imported byte-identical | `4eb61239328120bb77ae3ebc34f4f911ad51936b` |
| `libavutil/thread.h` | AUTHORED stand-in (upstream name exists) | `be5c4b1340e7ad7b1bef227a44b78af44809c227` |
| `libavutil/version.h` | imported byte-identical | `f888dbb2dc12e3f7180fe4ec639b453d924272e0` |
| `LICENSE.LGPLv2.1.txt` | imported byte-identical (`COPYING.LGPLv2.1`) | `58af0d3787aec7d6f8d833535d64879156e7ec23` |

## LOCAL DELTAs (imported files with local modification)

Each file below keeps its upstream licence header unchanged and carries, just
under it, the dated notice `Modified by Nakagawa Recomp contributors, <date>.`
(LGPL-2.1 section 2(a)); its changes are marked `LOCAL DELTA` in the file. The
notices were added on 2026-11-17 and date each file's changes: `atrac3.c`
2026-10-20 (the plain AT3 import, #1109), 2026-11-17 and 2026-11-21, `atrac3plus.c` and `sinewin.c`
2026-08-11 (the first public release), `atrac3plusdec.c` 2026-08-11 and
2026-11-17.

### `libavcodec/atrac3.c`

- Added `#include "../atrac3p_api.h"` (marked `LOCAL DELTA`).
- Replaced the framework-only `AVFrame`/`AVPacket` entry point with a
  standalone plane-output entry point that calls FFmpeg's existing
  `decode_frame()` and reports 1024 samples only on success. The frame decode
  algorithm and tables remain upstream n4.4.
- Exported `atrac3_context_size()`, `atrac3_init()`, and `atrac3_close()` so the
  project API can manage the opaque upstream `ATRAC3Context` without exposing
  its layout.
- The unused FFmpeg `AVFrame`/`AVPacket` decode wrappers and decoder registry
  entries are disabled, matching the standalone subset pattern in
  `atrac3plusdec.c`. They are only wrapped in `#if 0` ... `#endif` (each
  block marked `LOCAL DELTA` at its `#if 0`); the code inside is the upstream
  n4.4 bytes. (2026-11-17: the disabled `atrac3_decode_frame()` wrapper had lost
  two upstream comments, some blank lines and its `"Frame decoding error!"`
  log; those upstream bytes are restored.)
- 2026-11-21 (vendored-update lane; new bytes need the maintainer's approval at
  landing): `decode_channel_sound_unit()` sizes the inverse transform from
  `subband_tab[num_subbands + 1]`, the one-line change of FFmpeg `master`
  commit `0828a3b6366042a2d65188c89a1f5bf0ae52ec75` ("avcodec/atrac3: fix
  inconsistent band num calculation", 2025-08-29; first in release `n8.1`,
  not on `release/4.4`), applied as upstream wrote it, under a `LOCAL DELTA`
  comment. `decode_spectrum()` codes subbands `0..num_subbands` inclusive, so
  the last coded subband ends at `subband_tab[num_subbands + 1]`; n4.4 used
  `subband_tab[num_subbands]` and, when that last subband is the first of a
  256-line QMF band (subbands 18, 26 and 30), zeroed the band and dropped its
  coded lines. Upstream changed its FATE references for the ATRAC3 samples
  with the fix. The commit was fetched from `https://git.ffmpeg.org/ffmpeg.git`
  (upstream `atrac3.c` blob `fe156fa4821e17e44f86927199939713b97bf4e2` before
  it, `d91bdc79ac9e88366b9ced89f2ec9763d8e2d7ad` after; the patch, as
  `git show 0828a3b6 -- libavcodec/atrac3.c` prints it, has SHA-256
  `e0256cf6b5b57f6880199ef18bfd6fc8736382e32b4b52fc4584faadfec9b759`).
  The hunk applies to the n4.4 `atrac3.c` without fuzz (`git apply --check`,
  two lines earlier than in the parent). `test_at3_last_coded_subband_decoded` in
  `tests/native/atrac3p_selftest.c` decodes one synthetic mono sound unit per
  such subband through `atrac3_decode()`; on the n4.4 line each decodes to
  all-zero PCM. This is FFmpeg's fix, not a PSP measurement.

### `libavcodec/atrac3plus.c`

- Added `#include "internal.h"` (marked `LOCAL DELTA`). Upstream n4.4 calls
  `avpriv_report_missing_feature()` without a declaring include (tolerated as
  an implicit declaration upstream; the standalone subset builds with
  `-Werror=implicit-function-declaration`).

### `libavcodec/sinewin.c`

- Added `#include "libavutil/mathematics.h"` (marked `LOCAL DELTA`, placed
  before `sinewin_tablegen.h`, which uses `M_PI`). MinGW-w64 UCRT `<math.h>`
  never exposes `M_PI` even with `-D_USE_MATH_DEFINES` (FFmpeg configure
  adds that define only for MSVC, configure line 5664 of n4.4).

### `libavcodec/atrac3plusdec.c`

- Added `#include "../atrac3p_api.h"` (marked `LOCAL DELTA`).
- `atrac3p_context_size()`: exported helper returning `sizeof(ATRAC3PContext)`
  so the API consumer can size `priv_data` without including this private
  header.
- `atrac3p_init()`: exported initializer (upstream `atrac3p_decode_init()` was
  `static av_cold`). It also checks the IPQF transform's
  `ff_mdct_init(&ctx->ipqf_dct_ctx, 5, 1, 32.0 / 32768.0)`, whose result
  upstream ignores, and returns `AVERROR(ENOMEM)` when it fails, so
  `atrac3p_create()` fails. The second transform (`ctx->mdct_ctx`, through the
  `void` `ff_atrac3p_init_imdct()`) is still unchecked, as upstream.
- `atrac3p_close()`: exported destructor, now `void` (upstream
  `atrac3p_decode_close()` was `static` returning `int`; it always succeeded).
- `atrac3p_decode_frame()`: rewritten entry point. Upstream signature takes
  `AVFrame`/`AVPacket` and calls `ff_get_buffer()`; this subset writes planar
  float PCM into caller buffers (`out[ATRAC3P_MAX_CHANNELS]`), reports
  `*nb_samples`, zeroed on entry and set to `ATRAC3P_FRAME_SAMPLES` only on
  success, and returns `FFMIN(block_align, buf_size)` consumed bytes or a
  negative `AVERROR`. This shape (the plane-output signature, `*nb_samples = 0`
  on entry in place of `ff_get_buffer()`, `*nb_samples = ATRAC3P_FRAME_SAMPLES`
  in place of `*got_frame_ptr = 1`, and the `FFMIN` return without the
  upstream ATRAC3PAL `avpkt->size` branch) follows PPSSPP commit
  `39b884cfb35724ecd977901ecf6921f120e06573` ("Atrac3/3+: Improve error
  handling"), `ext/at3_standalone/atrac3plusdec.cpp`, which is itself
  FFmpeg-derived LGPL code. It is comparative source, not PSP measurement.
- `atrac3p_decode_frame()` (2026-11-17): after the channel-unit loop, the
  output planes that no unit of the frame wrote are cleared. A frame may end
  at `CH_UNIT_TERMINATOR` before every channel block has a unit; the loop
  writes only the planes of the units it decoded, and the API converts every
  plane, so those planes used to report the previous frame's PCM (also after
  `atrac3p_reset()`). They now report silence. The PSP's result for such a
  frame is `NOT_MEASURED`; `tests/native/atrac3p_selftest.c`
  (`test_short_frame_does_not_repeat_pcm`) is the regression.
- `atrac3p_flush_context()`: exported state reset, Nakagawa-authored. Upstream
  FFmpeg n4.4 has no flush path and PPSSPP's `atrac3p_flush()` is an empty
  stub. The implementation zeroes all per-frame and cross-frame history state
  (`gb`, `samples`, `mdct_buf`, `time_buf`, `outp_buf`, per-unit `prev_buf`,
  `ipqf_ctx`, `wave_synth_hist`, and per-channel `spectrum`,
  `wnd_shape_hist`, `gain_data_hist`, `tones_info_hist`) while keeping the
  transform/DSP contexts and static VLC tables alive.
- The upstream `AVCodec ff_atrac3p_decoder` / `ff_atrac3pal_decoder`
  registrations are kept in an `#if 0` block; the standalone subset is not a
  registered FFmpeg codec.

## Authored stand-ins

These replace configure-generated or FFmpeg-internal headers with minimal
standalone equivalents; each carries an SPDX header and documents its
contract:

- `libavcodec/config.h`, `libavutil/config.h`, `libavutil/avconfig.h` —
  authored equivalents of configure output. Notable choices: `CONFIG_SMALL 0`,
  `CONFIG_HARDCODED_TABLES 0`, `CONFIG_MEMORY_POISONING 0`, `CONFIG_MDCT 1`
  (REQUIRED: `ff_fft_init()` in `fft_template.c` assigns
  `s->imdct_calc`/`s->imdct_half` only under `#if CONFIG_MDCT`; without it
  every real channel-unit decode reaches `reconstruct_frame()` with NULL
  function pointers and crashes at RIP 0 — found by random-input fuzzing,
  2026-08-06), `CONFIG_SAFE_BITSTREAM_READER 1` (upstream configure default;
  keeps the bounds-checked bitreader), all arch/HAVE feature flags 0 except
  the allocator: on Windows `HAVE_ALIGNED_MALLOC 1` and `HAVE_MALLOC_H 1`
  (`_aligned_malloc`), elsewhere `HAVE_POSIX_MEMALIGN 1` with the other two
  0; `HAVE_FAST_CLZ`, `HAVE_FAST_64BIT` and `HAVE_LOCAL_ALIGNED`, which
  imported files test with `#if`, are defined 0 explicitly (2026-11-17;
  before that they were undefined, which `#if` also reads as 0);
  `FF_MEMORY_POISON 0x2a` (upstream n4.4 `mem.c` lines 129/334 uses it but
  only `libavutil/internal.h:88` defines it, which `mem.c` never includes —
  upstream fixed this in `0f78b26e9c`, 2024-03-28); `av_restrict` defined in
  `avconfig.h` and pulled in via `#include "avconfig.h"` at the top of
  `libavutil/config.h` (imported `float_dsp.h` uses `av_restrict` but includes
  only `config.h`).
- `libavcodec/avcodec.h` — minimal `AVCodecContext` subset
  (`priv_data`, `channels`, `channel_layout`, `block_align`, `flags`,
  `sample_fmt`, `codec_id`, `frame_number`, `extradata`, `extradata_size`),
  `AV_INPUT_BUFFER_PADDING_SIZE`, `AV_SAMPLE_FMT_FLTP`, and the ATRAC3/ATRAC3+
  codec IDs. `AV_CODEC_FLAG_BITEXACT` is
  intentionally a subset-local flag bit, not the upstream enum position.
- `libavcodec/internal.h` — static-inline `avpriv_report_missing_feature()`,
  `avpriv_request_sample()`, and DEBUG-gated `ff_dlog()`.
- `libavutil/avutil.h` — include umbrella for the subset.
- `libavutil/mathematics.h` — `M_PI` family guards.
- `libavutil/libm.h` — `math.h` + `mathematics.h` include shim.
- `libavutil/thread.h` — single-threaded `AVOnce`/`ff_thread_once()` contract
  (plain flag guard; concurrent first use is not thread-safe, documented in
  the header).

## Authored API layer

- `atrac3p_api.h` — public contract: opaque `Atrac3pHandle`;
  `atrac3p_create(channels, block_align, &h)` (validates channel counts
  {1,2,3,4,6,7,8} and `block_align > 0`), `atrac3p_decode(h, frame,
  frame_size, pcm_out, samples_out)` (one frame, bounded s16 PCM interleaved,
  sample count), `atrac3p_reset(h)`, `atrac3p_flush(h)`, `atrac3p_destroy(h)`;
  low-level exports `atrac3p_context_size/init/decode_frame/flush_context/
  close`. No PSP HLE state, no guest pointers, no C++ exceptions; `create`
  forces `AV_CODEC_FLAG_BITEXACT`.
- `atrac3p_api.c` — thin C wrapper: handle struct holds the `AVCodecContext`,
  planar float decode buffer, per-channel plane pointers, and a padded
  scratch copy of the input frame; the ATRAC3+ float→s16 conversion is
  `atrac3p_pcm_sample()`, interleaved into caller memory. Since 2026-11-17
  both conversions clamp the scaled float to [-32768, 32767] before
  `lrintf()` and map NaN to 0: before that `av_clip_int16(lrintf(x))` was
  applied to the raw product, and `lrintf()` is unspecified outside the range
  of `long` (32 bits on Windows) and for inf/NaN, so out-of-range decoder
  output gave 0 or wrapped instead of saturating
  (`tests/native/atrac3p_selftest.c`, `test_pcm_saturation`).
- `av_log()`/`av_vlog()` (also in `atrac3p_api.c`; upstream's live in the
  unimported `libavutil/log.c`): since 2026-11-17 a message above
  `AV_LOG_INFO` (upstream's default `av_log_level`) is dropped, so FFmpeg's
  `AV_LOG_DEBUG` lines from `atrac3_decode_init()` no longer reach stderr on
  every AT3 create and reset. Each kept call is one line, prefixed
  `"atrac3p: "` (a trailing space included) and ended with a newline if
  the format has none, written to stderr or to a sink installed with
  `atrac3p_set_log_sink()`. Before that
  the level was ignored and the format was written verbatim
  (`test_decoder_log_lines`).
- Ledger record: the public provenance ledger lists `atrac3p_api.c` and
  `atrac3p_api.h` under the decoder import's trusted record
  (`atrac3p-decoder-import`, class `upstream_derived`), the record they
  were admitted with, not under a project-authored record of their own. The
  `AUTHORED stand-in` kind in the manifest above describes their content (no
  FFmpeg n4.4 counterpart); giving them their own project-authored record is
  a maintainer admission step that has not been taken. The HLE bridge that
  drives these APIs, `src/rt/atrac3p_bridge.c`/`.h`, does have its own
  project-authored record (`atrac3p-bridge-hle`).
  Every public entry validates all arguments (including a NULL handle on
  `decode`, which previously dereferenced `h` before the check) before any
  dereference, so hostile callers get a clean `AVERROR(EINVAL)`.
- The same API files now also expose a separate plain-AT3 handle. Its
  project-authored configuration check (`atrac3_config_valid`) enforces exactly
  these conditions, each with its source:
  - RIFF format tag `0x0270`: public WAVE format registry (`WAVE_FORMAT_SONY_SCX`).
  - 1 or 2 channels, a 14-byte fmt extension, a non-zero frame factor (word
    [10-11]), joint stereo (non-zero word [6-7]) only with 2 channels, and
    `block_align` equal to 96, 152 or 192 × channels × frame factor: the pinned
    FFmpeg n4.4 `atrac3_decode_init` rules for the 14-byte WAV extension.
  - `block_align` ≤ 4096: a project-authored bound on the scratch frame copy.
  The extension words [0-1], [2-5], [8-9] and [12-13] are not gates, matching
  upstream, because their PSP-visible values are `NOT_MEASURED`. It converts FFmpeg's 1024 planar float samples
  per channel to interleaved s16 with the opposite sign:
  `atrac3_pcm_sample()` negates the float, scales it by 32768 and saturates
  it like `atrac3p_pcm_sample()` above. The
  negation is project-authored and MEASURED on a PSP-3000 (firmware 6.61,
  #1096, probe `at3-decode`): the PSP decoder's waveform is FFmpeg's negated.
  It never routes AT3 data through the ATRAC3+ context.
- HLE's first-frame trimming from an 8-byte RIFF `fact` chunk's delay `d` is
  MEASURED on a PSP-3000 (firmware 6.61, #1096, PSP-AT3-002
  `at3-delay-sweep`) and lives in `src/rt/hle_audio_codec.c`
  (`atrac3_first_frame_skip()`, `atrac3_first_frame_rule()`): for `d == 0`
  or `955 <= d <= 1797` the first decode drops the leading
  `(d + 69) mod 1024` samples of the first frame; for `1 <= d <= 954`
  `sceAtracDecodeData` returns `0x80630002` with no samples; `d >= 1855` is
  refused by `sceAtracSetData`. Delays `1798 .. 1854` (the measured length
  rule is applied) and a 12-byte `fact` chunk (the full decoder frame is
  kept) stay `NOT_MEASURED`, and the HLE reports `ATRAC3_NOT_MEASURED` once.

## Licensing notes

- Imported FFmpeg files retain their original upstream SPDX/LGPL headers.
- This directory is LGPL-2.1-or-later (see `LICENSE.LGPLv2.1.txt`); the
  authored files are SPDX-tagged accordingly and are licensed under
  LGPL-2.1-or-later to be consistent with the imported core.
- Repository-level declaration remains GPL-3.0-or-later (LICENSE, NOTICE.md);
  per-file SPDX is preserved and deliberate. PGF/PGD licensing blockers
  for the separate PGF/font and PGD/amctrl surfaces do not apply to this
  FFmpeg-derived subtree.

### `libavutil/mem.h`

- `av_size_mult()`: replaced upstream's glibc-inspired trick with the
  compiler's own `__builtin_mul_overflow()` where it exists (non-Intel GCC
  5.1 or later, or any compiler whose `__has_builtin` reports it), and with
  the division check `b > SIZE_MAX / a` otherwise (marked `LOCAL DELTA`). The
  trick skipped the division whenever both operands were below sqrt(SIZE_MAX)
  and then relied on `t / a != b`; the replacement decides every operand pair
  directly. `*r` is still written only on success, as upstream had it: the
  built-in stores its truncated product into a local, not into `*r`.
  Modified 2026-11-17 by the Nakagawa Recomp authors.

## Verification

- Every `imported byte-identical` entry verified by comparing `git hash-object
  <file>` (the checked-out LF content under `.gitattributes` `eol=lf`
  normalization) with `git rev-parse n4.4:<path>` in a clone pinned at `n4.4`
  (tag object `09c358362008e2d04cec8239526c6827543da4cf` → commit
  `dc91b913b6260e85e1304c74ff7bb3c22a8c9fb1`). 47/47 imported blobs verified
  byte-identical on 2026-08-06; 44/44 byte-identical blobs re-verified against
  the committed tree after LF normalization on the same date.
- 2026-10-20 AT3 import verification: all 50 imported manifest blob SHAs
  match the n4.4 tree; the 46 byte-identical rows match their local Git blob
  hashes, and all four `LOCAL DELTA` rows differ from their upstream blobs as
  documented. Of the AT3 additions, `atrac3.c` is a `LOCAL DELTA` row and
  `atrac3data.h` and `bytestream.h` are byte-identical rows.
- 2026-11-17: `LICENSE.LGPLv2.1.txt` became a manifest row, so the vendored
  byte gate covers it; its `git hash-object` equals `n4.4:COPYING.LGPLv2.1`
  (`58af0d3787aec7d6f8d833535d64879156e7ec23`).
- The subset (16 translation units: `atrac3p_api.c` + 15 imported `.c`
  files) compiles with MinGW-w64 gcc `-std=c11 -O1 -Wall -Wextra` and the
  smoke driver passes create/garbage/decode/reset/flush/destroy checks.
- 2026-11-17: `libavutil/mem.h` became a `LOCAL DELTA` row for its
  `av_size_mult()` overflow check, so the current manifest has 45
  `imported byte-identical` rows and five `LOCAL DELTA` rows. The 2026-10-20
  note above records that run's counts (46 and four), not the current ones;
  its byte-identical verifications still stand for the rows it covered.
