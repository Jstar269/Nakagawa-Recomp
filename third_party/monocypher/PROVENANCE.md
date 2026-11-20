# PROVENANCE.md — `third_party/monocypher/`

The Ed25519 signature check and BLAKE2b hash from Monocypher, vendored for the player's signed update download
([`docs/RELEASE_SIGNING.md`](../../docs/RELEASE_SIGNING.md); [`docs/ONLINE_FEATURES.md`](../../docs/ONLINE_FEATURES.md#implementation-order),
item 3). The project wrappers are `nk_ed25519_verify` and `nk_minisign_verify` in `src/core/nk_sig.c`; the player's
update check (`src/player/update_check.c`) calls them to check a release's minisign signature, and the native
selftest `tests/native/test_nk_sig.c` (`ed25519-verify-selftest`, also run by `native-core-tests`) tests them. The
vendored files contain no network code and no key.

## Upstream source

- Project: Monocypher
- Repository: `https://github.com/LoupVaillant/Monocypher`
- Pin: release tag `4.0.3`, commit `ab2b16dd619ad5f6979a4fbe69cfa324a6fcc35f` (published 2026-06-15, the newest
  release at the time of import).
- License: dual `BSD-2-Clause OR CC0-1.0` (each file's SPDX line; `LICENCE.md` lets the user choose). The project
  elects **CC0-1.0**, the public-domain dedication, under the maintainer's 2026-10-24 rule for officially
  public-domain code. The complete upstream `LICENCE.md`, with both licences and the author list, is kept beside the
  sources unchanged.
- Import method: the four source files and `LICENCE.md` come from the Git tree at the pinned commit
  (`raw.githubusercontent.com` at `ab2b16dd`, or the Git blobs). Their Git blob IDs match the pinned commit's tree,
  and the SHA-256 values below were calculated from the local bytes. The tag's release tarball
  (`monocypher-4.0.3.tar.gz`) is not the same bytes: its release script replaces `__git__` in the first line of each
  source file with the version (`// Monocypher version 4.0.3`), and its `LICENCE.md` lacks the six-line "Special
  notes" section at the end. Re-verify the SHA-256 column against the Git tree, never against the tarball.
  (Corrected 2026-11-17: this line said the tarball and the commit copies were identical; checked against the
  release asset, they are not.)
- Only what Ed25519 verification needs is vendored: the core library (`monocypher.c`, `.h`) and the optional
  Ed25519 file (`src/optional/monocypher-ed25519.c`, `.h`), which supplies the SHA-512 that RFC 8032 Ed25519
  requires (the core library's EdDSA uses BLAKE2b). The optional pair is flattened into this directory; its
  `#include "monocypher.h"` resolves here without an edit.

## File manifest

| File | Upstream path | Kind | Upstream 4.0.3 blob SHA | SHA-256 |
| --- | --- | --- | --- | --- |
| `monocypher.c` | `src/monocypher.c` | imported + LOCAL DELTA | `c7c5450e84e4dffda44514d9f918162f1830de56` | `97ff742e46e6c921d8df73f4099a804bff5d65501eb7a94b7574e9593aca1439` (this tree's bytes) |
| `monocypher.h` | `src/monocypher.h` | imported byte-identical | `cf635e88e4f5851e1f58a9e2b3e85b8029a73861` | `fcaf6ed771358bb4f40fba016f6518ae86ec02b1b877d2cc35ad92d3a26fd7b3` |
| `monocypher-ed25519.c` | `src/optional/monocypher-ed25519.c` | imported byte-identical | `4fdd07641e1c4d54a348e95d0ab05c28f84654a6` | `ce0d2f8e32ca8f66398ba5b3456cc74327c3eff14e7b950ce7d57be9025cc453` |
| `monocypher-ed25519.h` | `src/optional/monocypher-ed25519.h` | imported byte-identical | `d7aa004123e1d38b891fdabd4171fbf9957242ae` | `3a3035181f991a158d0e1c7567258f0bae8ba0f1f23c5512b4a1db1b3c9730ce` |
| `LICENCE.md` | `LICENCE.md` | imported byte-identical | `9d69ad72ec113a743264d34dd86a5c04d0c42d35` | `a5781770269d2516e52ba4863f790c10a16da4089a1e81823aee19ff1e9026b0` |
| `PROVENANCE.md` | n/a | project-authored manifest | n/a | n/a |

## Local deltas

The files build warning-free with `-std=c99 -Wall -Wextra -Wpedantic` under MinGW-w64 GCC and Linux GCC, so no
compile flag is needed. One file is modified.

### `monocypher.c`: constant-time `neq0`

`monocypher.c` was modified by Nakagawa Recomp contributors on 2026-11-20 (upstream freshness, the maintainer's
standing decision of 2026-11-19 to take upstream fixes). Its upstream 4.0.3 bytes have SHA-256
`f1f838cdd483bdebe0df0ff5c5ed60535e496f769c6a2f933ac4c0b114207123`; the table gives this tree's. The change is
upstream commit `55158d37f4d86a536fb807096fa9cacc7d4469db` ("Fix timing leak in constant time comparisons",
committed 2026-10-01, in no release yet; `4.0.3` is still the newest tag on 2026-11-20, and `LICENCE.md` is
unchanged at `master` `ab22d3c2dc14e4c5e2b069134aa7db4c57b0ac57`), applied unchanged; the only other difference
from that commit's `src/monocypher.c` is the three-line `LOCAL DELTA (Nakagawa Recomp)` comment above `neq0`. A
pin that includes the commit drops this delta.

- `neq0`, the helper behind `crypto_verify16/32/64`, folds the difference to 32 bits and takes the sign bit of
  `fold | -fold`. The pinned form computed the same result in a way that, depending on compiler and target, could be
  lowered to a data-dependent branch, leaking whether two buffers matched. The project's own use was not exposed
  (`nk_sig.c` verifies public Ed25519 signatures over public data, and Ed25519's final check compares public
  values), but the fix keeps any future use on secret data constant-time. A scratch check gave the same result from
  both forms for 50,000,072 inputs (edge values, every single-bit value, and pseudo-random values).

## Upstream changes not included

- `f5f0a19a5dc6c14a49adb3cd63ea79a60f6ba77b` (2026-10-20, "Skip field inversion in signature check") changes how
  `crypto_eddsa_check_equation` tests its final point, for about 6 % faster verification. It is an optimisation of
  the signature path, not a fix, and waits for a release.
- The other commits after the pin change Argon2 and X25519 code this project does not call, comments and the C++
  namespace guards.
