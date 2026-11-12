/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * Minimal, fail-closed DEFLATE/gzip/zlib inflater for the built-in PSP
 * decryption boundary (issue #295).  Project-authored from the public
 * RFC 1950 (zlib), RFC 1951 (DEFLATE) and RFC 1952 (gzip)
 * specifications; no third-party inflate source was copied.
 *
 * Contract: on success returns 0 and hands the caller a malloc'd buffer
 * (caller frees).  Every malformed, truncated, checksum-mismatched or
 * over-capacity stream returns a negative code with a fail-closed
 * message; nothing is ever partially returned.
 */

#ifndef NK_PSP_INFLATE_H
#define NK_PSP_INFLATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NK_INFLATE_OK = 0,
    NK_INFLATE_ERR_FORMAT = -1,   /* bad magic/blocks/huffman codes */
    NK_INFLATE_ERR_TRUNCATED = -2,/* input ended mid-stream */
    NK_INFLATE_ERR_OVERFLOW = -3, /* output exceeded the cap */
    NK_INFLATE_ERR_CHECKSUM = -4, /* CRC32/adler32/ISIZE mismatch */
    NK_INFLATE_ERR_MEMORY = -5
};

/*
 * Decompress ``src`` (gzip, zlib or raw-deflate auto-detected from the
 * leading bytes) into a freshly malloc'd buffer.
 *
 * ``expect`` is a size hint for the initial allocation (0 = default);
 * ``cap`` is the hard fail-closed output ceiling (0 = 64 MiB).
 * On success returns 0, ``*out``/``*outlen`` own the result.
 * On failure returns NK_INFLATE_ERR_* and fills err (when provided).
 */
int nk_psp_inflate(const uint8_t *src, size_t srclen,
                   uint8_t **out, size_t *outlen,
                   size_t expect, size_t cap,
                   char *err, size_t err_len);

/*
 * The same contract for a raw DEFLATE stream with no zlib or gzip wrapper,
 * which a CSO disc-image block is: the wrapper is never guessed from the
 * leading bytes. Bytes after the final block (alignment padding) are ignored.
 */
int nk_psp_inflate_raw(const uint8_t *src, size_t srclen,
                       uint8_t **out, size_t *outlen,
                       size_t expect, size_t cap,
                       char *err, size_t err_len);

/*
 * A raw DEFLATE stream unpacked into the caller's buffer of ``dst_cap``
 * bytes, which is the hard ceiling: nothing is allocated for the output, and
 * a stream that would write past it fails with NK_INFLATE_ERR_OVERFLOW.
 * ``*outlen`` is the bytes written (on failure too, and the buffer then holds
 * them, not a result). ``*consumed``, when given, is the input bytes the
 * stream used, up to and including the byte that holds its last bit.
 */
int nk_psp_inflate_raw_into(const uint8_t *src, size_t srclen,
                            uint8_t *dst, size_t dst_cap, size_t *outlen,
                            size_t *consumed, char *err, size_t err_len);

#ifdef NK_INFLATE_COUNT_BUILDS
/* Huffman code tables built so far, in a build that counts them (tests). */
extern unsigned long long nk_inflate_table_builds;
#endif

#ifdef __cplusplus
}
#endif

#endif /* NK_PSP_INFLATE_H */
