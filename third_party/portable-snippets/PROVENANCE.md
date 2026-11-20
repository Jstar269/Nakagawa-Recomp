# PROVENANCE.md — `third_party/portable-snippets/`

The overflow-checked arithmetic header from Portable Snippets, vendored for the
checked size arithmetic in `src/core/nk_xb.c` (`checked_mul_size`, the XB
filesystem table size) and `src/core/nk_iso.c` (`nk_iso_checked_add_u64`, the
ELF program and section header table ends). Only `safe-math` is imported; the
other snippets have no use in the project yet.

## Upstream source

- Project: Portable Snippets
- Repository: `https://github.com/nemequ/portable-snippets`
- Pin: commit `84abba93ff3d52c87e08ba81de1cc6615a42b72e` (2021-03-15), the
  head of the default branch `master` at the time of import. Upstream has no
  release tags.
- License: `CC0-1.0`. `safe-math.h` carries its own CC0 1.0 waiver in its
  header comment; `COPYING.md` explains that each file's preamble governs it
  and reproduces the CC0 1.0 Universal text. The project adopted this import
  under the owner's 2026-10-24 rule that officially public-domain code (CC0,
  Unlicense, an explicit public-domain dedication) may be vendored with the
  normal bookkeeping.
- Import method: both files were downloaded from
  `raw.githubusercontent.com` at the pinned commit. Their Git blob IDs match
  the files in that commit, and the SHA-256 values below were calculated from
  the local bytes.

## File manifest

| File | Kind | Upstream blob SHA | SHA-256 |
| --- | --- | --- | --- |
| `safe-math/safe-math.h` | imported byte-identical | `d67a985345500f9111d315c6c63380c4a06ff821` | `f0ec77d1f12954a03301bcf39cd58a4b266639cf403e469df50f49a107f55d0b` |
| `COPYING.md` | imported byte-identical | `77b4b2193e04b7734a12e3dc8b055cdc1f46c9b8` | `ff5ac505f6fd6dd3c841f210db469a5dd2fc00a81559b0487c15357f6e2bd331` |
| `PROVENANCE.md` | project-authored manifest | n/a | n/a |

## Local deltas

None. The header declares `__int128` helper types, which GCC reports under
`-Wpedantic`, so the project's call sites wrap the include in
`#pragma GCC diagnostic ignored "-Wpedantic"` instead of editing the header.
`tests/native/test_safe_math.c` (run by `native-core-tests`) includes it the
same way under `-Wpedantic -Werror` and checks the size and 64-bit edges those
call sites rely on.

## Supported surface and known upstream defects

The project uses two typed helpers, `psnip_safe_size_mul` and
`psnip_safe_uint64_add`, and only on its GCC, Clang and MinGW-w64 builds, where
the header defines `PSNIP_SAFE_HAVE_BUILTIN_OVERFLOW` and each helper is a
`__builtin_*_overflow` call. Everything else in the header is unsupported here.
The pinned revision is still upstream's `master`, so none of the defects below
has an upstream fix; they are kept byte-identical rather than edited, because no
project path reaches them. Checked 2026-11-17 against the file in this tree:

- **Do not define `PSNIP_SAFE_STATIC_INLINE`.** `PSNIP_SAFE__FUNCTION`, which every
  helper definition uses, is defined only inside
  `#if !defined(PSNIP_SAFE_STATIC_INLINE)` and nothing maps the override onto it,
  so the header stops compiling.
- **Do not define `PSNIP_SAFE_FORCE_PORTABLE`, or build with a compiler that has
  neither the overflow builtins nor `intsafe.h`.** The portable fallbacks are
  then compiled, and two of them are wrong:
  - The promoted ("larger type") fallbacks are dead. The header defines
    `PSNIP_SAFE_HAVE_INT8_LARGER` ... `PSNIP_SAFE_HAVE_INT64_LARGER` and
    `PSNIP_SAFE_HAVE_LARGER_SHRT`/`USHRT`, but the users test
    `PSNIP_SAFE_HAVE_LARGER_INT8` ... `UINT64` and `PSNIP_SAFE_HAVE_LARGER_SHORT`/`USHORT`,
    names nothing defines, so the division-based fallbacks run instead.
  - The unsigned multiply fallback (`PSNIP_SAFE_DEFINE_UNSIGNED_MUL`) computes
    `(T) (a * b)` before its overflow check. For `unsigned short` and
    `psnip_uint16_t` both operands promote to signed `int`, so
    `psnip_safe_uint16_mul(&r, 0xffff, 0xffff)` overflows `int`, which is undefined
    behaviour. Reproduced 2026-11-17 with Linux GCC,
    `-DPSNIP_SAFE_FORCE_PORTABLE -fsanitize=signed-integer-overflow`: "signed
    integer overflow: 65535 * 65535 cannot be represented in type 'int'" at the
    `psnip_uint16_t` instantiation.
- **Do not use the type-generic `psnip_safe_div` or `psnip_safe_mod`.** With the
  builtins they expand to `__builtin_div_overflow` and `__builtin_mod_overflow`,
  which neither GCC nor Clang provides, so a use fails to compile or link. Use a
  typed helper instead.
- **Do not define `PSNIP_SAFE_EMULATE_NATIVE` or `PSNIP_BUILTIN_EMULATE_NATIVE`.**
  The guard that should skip the emulation tests `PSNIP_SAFE_HAVE_BUILTINS`, a
  name the header never defines (it defines `PSNIP_SAFE_HAVE_BUILTIN_OVERFLOW`),
  so on GCC or Clang the header would `#define __builtin_sadd_overflow` and the
  other reserved builtin names over the real intrinsics.

A project use outside this surface first needs a recorded LOCAL DELTA that fixes
the defect it would reach.
