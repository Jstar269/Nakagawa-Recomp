# PROVENANCE.md — `fixtures/homebrew_corpus/`

The public PSP homebrew corpus (#2144): open-source PSPSDK sample programs that the no-title-hacks smoke builds and runs. **No upstream source is copied into this repository.** The programs ship in the PSPDEV release archive that [`assets/upstream/pspdev.lock.json`](../../assets/upstream/pspdev.lock.json) already pins by SHA-256 (the same pinned download CI uses for the showcase and profile-zero builds), and `corpus.json` names each file by its Git blob ID. A test (`tools/test_homebrew_corpus.py`) recomputes those IDs from an installed PSPDEV, so a toolchain carrying other bytes fails instead of being used.

## Upstream source

- Project: PSPSDK (the PSPDEV project)
- Repository: `https://github.com/pspdev/pspsdk`
- Pin: commit `687501d09733390ad75d855f73d69a0672b93a12` (the commit `assets/upstream/pspdev.lock.json` records for `pspsdk`)
- License: BSD-3-Clause. The root `LICENSE` (Git blob `6a7c199bb9063529bbb70c1387122adf88c96c42`) is a 3-clause BSD grant; every sample file carries the header "Licensed under the BSD license, see LICENSE in PSPSDK root for details". The BSD licence is GPL-3.0-or-later compatible (AGENTS.md Section 4). The separately GPL-3.0-only `tools/PrxEncrypter` is not part of the corpus.
- Delivery: PSPDEV release `v20261101`, asset `pspdev-ubuntu-latest-x86_64.tar.gz`, SHA-256 `5dc9d785a5fd490ee9d5204b90f715efb6c46940ba3944036bbef9c993c17713`, installed under `psp/sdk/samples`.
- Verification: all 12 files below were checked byte-identical to the pinned PSPSDK commit by comparing the Git blob ID of the installed file with the blob ID GitHub reports for `src/samples/...` at that commit (2026-11-08). An installed sample `Makefile` is upstream's `Makefile.sample`.

## File manifest

Kind for every row: **referenced byte-identical**, not copied. Local deltas: none.

| Program | Installed file (below `psp/sdk/samples`) | Upstream path at the pin | Git blob ID |
| --- | --- | --- | --- |
| `gu-cube` | `gu/cube/Makefile` | `src/samples/gu/cube/Makefile.sample` | `b21accdc19a117dd23c14d42e6b84b9d5a3763c4` |
| `gu-cube` | `gu/cube/cube.c` | `src/samples/gu/cube/cube.c` | `fc278924ec7f4f1065a373e0292308adb14ef54d` |
| `gu-cube` | `gu/cube/logo.raw` | `src/samples/gu/cube/logo.raw` | `fbac33e10e4dcf356365dab805ca39c271e1fcc3` |
| `gu-cube` | `gu/common/callbacks.c` | `src/samples/gu/common/callbacks.c` | `7233aa64852573bc7ae33e6448fbe2bf8884029a` |
| `gu-cube` | `gu/common/callbacks.h` | `src/samples/gu/common/callbacks.h` | `a6ecd7a66429c1c479a3a00abb9939655a542ed2` |
| `controller-basic` | `controller/basic/Makefile` | `src/samples/controller/basic/Makefile.sample` | `10971a9ca56347b0ab5e328e3eb14aae6baaaae2` |
| `controller-basic` | `controller/basic/main.c` | `src/samples/controller/basic/main.c` | `5df7169a3c7f6a664e0f026af79e960311e9af2e` |
| `audio-polyphonic` | `audio/polyphonic/Makefile` | `src/samples/audio/polyphonic/Makefile.sample` | `db3c90119d57ff0a6b21042ba2f111eb963130b5` |
| `audio-polyphonic` | `audio/polyphonic/main.c` | `src/samples/audio/polyphonic/main.c` | `1be775f1ea919698420343fe8b2035c7ee6d0c9f` |
| `savedata-utility` | `savedata/utility/Makefile` | `src/samples/savedata/utility/Makefile.sample` | `93404bbf20a07761ba0b69ad9b8dc807f72c1eae` |
| `savedata-utility` | `savedata/utility/main.c` | `src/samples/savedata/utility/main.c` | `bd90e36f96cdb9b9007055e79a30ccfe1ecaab16` |
| `savedata-utility` | `savedata/utility/data.h` | `src/samples/savedata/utility/data.h` | `1e627adcec724e1f88365345049686dd5dd5aa13` |

## Programs and copyright holders

- `gu-cube` (Cube Sample): copyright 2005 Jesper Svennevid.
- `controller-basic` (Basic controller sample): copyright 2005 Donour Sizemore; 2005 James Forshaw; 2005 John Kelley; 2005 Marcus R. Brown.
- `audio-polyphonic` (Polyphonic sample by Shine): copyright 2005 Frank Buss.
- `savedata-utility` (PSPSDK Savedata Sample): copyright 2005 weltall.

## Project-authored files

| File | Kind |
| --- | --- |
| `corpus.json` | project-authored: the selection, pins and expected results |
| `PROVENANCE.md` | project-authored manifest |
| `tools/homebrew_corpus.py`, `tools/test_homebrew_corpus.py` | project-authored loader, validator and tests |
