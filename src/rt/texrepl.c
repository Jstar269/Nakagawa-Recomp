/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2012- PPSSPP Project */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/* Derived from PPSSPP (GPL-2.0-or-later) at 3bc60270eae3069f13dfbfdb7292a7e1375de6f6; modified by Nakagawa Recomp
 * contributors, 2026-11-06. See NOTICE.md and CREDITS.md for upstream lineage. */
/*
 * texrepl.c - Texture replacement packs in PPSSPP's layout (#1880), phase 1. See texrepl.h.
 *
 * The quick texture hash and the replacement-pack key and lookup order are translated from PPSSPP
 * (GPL-2.0-or-later), GPU/Common/TextureDecoder.cpp (StableQuickTexHash, QuickTexHashNonSSE),
 * GPU/Common/TextureReplacer.cpp (textures.ini, hash ranges, key and wildcard lookup) and
 * GPU/Common/TextureCacheCommon.cpp (the palette hash) at 3bc60270eae3069f13dfbfdb7292a7e1375de6f6.
 * Copyright (c) 2012- PPSSPP Project. Modified for Nakagawa Recomp.
 *
 * The hashes must match PPSSPP's bit for bit, since a pack's file names depend on them; the
 * selftest checks them against fixed quick-hash vectors taken from PPSSPP's own code and published
 * xxHash vectors (XXH32 and XXH64 are the public algorithms). The ini parser, the PNG decoder (written
 * from the PNG specification, its image data unpacked by src/core/nk_psp_inflate.c), the cache and
 * the budget are this project's own. The ini line rules, the texture-name and mip-chain
 * rules and the ZIP pack rules follow the behaviour of PPSSPP's IniFile.cpp and TextureReplacer.cpp
 * at the same revision, read as reference and written here in this project's own code; ZIP packs are
 * read in place through the project's own reader (src/core/nk_zip.h).
 */
#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700   /* realpath, O_CLOEXEC and O_NOCTTY under -std=c99 */
#endif
#include "texrepl.h"

#include "nk_platform.h"
#include "nk_zip.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "nk_psp_inflate.h"

/* ===================================================================================== hashes */

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;   /* the PSP and every host this builds for are little-endian */
}
static uint64_t rd64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}
static uint32_t rotl32(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
static uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

#define P32_1 2654435761u
#define P32_2 2246822519u
#define P32_3 3266489917u
#define P32_4 668265263u
#define P32_5 374761393u

uint32_t nk_texrepl_xxh32(const void *data, size_t len, uint32_t seed) {
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *end = p + len;
    uint32_t h;
    if (len >= 16) {
        uint32_t v1 = seed + P32_1 + P32_2, v2 = seed + P32_2, v3 = seed, v4 = seed - P32_1;
        const uint8_t *limit = end - 16;
        do {
            v1 = rotl32(v1 + rd32(p) * P32_2, 13) * P32_1;
            v2 = rotl32(v2 + rd32(p + 4) * P32_2, 13) * P32_1;
            v3 = rotl32(v3 + rd32(p + 8) * P32_2, 13) * P32_1;
            v4 = rotl32(v4 + rd32(p + 12) * P32_2, 13) * P32_1;
            p += 16;
        } while (p <= limit);
        h = rotl32(v1, 1) + rotl32(v2, 7) + rotl32(v3, 12) + rotl32(v4, 18);
    } else {
        h = seed + P32_5;
    }
    h += (uint32_t)len;
    while (p + 4 <= end) {
        h = rotl32(h + rd32(p) * P32_3, 17) * P32_4;
        p += 4;
    }
    while (p < end) {
        h = rotl32(h + (uint32_t)(*p++) * P32_5, 11) * P32_1;
    }
    h ^= h >> 15;
    h *= P32_2;
    h ^= h >> 13;
    h *= P32_3;
    h ^= h >> 16;
    return h;
}

#define P64_1 11400714785074694791ull
#define P64_2 14029467366897019727ull
#define P64_3 1609587929392839161ull
#define P64_4 9650029242287828579ull
#define P64_5 2870177450012600261ull

static uint64_t xxh64_round(uint64_t acc, uint64_t input) {
    acc += input * P64_2;
    acc = rotl64(acc, 31);
    return acc * P64_1;
}
static uint64_t xxh64_merge(uint64_t acc, uint64_t val) {
    acc ^= xxh64_round(0, val);
    return acc * P64_1 + P64_4;
}

uint64_t nk_texrepl_xxh64(const void *data, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *end = p + len;
    uint64_t h;
    if (len >= 32) {
        uint64_t v1 = seed + P64_1 + P64_2, v2 = seed + P64_2, v3 = seed, v4 = seed - P64_1;
        const uint8_t *limit = end - 32;
        do {
            v1 = xxh64_round(v1, rd64(p));
            v2 = xxh64_round(v2, rd64(p + 8));
            v3 = xxh64_round(v3, rd64(p + 16));
            v4 = xxh64_round(v4, rd64(p + 24));
            p += 32;
        } while (p <= limit);
        h = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        h = xxh64_merge(h, v1);
        h = xxh64_merge(h, v2);
        h = xxh64_merge(h, v3);
        h = xxh64_merge(h, v4);
    } else {
        h = seed + P64_5;
    }
    h += (uint64_t)len;
    while (p + 8 <= end) {
        h ^= xxh64_round(0, rd64(p));
        h = rotl64(h, 27) * P64_1 + P64_4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= (uint64_t)rd32(p) * P64_1;
        h = rotl64(h, 23) * P64_2 + P64_3;
        p += 4;
    }
    while (p < end) {
        h ^= (uint64_t)(*p++) * P64_5;
        h = rotl64(h, 11) * P64_1;
    }
    h ^= h >> 33;
    h *= P64_2;
    h ^= h >> 29;
    h *= P64_3;
    h ^= h >> 32;
    return h;
}

uint32_t nk_texrepl_quick_hash(const void *data, uint32_t size, uint32_t guest_addr) {
    const uint8_t *b = (const uint8_t *)data;
    uint32_t check = 0;
    if ((guest_addr & 0xfu) == 0 && (size & 0x3fu) == 0) {
        /* Eight 16-bit lanes (viewed as four 32-bit ones for the xor and add steps). */
        static const uint16_t kInitial[8] = {0xc00b, 0x9bd9, 0x4b73, 0xb651, 0x4d9b, 0x4309, 0x0083, 0x0001};
        uint16_t cursor16[8] = {0};
        uint16_t cursor2[8];
        memcpy(cursor2, kInitial, sizeof(cursor2));
        for (uint32_t i = 0; i < size / 16u; i += 4) {
            const uint8_t *b0 = b + (size_t)(i + 0) * 16u, *b1 = b + (size_t)(i + 1) * 16u;
            const uint8_t *b2 = b + (size_t)(i + 2) * 16u, *b3 = b + (size_t)(i + 3) * 16u;
            for (int j = 0; j < 8; j++) {
                uint16_t w;
                memcpy(&w, b0 + 2 * j, 2);
                cursor16[j] = (uint16_t)(cursor16[j] + (uint16_t)((uint32_t)w * cursor2[j]));
            }
            for (int j = 0; j < 4; j++) {
                uint32_t c;
                memcpy(&c, (const uint8_t *)cursor16 + 4 * j, 4);
                c ^= rd32(b1 + 4 * j);
                c += rd32(b2 + 4 * j);
                memcpy((uint8_t *)cursor16 + 4 * j, &c, 4);
            }
            for (int j = 0; j < 8; j++) {
                uint16_t w;
                memcpy(&w, b3 + 2 * j, 2);
                cursor16[j] = (uint16_t)(cursor16[j] ^ (uint16_t)((uint32_t)w * cursor2[j]));
            }
            for (int j = 0; j < 8; j++) cursor2[j] = (uint16_t)(cursor2[j] + 0x2455u);
        }
        for (int j = 0; j < 4; j++) {
            uint32_t c, c2;
            memcpy(&c, (const uint8_t *)cursor16 + 4 * j, 4);
            memcpy(&c2, (const uint8_t *)cursor2 + 4 * j, 4);
            c += c2;
            check += c;
        }
    } else {
        for (uint32_t i = 0; i < size / 8u; i++) {
            check += rd32(b + (size_t)i * 8u);
            check ^= rd32(b + (size_t)i * 8u + 4u);
        }
    }
    return check;
}

uint32_t nk_texrepl_clut_hash(const void *clut, uint32_t bytes) {
    return nk_texrepl_xxh32(clut, bytes, 0xC0108888u);
}

uint32_t nk_texrepl_clut_identity(const void *clutram, uint32_t clutram_bytes, uint32_t clut_format,
                                  uint32_t texture_format, uint32_t last_load_bytes,
                                  uint32_t max_load_bytes) {
    const uint32_t entry_bytes = (clut_format & 3u) == 3u ? 4u : 2u;
    const uint32_t start_bytes = (((clut_format >> 16) & 0x1Fu) << 4) * entry_bytes;
    uint32_t bytes;
    if (max_load_bytes) {
        bytes = last_load_bytes + start_bytes;
        if (bytes > max_load_bytes) bytes = max_load_bytes;
    } else {
        bytes = (texture_format == 4u ? 16u : 256u) * entry_bytes;
    }
    if (bytes > clutram_bytes) bytes = clutram_bytes;
#ifdef NK_TEXREPL_MUTANT_NO_CLUTFORMAT_XOR   /* the selftests' mutant: the CLUTFORMAT xor left out */
    return nk_texrepl_clut_hash(clutram, bytes);
#else
    return nk_texrepl_clut_hash(clutram, bytes) ^ (0xC5000000u | (clut_format & 0x00FFFFFFu));
#endif
}

uint64_t nk_texrepl_cache_key(uint32_t addr, uint32_t format, uint32_t dim, uint32_t clut_hash) {
    uint64_t key = ((uint64_t)(addr & 0x3FFFFFFFu) << 32) | (dim & 0xFFFFu);
    if (format & 4u) key ^= clut_hash;
    return key;
}

uint32_t nk_texrepl_bits_per_texel(uint32_t format) {
    static const uint8_t kBits[16] = {16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0};
    return kBits[format & 15u];
}

static uint64_t now_ns(void) {
    return (uint64_t)((double)clock() * (1e9 / (double)CLOCKS_PER_SEC));
}

/* ======================================================================================= pack */

/* One image file the pack names, read, checked, decoded and held once for every level that names it:
 * a file listed under many keys costs one read and one decode, is noted once if it cannot be used, and
 * counts against the budget once. */
typedef struct {
    char *file;            /* under the pack's root, '/' between folders */
    uint32_t *rgba;        /* decoded image while state is 1 */
    int w, h;              /* known while state is 1 or 3 */
    uint8_t state;         /* 0 not read yet, 1 decoded, 2 cannot be used, 3 size known, image not held */
    uint32_t pins;
    uint32_t lru_prev, lru_next; /* the held (state 1) images, most recently used first: 1 + row, 0 none */
    uint32_t decodes;      /* times its pixels were decoded */
    uint32_t wait;         /* lookups that pass before an evicted image is decoded again (see image_load) */
    uint32_t same;         /* 0, or 1 + the row that holds this file's state: the same file under another
                              name (other case, an 8.3 name, a link), found before its bytes are read */
} Image;

/* One mip level of a listed texture. */
typedef struct {
    char *file;            /* the image's name: the level's own copy until images_build, then the image's */
    uint32_t image;        /* its row in the pack's images (images_build) */
    uint8_t state;         /* 0 not checked yet, 1 its image is the size this level needs, 2 cannot be used */
} Level;

/* One texture key and its mip chain. level_n 0: a texture the pack leaves alone. */
typedef struct {
    uint64_t cachekey;
    uint32_t hash;
    uint32_t level_n;
    uint32_t indexed_noted;
    Level *levels;
} Alias;

/* A [hashes] line or a hash-named file, before the lines of one key are put together. */
typedef struct {
    uint64_t cachekey;
    uint32_t hash;
    int level;
    uint32_t order;        /* the folder listing first, then the ini: a later one wins */
    char *file;            /* "" for a blank value */
} Record;

/* An [indexed] line: the texture's image holds palette indices (mode 1, CPU; 2, GPU) or colours (0). */
typedef struct {
    uint64_t cachekey;
    uint32_t hash;
    uint32_t order;
    uint8_t mode;
} IndexedKey;

typedef struct { uint32_t addr, w, h, new_w, new_h, seq; } HashRange;
typedef struct { uint32_t w, h; float value; uint32_t seq; } ReduceRange;

/* A ZIP pack's file, by its name under the pack's root (the reader owns the name). */
typedef struct { const char *rel; size_t index; uint32_t size; uint8_t deflated; uint64_t packed; } ZipName;

struct NkTexRepl {
    char *dir;                 /* a folder pack */
#if defined(_WIN32)
    WCHAR *root_real;          /* the folder's final path, links resolved: files must be inside it */
#else
    char *root_real;
#endif
    NkZipReader *zip;          /* a ZIP pack */
    ZipName *zip_names;
    size_t zip_name_n;
    NkTexReplHash hash_kind;
    int ignore_address, reduce_hash;
    int skip_dxt1_128x64, skip_dxt1_128x128;
    float reduce_global;
    Record *records;
    size_t record_n, record_cap;
    IndexedKey *indexed;
    size_t indexed_n, indexed_cap;
    size_t nonpng_listed;
    Alias *aliases;
    size_t alias_n;
    Image *images;             /* every file the levels name, once (images_build) */
    size_t image_n;
    uint64_t *id_key;          /* files read so far, by identity (ZIP entry, or volume and file ID): */
    uint32_t *id_row;          /* an open-addressed table, ID_WORDS words a key in id_key, 0 an empty slot */
    size_t id_cap, id_n;
    HashRange *ranges;
    size_t range_n, range_cap;
    ReduceRange *reduce;
    size_t reduce_n, reduce_cap;
    char *diag[NK_TEXREPL_MAX_DIAG + 1];
    size_t diag_n, diag_dropped;
    char diag_last[64];
    uint64_t budget, used;
    uint32_t lru_head, lru_tail;   /* the held images' list (Image.lru_prev, lru_next), 1 + row, 0 none */
    NkTexReplStats stats;
    char (*games)[NK_TEXREPL_GAME_ID_MAX];   /* [games] keys: the disc IDs the pack is made for */
    size_t games_n, games_cap;
};

/* The length of the valid UTF-8 sequence at c (RFC 3629: no overlong form, no surrogate, nothing
 * past U+10FFFF), or 0. */
static size_t utf8_valid_at(const unsigned char *c) {
    const unsigned char a = c[0];
    if (a >= 0xc2u && a <= 0xdfu) return (c[1] & 0xc0u) == 0x80u ? 2u : 0u;
    if (a >= 0xe0u && a <= 0xefu) {
        const unsigned char lo = a == 0xe0u ? 0xa0u : 0x80u, hi = a == 0xedu ? 0x9fu : 0xbfu;
        return c[1] >= lo && c[1] <= hi && (c[2] & 0xc0u) == 0x80u ? 3u : 0u;
    }
    if (a >= 0xf0u && a <= 0xf4u) {
        const unsigned char lo = a == 0xf0u ? 0x90u : 0x80u, hi = a == 0xf4u ? 0x8fu : 0xbfu;
        return c[1] >= lo && c[1] <= hi && (c[2] & 0xc0u) == 0x80u && (c[3] & 0xc0u) == 0x80u ? 4u : 0u;
    }
    return 0u;
}

/* Text from a pack (ini keys and values, file names) is printed to the terminal: every control
 * character becomes '?', C0 and DEL as bytes and C1 (U+0080 to U+009F) as its two UTF-8 bytes, and
 * so does every byte that is not part of valid UTF-8, a raw C1 byte (0x9B is CSI to some terminals)
 * among them, so a pack cannot send escape sequences. */
static void text_clean(char *s) {
    for (unsigned char *c = (unsigned char *)s; *c;) {
        if (*c < 0x80u) {
            if (*c < 0x20u || *c == 0x7fu) *c = '?';
            c++;
            continue;
        }
        const size_t n = utf8_valid_at(c);
        if (n == 0u) { *c++ = '?'; continue; }
        if (n == 2u && c[0] == 0xc2u && c[1] <= 0x9fu) { c[0] = '?'; c[1] = '?'; }
        c += n;
    }
}

static void diag_add(NkTexRepl *p, const char *fmt, ...) {
    if (p->diag_n >= NK_TEXREPL_MAX_DIAG) { p->diag_dropped++; return; }
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    text_clean(buf);
    size_t n = strlen(buf);
    char *copy = (char *)malloc(n + 1);
    if (!copy) { p->diag_dropped++; return; }
    memcpy(copy, buf, n + 1);
    p->diag[p->diag_n++] = copy;
}

int nk_texrepl_diag_take(NkTexRepl *p, char *out, size_t cap) {
    if (!p || !out || !cap) return 0;
    if (p->diag_n) {
        snprintf(out, cap, "%s", p->diag[0]);
        free(p->diag[0]);
        memmove(&p->diag[0], &p->diag[1], (p->diag_n - 1) * sizeof(p->diag[0]));
        p->diag_n--;
        return 1;
    }
    if (p->diag_dropped) {
        snprintf(out, cap, "... and %zu more not listed", p->diag_dropped);
        p->diag_dropped = 0;
        return 1;
    }
    return 0;
}

/* A disc ID in one normal form, so ULUS-10515, ulus10515 and ULUS_10515 are the same game: upper
   case, with '-' and '_' dropped. Returns its length, or 0 when `key` is not a disc ID (another
   character, or no letter or digit) or its normal form does not fit NK_TEXREPL_GAME_ID_MAX. */
static size_t game_id_normal(const char *key, char id[NK_TEXREPL_GAME_ID_MAX]) {
    size_t n = 0;
    for (; key && *key; key++) {
        const unsigned char c = (unsigned char)*key;
        if (c == '-' || c == '_') continue;
        if (!isalnum(c) || n + 1u >= NK_TEXREPL_GAME_ID_MAX) return 0;
        id[n++] = (char)toupper(c);
    }
    id[n] = '\0';
    return n;
}

/* A [games] key that reads as a disc ID is kept in its normal form, once; any other key is only
   counted. Returns 0 when memory runs out and -1 past NK_TEXREPL_GAMES_MAX kept IDs, which bounds
   the duplicate check (a 4 MiB ini could otherwise list some 350 000 IDs). */
static int game_push(NkTexRepl *p, const char *key) {
    char id[NK_TEXREPL_GAME_ID_MAX];
    const size_t n = game_id_normal(key, id);
    if (n == 0) return 1;
    for (size_t i = 0; i < p->games_n; i++) if (strcmp(p->games[i], id) == 0) return 1;
    if (p->games_n >= NK_TEXREPL_GAMES_MAX) return -1;
    if (p->games_n == p->games_cap) {
        const size_t cap = p->games_cap ? p->games_cap * 2u : 4u;
        char (*grown)[NK_TEXREPL_GAME_ID_MAX] = realloc(p->games, cap * sizeof(*grown));
        if (!grown) return 0;
        p->games = grown;
        p->games_cap = cap;
    }
    memcpy(p->games[p->games_n++], id, n + 1u);
    return 1;
}

size_t nk_texrepl_game_count(const NkTexRepl *p) { return p ? p->games_n : 0u; }
const char *nk_texrepl_game(const NkTexRepl *p, size_t i) {
    return p && i < p->games_n ? p->games[i] : NULL;
}
int nk_texrepl_names_game(const NkTexRepl *p, const char *disc_id) {
    if (!p || p->games_n == 0) return -1;
    char id[NK_TEXREPL_GAME_ID_MAX];
    if (game_id_normal(disc_id, id) == 0) return 0;
    for (size_t i = 0; i < p->games_n; i++) if (strcmp(p->games[i], id) == 0) return 1;
    return 0;
}

size_t nk_texrepl_diag_count(const NkTexRepl *p) {
    return p ? p->diag_n + (p->diag_dropped ? 1u : 0u) : 0u;
}
const char *nk_texrepl_diag(const NkTexRepl *p, size_t i) {
    if (!p) return "";
    if (i < p->diag_n) return p->diag[i];
    if (i == p->diag_n && p->diag_dropped) {
        NkTexRepl *m = (NkTexRepl *)p;   /* the summary line is formatted on demand */
        snprintf(m->diag_last, sizeof(m->diag_last), "... and %zu more not listed", p->diag_dropped);
        return m->diag_last;
    }
    return "";
}

NkTexReplHash nk_texrepl_hash_kind(const NkTexRepl *p) { return p ? p->hash_kind : NK_TEXREPL_HASH_QUICK; }
size_t nk_texrepl_entry_count(const NkTexRepl *p) { return p ? p->alias_n : 0u; }
int nk_texrepl_is_zip(const NkTexRepl *p) { return p && p->zip ? 1 : 0; }

static char *dup_str(const char *s) {
    size_t n = strlen(s);
    char *c = (char *)malloc(n + 1);
    if (c) memcpy(c, s, n + 1);
    return c;
}

static int record_push(NkTexRepl *p, uint64_t cachekey, uint32_t hash, int level, const char *file) {
    if (p->record_n == p->record_cap) {
        size_t cap = p->record_cap ? p->record_cap * 2 : 256;
        Record *grown = (Record *)realloc(p->records, cap * sizeof(Record));
        if (!grown) return 0;
        p->records = grown;
        p->record_cap = cap;
    }
    char *copy = dup_str(file ? file : "");
    if (!copy) return 0;
    for (char *c = copy; *c; c++) if (*c == '\\') *c = '/';
    Record *r = &p->records[p->record_n];
    r->cachekey = cachekey;
    r->hash = hash;
    r->level = level;
    r->order = (uint32_t)p->record_n;
    r->file = copy;
    p->record_n++;
    return 1;
}

static int record_cmp(const void *x, const void *y) {
    NK_SORT_COMPARE_COUNT();
    const Record *a = (const Record *)x, *b = (const Record *)y;
    if (a->cachekey != b->cachekey) return a->cachekey < b->cachekey ? -1 : 1;
    if (a->hash != b->hash) return a->hash < b->hash ? -1 : 1;
    if (a->level != b->level) return a->level < b->level ? -1 : 1;
    if (a->order != b->order) return a->order < b->order ? -1 : 1;
    return 0;
}

static void records_free(NkTexRepl *p) {
    for (size_t i = 0; i < p->record_n; i++) free(p->records[i].file);
    free(p->records);
    p->records = NULL;
    p->record_n = p->record_cap = 0;
}

/* The lines of one key make its mip chain the way PPSSPP's alias map does (ComputeAliasMap): the
 * last line for each level counts, levels are taken from 0 up while they follow one another, and the
 * names are joined with '|' and split again, so a value holding '|' names several levels. The chain
 * ends at the first blank name, and at 12 levels. A key whose lines do not start at level 0, or
 * whose level 0 is blank, has no chain: the texture is left alone, and no wildcard line is tried. */
static int build_chain(NkTexRepl *p, const Record *first, size_t n, Alias *a) {
    const char *names[NK_TEXREPL_MAX_LEVELS];
    size_t name_len[NK_TEXREPL_MAX_LEVELS];
    uint32_t count = 0;
    int expect = 0, stopped = 0;
    for (size_t i = 0; i < n && !stopped; i++) {
        if (i + 1 < n && first[i + 1].level == first[i].level) continue;   /* a later line for this level */
        if (first[i].level != expect) {
            diag_add(p, "%016llx%08x: mip level %d is listed with no level %d before it; %s",
                     (unsigned long long)a->cachekey, a->hash, first[i].level, expect,
                     expect == 0 ? "the texture is left alone, as PPSSPP leaves it" : "the levels stop there");
            break;
        }
        expect++;
        const char *s = first[i].file;
        for (;;) {   /* each '|'-separated part is a level */
            const char *bar = strchr(s, '|');
            size_t len = bar ? (size_t)(bar - s) : strlen(s);
            if (len == 0 || count == NK_TEXREPL_MAX_LEVELS) { stopped = 1; break; }
            names[count] = s;
            name_len[count] = len;
            count++;
            if (!bar) break;
            s = bar + 1;
        }
    }
    a->level_n = 0;
    a->levels = NULL;
    if (!count) return 1;
    a->levels = (Level *)calloc(count, sizeof(Level));
    if (!a->levels) return 0;
    for (uint32_t i = 0; i < count; i++) {
        a->levels[i].file = (char *)malloc(name_len[i] + 1);
        if (!a->levels[i].file) return 0;
        memcpy(a->levels[i].file, names[i], name_len[i]);
        a->levels[i].file[name_len[i]] = '\0';
        a->level_n = i + 1;
    }
    return 1;
}

static int aliases_build(NkTexRepl *p) {
    nk_zip_sort(p->records, p->record_n, sizeof(Record), record_cmp);
    size_t keys = 0;
    for (size_t i = 0; i < p->record_n; i++)
        if (i == 0 || p->records[i].cachekey != p->records[i - 1].cachekey || p->records[i].hash != p->records[i - 1].hash)
            keys++;
    p->aliases = (Alias *)calloc(keys ? keys : 1u, sizeof(Alias));
    if (!p->aliases) return 0;
    for (size_t i = 0; i < p->record_n;) {
        size_t j = i + 1;
        while (j < p->record_n && p->records[j].cachekey == p->records[i].cachekey && p->records[j].hash == p->records[i].hash) j++;
        Alias *a = &p->aliases[p->alias_n++];
        a->cachekey = p->records[i].cachekey;
        a->hash = p->records[i].hash;
        if (!build_chain(p, &p->records[i], j - i, a)) return 0;
        i = j;
    }
    records_free(p);
    return 1;
}

static int level_file_cmp(const void *x, const void *y) {
    NK_SORT_COMPARE_COUNT();
    return strcmp((*(Level *const *)x)->file, (*(Level *const *)y)->file);
}

/* Gives every distinct file name the levels hold one row in p->images, and each level its row; the
 * levels' duplicate copies of a name are freed. All memory is taken before anything changes, so a
 * failure leaves the levels as they were. */
static int images_build(NkTexRepl *p) {
    size_t n = 0;
    for (size_t i = 0; i < p->alias_n; i++) n += p->aliases[i].level_n;
    if (!n) return 1;
    Level **refs = (Level **)malloc(n * sizeof(*refs));
    Image *images = (Image *)calloc(n, sizeof(*images));
    if (!refs || !images) {
        free(refs);
        free(images);
        return 0;
    }
    size_t k = 0;
    for (size_t i = 0; i < p->alias_n; i++)
        for (uint32_t l = 0; l < p->aliases[i].level_n; l++) refs[k++] = &p->aliases[i].levels[l];
    nk_zip_sort(refs, n, sizeof(*refs), level_file_cmp);
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        Level *v = refs[i];
        if (m == 0 || strcmp(v->file, images[m - 1].file) != 0) {
            images[m++].file = v->file;
        } else {
            free(v->file);
            v->file = images[m - 1].file;
        }
        v->image = (uint32_t)(m - 1);
    }
    free(refs);
    Image *fit = (Image *)realloc(images, m * sizeof(*images));
    p->images = fit ? fit : images;
    p->image_n = m;
    return 1;
}

/* A sorted table whose rows start with the key (Alias, IndexedKey): the row for one exact key. */
typedef struct { uint64_t cachekey; uint32_t hash; } KeyHead;
static const void *key_find(const void *base, size_t n, size_t stride, uint64_t cachekey, uint32_t hash) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const KeyHead *a = (const KeyHead *)((const char *)base + mid * stride);
        if (a->cachekey == cachekey && a->hash == hash) return a;
        if (a->cachekey < cachekey || (a->cachekey == cachekey && a->hash < hash)) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

static int indexed_cmp(const void *x, const void *y) {
    NK_SORT_COMPARE_COUNT();
    const IndexedKey *a = (const IndexedKey *)x, *b = (const IndexedKey *)y;
    if (a->cachekey != b->cachekey) return a->cachekey < b->cachekey ? -1 : 1;
    if (a->hash != b->hash) return a->hash < b->hash ? -1 : 1;
    return a->order < b->order ? -1 : (a->order > b->order);
}

/* Sorts the [indexed] lines and keeps the last line of each key. */
static void indexed_finish(NkTexRepl *p) {
    if (p->indexed_n < 2) return;
    nk_zip_sort(p->indexed, p->indexed_n, sizeof(IndexedKey), indexed_cmp);
    size_t out = 0;
    for (size_t i = 0; i < p->indexed_n; i++) {
        if (i + 1 < p->indexed_n && p->indexed[i + 1].cachekey == p->indexed[i].cachekey &&
            p->indexed[i + 1].hash == p->indexed[i].hash) continue;
        p->indexed[out++] = p->indexed[i];
    }
    p->indexed_n = out;
}

/* ------------------------------------------------------------------------------------- ini */

static int ci_equal(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static int ci_cmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
}

/* PPSSPP's StripSpaces: space, tab, CR and LF. */
static int ini_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static char *trim(char *s) {
    while (*s && ini_space(*s)) s++;
    char *e = s + strlen(s);
    while (e > s && ini_space(e[-1])) e--;
    *e = '\0';
    return s;
}

/* A whole unsigned number as PPSSPP's TryParse reads one (strtoul with base 0: 0x hexadecimal, a
 * leading 0 octal; leading blanks allowed, nothing after it). */
static int parse_u32(const char *s, uint32_t *out) {
    while (*s == ' ' || *s == '\t') s++;
    if (!*s || *s == '-') return 0;
    char *end;
    unsigned long long v = strtoull(s, &end, 0);
    if (*end || v > 0xFFFFFFFFull) return 0;
    *out = (uint32_t)v;
    return 1;
}

static int parse_int(const char *s, int *out) {
    if (!*s) return 0;
    char *end;
    long v = strtol(s, &end, 10);
    if (*end || v < -1000000 || v > 1000000) return 0;
    *out = (int)v;
    return 1;
}

/* PPSSPP's TryParse for a bool: 1, 0, true or false (case ignored). */
static int parse_bool(const char *s, int *out) {
    if (ci_equal(s, "true") || !strcmp(s, "1")) { *out = 1; return 1; }
    if (ci_equal(s, "false") || !strcmp(s, "0")) { *out = 0; return 1; }
    return 0;
}

/* A path that stays inside the pack: relative, no ".." component, no drive or stream. */
static int path_inside_pack(const char *rel) {
    if (!*rel || rel[0] == '/' || rel[0] == '\\' || strchr(rel, ':')) return 0;
    const char *s = rel;
    while (*s) {
        const char *e = s;
        while (*e && *e != '/' && *e != '\\') e++;
        if (e - s == 2 && s[0] == '.' && s[1] == '.') return 0;
        s = *e ? e + 1 : e;
    }
    return 1;
}

/* A texture name as PPSSPP reads one, with sscanf("%16llx%8x_%d") and at least the first field
 * matched: up to 16 hex digits of key, then up to 8 of data hash, then "_level". A field that does
 * not match is 0 (the level, 0 too), so a short name is a wildcard: "<16 digits>" alone is the key
 * with no data hash. The C library's sscanf is the one PPSSPP calls, so its rules (leading blanks,
 * a 0x prefix, a sign) are the same. The level alone is read with strtol, as %d reads it, but a
 * level past an int's range (undefined for sscanf) becomes INT_MAX or INT_MIN, which no chain has. */
static int parse_hash_name(const char *name, uint64_t *cachekey, uint32_t *hash, int *level) {
    char k[128];
    snprintf(k, sizeof(k), "%s", name);   /* PPSSPP copies the key into 128 bytes */
    unsigned long long ck = 0;
    unsigned int h = 0;
    int lv = 0, n1 = -1, n2 = -1;
    if (sscanf(k, "%16llx%n", &ck, &n1) < 1 || n1 < 0) return 0;
    if (sscanf(k + n1, "%8x%n", &h, &n2) == 1 && n2 >= 0 && k[n1 + n2] == '_') {
        const char *ls = k + n1 + n2 + 1;
        char *end = NULL;
        errno = 0;
        const long v = strtol(ls, &end, 10);
        if (end != ls) lv = (errno == ERANGE || v > INT_MAX) ? (v < 0 ? INT_MIN : INT_MAX) : v < INT_MIN ? INT_MIN : (int)v;
    }
    *cachekey = (uint64_t)ck;
    *hash = (uint32_t)h;
    *level = lv;
    return 1;
}

typedef enum { SEC_NONE, SEC_OPTIONS, SEC_HASHES, SEC_HASHRANGES, SEC_REDUCE, SEC_FILTERING,
               SEC_INDEXED, SEC_GAMES, SEC_UNKNOWN, SEC_REPEATED, SEC_COUNT } Section;

/* [hashranges] and [reducehashranges] lines are kept in file order, then sorted once (ranges_finish)
 * with the last line for each texture kept, as PPSSPP's map keeps it, and looked up by binary search:
 * reading them costs O(n log n) and each texture hash O(log n), with n bounded by the 4 MiB ini. */
static int range_push(NkTexRepl *p, HashRange r) {
    if (p->range_n == p->range_cap) {
        size_t cap = p->range_cap ? p->range_cap * 2 : 16;
        HashRange *g = (HashRange *)realloc(p->ranges, cap * sizeof(*g));
        if (!g) return 0;
        p->ranges = g;
        p->range_cap = cap;
    }
    r.seq = (uint32_t)p->range_n;
    p->ranges[p->range_n++] = r;
    return 1;
}
static int reduce_push(NkTexRepl *p, ReduceRange r) {
    if (p->reduce_n == p->reduce_cap) {
        size_t cap = p->reduce_cap ? p->reduce_cap * 2 : 16;
        ReduceRange *g = (ReduceRange *)realloc(p->reduce, cap * sizeof(*g));
        if (!g) return 0;
        p->reduce = g;
        p->reduce_cap = cap;
    }
    r.seq = (uint32_t)p->reduce_n;
    p->reduce[p->reduce_n++] = r;
    return 1;
}

static int range_key_cmp(const HashRange *a, uint32_t addr, uint32_t w, uint32_t h) {
    if (a->addr != addr) return a->addr < addr ? -1 : 1;
    if (a->w != w) return a->w < w ? -1 : 1;
    if (a->h != h) return a->h < h ? -1 : 1;
    return 0;
}
static int range_cmp(const void *x, const void *y) {
    NK_SORT_COMPARE_COUNT();
    const HashRange *a = (const HashRange *)x, *b = (const HashRange *)y;
    const int c = range_key_cmp(a, b->addr, b->w, b->h);
    return c ? c : (a->seq < b->seq ? -1 : (a->seq > b->seq));
}
static int reduce_key_cmp(const ReduceRange *a, uint32_t w, uint32_t h) {
    if (a->w != w) return a->w < w ? -1 : 1;
    if (a->h != h) return a->h < h ? -1 : 1;
    return 0;
}
static int reduce_cmp(const void *x, const void *y) {
    NK_SORT_COMPARE_COUNT();
    const ReduceRange *a = (const ReduceRange *)x, *b = (const ReduceRange *)y;
    const int c = reduce_key_cmp(a, b->w, b->h);
    return c ? c : (a->seq < b->seq ? -1 : (a->seq > b->seq));
}

static void ranges_finish(NkTexRepl *p) {
    nk_zip_sort(p->ranges, p->range_n, sizeof(HashRange), range_cmp);
    size_t out = 0;
    for (size_t i = 0; i < p->range_n; i++) {
        if (i + 1 < p->range_n && !range_key_cmp(&p->ranges[i + 1], p->ranges[i].addr, p->ranges[i].w, p->ranges[i].h))
            continue;   /* a later line for the same texture replaces this one */
        p->ranges[out++] = p->ranges[i];
    }
    p->range_n = out;
    nk_zip_sort(p->reduce, p->reduce_n, sizeof(ReduceRange), reduce_cmp);
    out = 0;
    for (size_t i = 0; i < p->reduce_n; i++) {
        if (i + 1 < p->reduce_n && !reduce_key_cmp(&p->reduce[i + 1], p->reduce[i].w, p->reduce[i].h)) continue;
        p->reduce[out++] = p->reduce[i];
    }
    p->reduce_n = out;
}

/* Splits `s` at commas into at most `max` parts; returns how many there were (more than max: max + 1). */
static int split_commas(char *s, char **parts, int max) {
    int n = 0;
    for (;;) {
        if (n == max) return max + 1;
        parts[n++] = s;
        char *c = strchr(s, ',');
        if (!c) return n;
        *c = '\0';
        s = c + 1;
    }
}

/* PPSSPP's ini line rules (Common/Data/Format/IniFile.cpp), for "key = value": the key ends at the
 * first '='; an unescaped '#' before it makes the line no key = value; in the value, an unescaped '#'
 * starts a comment unless the whole value is in double quotes, "\#" is a '#', and the blanks around
 * and one pair of double quotes around the value are removed. Edits `line` in place. Returns 0 when
 * the line is no key = value. */
static int split_key_value(char *line, char **key_out, char **value_out) {
    char *w = line, *r = line;
    for (;; r++) {   /* the key, unescaping "\#" */
        if (!*r) return 0;
        if (*r == '=') break;
        if (*r == '#') {
            if (r == line || r[-1] != '\\') return 0;
            w[-1] = '#';   /* the backslash already copied becomes the '#' */
            continue;
        }
        *w++ = *r;
    }
    if (r == line) return 0;   /* nothing before the '=' */
    *w = '\0';
    char *value = r + 1;
    char *stripped = trim(value);
    size_t sl = strlen(stripped);
    if (!(sl >= 2 && stripped[0] == '"' && stripped[sl - 1] == '"')) {
        char *vw = stripped, *vr = stripped;
        for (; *vr; vr++) {
            if (*vr == '#') {
                if (vr == stripped || vr[-1] != '\\') break;
                vw[-1] = '#';
                continue;
            }
            *vw++ = *vr;
        }
        *vw = '\0';
        stripped = trim(stripped);
        sl = strlen(stripped);
    }
    if (sl >= 2 && stripped[0] == '"' && stripped[sl - 1] == '"') {
        stripped[sl - 1] = '\0';
        stripped++;
    }
    *key_out = trim(line);
    *value_out = stripped;
    return 1;
}

static const char *section_name(Section s) {
    switch (s) {
    case SEC_OPTIONS: return "options";
    case SEC_HASHES: return "hashes";
    case SEC_HASHRANGES: return "hashranges";
    case SEC_REDUCE: return "reducehashranges";
    case SEC_FILTERING: return "filtering";
    case SEC_INDEXED: return "indexed";
    case SEC_GAMES: return "games";
    default: return "";
    }
}

/* The [options] keys read; PPSSPP takes the first line of each. */
enum { OPT_HASH, OPT_IGNORE_ADDRESS, OPT_REDUCE_HASH, OPT_IGNORE_MIPMAP, OPT_SAVE_INDEXED, OPT_SKIP64,
       OPT_SKIP128, OPT_VERSION, OPT_COUNT };
static const char *const kOptionKeys[OPT_COUNT] = {
    "hash", "ignoreAddress", "reduceHash", "ignoreMipmap", "saveIndexed", "skipLastDXT1Blocks128x64",
    "skipLastDXT1Blocks128x128", "version"};

/* Parses the whole ini into `p`. Returns 0 with `err` set when it is malformed. */
static int parse_ini(NkTexRepl *p, char *text, char *err, size_t err_cap) {
    Section sec = SEC_NONE;
    int line_no = 0;
    int seen[SEC_COUNT] = {0};
    int opt_seen[OPT_COUNT] = {0};
    size_t filtering = 0, indexed = 0, games = 0;
    char *save = NULL;
    for (char *c = text; *c; c++) if (*c == '\r') *c = ' ';   /* a CRLF line end's CR is a blank */
    for (char *line = text; line; line = save) {
        char *nl = strchr(line, '\n');
        if (nl) { *nl = '\0'; save = nl + 1; } else save = NULL;
        line_no++;
        if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
            line += 3;   /* a UTF-8 byte order mark, removed from any line as PPSSPP does */
        if (line[0] == '[') {
            /* A section header starts the line; what follows its ']' is a comment. */
            char *close = strchr(line, ']');
            if (!close) {
                snprintf(err, err_cap, "textures.ini line %d: unfinished section header '%s'", line_no, trim(line));
                return 0;
            }
            *close = '\0';
            const char *name = line + 1;   /* not trimmed: PPSSPP compares the name as written */
            Section next = SEC_UNKNOWN;
            for (int s = SEC_OPTIONS; s <= SEC_GAMES; s++)
                if (ci_equal(name, section_name((Section)s))) next = (Section)s;
            if (next == SEC_UNKNOWN) {
                diag_add(p, "textures.ini: unknown section [%s] not read", name);
            } else if (seen[next]) {
                diag_add(p, "textures.ini: a second [%s] section is not read (only the first one is, as in PPSSPP)", name);
                next = SEC_REPEATED;
            } else {
                seen[next] = 1;
            }
            sec = next;
            continue;
        }
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            snprintf(err, err_cap, "textures.ini line %d: '%s' is indented; a section header starts its line", line_no, s);
            return 0;
        }
        char *key, *value;
        if (!split_key_value(s, &key, &value)) {
            snprintf(err, err_cap, "textures.ini line %d: '%s' is not a section, a comment or key = value",
                     line_no, s);
            return 0;
        }
        if (sec == SEC_NONE) {
            snprintf(err, err_cap, "textures.ini line %d: '%s' is outside any section", line_no, key);
            return 0;
        }
        switch (sec) {
        case SEC_OPTIONS: {
            int opt = -1;
            for (int o = 0; o < OPT_COUNT; o++) if (ci_equal(key, kOptionKeys[o])) opt = o;
            if (opt < 0) { diag_add(p, "textures.ini: unknown [options] key '%s' not read", key); break; }
            if (opt_seen[opt]) {
                diag_add(p, "textures.ini: a later '%s' line is not read (the first one counts, as in PPSSPP)", key);
                break;
            }
            opt_seen[opt] = 1;
            if (opt == OPT_HASH) {
                if (ci_equal(value, "quick")) p->hash_kind = NK_TEXREPL_HASH_QUICK;
                else if (ci_equal(value, "xxh32")) p->hash_kind = NK_TEXREPL_HASH_XXH32;
                else if (ci_equal(value, "xxh64")) p->hash_kind = NK_TEXREPL_HASH_XXH64;
                else {
                    snprintf(err, err_cap, "textures.ini: unsupported hash type '%s' (use quick, xxh32 or xxh64)",
                             value);
                    return 0;
                }
            } else if (opt == OPT_IGNORE_ADDRESS || opt == OPT_REDUCE_HASH || opt == OPT_IGNORE_MIPMAP) {
                int b;
                if (!parse_bool(value, &b)) {
                    /* PPSSPP's Get leaves the option at its default (false) then. */
                    diag_add(p, "textures.ini: %s = '%s' is not true or false; read as false, as PPSSPP reads it",
                             key, value);
                    b = 0;
                }
                if (opt == OPT_IGNORE_ADDRESS) p->ignore_address = b;
                else if (opt == OPT_REDUCE_HASH) p->reduce_hash = b;
                /* ignoreMipmap only stops PPSSPP from saving mip levels when it dumps textures. */
            } else if (opt == OPT_SKIP64 || opt == OPT_SKIP128) {
                int n;
                if (!parse_int(value, &n)) {
                    diag_add(p, "textures.ini: %s = '%s' is not a whole number; read as 0", key, value);
                    n = 0;
                }
                if (opt == OPT_SKIP64) p->skip_dxt1_128x64 = n;
                else p->skip_dxt1_128x128 = n;
            } else if (opt == OPT_VERSION) {
                int v;
                if (parse_int(value, &v) && v > 1)
                    diag_add(p, "textures.ini: version %d is newer than this build reads (1); reading it anyway", v);
            } else if (opt == OPT_SAVE_INDEXED) {
                if (*value && !ci_equal(value, "false") && strcmp(value, "0") != 0)
                    diag_add(p, "textures.ini: saveIndexed not applied (indexed textures are not supported)");
            }
            break;
        }
        case SEC_HASHES: {
            uint64_t ck;
            uint32_t h;
            int level;
            if (!*key) break;
            if (!parse_hash_name(key, &ck, &h, &level)) {
                diag_add(p, "textures.ini: [hashes] line '%s' does not start with a hash name; not read", key);
                break;
            }
            if (*value && !path_inside_pack(value)) {
                diag_add(p, "textures.ini: [hashes] '%s' names '%s', which is outside the pack; not read",
                         key, value);
                break;
            }
            if (!record_push(p, ck, h, level, value)) { snprintf(err, err_cap, "out of memory"); return 0; }
            break;
        }
        case SEC_HASHRANGES: {
            /* addr,w,h = newW,newH; an address without 0x is hexadecimal all the same */
            char *kp[4], *vp[3];
            HashRange r;
            char addr[40];
            if (split_commas(key, kp, 3) != 3 || split_commas(value, vp, 2) != 2) {
                diag_add(p, "textures.ini: [hashranges] '%s' is not addr,w,h = w,h; not read", key);
                break;
            }
            snprintf(addr, sizeof(addr), "%s%s", (kp[0][0] == '0' && (kp[0][1] == 'x' || kp[0][1] == 'X')) ? "" : "0x", kp[0]);
            if (!parse_u32(addr, &r.addr) || !parse_u32(kp[1], &r.w) || !parse_u32(kp[2], &r.h) ||
                !parse_u32(vp[0], &r.new_w) || !parse_u32(vp[1], &r.new_h)) {
                diag_add(p, "textures.ini: [hashranges] line for '%s' has a bad number; not read", kp[0]);
                break;
            }
            if (r.new_w > r.w || r.new_h > r.h || r.new_w == 0 || r.new_h == 0) {
                diag_add(p, "textures.ini: [hashranges] line for '%s' is larger than its texture or empty; not read", kp[0]);
                break;
            }
            if (!range_push(p, r)) { snprintf(err, err_cap, "out of memory"); return 0; }
            break;
        }
        case SEC_REDUCE: {
            char *kp[3], *vp[2];
            ReduceRange r;
            char *end;
            if (split_commas(key, kp, 2) != 2 || split_commas(value, vp, 1) != 1) {
                diag_add(p, "textures.ini: [reducehashranges] '%s' is not w,h = fraction; not read", key);
                break;
            }
            r.value = strtof(vp[0], &end);
            if (!parse_u32(kp[0], &r.w) || !parse_u32(kp[1], &r.h) || *end || !*vp[0]) {
                diag_add(p, "textures.ini: [reducehashranges] line for '%s' has a bad number; not read", kp[0]);
                break;
            }
            if (!isfinite(r.value)) {   /* NaN or infinity would not convert to a size */
                diag_add(p, "textures.ini: [reducehashranges] line for '%s' is not a finite number; not read", kp[0]);
                break;
            }
            if (r.value == 0.0f) {
                diag_add(p, "textures.ini: [reducehashranges] line for '%s' is 0; not read", kp[0]);
                break;
            }
            if (!reduce_push(p, r)) { snprintf(err, err_cap, "out of memory"); return 0; }
            break;
        }
        case SEC_FILTERING: filtering++; break;
        case SEC_INDEXED: {
            /* hashname = true | 1 | gpu | false | 0, the name read as [hashes] reads one but with no level */
            unsigned long long ck = 0;
            unsigned int h = 0;
            char k[128];
            uint8_t mode;
            snprintf(k, sizeof(k), "%s", key);
            if (sscanf(k, "%16llx%8x", &ck, &h) < 1) {
                diag_add(p, "textures.ini: [indexed] line '%s' does not start with a hash name; not read", key);
                break;
            }
            if (ci_equal(value, "true") || !strcmp(value, "1")) mode = 1;
            else if (ci_equal(value, "gpu")) mode = 2;
            else if (ci_equal(value, "false") || !strcmp(value, "0")) mode = 0;
            else {
                diag_add(p, "textures.ini: [indexed] '%s = %s' is not true, gpu or false; not read", key, value);
                break;
            }
            if (p->indexed_n == p->indexed_cap) {
                size_t cap = p->indexed_cap ? p->indexed_cap * 2 : 16;
                IndexedKey *g = (IndexedKey *)realloc(p->indexed, cap * sizeof(*g));
                if (!g) { snprintf(err, err_cap, "out of memory"); return 0; }
                p->indexed = g;
                p->indexed_cap = cap;
            }
            IndexedKey *ik = &p->indexed[p->indexed_n];
            ik->cachekey = (uint64_t)ck;
            ik->hash = (uint32_t)h;
            ik->order = (uint32_t)p->indexed_n;
            ik->mode = mode;
            p->indexed_n++;
            indexed++;
            break;
        }
        case SEC_GAMES:
            games++;
            switch (game_push(p, key)) {
            case 0: snprintf(err, err_cap, "out of memory"); return 0;
            case -1:
                snprintf(err, err_cap, "[games] lists more than %d games", NK_TEXREPL_GAMES_MAX);
                return 0;
            default: break;
            }
            break;
        default: break;
        }
    }
    if (!opt_seen[OPT_HASH]) {
        snprintf(err, err_cap, "textures.ini: no hash type in [options] (use hash = quick, xxh32 or xxh64)");
        return 0;
    }
    if (filtering) diag_add(p, "textures.ini: %zu [filtering] entries not applied (the game's own filtering is kept)", filtering);
    if (indexed)
        diag_add(p, "textures.ini: %zu [indexed] entries read; a texture they mark indexed is not replaced "
                    "(its image holds palette indices, which this build does not draw)", indexed);
    if (games) diag_add(p, "textures.ini: %zu [games] entries not applied (per-game ini overrides are not supported)", games);
    if (p->reduce_hash && p->hash_kind == NK_TEXREPL_HASH_QUICK) {
        p->reduce_hash = 0;
        diag_add(p, "textures.ini: reduceHash needs hash = xxh32 or xxh64; ignored with the quick hash");
    }
    if (p->ignore_address && p->hash_kind == NK_TEXREPL_HASH_QUICK) {
        p->ignore_address = 0;
        diag_add(p, "textures.ini: ignoreAddress needs hash = xxh32 or xxh64; ignored with the quick hash");
    }
    return 1;
}

/* ------------------------------------------------------------------------------- files */

typedef enum { PACK_READ_OK, PACK_READ_MISSING, PACK_READ_TOO_LARGE, PACK_READ_DAMAGED, PACK_READ_NOT_REGULAR,
               PACK_READ_OUTSIDE, PACK_READ_SAME } PackRead;

/* Files already read, by identity, so that one file reached by several names (a ZIP entry under
 * names that differ in case; a folder file by another case, an 8.3 name or a link) is read once.
 * The identity is known before the bytes are read: the ZIP entry the name resolves to, or the opened
 * handle's volume and 128-bit file ID (device and inode on POSIX). A file whose ID cannot be had has
 * none, and is read under each name as any other file. The key is ID_WORDS 64-bit words, the first
 * the kind (ID_ZIP, ID_POSIX, ID_WIN; 0 when no identity is known); a row of 0 in id_row marks an
 * empty slot. */
enum { ID_WORDS = 4, ID_NONE = 0, ID_ZIP = 1, ID_POSIX = 2, ID_WIN = 3 };

static size_t id_slot(const NkTexRepl *p, const uint64_t id[ID_WORDS]) {
    uint64_t h = 0x632BE59BD9B4E019ull;
    for (int k = 0; k < ID_WORDS; k++) {
        h = (h ^ id[k]) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    size_t at = (size_t)h & (p->id_cap - 1u);
    while (p->id_row[at] && memcmp(&p->id_key[ID_WORDS * at], id, ID_WORDS * sizeof(uint64_t)) != 0)
        at = (at + 1u) & (p->id_cap - 1u);
    return at;
}

/* The image already read from file `id` (1 + its row), or 0. */
static uint32_t id_find(const NkTexRepl *p, const uint64_t id[ID_WORDS]) {
    return p->id_cap ? p->id_row[id_slot(p, id)] : 0u;
}

/* Puts a file's identity in `id` (when asked) and says whether an image has read that file already
 * (when `same` is given; *same is then that image's row plus 1). */
static int seen_before(const NkTexRepl *p, uint64_t id[ID_WORDS], uint64_t kind, uint64_t a, uint64_t b, uint64_t c,
                       uint32_t *same) {
    if (!id) return 0;
    id[0] = kind;
    id[1] = a;
    id[2] = b;
    id[3] = c;
    return same && (*same = id_find(p, id)) != 0;
}

/* Records that image `row` read file `id` (or found it damaged). Without memory the file is simply
 * read again under its other names, as before this table: nothing is refused for it. */
static void id_add(NkTexRepl *p, const uint64_t id[ID_WORDS], uint32_t row) {
    if (id[0] == ID_NONE) return;
    if ((p->id_n + 1u) * 2u > p->id_cap) {
        const size_t cap = p->id_cap ? p->id_cap * 2u : 64u;
        uint64_t *key = (uint64_t *)calloc(cap, ID_WORDS * sizeof(uint64_t));
        uint32_t *rows = (uint32_t *)calloc(cap, sizeof(uint32_t));
        if (!key || !rows) { free(key); free(rows); return; }
        uint64_t *old_key = p->id_key;
        uint32_t *old_row = p->id_row;
        const size_t old_cap = p->id_cap;
        p->id_key = key;
        p->id_row = rows;
        p->id_cap = cap;
        for (size_t i = 0; i < old_cap; i++) {
            if (!old_row[i]) continue;
            const size_t at = id_slot(p, &old_key[ID_WORDS * i]);
            memcpy(&p->id_key[ID_WORDS * at], &old_key[ID_WORDS * i], ID_WORDS * sizeof(uint64_t));
            p->id_row[at] = old_row[i];
        }
        free(old_key);
        free(old_row);
    }
    const size_t at = id_slot(p, id);
    if (p->id_row[at]) return;
    memcpy(&p->id_key[ID_WORDS * at], id, ID_WORDS * sizeof(uint64_t));
    p->id_row[at] = row + 1u;
    p->id_n++;
}

/* A folder pack's file is read only when the handle it opens is a regular file on a disk, no larger
 * than the cap, whose final path (links and junctions resolved) is inside the pack's resolved root.
 * The checks are on the opened handle, not the name, so a device (CON, COM1, a FIFO), a link out of
 * the pack and a file swapped for one between a listing and the read are all refused without a
 * byte read. POSIX opens without blocking, so a FIFO is refused instead of waiting for a writer. */
#if defined(_WIN32)
static WCHAR *wide_of(const char *utf8) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (n <= 0) return NULL;
    WCHAR *w = (WCHAR *)malloc((size_t)n * sizeof(WCHAR));
    if (w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, w, n) != n) { free(w); w = NULL; }
    return w;
}

/* The handle's final path, links and junctions resolved (malloc'd), or NULL. */
static WCHAR *final_path_of(HANDLE h) {
    DWORD n = GetFinalPathNameByHandleW(h, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n == 0) return NULL;
    WCHAR *w = (WCHAR *)malloc(((size_t)n + 1) * sizeof(WCHAR));
    if (!w) return NULL;
    DWORD got = GetFinalPathNameByHandleW(h, w, n + 1, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (got == 0 || got > n) { free(w); return NULL; }
    return w;
}

static int root_resolve(NkTexRepl *p) {
    WCHAR *w = wide_of(p->dir);
    if (!w) return 0;
    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return 0;
    p->root_real = final_path_of(h);
    CloseHandle(h);
    if (!p->root_real) return 0;
    size_t n = wcslen(p->root_real);
    while (n > 0 && p->root_real[n - 1] == L'\\') p->root_real[--n] = L'\0';   /* a drive root's own '\' */
    return 1;
}

static int inside_root(const NkTexRepl *p, const WCHAR *final) {
    size_t n = wcslen(p->root_real);
    return wcslen(final) > n + 1 && CompareStringOrdinal(final, (int)n, p->root_real, (int)n, TRUE) == CSTR_EQUAL &&
           final[n] == L'\\';
}

/* The handle's volume and 128-bit file ID: the 64-bit index GetFileInformationByHandle gives is not
 * unique on ReFS. 0 when the file system gives none (the ID all zeros). */
static int file_id_of(HANDLE h, uint64_t out[3]) {
    FILE_ID_INFO fid;
    if (!GetFileInformationByHandleEx(h, FileIdInfo, &fid, sizeof(fid))) return 0;
    memcpy(&out[1], fid.FileId.Identifier, 16);
    out[0] = fid.VolumeSerialNumber;
    return out[1] != 0 || out[2] != 0;
}

static unsigned char *read_regular(const NkTexRepl *p, const char *path, size_t cap, size_t *size, PackRead *why,
                                   int probe, uint64_t id[ID_WORDS], uint32_t *same) {
    *why = PACK_READ_MISSING;
    uint64_t fid[3] = { 0, 0, 0 };
    WCHAR *w = wide_of(path);
    if (!w) return NULL;
    /* Opened before it is checked (the checks are on this handle): should a name lead to a pipe, its
     * server may only identify the caller, not act as it. */
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    unsigned char *buf = NULL;
    BY_HANDLE_FILE_INFORMATION info;
    LARGE_INTEGER len;
    WCHAR *final = NULL;
    if (GetFileType(h) != FILE_TYPE_DISK || !GetFileInformationByHandle(h, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        *why = PACK_READ_NOT_REGULAR;
    } else if (p->root_real && (!(final = final_path_of(h)) || !inside_root(p, final))) {
        *why = PACK_READ_OUTSIDE;
    } else if (!GetFileSizeEx(h, &len) || len.QuadPart < 0 || (unsigned long long)len.QuadPart > cap) {
        *why = PACK_READ_TOO_LARGE;
    } else if (id && file_id_of(h, fid) && seen_before(p, id, ID_WIN, fid[0], fid[1], fid[2], same)) {
        *why = PACK_READ_SAME;
    } else if (probe) {
        *why = PACK_READ_OK;
    } else if ((buf = (unsigned char *)malloc(len.QuadPart ? (size_t)len.QuadPart : 1u)) != NULL) {
        size_t at = 0, want = (size_t)len.QuadPart;
        while (at < want) {
            DWORD got = 0, ask = want - at > 0x40000000u ? 0x40000000u : (DWORD)(want - at);
            if (!ReadFile(h, buf + at, ask, &got, NULL) || got == 0) break;
            at += got;
        }
        if (at == want) { *size = want; *why = PACK_READ_OK; }
        else { free(buf); buf = NULL; *why = PACK_READ_DAMAGED; }
    }
    free(final);
    CloseHandle(h);
    return buf;
}
#else
static int root_resolve(NkTexRepl *p) {
    p->root_real = realpath(p->dir, NULL);
    return p->root_real != NULL;
}

static int inside_root(const NkTexRepl *p, const char *final) {
    size_t n = strlen(p->root_real);
    if (n == 1 && p->root_real[0] == '/') n = 0;   /* the file system's root */
    return strlen(final) > n + 1 && strncmp(final, p->root_real, n) == 0 && final[n] == '/';
}

static unsigned char *read_regular(const NkTexRepl *p, const char *path, size_t cap, size_t *size, PackRead *why,
                                   int probe, uint64_t id[ID_WORDS], uint32_t *same) {
    *why = PACK_READ_MISSING;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) return NULL;
    unsigned char *buf = NULL;
    struct stat st;
    char *final = NULL;
#if defined(__linux__)
    char link[64], target[PATH_MAX + 1];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t tn = readlink(link, target, PATH_MAX);
    if (tn > 0) {
        target[tn] = '\0';
        final = (char *)malloc((size_t)tn + 1);
        if (final) memcpy(final, target, (size_t)tn + 1);
    } else
#endif
    {
        final = realpath(path, NULL);   /* the handle's own path where the system cannot say */
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        *why = PACK_READ_NOT_REGULAR;
    } else if (p->root_real && (!final || !inside_root(p, final))) {
        *why = PACK_READ_OUTSIDE;
    } else if (st.st_size < 0 || (unsigned long long)st.st_size > cap) {
        *why = PACK_READ_TOO_LARGE;
    } else if (seen_before(p, id, ID_POSIX, (uint64_t)st.st_dev, (uint64_t)st.st_ino, 0u, same)) {
        *why = PACK_READ_SAME;
    } else if (probe) {
        *why = PACK_READ_OK;
    } else if ((buf = (unsigned char *)malloc(st.st_size ? (size_t)st.st_size : 1u)) != NULL) {
        size_t at = 0, want = (size_t)st.st_size;
        while (at < want) {
            ssize_t got = read(fd, buf + at, want - at);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) break;
            at += (size_t)got;
        }
        if (at == want) { *size = want; *why = PACK_READ_OK; }
        else { free(buf); buf = NULL; *why = PACK_READ_DAMAGED; }
    }
    free(final);
    close(fd);
    return buf;
}
#endif

static int zip_name_cmp(const void *x, const void *y) {
    NK_SORT_COMPARE_COUNT();
    return ci_cmp(((const ZipName *)x)->rel, ((const ZipName *)y)->rel);
}

/* A ZIP pack's file by its name under the root, case ignored as PPSSPP's ZIP lookups ignore it. */
static const ZipName *zip_find(const NkTexRepl *p, const char *rel) {
    size_t lo = 0, hi = p->zip_name_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = ci_cmp(p->zip_names[mid].rel, rel);
        if (c == 0) return &p->zip_names[mid];
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

/* A pack file's bytes, at most `cap`: from the folder, or from the ZIP without writing anything.
 * With `id`, the file's identity is put there; with `same` too, a file some image has already read
 * is not read again: *why is PACK_READ_SAME and *same that image's row plus 1. With `held`, the most
 * memory the read held at once: the file, and for a DEFLATE entry its packed bytes beside it. */
static unsigned char *pack_read(NkTexRepl *p, const char *rel, size_t cap, size_t *size, PackRead *why,
                                char *zip_why, size_t zip_why_cap, uint64_t id[ID_WORDS], uint32_t *same,
                                uint64_t *held) {
    *why = PACK_READ_MISSING;
    if (held) *held = 0;
    if (zip_why_cap) zip_why[0] = '\0';
    if (p->zip) {
        const ZipName *z = zip_find(p, rel);
        if (!z) return NULL;
        if (z->size > cap) { *why = PACK_READ_TOO_LARGE; return NULL; }
        if (id) {
            if (seen_before(p, id, ID_ZIP, (uint64_t)z->index, 0u, 0u, same)) { *why = PACK_READ_SAME; return NULL; }
        }
        unsigned char *buf = (unsigned char *)malloc(z->size ? z->size : 1u);
        if (!buf) { *why = PACK_READ_DAMAGED; return NULL; }
        if (held) *held = (uint64_t)z->size + (z->deflated ? z->packed : 0u);
        NkZipError ze;
        NkZipStatus st = nk_zip_reader_read(p->zip, z->index, buf, z->size, &ze);
        if (st != NK_ZIP_OK) {
            free(buf);
            *why = PACK_READ_DAMAGED;
            if (zip_why_cap) snprintf(zip_why, zip_why_cap, "%s", nk_zip_status_code(st));
            return NULL;
        }
        *size = z->size;
        *why = PACK_READ_OK;
        return buf;
    }
    char path[1400];
    if (snprintf(path, sizeof(path), "%s/%s", p->dir, rel) >= (int)sizeof(path)) return NULL;
    unsigned char *buf = read_regular(p, path, cap, size, why, 0, id, same);
    if (held && buf) *held = *size;
    return buf;
}

static int ends_with_ci(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && ci_equal(s + a - b, suffix);
}

/* A file in the pack's root named for a texture, as PPSSPP's ScanForHashNamedFiles takes one: a
 * name of 24 characters, or 26 to 27 with '_' at the 25th, before a .png, .dds, .ktx2 or .zim
 * extension, and not starting with '.'. Each is listed; only PNG is decoded. */
static int scan_name(NkTexRepl *p, const char *name) {
    if (!name || !*name || name[0] == '.') return 1;
    const char *dot = strrchr(name, '.');
    if (!dot) return 1;
    size_t stem = (size_t)(dot - name);
    if (!(stem == 24 || (stem >= 26 && stem <= 27 && name[24] == '_'))) return 1;
    int png = ends_with_ci(name, ".png");
    if (!png && !ends_with_ci(name, ".dds") && !ends_with_ci(name, ".ktx2") && !ends_with_ci(name, ".zim")) return 1;
    char base[40];
    memcpy(base, name, stem);
    base[stem] = '\0';
    uint64_t ck;
    uint32_t h;
    int level;
    if (!parse_hash_name(base, &ck, &h, &level)) return 1;
    if (!png) p->nonpng_listed++;
    return record_push(p, ck, h, level, name);
}

typedef struct { NkTexRepl *p; int ok; } ScanCtx;

static bool scan_file(const char *name, void *ctx) {
    ScanCtx *sc = (ScanCtx *)ctx;
    if (!scan_name(sc->p, name)) { sc->ok = 0; return false; }
    return true;
}

static int name_is_dot_or_mac(const char *seg, size_t n) {
    return (n > 0 && seg[0] == '.') || (n == 8 && !strncmp(seg, "__MACOSX", 8));
}

/* Opens the ZIP at `path` and indexes its files under the pack's root: the archive's top when
 * textures.ini is there, else its one top-level folder when every file is in it and textures.ini is
 * in that folder (a pack zipped with its folder). Entries starting with '.' or in __MACOSX at the top
 * are not counted when looking for that folder. A ZIP with no textures.ini in either place is
 * refused, as PPSSPP refuses a ZIP pack without one. */
static int open_zip(NkTexRepl *p, const char *path, char *err, size_t err_cap) {
    NkZipReadOptions opt;
    NkZipError ze;
    memset(&opt, 0, sizeof(opt));
    opt.max_entries = NK_ZIP_READ_MAX_ENTRIES;
    opt.max_total_bytes = NK_TEXREPL_MAX_PACK_BYTES;   /* what all its entries declare, unpacked */
    opt.deflate = true;
    opt.data_descriptor = true;
    NkZipStatus st = nk_zip_reader_open_file_with(&p->zip, path, &opt, &ze);
    if (st != NK_ZIP_OK) {
        snprintf(err, err_cap, "texture pack ZIP refused (%s): %s%s%s", nk_zip_status_code(st),
                 nk_zip_status_message(st), ze.entry[0] ? " Entry: " : "", ze.entry);
        p->zip = NULL;
        return 0;
    }
    size_t n = nk_zip_reader_count(p->zip);
    size_t root_len = 0;
    int ini_at_top = 0, top_ok = 1;
    const char *top = NULL;
    size_t top_len = 0;
    for (size_t i = 0; i < n; i++) {
        NkZipEntryInfo e;
        if (!nk_zip_reader_entry(p->zip, i, &e)) continue;
        if (!e.is_folder && ci_equal(e.name, "textures.ini")) ini_at_top = 1;
        const char *slash = strchr(e.name, '/');
        size_t seg = slash ? (size_t)(slash - e.name) : strlen(e.name);
        if (name_is_dot_or_mac(e.name, seg)) continue;
        if (!slash) { top_ok = 0; continue; }   /* a file at the top */
        if (!top) { top = e.name; top_len = seg; }
        else if (seg != top_len || strncmp(top, e.name, seg) != 0) top_ok = 0;
    }
    if (!ini_at_top) {
        int ini_in_top = 0;
        if (top_ok && top) {
            for (size_t i = 0; i < n && !ini_in_top; i++) {
                NkZipEntryInfo e;
                if (nk_zip_reader_entry(p->zip, i, &e) && !e.is_folder && strlen(e.name) > top_len &&
                    !strncmp(e.name, top, top_len) && e.name[top_len] == '/' && ci_equal(e.name + top_len + 1, "textures.ini"))
                    ini_in_top = 1;
            }
        }
        if (!ini_in_top) {
            snprintf(err, err_cap, "texture pack ZIP has no textures.ini at its top or in its one top-level folder "
                                   "(a ZIP pack needs one, as in PPSSPP)");
            return 0;
        }
        root_len = top_len + 1;
    }
    p->zip_names = (ZipName *)calloc(n ? n : 1u, sizeof(ZipName));
    if (!p->zip_names) { snprintf(err, err_cap, "out of memory"); return 0; }
    for (size_t i = 0; i < n; i++) {
        NkZipEntryInfo e;
        if (!nk_zip_reader_entry(p->zip, i, &e) || e.is_folder || strlen(e.name) <= root_len) continue;
        if (root_len && strncmp(e.name, top, root_len) != 0) continue;
        ZipName *z = &p->zip_names[p->zip_name_n++];
        z->rel = e.name + root_len;
        z->index = i;
        z->size = e.size;
        z->deflated = e.deflated;
        z->packed = e.packed_size;
    }
    nk_zip_sort(p->zip_names, p->zip_name_n, sizeof(ZipName), zip_name_cmp);
    for (size_t i = 0; i < p->zip_name_n; i++) {
        if (strchr(p->zip_names[i].rel, '/')) continue;   /* only the root's files are scanned */
        if (!scan_name(p, p->zip_names[i].rel)) { snprintf(err, err_cap, "out of memory"); return 0; }
    }
    return 1;
}

static uint64_t index_bytes(const NkTexRepl *p) {
    uint64_t b = sizeof(*p) + (uint64_t)p->alias_n * sizeof(Alias) + (uint64_t)p->zip_name_n * sizeof(ZipName) +
                 (uint64_t)p->range_cap * sizeof(HashRange) + (uint64_t)p->reduce_cap * sizeof(ReduceRange);
    for (size_t i = 0; i < p->alias_n; i++) b += (uint64_t)p->aliases[i].level_n * sizeof(Level);
    b += (uint64_t)p->image_n * sizeof(Image);
    for (size_t i = 0; i < p->image_n; i++) b += strlen(p->images[i].file) + 1u;
    return b;
}

/* The last component of a pack's path, for messages: a full path would show the user's folders. */
static const char *last_component(const char *path, char *buf, size_t cap) {
    size_t n = strlen(path);
    while (n > 1 && (path[n - 1] == '/' || path[n - 1] == '\\')) n--;
    size_t b = n;
    while (b > 0 && path[b - 1] != '/' && path[b - 1] != '\\' && path[b - 1] != ':') b--;
    snprintf(buf, cap, "%.*s", (int)(n - b), path + b);
    return buf;
}

static const char *read_reason(PackRead why) {
    switch (why) {
    case PACK_READ_TOO_LARGE: return "it is larger than the cap";
    case PACK_READ_DAMAGED: return "it could not be read whole";
    case PACK_READ_NOT_REGULAR: return "it is not a regular file (a device, pipe or folder)";
    case PACK_READ_OUTSIDE: return "it resolves outside the pack folder (a link or junction)";
    default: return "it is missing or cannot be opened";
    }
}

static NkTexRepl *open_fail(NkTexRepl *p) {
    nk_texrepl_close(p);
    return NULL;
}

static NkTexRepl *texrepl_open(const char *path, uint64_t budget_bytes, char *err, size_t err_cap) {
    char name[128];
    int is_dir = path && *path && nk_platform_dir_exists(path);
    if (!is_dir && !(path && *path && nk_platform_file_exists(path))) {
        snprintf(err, err_cap, "texture pack '%s' does not exist (a folder or a .zip file)",
                 path ? last_component(path, name, sizeof(name)) : "");
        return NULL;
    }
    NkTexRepl *p = (NkTexRepl *)calloc(1, sizeof(*p));
    if (!p) { snprintf(err, err_cap, "out of memory"); return NULL; }
    p->budget = budget_bytes ? budget_bytes : (uint64_t)NK_TEXREPL_DEFAULT_BUDGET_MB << 20;
    p->reduce_global = 0.5f;
    uint64_t t0 = now_ns();

    char zip_path[1024];
    zip_path[0] = '\0';
    if (is_dir) {
        p->dir = dup_str(path);
        if (!p->dir) { snprintf(err, err_cap, "out of memory"); return open_fail(p); }
        size_t dn = strlen(p->dir);
        while (dn > 1 && (p->dir[dn - 1] == '/' || p->dir[dn - 1] == '\\')) p->dir[--dn] = '\0';
        /* PPSSPP reads a textures.zip in the pack folder in place of the folder's own files. */
        if (snprintf(zip_path, sizeof(zip_path), "%s/textures.zip", p->dir) >= (int)sizeof(zip_path)) {
            snprintf(err, err_cap, "texture pack folder path is too long");
            return open_fail(p);
        }
        if (!root_resolve(p)) {
            snprintf(err, err_cap, "texture pack folder '%s' cannot be resolved to its final path",
                     last_component(path, name, sizeof(name)));
            return open_fail(p);
        }
        if (!nk_platform_file_exists(zip_path)) zip_path[0] = '\0';
    } else if (snprintf(zip_path, sizeof(zip_path), "%s", path) >= (int)sizeof(zip_path)) {
        snprintf(err, err_cap, "texture pack path is too long");
        return open_fail(p);
    }

    if (zip_path[0]) {
        /* The ZIP's handle is checked as a folder file's is (a regular file, inside the folder when it
         * is the folder's textures.zip) before the ZIP reader opens it. */
        size_t zsize = 0;
        PackRead zwhy;
        read_regular(p, zip_path, (size_t)-1, &zsize, &zwhy, 1, NULL, NULL);
        if (zwhy != PACK_READ_OK) {
            snprintf(err, err_cap, "texture pack ZIP '%s' refused: %s",
                     is_dir ? "textures.zip" : last_component(path, name, sizeof(name)), read_reason(zwhy));
            return open_fail(p);
        }
        const uint64_t tl = now_ns();
        if (!open_zip(p, zip_path, err, err_cap)) return open_fail(p);
        p->stats.list_ns = now_ns() - tl;
        if (is_dir) diag_add(p, "textures.zip read in place of the folder's own files, as PPSSPP reads it");
    } else {
        ScanCtx sc = { p, 1 };
        const uint64_t tl = now_ns();
        if (!nk_platform_list_files(p->dir, scan_file, &sc) || !sc.ok) {
            snprintf(err, err_cap, "texture pack folder '%s' cannot be read", last_component(path, name, sizeof(name)));
            return open_fail(p);
        }
        p->stats.list_ns = now_ns() - tl;
    }

    size_t size = 0;
    PackRead why;
    char zip_why[40];
    unsigned char *text = pack_read(p, "textures.ini", 4u << 20, &size, &why, zip_why, sizeof(zip_why), NULL, NULL, NULL);
    if (!text && why != PACK_READ_MISSING) {
        snprintf(err, err_cap, "textures.ini cannot be read: %s%s%s",
                 why == PACK_READ_TOO_LARGE ? "it is larger than 4 MiB" : read_reason(why), zip_why[0] ? ", " : "",
                 zip_why);
        return open_fail(p);
    }
    if (text) {
        char *z = (char *)realloc(text, size + 1);
        if (!z) { free(text); snprintf(err, err_cap, "out of memory"); return open_fail(p); }
        z[size] = '\0';
        const char *nul = (const char *)memchr(z, 0, size);
        if (nul) {   /* the text after it would go unread without a word */
            int line = 1;
            for (const char *c = z; c < nul; c++) line += *c == '\n';
            free(z);
            snprintf(err, err_cap, "textures.ini line %d holds a NUL byte, which no text file has", line);
            return open_fail(p);
        }
        int ok = parse_ini(p, z, err, err_cap);
        free(z);
        if (!ok) return open_fail(p);
    }
    if (!aliases_build(p) || !images_build(p)) { snprintf(err, err_cap, "out of memory"); return open_fail(p); }
    indexed_finish(p);
    ranges_finish(p);
    if (p->nonpng_listed)
        diag_add(p, "%zu hash-named DDS, KTX2 or ZIM files listed; only PNG replacements are decoded in this version",
                 p->nonpng_listed);
    p->stats.open_ns = now_ns() - t0;
    p->stats.index_bytes = index_bytes(p);
    return p;
}

NkTexRepl *nk_texrepl_open(const char *path, uint64_t budget_bytes, char *err, size_t err_cap) {
    char ebuf[256];
    if (!err || !err_cap) { err = ebuf; err_cap = sizeof(ebuf); }
    err[0] = '\0';
    NkTexRepl *p = texrepl_open(path, budget_bytes, err, err_cap);
    text_clean(err);   /* the reason quotes the pack's own text */
    return p;
}

void nk_texrepl_close(NkTexRepl *p) {
    if (!p) return;
    for (size_t i = 0; i < p->alias_n; i++) {
        if (!p->images)   /* before images_build the levels own their names */
            for (uint32_t l = 0; l < p->aliases[i].level_n; l++) free(p->aliases[i].levels[l].file);
        free(p->aliases[i].levels);
    }
    for (size_t i = 0; i < p->image_n; i++) {
        free(p->images[i].file);
        free(p->images[i].rgba);
    }
    free(p->images);
    free(p->id_key);
    free(p->id_row);
    records_free(p);
    for (size_t i = 0; i < p->diag_n; i++) free(p->diag[i]);
    free(p->aliases);
    free(p->ranges);
    free(p->reduce);
    free(p->games);
    free(p->zip_names);
    free(p->indexed);
    nk_zip_reader_close(p->zip);
    free(p->root_real);
    free(p->dir);
    free(p);
}

/* ----------------------------------------------------------------------------- hashing */

static int find_range(const NkTexRepl *p, uint32_t addr, int w, int h, int *nw, int *nh) {
    size_t lo = 0, hi = p->range_n;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const int c = range_key_cmp(&p->ranges[mid], addr, (uint32_t)w, (uint32_t)h);
        if (c == 0) {
            *nw = (int)p->ranges[mid].new_w;
            *nh = (int)p->ranges[mid].new_h;
            return 1;
        }
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    *nw = w;
    *nh = h;
    return 0;
}

static float reduce_for(const NkTexRepl *p, int w, int h) {
    size_t lo = 0, hi = p->reduce_n;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const int c = reduce_key_cmp(&p->reduce[mid], (uint32_t)w, (uint32_t)h);
        if (c == 0) return p->reduce[mid].value;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return p->reduce_global;
}

static uint32_t hash_bytes(const NkTexRepl *p, const uint8_t *d, uint32_t n, uint32_t guest_addr) {
    switch (p->hash_kind) {
    case NK_TEXREPL_HASH_QUICK: return nk_texrepl_quick_hash(d, n, guest_addr);
    case NK_TEXREPL_HASH_XXH32: return nk_texrepl_xxh32(d, n, 0xBACD7814u);
    case NK_TEXREPL_HASH_XXH64: return (uint32_t)nk_texrepl_xxh64(d, n, 0xBACD7814u);
    }
    return 0;
}

uint32_t nk_texrepl_hash_texture(const NkTexRepl *p, const uint8_t *data, size_t avail, uint32_t guest_addr,
                                 int bufw, int w, int h, uint32_t format, int max_seen_v) {
    if (!p || !data || w <= 0 || h <= 0 || bufw <= 0 || format > 10) return 0;
    if (!find_range(p, guest_addr, w, h, &w, &h)) {
        if (h == 512 && max_seen_v < 512 && max_seen_v != 0) h = max_seen_v;
    }
    float reduce = 1.0f;
    if (p->reduce_hash) {
        reduce = reduce_for(p, w, h);
        if (reduce < 0.0f) reduce = 0.0f;
        if (reduce > 1.0f) reduce = 1.0f;
    }
    const uint64_t bits = nk_texrepl_bits_per_texel(format);
    if (bufw <= w) {
        /* Contiguous: the pixels used. */
        const uint64_t total = (uint64_t)bufw * (uint64_t)h + (uint64_t)(w - bufw);
        uint32_t size = (uint32_t)((float)((bits * total) / 8u) * reduce);
        if (p->skip_dxt1_128x64 && format == 8 && w == 128 && h == 64) size -= 8u * (uint32_t)p->skip_dxt1_128x64;
        if (p->skip_dxt1_128x128 && format == 8 && w == 128 && h == 128) size -= 8u * (uint32_t)p->skip_dxt1_128x128;
        if (size > avail) return 0;
        return hash_bytes(p, data, size, guest_addr);
    }
    /* Rows with gaps: hash each row's texels and fold. */
    const uint32_t per_line = (uint32_t)((float)((bits * (uint64_t)w) / 8u) * reduce);
    const uint64_t stride = (bits * (uint64_t)bufw) / 8u;
    const uint64_t span = (uint64_t)(h - 1) * stride + per_line;
    if (span > avail) return 0;
    uint32_t result = 0;
    for (int y = 0; y < h; y++) {
        uint32_t row = hash_bytes(p, data + (uint64_t)y * stride, per_line, guest_addr);
        result = (result * 11u) ^ row;
    }
    return result;
}

/* ------------------------------------------------------------------------------ lookup */

/* PPSSPP's LookupWildcard order over a sorted key table, the key with its address zeroed first
 * under ignoreAddress. */
static const void *wildcard(const NkTexRepl *p, const void *base, size_t n, size_t stride, const NkTexReplId *id) {
    uint64_t ck = nk_texrepl_cache_key(id->addr, id->format, id->dim, id->clut_hash);
    uint32_t hash = id->hash;
    if (p->ignore_address) ck &= 0xFFFFFFFFull;
    const void *a;
    if (!n) return NULL;
    if ((a = key_find(base, n, stride, ck, hash))) return a;
    if ((a = key_find(base, n, stride, ck & 0xFFFFFFFFull, 0))) return a;              /* palette hash only */
    if (!p->ignore_address && (a = key_find(base, n, stride, ck, 0))) return a;         /* no data hash */
    if ((a = key_find(base, n, stride, ck & 0xFFFFFFFFull, hash))) return a;            /* no address */
    if (!p->ignore_address) {
        if ((a = key_find(base, n, stride, ck & ~0xFFFFFFFFull, hash))) return a;       /* address, not palette */
        if ((a = key_find(base, n, stride, ck & ~0xFFFFFFFFull, 0))) return a;          /* address only */
    }
    return key_find(base, n, stride, 0, hash);                                          /* the data hash alone */
}

static Alias *resolve(const NkTexRepl *p, const NkTexReplId *id) {
    return (Alias *)(uintptr_t)wildcard(p, p->aliases, p->alias_n, sizeof(Alias), id);
}

/* A texture [indexed] marks: its image holds palette indices, which this build does not draw. */
static int is_indexed(const NkTexRepl *p, const NkTexReplId *id) {
    const IndexedKey *k = (const IndexedKey *)wildcard(p, p->indexed, p->indexed_n, sizeof(IndexedKey), id);
    return k && k->mode != 0;
}

/* The held images are kept in a list, most recently used first, so that eviction takes the least
 * recently used one without looking at every image the pack names. */
static void lru_unlink(NkTexRepl *p, Image *im) {
    if (im->lru_prev) p->images[im->lru_prev - 1u].lru_next = im->lru_next;
    else p->lru_head = im->lru_next;
    if (im->lru_next) p->images[im->lru_next - 1u].lru_prev = im->lru_prev;
    else p->lru_tail = im->lru_prev;
    im->lru_prev = im->lru_next = 0;
}

static void lru_push_front(NkTexRepl *p, Image *im) {
    const uint32_t row = (uint32_t)(im - p->images) + 1u;
    im->lru_prev = 0;
    im->lru_next = p->lru_head;
    if (p->lru_head) p->images[p->lru_head - 1u].lru_prev = row;
    else p->lru_tail = row;
    p->lru_head = row;
}

/* Drops the least recently used images nobody holds a pin on until `need` more bytes fit, or none
 * is left to drop. Each step looks at one held image. */
static void evict_for(NkTexRepl *p, uint64_t need) {
    uint32_t at = p->lru_tail;
    while (at && p->used + need > p->budget) {
        Image *v = &p->images[at - 1u];
        at = v->lru_prev;
        if (v->pins) continue;
        lru_unlink(p, v);
        free(v->rgba);
        v->rgba = NULL;
        v->state = 3;   /* its size stays known */
        p->used -= (uint64_t)v->w * (uint64_t)v->h * 4u;
        p->stats.evicted++;
    }
}

static uint32_t png_be32(const unsigned char *b) {
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | (uint32_t)b[3];
}

/* What png_check found: the header, where PLTE and tRNS are, and the image data's chunks. */
typedef struct {
    int w, h;
    unsigned depth, color, interlace, channels;
    const unsigned char *plte, *trns;
    uint32_t plte_len, trns_len;
    const unsigned char *idat;   /* the first IDAT's data */
    size_t idat_total;           /* all IDAT data, which follow one another */
    size_t idat_count;
    size_t raw_size;             /* what the image data must unpack to: every row with its filter byte */
} PngInfo;

static const unsigned char kAdam7[7][4] = {   /* x0, y0, dx, dy */
    { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 }, { 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 },
};

static size_t png_row_bytes(const PngInfo *g, uint32_t w) {
    return ((size_t)w * g->depth * g->channels + 7u) / 8u;
}

static uint32_t png_pass_extent(uint32_t full, unsigned origin, unsigned step) {
    return full > origin ? (full - origin + step - 1u) / step : 0u;
}

/* The unpacked size the header implies: each row of each pass with its filter byte. */
static size_t png_raw_size(const PngInfo *g) {
    if (!g->interlace) return (size_t)g->h * (1u + png_row_bytes(g, (uint32_t)g->w));
    size_t total = 0;
    for (int k = 0; k < 7; k++) {
        const uint32_t pw = png_pass_extent((uint32_t)g->w, kAdam7[k][0], kAdam7[k][2]);
        const uint32_t ph = png_pass_extent((uint32_t)g->h, kAdam7[k][1], kAdam7[k][3]);
        if (pw && ph) total += (size_t)ph * (1u + png_row_bytes(g, pw));
    }
    return total;
}

/* PNG bytes checked before anything is allocated for them: the signature; IHDR first, once, with a
 * size of 1 to NK_TEXREPL_MAX_DIM a side and a pixel format the PNG standard defines; every chunk
 * whole with its CRC right; PLTE (required for a palette image, refused for a grey one) and tRNS in
 * their place and of a length their format allows; the IDAT chunks one after another; IEND. Bytes
 * after IEND are ignored, as decoders ignore them. The image data's unpacked size is fixed here
 * (raw_size), and png_decode unpacks it with that as the hard limit. */
static int png_check(const unsigned char *d, size_t n, PngInfo *g, char *why, size_t why_cap) {
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    memset(g, 0, sizeof(*g));
    if (n < 8 || memcmp(d, sig, 8) != 0) { snprintf(why, why_cap, "no PNG signature"); return 0; }
    size_t at = 8;
    int chunks = 0, idat_done = 0, plte_seen = 0;
    while (at < n) {
        if (n - at < 12) { snprintf(why, why_cap, "cut short in a chunk header"); return 0; }
        uint32_t len = png_be32(d + at);
        const unsigned char *type = d + at + 4, *c = type + 4;
        if (len > 0x7fffffffu || len > n - at - 12) { snprintf(why, why_cap, "cut short in a chunk"); return 0; }
        for (int k = 0; k < 4; k++) {
            if (!((type[k] >= 'A' && type[k] <= 'Z') || (type[k] >= 'a' && type[k] <= 'z'))) {
                snprintf(why, why_cap, "a chunk type is not four letters");
                return 0;
            }
        }
        if (nk_zip_crc32(0, type, (size_t)len + 4u) != png_be32(type + 4 + len)) {
            snprintf(why, why_cap, "chunk %.4s has a wrong CRC", (const char *)type);
            return 0;
        }
        const int is_ihdr = !memcmp(type, "IHDR", 4), is_idat = !memcmp(type, "IDAT", 4);
        if ((chunks == 0) != is_ihdr) { snprintf(why, why_cap, "IHDR is not the first chunk, or not the only one"); return 0; }
        if (g->idat_count && !is_idat) idat_done = 1;
        if (is_ihdr) {
            if (len != 13) { snprintf(why, why_cap, "IHDR is not 13 bytes"); return 0; }
            uint32_t pw = png_be32(c), ph = png_be32(c + 4);
            if (pw == 0 || ph == 0 || pw > NK_TEXREPL_MAX_DIM || ph > NK_TEXREPL_MAX_DIM) {
                snprintf(why, why_cap, "%lux%lu is not 1 to %d texels a side", (unsigned long)pw, (unsigned long)ph,
                         NK_TEXREPL_MAX_DIM);
                return 0;
            }
            const unsigned depth = c[8], color = c[9];
            const int ok = (color == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) ||
                           (color == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8)) ||
                           ((color == 2 || color == 4 || color == 6) && (depth == 8 || depth == 16));
            if (!ok || c[10] != 0 || c[11] != 0 || c[12] > 1) {
                snprintf(why, why_cap, "IHDR names no PNG pixel format");
                return 0;
            }
            g->w = (int)pw;
            g->h = (int)ph;
            g->depth = depth;
            g->color = color;
            g->interlace = c[12];
            g->channels = color == 2 ? 3u : color == 4 ? 2u : color == 6 ? 4u : 1u;
        } else if (!memcmp(type, "PLTE", 4)) {
            if (plte_seen || g->trns || g->idat_count || len == 0 || len % 3u || len / 3u > 256u) {
                snprintf(why, why_cap, "PLTE is out of place or of a wrong length");
                return 0;
            }
            /* A second PLTE is refused, as libpng refuses it. A grey image's PLTE is ignored, as libpng
             * ignores it. A palette longer than the depth can index keeps its entries, as stb_image
             * keeps them (a pixel can name only the first 2^depth), and so does a tRNS that long;
             * libpng cuts the palette to 2^depth and then drops such a tRNS whole. */
            plte_seen = 1;
            if (g->color != 0 && g->color != 4) {
                g->plte = c;
                g->plte_len = len;
            }
        } else if (!memcmp(type, "tRNS", 4)) {
            const int fits = g->color == 3 ? (g->plte && len >= 1 && len <= g->plte_len / 3u)
                             : g->color == 0 ? len == 2
                             : g->color == 2 ? len == 6
                                             : 0;
            if (g->trns || g->idat_count || !fits) {
                snprintf(why, why_cap, "tRNS is out of place or of a wrong length");
                return 0;
            }
            if (g->color != 3) {   /* each key sample within the image's depth */
                for (uint32_t k = 0; k < len; k += 2) {
                    if (g->depth < 16 && (((uint32_t)c[k] << 8) | c[k + 1]) >> g->depth) {
                        snprintf(why, why_cap, "tRNS names a sample the image cannot hold");
                        return 0;
                    }
                }
            }
            g->trns = c;
            g->trns_len = len;
        } else if (is_idat) {
            if (idat_done) { snprintf(why, why_cap, "the IDAT chunks do not follow one another"); return 0; }
            if (g->color == 3 && !g->plte) { snprintf(why, why_cap, "a palette image with no PLTE before its data"); return 0; }
            if (!g->idat_count) g->idat = c;
            g->idat_count++;
            g->idat_total += len;
        } else if (!(type[0] & 0x20u) && memcmp(type, "IEND", 4) != 0) {
            /* An unknown chunk whose ancillary bit is 0: PNG says it cannot be skipped safely. */
            snprintf(why, why_cap, "chunk %.4s is critical and not known here", (const char *)type);
            return 0;
        }
        chunks++;
        at += (size_t)len + 12u;
        if (!memcmp(type, "IEND", 4)) {
            if (!g->idat_count) { snprintf(why, why_cap, "no IDAT before IEND"); return 0; }
            g->raw_size = png_raw_size(g);
            /* The image data may take what DEFLATE needs at worst for raw_size bytes and no more: the
             * ZIP rule (an eighth over, plus 64 bytes; every literal of a fixed block is at most 9
             * bits, a stored block costs 5 bytes in 65,535) and the zlib header and Adler-32. More is
             * padding (empty blocks) the inflater would have to work through for nothing. */
            if (g->idat_total > g->raw_size + g->raw_size / 8u + 64u + 6u) {
                snprintf(why, why_cap, "its image data is %llu bytes, more than %llu can need",
                         (unsigned long long)g->idat_total, (unsigned long long)g->raw_size);
                return 0;
            }
            return 1;
        }
    }
    snprintf(why, why_cap, "no IEND");
    return 0;
}

static uint32_t png_adler32(const unsigned char *d, size_t n) {
    uint32_t a = 1, b = 0;
    while (n) {
        size_t run = n > 5552u ? 5552u : n;   /* the most bytes before b can pass 2^32 */
        n -= run;
        while (run--) { a += *d++; b += a; }
        a %= 65521u;
        b %= 65521u;
    }
    return b << 16 | a;
}

static unsigned png_paeth(unsigned a, unsigned b, unsigned c) {
    const int p = (int)a + (int)b - (int)c;
    const int pa = abs(p - (int)a), pb = abs(p - (int)b), pc = abs(p - (int)c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* One sample of a row, at the image's depth. */
static unsigned png_sample(const unsigned char *row, size_t i, unsigned depth) {
    switch (depth) {
    case 16: return (unsigned)row[2 * i] << 8 | row[2 * i + 1];
    case 8: return row[i];
    default: {
        const size_t bit = i * depth;
        return (row[bit / 8u] >> (8u - depth - (unsigned)(bit % 8u))) & ((1u << depth) - 1u);
    }
    }
}

/* Decodes PNG bytes png_check passed, to R, G, B, A (malloc'd). The image data is unpacked by the
 * project's own inflater with g->raw_size as its hard limit, so a stream that unpacks to more is
 * refused after at most that many bytes, whatever its length; one that unpacks to less, or whose
 * zlib header or Adler-32 is wrong, is refused too. `*scratch` is the most memory the decode held
 * at once (the joined image data and the unpacked rows, then the rows and the result), set whether
 * the image is decoded or refused. */
static uint32_t *png_decode(const unsigned char *d, size_t n, const PngInfo *g, uint64_t *scratch, char *why,
                            size_t why_cap) {
    (void)d;
    (void)n;
    *scratch = 0;
    const unsigned char *z = g->idat;
    unsigned char *joined = NULL;
    size_t joined_bytes = 0;
    if (g->idat_count > 1) {   /* the chunks follow one another: copy each one's data out */
        joined = (unsigned char *)malloc(g->idat_total);
        if (!joined) { snprintf(why, why_cap, "out of memory"); return NULL; }
        size_t got = 0;
        const unsigned char *c = g->idat - 8;
        for (size_t k = 0; k < g->idat_count; k++) {
            const uint32_t len = png_be32(c);
            memcpy(joined + got, c + 8, len);
            got += len;
            c += (size_t)len + 12u;
        }
        z = joined;
        joined_bytes = g->idat_total;
    }
    const size_t zn = g->idat_total;
    if (zn < 6 || (z[0] & 0x0fu) != 8u || (z[0] >> 4) > 7u || ((unsigned)z[0] << 8 | z[1]) % 31u || (z[1] & 0x20u)) {
        free(joined);
        snprintf(why, why_cap, "its image data has no zlib header PNG allows");
        return NULL;
    }
    /* The rows are unpacked into a buffer of exactly raw_size, the inflater's hard limit: nothing
     * else is allocated for them, whatever the stream holds. */
    uint8_t *raw = (uint8_t *)malloc(g->raw_size);
    if (!raw) { free(joined); snprintf(why, why_cap, "out of memory"); return NULL; }
    *scratch = (uint64_t)joined_bytes + (uint64_t)g->raw_size;
    size_t raw_n = 0, used = 0;
    char inflate_why[96] = "";
    const int rc = nk_psp_inflate_raw_into(z + 2, zn - 2, raw, g->raw_size, &raw_n, &used, inflate_why,
                                           sizeof(inflate_why));
    /* The Adler-32 follows the stream's last byte, and ends the image data. */
    const int trailer_ok = rc == NK_INFLATE_OK && zn - 2 >= 4u && used == zn - 2 - 4u;
    const uint32_t adler = trailer_ok ? png_be32(z + 2 + used) : 0u;
    free(joined);
    if (rc != NK_INFLATE_OK || raw_n != g->raw_size || !trailer_ok) {
        free(raw);
        /* The inflater's reason, without its "inflate: " prefix, names a refusal of a valid
         * stream's shape (too many dynamic blocks) apart from damage. */
        const char *reason = strncmp(inflate_why, "inflate: ", 9) == 0 ? inflate_why + 9 : inflate_why;
        if (rc != NK_INFLATE_OK && rc != NK_INFLATE_ERR_OVERFLOW && rc != NK_INFLATE_ERR_MEMORY && reason[0]) {
            snprintf(why, why_cap, "its image data does not unpack (%s)", reason);
            return NULL;
        }
        snprintf(why, why_cap, "%s",
                 rc == NK_INFLATE_ERR_OVERFLOW ? "its image data unpacks to more than the image"
                 : rc == NK_INFLATE_ERR_MEMORY ? "out of memory"
                 : rc != NK_INFLATE_OK         ? "its image data does not unpack"
                 : raw_n != g->raw_size        ? "its image data unpacks to less than the image"
                 : used + 4u > zn - 2          ? "its image data ends before its Adler-32"
                                               : "its image data has bytes after its Adler-32");
        return NULL;
    }
    if (png_adler32(raw, raw_n) != adler) {
        free(raw);
        snprintf(why, why_cap, "its image data's Adler-32 is wrong");
        return NULL;
    }
    const size_t out_bytes = (size_t)g->w * (size_t)g->h * 4u;
    unsigned char *out = (unsigned char *)malloc(out_bytes);
    if (!out) { free(raw); snprintf(why, why_cap, "out of memory"); return NULL; }
    /* Now the rows and the result are held. */
    if ((uint64_t)raw_n + out_bytes > *scratch) *scratch = (uint64_t)raw_n + out_bytes;
    const size_t bpp = (g->depth * g->channels + 7u) / 8u;   /* bytes back the filters look */
    const unsigned max = (1u << (g->depth < 16 ? g->depth : 16u)) - 1u;
    unsigned char *row = raw;
    for (int k = 0; k < (g->interlace ? 7 : 1); k++) {
        const unsigned x0 = g->interlace ? kAdam7[k][0] : 0u, y0 = g->interlace ? kAdam7[k][1] : 0u;
        const unsigned dx = g->interlace ? kAdam7[k][2] : 1u, dy = g->interlace ? kAdam7[k][3] : 1u;
        const uint32_t pw = png_pass_extent((uint32_t)g->w, x0, dx), ph = png_pass_extent((uint32_t)g->h, y0, dy);
        if (!pw || !ph) continue;
        const size_t rb = png_row_bytes(g, pw);
        const unsigned char *prev = NULL;
        for (uint32_t y = 0; y < ph; y++) {
            const unsigned f = row[0];
            unsigned char *s = row + 1;
            if (f > 4) {
                free(raw);
                free(out);
                snprintf(why, why_cap, "a row has filter type %u", f);
                return NULL;
            }
            for (size_t i = 0; i < rb; i++) {   /* undo the row's filter in place */
                const unsigned a = i >= bpp ? s[i - bpp] : 0u, b = prev ? prev[i] : 0u;
                const unsigned c = (prev && i >= bpp) ? prev[i - bpp] : 0u;
                const unsigned pred = f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2u : f == 4 ? png_paeth(a, b, c) : 0u;
                s[i] = (unsigned char)(s[i] + pred);
            }
            for (uint32_t x = 0; x < pw; x++) {
                unsigned char *o = out + (((size_t)(y0 + y * dy) * (size_t)g->w) + x0 + (size_t)x * dx) * 4u;
                unsigned v[4] = { 0, 0, 0, 0 };
                for (unsigned ch = 0; ch < g->channels; ch++) v[ch] = png_sample(s, (size_t)x * g->channels + ch, g->depth);
                unsigned alpha = 255u;
                if (g->color == 3) {
                    if (v[0] >= g->plte_len / 3u) {
                        free(raw);
                        free(out);
                        snprintf(why, why_cap, "a pixel names palette entry %u, which PLTE does not have", v[0]);
                        return NULL;
                    }
                    o[0] = g->plte[3 * v[0]];
                    o[1] = g->plte[3 * v[0] + 1];
                    o[2] = g->plte[3 * v[0] + 2];
                    o[3] = (g->trns && v[0] < g->trns_len) ? g->trns[v[0]] : 255u;
                    continue;
                }
                if (g->trns) {   /* the one colour tRNS names is transparent */
                    int same = 1;
                    for (unsigned ch = 0; ch < g->channels; ch++)
                        if (v[ch] != ((unsigned)g->trns[2 * ch] << 8 | g->trns[2 * ch + 1])) same = 0;
                    if (same) alpha = 0u;
                }
                /* To 8 bits as stb_image converts: 16-bit samples by their high byte, 1, 2 and 4 bits
                 * scaled up to 255. */
                for (unsigned ch = 0; ch < g->channels; ch++)
                    v[ch] = g->depth == 16 ? v[ch] >> 8 : g->depth == 8 ? v[ch] : v[ch] * (255u / max);
                if (g->color == 0) { o[0] = o[1] = o[2] = (unsigned char)v[0]; o[3] = (unsigned char)alpha; }
                else if (g->color == 4) { o[0] = o[1] = o[2] = (unsigned char)v[0]; o[3] = (unsigned char)v[1]; }
                else if (g->color == 2) { o[0] = (unsigned char)v[0]; o[1] = (unsigned char)v[1]; o[2] = (unsigned char)v[2]; o[3] = (unsigned char)alpha; }
                else { o[0] = (unsigned char)v[0]; o[1] = (unsigned char)v[1]; o[2] = (unsigned char)v[2]; o[3] = (unsigned char)v[3]; }
            }
            prev = s;
            row += 1u + rb;
        }
    }
    free(raw);
    return (uint32_t *)(void *)out;
}

int nk_texrepl_decode_png(const void *data, size_t size, uint64_t max_bytes, uint32_t **rgba, int *w, int *h,
                          char *why, size_t why_cap) {
    char wbuf[96];
    if (!why || !why_cap) { why = wbuf; why_cap = sizeof(wbuf); }
    why[0] = '\0';
    if (!rgba || !w || !h) { snprintf(why, why_cap, "no place for the image"); return 0; }
    *rgba = NULL;
    *w = *h = 0;
    PngInfo g;
    if (!data || !png_check((const unsigned char *)data, size, &g, why, why_cap)) return 0;
    const uint64_t bytes = (uint64_t)g.w * (uint64_t)g.h * 4u;
    if (max_bytes && bytes > max_bytes) {
        snprintf(why, why_cap, "%dx%d needs %llu KiB, more than the %llu KiB allowed", g.w, g.h,
                 (unsigned long long)(bytes >> 10), (unsigned long long)(max_bytes >> 10));
        return 0;
    }
    uint64_t scratch = 0;
    uint32_t *px = png_decode((const unsigned char *)data, size, &g, &scratch, why, why_cap);
    if (!px) return 0;
    *rgba = px;
    *w = g.w;
    *h = g.h;
    return 1;
}

/* A level that cannot be used is skipped from then on, and counted once in stats.skipped. */
static int skip_level(NkTexRepl *p, Level *v) {
    if (v->state != 2) p->stats.skipped++;
    v->state = 2;
    return 0;
}

/* An image that cannot be used: noted once, and every level naming it is skipped. */
static int skip_image(NkTexRepl *p, Image *im) {
    im->state = 2;
    p->stats.load_failed++;
    return 0;
}

/* Reads an image's file and, with `decode`, its pixels, unless its size turns out not to be
 * want_w x want_h (0: any size), which the level asking checks. Returns 1 when the image is usable
 * as asked (decoded, or with `decode` 0 its size known); 0 otherwise, with state 2 (and a note) when
 * it can never be used, or state 3 when only the budget is full now or the size is not the one wanted. */
static int image_load(NkTexRepl *p, Image *im, int decode, int want_w, int want_h) {
    if (im->state == 2) return 0;
    if (im->state == 1 || (im->state == 3 && !decode)) return 1;
    if (im->state == 3 && want_w && (im->w != want_w || im->h != want_h)) return 0;
    if (im->state == 3 && im->wait) {
        /* Evicted and wanted again soon after, more than once: the budget cannot hold what is being
         * drawn. Each further decode waits twice as many lookups as the one before (at most 65,535),
         * so an image evicted on every use is decoded about log2 of its lookups times, not on each. */
        im->wait--;
        p->stats.deferred++;
        return 0;
    }
    if (im->state == 3) {   /* its size is known: make room before reading the file again */
        const uint64_t known = (uint64_t)im->w * (uint64_t)im->h * 4u;
        evict_for(p, known);
        if (p->used + known > p->budget) { p->stats.over_budget++; return 0; }
    }
    if (!path_inside_pack(im->file)) return skip_image(p, im);
    if (!ends_with_ci(im->file, ".png")) {
        diag_add(p, "%s: only PNG replacements are read in this version; not used", im->file);
        return skip_image(p, im);
    }
    /* The ZIP reader's name rules, for a folder's file too: no device name (CON, COM1), no trailing
     * dot or space, no character Windows refuses. */
    char norm[NK_ZIP_MAX_NAME_BYTES + 1];
    const size_t fl = strlen(im->file);
    NkZipStatus ns = NK_ZIP_ERR_NAME_TOO_LONG;
    if (fl <= NK_ZIP_MAX_NAME_BYTES) {
        for (size_t k = 0; k <= fl; k++) norm[k] = im->file[k] == '\\' ? '/' : im->file[k];
        ns = nk_zip_check_name(norm, fl, NULL);
    }
    if (ns != NK_ZIP_OK) {
        diag_add(p, "%s: not a file name this module opens (%s: %s); not used", im->file, nk_zip_status_code(ns),
                 nk_zip_status_message(ns));
        return skip_image(p, im);
    }
    uint64_t t0 = now_ns();
    size_t size = 0;
    PackRead why;
    char zip_why[40];
    uint64_t id[ID_WORDS] = { ID_NONE, 0, 0, 0 };
    uint32_t same = 0;
    const uint32_t row = (uint32_t)(im - p->images);
    /* Only a first read looks for the file under another name: a later one (to decode an image whose
     * size alone was read, or one evicted) finds this image itself. */
    uint64_t read_held = 0;
    unsigned char *file = pack_read(p, im->file, 64u << 20, &size, &why, zip_why, sizeof(zip_why), id,
                                    im->state == 0 ? &same : NULL, &read_held);
    if (read_held > p->stats.decode_peak) p->stats.decode_peak = read_held;
    if (why == PACK_READ_SAME) {
        /* The file another image has read (or refused) under another name: that image holds its
         * state from now on, for every level naming either. */
        im->same = same;
        return image_load(p, &p->images[same - 1u], decode, want_w, want_h);
    }
    /* A file found damaged is recorded too, so that its other names find this image refused instead of
     * unpacking the entry again. */
    if (file || why == PACK_READ_DAMAGED) {
        p->stats.reads++;
        id_add(p, id, row);
    }
    if (!file) {
        if (zip_why[0])
            diag_add(p, "%s: damaged in the ZIP (%s); texture left as the game draws it", im->file, zip_why);
        else
            diag_add(p, "%s: not used: %s%s; texture left as the game draws it", im->file, read_reason(why),
                     why == PACK_READ_TOO_LARGE ? " of 64 MiB" : "");
        return skip_image(p, im);
    }
    PngInfo g;
    char bad[96];
    if (!png_check(file, size, &g, bad, sizeof(bad))) {
        free(file);
        diag_add(p, "%s: not a PNG this module decodes (%s); not used", im->file, bad);
        return skip_image(p, im);
    }
    im->w = g.w;
    im->h = g.h;
    im->state = 3;
    const uint64_t bytes = (uint64_t)g.w * (uint64_t)g.h * 4u;
    if (bytes > p->budget) {
        free(file);
        diag_add(p, "%s: %dx%d needs %llu KiB, more than the %llu KiB texture budget; not used", im->file, g.w, g.h,
                 (unsigned long long)(bytes >> 10), (unsigned long long)(p->budget >> 10));
        p->stats.over_budget++;
        im->state = 2;
        return 0;
    }
    if (!decode || (want_w && (g.w != want_w || g.h != want_h))) { free(file); return !decode; }
    evict_for(p, bytes);
    if (p->used + bytes > p->budget) {
        /* Everything held is in use: draw this texture unreplaced now, and try again on a later lookup. */
        free(file);
        p->stats.over_budget++;
        return 0;
    }
    uint64_t scratch = 0;
    im->rgba = png_decode(file, size, &g, &scratch, bad, sizeof(bad));
    scratch += size;   /* the file was held throughout */
    if (scratch > p->stats.decode_peak) p->stats.decode_peak = scratch;
    free(file);
    if (!im->rgba) {
        diag_add(p, "%s: not a PNG this module decodes (%s); not used", im->file, bad);
        return skip_image(p, im);
    }
    im->state = 1;
    lru_push_front(p, im);
    p->used += bytes;
    if (p->used > p->stats.cache_peak) p->stats.cache_peak = p->used;
    p->stats.loaded++;
    if (++im->decodes >= 2u) {
        im->wait = im->decodes >= 17u ? 65535u : (1u << (im->decodes - 1u)) - 1u;
        if (im->decodes == 2u)
            diag_add(p, "%s: evicted and decoded again; the %llu MiB texture budget cannot hold what is drawn "
                        "(SR_TEX_REPLACE_BUDGET_MB raises it), so it is decoded less often from now on and drawn "
                        "as the game draws it meanwhile", im->file, (unsigned long long)(p->budget >> 20));
    }
    p->stats.decode_ns += now_ns() - t0;
    return 1;
}

/* The image that holds a level's file: its own row, or the row that read the same file first. */
static Image *image_of(NkTexRepl *p, const Level *v) {
    Image *im = &p->images[v->image];
    return im->same ? &p->images[im->same - 1u] : im;
}

/* Makes a level's image usable as asked (see image_load), and checks once that it is the size the
 * level needs: level 0 any size, level n the size of level 0 halved n times (at least 1). A level
 * whose image cannot be used, or is the wrong size, is skipped (state 2); the image itself may
 * still serve other levels. Level 0's size must be known when a later level is loaded. */
static int load_level(NkTexRepl *p, Alias *a, uint32_t level, int decode) {
    Level *v = &a->levels[level];
    if (v->state == 2) return 0;
    int want_w = 0, want_h = 0;
    if (level) {
        const Image *l0 = image_of(p, &a->levels[0]);
        want_w = l0->w >> level > 0 ? l0->w >> level : 1;
        want_h = l0->h >> level > 0 ? l0->h >> level : 1;
    }
    const int r = image_load(p, image_of(p, v), decode, want_w, want_h);
    Image *im = image_of(p, v);   /* image_load may have found the file under another name */
    if (im->state == 2) return skip_level(p, v);
    if (v->state == 0 && (im->state == 1 || im->state == 3)) {
        if (level && (im->w != want_w || im->h != want_h)) {
            diag_add(p, "%s: mip level %u is %dx%d, not %dx%d (level 0 halved %u times); levels from %u on are not used",
                     im->file, (unsigned)level, im->w, im->h, want_w, want_h, (unsigned)level, (unsigned)level);
            p->stats.load_failed++;
            return skip_level(p, v);
        }
        v->state = 1;
    }
    return r;
}

int nk_texrepl_level_count(const NkTexRepl *p, const NkTexReplId *id) {
    if (!p || !id) return 0;
    const Alias *a = resolve(p, id);
    return a ? (int)a->level_n : 0;
}

int nk_texrepl_lookup_level(NkTexRepl *p, const NkTexReplId *id, int level, NkTexReplHit *out) {
    if (!p || !id || !out) return 0;
    memset(out, 0, sizeof(*out));
    p->stats.lookups++;
    Alias *a = resolve(p, id);
    if (!a) { p->stats.misses++; return 0; }
    if (!a->level_n) { p->stats.left_alone++; p->stats.misses++; return 0; }
    if (p->indexed_n && is_indexed(p, id)) {
        if (!a->indexed_noted) {
            a->indexed_noted = 1;
            diag_add(p, "%s: [indexed] marks its texture indexed (the image holds palette indices); not replaced",
                     a->levels[0].file);
        }
        p->stats.left_alone++;
        p->stats.misses++;
        return 0;
    }
    if (level < 0 || (uint32_t)level >= a->level_n) { p->stats.misses++; return 0; }
    /* PPSSPP loads the levels in order and stops at the first it cannot use: every level below this
     * one must be readable, and each the size of level 0 halved, before this one counts. */
    for (uint32_t j = 0; j < (uint32_t)level; j++) {
        Level *u = &a->levels[j];
        if (u->state == 0) load_level(p, a, j, 0);
        /* its image may since have proved unusable through another level naming the same file */
        if (u->state != 1 || image_of(p, u)->state == 2) { p->stats.misses++; return 0; }
    }
    Level *v = &a->levels[level];
    if ((v->state != 1 || image_of(p, v)->state != 1) && !load_level(p, a, (uint32_t)level, 1)) {
        p->stats.misses++;
        return 0;
    }
    Image *im = image_of(p, v);
    if (v->state != 1 || im->state != 1) { p->stats.misses++; return 0; }
    im->pins++;
    lru_unlink(p, im);
    lru_push_front(p, im);
    p->stats.hits++;
    out->rgba = im->rgba;
    out->w = im->w;
    out->h = im->h;
    out->pin = im;
    return 1;
}

int nk_texrepl_lookup(NkTexRepl *p, const NkTexReplId *id, NkTexReplHit *out) {
    return nk_texrepl_lookup_level(p, id, 0, out);
}

void nk_texrepl_unpin(NkTexRepl *p, NkTexReplHit *hit) {
    (void)p;
    if (!hit || !hit->pin) return;
    Image *v = (Image *)hit->pin;
    if (v->pins) v->pins--;
    memset(hit, 0, sizeof(*hit));
}

int nk_texrepl_factor(const NkTexReplHit *hit, int w, int h) {
    if (!hit || !hit->rgba || w <= 0 || h <= 0) return 0;
    if (hit->w % w || hit->h % h) return 0;
    const int fx = hit->w / w, fy = hit->h / h;
    return fx == fy ? fx : 0;
}

void nk_texrepl_stats(const NkTexRepl *p, NkTexReplStats *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!p) return;
    *out = p->stats;
    out->cache_bytes = p->used;
}

void nk_texrepl_note_bad_factor(NkTexRepl *p) {
    if (p) p->stats.bad_factor++;
}
