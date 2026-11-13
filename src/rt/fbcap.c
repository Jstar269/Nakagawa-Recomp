// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-10-08.
// See NOTICE.md for upstream lineage and modification provenance.
//
// fbcap.c - presenter-neutral frame capture service (issue #57)
//
// The capture state machine, the parent-directory helper and the atomic P6 publisher were
// moved here from src/rt/gpu_sdl3vk/sdl3vk.c, so this file keeps that file's notices.
//
// The single owner of capture state and of the P6 PPM publisher. Presenters only supply
// the pixels they presented (see fbcap.h for the arm/service/resolve protocol); they never
// hold capture state of their own. Host-only C: no SDL, Vulkan or guest-memory dependency,
// so fbcap-selftest drives it directly with synthetic frames.

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "fbcap.h"

#include "../core/nk_platform.h"   /* the wide-path (UTF-8) file calls: user data paths */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CAP_IDLE = 0,  /* nothing armed */
    CAP_ARMED,     /* armed; the presenter will service the next presented frame */
    CAP_RECORDED,  /* GPU readback recorded in a submitted present; waiting on its fence */
    CAP_DONE,      /* file published */
    CAP_FAILED     /* attempted and failed */
};

static int s_state = CAP_IDLE;
static int s_result;                 /* 1 published, 0 nothing attempted, -1 failed */
static char s_path[1024];
static sr_cap_source s_src = SR_CAP_SRC_NONE;
static uint32_t s_w, s_h;
static int s_probe;                  /* the pending request is a probe: no file */
static int s_probe_ready;            /* a probe result is waiting to be taken */
static uint32_t s_probe_brightest;
static int s_probe_in_flight;        /* a deferred probe's readback is still on the GPU */
static uint32_t s_probe_w, s_probe_h;
static SrCaptureProbeResolver s_probe_resolver;

static int cap_is_sep(char c) { return c == '/' || c == '\\'; }

/* 1 when `dir` is a directory after this call, created now or already there. A failed
 * mkdir is judged by what is on disk, not by its errno (nk_platform_mkdir_p): Windows reports
 * an existing drive root ("C:/") as EACCES, and an existing directory must never fail a
 * publish. An existing non-directory is not a directory, so the publish then fails cleanly.
 * The path is UTF-8 and is made through the platform layer's wide-path calls. */
static int cap_dir_ready(const char *dir) {
    return nk_platform_mkdir_p(dir) ? 1 : 0;
}

/* Create the parent directory of `path` (mkdir -p on the directory component only). FBSNAP
 * publishes under build/snapshots/, which no build step creates. Every prefix is checked
 * without a trailing separator; a root ("/", or a drive root such as "C:/") always exists
 * and is never created. Relative and absolute paths, with either separator, behave alike. */
static int cap_ensure_parent_dir(const char *path) {
    char dir[1024];
    size_t n = strlen(path);
    if (!n || n >= sizeof dir) return 0;
    memcpy(dir, path, n + 1);
    char *sep = NULL;
    for (char *q = dir; *q; q++)
        if (cap_is_sep(*q)) sep = q;
    if (!sep) return 1;                          /* bare file name: the current directory */
    while (sep > dir && cap_is_sep(sep[-1])) sep--;  /* "a//file" names directory "a" */
    *sep = '\0';
    n = (size_t)(sep - dir);
    if (n == 0 || (n == 2 && dir[1] == ':')) return 1;   /* a file directly under "/" or a drive root */
    for (char *q = dir + 1; *q; q++) {
        if (!cap_is_sep(*q) || cap_is_sep(q[-1])) continue;
        size_t len = (size_t)(q - dir);
        if (len == 2 && dir[1] == ':') continue;          /* drive root */
        char save = *q;
        *q = '\0';
        int ready = cap_dir_ready(dir);
        *q = save;
        if (!ready) return 0;
    }
    return cap_dir_ready(dir);
}

/* The single terminal transition out of the armed/recorded states. Every failure funnels
 * through here, so sr_capture_result() can never report a stale or invented outcome. */
static void cap_finish(int ok, const char *why) {
    if (s_state != CAP_ARMED && s_state != CAP_RECORDED) return;
    if (ok) {
        s_state = CAP_DONE;
        s_result = 1;
    } else {
        s_state = CAP_FAILED;
        s_result = -1;
        fprintf(stderr, "fbcap: present capture failed: %s\n", why ? why : "unknown reason");
    }
}

/* ---- PNG publication (screenshots) --------------------------------------------------------
 * A path ending in ".png" (any case) is published as a PNG: 8-bit RGB, no interlace, filter
 * type 0 on every row, and a zlib stream of stored (uncompressed) deflate blocks with its
 * Adler-32. Stored blocks keep the writer small and exact; the file is about as large as the
 * PPM would be. Chunk CRCs are the PNG CRC-32 (polynomial 0xEDB88320). */
static int cap_path_is_png(const char *path) {
    size_t n = strlen(path);
    if (n < 4u) return 0;
    const char *e = path + n - 4u;
    return e[0] == '.' && (e[1] == 'p' || e[1] == 'P') && (e[2] == 'n' || e[2] == 'N') &&
           (e[3] == 'g' || e[3] == 'G');
}

/* The PNG CRC-32 table (polynomial 0xEDB88320), constant: the texture dump's workers encode on
 * two threads at once, so it is never built at run time. fbcap-selftest checks every entry
 * against the bit-by-bit definition. */
static const uint32_t s_png_crc_table[256] = {
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u, 0x706AF48Fu,
    0xE963A535u, 0x9E6495A3u, 0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u,
    0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u, 0x1DB71064u, 0x6AB020F2u,
    0xF3B97148u, 0x84BE41DEu, 0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
    0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu, 0x14015C4Fu, 0x63066CD9u,
    0xFA0F3D63u, 0x8D080DF5u, 0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u,
    0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu, 0x35B5A8FAu, 0x42B2986Cu,
    0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
    0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u,
    0xCFBA9599u, 0xB8BDA50Fu, 0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u,
    0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du, 0x76DC4190u, 0x01DB7106u,
    0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
    0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u, 0x7F6A0DBBu, 0x086D3D2Du,
    0x91646C97u, 0xE6635C01u, 0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu,
    0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u, 0x65B0D9C6u, 0x12B7E950u,
    0x8BBEB8EAu, 0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
    0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u, 0x4ADFA541u, 0x3DD895D7u,
    0xA4D1C46Du, 0xD3D6F4FBu, 0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u,
    0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u, 0x5005713Cu, 0x270241AAu,
    0xBE0B1010u, 0xC90C2086u, 0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
    0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u, 0x2EB40D81u,
    0xB7BD5C3Bu, 0xC0BA6CADu, 0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au,
    0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u, 0xE3630B12u, 0x94643B84u,
    0x0D6D6A3Eu, 0x7A6A5AA8u, 0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
    0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu, 0xF762575Du, 0x806567CBu,
    0x196C3671u, 0x6E6B06E7u, 0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu,
    0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u, 0xD6D6A3E8u, 0xA1D1937Eu,
    0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
    0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u, 0xDF60EFC3u, 0xA867DF55u,
    0x316E8EEFu, 0x4669BE79u, 0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u,
    0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu, 0xC5BA3BBEu, 0xB2BD0B28u,
    0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
    0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au, 0x9C0906A9u, 0xEB0E363Fu,
    0x72076785u, 0x05005713u, 0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u,
    0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u, 0x86D3D2D4u, 0xF1D4E242u,
    0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
    0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu, 0x8F659EFFu, 0xF862AE69u,
    0x616BFFD3u, 0x166CCF45u, 0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u,
    0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu, 0xAED16A4Au, 0xD9D65ADCu,
    0x40DF0B66u, 0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
    0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u, 0xBAD03605u, 0xCDD70693u,
    0x54DE5729u, 0x23D967BFu, 0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u,
    0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du,
};

const uint32_t *sr_png_crc_table(void) { return s_png_crc_table; }

static uint32_t png_crc_update(uint32_t crc, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) crc = s_png_crc_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

typedef struct {
    FILE *f;
    uint32_t crc;         /* running CRC of the IDAT chunk (type and data), pre-inverted */
    uint32_t adler_a;
    uint32_t adler_b;
    uint32_t left_total;  /* image bytes (filter bytes included) still to store */
    uint32_t left_block;  /* bytes still to store in the open deflate block */
    int ok;
} PngOut;

static void png_be32(uint8_t *out, uint32_t v) {
    out[0] = (uint8_t)(v >> 24);
    out[1] = (uint8_t)(v >> 16);
    out[2] = (uint8_t)(v >> 8);
    out[3] = (uint8_t)v;
}

static void png_put(PngOut *z, const uint8_t *p, size_t n) {
    if (!z->ok) return;
    z->crc = png_crc_update(z->crc, p, n);
    if (fwrite(p, 1, n, z->f) != n) z->ok = 0;
}

/* One whole chunk with its length and CRC (IHDR, IEND). */
static int png_chunk(FILE *f, const char type[4], const uint8_t *data, uint32_t len) {
    uint8_t head[8];
    png_be32(head, len);
    memcpy(head + 4, type, 4u);
    uint32_t crc = png_crc_update(0xFFFFFFFFu, head + 4, 4u);
    if (len) crc = png_crc_update(crc, data, len);
    uint8_t tail[4];
    png_be32(tail, crc ^ 0xFFFFFFFFu);
    return fwrite(head, 1, 8u, f) == 8u && (len == 0u || fwrite(data, 1, len, f) == len) &&
           fwrite(tail, 1, 4u, f) == 4u;
}

/* Signature, IHDR, and the head of the one IDAT chunk, for `channels` 3 (RGB) or 4 (RGBA).
 * cap_pixels_valid bounds w*4*h by UINT32_MAX, so the stored image
 * (h * (1 + channels * w) bytes) fits in 64 bits, and the IDAT length is checked below. */
static int png_begin(PngOut *z, FILE *f, uint32_t w, uint32_t h, uint32_t channels) {
    static const uint8_t sig[8] = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    uint8_t ihdr[13];
    png_be32(ihdr, w);
    png_be32(ihdr + 4, h);
    ihdr[8] = 8u;    /* bit depth */
    ihdr[9] = channels == 4u ? 6u : 2u;   /* colour type: RGBA or RGB */
    ihdr[10] = 0u;   /* deflate */
    ihdr[11] = 0u;   /* filter method 0 */
    ihdr[12] = 0u;   /* no interlace */
    uint64_t raw = (uint64_t)h * (1u + (uint64_t)channels * (uint64_t)w);
    uint64_t blocks = (raw + 65534u) / 65535u;
    uint64_t idat = 2u + 5u * blocks + raw + 4u;
    if (raw == 0u || idat > 0x7FFFFFFFu) return 0;   /* PNG chunk lengths are at most 2^31 - 1 */
    if (fwrite(sig, 1, 8u, f) != 8u || !png_chunk(f, "IHDR", ihdr, 13u)) return 0;
    uint8_t len[4];
    png_be32(len, (uint32_t)idat);
    if (fwrite(len, 1, 4u, f) != 4u) return 0;
    z->f = f;
    z->crc = 0xFFFFFFFFu;
    z->adler_a = 1u;
    z->adler_b = 0u;
    z->left_total = (uint32_t)raw;
    z->left_block = 0u;
    z->ok = 1;
    static const uint8_t zhead[2] = {0x78u, 0x01u};   /* deflate, 32K window, no dictionary */
    png_put(z, (const uint8_t *)"IDAT", 4u);
    png_put(z, zhead, 2u);
    return z->ok;
}

/* Store n image bytes, opening a stored block (at most 65535 bytes) whenever one is full. */
static void png_z_data(PngOut *z, const uint8_t *p, size_t n) {
    while (z->ok && n) {
        if (n > z->left_total) { z->ok = 0; return; }
        if (z->left_block == 0u) {
            uint32_t blk = z->left_total < 65535u ? z->left_total : 65535u;
            uint8_t hdr[5];
            hdr[0] = blk == z->left_total ? 1u : 0u;   /* BFINAL on the last block, BTYPE 00 */
            hdr[1] = (uint8_t)blk;
            hdr[2] = (uint8_t)(blk >> 8);
            hdr[3] = (uint8_t)~blk;
            hdr[4] = (uint8_t)(~blk >> 8);
            png_put(z, hdr, 5u);
            z->left_block = blk;
        }
        size_t take = n < z->left_block ? n : z->left_block;
        for (size_t i = 0; i < take; i++) {
            z->adler_a = (z->adler_a + p[i]) % 65521u;
            z->adler_b = (z->adler_b + z->adler_a) % 65521u;
        }
        png_put(z, p, take);
        z->left_block -= (uint32_t)take;
        z->left_total -= (uint32_t)take;
        p += take;
        n -= take;
    }
}

/* The Adler-32, the IDAT CRC and IEND. Every image byte must have been stored. */
static int png_end(PngOut *z) {
    if (!z->ok || z->left_total != 0u) return 0;
    uint8_t adler[4];
    png_be32(adler, (z->adler_b << 16) | z->adler_a);
    png_put(z, adler, 4u);
    if (!z->ok) return 0;
    uint8_t crc[4];
    png_be32(crc, z->crc ^ 0xFFFFFFFFu);
    return fwrite(crc, 1, 4u, z->f) == 4u && png_chunk(z->f, "IEND", NULL, 0u);
}

/* Rename a completely written temp file over `path` (a capture replaces an older capture of
 * the same name): a reader never observes a half-written file. The temp file is removed when
 * the rename fails. */
static int cap_publish_file(const char *tmp, const char *path) {
    if (nk_rename_utf8(tmp, path) != 0) {
        nk_remove_utf8(tmp);
        fprintf(stderr, "fbcap: publish failed (the finished file could not be renamed into place)\n");
        return 0;
    }
    return 1;
}

/* ---- Deflate with fixed Huffman codes (the texture dump's PNGs) ----------------------------
 * Project-authored from RFC 1950 and RFC 1951. One final block of type 01 (the fixed codes of
 * RFC 1951 3.2.6): literals, and LZ77 matches of 3 to 258 bytes at most 32768 bytes back. A
 * match is looked for through a hash of the next three bytes and a chain of the earlier positions
 * with that hash; ZD_CHAIN candidates at most are compared at each position. That bounds the time
 * a position costs, not what is encoded: every byte is encoded, as a literal when no match is
 * found, so the stream always inflates to exactly the input. */
#define ZD_WINDOW 32768u
#define ZD_HASH_BITS 15u
#define ZD_CHAIN 48u
#define ZD_MIN_MATCH 3u
#define ZD_MAX_MATCH 258u

typedef struct {
    uint8_t *p;
    size_t n, cap;
    uint32_t bits;    /* bits not yet in a whole byte, first bit lowest */
    unsigned nbits;
    int ok;
} ZdOut;

static void zd_put(ZdOut *z, const uint8_t *src, size_t n) {
    if (!z->ok || n == 0u) return;
    if (z->cap - z->n < n) {
        size_t cap = z->cap ? z->cap : 4096u;
        while (cap - z->n < n) {
            if (cap > ((size_t)-1) / 2u) { z->ok = 0; return; }
            cap *= 2u;
        }
        uint8_t *q = (uint8_t *)realloc(z->p, cap);
        if (!q) { z->ok = 0; return; }
        z->p = q;
        z->cap = cap;
    }
    memcpy(z->p + z->n, src, n);
    z->n += n;
}

/* `n` (at most 16) bits of `v`, lowest first (RFC 1951 3.1.1: data elements). */
static void zd_bits(ZdOut *z, uint32_t v, unsigned n) {
    z->bits |= v << z->nbits;
    z->nbits += n;
    while (z->nbits >= 8u) {
        const uint8_t b = (uint8_t)z->bits;
        zd_put(z, &b, 1u);
        z->bits >>= 8;
        z->nbits -= 8u;
    }
}

/* A Huffman code of `len` bits, its most significant bit first (RFC 1951 3.1.1). */
static void zd_code(ZdOut *z, uint32_t code, unsigned len) {
    uint32_t r = 0;
    for (unsigned i = 0; i < len; i++) r |= ((code >> i) & 1u) << (len - 1u - i);
    zd_bits(z, r, len);
}

/* A literal/length symbol in the fixed code (RFC 1951 3.2.6). */
static void zd_litlen(ZdOut *z, unsigned sym) {
    if (sym < 144u) zd_code(z, 0x30u + sym, 8u);
    else if (sym < 256u) zd_code(z, 0x190u + (sym - 144u), 9u);
    else if (sym < 280u) zd_code(z, sym - 256u, 7u);
    else zd_code(z, 0xC0u + (sym - 280u), 8u);
}

static const uint16_t zd_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
    131, 163, 195, 227, 258
};
static const uint8_t zd_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t zd_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
    2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t zd_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static void zd_match(ZdOut *z, unsigned len, unsigned dist) {
    unsigned l = 28u;
    while (zd_len_base[l] > len) l--;
    zd_litlen(z, 257u + l);
    if (zd_len_extra[l]) zd_bits(z, len - zd_len_base[l], zd_len_extra[l]);
    unsigned d = 29u;
    while (zd_dist_base[d] > dist) d--;
    zd_code(z, d, 5u);   /* the fixed distance codes are the 5-bit numbers 0..29 */
    if (zd_dist_extra[d]) zd_bits(z, dist - zd_dist_base[d], zd_dist_extra[d]);
}

static uint32_t zd_hash(const uint8_t *p) {
    const uint32_t v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    return (v * 2654435761u) >> (32u - ZD_HASH_BITS);
}

/* Index position `i` of `in` (n bytes) under the hash of its next three bytes. */
static void zd_insert(int32_t *head, int32_t *prev, const uint8_t *in, size_t n, size_t i) {
    if (n - i < ZD_MIN_MATCH) return;
    const uint32_t h = zd_hash(in + i);
    prev[i % ZD_WINDOW] = head[h];
    head[h] = (int32_t)i;
}

/* The raw deflate stream of `in` (n bytes, n below 2^31), appended to z; ends on a whole byte. */
static int zd_deflate(ZdOut *z, const uint8_t *in, size_t n) {
    int32_t *head = (int32_t *)malloc(sizeof(int32_t) << ZD_HASH_BITS);
    int32_t *prev = (int32_t *)malloc(sizeof(int32_t) * ZD_WINDOW);
    if (!head || !prev) {
        free(head);
        free(prev);
        return 0;
    }
    for (size_t k = 0; k < ((size_t)1 << ZD_HASH_BITS); k++) head[k] = -1;
    zd_bits(z, 1u, 1u);   /* BFINAL: the only block */
    zd_bits(z, 1u, 2u);   /* BTYPE 01: fixed Huffman codes */
    size_t i = 0;
    while (i < n && z->ok) {
        size_t best = 0, best_dist = 0;
        if (n - i >= ZD_MIN_MATCH) {
            const size_t most = n - i < ZD_MAX_MATCH ? n - i : ZD_MAX_MATCH;
            int32_t cand = head[zd_hash(in + i)];
            for (unsigned tries = 0; cand >= 0 && tries < ZD_CHAIN; tries++) {
                const size_t c = (size_t)cand, dist = i - c;
                if (dist > ZD_WINDOW) break;   /* older positions are out of the window */
                if (in[c + best] == in[i + best]) {
                    size_t len = 0;
                    while (len < most && in[c + len] == in[i + len]) len++;
                    if (len > best) {
                        best = len;
                        best_dist = dist;
                        if (len == most) break;
                    }
                }
                cand = prev[c % ZD_WINDOW];
            }
        }
        if (best >= ZD_MIN_MATCH) {
            zd_match(z, (unsigned)best, (unsigned)best_dist);
            for (size_t k = 0; k < best; k++) zd_insert(head, prev, in, n, i + k);
            i += best;
        } else {
            zd_litlen(z, in[i]);
            zd_insert(head, prev, in, n, i);
            i++;
        }
    }
    zd_litlen(z, 256u);                            /* end of block */
    if (z->nbits) zd_bits(z, 0u, 8u - z->nbits);   /* to a whole byte */
    free(head);
    free(prev);
    return z->ok;
}

uint64_t sr_png_encode_bound(uint32_t w, uint32_t h) {
    if (w == 0u || h == 0u || (uint64_t)w * 4u * h > UINT32_MAX) return 0u;
    const uint64_t raw = (uint64_t)h * (1u + 4u * (uint64_t)w);
    if (raw > 0x7FFFFFFFu) return 0u;
    return 63u + (9u * raw + 17u) / 8u;
}

int sr_png_encode_rgba(const uint32_t *rgba, uint32_t w, uint32_t h, uint8_t **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0u;
    if (!rgba || !out || !out_len || w == 0u || h == 0u || (uint64_t)w * 4u * h > UINT32_MAX)
        return 0;
    const uint64_t raw64 = (uint64_t)h * (1u + 4u * (uint64_t)w);
    if (raw64 > 0x7FFFFFFFu) return 0;   /* PNG chunk lengths are at most 2^31 - 1 */
    const size_t raw = (size_t)raw64;
    uint8_t *img = (uint8_t *)malloc(raw);
    if (!img) return 0;
    uint8_t *q = img;
    for (uint32_t y = 0; y < h; y++) {
        *q++ = 0u;   /* filter type 0 */
        /* R in the low byte of each word, whatever the host's byte order. */
        const uint32_t *row = rgba + (size_t)y * w;
        for (uint32_t x = 0; x < w; x++) {
            const uint32_t c = row[x];
            *q++ = (uint8_t)c;
            *q++ = (uint8_t)(c >> 8);
            *q++ = (uint8_t)(c >> 16);
            *q++ = (uint8_t)(c >> 24);
        }
    }
    uint32_t a = 1u, b = 0u;
    for (size_t i = 0; i < raw; i++) {
        a = (a + img[i]) % 65521u;
        b = (b + a) % 65521u;
    }

    static const uint8_t sig[8] = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    uint8_t ihdr[8 + 13 + 4];
    png_be32(ihdr, 13u);
    memcpy(ihdr + 4, "IHDR", 4u);
    png_be32(ihdr + 8, w);
    png_be32(ihdr + 12, h);
    ihdr[16] = 8u;   /* bit depth */
    ihdr[17] = 6u;   /* colour type: RGBA */
    ihdr[18] = 0u;   /* deflate */
    ihdr[19] = 0u;   /* filter method 0 */
    ihdr[20] = 0u;   /* no interlace */
    png_be32(ihdr + 21, png_crc_update(0xFFFFFFFFu, ihdr + 4, 17u) ^ 0xFFFFFFFFu);
    static const uint8_t idat_head[8] = {0, 0, 0, 0, 'I', 'D', 'A', 'T'};
    static const uint8_t zhead[2] = {0x78u, 0x01u};   /* deflate, 32K window, no dictionary */

    ZdOut z;
    memset(&z, 0, sizeof(z));
    z.ok = 1;
    zd_put(&z, sig, sizeof(sig));
    zd_put(&z, ihdr, sizeof(ihdr));
    const size_t idat_at = z.n;
    zd_put(&z, idat_head, sizeof(idat_head));
    zd_put(&z, zhead, sizeof(zhead));
    int ok = z.ok && zd_deflate(&z, img, raw);
    free(img);
    uint8_t tail[4];
    png_be32(tail, (b << 16) | a);
    zd_put(&z, tail, 4u);
    ok = ok && z.ok;
    const size_t idat_len = ok ? z.n - idat_at - 8u : 0u;
    if (ok && idat_len > 0x7FFFFFFFu) ok = 0;
    if (ok) {
        png_be32(z.p + idat_at, (uint32_t)idat_len);
        png_be32(tail, png_crc_update(0xFFFFFFFFu, z.p + idat_at + 4u, idat_len + 4u) ^ 0xFFFFFFFFu);
        zd_put(&z, tail, 4u);
        static const uint8_t iend[12] = {0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAEu, 0x42u, 0x60u, 0x82u};
        zd_put(&z, iend, sizeof(iend));
        ok = z.ok;
    }
    if (!ok) {
        free(z.p);
        return 0;
    }
    *out = z.p;
    *out_len = z.n;
    return 1;
}

/* sr_file_publish_new and sr_file_publish_new_tagged: `tag` NULL takes the temp names from this
   file's counter (one thread), else from the caller's tag. */
static int publish_new(const char *path, const void *data, size_t len, const unsigned long *tag) {
    if (!path || !path[0] || (!data && len)) return SR_PUBLISH_FAILED;
    static unsigned long s_publish_seq;
    char tmp[1024 + 64];
    FILE *f = NULL;
    /* The temp name carries this process's id, and is created only where no file is ("x": no
       file, and no link, already at that name is opened, truncated or followed), so another run
       dumping into the same folder, or a name left behind, is never written through. A name that
       is taken costs one more try, up to PUBLISH_TEMP_TRIES; then the publish fails and says so. */
    enum { PUBLISH_TEMP_TRIES = 16 };
    for (int attempt = 0; attempt < PUBLISH_TEMP_TRIES && !f; attempt++) {
        const int n = tag ? snprintf(tmp, sizeof tmp, "%s.tmp%lu.t%lu.%d", path, nk_platform_process_id(), *tag, attempt)
                          : snprintf(tmp, sizeof tmp, "%s.tmp%lu.%lu", path, nk_platform_process_id(), ++s_publish_seq);
        if (n < 0 || n >= (int)sizeof tmp)
            return SR_PUBLISH_FAILED;
        errno = 0;
        f = nk_fopen_utf8(tmp, "wbx");
        if (!f && errno != EEXIST) return SR_PUBLISH_FAILED;   /* a failure, not a taken name */
    }
    if (!f) return SR_PUBLISH_FAILED;
    int ok = len == 0u || fwrite(data, 1, len, f) == len;
    if (ok && fflush(f) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        nk_remove_utf8(tmp);
        return SR_PUBLISH_FAILED;
    }
    const int moved = nk_rename_noreplace_utf8(tmp, path);
    if (moved == 0) return SR_PUBLISH_WRITTEN;
    nk_remove_utf8(tmp);
    return moved == 1 ? SR_PUBLISH_EXISTS : SR_PUBLISH_FAILED;
}

int sr_file_publish_new(const char *path, const void *data, size_t len) {
    return publish_new(path, data, len, NULL);
}

int sr_file_publish_new_tagged(const char *path, const void *data, size_t len, unsigned long tag) {
    return publish_new(path, data, len, &tag);
}

/* The length of the strict UTF-8 sequence at s (RFC 3629: no overlong form, no surrogate, nothing
   past U+10FFFF), or 0. */
static size_t texdump_utf8_at(const unsigned char *s) {
    const unsigned char a = s[0];
    if (a < 0x80u) return 1u;
    if (a >= 0xc2u && a <= 0xdfu) return (s[1] & 0xc0u) == 0x80u ? 2u : 0u;
    if (a >= 0xe0u && a <= 0xefu) {
        const unsigned char lo = a == 0xe0u ? 0xa0u : 0x80u, hi = a == 0xedu ? 0x9fu : 0xbfu;
        return s[1] >= lo && s[1] <= hi && (s[2] & 0xc0u) == 0x80u ? 3u : 0u;
    }
    if (a >= 0xf0u && a <= 0xf4u) {
        const unsigned char lo = a == 0xf0u ? 0x90u : 0x80u, hi = a == 0xf4u ? 0x8fu : 0xbfu;
        return s[1] >= lo && s[1] <= hi && (s[2] & 0xc0u) == 0x80u && (s[3] & 0xc0u) == 0x80u ? 4u : 0u;
    }
    return 0u;
}

const char *sr_texdump_folder_problem(const char *dir) {
    if (!dir || !dir[0]) return "no dump folder could be named";
    for (const unsigned char *p = (const unsigned char *)dir; *p;) {
        const size_t n = texdump_utf8_at(p);
        if (!n) return "the dump folder's path is not valid UTF-8";
        p += n;
    }
    if (!nk_platform_path_is_absolute(dir)) return "the dump folder's path is not a full path";
    if (strlen(dir) >= SR_TEXDUMP_DIR_MAX) return "the dump folder's path is 512 characters or longer";
    if ((dir[0] == '\\' || dir[0] == '/') && (dir[1] == '\\' || dir[1] == '/') && (dir[2] == '.' || dir[2] == '?') &&
        (dir[3] == '\\' || dir[3] == '/'))
        return "the dump folder is in a device namespace";
    return NULL;
}

/* Publish w x h pixels as a P6 PPM whose name ends in .ppm, or as the PNG above for a name
 * ending in .png: the header matches the extension and exactly w*h*3 colour bytes follow, in
 * R,G,B order whatever the source byte order. Publication is atomic: a unique temp sibling is
 * written completely and then renamed over the destination, so a reader never observes a
 * half-written file. */
static int cap_write_file(const uint8_t *px, uint32_t row_bytes, sr_cap_order order) {
    if (!cap_ensure_parent_dir(s_path)) return 0;
    static unsigned long s_tmp_seq;
    char tmp[1024 + 64];
    if (snprintf(tmp, sizeof tmp, "%s.tmp%lu", s_path, ++s_tmp_seq) >= (int)sizeof tmp)
        return 0;
    unsigned char *rgb = (unsigned char *)malloc((size_t)s_w * 3u);
    if (!rgb) return 0;
    FILE *f = nk_fopen_utf8(tmp, "wb");
    if (!f) { free(rgb); return 0; }
    const int png = cap_path_is_png(s_path);
    PngOut z;
    int ok = png ? png_begin(&z, f, s_w, s_h, 3u) : fprintf(f, "P6\n%u %u\n255\n", s_w, s_h) > 0;
    /* Byte offsets of R, G, B inside one pixel for the source byte order. */
    const int ir = order == SR_CAP_ORDER_RGBX ? 0 : 2;
    const int ib = order == SR_CAP_ORDER_RGBX ? 2 : 0;
    for (uint32_t y = 0; ok && y < s_h; y++) {
        const uint8_t *row = px + (size_t)y * row_bytes;
        for (uint32_t x = 0; x < s_w; x++) {
            const uint8_t *p = row + (size_t)x * 4u;
            rgb[x * 3u + 0u] = p[ir];
            rgb[x * 3u + 1u] = p[1];
            rgb[x * 3u + 2u] = p[ib];
        }
        if (png) {
            static const uint8_t filter_none = 0u;   /* every PNG row starts with its filter type */
            png_z_data(&z, &filter_none, 1u);
            png_z_data(&z, rgb, (size_t)s_w * 3u);
            ok = z.ok;
        } else {
            ok = fwrite(rgb, 1, (size_t)s_w * 3u, f) == (size_t)s_w * 3u;
        }
    }
    if (ok && png) ok = png_end(&z);
    if (ok && fflush(f) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    free(rgb);
    if (!ok) { nk_remove_utf8(tmp); return 0; }
    return cap_publish_file(tmp, s_path);
}

/* A w x h frame of row_bytes-long rows is a valid pixel description: the frame size is
 * checked in 64-bit arithmetic and bounded like the readback buffer (at most UINT32_MAX
 * source bytes), so no size computation that follows can wrap. */
static int cap_pixels_valid(const void *px, uint32_t w, uint32_t h, uint32_t row_bytes,
                            sr_cap_order order) {
    return px && w != 0u && h != 0u && (uint64_t)row_bytes >= (uint64_t)w * 4u &&
           (uint64_t)row_bytes * h <= UINT32_MAX &&
           (order == SR_CAP_ORDER_BGRX || order == SR_CAP_ORDER_RGBX);
}

/* The probe's result: the brightest of the three colour bytes of any pixel. The ignored byte
 * sits at offset 3 in both orders. Rows are scanned in order and the scan stops after the
 * first row that holds a 255, the largest possible result. */
static uint32_t probe_brightest(const uint8_t *px, uint32_t w, uint32_t h, uint32_t row_bytes) {
    uint32_t brightest = 0;
    for (uint32_t y = 0; y < h && brightest < 255u; y++) {
        const uint8_t *row = px + (size_t)y * row_bytes;
        uint32_t m = 0;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = row + (size_t)x * 4u;
            if (p[0] > m) m = p[0];
            if (p[1] > m) m = p[1];
            if (p[2] > m) m = p[2];
        }
        if (m > brightest) brightest = m;
    }
    return brightest;
}

/* Validate the pixel description, publish the capture, then resolve it. */
static int cap_publish(const void *px, uint32_t row_bytes, sr_cap_order order) {
    if (!cap_pixels_valid(px, s_w, s_h, row_bytes, order)) {
        cap_finish(0, "invalid pixel description");
        return -1;
    }
    if (s_probe) {
        s_probe_brightest = probe_brightest((const uint8_t *)px, s_w, s_h, row_bytes);
        s_probe_ready = 1;
        cap_finish(1, NULL);
        return 1;
    }
    if (!cap_write_file((const uint8_t *)px, row_bytes, order)) {
        cap_finish(0, "write or publish failed");
        return -1;
    }
    cap_finish(1, NULL);
    return 1;
}

int sr_capture_arm(const char *path) {
    /* A rejected request is a new capture attempt, not permission to reuse the last
     * completed result. In-flight state is preserved; only the reportable outcome clears. */
    s_result = 0;
    if (s_state == CAP_ARMED || s_state == CAP_RECORDED) return 0;
    if (!path || !path[0] || strlen(path) >= sizeof s_path) return 0;
    snprintf(s_path, sizeof s_path, "%s", path);
    s_probe = 0;
    s_src = SR_CAP_SRC_NONE;
    s_w = s_h = 0;
    s_state = CAP_ARMED;
    return 1;
}

int sr_capture_arm_probe(void) {
    s_result = 0;
    if (s_state == CAP_ARMED || s_state == CAP_RECORDED || s_probe_in_flight) return 0;
    s_path[0] = '\0';
    s_probe = 1;
    s_src = SR_CAP_SRC_NONE;
    s_w = s_h = 0;
    s_state = CAP_ARMED;
    return 1;
}

int sr_capture_probe_take(uint32_t *brightest) {
    if (!s_probe_ready) return 0;
    s_probe_ready = 0;
    if (brightest) *brightest = s_probe_brightest;
    return 1;
}

int sr_capture_pending_is_probe(void) {
    return (s_state == CAP_ARMED || s_state == CAP_RECORDED) && s_probe;
}

void sr_capture_probe_defer(void) {
    if (s_state != CAP_RECORDED || !s_probe) return;
    s_probe_in_flight = 1;
    s_probe_w = s_w;
    s_probe_h = s_h;
    /* The capture slot is free again; the probe has no reportable file result. */
    s_probe = 0;
    s_state = CAP_IDLE;
    s_result = 0;
}

int sr_capture_probe_in_flight(void) { return s_probe_in_flight; }

int sr_capture_publish_probe(const void *px, uint32_t row_bytes, sr_cap_order order) {
    if (!s_probe_in_flight) return 0;
    s_probe_in_flight = 0;
    if (!cap_pixels_valid(px, s_probe_w, s_probe_h, row_bytes, order)) {
        fprintf(stderr, "fbcap: picture probe failed: invalid pixel description\n");
        return -1;
    }
    s_probe_brightest = probe_brightest((const uint8_t *)px, s_probe_w, s_probe_h, row_bytes);
    s_probe_ready = 1;
    return 1;
}

void sr_capture_probe_drop(const char *why) {
    if (!s_probe_in_flight) return;
    s_probe_in_flight = 0;
    fprintf(stderr, "fbcap: picture probe dropped: %s\n", why ? why : "unknown reason");
}

void sr_capture_set_probe_resolver(SrCaptureProbeResolver resolver) { s_probe_resolver = resolver; }

int sr_capture_probe_poll(int block) {
    if (!s_probe_in_flight || !s_probe_resolver) return 0;
    return s_probe_resolver(block);
}

int sr_picture_watch_probe(SrPictureWatch *w, uint32_t vcount, uint32_t brightest) {
    if (!w) return SR_PICTURE_WATCH_NONE;
    w->pending = 0;
    w->last_vcount = vcount;
    if (brightest <= SR_PICTURE_DARK_MAX) {
        if (!w->dark) {
            w->dark = 1;
            w->first_vcount = vcount;
            w->reported = 0;
        }
        return SR_PICTURE_WATCH_NONE;
    }
    int resumed = w->dark && w->reported > 0u;
    w->dark = 0;
    w->reported = 0;
    return resumed ? SR_PICTURE_WATCH_RESUMED : SR_PICTURE_WATCH_NONE;
}

int sr_picture_watch_tick(SrPictureWatch *w, uint32_t vcount, uint32_t *dark_vblanks) {
    if (!w || !w->dark) return SR_PICTURE_WATCH_NONE;
    /* VCOUNT is monotonic for the process, so both differences are true elapsed counts. */
    if (vcount - w->last_vcount > 2u * SR_PICTURE_PROBE_EVERY) return SR_PICTURE_WATCH_NONE;
    uint32_t elapsed = vcount - w->first_vcount;
    uint32_t bucket = elapsed / SR_PICTURE_REPORT_EVERY;
    if (bucket <= w->reported) return SR_PICTURE_WATCH_NONE;
    w->reported = bucket;
    if (dark_vblanks) *dark_vblanks = elapsed;
    return SR_PICTURE_WATCH_REPORT;
}

void sr_picture_watch_probe_sent(SrPictureWatch *w, uint32_t vcount) {
    if (!w) return;
    w->pending = 1;
    w->pending_vcount = vcount;
}

int sr_picture_watch_tick_needs_probe(const SrPictureWatch *w, uint32_t vcount) {
    if (!w || !w->pending) return 0;
    /* A dark result opens a stretch at the probe, due a report once a period has passed. */
    if (vcount - w->pending_vcount >= SR_PICTURE_REPORT_EVERY) return 1;
    /* Fed, the result either keeps the stretch (last_vcount becomes the probe's, so the tick
     * is not quiet) or ends it; it matters only when a new report bucket is due. */
    return w->dark && (vcount - w->first_vcount) / SR_PICTURE_REPORT_EVERY > w->reported;
}

int sr_picture_watch_collect(SrPictureWatch *w, int block, uint32_t *brightest) {
    if (!w || !w->pending) return SR_PICTURE_WATCH_NONE;
    uint32_t b = 0u;
    (void)sr_capture_probe_poll(block);
    if (sr_capture_probe_take(&b)) {
        if (brightest) *brightest = b;
        return sr_picture_watch_probe(w, w->pending_vcount, b);
    }
    if (block) sr_capture_probe_drop("no presenter finished the readback");
    if (!sr_capture_probe_in_flight()) w->pending = 0;
    return SR_PICTURE_WATCH_NONE;
}

int sr_capture_result(void) { return s_result; }

void sr_capture_cancel(void) {
    if (s_state != CAP_ARMED) return;
    s_state = CAP_IDLE;
    s_path[0] = '\0';
    s_src = SR_CAP_SRC_NONE;
    s_w = s_h = 0;
    s_result = 0;
}

void sr_capture_fail(const char *why) {
    if (s_state != CAP_ARMED && s_state != CAP_RECORDED) return;
    cap_finish(0, why);
}

const char *sr_capture_source_label(void) {
    switch (s_src) {
    case SR_CAP_SRC_CPU: return "cpu-framebuffer";
    case SR_CAP_SRC_GPU: return "gpu-render-target";
    default:             return "";
    }
}

int sr_capture_is_armed(void) { return s_state == CAP_ARMED; }

int sr_capture_is_recorded(void) { return s_state == CAP_RECORDED; }

int sr_capture_serve_host(const void *px, uint32_t w, uint32_t h, uint32_t row_bytes,
                          sr_cap_order order) {
    if (s_state != CAP_ARMED) return 0;
    s_src = SR_CAP_SRC_CPU;
    s_w = w;
    s_h = h;
    return cap_publish(px, row_bytes, order);
}

void sr_capture_mark_recorded(sr_cap_source src, uint32_t w, uint32_t h) {
    if (s_state != CAP_ARMED) return;
    s_src = src;
    s_w = w;
    s_h = h;
    s_state = CAP_RECORDED;
}

int sr_capture_publish_recorded(const void *px, uint32_t row_bytes, sr_cap_order order) {
    if (s_state != CAP_RECORDED) return 0;
    return cap_publish(px, row_bytes, order);
}
