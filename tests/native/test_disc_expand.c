/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/*
 * Compressed disc images expanded once at import (src/core/nk_disc_expand.c,
 * src/core/nk_disc_chd.c).
 *
 * Every fixture is generated here from a synthetic ISO9660 image built in
 * memory (a PSP_GAME folder, a PARAM.SFO naming disc TEST00042, and a data
 * file of zero, pseudo-random, text and counting sectors). No disc image of
 * any game is involved. Writers in this file produce:
 *
 *   - CSO v1: DEFLATE blocks (fixed-Huffman with back-references, and stored
 *     DEFLATE blocks) plus stored CSO blocks, with index alignment 0 and 2;
 *   - CSO v2: LZ4-flagged blocks, DEFLATE blocks and blocks stored by size;
 *   - ZSO: LZ4 blocks with literals and overlapping matches, plus stored blocks;
 *   - CHD v5 DVD images: an uncompressed map (with an all-zero hunk) and a
 *     compressed map (a Huffman-coded type table) whose hunks use the CHD
 *     "zlib" codec (fixed-Huffman DEFLATE) or are stored uncompressed.
 *
 * CHD coverage: libchdr's v5 header and metadata reader, the uncompressed and
 * compressed v5 map decoders, the hunk CRC check, and the "zlib" codec path
 * (miniz's inflater over fixed-Huffman DEFLATE with back-references). The
 * lzma, huff, flac and zstd codecs, the CD codecs and self/parent hunk
 * references are covered only by libchdr upstream: there is no chdman here to
 * make them. The DVD/CD/hard-disk decision is tested through the metadata tag
 * each image carries. Two of libchdr's local deltas are checked directly: a
 * reused Huffman decoder's lookup gaps, and the FLAC codec's empty hunk.
 */

#if !defined(_WIN32) && !defined(_WIN64)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#ifdef NDEBUG
#undef NDEBUG
#endif

#include "native_test_isolation.h"
#include "nk_disc_chd.h"
#include "nk_disc_expand.h"
#include "nk_disc_verify.h"
#include "nk_iso.h"
#include "nk_platform.h"
#include "nk_psp_sha1.h"

#include "libchdr/bitstream.h"
#include "libchdr/huffman.h"
#include "../../third_party/libchdr/src/codec_flac.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32) || defined(_WIN64)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define SECTOR 2048u
#define ISO_SECTORS 64u
#define ISO_BYTES (SECTOR * ISO_SECTORS)
#define DATA_FIRST 21u

static uint8_t g_iso[ISO_BYTES];
static uint8_t g_other_iso[ISO_BYTES]; /* an ISO9660 image with no PSP_GAME folder */
static char g_dir[NATIVE_TEST_PATH_MAX];
static int g_checks = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

/* ---- small helpers ----------------------------------------------------- */

static void path_in(char *out, size_t size, const char *name) {
    int n = snprintf(out, size, "%s%c%s", g_dir, nk_platform_path_separator(), name);
    assert(n > 0 && (size_t)n < size);
}

static void write_bytes(const char *path, const uint8_t *data, size_t size) {
    FILE *f = nk_fopen_utf8(path, "wb");
    assert(f != NULL);
    assert(fwrite(data, 1, size, f) == size);
    assert(fclose(f) == 0);
}

/* Gives a file one fixed write time (2020-01-01 00:00:00 UTC), through its own
   handle. Two files stamped so share a write time on every host, as two files
   written inside one tick of the host's clock do. */
static void stamp_write_time(const char *path) {
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wide[NATIVE_TEST_PATH_MAX];
    FILETIME stamp;
    HANDLE file;
    stamp.dwLowDateTime = 0x69050000u;
    stamp.dwHighDateTime = 0x01D5C036u;
    assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide,
                               NATIVE_TEST_PATH_MAX) > 0);
    file = CreateFileW(wide, FILE_WRITE_ATTRIBUTES,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(file != INVALID_HANDLE_VALUE);
    assert(SetFileTime(file, NULL, NULL, &stamp));
    assert(CloseHandle(file));
#else
    struct timespec times[2];
    int fd = open(path, O_WRONLY);
    assert(fd >= 0);
    times[0].tv_sec = 0;
    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_sec = 1577836800;
    times[1].tv_nsec = 0;
    assert(futimens(fd, times) == 0);
    assert(close(fd) == 0);
#endif
}

static uint8_t *read_all(const char *path, size_t *size) {
    FILE *f = nk_fopen_utf8(path, "rb");
    uint8_t *data;
    long end;
    if (!f) return NULL;
    assert(fseek(f, 0, SEEK_END) == 0);
    end = ftell(f);
    assert(end >= 0);
    assert(fseek(f, 0, SEEK_SET) == 0);
    data = (uint8_t *)malloc((size_t)end + 1u);
    assert(data != NULL);
    assert(fread(data, 1, (size_t)end, f) == (size_t)end);
    fclose(f);
    *size = (size_t)end;
    return data;
}

static bool file_equals(const char *path, const uint8_t *expected, size_t size) {
    size_t got = 0;
    uint8_t *data = read_all(path, &got);
    bool same = data && got == size && memcmp(data, expected, size) == 0;
    free(data);
    return same;
}

static void put_le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_le32(uint8_t *p, uint32_t v) { put_le16(p, v); put_le16(p + 2, v >> 16); }
static void put_le64(uint8_t *p, uint64_t v) { put_le32(p, (uint32_t)v); put_le32(p + 4, (uint32_t)(v >> 32)); }
static void put_be16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_be32(uint8_t *p, uint32_t v) { put_be16(p, v >> 16); put_be16(p + 2, v); }
static void put_be64(uint8_t *p, uint64_t v) { put_be32(p, (uint32_t)(v >> 32)); put_be32(p + 4, (uint32_t)v); }

/* A growable output buffer. */
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} Buf;

static void buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra <= b->cap) return;
    while (b->len + extra > b->cap) b->cap = b->cap ? b->cap * 2u : 4096u;
    b->data = (uint8_t *)realloc(b->data, b->cap);
    assert(b->data != NULL);
}

static void buf_put(Buf *b, const void *src, size_t n) {
    buf_reserve(b, n);
    memcpy(b->data + b->len, src, n);
    b->len += n;
}

static void buf_zero(Buf *b, size_t n) {
    buf_reserve(b, n);
    memset(b->data + b->len, 0, n);
    b->len += n;
}

/* ---- the synthetic ISO -------------------------------------------------- */

static void iso_both32(uint8_t *p, uint32_t v) {
    put_le32(p, v);
    put_be32(p + 4, v);
}

static size_t iso_record(uint8_t *dir, size_t offset, const char *name, size_t name_len,
                         uint32_t lba, uint32_t size, bool is_dir) {
    size_t len = 33u + name_len + ((33u + name_len) & 1u);
    memset(dir + offset, 0, len);
    dir[offset] = (uint8_t)len;
    iso_both32(dir + offset + 2, lba);
    iso_both32(dir + offset + 10, size);
    dir[offset + 25] = is_dir ? 2u : 0u;
    dir[offset + 28] = 1;
    dir[offset + 31] = 1;
    dir[offset + 32] = (uint8_t)name_len;
    memcpy(dir + offset + 33, name, name_len);
    return len;
}

static size_t build_sfo(uint8_t *out, const char *disc_id) {
    const char *keys[4] = { "CATEGORY", "DISC_ID", "DISC_VERSION", "TITLE" };
    const char *values[4] = { "UG", disc_id, "1.00", "Synthetic Disc" };
    size_t key_start = 20u + 4u * 16u;
    size_t key_len = 0;
    size_t data_start;
    size_t data_len = 0;
    for (int i = 0; i < 4; i++) key_len += strlen(keys[i]) + 1u;
    data_start = key_start + ((key_len + 3u) & ~(size_t)3u);
    memcpy(out, "\0PSF", 4);
    put_le32(out + 4, 0x0101u);
    put_le32(out + 8, (uint32_t)key_start);
    put_le32(out + 12, (uint32_t)data_start);
    put_le32(out + 16, 4u);
    key_len = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t *entry = out + 20 + i * 16;
        size_t vlen = strlen(values[i]) + 1u;
        size_t vmax = (vlen + 3u) & ~(size_t)3u;
        put_le16(entry, (uint32_t)key_len);
        put_le16(entry + 2, 0x0204u);
        put_le32(entry + 4, (uint32_t)vlen);
        put_le32(entry + 8, (uint32_t)vmax);
        put_le32(entry + 12, (uint32_t)data_len);
        memcpy(out + key_start + key_len, keys[i], strlen(keys[i]) + 1u);
        memcpy(out + data_start + data_len, values[i], vlen);
        key_len += strlen(keys[i]) + 1u;
        data_len += vmax;
    }
    return data_start + data_len;
}

static void build_iso(uint8_t *image, bool psp) {
    uint8_t *pvd = image + 16u * SECTOR;
    uint8_t *root = image + 18u * SECTOR;
    uint32_t state = 0x2545F491u;
    size_t off;
    memset(image, 0, ISO_BYTES);
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    memcpy(pvd + 40, "SYNTHETIC_DISC                  ", 32);
    iso_both32(pvd + 80, ISO_SECTORS);
    iso_record(pvd, 156, "\0", 1, 18, SECTOR, true);
    image[17u * SECTOR] = 255;
    memcpy(image + 17u * SECTOR + 1, "CD001", 5);
    image[17u * SECTOR + 6] = 1;
    off = iso_record(root, 0, "\0", 1, 18, SECTOR, true);
    off += iso_record(root, off, "\1", 1, 18, SECTOR, true);
    if (psp) {
        uint8_t *game = image + 19u * SECTOR;
        size_t sfo_len = build_sfo(image + 20u * SECTOR, "TEST00042");
        size_t g = iso_record(game, 0, "\0", 1, 19, SECTOR, true);
        off += iso_record(root, off, "DATA.BIN;1", 10, DATA_FIRST,
                          (ISO_SECTORS - DATA_FIRST) * SECTOR, false);
        off += iso_record(root, off, "PSP_GAME", 8, 19, SECTOR, true);
        g += iso_record(game, g, "\1", 1, 18, SECTOR, true);
        iso_record(game, g, "PARAM.SFO;1", 11, 20, (uint32_t)sfo_len, false);
    } else {
        off += iso_record(root, off, "DATA.BIN;1", 10, DATA_FIRST,
                          (ISO_SECTORS - DATA_FIRST) * SECTOR, false);
    }
    (void)off;
    for (uint32_t s = DATA_FIRST; s < ISO_SECTORS; s++) {
        uint8_t *sector = image + (size_t)s * SECTOR;
        switch (s % 4u) {
        case 0: /* zeros */
            break;
        case 1: /* pseudo-random: does not compress */
            for (uint32_t i = 0; i < SECTOR; i++) {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                sector[i] = (uint8_t)state;
            }
            break;
        case 2: { /* repeating text */
            static const char text[] = "synthetic disc data, sector pattern two. ";
            for (uint32_t i = 0; i < SECTOR; i++) sector[i] = (uint8_t)text[i % (sizeof(text) - 1u)];
            break;
        }
        default: /* counting */
            for (uint32_t i = 0; i < SECTOR; i++) sector[i] = (uint8_t)(i * 7u + s);
            break;
        }
    }
}

/* ---- DEFLATE writers (stored and fixed-Huffman blocks) ------------------- */

typedef struct {
    Buf *out;
    uint32_t acc;
    int bits;
} BitWriter;

static void bw_put(BitWriter *w, uint32_t value, int count) {
    for (int i = 0; i < count; i++) {
        w->acc |= ((value >> i) & 1u) << w->bits;
        if (++w->bits == 8) {
            uint8_t byte = (uint8_t)w->acc;
            buf_put(w->out, &byte, 1);
            w->acc = 0;
            w->bits = 0;
        }
    }
}

/* Huffman codes go most significant bit first. */
static void bw_code(BitWriter *w, uint32_t code, int length) {
    for (int i = length - 1; i >= 0; i--) bw_put(w, (code >> i) & 1u, 1);
}

static void bw_flush(BitWriter *w) {
    if (w->bits) bw_put(w, 0, 8 - w->bits);
}

static void fixed_symbol(BitWriter *w, uint32_t sym) {
    if (sym < 144u) bw_code(w, 0x30u + sym, 8);
    else if (sym < 256u) bw_code(w, 0x190u + (sym - 144u), 9);
    else if (sym < 280u) bw_code(w, sym - 256u, 7);
    else bw_code(w, 0xC0u + (sym - 280u), 8);
}

static void fixed_length(BitWriter *w, uint32_t length) {
    static const uint16_t base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                                       31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227,
                                       258 };
    static const uint8_t extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    int code = 28;
    while (base[code] > length) code--;
    fixed_symbol(w, 257u + (uint32_t)code);
    bw_put(w, length - base[code], extra[code]);
    bw_code(w, 0, 5); /* distance code 0: distance 1 */
}

/* Raw DEFLATE, one fixed-Huffman block: runs of a repeated byte become
   distance-1 matches, everything else literals. */
static void deflate_fixed(Buf *out, const uint8_t *src, size_t n) {
    BitWriter w = { out, 0, 0 };
    size_t i = 0;
    bw_put(&w, 1, 1);
    bw_put(&w, 1, 2);
    while (i < n) {
        size_t run = 0;
        fixed_symbol(&w, src[i]);
        i++;
        while (i + run < n && src[i + run] == src[i - 1] && run < 258u) run++;
        if (run >= 3u) {
            fixed_length(&w, (uint32_t)run);
            i += run;
        }
    }
    fixed_symbol(&w, 256u);
    bw_flush(&w);
}

static void deflate_stored(Buf *out, const uint8_t *src, size_t n) {
    uint8_t head[5];
    assert(n <= 65535u);
    head[0] = 1;
    put_le16(head + 1, (uint32_t)n);
    put_le16(head + 3, (uint32_t)(~n & 0xFFFFu));
    buf_put(out, head, sizeof(head));
    buf_put(out, src, n);
}

/* ---- LZ4 block writer ------------------------------------------------------ */

static void lz4_length(Buf *out, size_t extra) {
    while (extra >= 255u) {
        uint8_t b = 255;
        buf_put(out, &b, 1);
        extra -= 255u;
    }
    {
        uint8_t b = (uint8_t)extra;
        buf_put(out, &b, 1);
    }
}

static void lz4_sequence(Buf *out, const uint8_t *lit, size_t lit_len, size_t offset,
                         size_t match_len) {
    uint8_t token = (uint8_t)((lit_len >= 15u ? 15u : lit_len) << 4);
    if (match_len) token |= (uint8_t)(match_len - 4u >= 15u ? 15u : match_len - 4u);
    buf_put(out, &token, 1);
    if (lit_len >= 15u) lz4_length(out, lit_len - 15u);
    buf_put(out, lit, lit_len);
    if (match_len) {
        uint8_t off[2];
        put_le16(off, (uint32_t)offset);
        buf_put(out, off, 2);
        if (match_len - 4u >= 15u) lz4_length(out, match_len - 19u);
    }
}

/* A greedy LZ4 block: offsets 1..64, the end-of-block rules kept (the last
   match starts 12 or more bytes before the end, the last 5 bytes are
   literals). */
static void lz4_compress(Buf *out, const uint8_t *src, size_t n) {
    size_t ip = 0;
    size_t anchor = 0;
    while (ip + 12u <= n) {
        size_t best_len = 0;
        size_t best_off = 0;
        size_t limit = n - 5u - ip;
        for (size_t off = 1; off <= 64u && off <= ip; off++) {
            size_t len = 0;
            while (len < limit && src[ip + len] == src[ip + len - off]) len++;
            if (len > best_len) {
                best_len = len;
                best_off = off;
            }
        }
        if (best_len >= 4u) {
            lz4_sequence(out, src + anchor, ip - anchor, best_off, best_len);
            ip += best_len;
            anchor = ip;
        } else {
            ip++;
        }
    }
    lz4_sequence(out, src + anchor, n - anchor, 0, 0);
}

/* ---- CSO / ZSO writer ----------------------------------------------------- */

typedef enum { BLOCK_STORED, BLOCK_DEFLATE_FIXED, BLOCK_DEFLATE_STORED, BLOCK_LZ4 } BlockKind;

static BlockKind cso_v1_kind(uint32_t block) {
    switch (block % 3u) {
    case 0: return BLOCK_DEFLATE_FIXED;
    case 1: return BLOCK_DEFLATE_STORED;
    default: return BLOCK_STORED;
    }
}

static BlockKind cso_v2_kind(uint32_t block) {
    switch (block % 3u) {
    case 0: return BLOCK_LZ4;
    case 1: return BLOCK_DEFLATE_FIXED;
    default: return BLOCK_STORED;
    }
}

static BlockKind zso_kind(uint32_t block) {
    return (block % 5u) == 4u ? BLOCK_STORED : BLOCK_LZ4;
}

/* Build a CISO/ZISO image of `iso`. version 1 or 2 for "CISO", 1 for "ZISO". */
static void build_ciso(Buf *out, const char *magic, uint8_t version, uint8_t align,
                       const uint8_t *iso, size_t iso_len, BlockKind (*kind)(uint32_t)) {
    uint32_t blocks = (uint32_t)(iso_len / SECTOR);
    size_t index_off = 24u;
    size_t pos;
    uint8_t header[24];
    memset(header, 0, sizeof(header));
    memcpy(header, magic, 4);
    put_le32(header + 4, 24u);
    put_le64(header + 8, iso_len);
    put_le32(header + 16, SECTOR);
    header[20] = version;
    header[21] = align;
    out->len = 0;
    buf_put(out, header, sizeof(header));
    buf_zero(out, ((size_t)blocks + 1u) * 4u);
    pos = out->len;
    for (uint32_t i = 0; i <= blocks; i++) {
        uint32_t entry;
        BlockKind k = i < blocks ? kind(i) : BLOCK_STORED;
        size_t pad = ((size_t)1u << align) - 1u;
        /* Pad each block start to the index alignment. */
        while (out->len & pad) buf_zero(out, 1);
        pos = out->len;
        entry = (uint32_t)(pos >> align);
        if (i < blocks) {
            const uint8_t *src = iso + (size_t)i * SECTOR;
            bool plain_flag = false;
            bool v2 = magic[0] == 'C' && version == 2u;
            size_t start = out->len;
            if (k == BLOCK_DEFLATE_FIXED) {
                deflate_fixed(out, src, SECTOR);
            } else if (k == BLOCK_DEFLATE_STORED) {
                deflate_stored(out, src, SECTOR);
            } else if (k == BLOCK_LZ4) {
                lz4_compress(out, src, SECTOR);
                plain_flag = v2; /* v2: the flag marks LZ4 */
            }
            /* v2 marks a stored block by its size, so a block that did not
               shrink is written stored, as a v2 writer does. */
            if (k == BLOCK_STORED || (v2 && out->len - start >= SECTOR)) {
                out->len = start;
                buf_put(out, src, SECTOR);
                plain_flag = !v2;
            }
            if (plain_flag) entry |= 0x80000000u;
        }
        put_le32(out->data + index_off + (size_t)i * 4u, entry);
    }
}

/* ---- CHD v5 writer ---------------------------------------------------------- */

static uint16_t crc16_ccitt(const uint8_t *data, size_t n) {
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)(data[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

typedef struct {
    Buf *out;
    uint32_t acc;
    int bits;
} MsbWriter;

static void msb_put(MsbWriter *w, uint32_t value, int count) {
    for (int i = count - 1; i >= 0; i--) {
        w->acc = (w->acc << 1) | ((value >> i) & 1u);
        if (++w->bits == 8) {
            uint8_t byte = (uint8_t)w->acc;
            buf_put(w->out, &byte, 1);
            w->acc = 0;
            w->bits = 0;
        }
    }
}

static void msb_flush(MsbWriter *w) {
    while (w->bits) msb_put(w, 0, 1);
}

#define CHD_HUNK 4096u
#define CHD_TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

typedef enum { CHD_MAP_UNCOMPRESSED, CHD_MAP_COMPRESSED } ChdMap;

/* A CHD v5 image of `iso` with one metadata entry (`meta_tag`). */
static void build_chd(Buf *out, const uint8_t *iso, size_t iso_len, ChdMap map, uint32_t meta_tag) {
    uint32_t hunks = (uint32_t)((iso_len + CHD_HUNK - 1u) / CHD_HUNK);
    uint8_t header[124];
    size_t meta_off = 124u;
    size_t map_off;
    memset(header, 0, sizeof(header));
    memcpy(header, "MComprHD", 8);
    put_be32(header + 8, 124u);
    put_be32(header + 12, 5u);
    if (map == CHD_MAP_COMPRESSED) put_be32(header + 16, CHD_TAG('z', 'l', 'i', 'b'));
    put_be64(header + 32, iso_len);
    put_be64(header + 48, meta_off);
    put_be32(header + 56, CHD_HUNK);
    put_be32(header + 60, SECTOR);
    out->len = 0;
    buf_put(out, header, sizeof(header));
    {
        static const char text[] = "synthetic";
        uint8_t meta[16];
        put_be32(meta, meta_tag);
        put_be32(meta + 4, (uint32_t)sizeof(text));
        put_be64(meta + 8, 0);
        buf_put(out, meta, sizeof(meta));
        buf_put(out, text, sizeof(text));
    }
    map_off = out->len;
    put_be64(out->data + 40, map_off);

    if (map == CHD_MAP_UNCOMPRESSED) {
        /* Entries are hunk-aligned offsets / hunk size; 0 is an all-zero hunk. */
        size_t data_off;
        buf_zero(out, (size_t)hunks * 4u);
        while (out->len % CHD_HUNK) buf_zero(out, 1);
        data_off = out->len;
        for (uint32_t h = 0; h < hunks; h++) {
            const uint8_t *src = iso + (size_t)h * CHD_HUNK;
            bool zero = true;
            for (uint32_t i = 0; i < CHD_HUNK; i++) zero = zero && src[i] == 0;
            if (zero && h != 0u) continue; /* leave the entry 0 */
            put_be32(out->data + map_off + (size_t)h * 4u, (uint32_t)(out->len / CHD_HUNK));
            buf_put(out, src, CHD_HUNK);
        }
        (void)data_off;
        return;
    }

    /* Compressed map: hunk payloads first (in a side buffer), then the map. */
    {
        Buf payload = { NULL, 0, 0 };
        Buf bits = { NULL, 0, 0 };
        MsbWriter w = { &bits, 0, 0 };
        uint8_t *raw = (uint8_t *)calloc(hunks, 12u);
        uint32_t *lengths = (uint32_t *)calloc(hunks, sizeof(uint32_t));
        uint8_t *types = (uint8_t *)calloc(hunks, 1u);
        uint64_t first_offs;
        uint64_t cur;
        uint8_t maphdr[16];
        assert(raw && lengths && types);
        for (uint32_t h = 0; h < hunks; h++) {
            const uint8_t *src = iso + (size_t)h * CHD_HUNK;
            size_t before = payload.len;
            types[h] = 4; /* COMPRESSION_NONE */
            if (h % 2u == 0u) {
                deflate_fixed(&payload, src, CHD_HUNK);
                /* A compressed hunk must be smaller than a hunk; one that
                   did not shrink is stored, as chdman does. */
                if (payload.len - before < CHD_HUNK) {
                    types[h] = 0; /* COMPRESSION_TYPE_0: codec 0, zlib */
                } else {
                    payload.len = before;
                }
            }
            if (types[h] == 4) buf_put(&payload, src, CHD_HUNK);
            lengths[h] = (uint32_t)(payload.len - before);
        }
        /* Huffman tree for the 16 type symbols, 4 bits per entry: symbols 0
           and 4 get one-bit codes (0 and 1); a literal 1 is escaped as 1,1. */
        for (uint32_t s = 0; s < 16u; s++) {
            if (s == 0u || s == 4u) {
                msb_put(&w, 1, 4);
                msb_put(&w, 1, 4);
            } else {
                msb_put(&w, 0, 4);
            }
        }
        for (uint32_t h = 0; h < hunks; h++) msb_put(&w, types[h] == 0 ? 0u : 1u, 1);
        first_offs = 0; /* fixed below once the map size is known */
        for (uint32_t h = 0; h < hunks; h++) {
            uint16_t crc = crc16_ccitt(iso + (size_t)h * CHD_HUNK, CHD_HUNK);
            if (types[h] == 0) msb_put(&w, lengths[h], 16);
            msb_put(&w, crc, 16);
        }
        msb_flush(&w);
        first_offs = map_off + 16u + bits.len + 64u; /* a gap before the hunks */
        cur = first_offs;
        for (uint32_t h = 0; h < hunks; h++) {
            uint8_t *e = raw + (size_t)h * 12u;
            uint32_t len = lengths[h]; /* a NONE hunk records the hunk size */
            e[0] = types[h];
            e[1] = (uint8_t)(len >> 16);
            e[2] = (uint8_t)(len >> 8);
            e[3] = (uint8_t)len;
            for (int b = 0; b < 6; b++) e[4 + b] = (uint8_t)(cur >> (8 * (5 - b)));
            put_be16(e + 10, crc16_ccitt(iso + (size_t)h * CHD_HUNK, CHD_HUNK));
            cur += lengths[h];
        }
        put_be32(maphdr, (uint32_t)bits.len);
        for (int b = 0; b < 6; b++) maphdr[4 + b] = (uint8_t)(first_offs >> (8 * (5 - b)));
        put_be16(maphdr + 10, crc16_ccitt(raw, (size_t)hunks * 12u));
        maphdr[12] = 16; /* lengthbits */
        maphdr[13] = 0;  /* selfbits */
        maphdr[14] = 0;  /* parentbits */
        maphdr[15] = 0;
        buf_put(out, maphdr, sizeof(maphdr));
        buf_put(out, bits.data, bits.len);
        buf_zero(out, 64u);
        assert(out->len == first_offs);
        buf_put(out, payload.data, payload.len);
        free(payload.data);
        free(bits.data);
        free(raw);
        free(lengths);
        free(types);
    }
}

/* ---- callbacks ---------------------------------------------------------------- */

typedef struct {
    uint64_t last_done;
    uint64_t total;
    int calls;
    bool monotonic;
    int cancel_after;
} Progress;

static void on_progress(uint64_t done, uint64_t total, void *user) {
    Progress *p = (Progress *)user;
    if (done < p->last_done) p->monotonic = false;
    p->last_done = done;
    p->total = total;
    p->calls++;
}

static bool on_cancel(void *user) {
    Progress *p = (Progress *)user;
    return p->cancel_after >= 0 && p->calls > p->cancel_after;
}

/* ---- tests ---------------------------------------------------------------------- */

static void expect_expands(const char *label, const Buf *image, const uint8_t *iso, size_t iso_len) {
    char src[NATIVE_TEST_PATH_MAX];
    char dst[NATIVE_TEST_PATH_MAX];
    char part[NATIVE_TEST_PATH_MAX + 8];
    char name[64];
    NkDiscExpandStatus status;
    Progress progress = { 0, 0, 0, true, -1 };
    NkDiscExpandCallbacks callbacks = { on_progress, on_cancel, &progress };
    uint64_t bytes = 0;
    NkDiscFormat format;
    uint64_t probed = 0;
    snprintf(name, sizeof(name), "%s.img", label);
    path_in(src, sizeof(src), name);
    snprintf(name, sizeof(name), "%s.iso", label);
    path_in(dst, sizeof(dst), name);
    write_bytes(src, image->data, image->len);
    if (!nk_disc_expanded_size(src, &format, &probed, &status)) {
        fprintf(stderr, "%s: %s: %s\n", label, status.code, status.message);
        CHECK(false);
    }
    CHECK(probed == iso_len);
    if (!nk_disc_expand_file(src, dst, &callbacks, &bytes, &status)) {
        fprintf(stderr, "%s: %s: %s\n", label, status.code, status.message);
        CHECK(false);
    }
    CHECK(bytes == iso_len);
    CHECK(file_equals(dst, iso, iso_len));
    CHECK(progress.monotonic && progress.last_done == iso_len && progress.total == iso_len);
    snprintf(part, sizeof(part), "%s.part", dst);
    CHECK(nk_platform_path_missing(part));
    printf("PASS expand %s (%s, %zu -> %zu bytes)\n", label,
           nk_disc_format_extension(format), image->len, iso_len);
}

static void expect_refused(const char *label, const uint8_t *data, size_t len, const char *code) {
    char src[NATIVE_TEST_PATH_MAX];
    char dst[NATIVE_TEST_PATH_MAX];
    char part[NATIVE_TEST_PATH_MAX + 8];
    char name[64];
    NkDiscExpandStatus status;
    uint64_t bytes = 7;
    snprintf(name, sizeof(name), "bad-%s.img", label);
    path_in(src, sizeof(src), name);
    snprintf(name, sizeof(name), "bad-%s.iso", label);
    path_in(dst, sizeof(dst), name);
    write_bytes(src, data, len);
    CHECK(!nk_disc_expand_file(src, dst, NULL, &bytes, &status));
    if (strcmp(status.code, code) != 0) {
        fprintf(stderr, "%s: expected %s, got %s: %s\n", label, code, status.code, status.message);
        CHECK(false);
    }
    CHECK(status.message[0] != '\0');
    CHECK(bytes == 0);
    CHECK(nk_platform_path_missing(dst));
    snprintf(part, sizeof(part), "%s.part", dst);
    CHECK(nk_platform_path_missing(part));
    printf("PASS refuse %s -> %s\n", label, code);
}

static void test_cso_zso_round_trips(void) {
    Buf image = { NULL, 0, 0 };
    build_ciso(&image, "CISO", 1, 0, g_iso, ISO_BYTES, cso_v1_kind);
    expect_expands("cso-v1", &image, g_iso, ISO_BYTES);
    build_ciso(&image, "CISO", 1, 2, g_iso, ISO_BYTES, cso_v1_kind);
    expect_expands("cso-v1-align2", &image, g_iso, ISO_BYTES);
    build_ciso(&image, "CISO", 2, 0, g_iso, ISO_BYTES, cso_v2_kind);
    expect_expands("cso-v2", &image, g_iso, ISO_BYTES);
    build_ciso(&image, "ZISO", 1, 0, g_iso, ISO_BYTES, zso_kind);
    expect_expands("zso", &image, g_iso, ISO_BYTES);
    build_ciso(&image, "ZISO", 1, 3, g_iso, ISO_BYTES, zso_kind);
    expect_expands("zso-align3", &image, g_iso, ISO_BYTES);
    free(image.data);
}

static void test_chd_round_trips(void) {
    Buf image = { NULL, 0, 0 };
    build_chd(&image, g_iso, ISO_BYTES, CHD_MAP_UNCOMPRESSED, CHD_TAG('D', 'V', 'D', ' '));
    expect_expands("chd-uncompressed-map", &image, g_iso, ISO_BYTES);
    build_chd(&image, g_iso, ISO_BYTES, CHD_MAP_COMPRESSED, CHD_TAG('D', 'V', 'D', ' '));
    expect_expands("chd-zlib", &image, g_iso, ISO_BYTES);
    free(image.data);
}

static void test_malformed_refused(void) {
    Buf image = { NULL, 0, 0 };
    uint8_t *copy;

    /* Truncated index: 64 blocks named, the file ends after 4 entries. */
    build_ciso(&image, "CISO", 1, 0, g_iso, ISO_BYTES, cso_v1_kind);
    expect_refused("cso-truncated-index", image.data, 24u + 16u, NK_DISC_TRUNCATED);
    expect_refused("cso-truncated-header", image.data, 20u, NK_DISC_TRUNCATED);

    /* A block offset past the end of the file. */
    copy = (uint8_t *)malloc(image.len);
    assert(copy);
    memcpy(copy, image.data, image.len);
    put_le32(copy + 24u + 10u * 4u, (uint32_t)image.len + 4096u);
    expect_refused("cso-offset-past-end", copy, image.len, NK_DISC_INDEX_INVALID);

    /* Offsets going backwards. */
    memcpy(copy, image.data, image.len);
    put_le32(copy + 24u + 11u * 4u, 30u);
    expect_refused("cso-offset-backwards", copy, image.len, NK_DISC_INDEX_INVALID);

    /* A file that ends inside its last block. */
    expect_refused("cso-truncated-data", image.data, image.len - 100u, NK_DISC_INDEX_INVALID);

    /* Size overflow and sizes no disc has. */
    memcpy(copy, image.data, image.len);
    put_le64(copy + 8, UINT64_MAX - 1023u);
    expect_refused("cso-size-overflow", copy, image.len, NK_DISC_SIZE_INVALID);
    put_le64(copy + 8, NK_DISC_MAX_EXPANDED_BYTES + SECTOR);
    expect_refused("cso-size-too-large", copy, image.len, NK_DISC_SIZE_INVALID);
    put_le64(copy + 8, ISO_BYTES - 1u);
    expect_refused("cso-size-not-sectors", copy, image.len, NK_DISC_SIZE_INVALID);
    put_le64(copy + 8, 0);
    expect_refused("cso-size-zero", copy, image.len, NK_DISC_SIZE_INVALID);

    /* Version, header size, block size and alignment. */
    memcpy(copy, image.data, image.len);
    copy[20] = 3;
    expect_refused("cso-version-3", copy, image.len, NK_DISC_VERSION_UNSUPPORTED);
    memcpy(copy, image.data, image.len);
    put_le32(copy + 4, 32u);
    expect_refused("cso-header-size", copy, image.len, NK_DISC_HEADER_INVALID);
    memcpy(copy, image.data, image.len);
    put_le32(copy + 16, 3000u);
    expect_refused("cso-block-size", copy, image.len, NK_DISC_HEADER_INVALID);
    memcpy(copy, image.data, image.len);
    copy[21] = 31;
    expect_refused("cso-align", copy, image.len, NK_DISC_HEADER_INVALID);

    /* A DEFLATE block whose data is damaged (block 0 is fixed-Huffman). */
    memcpy(copy, image.data, image.len);
    {
        uint32_t first = (uint32_t)(copy[24] | (copy[25] << 8) | (copy[26] << 16));
        copy[first] = 0x07; /* BFINAL=1, BTYPE=11: reserved */
    }
    expect_refused("cso-bad-deflate", copy, image.len, NK_DISC_BLOCK_CORRUPT);
    free(copy);

    /* ZSO: wrong version, and an LZ4 match pointing before the block. */
    build_ciso(&image, "ZISO", 1, 0, g_iso, ISO_BYTES, zso_kind);
    copy = (uint8_t *)malloc(image.len);
    assert(copy);
    memcpy(copy, image.data, image.len);
    copy[20] = 2;
    expect_refused("zso-version-2", copy, image.len, NK_DISC_VERSION_UNSUPPORTED);
    memcpy(copy, image.data, image.len);
    {
        uint32_t first = (uint32_t)(copy[24] | (copy[25] << 8) | (copy[26] << 16));
        copy[first] = 0x0F; /* no literals, then a match */
        copy[first + 1] = 0x05;
        copy[first + 2] = 0x00; /* offset 5 with nothing written yet */
    }
    expect_refused("zso-bad-offset", copy, image.len, NK_DISC_BLOCK_CORRUPT);
    free(copy);

    /* CHD: a CD-layout image, a header with nothing after it, and a build
       without the CHD reader. */
    build_chd(&image, g_iso, ISO_BYTES, CHD_MAP_UNCOMPRESSED, CHD_TAG('C', 'H', 'T', '2'));
    expect_refused("chd-cd", image.data, image.len, NK_DISC_CHD_CD_UNSUPPORTED);
    build_chd(&image, g_iso, ISO_BYTES, CHD_MAP_UNCOMPRESSED, CHD_TAG('G', 'D', 'D', 'D'));
    expect_refused("chd-hard-disk", image.data, image.len, NK_DISC_CHD_UNSUPPORTED);
    build_chd(&image, g_iso, ISO_BYTES, CHD_MAP_UNCOMPRESSED, CHD_TAG('D', 'V', 'D', ' '));
    expect_refused("chd-truncated", image.data, 124u, NK_DISC_CHD_INVALID);
    nk_disc_set_chd_backend(NULL);
    expect_refused("chd-no-backend", image.data, image.len, NK_DISC_CHD_UNAVAILABLE);
    nk_disc_chd_register();

    /* A plain ISO is not a compressed image. */
    expect_refused("plain-iso", g_iso, ISO_BYTES, NK_DISC_NOT_COMPRESSED);
    free(image.data);
}

/* libchdr LOCAL DELTA (third_party/libchdr/PROVENANCE.md): a Huffman decoder
   that is reused across hunks (the HUFF codec's) imports a malformed tree, a
   single one-bit code, that leaves half the lookup table unwritten. Every gap
   decodes as the bounded placeholder (symbol 0, one bit), not as whatever the
   previous hunk's tree, or uninitialised heap, left there. */
static void test_chd_huffman_gap_is_deterministic(void) {
    /* RLE-coded trees for 4 codes at most 2 bits, 3 bits per entry. */
    static const uint8_t full_tree[] = { 0x49, 0x20, 0x00, 0x00 };  /* lengths 2,2,2,2 */
    static const uint8_t gap_tree[] = { 0x24, 0x00, 0x00, 0x00 };   /* lengths 1,0,0,0 */
    static const uint8_t data[] = { 0xC0, 0x00, 0x00, 0x00 };       /* window 0b11 */
    struct huffman_decoder *decoder = create_huffman_decoder(4, 2);
    struct bitstream *bits;
    CHECK(decoder != NULL);

    bits = create_bitstream(full_tree, sizeof(full_tree));
    CHECK(bits != NULL);
    CHECK(huffman_import_tree_rle(decoder, bits) == HUFFERR_NONE);
    free(bits);
    bits = create_bitstream(data, sizeof(data));
    CHECK(huffman_decode_one(decoder, bits) == 3u); /* the full tree owns 0b11 */
    free(bits);

    bits = create_bitstream(gap_tree, sizeof(gap_tree));
    CHECK(huffman_import_tree_rle(decoder, bits) == HUFFERR_NONE);
    free(bits);
    bits = create_bitstream(data, sizeof(data));
    CHECK(huffman_decode_one(decoder, bits) == 0u); /* not the stale symbol 3 */
    CHECK(bitstream_position_bits(bits) == 1u);     /* one bit consumed */
    CHECK(huffman_decode_one(decoder, bits) == 0u);
    free(bits);
    delete_huffman_decoder(decoder);
    printf("PASS CHD Huffman lookup gaps decode deterministically\n");
}

/* libchdr LOCAL DELTA: a zero-length FLAC hunk is refused before its
   endianness byte, which does not exist, is read. NULL stands for the empty
   buffer; the upstream order dereferences it. */
static void test_chd_flac_empty_hunk_refused(void) {
    flac_codec_data flac;
    uint8_t dest[CHD_HUNK];
    memset(&flac, 0, sizeof(flac));
    CHECK(flac_codec_init(&flac, CHD_HUNK) == CHDERR_NONE);
    CHECK(flac_codec_decompress(&flac, NULL, 0, dest, CHD_HUNK) == CHDERR_DECOMPRESSION_ERROR);
    flac_codec_free(&flac);
    printf("PASS CHD FLAC zero-length hunk refused\n");
}

static void test_cancel_removes_partial(void) {
    Buf image = { NULL, 0, 0 };
    char src[NATIVE_TEST_PATH_MAX];
    char dst[NATIVE_TEST_PATH_MAX];
    char part[NATIVE_TEST_PATH_MAX + 8];
    NkDiscExpandStatus status;
    /* Progress reports every MiB; the image is smaller, so cancel on the
       first report after the start. */
    Progress progress = { 0, 0, 0, true, 1 };
    NkDiscExpandCallbacks callbacks = { on_progress, on_cancel, &progress };
    build_ciso(&image, "CISO", 1, 0, g_iso, ISO_BYTES, cso_v1_kind);
    path_in(src, sizeof(src), "cancel.cso");
    path_in(dst, sizeof(dst), "cancel.iso");
    write_bytes(src, image.data, image.len);
    CHECK(!nk_disc_expand_file(src, dst, &callbacks, NULL, &status));
    CHECK(strcmp(status.code, NK_DISC_CANCELLED) == 0);
    CHECK(nk_platform_path_missing(dst));
    snprintf(part, sizeof(part), "%s.part", dst);
    CHECK(nk_platform_path_missing(part));
    free(image.data);
    printf("PASS cancel removes the partial image\n");
}

static void test_import_and_reuse(void) {
    Buf image = { NULL, 0, 0 };
    char src[NATIVE_TEST_PATH_MAX];
    char root[NATIVE_TEST_PATH_MAX];
    char expected[NATIVE_TEST_PATH_MAX + 64];
    char sep = nk_platform_path_separator();
    NkDiscImport result;
    NkDiscExpandStatus status;
    NkIsoMetadata meta;
    path_in(root, sizeof(root), "data-root");
    CHECK(nk_platform_mkdir_p(root));
    path_in(src, sizeof(src), "import.cso");
    build_ciso(&image, "CISO", 1, 0, g_iso, ISO_BYTES, cso_v1_kind);
    write_bytes(src, image.data, image.len);

    /* A compressed image is named by nk_iso_inspect, never read as ISO9660. */
    memset(&meta, 0, sizeof(meta));
    CHECK(nk_iso_inspect(src, &meta) != NK_OK);
    CHECK(strcmp(meta.boundary_code, "DISC_IMAGE_NOT_EXPANDED") == 0);

    /* The probe reads the header only: no identity, never a reuse. */
    CHECK(nk_disc_import_probe(src, root, &result, &status));
    CHECK(!result.reused && result.format == NK_DISC_FORMAT_CSO);
    CHECK(result.expanded_bytes == ISO_BYTES && result.source_bytes == image.len);
    CHECK(result.identity[0] == '\0');

    if (!nk_disc_import(src, root, NULL, &result, &status)) {
        fprintf(stderr, "import: %s: %s\n", status.code, status.message);
        CHECK(false);
    }
    CHECK(!result.reused);
    /* The identity is the SHA-256 of the whole file, and the per-title name
       carries its start. */
    {
        char identity[NK_DISC_IDENTITY_CHARS + 1];
        uint64_t hashed = 0;
        CHECK(strlen(result.identity) == NK_DISC_IDENTITY_CHARS);
        CHECK(nk_disc_file_identity(src, NULL, identity, &hashed, &status));
        CHECK(strcmp(identity, result.identity) == 0 && hashed == image.len);
    }
    snprintf(expected, sizeof(expected), "%s%cdiscs%cTEST00042%cTEST00042.%.16s.cso.iso", root,
             sep, sep, sep, result.identity);
    CHECK(strcmp(result.disc_id, "TEST00042") == 0);
    CHECK(strcmp(result.iso_path, expected) == 0);
    CHECK(file_equals(result.iso_path, g_iso, ISO_BYTES));
    memset(&meta, 0, sizeof(meta));
    CHECK(nk_iso_inspect(result.iso_path, &meta) == NK_OK);
    CHECK(strcmp(meta.disc_id, "TEST00042") == 0 && meta.has_psp_game_folder);

    /* Disc verification reads the expansion the import made (nk_disc_verify.h), so
       the compressed image gets the SHA-1, CRC-32 and size of the .iso it holds,
       never those of the compressed file. */
    {
        char plain[NATIVE_TEST_PATH_MAX];
        NkDiscVerification from_import, from_iso, from_compressed;
        path_in(plain, sizeof(plain), "import-plain.iso");
        write_bytes(plain, g_iso, ISO_BYTES);
        CHECK(nk_disc_verify_image(result.iso_path, root, NULL, result.disc_id, false, NULL,
                                   &from_import));
        CHECK(nk_disc_verify_image(plain, root, NULL, "TEST00042", false, NULL, &from_iso));
        CHECK(nk_disc_verify_image(src, root, NULL, "TEST00042", false, NULL, &from_compressed));
        CHECK(from_import.result == NK_DISC_VERIFY_HASHED);
        CHECK(from_import.digest.size_bytes == ISO_BYTES);
        CHECK(strcmp(from_import.digest.sha1, from_iso.digest.sha1) == 0);
        CHECK(strcmp(from_import.digest.crc32, from_iso.digest.crc32) == 0);
        CHECK(from_iso.digest.size_bytes == ISO_BYTES);
        CHECK(strcmp(from_import.digest.sha1, from_compressed.digest.sha1) != 0);
        CHECK(from_compressed.digest.size_bytes == image.len);
    }

    /* The same unchanged file again: reused, nothing written. */
    CHECK(nk_disc_import(src, root, NULL, &result, &status));
    CHECK(result.reused);
    CHECK(strcmp(result.iso_path, expected) == 0);

    /* An expansion whose size changed on the drive is not reused. Only the
       size is checked: a change that keeps the size is not seen by the probe. */
    {
        FILE *f = nk_fopen_utf8(expected, "ab");
        assert(f);
        fputc(0, f);
        fclose(f);
    }
    CHECK(nk_disc_import(src, root, NULL, &result, &status));
    CHECK(!result.reused && file_equals(result.iso_path, g_iso, ISO_BYTES));

    /* A different file (a ZSO of the same disc) gets its own expansion. */
    path_in(src, sizeof(src), "import.zso");
    build_ciso(&image, "ZISO", 1, 0, g_iso, ISO_BYTES, zso_kind);
    write_bytes(src, image.data, image.len);
    CHECK(nk_disc_import(src, root, NULL, &result, &status));
    CHECK(!result.reused && strstr(result.iso_path, "TEST00042.") != NULL &&
          strstr(result.iso_path, ".zso.iso") != NULL);
    CHECK(file_equals(result.iso_path, g_iso, ISO_BYTES));

    /* A CHD imports the same way. */
    path_in(src, sizeof(src), "import.chd");
    build_chd(&image, g_iso, ISO_BYTES, CHD_MAP_COMPRESSED, CHD_TAG('D', 'V', 'D', ' '));
    write_bytes(src, image.data, image.len);
    CHECK(nk_disc_import(src, root, NULL, &result, &status));
    CHECK(!result.reused && strstr(result.iso_path, "TEST00042.") != NULL &&
          strstr(result.iso_path, ".chd.iso") != NULL);
    CHECK(file_equals(result.iso_path, g_iso, ISO_BYTES));
    CHECK(nk_disc_import(src, root, NULL, &result, &status) && result.reused);

    /* A compressed image of something that is not a PSP game is refused, and
       nothing of it is kept. */
    path_in(src, sizeof(src), "not-psp.cso");
    build_ciso(&image, "CISO", 1, 0, g_other_iso, ISO_BYTES, cso_v1_kind);
    write_bytes(src, image.data, image.len);
    CHECK(!nk_disc_import(src, root, NULL, &result, &status));
    CHECK(strcmp(status.code, NK_DISC_NOT_PSP) == 0);
    {
        char staged[NATIVE_TEST_PATH_MAX + 96];
        CHECK(strlen(result.identity) == NK_DISC_IDENTITY_CHARS);
        snprintf(staged, sizeof(staged), "%s%cdiscs%cexpanded%c%s.iso", root, sep, sep, sep,
                 result.identity);
        CHECK(nk_platform_path_missing(staged));
    }
    free(image.data);
    printf("PASS import, reuse and per-title placement\n");
}

/* The SHA-1 a CHD header records for its data (offset 64 in a v5 header). */
static void chd_set_raw_sha1(Buf *image, const uint8_t *iso, size_t iso_len) {
    SHA_CTX ctx;
    SHAInit(&ctx);
    SHAUpdate(&ctx, (BYTE *)iso, (int)iso_len);
    SHAFinal(image->data + 64, &ctx);
}

/* A damaged metadata chain is a damaged image, not "not a DVD image". */
static void test_chd_damaged_metadata(void) {
    Buf image = { NULL, 0, 0 };
    /* A hard-disk tag whose entry links back to itself: every search for a
       tag it does not hold runs round the cycle until libchdr gives up. */
    build_chd(&image, g_iso, ISO_BYTES, CHD_MAP_UNCOMPRESSED, CHD_TAG('G', 'D', 'D', 'D'));
    put_be64(image.data + 124u + 8u, 124u);
    expect_refused("chd-metadata-cycle", image.data, image.len, NK_DISC_CHD_INVALID);
    free(image.data);
}

/* An uncompressed CHD hunk carries no CRC; the header's SHA-1 of the data
   is what finds a damaged one. */
static void test_chd_raw_sha1(void) {
    Buf image = { NULL, 0, 0 };
    size_t map_off = 0;
    size_t data_off;
    uint32_t entry;
    build_chd(&image, g_iso, ISO_BYTES, CHD_MAP_UNCOMPRESSED, CHD_TAG('D', 'V', 'D', ' '));
    chd_set_raw_sha1(&image, g_iso, ISO_BYTES);
    expect_expands("chd-uncompressed-sha1", &image, g_iso, ISO_BYTES);
    /* Flip one byte of hunk 12 (sectors 24 and 25, in DATA.BIN) where it is
       stored: its map entry names the place. */
    for (int b = 0; b < 8; b++) map_off = (map_off << 8) | image.data[40 + b];
    map_off += 12u * 4u;
    entry = ((uint32_t)image.data[map_off] << 24) | ((uint32_t)image.data[map_off + 1] << 16) |
            ((uint32_t)image.data[map_off + 2] << 8) | (uint32_t)image.data[map_off + 3];
    CHECK(entry != 0u);
    data_off = (size_t)entry * CHD_HUNK + 100u;
    CHECK(data_off < image.len);
    image.data[data_off] ^= 0x5Au;
    expect_refused("chd-uncompressed-flipped", image.data, image.len, NK_DISC_BLOCK_CORRUPT);
    build_chd(&image, g_iso, ISO_BYTES, CHD_MAP_COMPRESSED, CHD_TAG('D', 'V', 'D', ' '));
    chd_set_raw_sha1(&image, g_iso, ISO_BYTES);
    expect_expands("chd-zlib-sha1", &image, g_iso, ISO_BYTES);
    free(image.data);
}

/* Header fields no PSP disc has are refused from the header alone, before
   libchdr's open allocates for them: these files end right after the header,
   so the full open would only report them as damaged. */
static void test_chd_header_caps(void) {
    Buf image = { NULL, 0, 0 };
    build_chd(&image, g_iso, ISO_BYTES, CHD_MAP_UNCOMPRESSED, CHD_TAG('D', 'V', 'D', ' '));
    put_be64(image.data + 32, 8ull * 1024u * 1024u * 1024u);
    expect_refused("chd-header-size", image.data, 124u, NK_DISC_SIZE_INVALID);
    build_chd(&image, g_iso, ISO_BYTES, CHD_MAP_UNCOMPRESSED, CHD_TAG('D', 'V', 'D', ' '));
    put_be32(image.data + 56, 32u * 1024u * 1024u);
    expect_refused("chd-header-hunk", image.data, 124u, NK_DISC_CHD_UNSUPPORTED);
    free(image.data);
}

/* An intact CHD whose codec this libchdr lacks is unsupported, not damaged. */
static void test_chd_unknown_codec(void) {
    Buf image = { NULL, 0, 0 };
    build_chd(&image, g_iso, ISO_BYTES, CHD_MAP_COMPRESSED, CHD_TAG('D', 'V', 'D', ' '));
    put_be32(image.data + 16, CHD_TAG('n', 'k', 'x', 'x'));
    expect_refused("chd-unknown-codec", image.data, image.len, NK_DISC_CHD_UNSUPPORTED);
    free(image.data);
}

/* Two different images of one disc ID (say a retail dump and a fan
   translation) keep two expansions: neither import replaces the other. The
   two files here have one write time, so what keeps them apart is their
   bytes, never the moment each happened to be written. */
static void test_import_same_disc_id(void) {
    Buf image = { NULL, 0, 0 };
    char root[NATIVE_TEST_PATH_MAX];
    char first_src[NATIVE_TEST_PATH_MAX];
    char second_src[NATIVE_TEST_PATH_MAX];
    char first_iso[NK_MAX_PATH];
    static uint8_t translated[ISO_BYTES];
    size_t first_len;
    NkDiscImport result;
    NkDiscExpandStatus status;
    /* The same disc ID with different data: the first DATA.BIN sector is
       rewritten. Its block is deflated, so the compressed image's size and
       block index change with it, as a translation's do. */
    memcpy(translated, g_iso, ISO_BYTES);
    memset(translated + (size_t)DATA_FIRST * SECTOR, 'T', SECTOR);
    path_in(root, sizeof(root), "same-id-root");
    CHECK(nk_platform_mkdir_p(root));
    path_in(first_src, sizeof(first_src), "same-id-a.cso");
    build_ciso(&image, "CISO", 1, 0, g_iso, ISO_BYTES, cso_v1_kind);
    write_bytes(first_src, image.data, image.len);
    first_len = image.len;
    path_in(second_src, sizeof(second_src), "same-id-b.cso");
    build_ciso(&image, "CISO", 1, 0, translated, ISO_BYTES, cso_v1_kind);
    write_bytes(second_src, image.data, image.len);
    CHECK(image.len != first_len);
    stamp_write_time(first_src);
    stamp_write_time(second_src);

    CHECK(nk_disc_import(first_src, root, NULL, &result, &status) && !result.reused);
    CHECK(strcmp(result.disc_id, "TEST00042") == 0);
    snprintf(first_iso, sizeof(first_iso), "%s", result.iso_path);
    CHECK(nk_disc_import(second_src, root, NULL, &result, &status) && !result.reused);
    CHECK(strcmp(result.disc_id, "TEST00042") == 0);
    CHECK(strcmp(result.iso_path, first_iso) != 0);
    CHECK(file_equals(first_iso, g_iso, ISO_BYTES));
    CHECK(file_equals(result.iso_path, translated, ISO_BYTES));
    /* Adding the first again reuses its own expansion. */
    CHECK(nk_disc_import(first_src, root, NULL, &result, &status) && result.reused);
    CHECK(strcmp(result.iso_path, first_iso) == 0);
    free(image.data);
    printf("PASS two images of one disc ID keep two expansions\n");
}

/* Every block stored as it is, so a change to one sector changes only that
   sector's bytes in the compressed file: its size and block index stay. */
static BlockKind stored_kind(uint32_t block) {
    (void)block;
    return BLOCK_STORED;
}

/* Two images with one size, one write time and the same first 64 KiB, which
   differ only further on (two dumps of one game, or a patch that touches a
   late file), are two imports: neither is served the other's data
   (F-f6233fe0). The same bytes under another name, in another folder and
   with another write time, are the same disc. */
static void test_import_full_content_identity(void) {
    Buf image = { NULL, 0, 0 };
    char root[NATIVE_TEST_PATH_MAX];
    char first_src[NATIVE_TEST_PATH_MAX];
    char second_src[NATIVE_TEST_PATH_MAX];
    char elsewhere[NATIVE_TEST_PATH_MAX];
    char copy_src[NATIVE_TEST_PATH_MAX + 32];
    char first_iso[NK_MAX_PATH];
    static uint8_t late[ISO_BYTES];
    uint8_t *first_bytes;
    size_t first_len = 0;
    NkDiscImport result;
    NkDiscExpandStatus status;
    memcpy(late, g_iso, ISO_BYTES);
    late[(size_t)50u * SECTOR + 100u] ^= 0x5Au; /* in DATA.BIN, past the first 64 KiB */
    path_in(root, sizeof(root), "identity-root");
    CHECK(nk_platform_mkdir_p(root));
    path_in(first_src, sizeof(first_src), "identity-a.cso");
    path_in(second_src, sizeof(second_src), "identity-b.cso");
    build_ciso(&image, "CISO", 1, 0, g_iso, ISO_BYTES, stored_kind);
    write_bytes(first_src, image.data, image.len);
    build_ciso(&image, "CISO", 1, 0, late, ISO_BYTES, stored_kind);
    write_bytes(second_src, image.data, image.len);
    stamp_write_time(first_src);
    stamp_write_time(second_src);
    first_bytes = read_all(first_src, &first_len);
    CHECK(first_bytes != NULL && first_len == image.len && first_len > 64u * 1024u);
    CHECK(memcmp(first_bytes, image.data, 64u * 1024u) == 0);
    CHECK(memcmp(first_bytes, image.data, first_len) != 0);

    CHECK(nk_disc_import(first_src, root, NULL, &result, &status) && !result.reused);
    snprintf(first_iso, sizeof(first_iso), "%s", result.iso_path);
    if (!nk_disc_import(second_src, root, NULL, &result, &status)) {
        fprintf(stderr, "identity: %s: %s\n", status.code, status.message);
        CHECK(false);
    }
    CHECK(!result.reused);
    CHECK(strcmp(result.disc_id, "TEST00042") == 0);
    CHECK(strcmp(result.iso_path, first_iso) != 0);
    CHECK(file_equals(result.iso_path, late, ISO_BYTES));
    CHECK(file_equals(first_iso, g_iso, ISO_BYTES));

    /* The first image's bytes under another name and folder, written now. */
    path_in(elsewhere, sizeof(elsewhere), "identity-elsewhere");
    CHECK(nk_platform_mkdir_p(elsewhere));
    snprintf(copy_src, sizeof(copy_src), "%s%crenamed.cso", elsewhere,
             nk_platform_path_separator());
    write_bytes(copy_src, first_bytes, first_len);
    CHECK(nk_disc_import(copy_src, root, NULL, &result, &status) && result.reused);
    CHECK(strcmp(result.iso_path, first_iso) == 0);
    free(first_bytes);
    free(image.data);
    printf("PASS an import is matched by its whole content, not a sample of it\n");
}

static void write_line(const char *path, const char *line) {
    FILE *f = nk_fopen_utf8(path, "wb");
    assert(f != NULL);
    assert(fprintf(f, "%s\n", line) > 0);
    assert(fclose(f) == 0);
}

static bool line_equals(const char *path, const char *expected) {
    size_t got = 0;
    uint8_t *data = read_all(path, &got);
    size_t want = strlen(expected);
    bool same = data && got == want + 1u && memcmp(data, expected, want) == 0 && data[want] == '\n';
    free(data);
    return same;
}

/* Turns this build's import of src back into the record a build before the
   full identity wrote: the image as <ID>.<fp>.cso.iso (or <ID>.cso.iso when
   unnamed), its .source holding the sampled fingerprint, <fp>.ref holding the
   disc ID, and no ref for the identity. */
static void make_sampled_record(const char *root, const char *src, const NkDiscImport *import,
                                bool unnamed, char *legacy_iso, size_t legacy_size) {
    char fingerprint[NK_DISC_LEGACY_FINGERPRINT_CHARS + 1];
    char path[NK_MAX_PATH + 96];
    char sep = nk_platform_path_separator();
    NkDiscExpandStatus status;
    CHECK(nk_disc_legacy_fingerprint(src, NK_DISC_FORMAT_CSO, fingerprint, &status));
    CHECK(strlen(fingerprint) == NK_DISC_LEGACY_FINGERPRINT_CHARS);
    if (unnamed) {
        snprintf(legacy_iso, legacy_size, "%s%cdiscs%cTEST00042%cTEST00042.cso.iso", root, sep,
                 sep, sep);
    } else {
        snprintf(legacy_iso, legacy_size, "%s%cdiscs%cTEST00042%cTEST00042.%.16s.cso.iso", root,
                 sep, sep, sep, fingerprint);
    }
    CHECK(nk_rename_utf8(import->iso_path, legacy_iso) == 0);
    snprintf(path, sizeof(path), "%s.source", import->iso_path);
    CHECK(nk_remove_utf8(path) == 0);
    snprintf(path, sizeof(path), "%s.source", legacy_iso);
    write_line(path, fingerprint);
    snprintf(path, sizeof(path), "%s%cdiscs%cexpanded%c%s.ref", root, sep, sep, sep,
             import->identity);
    CHECK(nk_remove_utf8(path) == 0);
    snprintf(path, sizeof(path), "%s%cdiscs%cexpanded%c%s.ref", root, sep, sep, sep, fingerprint);
    write_line(path, "TEST00042");
}

/* An import an earlier build recorded under the sampled fingerprint is never
   a match on that record alone: a different file with the same sample gets
   its own expansion and leaves the record as it was, and the file the record
   is for is compared with the image byte for byte, then upgraded where it is
   (its name kept, so a library card naming it keeps working). Both earlier
   names, <ID>.<fp>.cso.iso and <ID>.cso.iso, are found. */
static void test_import_upgrades_sampled_record(void) {
    Buf image = { NULL, 0, 0 };
    char root[NATIVE_TEST_PATH_MAX];
    char named_root[NATIVE_TEST_PATH_MAX];
    char first_src[NATIVE_TEST_PATH_MAX];
    char second_src[NATIVE_TEST_PATH_MAX];
    char legacy_iso[NK_MAX_PATH + 64];
    char legacy_sidecar[NK_MAX_PATH + 80];
    char first_sample[NK_DISC_LEGACY_FINGERPRINT_CHARS + 1];
    char second_sample[NK_DISC_LEGACY_FINGERPRINT_CHARS + 1];
    char expected[NK_DISC_IDENTITY_CHARS + 16];
    char sep = nk_platform_path_separator();
    static uint8_t late[ISO_BYTES];
    NkDiscImport result;
    NkDiscExpandStatus status;
    memcpy(late, g_iso, ISO_BYTES);
    late[(size_t)50u * SECTOR + 100u] ^= 0x5Au;
    path_in(root, sizeof(root), "sampled-root");
    CHECK(nk_platform_mkdir_p(root));
    path_in(first_src, sizeof(first_src), "sampled-a.cso");
    path_in(second_src, sizeof(second_src), "sampled-b.cso");
    build_ciso(&image, "CISO", 1, 0, g_iso, ISO_BYTES, stored_kind);
    write_bytes(first_src, image.data, image.len);
    build_ciso(&image, "CISO", 1, 0, late, ISO_BYTES, stored_kind);
    write_bytes(second_src, image.data, image.len);
    stamp_write_time(first_src);
    stamp_write_time(second_src);
    CHECK(nk_disc_legacy_fingerprint(first_src, NK_DISC_FORMAT_CSO, first_sample, &status));
    CHECK(nk_disc_legacy_fingerprint(second_src, NK_DISC_FORMAT_CSO, second_sample, &status));
    CHECK(strcmp(first_sample, second_sample) == 0);

    CHECK(nk_disc_import(first_src, root, NULL, &result, &status) && !result.reused);
    make_sampled_record(root, first_src, &result, true, legacy_iso, sizeof(legacy_iso));
    snprintf(legacy_sidecar, sizeof(legacy_sidecar), "%s.source", legacy_iso);

    /* The other file with the same sample: compared, not this record's. */
    if (!nk_disc_import(second_src, root, NULL, &result, &status)) {
        fprintf(stderr, "sampled: %s: %s\n", status.code, status.message);
        CHECK(false);
    }
    CHECK(!result.reused && strcmp(result.iso_path, legacy_iso) != 0);
    CHECK(file_equals(result.iso_path, late, ISO_BYTES));
    CHECK(file_equals(legacy_iso, g_iso, ISO_BYTES));
    CHECK(line_equals(legacy_sidecar, first_sample));

    /* The record's own file: compared equal, upgraded in place. */
    CHECK(nk_disc_import(first_src, root, NULL, &result, &status) && result.reused);
    CHECK(strcmp(result.iso_path, legacy_iso) == 0);
    CHECK(strcmp(result.disc_id, "TEST00042") == 0);
    CHECK(file_equals(legacy_iso, g_iso, ISO_BYTES));
    snprintf(expected, sizeof(expected), "sha256:%s", result.identity);
    CHECK(line_equals(legacy_sidecar, expected));
    /* From now on the identity's own ref finds it, with the sampled ref gone. */
    {
        char ref[NATIVE_TEST_PATH_MAX + 96];
        snprintf(ref, sizeof(ref), "%s%cdiscs%cexpanded%c%s.ref", root, sep, sep, sep,
                 first_sample);
        CHECK(nk_remove_utf8(ref) == 0);
    }
    CHECK(nk_disc_import(first_src, root, NULL, &result, &status) && result.reused);
    CHECK(strcmp(result.iso_path, legacy_iso) == 0);

    /* The named earlier form is upgraded the same way. */
    path_in(named_root, sizeof(named_root), "sampled-named-root");
    CHECK(nk_platform_mkdir_p(named_root));
    CHECK(nk_disc_import(first_src, named_root, NULL, &result, &status) && !result.reused);
    make_sampled_record(named_root, first_src, &result, false, legacy_iso, sizeof(legacy_iso));
    CHECK(nk_disc_import(first_src, named_root, NULL, &result, &status) && result.reused);
    CHECK(strcmp(result.iso_path, legacy_iso) == 0);
    free(image.data);
    printf("PASS a sampled record is upgraded only for the file it holds\n");
}

/* A CSO of a 3 MiB image, every block stored, so its identity is read in
   more than one piece. Only its header is valid as a disc: these tests stop
   before the expansion. */
static void build_large_cso(Buf *image) {
    size_t len = 3u * 1024u * 1024u;
    uint8_t *iso = (uint8_t *)calloc(len, 1u);
    assert(iso != NULL);
    memcpy(iso, g_iso, ISO_BYTES);
    build_ciso(image, "CISO", 1, 0, iso, len, stored_kind);
    free(iso);
}

/* What a test does to the source once, at the first progress report past
   `after` bytes of the import's bar (identity, expansion or comparison,
   identity again). */
typedef enum {
    MEDDLE_SHRINK,  /* cut the file to `keep` bytes */
    MEDDLE_GROW,    /* append a byte */
    MEDDLE_REWRITE, /* write `bytes` (the same size) and give it the fixed write time again */
    MEDDLE_TOUCH,   /* write the same bytes again: only the write time changes */
    MEDDLE_CANCEL   /* ask for a cancel from here on */
} MeddleKind;

typedef struct {
    const char *path;
    MeddleKind kind;
    uint64_t after;
    uint64_t keep;
    const uint8_t *bytes;
    size_t len;
    bool acted;
    bool refused; /* Windows: the change could not be made, the import holds the file */
    bool cancel_when_refused;
    bool cancel;
} Meddle;

/* Open the source for writing, as another program would. On Windows the
   import holds it with writes denied, so the open fails; that is recorded,
   nothing is changed, and the import is cancelled when the test asks. */
static FILE *meddle_open(Meddle *m, const char *mode) {
    FILE *f = nk_fopen_utf8(m->path, mode);
    if (!f) {
        m->refused = true;
        if (m->cancel_when_refused) m->cancel = true;
    }
    return f;
}

static void meddle_now(Meddle *m) {
    FILE *f;
    switch (m->kind) {
    case MEDDLE_SHRINK:
#if defined(_WIN32) || defined(_WIN64)
        f = meddle_open(m, "r+b");
        if (f) {
            assert(_chsize_s(_fileno(f), (long long)m->keep) == 0);
            assert(fclose(f) == 0);
        }
#else
        assert(truncate(m->path, (off_t)m->keep) == 0);
#endif
        break;
    case MEDDLE_GROW:
        f = meddle_open(m, "ab");
        if (f) {
            assert(fputc('G', f) == 'G');
            assert(fclose(f) == 0);
        }
        break;
    case MEDDLE_REWRITE:
    case MEDDLE_TOUCH:
        f = meddle_open(m, "r+b");
        if (f) {
            assert(fwrite(m->bytes, 1, m->len, f) == m->len);
            assert(fclose(f) == 0);
            if (m->kind == MEDDLE_REWRITE) stamp_write_time(m->path);
        }
        break;
    case MEDDLE_CANCEL:
        m->cancel = true;
        break;
    }
}

static void on_progress_meddle(uint64_t done, uint64_t total, void *user) {
    Meddle *m = (Meddle *)user;
    (void)total;
    if (!m->acted && done > m->after) {
        m->acted = true;
        meddle_now(m);
    }
}

static bool on_cancel_meddle(void *user) {
    return ((Meddle *)user)->cancel;
}

static bool host_denies_writers(void) {
#if defined(_WIN32) || defined(_WIN64)
    return true;
#else
    return false;
#endif
}

/* After a meddle: where the host denies writers (Windows), the change was
   refused (and the import then cancelled, or went on, as the test asked);
   elsewhere the import stopped as `code`. */
static void expect_meddle_refused(const char *label, const Meddle *m, bool ok,
                                  const NkDiscExpandStatus *status, const char *code) {
    if (!m->acted) {
        fprintf(stderr, "%s: the change was never made\n", label);
        CHECK(false);
    }
    if (host_denies_writers() && m->kind != MEDDLE_CANCEL) {
        if (!m->refused) {
            fprintf(stderr, "%s: a writer was let in while the import held the file\n", label);
            CHECK(false);
        }
        if (m->cancel_when_refused) code = NK_DISC_CANCELLED;
        else if (ok) return;
    }
    if (ok || strcmp(status->code, code) != 0) {
        fprintf(stderr, "%s: got ok=%d %s: %s\n", label, (int)ok, status->code, status->message);
        CHECK(false);
    }
}

/* A file that cannot be read to its end while its identity is read is a
   named import error, never a match on what was read; and a cancel then
   keeps nothing. */
static void test_import_identity_read_error_and_cancel(void) {
    Buf image = { NULL, 0, 0 };
    char root[NATIVE_TEST_PATH_MAX];
    char src[NATIVE_TEST_PATH_MAX];
    char discs[NATIVE_TEST_PATH_MAX + 16];
    NkDiscImport result;
    NkDiscExpandStatus status;
    Meddle meddle;
    NkDiscExpandCallbacks meddling = { on_progress_meddle, on_cancel_meddle, &meddle };
    Progress progress = { 0, 0, 0, true, 0 };
    NkDiscExpandCallbacks cancelling = { on_progress, on_cancel, &progress };
    bool ok;
    build_large_cso(&image);
    path_in(root, sizeof(root), "identity-error-root");
    CHECK(nk_platform_mkdir_p(root));
    snprintf(discs, sizeof(discs), "%s%cdiscs", root, nk_platform_path_separator());
    path_in(src, sizeof(src), "identity-error.cso");

    /* Shrinks after the identity's first piece, as a file on a drive that goes
       away does. Then the cancel below stops the import, whatever happened. */
    write_bytes(src, image.data, image.len);
    memset(&meddle, 0, sizeof(meddle));
    meddle.path = src;
    meddle.kind = MEDDLE_SHRINK;
    meddle.keep = (uint64_t)image.len - 512u * 1024u;
    meddle.cancel_when_refused = true;
    ok = nk_disc_import(src, root, &meddling, &result, &status);
    expect_meddle_refused("shrink while hashed", &meddle, ok, &status, NK_DISC_READ_FAILED);
    CHECK(status.message[0] != '\0');
    CHECK(nk_platform_path_missing(discs));

    /* Grows while it is hashed: not the file that was measured. */
    write_bytes(src, image.data, image.len);
    memset(&meddle, 0, sizeof(meddle));
    meddle.path = src;
    meddle.kind = MEDDLE_GROW;
    meddle.cancel_when_refused = true;
    ok = nk_disc_import(src, root, &meddling, &result, &status);
    expect_meddle_refused("grow while hashed", &meddle, ok, &status, NK_DISC_READ_FAILED);
    CHECK(nk_platform_path_missing(discs));
    CHECK(nk_platform_get_file_size(src) == (int64_t)image.len + (host_denies_writers() ? 0 : 1));

    /* Cancelled after the first piece: nothing of the import is kept. */
    write_bytes(src, image.data, image.len);
    CHECK(!nk_disc_import(src, root, &cancelling, &result, &status));
    CHECK(strcmp(status.code, NK_DISC_CANCELLED) == 0);
    CHECK(progress.calls == 1 && progress.last_done < progress.total);
    CHECK(nk_platform_path_missing(discs));
    free(image.data);
    printf("PASS a read error or a cancel while the identity is read keeps nothing\n");
}

/* <root>/discs/expanded/<name> and <root>/discs/TEST00042/<name>. */
static void expanded_path(char *out, size_t size, const char *root, const char *name) {
    char sep = nk_platform_path_separator();
    int n = snprintf(out, size, "%s%cdiscs%cexpanded%c%s", root, sep, sep, sep, name);
    assert(n > 0 && (size_t)n < size);
}

static void title_path(char *out, size_t size, const char *root, const char *name) {
    char sep = nk_platform_path_separator();
    int n = snprintf(out, size, "%s%cdiscs%cTEST00042%c%s", root, sep, sep, sep, name);
    assert(n > 0 && (size_t)n < size);
}

/* Two stored-block CSOs of one size, one write time and the same first
   64 KiB: `first` holds g_iso, `second` the same disc with one byte changed in
   DATA.BIN. Both written; bytes of each kept for rewrites. */
typedef struct {
    char first[NATIVE_TEST_PATH_MAX];
    char second[NATIVE_TEST_PATH_MAX];
    uint8_t *first_bytes;
    uint8_t *second_bytes;
    size_t len;
} TwinImages;

static uint8_t g_late_iso[ISO_BYTES];

static void make_twins(TwinImages *t, const char *stem) {
    Buf image = { NULL, 0, 0 };
    char name[64];
    memcpy(g_late_iso, g_iso, ISO_BYTES);
    g_late_iso[(size_t)50u * SECTOR + 100u] ^= 0x5Au;
    snprintf(name, sizeof(name), "%s-a.cso", stem);
    path_in(t->first, sizeof(t->first), name);
    snprintf(name, sizeof(name), "%s-b.cso", stem);
    path_in(t->second, sizeof(t->second), name);
    build_ciso(&image, "CISO", 1, 0, g_iso, ISO_BYTES, stored_kind);
    write_bytes(t->first, image.data, image.len);
    t->first_bytes = (uint8_t *)malloc(image.len);
    assert(t->first_bytes != NULL);
    memcpy(t->first_bytes, image.data, image.len);
    t->len = image.len;
    build_ciso(&image, "CISO", 1, 0, g_late_iso, ISO_BYTES, stored_kind);
    assert(image.len == t->len);
    write_bytes(t->second, image.data, image.len);
    t->second_bytes = (uint8_t *)malloc(image.len);
    assert(t->second_bytes != NULL);
    memcpy(t->second_bytes, image.data, image.len);
    stamp_write_time(t->first);
    stamp_write_time(t->second);
    free(image.data);
}

static void free_twins(TwinImages *t) {
    free(t->first_bytes);
    free(t->second_bytes);
}

/* Standard error sent to a file between begin and end, so a test can read what
   an import logged. */
typedef struct {
    int saved;
    char path[NATIVE_TEST_PATH_MAX];
} StderrCapture;

static void stderr_capture_begin(StderrCapture *c, const char *name) {
    int fd;
    path_in(c->path, sizeof(c->path), name);
    fflush(stderr);
#if defined(_WIN32) || defined(_WIN64)
    c->saved = _dup(_fileno(stderr));
    fd = _open(c->path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
    assert(c->saved >= 0 && fd >= 0);
    assert(_dup2(fd, _fileno(stderr)) == 0);
    _close(fd);
#else
    c->saved = dup(fileno(stderr));
    fd = open(c->path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(c->saved >= 0 && fd >= 0);
    assert(dup2(fd, fileno(stderr)) >= 0);
    close(fd);
#endif
}

/* Puts standard error back; whether the captured log holds `line`. */
static bool stderr_capture_end(StderrCapture *c, const char *line) {
    char log[8192];
    size_t got;
    FILE *f;
    fflush(stderr);
#if defined(_WIN32) || defined(_WIN64)
    assert(_dup2(c->saved, _fileno(stderr)) == 0);
    _close(c->saved);
#else
    assert(dup2(c->saved, fileno(stderr)) >= 0);
    close(c->saved);
#endif
    f = nk_fopen_utf8(c->path, "rb");
    assert(f != NULL);
    got = fread(log, 1, sizeof(log) - 1, f);
    fclose(f);
    log[got] = '\0';
    return strstr(log, line) != NULL;
}

/* Every permission taken from a file and given back later, both through one
   handle or descriptor opened once, so both changes reach the file the test
   then opens by name. Windows: a security descriptor that grants no one
   anything, then none at all (everyone may again); elsewhere mode 0, then
   0600. */
typedef struct {
#if defined(_WIN32) || defined(_WIN64)
    HANDLE handle;
#else
    int fd;
#endif
} NoAccess;

static void no_access_begin(NoAccess *n, const char *path) {
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wide[NATIVE_TEST_PATH_MAX];
    SECURITY_DESCRIPTOR descriptor;
    ACL nobody;
    assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide,
                               NATIVE_TEST_PATH_MAX) > 0);
    /* Its security only: no read or write of the data, every sharing allowed, so
       the opens under test see only the permissions. */
    n->handle = CreateFileW(wide, READ_CONTROL | WRITE_DAC,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(n->handle != INVALID_HANDLE_VALUE);
    assert(InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION));
    assert(InitializeAcl(&nobody, sizeof(nobody), ACL_REVISION));
    assert(SetSecurityDescriptorDacl(&descriptor, TRUE, &nobody, FALSE));
    assert(SetKernelObjectSecurity(n->handle, DACL_SECURITY_INFORMATION, &descriptor));
#else
    n->fd = open(path, O_RDONLY | O_NOFOLLOW);
    assert(n->fd >= 0);
    assert(fchmod(n->fd, 0) == 0);
#endif
}

/* Whether this account is now refused a read of the file: false for a user no
   permission stops (root, or a Windows token using its backup privilege). */
static bool no_access_refused(const char *path) {
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wide[NATIVE_TEST_PATH_MAX];
    HANDLE probe;
    bool refused;
    assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide,
                               NATIVE_TEST_PATH_MAX) > 0);
    probe = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    refused = probe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED;
    if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);
    return refused;
#else
    FILE *probe = nk_fopen_utf8(path, "rb");
    if (probe == NULL) return true;
    fclose(probe);
    return false;
#endif
}

static void no_access_end(NoAccess *n) {
#if defined(_WIN32) || defined(_WIN64)
    SECURITY_DESCRIPTOR descriptor;
    assert(InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION));
    assert(SetSecurityDescriptorDacl(&descriptor, TRUE, NULL, FALSE));
    assert(SetKernelObjectSecurity(n->handle, DACL_SECURITY_INFORMATION, &descriptor));
    assert(CloseHandle(n->handle));
#else
    assert(fchmod(n->fd, 0600) == 0);
    assert(close(n->fd) == 0);
#endif
}

#define SHARED_READ_LOG                                                                   \
    "[DISC_IMPORT] The disc image is open in another program, so it is read twice to " \
    "check that it does not change while it is added."

/* Nothing of an import of `identity` under root: no ref, no staging image,
   and no per-title image named for it. */
static bool nothing_recorded(const char *root, const char *identity) {
    char name[NK_DISC_IDENTITY_CHARS + 32];
    char path[NATIVE_TEST_PATH_MAX + 192];
    snprintf(name, sizeof(name), "%s.ref", identity);
    expanded_path(path, sizeof(path), root, name);
    if (!nk_platform_path_missing(path)) return false;
    snprintf(name, sizeof(name), "%s.iso", identity);
    expanded_path(path, sizeof(path), root, name);
    if (!nk_platform_path_missing(path)) return false;
    snprintf(name, sizeof(name), "TEST00042.%.16s.cso.iso", identity);
    title_path(path, sizeof(path), root, name);
    if (!nk_platform_path_missing(path)) return false;
    snprintf(name, sizeof(name), "TEST00042.%s.cso.iso", identity);
    title_path(path, sizeof(path), root, name);
    return nk_platform_path_missing(path);
}

/* The bytes recorded are the bytes expanded (review of F-f6233fe0, finding 1):
   a source rewritten in place after its identity was read, with the same size
   and write time (a copy that keeps the time, on any file system), or whose
   write time alone changes, is refused before anything is recorded, both after
   an expansion and after the comparison that upgrades a sampled record. On
   Windows the import holds the file with writes denied, so the rewrite itself
   is refused and the import records the bytes it read. */
static void test_import_source_changed(void) {
    TwinImages t;
    char root[NATIVE_TEST_PATH_MAX];
    char legacy_iso[NK_MAX_PATH + 64];
    char legacy_sidecar[NK_MAX_PATH + 80];
    char sample[NK_DISC_LEGACY_FINGERPRINT_CHARS + 1];
    char first_identity[NK_DISC_IDENTITY_CHARS + 1];
    uint64_t s;
    uint64_t e = ISO_BYTES;
    NkDiscImport result;
    NkDiscExpandStatus status;
    Meddle meddle;
    NkDiscExpandCallbacks meddling = { on_progress_meddle, on_cancel_meddle, &meddle };
    bool ok;
    make_twins(&t, "changed");
    s = t.len;
    CHECK(nk_disc_file_identity(t.first, NULL, first_identity, NULL, &status));

    /* Rewritten (same size, same write time) once the expansion began. */
    path_in(root, sizeof(root), "changed-expand-root");
    CHECK(nk_platform_mkdir_p(root));
    memset(&meddle, 0, sizeof(meddle));
    meddle.path = t.first;
    meddle.kind = MEDDLE_REWRITE;
    meddle.after = s;
    meddle.bytes = t.second_bytes;
    meddle.len = t.len;
    ok = nk_disc_import(t.first, root, &meddling, &result, &status);
    expect_meddle_refused("rewritten during the expansion", &meddle, ok, &status,
                          NK_DISC_READ_FAILED);
    if (host_denies_writers()) {
        CHECK(ok && file_equals(result.iso_path, g_iso, ISO_BYTES));
    } else {
        CHECK(nothing_recorded(root, first_identity));
        write_bytes(t.first, t.first_bytes, t.len);
        stamp_write_time(t.first);
    }

    /* Rewritten after the expansion ended, before anything is recorded: the
       second identity is what sees it. */
    path_in(root, sizeof(root), "changed-record-root");
    CHECK(nk_platform_mkdir_p(root));
    memset(&meddle, 0, sizeof(meddle));
    meddle.path = t.first;
    meddle.kind = MEDDLE_REWRITE;
    meddle.after = s + e - 1u;
    meddle.bytes = t.second_bytes;
    meddle.len = t.len;
    ok = nk_disc_import(t.first, root, &meddling, &result, &status);
    expect_meddle_refused("rewritten before the record", &meddle, ok, &status,
                          NK_DISC_READ_FAILED);
    if (!host_denies_writers()) {
        CHECK(nothing_recorded(root, first_identity));
        CHECK(strstr(status.message, "changed while Nakagawa was adding it") != NULL);
        /* A later add of the first file's bytes is a fresh import of them. */
        write_bytes(t.first, t.first_bytes, t.len);
        stamp_write_time(t.first);
        CHECK(nk_disc_import(t.first, root, NULL, &result, &status) && !result.reused);
        CHECK(file_equals(result.iso_path, g_iso, ISO_BYTES));
    }

    /* Only the write time changes during the expansion: the stamp sees it. */
    path_in(root, sizeof(root), "changed-touch-root");
    CHECK(nk_platform_mkdir_p(root));
    memset(&meddle, 0, sizeof(meddle));
    meddle.path = t.first;
    meddle.kind = MEDDLE_TOUCH;
    meddle.after = s;
    meddle.bytes = t.first_bytes;
    meddle.len = t.len;
    ok = nk_disc_import(t.first, root, &meddling, &result, &status);
    expect_meddle_refused("touched during the expansion", &meddle, ok, &status,
                          NK_DISC_READ_FAILED);
    if (!host_denies_writers()) {
        CHECK(nothing_recorded(root, first_identity));
        CHECK(strstr(status.message, "changed while Nakagawa was adding it") != NULL);
    }
    stamp_write_time(t.first);

    /* The same after the comparison that would upgrade a sampled record: the
       record stays sampled, no identity ref is written. */
    path_in(root, sizeof(root), "changed-upgrade-root");
    CHECK(nk_platform_mkdir_p(root));
    CHECK(nk_disc_import(t.first, root, NULL, &result, &status) && !result.reused);
    make_sampled_record(root, t.first, &result, false, legacy_iso, sizeof(legacy_iso));
    snprintf(legacy_sidecar, sizeof(legacy_sidecar), "%s.source", legacy_iso);
    CHECK(nk_disc_legacy_fingerprint(t.first, NK_DISC_FORMAT_CSO, sample, &status));
    memset(&meddle, 0, sizeof(meddle));
    meddle.path = t.first;
    meddle.kind = MEDDLE_REWRITE;
    meddle.after = s + e - 1u;
    meddle.bytes = t.second_bytes;
    meddle.len = t.len;
    ok = nk_disc_import(t.first, root, &meddling, &result, &status);
    expect_meddle_refused("rewritten after the comparison", &meddle, ok, &status,
                          NK_DISC_READ_FAILED);
    if (host_denies_writers()) {
        CHECK(ok && result.reused && strcmp(result.iso_path, legacy_iso) == 0);
    } else {
        char ref_name[NK_DISC_IDENTITY_CHARS + 8];
        char ref[NATIVE_TEST_PATH_MAX + 96];
        CHECK(line_equals(legacy_sidecar, sample));
        snprintf(ref_name, sizeof(ref_name), "%s.ref", first_identity);
        expanded_path(ref, sizeof(ref), root, ref_name);
        CHECK(nk_platform_path_missing(ref));
        CHECK(file_equals(legacy_iso, g_iso, ISO_BYTES));
    }
    free_twins(&t);
    printf("PASS a source changed during its import is never recorded\n");
}

/* Two changes made during an import, each undone before the second identity:
   RESTORE rewrites the source to other bytes once the expansion began and
   writes the first bytes back when it ends, the write time put back both
   times; RENAME moves the source away and another file of the same size and
   write time into its place once the first identity is read, and moves both
   back when the expansion ends. The second identity then reads the first
   file's bytes, so only the stamp's change time and file identity can see
   that the expansion read something else (review round 2 of F-f6233fe0). */
typedef enum { UNDONE_RESTORE, UNDONE_RENAME } UndoneKind;

typedef struct {
    UndoneKind kind;
    const char *path;
    const char *other;    /* RENAME: the file moved into place */
    const char *away;     /* RENAME: where the source waits */
    const uint8_t *first; /* RESTORE: the source's bytes */
    const uint8_t *second;
    size_t len;
    uint64_t change_after;
    uint64_t undo_after;
    int stage;
    bool refused;
#if defined(_WIN32) || defined(_WIN64)
    HANDLE writer; /* RESTORE: another program's handle with write access */
#endif
} Undone;

static void undone_write(Undone *u, const uint8_t *bytes) {
#if defined(_WIN32) || defined(_WIN64)
    FILETIME stamp;
    DWORD wrote = 0;
    LARGE_INTEGER at;
    stamp.dwLowDateTime = 0x69050000u;
    stamp.dwHighDateTime = 0x01D5C036u;
    at.QuadPart = 0;
    assert(SetFilePointerEx(u->writer, at, NULL, FILE_BEGIN));
    assert(WriteFile(u->writer, bytes, (DWORD)u->len, &wrote, NULL) && wrote == u->len);
    assert(FlushFileBuffers(u->writer));
    assert(SetFileTime(u->writer, NULL, NULL, &stamp));
#else
    write_bytes(u->path, bytes, u->len);
    stamp_write_time(u->path);
#endif
}

static void on_progress_undone(uint64_t done, uint64_t total, void *user) {
    Undone *u = (Undone *)user;
    (void)total;
    if (u->stage == 0 && done > u->change_after) {
        u->stage = 1;
        if (u->kind == UNDONE_RESTORE) {
            undone_write(u, u->second);
        } else if (nk_rename_utf8(u->path, u->away) != 0) {
            u->refused = true; /* Windows: the import holds the file */
        } else {
            CHECK(nk_rename_utf8(u->other, u->path) == 0);
        }
    } else if (u->stage == 1 && done > u->undo_after) {
        u->stage = 2;
        if (u->kind == UNDONE_RESTORE) {
            undone_write(u, u->first);
        } else if (!u->refused) {
            CHECK(nk_rename_utf8(u->path, u->other) == 0);
            CHECK(nk_rename_utf8(u->away, u->path) == 0);
        }
    }
}

static void test_import_change_undone(void) {
    TwinImages t;
    char root[NATIVE_TEST_PATH_MAX];
    char other[NATIVE_TEST_PATH_MAX];
    char away[NATIVE_TEST_PATH_MAX];
    char identity[NK_DISC_IDENTITY_CHARS + 1];
    NkDiscImport result;
    NkDiscExpandStatus status;
    Undone undone;
    NkDiscExpandCallbacks hooks = { on_progress_undone, NULL, &undone };
    bool ok;
    make_twins(&t, "undone");
    CHECK(nk_disc_file_identity(t.first, NULL, identity, NULL, &status));

    /* Rewritten and written back, the write time put back each time. Windows
       denies writers while the import holds the file, so the test plays a
       program that already had it open for writing: the import reads it
       shared, and that program's writes are what is checked. */
    path_in(root, sizeof(root), "undone-restore-root");
    CHECK(nk_platform_mkdir_p(root));
    memset(&undone, 0, sizeof(undone));
    undone.kind = UNDONE_RESTORE;
    undone.path = t.first;
    undone.first = t.first_bytes;
    undone.second = t.second_bytes;
    undone.len = t.len;
    undone.change_after = t.len;
    undone.undo_after = t.len + ISO_BYTES - 1u;
#if defined(_WIN32) || defined(_WIN64)
    {
        WCHAR wide[NATIVE_TEST_PATH_MAX];
        assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, t.first, -1, wide,
                                   NATIVE_TEST_PATH_MAX) > 0);
        undone.writer = CreateFileW(wide, GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        assert(undone.writer != INVALID_HANDLE_VALUE);
    }
#endif
    ok = nk_disc_import(t.first, root, &hooks, &result, &status);
#if defined(_WIN32) || defined(_WIN64)
    CloseHandle(undone.writer);
#endif
    if (undone.stage != 2 || ok || strcmp(status.code, NK_DISC_READ_FAILED) != 0) {
        fprintf(stderr, "rewritten and written back: stage=%d ok=%d %s: %s\n", undone.stage,
                (int)ok, status.code, status.message);
        CHECK(false);
    }
    CHECK(strstr(status.message, "changed while Nakagawa was adding it") != NULL);
    CHECK(nothing_recorded(root, identity));
    CHECK(file_equals(t.first, t.first_bytes, t.len));

    /* Moved away and back by rename, another file in its place meanwhile. */
    path_in(root, sizeof(root), "undone-rename-root");
    CHECK(nk_platform_mkdir_p(root));
    path_in(other, sizeof(other), "undone-other.cso");
    path_in(away, sizeof(away), "undone-away.cso");
    write_bytes(other, t.second_bytes, t.len);
    stamp_write_time(other);
    memset(&undone, 0, sizeof(undone));
    undone.kind = UNDONE_RENAME;
    undone.path = t.first;
    undone.other = other;
    undone.away = away;
    undone.change_after = t.len - 1u;
    undone.undo_after = t.len + ISO_BYTES - 1u;
    ok = nk_disc_import(t.first, root, &hooks, &result, &status);
    /* Refused at once (the stamp right after the first identity): put the
       files back as the second step would have. */
    if (undone.stage == 1 && !undone.refused) {
        CHECK(nk_rename_utf8(t.first, other) == 0);
        CHECK(nk_rename_utf8(away, t.first) == 0);
    }
    CHECK(undone.stage >= 1);
    if (host_denies_writers()) {
        /* Held: the source cannot be moved away while it is added. */
        CHECK(undone.refused && ok && file_equals(result.iso_path, g_iso, ISO_BYTES));
    } else if (ok || strcmp(status.code, NK_DISC_READ_FAILED) != 0) {
        fprintf(stderr, "moved away and back: ok=%d %s: %s\n", (int)ok, status.code,
                status.message);
        CHECK(false);
    } else {
        CHECK(nothing_recorded(root, identity));
        CHECK(file_equals(t.first, t.first_bytes, t.len));
    }
    free_twins(&t);
    printf("PASS a change undone before the second identity is still seen\n");
}

#if defined(_WIN32) || defined(_WIN64)
/* A file another program has open for writing (a seeding or syncing program)
   is added, read shared and checked twice: unchanged, it is imported; written
   during the import, it is refused and nothing is recorded. */
static void test_import_open_for_writing_elsewhere(void) {
    TwinImages t;
    char root[NATIVE_TEST_PATH_MAX];
    char identity[NK_DISC_IDENTITY_CHARS + 1];
    WCHAR wide[NATIVE_TEST_PATH_MAX];
    NkDiscImport result;
    NkDiscExpandStatus status;
    Undone undone;
    NkDiscExpandCallbacks hooks = { on_progress_undone, NULL, &undone };
    bool ok;
    make_twins(&t, "in-use");
    CHECK(nk_disc_file_identity(t.first, NULL, identity, NULL, &status));
    assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, t.first, -1, wide,
                               NATIVE_TEST_PATH_MAX) > 0);
    memset(&undone, 0, sizeof(undone));
    undone.writer = CreateFileW(wide, GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(undone.writer != INVALID_HANDLE_VALUE);

    /* Open for writing elsewhere, not written: imported, and the log says it
       was read shared. */
    path_in(root, sizeof(root), "in-use-root");
    CHECK(nk_platform_mkdir_p(root));
    {
        StderrCapture capture;
        bool logged;
        stderr_capture_begin(&capture, "in-use-stderr.log");
        ok = nk_disc_import(t.first, root, NULL, &result, &status);
        logged = stderr_capture_end(&capture, SHARED_READ_LOG);
        if (!logged) fprintf(stderr, "in use: the shared read was not logged\n");
        CHECK(logged);
    }
    if (!ok) fprintf(stderr, "in use: %s: %s\n", status.code, status.message);
    CHECK(ok && !result.reused && file_equals(result.iso_path, g_iso, ISO_BYTES));
    CHECK(strcmp(result.identity, identity) == 0);

    /* Written through that program's handle during the expansion: refused. */
    path_in(root, sizeof(root), "in-use-written-root");
    CHECK(nk_platform_mkdir_p(root));
    undone.kind = UNDONE_RESTORE;
    undone.path = t.first;
    undone.first = t.second_bytes; /* left as written */
    undone.second = t.second_bytes;
    undone.len = t.len;
    undone.change_after = t.len;
    undone.undo_after = UINT64_MAX;
    ok = nk_disc_import(t.first, root, &hooks, &result, &status);
    CloseHandle(undone.writer);
    CHECK(undone.stage == 1);
    if (ok || strcmp(status.code, NK_DISC_READ_FAILED) != 0) {
        fprintf(stderr, "in use, written: ok=%d %s: %s\n", (int)ok, status.code, status.message);
        CHECK(false);
    }
    CHECK(nothing_recorded(root, identity));
    free_twins(&t);
    printf("PASS a file open for writing elsewhere is read shared and checked twice\n");
}
#endif

/* Cancel while a sampled record is compared: nothing is written, the record
   stays as it was. */
static void test_import_cancel_during_comparison(void) {
    TwinImages t;
    char root[NATIVE_TEST_PATH_MAX];
    char legacy_iso[NK_MAX_PATH + 64];
    char legacy_sidecar[NK_MAX_PATH + 80];
    char sample[NK_DISC_LEGACY_FINGERPRINT_CHARS + 1];
    NkDiscImport result;
    NkDiscExpandStatus status;
    Meddle meddle;
    NkDiscExpandCallbacks meddling = { on_progress_meddle, on_cancel_meddle, &meddle };
    make_twins(&t, "compare-cancel");
    path_in(root, sizeof(root), "compare-cancel-root");
    CHECK(nk_platform_mkdir_p(root));
    CHECK(nk_disc_import(t.first, root, NULL, &result, &status) && !result.reused);
    make_sampled_record(root, t.first, &result, true, legacy_iso, sizeof(legacy_iso));
    snprintf(legacy_sidecar, sizeof(legacy_sidecar), "%s.source", legacy_iso);
    CHECK(nk_disc_legacy_fingerprint(t.first, NK_DISC_FORMAT_CSO, sample, &status));
    memset(&meddle, 0, sizeof(meddle));
    meddle.path = t.first;
    meddle.kind = MEDDLE_CANCEL;
    meddle.after = t.len;
    CHECK(!nk_disc_import(t.first, root, &meddling, &result, &status));
    CHECK(strcmp(status.code, NK_DISC_CANCELLED) == 0);
    CHECK(strstr(status.message, "before anything was written") != NULL);
    CHECK(line_equals(legacy_sidecar, sample));
    CHECK(file_equals(legacy_iso, g_iso, ISO_BYTES));
    CHECK(nothing_recorded(root, result.identity));
    free_twins(&t);
    printf("PASS a cancel during the comparison writes nothing\n");
}

/* A sampled record whose image is short, or cannot be read, is not shown to
   be the file's expansion: the file gets its own expansion and the record is
   left as it was (review of F-f6233fe0, finding 5). */
static void test_import_stored_image_short_or_unreadable(void) {
    TwinImages t;
    char root[NATIVE_TEST_PATH_MAX];
    char legacy_iso[NK_MAX_PATH + 64];
    char legacy_sidecar[NK_MAX_PATH + 80];
    char sample[NK_DISC_LEGACY_FINGERPRINT_CHARS + 1];
    NkDiscImport result;
    NkDiscExpandStatus status;
    make_twins(&t, "stored-bad");
    CHECK(nk_disc_legacy_fingerprint(t.first, NK_DISC_FORMAT_CSO, sample, &status));

    /* Short: a stored image cut by 2 KiB. */
    path_in(root, sizeof(root), "stored-short-root");
    CHECK(nk_platform_mkdir_p(root));
    CHECK(nk_disc_import(t.first, root, NULL, &result, &status) && !result.reused);
    make_sampled_record(root, t.first, &result, true, legacy_iso, sizeof(legacy_iso));
    snprintf(legacy_sidecar, sizeof(legacy_sidecar), "%s.source", legacy_iso);
    write_bytes(legacy_iso, g_iso, ISO_BYTES - SECTOR);
    CHECK(nk_disc_import(t.first, root, NULL, &result, &status) && !result.reused);
    CHECK(strcmp(result.iso_path, legacy_iso) != 0);
    CHECK(file_equals(result.iso_path, g_iso, ISO_BYTES));
    CHECK(line_equals(legacy_sidecar, sample));

    /* Unreadable: Windows, held open by another program with no sharing;
       elsewhere, no read permission (skipped where the test runs as a user no
       permission stops). */
    path_in(root, sizeof(root), "stored-unreadable-root");
    CHECK(nk_platform_mkdir_p(root));
    CHECK(nk_disc_import(t.first, root, NULL, &result, &status) && !result.reused);
    make_sampled_record(root, t.first, &result, true, legacy_iso, sizeof(legacy_iso));
    snprintf(legacy_sidecar, sizeof(legacy_sidecar), "%s.source", legacy_iso);
    {
        bool unreadable;
#if defined(_WIN32) || defined(_WIN64)
        WCHAR wide[NATIVE_TEST_PATH_MAX];
        HANDLE held;
        assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, legacy_iso, -1, wide,
                                   NATIVE_TEST_PATH_MAX) > 0);
        held = CreateFileW(wide, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           NULL);
        assert(held != INVALID_HANDLE_VALUE);
        unreadable = true;
#else
        NoAccess no_access;
        no_access_begin(&no_access, legacy_iso);
        unreadable = no_access_refused(legacy_iso);
#endif
        if (unreadable) {
            bool ok = nk_disc_import(t.first, root, NULL, &result, &status);
            if (!ok) fprintf(stderr, "unreadable stored image: %s: %s\n", status.code,
                             status.message);
            CHECK(ok && !result.reused && strcmp(result.iso_path, legacy_iso) != 0);
            CHECK(file_equals(result.iso_path, g_iso, ISO_BYTES));
            CHECK(line_equals(legacy_sidecar, sample));
        } else {
            printf("SKIP unreadable stored image: this user reads a file with no permission\n");
        }
#if defined(_WIN32) || defined(_WIN64)
        CloseHandle(held);
#else
        no_access_end(&no_access);
#endif
    }
    free_twins(&t);
    printf("PASS a short or unreadable stored image gets a fresh expansion\n");
}

/* A source this account may not read: the refusal says access was denied, not
   that the drive may be gone (NoAccess: no permission at all; skipped where the
   test runs as a user no permission stops). An ordinary
   import, held with writes denied, logs no shared read. */
static void test_import_access_denied(void) {
    TwinImages t;
    char root[NATIVE_TEST_PATH_MAX];
    NkDiscImport result;
    NkDiscExpandStatus status;
    NoAccess no_access;
    bool denied;
    bool ok;
    make_twins(&t, "denied");
    path_in(root, sizeof(root), "denied-root");
    CHECK(nk_platform_mkdir_p(root));
    {
        StderrCapture capture;
        bool logged;
        stderr_capture_begin(&capture, "held-stderr.log");
        ok = nk_disc_import(t.second, root, NULL, &result, &status);
        logged = stderr_capture_end(&capture, SHARED_READ_LOG);
        if (logged) fprintf(stderr, "held: a shared read was logged for a file nobody writes\n");
        CHECK(ok && !logged);
    }
    no_access_begin(&no_access, t.first);
    denied = no_access_refused(t.first);
    if (denied) {
        ok = nk_disc_import(t.first, root, NULL, &result, &status);
        if (ok || strcmp(status.code, NK_DISC_READ_FAILED) != 0 ||
            strstr(status.message, "Access to the disc image was denied") == NULL) {
            fprintf(stderr, "access denied: ok=%d %s: %s\n", (int)ok, status.code,
                    status.message);
            CHECK(false);
        }
    } else {
        printf("SKIP access denied: this user reads a file it has no permission for\n");
    }
    no_access_end(&no_access);
    free_twins(&t);
    printf("PASS a source this account may not read is refused as access denied\n");
}

/* A stamp field read the first time and 0 the next (or the other way round)
   is a change; a field 0 both times is one the file system does not keep. */
static void test_source_stamp_field_lost(void) {
    TwinImages t;
    NkDiscSourceStamp stamp;
    NkDiscSourceStamp lost;
    NkDiscExpandStatus status;
    int fields = 0;
    make_twins(&t, "stamp-lost");
    CHECK(nk_disc_source_stamp(t.first, &stamp));
    CHECK(nk_disc_source_unchanged(t.first, &stamp, &status));
    if (stamp.changed != 0) {
        lost = stamp;
        lost.changed = 0;
        CHECK(!nk_disc_source_unchanged(t.first, &lost, &status));
        CHECK(strcmp(status.code, NK_DISC_READ_FAILED) == 0);
        fields++;
    }
    if (stamp.volume != 0) {
        lost = stamp;
        lost.volume = 0;
        CHECK(!nk_disc_source_unchanged(t.first, &lost, &status));
        fields++;
    }
    if (stamp.file_id != 0) {
        lost = stamp;
        lost.file_id = 0;
        if (nk_disc_source_unchanged(t.first, &lost, &status)) {
            fprintf(stderr, "stamp: a file identity read once and lost the next time "
                            "was taken as unchanged\n");
            CHECK(false);
        }
        fields++;
    }
    free_twins(&t);
    if (fields == 0) {
        printf("SKIP stamp field lost: this file system reports no change time or file identity\n");
    } else {
        printf("PASS a stamp field read once and not again is a change (%d fields)\n", fields);
    }
}

/* Records that are not whole are never a match: an upgrade interrupted
   between its ref and its .source, a malformed .source or ref, and a ref that
   names an image recorded for another identity. */
static void test_import_damaged_records(void) {
    TwinImages t;
    char root[NATIVE_TEST_PATH_MAX];
    char legacy_iso[NK_MAX_PATH + 64];
    char legacy_sidecar[NK_MAX_PATH + 80];
    char sample[NK_DISC_LEGACY_FINGERPRINT_CHARS + 1];
    char name[NK_DISC_IDENTITY_CHARS + 64];
    char ref[NATIVE_TEST_PATH_MAX + 192];
    char sidecar[NK_MAX_PATH + 16];
    char first_iso[NK_MAX_PATH];
    char second_iso[NK_MAX_PATH];
    char first_identity[NK_DISC_IDENTITY_CHARS + 1];
    char expected[NK_DISC_IDENTITY_CHARS + 16];
    const char *base;
    NkDiscImport result;
    NkDiscExpandStatus status;
    make_twins(&t, "damaged");
    CHECK(nk_disc_legacy_fingerprint(t.first, NK_DISC_FORMAT_CSO, sample, &status));
    CHECK(nk_disc_file_identity(t.first, NULL, first_identity, NULL, &status));

    /* Interrupted upgrade: the identity's ref written, the .source still the
       sampled one. The other file with the same sample is not matched; the
       record's own file is compared and the upgrade finished. */
    path_in(root, sizeof(root), "damaged-upgrade-root");
    CHECK(nk_platform_mkdir_p(root));
    CHECK(nk_disc_import(t.first, root, NULL, &result, &status) && !result.reused);
    make_sampled_record(root, t.first, &result, false, legacy_iso, sizeof(legacy_iso));
    snprintf(legacy_sidecar, sizeof(legacy_sidecar), "%s.source", legacy_iso);
    base = strrchr(legacy_iso, nk_platform_path_separator()) + 1;
    snprintf(name, sizeof(name), "%s.ref", first_identity);
    expanded_path(ref, sizeof(ref), root, name);
    snprintf(expected, sizeof(expected), "TEST00042 %s", base);
    write_line(ref, expected);
    CHECK(nk_disc_import(t.second, root, NULL, &result, &status) && !result.reused);
    CHECK(strcmp(result.iso_path, legacy_iso) != 0);
    CHECK(file_equals(result.iso_path, g_late_iso, ISO_BYTES));
    CHECK(line_equals(legacy_sidecar, sample));
    CHECK(nk_disc_import(t.first, root, NULL, &result, &status) && result.reused);
    CHECK(strcmp(result.iso_path, legacy_iso) == 0);
    snprintf(expected, sizeof(expected), "sha256:%s", first_identity);
    CHECK(line_equals(legacy_sidecar, expected));

    /* A malformed sha256: .source: not this file's record. The image under
       that name is left alone, as one whose owner is not known; the file gets
       the name of its whole identity. */
    path_in(root, sizeof(root), "damaged-source-root");
    CHECK(nk_platform_mkdir_p(root));
    CHECK(nk_disc_import(t.first, root, NULL, &result, &status) && !result.reused);
    snprintf(first_iso, sizeof(first_iso), "%s", result.iso_path);
    snprintf(sidecar, sizeof(sidecar), "%s.source", first_iso);
    write_line(sidecar, "sha256:0123");
    CHECK(nk_disc_import(t.first, root, NULL, &result, &status) && !result.reused);
    snprintf(name, sizeof(name), "TEST00042.%s.cso.iso", first_identity);
    CHECK(strstr(result.iso_path, name) != NULL);
    CHECK(file_equals(result.iso_path, g_iso, ISO_BYTES));
    CHECK(line_equals(sidecar, "sha256:0123"));
    CHECK(nk_disc_import(t.first, root, NULL, &result, &status) && result.reused);
    CHECK(strstr(result.iso_path, name) != NULL);

    /* A malformed ref: not a match; the file's own image under its own name
       is replaced by a fresh expansion of the same bytes. */
    path_in(root, sizeof(root), "damaged-ref-root");
    CHECK(nk_platform_mkdir_p(root));
    CHECK(nk_disc_import(t.first, root, NULL, &result, &status) && !result.reused);
    snprintf(first_iso, sizeof(first_iso), "%s", result.iso_path);
    snprintf(name, sizeof(name), "%s.ref", first_identity);
    expanded_path(ref, sizeof(ref), root, name);
    write_line(ref, "TEST00042 ../../elsewhere.iso");
    CHECK(nk_disc_import(t.first, root, NULL, &result, &status) && !result.reused);
    CHECK(strcmp(result.iso_path, first_iso) == 0);
    CHECK(file_equals(first_iso, g_iso, ISO_BYTES));
    CHECK(nk_disc_import(t.first, root, NULL, &result, &status) && result.reused);

    /* A ref naming the other file's image (recorded for its identity): not a
       match, and that image is left as it is. */
    path_in(root, sizeof(root), "damaged-other-root");
    CHECK(nk_platform_mkdir_p(root));
    CHECK(nk_disc_import(t.first, root, NULL, &result, &status) && !result.reused);
    snprintf(first_iso, sizeof(first_iso), "%s", result.iso_path);
    CHECK(nk_disc_import(t.second, root, NULL, &result, &status) && !result.reused);
    snprintf(second_iso, sizeof(second_iso), "%s", result.iso_path);
    base = strrchr(second_iso, nk_platform_path_separator()) + 1;
    snprintf(name, sizeof(name), "%s.ref", first_identity);
    expanded_path(ref, sizeof(ref), root, name);
    snprintf(expected, sizeof(expected), "TEST00042 %s", base);
    write_line(ref, expected);
    CHECK(nk_disc_import(t.first, root, NULL, &result, &status) && !result.reused);
    CHECK(strcmp(result.iso_path, first_iso) == 0);
    CHECK(file_equals(first_iso, g_iso, ISO_BYTES));
    CHECK(file_equals(second_iso, g_late_iso, ISO_BYTES));
    free_twins(&t);
    printf("PASS damaged records are never a match\n");
}

/* An image under the short per-title name that is recorded for another
   identity (the first 16 hex digits of two identities alike, which takes a
   crafted file) is never overwritten: the newcomer takes the name of its whole
   identity (review of F-f6233fe0, finding 3). While that name is being placed
   its lock is held, so another import that would take it is refused as busy;
   and a record that cannot be written after the move removes the image again
   and says so (finding 4). */
static void test_import_name_collision(void) {
    TwinImages t;
    char root[NATIVE_TEST_PATH_MAX];
    char identity[NK_DISC_IDENTITY_CHARS + 1];
    char other[NK_DISC_IDENTITY_CHARS + 1];
    char name[NK_DISC_IDENTITY_CHARS + 64];
    char short_iso[NATIVE_TEST_PATH_MAX + 192];
    char short_sidecar[NATIVE_TEST_PATH_MAX + 200];
    char full_iso[NATIVE_TEST_PATH_MAX + 192];
    char path[NATIVE_TEST_PATH_MAX + 192];
    char line[NK_DISC_IDENTITY_CHARS + 16];
    NkDiscImport result;
    NkDiscExpandStatus status;
    NkFileLock held;
    make_twins(&t, "collide");
    CHECK(nk_disc_file_identity(t.first, NULL, identity, NULL, &status));
    /* Another identity with the same first 16 hex digits. */
    snprintf(other, sizeof(other), "%.16s%s", identity,
             "000000000000000000000000000000000000000000000000");
    CHECK(strcmp(other, identity) != 0 && strlen(other) == NK_DISC_IDENTITY_CHARS);

    /* An earlier import of that other identity sits under the short name. */
    path_in(root, sizeof(root), "collide-root");
    CHECK(nk_platform_mkdir_p(root));
    snprintf(name, sizeof(name), "TEST00042.%.16s.cso.iso", identity);
    title_path(short_iso, sizeof(short_iso), root, name);
    title_path(path, sizeof(path), root, "");
    CHECK(nk_platform_mkdir_p(path));
    write_bytes(short_iso, g_late_iso, ISO_BYTES);
    snprintf(short_sidecar, sizeof(short_sidecar), "%s.source", short_iso);
    snprintf(line, sizeof(line), "sha256:%s", other);
    write_line(short_sidecar, line);
    snprintf(name, sizeof(name), "TEST00042.%s.cso.iso", identity);
    title_path(full_iso, sizeof(full_iso), root, name);

    if (!nk_disc_import(t.first, root, NULL, &result, &status)) {
        fprintf(stderr, "collision: %s: %s\n", status.code, status.message);
        CHECK(false);
    }
    CHECK(!result.reused && strcmp(result.iso_path, full_iso) == 0);
    CHECK(file_equals(full_iso, g_iso, ISO_BYTES));
    CHECK(file_equals(short_iso, g_late_iso, ISO_BYTES));
    CHECK(line_equals(short_sidecar, line));
    CHECK(nk_disc_import(t.first, root, NULL, &result, &status) && result.reused);
    CHECK(strcmp(result.iso_path, full_iso) == 0);

    /* The short name's lock held by another import: refused as busy, nothing
       of this one kept. */
    path_in(root, sizeof(root), "collide-busy-root");
    CHECK(nk_platform_mkdir_p(root));
    expanded_path(path, sizeof(path), root, "");
    CHECK(nk_platform_mkdir_p(path));
    snprintf(name, sizeof(name), "TEST00042.%.16s.cso.lock", identity);
    expanded_path(path, sizeof(path), root, name);
    CHECK(nk_platform_file_lock_acquire(&held, path, 0u));
    CHECK(!nk_disc_import(t.first, root, NULL, &result, &status));
    CHECK(strcmp(status.code, NK_DISC_IMPORT_BUSY) == 0);
    CHECK(nothing_recorded(root, identity));
    nk_platform_file_lock_release(&held);
    CHECK(nk_disc_import(t.first, root, NULL, &result, &status) && !result.reused);

    /* The record cannot be written after the move (its ref's name is taken by
       a folder): the image just moved is removed again and the message says
       what failed. */
    path_in(root, sizeof(root), "collide-record-root");
    CHECK(nk_platform_mkdir_p(root));
    snprintf(name, sizeof(name), "%s.ref", identity);
    expanded_path(path, sizeof(path), root, name);
    CHECK(nk_platform_mkdir_p(path));
    CHECK(!nk_disc_import(t.first, root, NULL, &result, &status));
    CHECK(strcmp(status.code, NK_DISC_WRITE_FAILED) == 0);
    CHECK(strstr(status.message, "record could not be written") != NULL);
    snprintf(name, sizeof(name), "TEST00042.%.16s.cso.iso", identity);
    title_path(path, sizeof(path), root, name);
    CHECK(nk_platform_path_missing(path));
    snprintf(name, sizeof(name), "%s.iso", identity);
    expanded_path(path, sizeof(path), root, name);
    CHECK(nk_platform_path_missing(path));
    free_twins(&t);
    printf("PASS an image recorded for another identity is never overwritten\n");
}

typedef struct {
    const char *source;
    const char *root;
    uint64_t source_bytes;
    int nested_calls;
    bool nested_ok;
    char nested_code[48];
} NestedImport;

/* Starts a second import of the same file while the first is part-way
   through writing its partial image, as a second program would: past the
   source's identity (the first source_bytes of the progress). */
static void on_progress_import_again(uint64_t done, uint64_t total, void *user) {
    NestedImport *n = (NestedImport *)user;
    if (done > n->source_bytes && done < total && n->nested_calls == 0) {
        NkDiscImport again;
        NkDiscExpandStatus status;
        n->nested_calls++;
        n->nested_ok = nk_disc_import(n->source, n->root, NULL, &again, &status);
        snprintf(n->nested_code, sizeof(n->nested_code), "%s", status.code);
    }
}

static void test_import_busy(void) {
    Buf image = { NULL, 0, 0 };
    char root[NATIVE_TEST_PATH_MAX];
    char src[NATIVE_TEST_PATH_MAX];
    NkDiscImport result;
    NkDiscExpandStatus status;
    NestedImport nested;
    NkDiscExpandCallbacks callbacks = { on_progress_import_again, NULL, &nested };
    path_in(root, sizeof(root), "busy-root");
    CHECK(nk_platform_mkdir_p(root));
    path_in(src, sizeof(src), "busy.cso");
    build_ciso(&image, "CISO", 1, 0, g_iso, ISO_BYTES, cso_v1_kind);
    write_bytes(src, image.data, image.len);
    memset(&nested, 0, sizeof(nested));
    nested.source = src;
    nested.root = root;
    nested.source_bytes = image.len;
    if (!nk_disc_import(src, root, &callbacks, &result, &status)) {
        fprintf(stderr, "busy: %s: %s\n", status.code, status.message);
        CHECK(false);
    }
    CHECK(nested.nested_calls == 1);
    if (nested.nested_ok || strcmp(nested.nested_code, NK_DISC_IMPORT_BUSY) != 0) {
        fprintf(stderr, "busy: the second import gave ok=%d code=%s\n", (int)nested.nested_ok,
                nested.nested_code);
        CHECK(false);
    }
    CHECK(!result.reused && file_equals(result.iso_path, g_iso, ISO_BYTES));
    /* Once the first is done, the lock is free and the file is reused. */
    CHECK(nk_disc_import(src, root, NULL, &result, &status) && result.reused);
    free(image.data);
    printf("PASS a second import of the same file meanwhile is refused as busy\n");
}

int main(void) {
    char name[64];
    native_test_create_root("disc-expand");
    native_test_isolate_user_data_roots();
    snprintf(name, sizeof(name), "fixtures");
    {
        int n = snprintf(g_dir, sizeof(g_dir), "%s%c%s", native_test_get_root(),
                         nk_platform_path_separator(), name);
        assert(n > 0 && (size_t)n < sizeof(g_dir));
    }
    assert(nk_platform_mkdir_p(g_dir));
    build_iso(g_iso, true);
    build_iso(g_other_iso, false);
    nk_disc_chd_register();

    test_cso_zso_round_trips();
    test_chd_round_trips();
    test_malformed_refused();
    test_chd_huffman_gap_is_deterministic();
    test_chd_flac_empty_hunk_refused();
    test_cancel_removes_partial();
    test_import_and_reuse();
    test_chd_damaged_metadata();
    test_chd_raw_sha1();
    test_chd_header_caps();
    test_chd_unknown_codec();
    test_import_same_disc_id();
    test_import_full_content_identity();
    test_import_upgrades_sampled_record();
    test_import_identity_read_error_and_cancel();
    test_import_source_changed();
    test_import_change_undone();
#if defined(_WIN32) || defined(_WIN64)
    test_import_open_for_writing_elsewhere();
#endif
    test_import_cancel_during_comparison();
    test_import_stored_image_short_or_unreadable();
    test_import_access_denied();
    test_source_stamp_field_lost();
    test_import_damaged_records();
    test_import_name_collision();
    test_import_busy();
    printf("test_disc_expand: %d checks passed\n", g_checks);
    return 0;
}
