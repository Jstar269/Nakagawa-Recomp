/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Ported from kl4e.h of John-K/pspdecrypt
 * (https://github.com/John-K/pspdecrypt), commit
 * c156627db7634d395c380c0a9589130f603307fc.  Upstream carries no
 * per-file licence header; the repository's LICENSE.TXT is the GNU GPL
 * version 3 (the Readme states "Licensed under GPLv3"), so this port is
 * labelled GPL-3.0-only.  Original author artart78; his notice is kept
 * verbatim in nk_psp_kle.c.
 *
 * Modified for this project: header guard, SPDX/attribution, and an
 * include of the shared crypto contract for the u8 type alias.
 * 2026-11-18: the comment on decompress_kle states its real return values.
 */

#ifndef NK_PSP_KLE_H
#define NK_PSP_KLE_H

#include "nk_psp_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decompress a KL4E/KL3E stream.  inBuf points PAST the 4-byte magic
 * ("KL4E"/"KL3E") and inSize is how many compressed bytes are readable
 * from there; the decoder is fail-closed and never reads past inBuf +
 * inSize, so a truncated or malformed stream cannot read out of bounds.
 * isKl4e selects the variant.  On success returns the number of decoded
 * bytes (> 0) and sets *end to the end of the consumed input.  On a
 * malformed or truncated stream returns a negative SCE error
 * (0x80000104 SCE_ERROR_INVALID_SIZE or 0x80000108
 * SCE_ERROR_INVALID_FORMAT).  An empty stored stream returns 0, which
 * callers treat as a failure.
 */
int decompress_kle(u8 *outBuf, int outSize, u8 *inBuf, int inSize,
                   void **end, int isKl4e);

#ifdef __cplusplus
}
#endif

#endif /* NK_PSP_KLE_H */
