# PROVENANCE.md — `third_party/dr_libs/`

The single-file WAV reader and writer from dr_libs, vendored for selftests
only. `tests/native/pcm_wav_dump.c` uses its writer to save decoded PCM (the
ATRAC3+ bridge selftest's canaries today) as WAV files when `NK_PCM_WAV_DIR`
is set, and `tests/native/test_pcm_wav_dump.c` reads those files back. It is
not compiled into the native player, the runtime, or any shipped artifact.

## Upstream source

- Project: dr_libs (dr_wav)
- Repository: `https://github.com/mackron/dr_libs`
- Pin: tag `wav-0.14.6`, commit `35b5c392cc15456736c440a68dbe073178bbffe0`
  (2026-10-30, "dr_wav: Version 0.14.6"), also the head of the default branch
  `master` when re-pinned on 2026-11-05. The header names its version
  `v0.14.6 - 2026-10-31`. The previous pin was the untagged pre-release commit
  `d422edf14ec2b81ac40a7f22b5a7f64276b5fc34` (2026-10-23, header
  `v0.14.6 - TBD`); the release adds fixes to fact-chunk sample counts, AIFF
  ADPCM/a-law/mu-law decoding, u8-to-f32 conversion, reads into a NULL buffer
  and metadata parsing. None of those paths is used here: the selftests write
  and read back RIFF PCM through `drwav_write_pcm_frames` and
  `drwav_read_pcm_frames_s16` into real buffers, without metadata.
- License: the choice of `Unlicense` (public domain) or `MIT-0`. The project
  elects the Unlicense (public domain) option, under the owner's 2026-10-24 rule
  that officially public-domain code may be vendored with the normal
  bookkeeping. Both alternatives are kept, unchanged, at the end of `dr_wav.h`
  and in `LICENSE.txt`.
- `LICENSE.txt` is also the licence text shipped for dr_flac, the FLAC reader libchdr carries
  (`assets/third_party_components.json`, component `libchdr-dr-flac`; see
  `third_party/libchdr/PROVENANCE.md`), which is compiled into the native player. It offers the same
  two alternatives as the end of `dr_flac.h`, except that its MIT-0 copyright line reads
  "Copyright 2020 David Reid" where `dr_flac.h` reads "Copyright 2023 David Reid"; the Unlicense is the
  elected option. A re-pin or removal of this file changes that shipped notice too.
- Import method: both files were downloaded from `raw.githubusercontent.com`
  at the pinned commit (`LICENSE` is unchanged since the previous pin). Upstream `LICENSE` is stored as `LICENSE.txt`, bytes
  unchanged, so the publication classifier recognises it as a licence text.
  Their Git blob IDs match the files in that commit, and the SHA-256 values
  below were calculated from the local bytes.

## File manifest

| File | Kind | Upstream blob SHA | SHA-256 |
| --- | --- | --- | --- |
| `dr_wav.h` | imported + LOCAL DELTA | `f7920db42ff78e66cb74489435f8772ccc4d4977` | `6aa4fb378f6e7352b65cfcf61e3a6fd32ba4b07e30d40712fe837809ef0562bc` (this tree's bytes) |
| `LICENSE.txt` | imported byte-identical (upstream `LICENSE`) | `16dfbdfda2b761d1ec2ed57266dbdc777349ad25` | `dd1c647e6f767f8ff4b2dfae0fed314726600a01e0cf1ef556afddd5fa96ff15` |
| `PROVENANCE.md` | project-authored manifest | n/a | n/a |

## Local deltas

`dr_wav.h` was modified by Nakagawa Recomp contributors on 2026-11-20 (upstream freshness, the maintainer's
standing decision of 2026-11-19 to take upstream fixes). Its `wav-0.14.6` bytes have SHA-256
`fad60f1e360d6de726efc1b9a7985610c89c551719e3f110e8b992f0e1f308cd`; the table gives this tree's. `wav-0.14.6` is still
the newest dr_wav tag on 2026-11-20, and `LICENSE` is unchanged at `master`. The changes are the four upstream
commits to `dr_wav.h` after the tag, every change on `master` up to `e381981afa52800b9a3864df548353ff67252fbc`
except the version bump: the file's first comment still names `v0.14.6 - 2026-10-31` and
`DRWAV_VERSION_REVISION` stays 6, since these are fixes taken onto the release, not upstream's unreleased 0.14.7.
Apart from those two lines, four one-line `LOCAL DELTA (Nakagawa Recomp, 2026-11-20)` comments, one at each
commit's main change, and a four-line `Modified by Nakagawa Recomp contributors, 2026-11-20.` notice comment right
after upstream's opening header comment, the file is upstream's `master` file at `e381981a`. A pin that includes
the commits drops this delta and the notice.

- `ebfea72a291322bf26ce4eb393c98a0262b9b561` (2026-11-09, "Fix some metadata parsing errors", upstream issue
  #353): `drwav__metadata_process_chunk` reads a `LIST` sub-chunk's ID and size only while the chunk has room for
  them, and stops when the sub-chunk and its padding do not fit in the chunk.
- `3a3e120a7d9d802b8e7d91f3a9ab83a628cb70d4` (2026-11-18, "Fix an endian error with s24 conversion", issue #362):
  `drwav_s24_to_s16/f32/s32` read packed 24-bit samples in the host's byte order through one helper; on a
  big-endian host they read them as little-endian.
- `0901cdc54e719e2175827cb60b78fe725e581fe2` (2026-11-18, "Fix an endian error with Wave64 containers"): a Wave64
  `fact` chunk's sample count is decoded from little-endian bytes instead of copied into a host integer.
- `e381981afa52800b9a3864df548353ff67252fbc` (2026-11-20, "Minor rework to initialization for badly formed
  files"): `drwav_init__internal` learns the file size before reading chunks (and fails if it cannot seek back to
  the start), bounds the data chunk by it, treats a RIFF or RIFX data size of `0xFFFFFFFF` as "to the end of the
  file", and `drwav__on_tell_stdio` reports a failed `ftell`.

Only selftests compile the file, and they use the writer and read their own RIFF PCM files back
(`tests/native/pcm_wav_dump.c`, `tests/native/test_pcm_wav_dump.c`); the WAV files the ATRAC3+ bridge selftest
writes with `NK_PCM_WAV_DIR` set are byte-identical before and after this change. The endian fixes matter only on a
big-endian host, and the metadata parser is not used here.

Five whitespace-only lines in `dr_wav.h` (2677, 2694, 3847, 3865 and 6570; the third and fourth come from
`e381981a`) end in spaces; the repository's `.gitattributes` exempts that one file from the trailing-space check so
the bytes stay identical to upstream.
