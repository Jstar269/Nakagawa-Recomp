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

/* ---- the datafile format ------------------------------------------------ */

/* The format both readers accept is written down once, in the module
   documentation of tools/nk_core/disc_verify.py ("The datafile format", rules
   1-10). This reader applies those rules step for step, in the same order and
   with the same message for each refusal, as disc_verify.py's _DatScanner and
   _dat_from_zip do; tests/native/disc_dat_conformance.txt holds both to it. */

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

/* Rule 4: whitespace is space, tab, CR and LF, nothing else. */
static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static char ascii_upper(char c) {
    return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
}

size_t nk_disc_normalize_serials(const char *text, size_t length, char (*out_ids)[10],
                                 size_t max_ids) {
    size_t count = 0;
    size_t i = 0;
    while (i < length && count < max_ids) {
        /* One comma-separated part with space, tab, CR, LF and '-' taken out
           and a-z upper-cased (rule 9); its first nine bytes are the ID. */
        char compact[10];
        size_t used = 0;
        while (i < length && text[i] != ',') {
            char c = text[i++];
            if (is_space(c) || c == '-') continue;
            if (used < 9u) compact[used++] = ascii_upper(c);
        }
        if (i < length) i++; /* the comma */
        bool retail = used == 9u;
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

/* Rule 7: a header field's text, whitespace trimmed and the five named
   references decoded, into out (64 bytes): false when the result is longer
   than NK_DISC_DAT_MAX_HEADER_FIELD bytes or not printable ASCII. */
static bool copy_header_field(const char *p, const char *end, char out[64]) {
    static const struct { const char *name; char c; } named[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''},
    };
    size_t used = 0;
    while (p < end && is_space(*p)) p++;
    while (end > p && is_space(end[-1])) end--;
    while (p < end) {
        char c = *p;
        size_t step = 1;
        for (size_t e = 0; c == '&' && e < sizeof(named) / sizeof(named[0]); e++) {
            size_t n = strlen(named[e].name);
            if ((size_t)(end - p) >= n && memcmp(p, named[e].name, n) == 0) {
                c = named[e].c;
                step = n;
                break;
            }
        }
        if (c < 0x20 || c > 0x7E || used == NK_DISC_DAT_MAX_HEADER_FIELD) return false;
        out[used++] = c;
        p += step;
    }
    out[used] = '\0';
    return true;
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

/* ---- the datafile scanner (rules 4-10) ----------------------------------- */

/* One forward pass, never back: each search starts where the last one
   stopped, so the work is linear in the text's length. `examined` counts the
   bytes looked at, each time one is looked at (the serial splitting's passes
   too), so a test can hold any input's cost to a fixed multiple of its
   length. Offsets in messages count from `text`, after a byte-order mark. */
typedef struct {
    const char *text;
    const char *end;
    uint64_t examined;
} DatScan;

/* The longest reference rule 5 allows: "&#1234567;". */
#define NK_DISC_REFERENCE_WINDOW 10u

static unsigned long scan_offset(const DatScan *s, const char *at) {
    return (unsigned long)(at - s->text);
}

/* The first byte c in [from, stop), or NULL. */
static const char *find_byte(const char *from, const char *stop, char c) {
    return from < stop ? (const char *)memchr(from, c, (size_t)(stop - from)) : NULL;
}

/* An '&' at p that rule 5 allows: a named reference, or a decimal (1-7
   digits) or hex (1-6 digits) character reference. */
static bool is_allowed_reference(const char *p, const char *end) {
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

/* Whether every '&' in [from, stop) starts a reference rule 5 allows. */
static bool scan_references_ok(DatScan *s, const char *from, const char *stop) {
    const char *at = find_byte(from, stop, '&');
    s->examined += (uint64_t)((at ? at + 1 : stop) - from);
    while (at) {
        s->examined += NK_DISC_REFERENCE_WINDOW;
        if (!is_allowed_reference(at, s->end)) return false;
        const char *following = at + 1;
        at = find_byte(following, stop, '&');
        s->examined += (uint64_t)((at ? at + 1 : stop) - following);
    }
    return true;
}

/* The first byte in [from, stop) that is not whitespace, or NULL. */
static const char *scan_non_space(DatScan *s, const char *from, const char *stop) {
    const char *q = from;
    while (q < stop && is_space(*q)) q++;
    s->examined += (uint64_t)(stop - from);
    return q < stop ? q : NULL;
}

static bool is_name_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == ':';
}

static bool is_name_char(char c) {
    return is_name_start(c) || (c >= '0' && c <= '9') || c == '.' || c == '-';
}

/* The end of the name (rule 5) at p, before stop: p itself when there is none. */
static const char *scan_name(const char *p, const char *stop) {
    if (p >= stop || !is_name_start(*p)) return p;
    p++;
    while (p < stop && is_name_char(*p)) p++;
    return p;
}

static bool span_is(const char *p, size_t length, const char *want) {
    return strlen(want) == length && memcmp(p, want, length) == 0;
}

static bool scan_fail_at(NkDiscExpandStatus *status, const char *code, const char *format,
                         const DatScan *s, const char *at) {
    return verify_fail(status, code, format, scan_offset(s, at));
}

static bool markup_refused(NkDiscExpandStatus *status) {
    return verify_fail(status, NK_DISC_DAT_ENTITY_REFUSED,
                       "The datafile declares entities, a DTD of its own or markup a Redump list "
                       "does not use, so it was not read.");
}

static bool reference_refused(NkDiscExpandStatus *status) {
    return verify_fail(status, NK_DISC_DAT_ENTITY_REFUSED,
                       "The datafile refers to an entity XML does not predefine; a Redump list "
                       "has none, so it was not read.");
}

static bool no_memory(NkDiscExpandStatus *status) {
    return verify_fail(status, NK_DISC_INTERNAL, "No memory was free to read the datafile.");
}

/* Element kinds (rule 6): the stack holds one per open element. */
typedef enum {
    KIND_OTHER = 0,
    KIND_DATAFILE,
    KIND_HEADER,
    KIND_GAME,
    KIND_NAME,
    KIND_VERSION,
    KIND_DATE,
    KIND_SERIAL,
    KIND_ROM
} DatKind;

typedef struct {
    const char *name;
    size_t length;
    DatKind kind;
} OpenElement;

static bool is_text_field(DatKind kind) {
    return kind == KIND_NAME || kind == KIND_VERSION || kind == KIND_DATE || kind == KIND_SERIAL;
}

static const char k_malformed_tag[] = "The datafile has a malformed tag at offset %lu.";
static const char k_unmatched[] =
    "The datafile has an end tag that does not match its start tag at offset %lu.";
static const char k_repeated_field[] =
    "The datafile repeats a <name>, <version> or <date> in its header, or a <serial> in a game, "
    "at offset %lu.";

static bool dat_scan(DatScan *s, NkDiscDat *out, NkDiscExpandStatus *status) {
    const char *end = s->end;
    OpenElement stack[NK_DISC_DAT_MAX_DEPTH];
    size_t depth = 0;
    uint64_t tags = 0;
    bool root = false, closed = false, doctype = false, header_seen = false, game_seen = false;
    bool header_fields[3] = {false, false, false};
    bool game_serial = false;
    char ids[NK_DISC_DAT_MAX_GAME_SERIALS + 1u][10];
    size_t id_count = 0;
    size_t rom_capacity = 0, serial_capacity = 0;
    const char *field_start = s->text;
    const char *pos = s->text;
    for (;;) {
        const char *lt = find_byte(pos, end, '<');
        const char *stop = lt ? lt : end;
        s->examined += (uint64_t)((lt ? lt + 1 : end) - pos);
        if (depth == 0) {
            const char *text = scan_non_space(s, pos, stop);
            if (text) {
                return scan_fail_at(status, NK_DISC_DAT_NOT_DATAFILE,
                                    "The datafile has text outside its <datafile> element at "
                                    "offset %lu.", s, text);
            }
        } else if (!scan_references_ok(s, pos, stop)) {
            return reference_refused(status);
        }
        if (!lt) break;
        if (++tags > NK_DISC_DAT_MAX_TAGS) {
            return verify_fail(status, NK_DISC_DAT_TOO_LARGE, "The datafile has more than %u tags.",
                               (unsigned)NK_DISC_DAT_MAX_TAGS);
        }
        /* No NUL byte gets this far (rule 3), so '\0' means "none". */
        char marker = lt + 1 < end ? lt[1] : '\0';
        if (marker == '?') {
            if (root) return markup_refused(status);
            const char *q = lt + 2;
            const char *close = NULL;
            for (const char *at = find_byte(q, end, '?'); at; at = find_byte(at + 1, end, '?')) {
                if (at + 1 < end && at[1] == '>') {
                    close = at;
                    break;
                }
            }
            s->examined += (uint64_t)((close ? close + 2 : end) - q);
            if (!close) {
                return verify_fail(status, NK_DISC_DAT_NOT_DATAFILE,
                                   "The datafile is cut short inside a <?...?> declaration.");
            }
            pos = close + 2;
            continue;
        }
        if (marker == '!') {
            s->examined += 9u;
            if (doctype || root || (size_t)(end - lt) < 9u || memcmp(lt, "<!DOCTYPE", 9u) != 0) {
                return markup_refused(status);
            }
            const char *gt = find_byte(lt + 2, end, '>');
            const char *inner = find_byte(lt + 2, gt ? gt : end, '<');
            s->examined += (uint64_t)((gt ? gt + 1 : end) - (lt + 2));
            if (!inner && !gt) {
                return verify_fail(status, NK_DISC_DAT_NOT_DATAFILE,
                                   "The datafile is cut short inside its <!DOCTYPE>.");
            }
            if (inner || find_byte(lt, gt, '[')) return markup_refused(status);
            s->examined += (uint64_t)(gt - lt);
            doctype = true;
            pos = gt + 1;
            continue;
        }
        const char *gt = find_byte(lt + 1, end, '>');
        const char *inner = find_byte(lt + 1, gt ? gt : end, '<');
        s->examined += (uint64_t)((gt ? gt + 1 : end) - (lt + 1));
        if (inner) {
            return scan_fail_at(status, NK_DISC_DAT_NOT_DATAFILE,
                                "The datafile has a '<' inside a tag at offset %lu.", s, lt);
        }
        if (!gt) return verify_fail(status, NK_DISC_DAT_NOT_DATAFILE, "The datafile is cut short inside a tag.");
        if (!scan_references_ok(s, lt, gt)) return reference_refused(status);
        pos = gt + 1;
        /* Each tag's bytes are read once more by the grammar below. */
        s->examined += (uint64_t)(gt - lt);
        DatKind kind = KIND_OTHER;
        bool is_end = marker == '/';
        bool self_closing = false;
        if (is_end) {
            const char *name = lt + 2;
            const char *name_end = scan_name(name, gt);
            const char *q = name_end;
            while (q < gt && is_space(*q)) q++;
            if (name_end == name || q != gt) return scan_fail_at(status, NK_DISC_DAT_NOT_DATAFILE, k_malformed_tag, s, lt);
            if (depth == 0) return scan_fail_at(status, NK_DISC_DAT_NOT_DATAFILE, k_unmatched, s, lt);
            const OpenElement *top = &stack[depth - 1u];
            if (top->length != (size_t)(name_end - name) || memcmp(top->name, name, top->length) != 0) {
                return scan_fail_at(status, NK_DISC_DAT_NOT_DATAFILE, k_unmatched, s, lt);
            }
            kind = top->kind;
            depth--;
        } else {
            const char *name = lt + 1;
            const char *name_end = scan_name(name, gt);
            if (name_end == name) return scan_fail_at(status, NK_DISC_DAT_NOT_DATAFILE, k_malformed_tag, s, lt);
            size_t name_length = (size_t)(name_end - name);
            bool is_rom = span_is(name, name_length, "rom");
            /* A <rom>'s first sha1, crc, size and name (rule 8). */
            static const char *const wanted_names[4] = {"sha1", "crc", "size", "name"};
            Span wanted[4] = {{NULL, NULL}, {NULL, NULL}, {NULL, NULL}, {NULL, NULL}};
            bool found[4] = {false, false, false, false};
            const char *p = name_end;
            for (;;) {
                /* One attribute: whitespace, a name, '=', a value in double quotes. */
                const char *q = p;
                while (q < gt && is_space(*q)) q++;
                if (q == p) break;
                const char *attribute = q;
                q = scan_name(q, gt);
                if (q == attribute) break;
                size_t attribute_length = (size_t)(q - attribute);
                while (q < gt && is_space(*q)) q++;
                if (q >= gt || *q != '=') break;
                q++;
                while (q < gt && is_space(*q)) q++;
                if (q >= gt || *q != '"') break;
                const char *value = q + 1;
                const char *close = find_byte(value, gt, '"');
                if (!close) break;
                for (size_t k = 0; is_rom && k < 4u; k++) {
                    if (!found[k] && span_is(attribute, attribute_length, wanted_names[k])) {
                        wanted[k].p = value;
                        wanted[k].end = close;
                        found[k] = true;
                    }
                }
                p = close + 1;
            }
            const char *q = p;
            while (q < gt && is_space(*q)) q++;
            if (q < gt && *q == '/') {
                self_closing = true;
                q++;
            }
            if (q != gt) return scan_fail_at(status, NK_DISC_DAT_NOT_DATAFILE, k_malformed_tag, s, lt);
            if (depth >= NK_DISC_DAT_MAX_DEPTH) {
                return verify_fail(status, NK_DISC_DAT_NOT_DATAFILE,
                                   "The datafile nests elements more than %u deep at offset %lu.",
                                   (unsigned)NK_DISC_DAT_MAX_DEPTH, scan_offset(s, lt));
            }
            bool has_parent = depth > 0;
            DatKind parent = has_parent ? stack[depth - 1u].kind : KIND_OTHER;
            if (has_parent && is_text_field(parent)) {
                return scan_fail_at(status, NK_DISC_DAT_NOT_DATAFILE,
                                    "The datafile has an element inside a <name>, <version>, "
                                    "<date> or <serial> at offset %lu.", s, lt);
            }
            if (!has_parent) {
                if (!span_is(name, name_length, "datafile")) {
                    return scan_fail_at(status, NK_DISC_DAT_NOT_DATAFILE,
                                        "The datafile's first element is not <datafile> (offset "
                                        "%lu).", s, lt);
                }
                kind = KIND_DATAFILE;
                root = true;
            } else if (parent == KIND_DATAFILE && span_is(name, name_length, "header")) {
                if (header_seen || game_seen) {
                    return scan_fail_at(status, NK_DISC_DAT_NOT_DATAFILE,
                                        "The datafile has a second <header>, or one after a "
                                        "<game>, at offset %lu.", s, lt);
                }
                kind = KIND_HEADER;
                header_seen = true;
            } else if (parent == KIND_DATAFILE && span_is(name, name_length, "game")) {
                kind = KIND_GAME;
                game_seen = true;
                game_serial = false;
                id_count = 0;
            } else if (parent == KIND_HEADER && (span_is(name, name_length, "name") ||
                                                 span_is(name, name_length, "version") ||
                                                 span_is(name, name_length, "date"))) {
                kind = name[0] == 'n' ? KIND_NAME : name[0] == 'v' ? KIND_VERSION : KIND_DATE;
                size_t field = (size_t)(kind - KIND_NAME);
                if (header_fields[field]) return scan_fail_at(status, NK_DISC_DAT_NOT_DATAFILE, k_repeated_field, s, lt);
                header_fields[field] = true;
            } else if (parent == KIND_GAME && span_is(name, name_length, "serial")) {
                if (game_serial) return scan_fail_at(status, NK_DISC_DAT_NOT_DATAFILE, k_repeated_field, s, lt);
                kind = KIND_SERIAL;
                game_serial = true;
            } else if (parent == KIND_GAME && is_rom) {
                if (!self_closing) {
                    return scan_fail_at(status, NK_DISC_DAT_NOT_DATAFILE,
                                        "The datafile has a <rom> that is not empty (<rom .../>) "
                                        "at offset %lu.", s, lt);
                }
                kind = KIND_ROM;
                NkDiscDatRom entry;
                uint8_t crc_bytes[4];
                memset(&entry, 0, sizeof(entry));
                if (found[0] && found[1] && found[2] && parse_hex_bytes(&wanted[0], entry.sha1, 20u) &&
                    parse_hex_bytes(&wanted[1], crc_bytes, 4u) &&
                    parse_decimal(&wanted[2], &entry.size_bytes)) {
                    if (out->rom_count >= NK_DISC_DAT_MAX_ROMS) {
                        return verify_fail(status, NK_DISC_DAT_TOO_LARGE,
                                           "The datafile lists more than %u discs.",
                                           (unsigned)NK_DISC_DAT_MAX_ROMS);
                    }
                    entry.crc32 = ((uint32_t)crc_bytes[0] << 24) | ((uint32_t)crc_bytes[1] << 16) |
                                  ((uint32_t)crc_bytes[2] << 8) | (uint32_t)crc_bytes[3];
                    entry.listed = (uint32_t)out->rom_count;
                    if (!dat_push_rom(out, &rom_capacity, &entry)) return no_memory(status);
                } else {
                    out->rejected_roms++;
                }
            } else if (span_is(name, name_length, "datafile") || span_is(name, name_length, "header") ||
                       span_is(name, name_length, "game") || is_rom ||
                       span_is(name, name_length, "serial")) {
                return scan_fail_at(status, NK_DISC_DAT_NOT_DATAFILE,
                                    "The datafile has a <datafile>, <header>, <game>, <rom> or "
                                    "<serial> element out of place at offset %lu.", s, lt);
            } else {
                kind = KIND_OTHER;
            }
            field_start = pos;
            if (!self_closing) {
                stack[depth].name = name;
                stack[depth].length = name_length;
                stack[depth].kind = kind;
                depth++;
                continue;
            }
        }
        /* An element closes: its end tag, or a start tag that ends in "/>". */
        const char *raw = is_end ? field_start : lt;
        if (kind == KIND_SERIAL) {
            size_t length = (size_t)(lt - raw);
            if (length > NK_DISC_DAT_MAX_SERIAL_BYTES) {
                return verify_fail(status, NK_DISC_DAT_NOT_DATAFILE,
                                   "The datafile has a <serial> longer than %u bytes at offset %lu.",
                                   (unsigned)NK_DISC_DAT_MAX_SERIAL_BYTES, scan_offset(s, lt));
            }
            /* Three passes over the value, as disc_verify.py counts them. */
            s->examined += 3u * (uint64_t)length;
            id_count = nk_disc_normalize_serials(raw, length, ids, NK_DISC_DAT_MAX_GAME_SERIALS + 1u);
            if (id_count > NK_DISC_DAT_MAX_GAME_SERIALS) {
                return verify_fail(status, NK_DISC_DAT_NOT_DATAFILE,
                                   "The datafile has a <serial> naming more than %u disc IDs at "
                                   "offset %lu.", (unsigned)NK_DISC_DAT_MAX_GAME_SERIALS,
                                   scan_offset(s, lt));
            }
        } else if (kind == KIND_NAME || kind == KIND_VERSION || kind == KIND_DATE) {
            char *dest = kind == KIND_NAME ? out->name : kind == KIND_VERSION ? out->version : out->date;
            if (!copy_header_field(raw, lt, dest)) {
                return verify_fail(status, NK_DISC_DAT_NOT_DATAFILE,
                                   "The datafile's header has a <name>, <version> or <date> that "
                                   "is longer than %u bytes or not printable ASCII, at offset %lu.",
                                   (unsigned)NK_DISC_DAT_MAX_HEADER_FIELD, scan_offset(s, lt));
            }
        } else if (kind == KIND_GAME) {
            out->games++;
            if (out->serial_count + id_count > NK_DISC_DAT_MAX_SERIALS) {
                return verify_fail(status, NK_DISC_DAT_TOO_LARGE,
                                   "The datafile names more than %u disc IDs.",
                                   (unsigned)NK_DISC_DAT_MAX_SERIALS);
            }
            for (size_t k = 0; k < id_count; k++) {
                if (!dat_push_serial(out, &serial_capacity, ids[k])) return no_memory(status);
            }
            id_count = 0;
        } else if (kind == KIND_DATAFILE) {
            closed = true;
            const char *text = scan_non_space(s, pos, end);
            if (text) {
                return scan_fail_at(status, NK_DISC_DAT_NOT_DATAFILE,
                                    "The datafile goes on after its </datafile>, at offset %lu.", s, text);
            }
            break;
        }
    }
    if (!closed) {
        return verify_fail(status, NK_DISC_DAT_NOT_DATAFILE, "%s",
                           root ? "The datafile is cut short: it has no closing </datafile>."
                                : "This file isn't a Redump datafile: it has no <datafile> element.");
    }
    return true;
}

bool nk_disc_dat_parse_counted(const char *text, size_t length, NkDiscDat *out,
                               NkDiscExpandStatus *status, uint64_t *bytes_examined) {
    DatScan scan = {NULL, NULL, 0u};
    bool ok = false;
    verify_status_clear(status);
    if (bytes_examined) *bytes_examined = 0;
    if (!out) return verify_fail(status, NK_DISC_INTERNAL, "No datafile result was given.");
    memset(out, 0, sizeof(*out));
    if (!text) return verify_fail(status, NK_DISC_INTERNAL, "No datafile text was given.");
    /* Rule 3: one byte-order mark skipped; UTF-8; then no control byte. */
    size_t skip = length >= 3u && memcmp(text, "\xEF\xBB\xBF", 3u) == 0 ? 3u : 0u;
    scan.text = text + skip;
    scan.end = text + length;
    scan.examined = 2u * (uint64_t)(length - skip); /* the two passes below */
    const char *control = NULL;
    if (!nk_json_validate_utf8((const uint8_t *)scan.text, length - skip)) {
        ok = verify_fail(status, NK_DISC_DAT_NOT_DATAFILE, "The datafile isn't UTF-8 text.");
    } else {
        for (const char *p = scan.text; p < scan.end && !control; p++) {
            unsigned char c = (unsigned char)*p;
            if ((c < 0x20u && c != 0x09u && c != 0x0Au && c != 0x0Du) || c == 0x7Fu) control = p;
        }
        if (control) {
            ok = verify_fail(status, NK_DISC_DAT_NOT_DATAFILE,
                             "The datafile holds a control character (byte 0x%02X) at offset %lu.",
                             (unsigned)(unsigned char)*control, scan_offset(&scan, control));
        } else {
            ok = dat_scan(&scan, out, status);
        }
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

/* ---- the zip Redump serves (rule 2) -------------------------------------- */

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

/* Whether a zip extra field is made of whole blocks (*whole), and whether one
   is ZIP64 (0x0001); the walk stops at the first block that is ZIP64 or does
   not fit, as disc_verify.py's _extra_is_whole_without_zip64 does. */
static void zip_extra_check(const uint8_t *extra, size_t length, bool *whole, bool *zip64) {
    size_t at = 0;
    *whole = true;
    *zip64 = false;
    while (at < length) {
        if (length - at < 4u || length - at - 4u < read_le16(extra + at + 2u)) {
            *whole = false;
            return;
        }
        if (read_le16(extra + at) == 0x0001u) {
            *zip64 = true;
            return;
        }
        at += 4u + read_le16(extra + at + 2u);
    }
}

/* The one .dat inside a zip, under the format's rule 2, step for step as
   disc_verify.py's _dat_from_zip takes them. Every offset and size is checked
   against the bytes held before it is used. *out_text is malloc'd. */
static bool dat_text_from_zip(const uint8_t *zip, size_t size, char **out_text, size_t *out_length,
                              NkDiscExpandStatus *status) {
    enum { EOCD = 22u, CENTRAL = 46u, LOCAL = 30u };
    static const char disagrees[] = "its file's header disagrees with its central directory.";
    *out_text = NULL;
    *out_length = 0;
    if (size < EOCD) return zip_fail(status, "it is cut short.");
    size_t eocd = size - EOCD;
    const uint8_t *end_record = zip + eocd;
    if (read_le32(end_record) != 0x06054b50u || read_le16(end_record + 20u) != 0u) {
        return zip_fail(status, "its last 22 bytes are not its end record (it has a comment, or "
                                "data after it).");
    }
    if (read_le16(end_record + 4u) != 0u || read_le16(end_record + 6u) != 0u) {
        return zip_fail(status, "it spans several disks.");
    }
    if (read_le16(end_record + 8u) != 1u || read_le16(end_record + 10u) != 1u) {
        return zip_fail(status, "it does not hold exactly one file.");
    }
    uint32_t central_size = read_le32(end_record + 12u);
    uint32_t central_offset = read_le32(end_record + 16u);
    if ((uint64_t)central_offset + central_size != eocd) {
        return zip_fail(status, "its central directory is not where its end record says.");
    }
    const uint8_t *central = zip + central_offset;
    if (central_size < CENTRAL || read_le32(central) != 0x02014b50u ||
        (uint64_t)CENTRAL + read_le16(central + 28u) + read_le16(central + 30u) +
                read_le16(central + 32u) != central_size) {
        return zip_fail(status, "its central directory is malformed.");
    }
    uint16_t flags = read_le16(central + 8u);
    uint16_t method = read_le16(central + 10u);
    uint32_t crc = read_le32(central + 16u);
    uint32_t packed_size = read_le32(central + 20u);
    uint32_t plain_size = read_le32(central + 24u);
    size_t name_length = read_le16(central + 28u);
    size_t extra_length = read_le16(central + 30u);
    uint32_t local_offset = read_le32(central + 42u);
    if (packed_size == 0xFFFFFFFFu || plain_size == 0xFFFFFFFFu || local_offset == 0xFFFFFFFFu) {
        return zip_fail(status, "it uses ZIP64.");
    }
    bool whole, zip64;
    zip_extra_check(central + CENTRAL + name_length, extra_length, &whole, &zip64);
    if (!whole) return zip_fail(status, "its central directory is malformed.");
    if (zip64) return zip_fail(status, "it uses ZIP64.");
    if (flags & 0x41u) return zip_fail(status, "the file in it is encrypted.");
    if (flags & ~0x0806u) {
        return verify_fail(status, NK_DISC_DAT_ZIP_INVALID,
                           "The zip can't be read as a Redump datafile: the file in it uses a zip "
                           "feature Redump's zip does not (flags 0x%04X).", (unsigned)flags);
    }
    if (method != 0u && method != 8u) {
        return zip_fail(status, "the file in it is compressed with a method other than deflate.");
    }
    const uint8_t *name = central + CENTRAL;
    if (memchr(name, 0, name_length) || name_length < 4u ||
        memcmp(name + name_length - 4u, ".dat", 4u) != 0) {
        return zip_fail(status, "the file in it is not a .dat file.");
    }
    if (plain_size > NK_DISC_DAT_MAX_BYTES) {
        return verify_fail(status, NK_DISC_DAT_TOO_LARGE,
                           "The datafile in the zip is larger than %u bytes.",
                           (unsigned)NK_DISC_DAT_MAX_BYTES);
    }
    if (local_offset != 0u) return zip_fail(status, "its file does not start the zip.");
    if (central_offset < LOCAL) return zip_fail(status, disagrees);
    size_t local_name_length = read_le16(zip + 26u);
    size_t local_extra_length = read_le16(zip + 28u);
    if (read_le32(zip) != 0x04034b50u || read_le16(zip + 6u) != flags ||
        read_le16(zip + 8u) != method || read_le32(zip + 14u) != crc ||
        read_le32(zip + 18u) != packed_size || read_le32(zip + 22u) != plain_size ||
        local_name_length != name_length ||
        (uint64_t)LOCAL + local_name_length + local_extra_length > central_offset ||
        memcmp(zip + LOCAL, name, name_length) != 0) {
        return zip_fail(status, disagrees);
    }
    zip_extra_check(zip + LOCAL + local_name_length, local_extra_length, &whole, &zip64);
    if (!whole || zip64) return zip_fail(status, disagrees);
    size_t data_start = LOCAL + local_name_length + local_extra_length;
    if ((uint64_t)data_start + packed_size != central_offset) {
        return zip_fail(status, "its file's data does not end where its central directory starts.");
    }
    const uint8_t *packed = zip + data_start;
    if (method == 0u && packed_size != plain_size) {
        return zip_fail(status, "its stored file's sizes differ.");
    }
    uint8_t *plain = (uint8_t *)malloc((size_t)plain_size + 1u);
    if (!plain) return no_memory(status);
    size_t plain_length = plain_size;
    if (method == 0u) {
        memcpy(plain, packed, plain_size);
    } else {
        /* Inflated into exactly the declared size: a stream that would write
           more is refused where it would, before what follows is read. */
        size_t consumed = 0;
        char error[128];
        int rc = nk_psp_inflate_raw_zlib(packed, packed_size, plain, plain_size, &plain_length,
                                         &consumed, error, sizeof(error));
        const char *reason = NULL;
        if (rc == NK_INFLATE_ERR_MEMORY) {
            free(plain);
            return no_memory(status);
        }
        if (rc == NK_INFLATE_ERR_OVERFLOW) {
            reason = "its file inflates past its declared size.";
        } else if (rc != NK_INFLATE_OK) {
            reason = "its file does not inflate.";
        } else if (consumed != packed_size) {
            reason = "its file's compressed data goes on past its end.";
        }
        if (reason) {
            free(plain);
            return zip_fail(status, reason);
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

/* A datafile's own text (rules 1-2): the bytes given, or the .dat inside them
   when they are a zip (then *owned is that malloc'd text, else NULL). */
static bool dat_text(const uint8_t *bytes, size_t length, const char **out_text, size_t *out_length,
                     char **owned, NkDiscExpandStatus *status) {
    *owned = NULL;
    *out_text = (const char *)bytes;
    *out_length = length;
    if (length > NK_DISC_DAT_MAX_BYTES) {
        return verify_fail(status, NK_DISC_DAT_TOO_LARGE, "The datafile is larger than %u bytes.",
                           (unsigned)NK_DISC_DAT_MAX_BYTES);
    }
    if (length < 4u || memcmp(bytes, "PK\x03\x04", 4u) != 0) return true;
    if (!dat_text_from_zip(bytes, length, owned, out_length, status)) return false;
    *out_text = *owned;
    return true;
}

bool nk_disc_dat_read(const uint8_t *bytes, size_t length, NkDiscDat *out,
                      NkDiscExpandStatus *status) {
    const char *text = NULL;
    size_t text_length = 0;
    char *owned = NULL;
    verify_status_clear(status);
    if (out) memset(out, 0, sizeof(*out));
    if (!bytes || !out) return verify_fail(status, NK_DISC_INTERNAL, "No datafile was given.");
    if (!dat_text(bytes, length, &text, &text_length, &owned, status)) return false;
    bool ok = nk_disc_dat_parse(text, text_length, out, status);
    free(owned);
    return ok;
}

/* A file's bytes, read once and refused over NK_DISC_DAT_MAX_BYTES (rule 1).
   *out_bytes is malloc'd. */
static bool read_bounded_file(const char *path, uint8_t **out_bytes, size_t *out_length,
                              NkDiscExpandStatus *status) {
    *out_bytes = NULL;
    *out_length = 0;
    FILE *f = nk_fopen_utf8(path, "rb");
    if (!f) return verify_fail(status, NK_DISC_DAT_READ_FAILED, "The datafile can't be opened.");
    uint8_t *buffer = (uint8_t *)malloc((size_t)NK_DISC_DAT_MAX_BYTES + 1u);
    if (!buffer) {
        fclose(f);
        return no_memory(status);
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
    *out_bytes = buffer;
    *out_length = got;
    return true;
}

bool nk_disc_dat_load(const char *path, NkDiscDat *out, NkDiscExpandStatus *status) {
    uint8_t *bytes = NULL;
    size_t length = 0;
    verify_status_clear(status);
    if (out) memset(out, 0, sizeof(*out));
    if (!path || !path[0] || !out) {
        return verify_fail(status, NK_DISC_INTERNAL, "No datafile was given.");
    }
    if (!read_bounded_file(path, &bytes, &length, status)) return false;
    bool ok = nk_disc_dat_read(bytes, length, out, status);
    free(bytes);
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
    uint8_t *bytes = NULL;
    size_t length = 0;
    const char *text = NULL;
    size_t text_length = 0;
    char *owned = NULL;
    char target[NK_MAX_PATH];
    char dir[NK_MAX_PATH];
    char tmp[NK_MAX_PATH + 8];
    NkDiscDat parsed;
    verify_status_clear(status);
    if (out) memset(out, 0, sizeof(*out));
    if (!source || !source[0] || !nk_disc_dat_path(data_root, target, sizeof(target))) {
        return verify_fail(status, NK_DISC_INTERNAL, "No datafile or data folder was given.");
    }
    if (!read_bounded_file(source, &bytes, &length, status)) return false;
    /* From a zip, the .dat inside is what is checked and kept. */
    if (!dat_text(bytes, length, &text, &text_length, &owned, status) ||
        !nk_disc_dat_parse(text, text_length, &parsed, status)) {
        free(owned);
        free(bytes);
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
            ok = fwrite(text, 1, text_length, f) == text_length;
            ok = (fclose(f) == 0) && ok;
        }
        if (ok) ok = nk_rename_utf8(tmp, target) == 0;
        if (!ok) (void)nk_remove_utf8(tmp);
    }
    free(owned);
    free(bytes);
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

/* Rule 9: an image's disc ID matches when it is the same nine bytes after
   ASCII upper-casing. */
bool nk_disc_dat_has_serial(const NkDiscDat *dat, const char *disc_id) {
    char key[10];
    if (!dat || !dat->serials || !disc_id || strlen(disc_id) != 9u) return false;
    for (size_t i = 0; i < 9u; i++) key[i] = ascii_upper(disc_id[i]);
    key[9] = '\0';
    return bsearch(key, dat->serials, dat->serial_count, sizeof(dat->serials[0]),
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
                     "Not checked: your imported Redump list can't be used (%s). Import a list "
                     "again, or delete verification/redump.dat from your data folder to go "
                     "back to the list bundled with Nakagawa.", status.code);
        } else {
            snprintf(out->reason, sizeof(out->reason),
                     "Not checked: the Redump list bundled with Nakagawa can't be used (%s). "
                     "Reinstall Nakagawa, or import a list.", status.code);
        }
        return true;
    }
    copy_printable(dat.date[0] ? dat.date : dat.version, out->list_date, sizeof(out->list_date));
    char described[384];
    snprintf(described, sizeof(described), "%s (%s%.63s%s%u discs)%s%.63s%s",
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
