# PROVENANCE.md — `src/rt/minimp3/`

Single-header MPEG-1/2/2.5 audio decoder (Layers I, II and III) for the
Nakagawa recomp runtime. It decodes the frames behind the PSP `sceMp3`
library. The header is imported with two upstream lines from after the pin
(LOCAL DELTA, below); the project's narrow wrapper is `src/rt/mp3_bridge.c` and
`src/rt/mp3_bridge.h`, which are project-authored and live outside this
directory.

## Upstream source

- Project: minimp3
- Repository: `https://github.com/lieff/minimp3`
- Pin: commit `ea99364f61c14656440e8d77e9c233ccf3124633` (the `master` head when
  imported on 2026-10-24; committed 2026-07-27, "Don't enable ARMv6 features for
  ARMv6-m"). Blob SHAs below are `git rev-parse <commit>:<path>` against that
  commit's tree.
- Licence: CC0 1.0 Universal public-domain dedication (`LICENSE`, imported
  byte-identical from the same commit). The dedication notice at the top of
  `minimp3.h` is kept unchanged.
- Owner decision 2026-10-24: officially public-domain code may be vendored
  without a per-import request, with the normal pin, manifest, credit and
  admission.
- Import method: `git clone` of the upstream repository, files copied from the
  pinned tree, line endings normalised to LF by this repository's
  `.gitattributes` (upstream stores LF; the blob hashes match).

## File manifest

| File | Kind | Upstream blob SHA at the pin | SHA-256 |
| --- | --- | --- | --- |
| `LICENSE` | imported byte-identical | `2c4afabdb632b5d68e60c9fe9fe0b0819676a729` | n/a |
| `minimp3.h` | imported + LOCAL DELTA | `9fbd4d2cc50c06cc3e63dcafdd506dc6512b2103` | `af6bd3d32b0b4cd1ab73f9b25e7ed3a966dc246c623dcd70f6ce98b29a0a1ddc` |

The upstream `minimp3.h` bytes at the pin have SHA-256
`57e437c5c1f0e8b243885d3929c8973b5e6c778451e0100ab4251d19915cb3ad`; adding the
two lines below, each after a one-line `LOCAL DELTA (Nakagawa Recomp)` marker
comment, gives the SHA-256 in the table. Upstream's `minimp3_ex.h` (file and
seek helpers), tests, fuzzers, player and vectors are not imported: the runtime
feeds frames from guest memory and needs only the frame decoder.

## Local deltas

Modified 2026-11-18 (audit 2026-11-17): two upstream security fixes committed
after the pin are applied as upstream wrote them, line for line, without
taking the rest of upstream's later history.

- `mp3dec_decode_frame`: `memset(scratch.ist_pos, 0, sizeof(scratch.ist_pos));`
  from upstream commit `a01e51e230b58a27f234b60ceece2e76f501f517` ("security:
  fix use-of-uninitialized-value in ist_pos (scfsi reuse)"). In an MPEG-1
  stereo frame the three private bits become granule 0's right-channel scfsi,
  so a frame with them set reused scalefactors from stack scratch nothing had
  written, and the PCM handed to `sceMp3` followed the host stack.
  `tools/test_mp3_tone_fixture.py` decodes such a frame after filling the stack
  with different bytes and fails on the pinned bytes.
- `mp3d_synth_granule`: `memset(lins + 15*64, 0, sizeof(float)*64*nbands);`
  from upstream commit `19ff674dfc0b9fc28760a5bd1429d7e161272e91` ("security:
  fix uninitialized stack data in qmf_state (mp3d_synth_granule)"). The
  synthesis workspace is zeroed before it is copied back into `qmf_state`. No
  path was found where the unwritten lanes reach the output, so no test fails
  on the pinned bytes for this line; it is kept with its upstream partner so
  the decoder matches upstream's fixed state.

## Build configuration

`src/rt/mp3_bridge.c` is the only translation unit that defines
`MINIMP3_IMPLEMENTATION`. It leaves the upstream defaults in place (integer
`int16_t` output; SSE2/NEON paths where the compiler enables them), so the
decoder is compiled as upstream ships it, with the two lines above.

## Verification

```sh
sha256sum src/rt/minimp3/minimp3.h         # af6bd3d32b0b4cd1ab73f9b25e7ed3a966dc246c623dcd70f6ce98b29a0a1ddc
git hash-object src/rt/minimp3/LICENSE     # 2c4afabdb632b5d68e60c9fe9fe0b0819676a729
```

Removing each `LOCAL DELTA (Nakagawa Recomp)` marker line and the line after it
gives back the pinned upstream blob `9fbd4d2cc50c06cc3e63dcafdd506dc6512b2103`.
