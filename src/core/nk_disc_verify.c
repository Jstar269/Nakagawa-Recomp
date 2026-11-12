/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Disc verification (nk_disc_verify.h). SHA-1 follows FIPS 180-4 section 6.1;
   CRC-32 is the reflected 0xEDB88320 polynomial with ~0 in and out, the CRC
   Redump and zlib list. */

#include "nk_disc_verify.h"

#include "nk_json.h"
#include "nk_platform.h"
#include "nk_psp_inflate.h"
#include "nk_types.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NK_DISC_HASH_CHUNK (1024u * 1024u)
#define NK_DISC_MAX_GAME_SERIALS 16u

static bool verify_fail(NkDiscExpandStatus *status, const char *code, const char *fmt, ...) {
    if (status) {
        va_list args;
        snprintf(status->code, sizeof(status->code), "%s", code);
        va_start(args, fmt);
        vsnprintf(status->message, sizeof(status->message), fmt, args);
        va_end(args);
    }
    return false;
}

static void verify_status_clear(NkDiscExpandStatus *status) {
    if (status) {
        status->code[0] = '\0';
        status->message[0] = '\0';
    }
}

/* ---- SHA-1 ------------------------------------------------------------- */

static uint32_t rotl32(uint32_t x, unsigned n) {
    return (x << n) | (x >> (32u - n));
}

static void sha1_block(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (unsigned t = 0; t < 16u; t++) {
        w[t] = ((uint32_t)block[4u * t] << 24) | ((uint32_t)block[4u * t + 1u] << 16) |
               ((uint32_t)block[4u * t + 2u] << 8) | (uint32_t)block[4u * t + 3u];
    }
    for (unsigned t = 16u; t < 80u; t++) {
        w[t] = rotl32(w[t - 3u] ^ w[t - 8u] ^ w[t - 14u] ^ w[t - 16u], 1u);
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (unsigned t = 0; t < 80u; t++) {
        uint32_t f, k;
        if (t < 20u) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (t < 40u) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (t < 60u) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t temp = rotl32(a, 5u) + f + e + k + w[t];
        e = d;
        d = c;
        c = rotl32(b, 30u);
        b = a;
        a = temp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void nk_sha1_init(NkSha1 *ctx) {
    static const uint32_t initial[5] = {
        0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u,
    };
    memset(ctx, 0, sizeof(*ctx));
    memcpy(ctx->state, initial, sizeof(initial));
}

void nk_sha1_update(NkSha1 *ctx, const uint8_t *data, size_t len) {
    ctx->byte_count += len;
    if (ctx->block_len > 0) {
        size_t take = 64u - ctx->block_len < len ? 64u - ctx->block_len : len;
        memcpy(ctx->block + ctx->block_len, data, take);
        ctx->block_len += take;
        data += take;
        len -= take;
        if (ctx->block_len < 64u) return;
        sha1_block(ctx->state, ctx->block);
        ctx->block_len = 0;
    }
    while (len >= 64u) {
        sha1_block(ctx->state, data);
        data += 64u;
        len -= 64u;
    }
    if (len > 0) {
        memcpy(ctx->block, data, len);
        ctx->block_len = len;
    }
}

void nk_sha1_finish(NkSha1 *ctx, uint8_t digest[20]) {
    uint64_t bits = ctx->byte_count * 8u;
    ctx->block[ctx->block_len++] = 0x80u;
    if (ctx->block_len > 56u) {
        memset(ctx->block + ctx->block_len, 0, 64u - ctx->block_len);
        sha1_block(ctx->state, ctx->block);
        ctx->block_len = 0;
    }
    memset(ctx->block + ctx->block_len, 0, 56u - ctx->block_len);
    for (unsigned i = 0; i < 8u; i++) ctx->block[63u - i] = (uint8_t)(bits >> (8u * i));
    sha1_block(ctx->state, ctx->block);
    for (unsigned i = 0; i < 5u; i++) {
        digest[4u * i] = (uint8_t)(ctx->state[i] >> 24);
        digest[4u * i + 1u] = (uint8_t)(ctx->state[i] >> 16);
        digest[4u * i + 2u] = (uint8_t)(ctx->state[i] >> 8);
        digest[4u * i + 3u] = (uint8_t)ctx->state[i];
    }
    memset(ctx->block, 0, sizeof(ctx->block));
}

/* ---- CRC-32 ------------------------------------------------------------ */

/* Slicing-by-four tables: table[0] is the classic byte table. Each caller
   builds its own (4 KiB, a few microseconds), so concurrent workers never share
   a table that is still being filled. */
typedef uint32_t NkCrc32Tables[4][256];

static void crc32_build_tables(NkCrc32Tables table) {
    for (uint32_t n = 0; n < 256u; n++) {
        uint32_t c = n;
        for (unsigned k = 0; k < 8u; k++) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[0][n] = c;
    }
    for (uint32_t n = 0; n < 256u; n++) {
        for (unsigned t = 1; t < 4u; t++) {
            uint32_t prev = table[t - 1u][n];
            table[t][n] = table[0][prev & 0xFFu] ^ (prev >> 8);
        }
    }
}

static uint32_t crc32_with_tables(NkCrc32Tables table, uint32_t crc, const uint8_t *data,
                                  size_t len) {
    uint32_t c = ~crc;
    while (len >= 4u) {
        c ^= (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
             ((uint32_t)data[3] << 24);
        c = table[3][c & 0xFFu] ^ table[2][(c >> 8) & 0xFFu] ^ table[1][(c >> 16) & 0xFFu] ^
            table[0][c >> 24];
        data += 4u;
        len -= 4u;
    }
    while (len-- > 0) c = table[0][(c ^ *data++) & 0xFFu] ^ (c >> 8);
    return ~c;
}

uint32_t nk_crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    NkCrc32Tables table;
    crc32_build_tables(table);
    return crc32_with_tables(table, crc, data, len);
}

/* ---- whole-image hash -------------------------------------------------- */

static void hex_encode(const uint8_t *bytes, size_t count, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < count; i++) {
        out[2u * i] = hex[bytes[i] >> 4];
        out[2u * i + 1u] = hex[bytes[i] & 0x0Fu];
    }
    out[2u * count] = '\0';
}

bool nk_disc_hash_file(const char *path, const NkDiscExpandCallbacks *callbacks,
                       NkDiscDigest *out, NkDiscExpandStatus *status) {
    verify_status_clear(status);
    if (out) memset(out, 0, sizeof(*out));
    if (!path || !path[0] || !out) {
        return verify_fail(status, NK_DISC_INTERNAL, "No disc image was given to hash.");
    }
    FILE *f = nk_fopen_utf8(path, "rb");
    if (!f) {
        return verify_fail(status, NK_DISC_READ_FAILED,
                           "The disc image could not be opened for reading.");
    }
    int64_t total = nk_platform_get_file_size(path);
    uint8_t *buffer = (uint8_t *)malloc(NK_DISC_HASH_CHUNK);
    if (!buffer) {
        fclose(f);
        return verify_fail(status, NK_DISC_INTERNAL, "No memory was free to hash the disc image.");
    }
    NkSha1 sha1;
    nk_sha1_init(&sha1);
    NkCrc32Tables crc_table;
    crc32_build_tables(crc_table);
    uint32_t crc = 0;
    uint64_t done = 0;
    bool ok = true;
    for (;;) {
        if (callbacks && callbacks->is_cancelled && callbacks->is_cancelled(callbacks->user)) {
            ok = verify_fail(status, NK_DISC_CANCELLED, "Checking the disc image was cancelled.");
            break;
        }
        size_t got = fread(buffer, 1, NK_DISC_HASH_CHUNK, f);
        if (got > 0) {
            nk_sha1_update(&sha1, buffer, got);
            crc = crc32_with_tables(crc_table, crc, buffer, got);
            done += got;
            if (callbacks && callbacks->on_progress) {
                callbacks->on_progress(done, total > 0 ? (uint64_t)total : done, callbacks->user);
            }
        }
        if (got < NK_DISC_HASH_CHUNK) {
            if (ferror(f)) {
                ok = verify_fail(status, NK_DISC_READ_FAILED,
                                 "The disc image could not be read to the end.");
            }
            break;
        }
    }
    fclose(f);
    free(buffer);
    /* The file changed size while it was read: the hash is of no one image. */
    if (ok && total >= 0 && (uint64_t)total != done) {
        ok = verify_fail(status, NK_DISC_READ_FAILED,
                         "The disc image changed while it was being checked.");
    }
    if (!ok) return false;
    uint8_t digest[20];
    nk_sha1_finish(&sha1, digest);
    hex_encode(digest, sizeof(digest), out->sha1);
    snprintf(out->crc32, sizeof(out->crc32), "%08x", (unsigned)crc);
    out->size_bytes = done;
    return true;
}

/* ---- datafile scanner -------------------------------------------------- */

typedef struct {
    const char *p;
    const char *end;
} Span;

static bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static unsigned hex_value(char c) {
    if (c >= '0' && c <= '9') return (unsigned)(c - '0');
    if (c >= 'a' && c <= 'f') return (unsigned)(c - 'a' + 10);
    return (unsigned)(c - 'A' + 10);
}

static bool parse_hex_bytes(const Span *value, uint8_t *out, size_t count) {
    if ((size_t)(value->end - value->p) != 2u * count) return false;
    for (size_t i = 0; i < count; i++) {
        char hi = value->p[2u * i], lo = value->p[2u * i + 1u];
        if (!is_hex_digit(hi) || !is_hex_digit(lo)) return false;
        out[i] = (uint8_t)((hex_value(hi) << 4) | hex_value(lo));
    }
    return true;
}

static bool parse_decimal(const Span *value, uint64_t *out) {
    size_t length = (size_t)(value->end - value->p);
    uint64_t result = 0;
    if (length == 0 || length > 20u) return false;
    for (size_t i = 0; i < length; i++) {
        char c = value->p[i];
        if (c < '0' || c > '9') return false;
        uint64_t digit = (uint64_t)(c - '0');
        if (result > (UINT64_MAX - digit) / 10u) return false;
        result = result * 10u + digit;
    }
    *out = result;
    return true;
}

size_t nk_disc_normalize_serials(const char *text, size_t length, char (*out_ids)[10],
                                 size_t max_ids) {
    size_t count = 0;
    size_t i = 0;
    while (i < length && count < max_ids) {
        /* One comma-separated part with spaces and dashes taken out. */
        char compact[32];
        size_t used = 0;
        while (i < length && text[i] != ',') {
            char c = text[i++];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '-') continue;
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            if (used + 1u < sizeof(compact)) compact[used++] = c;
        }
        if (i < length) i++; /* the comma */
        compact[used] = '\0';
        bool retail = used >= 9u;
        for (size_t k = 0; retail && k < 9u; k++) {
            char c = compact[k];
            retail = k < 4u ? (c >= 'A' && c <= 'Z') : (c >= '0' && c <= '9');
        }
        if (!retail) continue;
        bool seen = false;
        for (size_t k = 0; k < count && !seen; k++) seen = memcmp(out_ids[k], compact, 9u) == 0;
        if (seen) continue;
        memcpy(out_ids[count], compact, 9u);
        out_ids[count][9] = '\0';
        count++;
    }
    return count;
}

/* Copy element text, with the five predefined XML entities decoded. */
static void copy_text(const Span *text, char *out, size_t out_size) {
    static const struct { const char *entity; char c; } entities[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''},
    };
    size_t used = 0;
    for (const char *q = text->p; q < text->end && used + 1u < out_size;) {
        bool decoded = false;
        for (size_t e = 0; *q == '&' && e < sizeof(entities) / sizeof(entities[0]); e++) {
            size_t n = strlen(entities[e].entity);
            if ((size_t)(text->end - q) >= n && memcmp(q, entities[e].entity, n) == 0) {
                out[used++] = entities[e].c;
                q += n;
                decoded = true;
                break;
            }
        }
        if (!decoded) out[used++] = *q++;
    }
    out[used] = '\0';
    while (used > 0 && (out[used - 1u] == ' ' || out[used - 1u] == '\n' || out[used - 1u] == '\r' ||
                        out[used - 1u] == '\t')) {
        out[--used] = '\0';
    }
}

/* Whether [p, end) starts with needle, ASCII letters compared without case. */
static bool starts_with_nocase(const char *p, const char *end, const char *needle) {
    size_t n = strlen(needle);
    if ((size_t)(end - p) < n) return false;
    for (size_t i = 0; i < n; i++) {
        char a = p[i], b = needle[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b) return false;
    }
    return true;
}

/* An '&' at p that XML defines without a DTD: one of the five predefined
   entities, or a decimal (1-7 digits) or hex (1-6 digits) character reference. */
static bool is_predefined_reference(const char *p, const char *end) {
    static const char *const names[] = {"&amp;", "&lt;", "&gt;", "&quot;", "&apos;"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        size_t n = strlen(names[i]);
        if ((size_t)(end - p) >= n && memcmp(p, names[i], n) == 0) return true;
    }
    if (end - p < 3 || p[1] != '#') return false;
    const char *q = p + 2;
    bool hex = *q == 'x';
    if (hex) q++;
    size_t digits = 0;
    /* At most one digit past the longest form is read: the lookahead is bounded. */
    while (q < end && digits <= (hex ? 6u : 7u) && (hex ? is_hex_digit(*q) : (*q >= '0' && *q <= '9'))) {
        q++;
        digits++;
    }
    return digits >= 1u && digits <= (hex ? 6u : 7u) && q < end && *q == ';';
}

static int rom_compare(const void *a, const void *b) {
    return memcmp(((const NkDiscDatRom *)a)->sha1, ((const NkDiscDatRom *)b)->sha1, 20u);
}

/* SHA-1, then the order the list gives: qsort is not stable, so the order is
   part of the key and a SHA-1 listed twice keeps its first entry. */
static int rom_compare_listed(const void *a, const void *b) {
    int by_sha1 = rom_compare(a, b);
    if (by_sha1 != 0) return by_sha1;
    uint32_t x = ((const NkDiscDatRom *)a)->listed, y = ((const NkDiscDatRom *)b)->listed;
    return (x > y) - (x < y);
}

static int serial_compare(const void *a, const void *b) {
    return memcmp(a, b, 9u);
}

void nk_disc_dat_free(NkDiscDat *dat) {
    if (!dat) return;
    free(dat->roms);
    free(dat->serials);
    memset(dat, 0, sizeof(*dat));
}

static bool dat_push_rom(NkDiscDat *dat, size_t *capacity, const NkDiscDatRom *rom) {
    if (dat->rom_count == *capacity) {
        size_t grown = *capacity ? *capacity * 2u : 256u;
        NkDiscDatRom *roms = (NkDiscDatRom *)realloc(dat->roms, grown * sizeof(*roms));
        if (!roms) return false;
        dat->roms = roms;
        *capacity = grown;
    }
    dat->roms[dat->rom_count++] = *rom;
    return true;
}

static bool dat_push_serial(NkDiscDat *dat, size_t *capacity, const char id[10]) {
    if (dat->serial_count == *capacity) {
        size_t grown = *capacity ? *capacity * 2u : 256u;
        char (*serials)[10] = realloc(dat->serials, grown * sizeof(*serials));
        if (!serials) return false;
        dat->serials = serials;
        *capacity = grown;
    }
    memcpy(dat->serials[dat->serial_count++], id, 10u);
    return true;
}

/* ---- the datafile scanner ------------------------------------------------- */

/* One forward pass over a datafile's text, the rules of disc_verify.py's
   _DatScanner. The scan moves from the start to </datafile> and never goes
   back: each search starts where the last one stopped, and lookahead is
   bounded (a tag name, a reference), so the work is linear in the text's
   length. `examined` counts the bytes looked at, each time one is looked at,
   so a test can hold any input's cost to a fixed multiple of its length. */
typedef struct {
    const char *end;
    uint64_t examined;
} DatScan;

/* A tag name is read from at most this many bytes after "<" or "</"; the
   names the reader knows are shorter, so a longer one is simply unknown. */
#define NK_DISC_TAG_NAME_WINDOW 16u
/* The longest reference XML defines without a DTD: "&#1114111;". */
#define NK_DISC_REFERENCE_WINDOW 10u

/* The first byte c in [from, stop), or NULL. */
static const char *scan_byte(DatScan *s, const char *from, const char *stop, char c) {
    if (from >= stop) return NULL;
    const char *at = (const char *)memchr(from, c, (size_t)(stop - from));
    s->examined += (uint64_t)((at ? at + 1 : stop) - from);
    return at;
}

/* The first needle in [from, stop), or NULL: each candidate is compared only
   up to its first differing byte, and the search moves on past it. */
static const char *scan_find(DatScan *s, const char *from, const char *stop, const char *needle) {
    size_t n = strlen(needle);
    for (const char *p = from; p < stop;) {
        const char *at = scan_byte(s, p, stop, needle[0]);
        if (!at) return NULL;
        size_t same = 1;
        while (same < n && at + same < stop && at[same] == needle[same]) same++;
        s->examined += same - 1u;
        if (same == n) return at;
        p = at + 1;
    }
    return NULL;
}

/* The end of a tag from `from`: its '>', or a '<' that makes it malformed. */
static const char *scan_tag_end(DatScan *s, const char *from) {
    const char *p = from;
    while (p < s->end && *p != '<' && *p != '>') p++;
    s->examined += (uint64_t)((p < s->end ? p + 1 : p) - from);
    return p < s->end ? p : NULL;
}

/* Whether every '&' in [from, stop) starts a reference XML defines without a DTD. */
static bool scan_references_ok(DatScan *s, const char *from, const char *stop) {
    for (const char *at = scan_byte(s, from, stop, '&'); at; at = scan_byte(s, at + 1, stop, '&')) {
        s->examined += NK_DISC_REFERENCE_WINDOW;
        if (!is_predefined_reference(at, s->end)) return false;
    }
    return true;
}

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool is_attribute_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool is_attribute_char(char c) {
    return is_attribute_start(c) || (c >= '0' && c <= '9') || c == '.' || c == ':' || c == '-';
}

static bool span_is(const char *p, size_t length, const char *want) {
    return strlen(want) == length && memcmp(p, want, length) == 0;
}

/* A <rom> tag's sha1, crc and size from its attributes in [p, stop), read in
   order up to the first malformed one; the first of a repeated name is kept,
   as disc_verify.py keeps it. Each byte is passed once. */
static void scan_rom_attributes(DatScan *s, const char *p, const char *stop, Span *sha1, Span *crc,
                                Span *size, bool found[3]) {
    const char *start = p;
    for (;;) {
        while (p < stop && is_space(*p)) p++;
        if (p >= stop || !is_attribute_start(*p)) break;
        const char *name = p;
        while (p < stop && is_attribute_char(*p)) p++;
        size_t name_length = (size_t)(p - name);
        while (p < stop && is_space(*p)) p++;
        if (p >= stop || *p != '=') break;
        p++;
        while (p < stop && is_space(*p)) p++;
        if (p >= stop || *p != '"') break;
        const char *value = p + 1;
        s->examined += (uint64_t)(value - start);
        const char *close = scan_byte(s, value, stop, '"');
        if (!close) return;
        Span text = {value, close};
        if (!found[0] && span_is(name, name_length, "sha1")) { *sha1 = text; found[0] = true; }
        if (!found[1] && span_is(name, name_length, "crc")) { *crc = text; found[1] = true; }
        if (!found[2] && span_is(name, name_length, "size")) { *size = text; found[2] = true; }
        p = close + 1;
        start = p;
    }
    s->examined += (uint64_t)(p - start);
}

static bool markup_refused(NkDiscExpandStatus *status) {
    return verify_fail(status, NK_DISC_DAT_ENTITY_REFUSED,
                       "The datafile declares entities, a DTD of its own or markup a Redump list "
                       "does not use, so it was not read.");
}

typedef enum { FIELD_NONE = 0, FIELD_NAME, FIELD_VERSION, FIELD_DATE, FIELD_SERIAL } DatField;

static bool dat_scan(DatScan *s, const char *text, NkDiscDat *out, NkDiscExpandStatus *status) {
    static const char *const field_names[] = {"", "name", "version", "date", "serial"};
    const char *end = s->end;
    if (!scan_find(s, text, end, "<datafile")) {
        return verify_fail(status, NK_DISC_DAT_NOT_DATAFILE,
                           "This file isn't a Redump datafile: it has no <datafile> element.");
    }
    bool root = false, closed = false, doctype = false, in_game = false;
    int header = 0; /* 0 before the first <header>, 1 inside it, 2 after it */
    DatField field = FIELD_NONE;
    const char *field_start = text;
    char ids[NK_DISC_MAX_GAME_SERIALS][10];
    size_t id_count = 0;
    size_t rom_capacity = 0, serial_capacity = 0;
    const char *pos = text;
    while (pos < end) {
        const char *lt = scan_byte(s, pos, end, '<');
        if (!scan_references_ok(s, pos, lt ? lt : end)) {
            return verify_fail(status, NK_DISC_DAT_ENTITY_REFUSED,
                               "The datafile refers to an entity XML does not predefine; a Redump "
                               "list has none, so it was not read.");
        }
        if (!lt) break;
        DatField pending = field;
        field = FIELD_NONE;
        if (end - lt >= 2 && lt[1] == '?') {
            const char *q = scan_find(s, lt + 2, end, "?>");
            if (!q) {
                return verify_fail(status, NK_DISC_DAT_NOT_DATAFILE,
                                   "The datafile is cut short inside a <?...?> declaration.");
            }
            pos = q + 2;
            continue;
        }
        if (end - lt >= 2 && lt[1] == '!') {
            s->examined += strlen("<!DOCTYPE");
            if (doctype || root || !starts_with_nocase(lt, end, "<!DOCTYPE")) {
                return markup_refused(status);
            }
            const char *q = scan_tag_end(s, lt + 2);
            if (!q) {
                return verify_fail(status, NK_DISC_DAT_NOT_DATAFILE,
                                   "The datafile is cut short inside its <!DOCTYPE>.");
            }
            /* Only "<!DOCTYPE name PUBLIC/SYSTEM ...>" naming an external DTD,
               which is never read: an internal subset ('[') or markup inside
               it is refused. */
            if (*q == '<' || scan_byte(s, lt, q, '[')) return markup_refused(status);
            doctype = true;
            pos = q + 1;
            continue;
        }
        const char *gt = scan_tag_end(s, lt + 1);
        if (!gt) {
            return verify_fail(status, NK_DISC_DAT_NOT_DATAFILE, "The datafile is cut short inside a tag.");
        }
        if (*gt == '<') {
            return verify_fail(status, NK_DISC_DAT_NOT_DATAFILE,
                               "The datafile has a malformed tag: a '<' inside a tag.");
        }
        if (!scan_references_ok(s, lt, gt)) {
            return verify_fail(status, NK_DISC_DAT_ENTITY_REFUSED,
                               "The datafile refers to an entity XML does not predefine; a Redump "
                               "list has none, so it was not read.");
        }
        bool closing = lt + 1 < gt && lt[1] == '/';
        const char *name = lt + (closing ? 2 : 1);
        size_t window = (size_t)(gt - name) < NK_DISC_TAG_NAME_WINDOW ? (size_t)(gt - name)
                                                                      : NK_DISC_TAG_NAME_WINDOW;
        size_t name_length = 0;
        while (name_length < window && !is_space(name[name_length]) && name[name_length] != '/') {
            name_length++;
        }
        s->examined += window;
        const char *name_end = name + name_length;
        bool bare = name_end == gt; /* "<name>" with nothing after the name */
        pos = gt + 1;
        if (closing) {
            if (pending != FIELD_NONE && span_is(name, name_length, field_names[pending])) {
                Span value = {field_start, lt};
                s->examined += (uint64_t)(lt - field_start);
                if (pending == FIELD_SERIAL) {
                    if (in_game && id_count < NK_DISC_MAX_GAME_SERIALS) {
                        id_count += nk_disc_normalize_serials(value.p, (size_t)(value.end - value.p),
                                                              ids + id_count,
                                                              NK_DISC_MAX_GAME_SERIALS - id_count);
                    }
                } else {
                    char *dest = pending == FIELD_NAME ? out->name
                               : pending == FIELD_VERSION ? out->version : out->date;
                    size_t dest_size = pending == FIELD_NAME ? sizeof(out->name)
                                     : pending == FIELD_VERSION ? sizeof(out->version)
                                                                : sizeof(out->date);
                    if (!dest[0]) copy_text(&value, dest, dest_size);
                }
            } else if (span_is(name, name_length, "header") && header == 1) {
                header = 2;
            } else if (span_is(name, name_length, "game") && in_game) {
                out->games++;
                for (size_t k = 0; k < id_count; k++) {
                    if (!dat_push_serial(out, &serial_capacity, ids[k])) {
                        return verify_fail(status, NK_DISC_INTERNAL, "No memory was free to read the datafile.");
                    }
                }
                in_game = false;
                id_count = 0;
            } else if (span_is(name, name_length, "datafile")) {
                const char *q = name_end;
                while (q < gt && is_space(*q)) q++;
                s->examined += (uint64_t)(gt - name_end);
                if (q == gt) {
                    closed = true;
                    break;
                }
            }
            continue;
        }
        bool self_closing = gt[-1] == '/';
        if (span_is(name, name_length, "datafile")) {
            root = true;
        } else if (span_is(name, name_length, "header") && header == 0 && !in_game) {
            header = 1;
        } else if (header == 1 && bare && (span_is(name, name_length, "name") ||
                                           span_is(name, name_length, "version") ||
                                           span_is(name, name_length, "date"))) {
            field = name_length == 4u && name[0] == 'n' ? FIELD_NAME
                  : name_length == 4u ? FIELD_DATE : FIELD_VERSION;
            field_start = pos;
        } else if (span_is(name, name_length, "game")) {
            if (in_game) {
                return verify_fail(status, NK_DISC_DAT_NOT_DATAFILE,
                                   "The datafile is cut short inside a <game> element.");
            }
            if (self_closing) {
                out->games++;
            } else {
                in_game = true;
                id_count = 0;
            }
        } else if (span_is(name, name_length, "serial") && in_game && bare) {
            field = FIELD_SERIAL;
            field_start = pos;
        } else if (span_is(name, name_length, "rom") && in_game) {
            Span sha1 = {NULL, NULL}, crc = {NULL, NULL}, size = {NULL, NULL};
            bool found[3] = {false, false, false};
            NkDiscDatRom entry;
            uint8_t crc_bytes[4];
            memset(&entry, 0, sizeof(entry));
            scan_rom_attributes(s, name_end, self_closing ? gt - 1 : gt, &sha1, &crc, &size, found);
            if (!(found[0] && found[1] && found[2] && parse_hex_bytes(&sha1, entry.sha1, 20u) &&
                  parse_hex_bytes(&crc, crc_bytes, 4u) && parse_decimal(&size, &entry.size_bytes))) {
                out->rejected_roms++;
                continue;
            }
            if (out->rom_count >= NK_DISC_DAT_MAX_ROMS) {
                return verify_fail(status, NK_DISC_DAT_TOO_LARGE, "The datafile lists more than %u discs.",
                                   (unsigned)NK_DISC_DAT_MAX_ROMS);
            }
            entry.crc32 = ((uint32_t)crc_bytes[0] << 24) | ((uint32_t)crc_bytes[1] << 16) |
                          ((uint32_t)crc_bytes[2] << 8) | (uint32_t)crc_bytes[3];
            entry.listed = (uint32_t)out->rom_count;
            if (!dat_push_rom(out, &rom_capacity, &entry)) {
                return verify_fail(status, NK_DISC_INTERNAL, "No memory was free to read the datafile.");
            }
        }
    }
    /* A list cut short (a download that stopped) is refused, never read as the
       whole list: its discs past the cut would all read as unknown. */
    if (!closed) {
        return verify_fail(status, NK_DISC_DAT_NOT_DATAFILE,
                           "The datafile is cut short: it has no closing </datafile>.");
    }
    if (in_game) {
        return verify_fail(status, NK_DISC_DAT_NOT_DATAFILE,
                           "The datafile is cut short inside a <game> element.");
    }
    return true;
}

bool nk_disc_dat_parse_counted(const char *text, size_t length, NkDiscDat *out,
                               NkDiscExpandStatus *status, uint64_t *bytes_examined) {
    DatScan scan = {NULL, 0u};
    bool ok = false;
    verify_status_clear(status);
    if (bytes_examined) *bytes_examined = 0;
    if (!out) return verify_fail(status, NK_DISC_INTERNAL, "No datafile result was given.");
    memset(out, 0, sizeof(*out));
    if (!text) return verify_fail(status, NK_DISC_INTERNAL, "No datafile text was given.");
    scan.end = text + length;
    scan.examined = length; /* the UTF-8 check reads each byte once */
    if (!nk_json_validate_utf8((const uint8_t *)text, length)) {
        ok = verify_fail(status, NK_DISC_DAT_NOT_DATAFILE, "The datafile isn't UTF-8 text.");
    } else {
        ok = dat_scan(&scan, text, out, status);
    }
    if (bytes_examined) *bytes_examined = scan.examined;
    if (!ok) {
        nk_disc_dat_free(out);
        return false;
    }
    if (out->rom_count == 0) {
        nk_disc_dat_free(out);
        return verify_fail(status, NK_DISC_DAT_EMPTY, "The datafile lists no disc with a SHA-1 hash.");
    }
    qsort(out->roms, out->rom_count, sizeof(out->roms[0]), rom_compare_listed);
    /* A SHA-1 listed twice keeps its first entry, as the Python reader does. */
    size_t kept = 0;
    for (size_t i = 0; i < out->rom_count; i++) {
        if (kept > 0 && memcmp(out->roms[kept - 1u].sha1, out->roms[i].sha1, 20u) == 0) continue;
        out->roms[kept++] = out->roms[i];
    }
    out->rom_count = kept;
    if (out->serial_count > 0) {
        qsort(out->serials, out->serial_count, sizeof(out->serials[0]), serial_compare);
        size_t unique = 0;
        for (size_t i = 0; i < out->serial_count; i++) {
            if (unique > 0 && memcmp(out->serials[unique - 1u], out->serials[i], 9u) == 0) continue;
            memcpy(out->serials[unique++], out->serials[i], 10u);
        }
        out->serial_count = unique;
    }
    return true;
}

bool nk_disc_dat_parse(const char *text, size_t length, NkDiscDat *out,
                       NkDiscExpandStatus *status) {
    return nk_disc_dat_parse_counted(text, length, out, status, NULL);
}

static bool date_digits(const char *p, const char *pattern) {
    for (; *pattern; p++, pattern++) {
        if (*pattern == 'd' ? !(*p >= '0' && *p <= '9') : *p != *pattern) return false;
    }
    return true;
}

bool nk_disc_list_date_is_older(const char *date, const char *other) {
    if (!date || !other) return false;
    size_t a = strlen(date), b = strlen(other);
    bool a_time = a == 19u, b_time = b == 19u;
    if ((a != 10u && !a_time) || (b != 10u && !b_time)) return false;
    if (!date_digits(date, a_time ? "dddd-dd-dd dd-dd-dd" : "dddd-dd-dd") ||
        !date_digits(other, b_time ? "dddd-dd-dd dd-dd-dd" : "dddd-dd-dd")) {
        return false;
    }
    int day = memcmp(date, other, 10u);
    if (day != 0) return day < 0;
    return a_time && b_time && memcmp(date + 11, other + 11, 8u) < 0;
}

static bool read_bounded_file(const char *path, char **out_text, size_t *out_length,
                              NkDiscExpandStatus *status) {
    *out_text = NULL;
    *out_length = 0;
    FILE *f = nk_fopen_utf8(path, "rb");
    if (!f) return verify_fail(status, NK_DISC_DAT_READ_FAILED, "The datafile can't be opened.");
    char *buffer = (char *)malloc((size_t)NK_DISC_DAT_MAX_BYTES + 1u);
    if (!buffer) {
        fclose(f);
        return verify_fail(status, NK_DISC_INTERNAL, "No memory was free to read the datafile.");
    }
    size_t got = fread(buffer, 1, (size_t)NK_DISC_DAT_MAX_BYTES + 1u, f);
    bool failed = ferror(f) != 0;
    fclose(f);
    if (failed) {
        free(buffer);
        return verify_fail(status, NK_DISC_DAT_READ_FAILED, "The datafile can't be read.");
    }
    if (got > NK_DISC_DAT_MAX_BYTES) {
        free(buffer);
        return verify_fail(status, NK_DISC_DAT_TOO_LARGE, "The datafile is larger than %u bytes.",
                           (unsigned)NK_DISC_DAT_MAX_BYTES);
    }
    buffer[got] = '\0';
    *out_text = buffer;
    *out_length = got;
    return true;
}

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool zip_fail(NkDiscExpandStatus *status, const char *reason) {
    return verify_fail(status, NK_DISC_DAT_ZIP_INVALID,
                       "The zip can't be read as a Redump datafile: %s", reason);
}

/* The one .dat inside a zip as Redump serves it (PKWARE APPNOTE: one entry,
   stored or deflated, not encrypted, no ZIP64), inflated under
   NK_DISC_DAT_MAX_BYTES and checked against its CRC-32. Every offset and size
   is checked against the bytes held before it is used. *out_text is malloc'd. */
static bool dat_text_from_zip(const uint8_t *zip, size_t size, char **out_text, size_t *out_length,
                              NkDiscExpandStatus *status) {
    enum { EOCD = 22u, CENTRAL = 46u, LOCAL = 30u };
    *out_text = NULL;
    *out_length = 0;
    if (size < EOCD) return zip_fail(status, "it is cut short.");
    /* The end record is in the last 22 + 65535 bytes; its comment runs to the end. */
    size_t eocd = SIZE_MAX;
    size_t lowest = size - EOCD > 0xFFFFu ? size - EOCD - 0xFFFFu : 0u;
    for (size_t at = size - EOCD + 1u; at-- > lowest;) {
        if (read_le32(zip + at) == 0x06054b50u && at + EOCD + read_le16(zip + at + 20u) == size) {
            eocd = at;
            break;
        }
    }
    if (eocd == SIZE_MAX) return zip_fail(status, "it has no end record.");
    /* A ZIP64 end record's locator sits just before the end record. */
    if (eocd >= 20u && read_le32(zip + eocd - 20u) == 0x07064b50u) {
        return zip_fail(status, "it uses ZIP64.");
    }
    const uint8_t *end_record = zip + eocd;
    uint32_t central_size = read_le32(end_record + 12u);
    uint32_t central_offset = read_le32(end_record + 16u);
    if (read_le16(end_record + 4u) != 0u || read_le16(end_record + 6u) != 0u) {
        return zip_fail(status, "it spans several disks.");
    }
    if (read_le16(end_record + 8u) != 1u || read_le16(end_record + 10u) != 1u) {
        return zip_fail(status, "it does not hold exactly one file.");
    }
    if ((uint64_t)central_offset + central_size > eocd || central_size < CENTRAL) {
        return zip_fail(status, "its central directory is not where its end record says.");
    }
    const uint8_t *central = zip + central_offset;
    uint16_t flags = read_le16(central + 8u);
    uint16_t method = read_le16(central + 10u);
    uint32_t crc = read_le32(central + 16u);
    uint32_t packed_size = read_le32(central + 20u);
    uint32_t plain_size = read_le32(central + 24u);
    size_t name_length = read_le16(central + 28u);
    size_t extra_length = read_le16(central + 30u);
    size_t comment_length = read_le16(central + 32u);
    uint32_t local_offset = read_le32(central + 42u);
    if (read_le32(central) != 0x02014b50u ||
        (uint64_t)CENTRAL + name_length + extra_length + comment_length != central_size) {
        return zip_fail(status, "its central directory is malformed.");
    }
    if (packed_size == 0xFFFFFFFFu || plain_size == 0xFFFFFFFFu || local_offset == 0xFFFFFFFFu) {
        return zip_fail(status, "it uses ZIP64.");
    }
    /* A ZIP64 block (header ID 0x0001) in the entry's extra field, as
       disc_verify.py's _has_zip64_extra finds it. The field lies inside the
       central directory, whose size was checked above. */
    const uint8_t *extra = central + CENTRAL + name_length;
    for (size_t at = 0; at + 4u <= extra_length; at += 4u + read_le16(extra + at + 2u)) {
        if (read_le16(extra + at) == 0x0001u) return zip_fail(status, "it uses ZIP64.");
    }
    if (flags & 0x41u) return zip_fail(status, "the file in it is encrypted.");
    if (method != 0u && method != 8u) {
        return zip_fail(status, "the file in it is compressed with a method other than deflate.");
    }
    const char *name = (const char *)central + CENTRAL;
    if (name_length < 4u || !starts_with_nocase(name + name_length - 4u, name + name_length, ".dat")) {
        return zip_fail(status, "the file in it is not a .dat file.");
    }
    if (plain_size > NK_DISC_DAT_MAX_BYTES) {
        return verify_fail(status, NK_DISC_DAT_TOO_LARGE,
                           "The datafile in the zip is larger than %u bytes.",
                           (unsigned)NK_DISC_DAT_MAX_BYTES);
    }
    if ((uint64_t)local_offset + LOCAL > central_offset) {
        return zip_fail(status, "its file's header is past its central directory.");
    }
    const uint8_t *local = zip + local_offset;
    uint64_t data_offset = (uint64_t)local_offset + LOCAL + read_le16(local + 26u) +
                           read_le16(local + 28u);
    if (read_le32(local) != 0x04034b50u || read_le16(local + 8u) != method) {
        return zip_fail(status, "its file's header disagrees with its central directory.");
    }
    if (data_offset + packed_size > central_offset) {
        return zip_fail(status, "its file's data runs past its central directory.");
    }
    const uint8_t *packed = zip + data_offset;
    uint8_t *plain = NULL;
    size_t plain_length = 0;
    if (method == 0u) {
        if (packed_size != plain_size) return zip_fail(status, "its stored file's sizes differ.");
        plain = (uint8_t *)malloc((size_t)plain_size + 1u);
        if (!plain) return verify_fail(status, NK_DISC_INTERNAL, "No memory was free to read the datafile.");
        memcpy(plain, packed, plain_size);
        plain_length = plain_size;
    } else {
        char error[128];
        int rc = nk_psp_inflate_raw(packed, packed_size, &plain, &plain_length, plain_size,
                                    (size_t)NK_DISC_DAT_MAX_BYTES, error, sizeof(error));
        if (rc != NK_INFLATE_OK) free(plain);  /* NULL by its contract; freed either way */
        if (rc == NK_INFLATE_ERR_OVERFLOW) {
            return verify_fail(status, NK_DISC_DAT_TOO_LARGE,
                               "The datafile in the zip is larger than %u bytes.",
                               (unsigned)NK_DISC_DAT_MAX_BYTES);
        }
        if (rc != NK_INFLATE_OK) {
            return zip_fail(status, rc == NK_INFLATE_ERR_MEMORY ? "no memory was free to inflate it."
                                                                : "its file does not inflate.");
        }
    }
    if (plain_length != plain_size || nk_crc32_update(0u, plain, plain_length) != crc) {
        free(plain);
        return zip_fail(status, "its file's size or CRC-32 does not match its header.");
    }
    *out_text = (char *)plain;
    *out_length = plain_length;
    return true;
}

/* A datafile's own text from a file: the file, or the .dat inside it when it
   is a zip. *out_text is malloc'd. */
static bool read_dat_source(const char *path, char **out_text, size_t *out_length,
                            NkDiscExpandStatus *status) {
    if (!read_bounded_file(path, out_text, out_length, status)) return false;
    if (*out_length < 4u || memcmp(*out_text, "PK\x03\x04", 4u) != 0) return true;
    char *zip = *out_text;
    size_t zip_length = *out_length;
    bool ok = dat_text_from_zip((const uint8_t *)zip, zip_length, out_text, out_length, status);
    free(zip);
    return ok;
}

bool nk_disc_dat_load(const char *path, NkDiscDat *out, NkDiscExpandStatus *status) {
    char *text = NULL;
    size_t length = 0;
    if (out) memset(out, 0, sizeof(*out));
    if (!path || !path[0] || !out) {
        return verify_fail(status, NK_DISC_INTERNAL, "No datafile was given.");
    }
    if (!read_dat_source(path, &text, &length, status)) return false;
    /* A UTF-8 byte-order mark is allowed, as the Python reader allows it. */
    size_t skip = length >= 3u && memcmp(text, "\xEF\xBB\xBF", 3u) == 0 ? 3u : 0u;
    bool ok = nk_disc_dat_parse(text + skip, length - skip, out, status);
    free(text);
    return ok;
}

/* Keep the printable ASCII of a list's own text (its date or version), so it
   can't put control characters, or half a UTF-8 sequence cut at the buffer's
   end, in a log line or on screen. disc_verify.py's RedumpDat.stamp keeps the
   same characters. */
static void copy_printable(const char *text, char *out, size_t out_size) {
    size_t used = 0;
    for (const char *p = text; *p && used + 1u < out_size; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 0x20u && c < 0x7Fu) out[used++] = (char)c;
    }
    out[used] = '\0';
}

bool nk_disc_dat_path(const char *data_root, char *out, size_t out_size) {
    if (!data_root || !data_root[0] || !out || out_size == 0) return false;
    char sep = nk_platform_path_separator();
    int written = snprintf(out, out_size, "%s%c%s%c%s", data_root, sep, NK_DISC_DAT_DIR, sep,
                           NK_DISC_DAT_FILE);
    return written > 0 && (size_t)written < out_size;
}

bool nk_disc_bundled_dat_path(const char *base_dir, char *out, size_t out_size) {
    if (!base_dir || !base_dir[0] || !out || out_size == 0) return false;
    char sep = nk_platform_path_separator();
    size_t base_length = strlen(base_dir);
    bool ends_in_separator = base_dir[base_length - 1u] == '/' || base_dir[base_length - 1u] == '\\';
    char joiner[2] = {ends_in_separator ? '\0' : sep, '\0'};
    int written = snprintf(out, out_size, "%s%s%s%c%s%c%s", base_dir, joiner,
                           NK_DISC_BUNDLED_DAT_DIR1, sep, NK_DISC_BUNDLED_DAT_DIR2, sep,
                           NK_DISC_BUNDLED_DAT_FILE);
    return written > 0 && (size_t)written < out_size;
}

bool nk_disc_dat_load_active(const char *data_root, const char *bundled_path, NkDiscDat *out,
                             NkDiscListSource *source, NkDiscExpandStatus *status) {
    char imported[NK_MAX_PATH];
    verify_status_clear(status);
    if (source) *source = NK_DISC_LIST_NONE;
    if (out) memset(out, 0, sizeof(*out));
    if (!out) return verify_fail(status, NK_DISC_INTERNAL, "No datafile result was given.");
    if (data_root && data_root[0] && nk_disc_dat_path(data_root, imported, sizeof(imported)) &&
        nk_platform_file_exists(imported)) {
        if (source) *source = NK_DISC_LIST_IMPORTED;
        if (!nk_disc_dat_load(imported, out, status)) return false;
        /* The user's choice stands, but an import older than the bundled list
           is named as such (disc_verify.load_verification_dat does the same). */
        NkDiscDat bundled;
        NkDiscExpandStatus ignored;
        if (bundled_path && bundled_path[0] && nk_platform_file_exists(bundled_path) &&
            nk_disc_dat_load(bundled_path, &bundled, &ignored)) {
            char mine[64], theirs[64];
            copy_printable(out->date[0] ? out->date : out->version, mine, sizeof(mine));
            copy_printable(bundled.date[0] ? bundled.date : bundled.version, theirs, sizeof(theirs));
            if (nk_disc_list_date_is_older(mine, theirs)) {
                memcpy(out->newer_bundled_date, theirs, sizeof(out->newer_bundled_date));
            }
            nk_disc_dat_free(&bundled);
        }
        return true;
    }
    if (bundled_path && bundled_path[0] && nk_platform_file_exists(bundled_path)) {
        if (source) *source = NK_DISC_LIST_BUNDLED;
        return nk_disc_dat_load(bundled_path, out, status);
    }
    return false;
}

bool nk_disc_dat_import(const char *source, const char *data_root, NkDiscDat *out,
                        NkDiscExpandStatus *status) {
    char *text = NULL;
    size_t length = 0;
    char target[NK_MAX_PATH];
    char dir[NK_MAX_PATH];
    char tmp[NK_MAX_PATH + 8];
    NkDiscDat parsed;
    verify_status_clear(status);
    if (out) memset(out, 0, sizeof(*out));
    if (!source || !source[0] || !nk_disc_dat_path(data_root, target, sizeof(target))) {
        return verify_fail(status, NK_DISC_INTERNAL, "No datafile or data folder was given.");
    }
    /* From a zip, the .dat inside is what is checked and kept. */
    if (!read_dat_source(source, &text, &length, status)) return false;
    size_t skip = length >= 3u && memcmp(text, "\xEF\xBB\xBF", 3u) == 0 ? 3u : 0u;
    if (!nk_disc_dat_parse(text + skip, length - skip, &parsed, status)) {
        free(text);
        return false;
    }
    int n = snprintf(dir, sizeof(dir), "%s%c%s", data_root, nk_platform_path_separator(),
                     NK_DISC_DAT_DIR);
    int m = snprintf(tmp, sizeof(tmp), "%s.tmp", target);
    bool ok = n > 0 && (size_t)n < sizeof(dir) && m > 0 && (size_t)m < sizeof(tmp) &&
              nk_platform_mkdir_p_private(dir);
    if (ok) {
        FILE *f = nk_platform_fopen_private(tmp, "wb");
        ok = f != NULL;
        if (ok) {
            ok = fwrite(text, 1, length, f) == length;
            ok = (fclose(f) == 0) && ok;
        }
        if (ok) ok = nk_rename_utf8(tmp, target) == 0;
        if (!ok) (void)nk_remove_utf8(tmp);
    }
    free(text);
    if (!ok) {
        nk_disc_dat_free(&parsed);
        return verify_fail(status, NK_DISC_DAT_WRITE_FAILED,
                           "The datafile can't be saved in the data folder.");
    }
    if (out) {
        *out = parsed;
    } else {
        nk_disc_dat_free(&parsed);
    }
    return true;
}

bool nk_disc_dat_has_serial(const NkDiscDat *dat, const char *disc_id) {
    if (!dat || !dat->serials || !disc_id || strlen(disc_id) != 9u) return false;
    return bsearch(disc_id, dat->serials, dat->serial_count, sizeof(dat->serials[0]),
                   serial_compare) != NULL;
}

NkDiscTier nk_disc_classify(const NkDiscDat *dat, const char *disc_id,
                            const NkDiscDigest *digest, bool disc_id_verified_elsewhere) {
    if (!dat || !dat->roms || !digest || !digest->sha1[0]) return NK_DISC_TIER_NOT_CHECKED;
    NkDiscDatRom key;
    Span sha1 = {digest->sha1, digest->sha1 + strlen(digest->sha1)};
    Span crc = {digest->crc32, digest->crc32 + strlen(digest->crc32)};
    uint8_t crc_bytes[4];
    memset(&key, 0, sizeof(key));
    if (parse_hex_bytes(&sha1, key.sha1, 20u) && parse_hex_bytes(&crc, crc_bytes, 4u)) {
        uint32_t crc32 = ((uint32_t)crc_bytes[0] << 24) | ((uint32_t)crc_bytes[1] << 16) |
                         ((uint32_t)crc_bytes[2] << 8) | (uint32_t)crc_bytes[3];
        const NkDiscDatRom *rom = (const NkDiscDatRom *)bsearch(
            &key, dat->roms, dat->rom_count, sizeof(dat->roms[0]), rom_compare);
        if (rom && rom->crc32 == crc32 && rom->size_bytes == digest->size_bytes) {
            return NK_DISC_TIER_VERIFIED_RETAIL;
        }
    }
    if (disc_id && disc_id[0] && (disc_id_verified_elsewhere || nk_disc_dat_has_serial(dat, disc_id))) {
        return NK_DISC_TIER_MODIFIED;
    }
    return NK_DISC_TIER_UNKNOWN;
}

/* ---- one call for an add worker ----------------------------------------- */

/* One sentence for a tier, naming the list it was checked against. */
static void tier_reason(NkDiscTier tier, const char *list, char *out, size_t out_size) {
    switch (tier) {
    case NK_DISC_TIER_VERIFIED_RETAIL:
        snprintf(out, out_size, "Verified retail: the whole image matches a retail disc in %s.",
                 list);
        break;
    case NK_DISC_TIER_MODIFIED:
        snprintf(out, out_size,
                 "Modified: its disc ID is a retail disc's, but its hashes differ from every "
                 "dump of it in %s. A damaged dump does that as well as a changed copy (a "
                 "patch or a fan translation); dump the disc again and check it to tell which.",
                 list);
        break;
    case NK_DISC_TIER_UNKNOWN:
    case NK_DISC_TIER_NOT_CHECKED:
    default:
        snprintf(out, out_size,
                 "Unknown: the image is not in %s, and nothing shows its disc ID is a retail "
                 "disc's. Not in the list does not mean bad: it is only not shown to be retail.",
                 list);
        break;
    }
}


bool nk_disc_verify_image(const char *iso_path, const char *data_root,
                          const char *bundled_dat_path, const char *disc_id,
                          bool disc_id_verified_elsewhere,
                          const NkDiscExpandCallbacks *callbacks, NkDiscVerification *out) {
    NkDiscExpandStatus status;
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->tier = NK_DISC_TIER_NOT_CHECKED;
    if (!nk_disc_hash_file(iso_path, callbacks, &out->digest, &status)) {
        out->result = strcmp(status.code, NK_DISC_CANCELLED) == 0 ? NK_DISC_VERIFY_CANCELLED
                                                                   : NK_DISC_VERIFY_READ_FAILED;
        snprintf(out->code, sizeof(out->code), "%s", status.code);
        snprintf(out->reason, sizeof(out->reason), "Not checked: %s", status.message);
        return false;
    }
    out->result = NK_DISC_VERIFY_HASHED;
    NkDiscDat dat;
    NkDiscListSource source = NK_DISC_LIST_NONE;
    bool loaded = nk_disc_dat_load_active(data_root, bundled_dat_path, &dat, &source, &status);
    out->list = source;
    if (!loaded && source == NK_DISC_LIST_NONE) {
        snprintf(out->reason, sizeof(out->reason),
                 "Not checked: no Redump list was found, neither an imported one nor the one "
                 "bundled with Nakagawa.");
        return true;
    }
    if (!loaded) {
        out->result = NK_DISC_VERIFY_LIST_UNUSABLE;
        snprintf(out->code, sizeof(out->code), "%s", status.code);
        if (source == NK_DISC_LIST_IMPORTED) {
            snprintf(out->reason, sizeof(out->reason),
                     "Not checked: your imported Redump list can't be used (%s). Import it again.",
                     status.code);
        } else {
            snprintf(out->reason, sizeof(out->reason),
                     "Not checked: the Redump list bundled with Nakagawa can't be used (%s). "
                     "Reinstall Nakagawa, or import a list.", status.code);
        }
        return true;
    }
    copy_printable(dat.date[0] ? dat.date : dat.version, out->list_date, sizeof(out->list_date));
    char described[320];
    snprintf(described, sizeof(described), "%s (%s%s%s%u discs)%s%s%s",
             source == NK_DISC_LIST_IMPORTED ? "your imported Redump list"
                                             : "the Redump list bundled with Nakagawa",
             out->list_date[0] ? "dated " : "", out->list_date, out->list_date[0] ? ", " : "",
             (unsigned)dat.rom_count,
             dat.newer_bundled_date[0] ? ", older than the list bundled with Nakagawa (dated " : "",
             dat.newer_bundled_date,
             dat.newer_bundled_date[0] ? "): import a newer list, or delete "
                                         "verification/redump.dat from your data folder to use "
                                         "the bundled one" : "");
    out->tier = nk_disc_classify(&dat, disc_id, &out->digest, disc_id_verified_elsewhere);
    nk_disc_dat_free(&dat);
    tier_reason(out->tier, described, out->reason, sizeof(out->reason));
    return true;
}

void nk_disc_verification_to_entry(const NkDiscVerification *check, NkGameEntry *entry) {
    if (!check || !entry) return;
    if (check->result != NK_DISC_VERIFY_HASHED && check->result != NK_DISC_VERIFY_LIST_UNUSABLE) {
        return;
    }
    memcpy(entry->content_sha1, check->digest.sha1, sizeof(entry->content_sha1));
    memcpy(entry->content_crc32, check->digest.crc32, sizeof(entry->content_crc32));
    entry->disc_verification = (uint32_t)check->tier;
}
