/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/*
 * Disc verification (src/core/nk_disc_verify.c) and the library keyed by disc
 * ID plus content hash (src/core/nk_library.c). First the conformance table
 * both datafile readers are held to (tests/native/disc_dat_conformance.txt,
 * written and checked by tools/test_disc_verify.py: its cases, and seeded
 * mutations of the bundled list); then, on synthetic data only: the
 * SHA-1 and CRC-32 known answers, the whole-image hash with progress and
 * cancel, the Redump datafile reader, the three tiers, the import into the
 * data folder, Redump's zip, the refusal of entity-bearing lists, the list
 * bundled with Nakagawa (the one real list read: assets/redump, from the
 * checkout) and an import overriding it, and library files: one written before hashing still loads, a
 * fan translation and its retail disc are two records, the modified one
 * labelled. The CSO/ZSO/CHD round trips in test_disc_expand.c check that an
 * expansion hashes the same as its .iso.
 */

#define _POSIX_C_SOURCE 200809L

#include "native_test_isolation.h"
#include "nk_disc_verify.h"
#include "nk_library.h"
#include "nk_platform.h"
#include "nk_psp_inflate.h"
#include "nk_zip.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

static void path_in(char *out, size_t size, const char *name) {
    int n = snprintf(out, size, "%s%c%s", native_test_get_root(), nk_platform_path_separator(),
                     name);
    CHECK(n > 0 && (size_t)n < size);
}

static void write_bytes(const char *path, const void *data, size_t size) {
    FILE *f = nk_fopen_utf8(path, "wb");
    CHECK(f != NULL);
    CHECK(fwrite(data, 1, size, f) == size);
    CHECK(fclose(f) == 0);
}

static char *read_text(const char *path) {
    FILE *f = nk_fopen_utf8(path, "rb");
    CHECK(f != NULL);
    CHECK(fseek(f, 0, SEEK_END) == 0);
    long size = ftell(f);
    CHECK(size >= 0 && fseek(f, 0, SEEK_SET) == 0);
    char *text = (char *)malloc((size_t)size + 1u);
    CHECK(text != NULL);
    CHECK(fread(text, 1, (size_t)size, f) == (size_t)size);
    fclose(f);
    text[size] = '\0';
    return text;
}

static void sha1_hex(const uint8_t *data, size_t len, char out[41]) {
    static const char hex[] = "0123456789abcdef";
    NkSha1 ctx;
    uint8_t digest[20];
    nk_sha1_init(&ctx);
    nk_sha1_update(&ctx, data, len);
    nk_sha1_finish(&ctx, digest);
    for (int i = 0; i < 20; i++) {
        out[2 * i] = hex[digest[i] >> 4];
        out[2 * i + 1] = hex[digest[i] & 15];
    }
    out[40] = '\0';
}

static bool is_xml_space(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* ---- one table of cases for both readers ------------------------------ */

/* tests/native/disc_dat_conformance.txt: inputs and the result each must
   give, written by tools/test_disc_verify.py from the Python reader, whose
   suite checks it too. This reader must give the same result for every case
   and every mutation, so neither reader can change alone. The file's header
   says how a case is written; the pieces and the mutations below follow
   tools/test_disc_verify.py's build_pieces and mutation step for step. */

#define CORPUS_PATH "tests/native/disc_dat_conformance.txt"

typedef struct {
    uint8_t *data;
    size_t size;
    size_t cap;
} Buf;

static void buf_reserve(Buf *b, size_t more) {
    if (more <= b->cap - b->size) return;
    CHECK(more <= SIZE_MAX - b->size);
    size_t cap = b->cap ? b->cap : 256u;
    while (cap < b->size + more) cap *= 2u;
    uint8_t *grown = (uint8_t *)realloc(b->data, cap);
    CHECK(grown != NULL);
    b->data = grown;
    b->cap = cap;
}

static void buf_add(Buf *b, const void *p, size_t n) {
    if (n == 0) return;
    buf_reserve(b, n);
    memcpy(b->data + b->size, p, n);
    b->size += n;
}

static void buf_add_byte(Buf *b, uint8_t c) {
    buf_add(b, &c, 1u);
}

static void buf_free(Buf *b) {
    free(b->data);
    memset(b, 0, sizeof(*b));
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* A piece's text: \\ \n \t \r and \xHH escaped, every other byte itself. */
static void unescape_into(Buf *out, const char *text) {
    for (const char *p = text; *p; p++) {
        if (*p != '\\') {
            buf_add_byte(out, (uint8_t)*p);
            continue;
        }
        p++;
        if (*p == '\\') buf_add_byte(out, '\\');
        else if (*p == 'n') buf_add_byte(out, '\n');
        else if (*p == 't') buf_add_byte(out, '\t');
        else if (*p == 'r') buf_add_byte(out, '\r');
        else {
            CHECK(*p == 'x' && hex_nibble(p[1]) >= 0 && hex_nibble(p[2]) >= 0);
            buf_add_byte(out, (uint8_t)(hex_nibble(p[1]) * 16 + hex_nibble(p[2])));
            p += 2;
        }
    }
}

/* One final fixed-Huffman deflate block (RFC 1951 3.2.6), as
   tools/test_disc_verify.py's fixed_deflate writes it: each byte a literal; a
   run of 259 or more of one byte, a literal and as many length-258
   distance-1 copies as fit; then the end of the block. */
typedef struct {
    Buf *out;
    uint32_t acc;
    unsigned bits;
} BitWriter;

static void bits_put(BitWriter *w, uint32_t value, unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        w->acc |= ((value >> i) & 1u) << w->bits;
        if (++w->bits == 8u) {
            buf_add_byte(w->out, (uint8_t)w->acc);
            w->acc = 0;
            w->bits = 0;
        }
    }
}

/* A Huffman code, most significant bit first. */
static void bits_code(BitWriter *w, unsigned symbol) {
    uint32_t code;
    unsigned length;
    if (symbol < 144u) {
        code = 0x30u + symbol;
        length = 8u;
    } else if (symbol < 256u) {
        code = 0x190u + symbol - 144u;
        length = 9u;
    } else if (symbol < 280u) {
        code = symbol - 256u;
        length = 7u;
    } else {
        code = 0xC0u + symbol - 280u;
        length = 8u;
    }
    for (unsigned i = length; i-- > 0;) bits_put(w, (code >> i) & 1u, 1u);
}

static void fixed_deflate(const uint8_t *data, size_t size, Buf *out) {
    BitWriter w = {out, 0u, 0u};
    bits_put(&w, 1u, 1u); /* BFINAL */
    bits_put(&w, 1u, 2u); /* BTYPE 01 */
    size_t at = 0;
    while (at < size) {
        size_t run = 1;
        while (at + run < size && data[at + run] == data[at]) run++;
        if (run < 259u) {
            for (size_t i = 0; i < run; i++) bits_code(&w, data[at]);
            at += run;
            continue;
        }
        bits_code(&w, data[at]);
        size_t copies = (run - 1u) / 258u;
        for (size_t i = 0; i < copies; i++) {
            bits_code(&w, 285u); /* length 258 */
            bits_put(&w, 0u, 5u); /* distance code 0: distance 1 */
        }
        at += 1u + copies * 258u;
    }
    bits_code(&w, 256u);
    if (w.bits) buf_add_byte(out, (uint8_t)w.acc);
}

static void put_le16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v) {
    put_le16(p, v & 0xFFFFu);
    put_le16(p + 2, v >> 16);
}

static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The zip the mutations start from (tools/test_disc_verify.py's zip_bytes):
   one entry, list.dat, version 20, no flags, time 0, date 0x21. */
static void zip_bytes(const uint8_t *payload, size_t size, unsigned method, Buf *out) {
    Buf packed = {NULL, 0, 0};
    if (method == 8u) fixed_deflate(payload, size, &packed);
    else buf_add(&packed, payload, size);
    uint32_t crc = nk_crc32_update(0u, payload, size);
    uint8_t local[38], central[54], end[22];
    memset(local, 0, sizeof(local));
    memset(central, 0, sizeof(central));
    memset(end, 0, sizeof(end));
    put_le32(local, 0x04034B50u);
    put_le16(local + 4, 20u);
    put_le16(local + 8, method);
    put_le16(local + 12, 0x21u);
    put_le32(local + 14, crc);
    put_le32(local + 18, (uint32_t)packed.size);
    put_le32(local + 22, (uint32_t)size);
    put_le16(local + 26, 8u);
    memcpy(local + 30, "list.dat", 8u);
    put_le32(central, 0x02014B50u);
    put_le16(central + 4, 20u);
    put_le16(central + 6, 20u);
    put_le16(central + 10, method);
    put_le16(central + 14, 0x21u);
    put_le32(central + 16, crc);
    put_le32(central + 20, (uint32_t)packed.size);
    put_le32(central + 24, (uint32_t)size);
    put_le16(central + 28, 8u);
    memcpy(central + 46, "list.dat", 8u);
    put_le32(end, 0x06054B50u);
    put_le16(end + 8, 1u);
    put_le16(end + 10, 1u);
    put_le32(end + 12, (uint32_t)sizeof(central));
    put_le32(end + 16, (uint32_t)(sizeof(local) + packed.size));
    buf_add(out, local, sizeof(local));
    buf_add(out, packed.data, packed.size);
    buf_add(out, central, sizeof(central));
    buf_add(out, end, sizeof(end));
    buf_free(&packed);
}

/* What the reader made of one input, as tools/test_disc_verify.py's outcome
   writes it: the counts and the SHA-1 of the list read, or the refusal. */
static void outcome_line(const uint8_t *data, size_t size, char *line, size_t line_size) {
    NkDiscDat dat;
    NkDiscExpandStatus status;
    if (!nk_disc_dat_read(data, size, &dat, &status)) {
        snprintf(line, line_size, "%s %s", status.code, status.message);
        return;
    }
    Buf canonical = {NULL, 0, 0};
    char row[160];
    snprintf(row, sizeof(row), "%lu\n%lu\n%lu\n", (unsigned long)dat.games,
             (unsigned long)dat.rom_count, (unsigned long)dat.rejected_roms);
    buf_add(&canonical, row, strlen(row));
    const char *const header[3] = {dat.name, dat.version, dat.date};
    for (size_t i = 0; i < 3u; i++) {
        buf_add(&canonical, header[i], strlen(header[i]));
        buf_add_byte(&canonical, '\n');
    }
    for (size_t i = 0; i < dat.serial_count; i++) {
        snprintf(row, sizeof(row), "serial %s\n", dat.serials[i]);
        buf_add(&canonical, row, strlen(row));
    }
    for (size_t i = 0; i < dat.rom_count; i++) {
        char sha1[41];
        static const char hex[] = "0123456789abcdef";
        for (size_t k = 0; k < 20u; k++) {
            sha1[2u * k] = hex[dat.roms[i].sha1[k] >> 4];
            sha1[2u * k + 1u] = hex[dat.roms[i].sha1[k] & 15u];
        }
        sha1[40] = '\0';
        snprintf(row, sizeof(row), "rom %s %08lx %llu %lu\n", sha1,
                 (unsigned long)dat.roms[i].crc32, (unsigned long long)dat.roms[i].size_bytes,
                 (unsigned long)dat.roms[i].listed);
        buf_add(&canonical, row, strlen(row));
    }
    char digest[41];
    sha1_hex(canonical.data, canonical.size, digest);
    snprintf(line, line_size, "OK %lu %lu %lu %lu %s", (unsigned long)dat.games,
             (unsigned long)dat.rom_count, (unsigned long)dat.rejected_roms,
             (unsigned long)dat.serial_count, digest);
    buf_free(&canonical);
    nk_disc_dat_free(&dat);
}

/* Registers a case's pieces may fill (reg NAME ... out). */
typedef struct {
    char name[32];
    Buf buf;
} Register;

#define MAX_REGISTERS 8u

static Buf *register_named(Register *regs, size_t *count, const char *name, bool fresh) {
    for (size_t i = 0; i < *count; i++) {
        if (strcmp(regs[i].name, name) == 0) {
            if (fresh) regs[i].buf.size = 0;
            return &regs[i].buf;
        }
    }
    CHECK(fresh && *count < MAX_REGISTERS && strlen(name) < sizeof(regs[0].name));
    snprintf(regs[*count].name, sizeof(regs[0].name), "%s", name);
    memset(&regs[*count].buf, 0, sizeof(Buf));
    return &regs[(*count)++].buf;
}

/* One piece line of a case, into *target (or a register change). */
static void build_piece(const char *line, Buf *output, Buf **target, Register *regs,
                        size_t *reg_count) {
    if (strcmp(line, "t") == 0) return;
    if (strncmp(line, "t ", 2) == 0) {
        unescape_into(*target, line + 2);
    } else if (strncmp(line, "h ", 2) == 0) {
        for (const char *p = line + 2; *p; p += 2) {
            CHECK(hex_nibble(p[0]) >= 0 && hex_nibble(p[1]) >= 0);
            buf_add_byte(*target, (uint8_t)(hex_nibble(p[0]) * 16 + hex_nibble(p[1])));
        }
    } else if (strncmp(line, "r ", 2) == 0 || strncmp(line, "seq ", 4) == 0) {
        bool seq = line[0] == 's';
        char *rest = NULL;
        unsigned long count = strtoul(line + (seq ? 4 : 2), &rest, 10);
        CHECK(rest && *rest == ' ');
        Buf unit = {NULL, 0, 0};
        unescape_into(&unit, rest + 1);
        if (!seq) {
            buf_reserve(*target, unit.size * count);
            for (unsigned long i = 0; i < count; i++) buf_add(*target, unit.data, unit.size);
        }
        for (unsigned long i = 0; seq && i < count; i++) {
            for (size_t k = 0; k < unit.size; k++) {
                char field[48];
                if (unit.data[k] != '%' || k + 1u >= unit.size) {
                    buf_add_byte(*target, unit.data[k]);
                    continue;
                }
                char kind = (char)unit.data[++k];
                if (kind == 'H') snprintf(field, sizeof(field), "%040lx", i);
                else if (kind == 'X') snprintf(field, sizeof(field), "%lx", i);
                else if (kind == 'D') snprintf(field, sizeof(field), "%05lu", i % 100000ul);
                else {
                    CHECK(kind == '%');
                    snprintf(field, sizeof(field), "%%");
                }
                buf_add(*target, field, strlen(field));
            }
        }
        buf_free(&unit);
    } else if (strncmp(line, "file ", 5) == 0) {
        FILE *f = nk_fopen_utf8(line + 5, "rb");
        CHECK(f != NULL);
        uint8_t chunk[65536];
        size_t got;
        while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) buf_add(*target, chunk, got);
        fclose(f);
    } else if (strncmp(line, "reg ", 4) == 0) {
        *target = register_named(regs, reg_count, line + 4, true);
    } else if (strcmp(line, "out") == 0) {
        *target = output;
    } else if (strncmp(line, "put ", 4) == 0) {
        Buf *source = register_named(regs, reg_count, line + 4, false);
        buf_add(*target, source->data, source->size);
    } else if (strncmp(line, "deflate ", 8) == 0) {
        Buf *source = register_named(regs, reg_count, line + 8, false);
        fixed_deflate(source->data, source->size, *target);
    } else {
        fprintf(stderr, "unknown corpus piece: %.40s\n", line);
        CHECK(false);
    }
}

/* ---- the seeded mutations ---- */

typedef struct {
    uint64_t state;
} SplitMix64;

static uint64_t rng_next(SplitMix64 *rng) {
    uint64_t z = (rng->state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static size_t rng_below(SplitMix64 *rng, size_t bound) {
    return (size_t)(rng_next(rng) % (uint64_t)bound);
}

typedef struct {
    const uint8_t *bytes;
    size_t size;
} Lit;

#define LIT(s) {(const uint8_t *)(s), sizeof(s) - 1u}

static const uint8_t k_interesting[] = {
    0x00, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x20, 0x21, 0x22, 0x26, 0x27, 0x2c, 0x2d, 0x2f,
    0x3c, 0x3d, 0x3e, 0x3f, 0x5b, 0x5d, 0x7f, 0x80, 0xa0, 0xc2, 0xe3, 0xef, 0xff,
};

static const Lit k_insertions[] = {
    LIT("\xc2\xa0"), LIT("\xe3\x80\x80"), LIT("\xc5\xbf"), LIT("\xc4\xb1"), LIT("\xef\xbb\xbf"),
    LIT("&amp;"), LIT("&#65;"), LIT("&#x41;"), LIT("&bogus;"), LIT("<!--"), LIT("-->"),
    LIT("<?x?>"), LIT("<!DOCTYPE x>"), LIT("<![CDATA["), LIT("</datafile>"), LIT("<datafile>"),
    LIT("<header>"), LIT("</header>"), LIT("<game>"), LIT("</game>"), LIT("<serial>"),
    LIT("</serial>"), LIT("<rom/>"),
    LIT("<rom sha1=\"0123456789abcdef0123456789abcdef01234567\" crc=\"0badf00d\" size=\"1\"/>"),
    LIT("<a>"), LIT("</a>"), LIT(","), LIT("ULUS-10041"), LIT("\xf0\x9f\x98\x80"),
    LIT("\xed\xa0\x80"), LIT("\xc0\xaf"), LIT("\xf4\x90\x80\x80"),
};

static const Lit k_whitespace_swaps[] = {
    LIT("\x0b"), LIT("\x0c"), LIT("\xc2\xa0"), LIT("\xe3\x80\x80"), LIT("\xc2\x85"),
    LIT("\xe2\x80\x8b"), LIT("\x00"), LIT("\x1f"), LIT(""), LIT("  "),
};

/* Replace data[at, at + removed) with insert[0, inserted). */
static void buf_splice(Buf *b, size_t at, size_t removed, const uint8_t *insert, size_t inserted) {
    CHECK(at <= b->size && removed <= b->size - at);
    uint8_t *copy = NULL;
    if (inserted) {
        copy = (uint8_t *)malloc(inserted);
        CHECK(copy != NULL);
        memcpy(copy, insert, inserted); /* insert may point into b */
    }
    if (inserted > removed) buf_reserve(b, inserted - removed);
    memmove(b->data + at + inserted, b->data + at + removed, b->size - at - removed);
    if (inserted) memcpy(b->data + at, copy, inserted);
    b->size = b->size - removed + inserted;
    free(copy);
}

static void mutate_text(SplitMix64 *rng, Buf *data) {
    size_t ops = 1u + rng_below(rng, 4u);
    for (size_t n = 0; n < ops; n++) {
        size_t op = rng_below(rng, 8u);
        size_t size = data->size;
        if (op == 2u) {
            size_t at = rng_below(rng, size + 1u);
            const Lit *seq = &k_insertions[rng_below(rng, sizeof(k_insertions) / sizeof(k_insertions[0]))];
            buf_splice(data, at, 0, seq->bytes, seq->size);
        } else if (size == 0) {
            continue;
        } else if (op == 0u) {
            size_t at = rng_below(rng, size);
            data->data[at] ^= (uint8_t)(1u << rng_below(rng, 8u));
        } else if (op == 1u) {
            size_t at = rng_below(rng, size);
            data->data[at] = k_interesting[rng_below(rng, sizeof(k_interesting))];
        } else if (op == 3u) {
            size_t at = rng_below(rng, size);
            size_t count = 1u + rng_below(rng, 16u);
            buf_splice(data, at, count < size - at ? count : size - at, NULL, 0);
        } else if (op == 4u) {
            size_t at = rng_below(rng, size);
            size_t count = 1u + rng_below(rng, 64u);
            buf_splice(data, at, 0, data->data + at, count < size - at ? count : size - at);
        } else if (op == 5u) {
            data->size = rng_below(rng, size + 1u);
        } else if (op == 6u) {
            size_t at = rng_below(rng, size);
            unsigned upper = data->data[at] & 0xDFu;
            if (upper >= 0x41u && upper <= 0x5Au) data->data[at] ^= 0x20u;
        } else {
            size_t at = rng_below(rng, size);
            while (at < size && !is_xml_space(data->data[at])) at++;
            if (at < size) {
                const Lit *swap = &k_whitespace_swaps[rng_below(
                    rng, sizeof(k_whitespace_swaps) / sizeof(k_whitespace_swaps[0]))];
                buf_splice(data, at, 1u, swap->bytes, swap->size);
            }
        }
    }
}

typedef struct {
    Buf bundled;
    Buf windows[8];
} MutationBases;

static size_t find_bytes(const uint8_t *hay, size_t size, const char *needle, size_t from) {
    size_t length = strlen(needle);
    for (size_t at = from; at + length <= size; at++) {
        if (hay[at] == (uint8_t)needle[0] && memcmp(hay + at, needle, length) == 0) return at;
    }
    return SIZE_MAX;
}

/* tools/test_disc_verify.py's bundled_windows: the bundled zip, and eight
   short lists cut from its .dat (its header, eight games, </datafile>). */
static void mutation_bases(MutationBases *bases) {
    memset(bases, 0, sizeof(*bases));
    char path[NATIVE_TEST_PATH_MAX];
    CHECK(nk_disc_bundled_dat_path(".", path, sizeof(path)));
    FILE *f = nk_fopen_utf8(path, "rb");
    CHECK(f != NULL);
    uint8_t chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) buf_add(&bases->bundled, chunk, got);
    fclose(f);
    const uint8_t *zip = bases->bundled.data;
    size_t start = 30u + (size_t)(zip[26] | (zip[27] << 8)) + (size_t)(zip[28] | (zip[29] << 8));
    uint32_t packed = get_le32(zip + 18), plain = get_le32(zip + 22);
    uint8_t *text = (uint8_t *)malloc(plain);
    CHECK(text != NULL);
    size_t written = 0, consumed = 0;
    char err[128];
    CHECK(nk_psp_inflate_raw_zlib(zip + start, packed, text, plain, &written, &consumed, err,
                                  sizeof(err)) == NK_INFLATE_OK && written == plain);
    size_t header_end = find_bytes(text, plain, "</header>\n", 0);
    CHECK(header_end != SIZE_MAX);
    header_end += strlen("</header>\n");
    size_t *starts = NULL;
    size_t games = 0, cap = 0;
    for (size_t at = find_bytes(text, plain, "\t<game ", 0); at != SIZE_MAX;
         at = find_bytes(text, plain, "\t<game ", at + 1u)) {
        if (games == cap) {
            cap = cap ? cap * 2u : 4096u;
            starts = (size_t *)realloc(starts, cap * sizeof(*starts));
            CHECK(starts != NULL);
        }
        starts[games++] = at;
    }
    size_t closing = find_bytes(text, plain, "</datafile>", 0);
    CHECK(games > 8u && closing != SIZE_MAX);
    for (size_t w = 0; w < 8u; w++) {
        size_t first = w * (games - 8u) / 7u, last = first + 7u;
        size_t end = last + 1u < games ? starts[last + 1u] : closing;
        buf_add(&bases->windows[w], text, header_end);
        buf_add(&bases->windows[w], text + starts[first], end - starts[first]);
        buf_add(&bases->windows[w], "</datafile>\n", 12u);
    }
    free(starts);
    free(text);
}

/* tools/test_disc_verify.py's mutation(index): the same draws in the same order. */
static void mutation(uint64_t seed, size_t index, const MutationBases *bases, Buf *out) {
    SplitMix64 rng = {(seed << 32) | (uint64_t)index};
    size_t kind = index % 16u;
    out->size = 0;
    if (kind < 10u) {
        const Buf *base = &bases->windows[rng_below(&rng, 8u)];
        buf_add(out, base->data, base->size);
        mutate_text(&rng, out);
    } else if (kind < 12u) {
        const Buf *base = &bases->windows[rng_below(&rng, 8u)];
        zip_bytes(base->data, base->size, 0u, out);
        size_t changes = 1u + rng_below(&rng, 3u);
        for (size_t n = 0; n < changes && out->size > 0; n++) {
            size_t region = rng_below(&rng, 3u), at;
            if (region == 0u) at = rng_below(&rng, out->size < 38u ? out->size : 38u);
            else if (region == 1u) at = out->size - 1u - rng_below(&rng, out->size < 76u ? out->size : 76u);
            else at = rng_below(&rng, out->size);
            size_t change = rng_below(&rng, 3u);
            if (change == 0u) out->data[at] ^= (uint8_t)(1u << rng_below(&rng, 8u));
            else if (change == 1u) out->data[at] = (uint8_t)rng_below(&rng, 256u);
            else out->size = at;
        }
    } else if (kind < 15u) {
        const Buf *base = &bases->windows[rng_below(&rng, 8u)];
        zip_bytes(base->data, base->size, 8u, out);
        uint32_t packed = get_le32(out->data + 18);
        size_t central = 38u + packed;
        size_t changes = 1u + rng_below(&rng, 4u);
        for (size_t n = 0; n < changes; n++) {
            if (rng_below(&rng, 4u) < 3u) {
                size_t at = 38u + rng_below(&rng, packed);
                out->data[at] ^= (uint8_t)(1u << rng_below(&rng, 8u));
            } else {
                uint32_t size = get_le32(out->data + 22) + (uint32_t)rng_below(&rng, 17u) - 8u;
                put_le32(out->data + 22, size);
                put_le32(out->data + central + 24, size);
            }
        }
    } else {
        buf_add(out, bases->bundled.data, bases->bundled.size);
        const uint8_t *zip = out->data;
        size_t start = 30u + (size_t)(zip[26] | (zip[27] << 8)) + (size_t)(zip[28] | (zip[29] << 8));
        uint32_t packed = get_le32(zip + 18);
        size_t flips = 1u + rng_below(&rng, 2u);
        for (size_t n = 0; n < flips; n++) {
            size_t at = start + rng_below(&rng, packed);
            out->data[at] ^= (uint8_t)(1u << rng_below(&rng, 8u));
        }
    }
}

static void test_conformance_corpus(void) {
    FILE *f = nk_fopen_utf8(CORPUS_PATH, "rb");
    CHECK(f != NULL);
    Buf file = {NULL, 0, 0};
    uint8_t chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) buf_add(&file, chunk, got);
    fclose(f);
    buf_add_byte(&file, '\0');

    Buf input = {NULL, 0, 0};
    Buf *target = &input;
    Register regs[MAX_REGISTERS];
    size_t reg_count = 0;
    char name[256] = "";
    char line_out[512];
    size_t cases = 0, failures = 0, mutations = 0;
    bool in_case = false;
    char *next = (char *)file.data;
    while (next && *next) {
        char *line = next;
        char *newline = strchr(line, '\n');
        next = newline ? newline + 1 : NULL;
        if (newline) *newline = '\0';
        if (!in_case && (line[0] == '#' || line[0] == '\0')) continue;
        if (!in_case && strncmp(line, "case ", 5) == 0) {
            snprintf(name, sizeof(name), "%s", line + 5);
            input.size = 0;
            target = &input;
            for (size_t i = 0; i < reg_count; i++) buf_free(&regs[i].buf);
            reg_count = 0;
            in_case = true;
            continue;
        }
        if (in_case && strncmp(line, "expect ", 7) == 0) {
            outcome_line(input.data, input.size, line_out, sizeof(line_out));
            if (strcmp(line_out, line + 7) != 0) {
                fprintf(stderr, "conformance case \"%s\":\n  expected %s\n  got      %s\n", name,
                        line + 7, line_out);
                failures++;
            }
            cases++;
            continue;
        }
        if (in_case && strcmp(line, "end") == 0) {
            in_case = false;
            continue;
        }
        if (in_case) {
            build_piece(line, &input, &target, regs, &reg_count);
            continue;
        }
        /* mutations SEED COUNT, then the result prefixes, eight to a line. */
        CHECK(strncmp(line, "mutations ", 10) == 0);
        char *rest = NULL;
        uint64_t seed = (uint64_t)strtoull(line + 10, &rest, 16);
        size_t count = (size_t)strtoul(rest, NULL, 10);
        MutationBases bases;
        mutation_bases(&bases);
        Buf mutated = {NULL, 0, 0};
        while (next && strncmp(next, "end", 3) != 0) {
            line = next;
            newline = strchr(line, '\n');
            next = newline ? newline + 1 : NULL;
            if (newline) *newline = '\0';
            for (char *prefix = strtok(line, " "); prefix; prefix = strtok(NULL, " ")) {
                CHECK(mutations < count && strlen(prefix) == 8u);
                mutation(seed, mutations, &bases, &mutated);
                outcome_line(mutated.data, mutated.size, line_out, sizeof(line_out));
                char digest[41];
                sha1_hex((const uint8_t *)line_out, strlen(line_out), digest);
                if (memcmp(digest, prefix, 8u) != 0) {
                    fprintf(stderr, "conformance mutation %lu (kind %lu): expected %s, got %.8s: %s\n",
                            (unsigned long)mutations, (unsigned long)(mutations % 16u), prefix,
                            digest, line_out);
                    failures++;
                }
                mutations++;
            }
        }
        CHECK(mutations == count);
        buf_free(&mutated);
        buf_free(&bases.bundled);
        for (size_t w = 0; w < 8u; w++) buf_free(&bases.windows[w]);
        next = next ? strchr(next, '\n') : NULL;
        if (next) next++;
    }
    for (size_t i = 0; i < reg_count; i++) buf_free(&regs[i].buf);
    buf_free(&input);
    buf_free(&file);
    CHECK(!in_case && cases >= 230u && mutations == 4096u);
    CHECK(failures == 0);
    printf("PASS conformance table: %lu cases and %lu seeded mutations, as the Python reader reads "
           "them\n", (unsigned long)cases, (unsigned long)mutations);
}

/* ---- hashes ------------------------------------------------------------ */

static void test_sha1_and_crc32_known_answers(void) {
    char hex[41];
    /* FIPS 180-4 / RFC 3174 examples. */
    sha1_hex((const uint8_t *)"", 0, hex);
    CHECK(strcmp(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709") == 0);
    sha1_hex((const uint8_t *)"abc", 3, hex);
    CHECK(strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0);
    const char *two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha1_hex((const uint8_t *)two_blocks, strlen(two_blocks), hex);
    CHECK(strcmp(hex, "84983e441c3bd26ebaae4aa1f95129e5e54670f1") == 0);
    uint8_t *million = (uint8_t *)malloc(1000000u);
    CHECK(million != NULL);
    memset(million, 'a', 1000000u);
    /* Fed in uneven pieces, so every block-boundary case of update runs. */
    NkSha1 ctx;
    uint8_t digest[20];
    nk_sha1_init(&ctx);
    size_t done = 0, step = 1;
    while (done < 1000000u) {
        size_t take = step < 1000000u - done ? step : 1000000u - done;
        nk_sha1_update(&ctx, million + done, take);
        done += take;
        step = step * 7u % 131u + 1u;
    }
    nk_sha1_finish(&ctx, digest);
    static const uint8_t expected[20] = {
        0x34, 0xaa, 0x97, 0x3c, 0xd4, 0xc4, 0xda, 0xa4, 0xf6, 0x1e,
        0xeb, 0x2b, 0xdb, 0xad, 0x27, 0x31, 0x65, 0x34, 0x01, 0x6f,
    };
    CHECK(memcmp(digest, expected, 20) == 0);

    /* The CRC-32 check value, and the same CRC fed byte by byte and in pieces
       that start off the four-byte stride. */
    CHECK(nk_crc32_update(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u);
    CHECK(nk_crc32_update(0, (const uint8_t *)"", 0) == 0);
    uint32_t whole = nk_crc32_update(0, million, 100003u);
    uint32_t pieces = nk_crc32_update(0, million, 1);
    pieces = nk_crc32_update(pieces, million + 1, 6);
    pieces = nk_crc32_update(pieces, million + 7, 100003u - 7u);
    CHECK(whole == pieces);
    uint32_t bytes = 0;
    for (size_t i = 0; i < 100003u; i++) bytes = nk_crc32_update(bytes, million + i, 1);
    CHECK(whole == bytes);
    free(million);
    printf("PASS SHA-1 and CRC-32 known answers\n");
}

typedef struct {
    uint64_t last_done;
    uint64_t total;
    int calls;
    int cancel_after;
} Progress;

static void on_progress(uint64_t done, uint64_t total, void *user) {
    Progress *p = (Progress *)user;
    CHECK(done >= p->last_done);
    p->last_done = done;
    p->total = total;
    p->calls++;
}

static bool on_cancel(void *user) {
    Progress *p = (Progress *)user;
    return p->cancel_after >= 0 && p->calls >= p->cancel_after;
}

static void test_hash_file(void) {
    /* Larger than one read chunk, and not a whole number of chunks. */
    size_t size = 3u * 1024u * 1024u + 12345u;
    uint8_t *image = (uint8_t *)malloc(size);
    CHECK(image != NULL);
    for (size_t i = 0; i < size; i++) image[i] = (uint8_t)(i * 2654435761u >> 13);
    char path[NATIVE_TEST_PATH_MAX];
    path_in(path, sizeof(path), "hash.iso");
    write_bytes(path, image, size);

    Progress progress = {0, 0, 0, -1};
    NkDiscExpandCallbacks callbacks = {on_progress, on_cancel, &progress};
    NkDiscDigest digest;
    NkDiscExpandStatus status;
    CHECK(nk_disc_hash_file(path, &callbacks, &digest, &status));
    char hex[41];
    sha1_hex(image, size, hex);
    char crc[9];
    snprintf(crc, sizeof(crc), "%08x", (unsigned)nk_crc32_update(0, image, size));
    CHECK(strcmp(digest.sha1, hex) == 0);
    CHECK(strcmp(digest.crc32, crc) == 0);
    CHECK(digest.size_bytes == size);
    CHECK(progress.last_done == size && progress.total == size && progress.calls == 4);
    CHECK(status.code[0] == '\0');

    /* A cancel leaves no digest behind. */
    Progress stop = {0, 0, 0, 2};
    NkDiscExpandCallbacks cancel = {on_progress, on_cancel, &stop};
    CHECK(!nk_disc_hash_file(path, &cancel, &digest, &status));
    CHECK(strcmp(status.code, NK_DISC_CANCELLED) == 0);
    CHECK(digest.sha1[0] == '\0' && digest.size_bytes == 0);

    char missing[NATIVE_TEST_PATH_MAX];
    path_in(missing, sizeof(missing), "missing.iso");
    CHECK(!nk_disc_hash_file(missing, NULL, &digest, &status));
    CHECK(strcmp(status.code, NK_DISC_READ_FAILED) == 0);
    free(image);
    printf("PASS whole-image hash, progress and cancel\n");
}

/* ---- datafile ---------------------------------------------------------- */

/* Three synthetic images: the retail one the list names, a modified copy of
   it (one byte changed, as a patch would), and one the list does not know. */
#define IMAGE_BYTES 8192u
static uint8_t g_retail[IMAGE_BYTES];
static uint8_t g_modified[IMAGE_BYTES];
static uint8_t g_other[IMAGE_BYTES];
static NkDiscDigest g_retail_digest;
static NkDiscDigest g_modified_digest;
static NkDiscDigest g_other_digest;

static void digest_of(const uint8_t *data, size_t size, NkDiscDigest *out) {
    memset(out, 0, sizeof(*out));
    sha1_hex(data, size, out->sha1);
    snprintf(out->crc32, sizeof(out->crc32), "%08x", (unsigned)nk_crc32_update(0, data, size));
    out->size_bytes = size;
}

static void build_images(void) {
    for (size_t i = 0; i < IMAGE_BYTES; i++) {
        g_retail[i] = (uint8_t)(i * 31u + 7u);
        g_other[i] = (uint8_t)(i * 17u + 3u);
    }
    memcpy(g_modified, g_retail, IMAGE_BYTES);
    g_modified[4096] ^= 0x5Au;
    digest_of(g_retail, IMAGE_BYTES, &g_retail_digest);
    digest_of(g_modified, IMAGE_BYTES, &g_modified_digest);
    digest_of(g_other, IMAGE_BYTES, &g_other_digest);
}

/* A datafile in Redump's layout naming the retail image (with serials) and
   another disc, plus a rom with a malformed hash and a repeated SHA-1. */
static char *build_dat(bool with_serials) {
    char *dat = (char *)malloc(8192);
    CHECK(dat != NULL);
    snprintf(dat, 8192,
             "<?xml version=\"1.0\"?>\n"
             "<!DOCTYPE datafile PUBLIC \"-//Logiqx//DTD ROM Management Datafile//EN\" "
             "\"http://www.logiqx.com/Dats/datafile.dtd\">\n"
             "<datafile>\n"
             "\t<header>\n\t\t<name>Synthetic &amp; Test</name>\n"
             "\t\t<version>2026-10-31</version>\n\t</header>\n"
             "\t<game name=\"Retail Disc (USA)\">\n"
             "\t\t<category>Games</category>\n%s"
             "\t\t<rom name=\"Retail Disc (USA).iso\" size=\"%u\" crc=\"%s\" md5=\"0\" sha1=\"%s\"/>\n"
             "\t</game>\n"
             "\t<game name=\"Bad Row\">\n"
             "\t\t<rom name=\"Bad.iso\" size=\"12\" crc=\"zz\" sha1=\"not-a-hash\"/>\n"
             "\t</game>\n"
             "\t<game name=\"Repeated\">\n"
             "\t\t<rom name=\"Again.iso\" size=\"%u\" crc=\"%s\" sha1=\"%s\"/>\n"
             "\t</game>\n"
             "\t<game name=\"Elsewhere (Japan)\">\n"
             "\t\t<rom name=\"Elsewhere.iso\" size=\"4\" crc=\"00000001\" "
             "sha1=\"00112233445566778899aabbccddeeff00112233\"/>\n"
             "\t</game>\n"
             "</datafile>\n",
             with_serials ? "\t\t<serial>TEST-70001, TEST-70001GA</serial>\n" : "",
             IMAGE_BYTES, g_retail_digest.crc32, g_retail_digest.sha1, IMAGE_BYTES,
             g_retail_digest.crc32, g_retail_digest.sha1);
    return dat;
}

static void test_dat_reader_and_tiers(void) {
    NkDiscExpandStatus status;
    NkDiscDat dat;
    char *text = build_dat(true);
    CHECK(nk_disc_dat_parse(text, strlen(text), &dat, &status));
    CHECK(strcmp(dat.name, "Synthetic & Test") == 0);
    CHECK(strcmp(dat.version, "2026-10-31") == 0);
    CHECK(dat.games == 4 && dat.rejected_roms == 1);
    CHECK(dat.rom_count == 2); /* the repeated SHA-1 is kept once */
    CHECK(dat.serial_count == 1 && nk_disc_dat_has_serial(&dat, "TEST70001"));
    CHECK(!nk_disc_dat_has_serial(&dat, "TEST70002"));
    /* A disc ID matches after ASCII upper-casing, and only that (rule 9). */
    CHECK(nk_disc_dat_has_serial(&dat, "test70001"));
    CHECK(nk_disc_classify(&dat, "test70001", &g_modified_digest, false) == NK_DISC_TIER_MODIFIED);

    /* Verified retail: the SHA-1 matches and the CRC-32 and size agree. */
    CHECK(nk_disc_classify(&dat, "TEST70001", &g_retail_digest, false) ==
          NK_DISC_TIER_VERIFIED_RETAIL);
    /* Modified: the list names the disc ID but not this image. */
    CHECK(nk_disc_classify(&dat, "TEST70001", &g_modified_digest, false) ==
          NK_DISC_TIER_MODIFIED);
    /* Unknown: neither the image nor the disc ID. */
    CHECK(nk_disc_classify(&dat, "TEST70009", &g_other_digest, false) == NK_DISC_TIER_UNKNOWN);
    /* ... unless a verified image in the library carries the disc ID. */
    CHECK(nk_disc_classify(&dat, "TEST70009", &g_other_digest, true) == NK_DISC_TIER_MODIFIED);
    /* A matching SHA-1 with another CRC-32 or size is not verified. */
    NkDiscDigest bent = g_retail_digest;
    bent.crc32[7] = bent.crc32[7] == '0' ? '1' : '0';
    CHECK(nk_disc_classify(&dat, "TEST70009", &bent, false) == NK_DISC_TIER_UNKNOWN);
    bent = g_retail_digest;
    bent.size_bytes += 2048u;
    CHECK(nk_disc_classify(&dat, "TEST70009", &bent, false) == NK_DISC_TIER_UNKNOWN);
    /* No list, or no hash: not checked. */
    CHECK(nk_disc_classify(NULL, "TEST70001", &g_retail_digest, false) ==
          NK_DISC_TIER_NOT_CHECKED);
    NkDiscDigest empty;
    memset(&empty, 0, sizeof(empty));
    CHECK(nk_disc_classify(&dat, "TEST70001", &empty, false) == NK_DISC_TIER_NOT_CHECKED);
    nk_disc_dat_free(&dat);
    free(text);

    /* A list without serials (the form Redump's PSP list takes): a modified
       image is unknown until a verified image of its disc ID is beside it. */
    text = build_dat(false);
    CHECK(nk_disc_dat_parse(text, strlen(text), &dat, &status));
    CHECK(dat.serial_count == 0);
    CHECK(nk_disc_classify(&dat, "TEST70001", &g_modified_digest, false) == NK_DISC_TIER_UNKNOWN);
    CHECK(nk_disc_classify(&dat, "TEST70001", &g_modified_digest, true) == NK_DISC_TIER_MODIFIED);
    nk_disc_dat_free(&dat);
    free(text);

    /* The first of a repeated SHA-1 is the one kept, whatever qsort does. */
    char repeated[1024];
    snprintf(repeated, sizeof(repeated),
             "<datafile><game name=\"a\"><rom size=\"%u\" crc=\"%s\" sha1=\"%s\"/></game>"
             "<game name=\"b\"><rom size=\"4\" crc=\"00000000\" sha1=\"%s\"/></game>"
             "<game name=\"c\"><rom size=\"5\" crc=\"00000001\" sha1=\"%s\"/></game>"
             "</datafile>",
             IMAGE_BYTES, g_retail_digest.crc32, g_retail_digest.sha1, g_retail_digest.sha1,
             g_retail_digest.sha1);
    CHECK(nk_disc_dat_parse(repeated, strlen(repeated), &dat, &status));
    CHECK(dat.rom_count == 1 && dat.roms[0].size_bytes == IMAGE_BYTES);
    CHECK(nk_disc_classify(&dat, "TEST70001", &g_retail_digest, false) ==
          NK_DISC_TIER_VERIFIED_RETAIL);
    nk_disc_dat_free(&dat);
    /* A datafile that isn't UTF-8 text is refused by name. */
    const char latin1[] = "<datafile><header><name>Caf\xe9</name></header></datafile>";
    CHECK(!nk_disc_dat_parse(latin1, strlen(latin1), &dat, &status));
    CHECK(strcmp(status.code, NK_DISC_DAT_NOT_DATAFILE) == 0);

    const char *not_dat = "<html><body>nothing</body></html>";
    CHECK(!nk_disc_dat_parse(not_dat, strlen(not_dat), &dat, &status));
    CHECK(strcmp(status.code, NK_DISC_DAT_NOT_DATAFILE) == 0);
    /* A list cut short is refused, at a game's end or inside one. */
    {
        char *whole = build_dat(false);
        const char *cut_at = strstr(whole, "\t<game name=\"Elsewhere (Japan)\">");
        CHECK(cut_at != NULL);
        CHECK(!nk_disc_dat_parse(whole, (size_t)(cut_at - whole), &dat, &status));
        CHECK(strcmp(status.code, NK_DISC_DAT_NOT_DATAFILE) == 0 && dat.roms == NULL);
        const char *inside = strstr(whole, "Elsewhere.iso");
        CHECK(inside != NULL);
        char *spliced = (char *)malloc(strlen(whole) + 1u);
        CHECK(spliced != NULL);
        size_t head = (size_t)(inside - whole);
        memcpy(spliced, whole, head);
        snprintf(spliced + head, strlen(whole) + 1u - head, "</datafile>\n");
        CHECK(!nk_disc_dat_parse(spliced, strlen(spliced), &dat, &status));
        CHECK(strcmp(status.code, NK_DISC_DAT_NOT_DATAFILE) == 0 && dat.roms == NULL);
        /* Cut inside the closing tag itself. */
        const char *closing = strstr(whole, "</datafile>");
        CHECK(closing != NULL);
        CHECK(!nk_disc_dat_parse(whole, (size_t)(closing - whole) + strlen("</datafile"), &dat,
                                 &status));
        CHECK(strcmp(status.code, NK_DISC_DAT_NOT_DATAFILE) == 0 && dat.roms == NULL);
        free(spliced);
        free(whole);
    }
    const char *empty_dat = "<datafile><game name=\"x\"><rom name=\"x\" size=\"1\"/></game></datafile>";
    CHECK(!nk_disc_dat_parse(empty_dat, strlen(empty_dat), &dat, &status));
    CHECK(strcmp(status.code, NK_DISC_DAT_EMPTY) == 0);
    CHECK(dat.roms == NULL && dat.rom_count == 0);

    char ids[4][10];
    const char *serials = "TEST-71002, TEST-71002GA, test 71003,BAD,TEST-7100";
    CHECK(nk_disc_normalize_serials(serials, strlen(serials), ids, 4) == 2);
    CHECK(strcmp(ids[0], "TEST71002") == 0 && strcmp(ids[1], "TEST71003") == 0);

    CHECK(strcmp(nk_disc_tier_name(NK_DISC_TIER_MODIFIED), "MODIFIED") == 0);
    NkDiscTier tier;
    CHECK(nk_disc_tier_from_name("VERIFIED_RETAIL", &tier) && tier == NK_DISC_TIER_VERIFIED_RETAIL);
    CHECK(!nk_disc_tier_from_name("GOLD", &tier) && tier == NK_DISC_TIER_NOT_CHECKED);
    printf("PASS datafile reader: verified retail, modified, unknown, not checked\n");
}

static void test_dat_import(void) {
    NkDiscExpandStatus status;
    NkDiscDat dat;
    char source[NATIVE_TEST_PATH_MAX];
    char data_root[NATIVE_TEST_PATH_MAX];
    char imported[NATIVE_TEST_PATH_MAX];
    char *text = build_dat(true);
    path_in(source, sizeof(source), "user-chosen.dat");
    path_in(data_root, sizeof(data_root), "data");
    write_bytes(source, text, strlen(text));
    CHECK(nk_disc_dat_import(source, data_root, &dat, &status));
    CHECK(dat.rom_count == 2);
    nk_disc_dat_free(&dat);
    CHECK(nk_disc_dat_path(data_root, imported, sizeof(imported)));
    char *copy = read_text(imported);
    CHECK(strcmp(copy, text) == 0);
    free(copy);
    CHECK(nk_disc_dat_load(imported, &dat, &status) && dat.rom_count == 2);
    nk_disc_dat_free(&dat);

    /* A file that is not a list is refused, and the earlier import stays. */
    char bad[NATIVE_TEST_PATH_MAX];
    path_in(bad, sizeof(bad), "not-a-list.dat");
    write_bytes(bad, "hello", 5);
    CHECK(!nk_disc_dat_import(bad, data_root, NULL, &status));
    CHECK(strcmp(status.code, NK_DISC_DAT_NOT_DATAFILE) == 0);
    copy = read_text(imported);
    CHECK(strcmp(copy, text) == 0);
    free(copy);
    free(text);

    /* A later import replaces the list whole. */
    text = build_dat(false);
    write_bytes(source, text, strlen(text));
    CHECK(nk_disc_dat_import(source, data_root, NULL, &status));
    CHECK(nk_disc_dat_load(imported, &dat, &status) && dat.serial_count == 0);
    nk_disc_dat_free(&dat);
    free(text);
    printf("PASS datafile import into the data folder\n");
}

/* ---- the one call an add worker makes --------------------------------- */

static void test_verify_image_for_an_add_worker(void) {
    char root[NATIVE_TEST_PATH_MAX], retail[NATIVE_TEST_PATH_MAX], modified[NATIVE_TEST_PATH_MAX];
    char other[NATIVE_TEST_PATH_MAX], source[NATIVE_TEST_PATH_MAX], imported[NATIVE_TEST_PATH_MAX];
    NkDiscVerification check;
    NkDiscExpandStatus status;
    path_in(root, sizeof(root), "worker-data");
    path_in(retail, sizeof(retail), "worker-retail.iso");
    path_in(modified, sizeof(modified), "worker-modified.iso");
    path_in(other, sizeof(other), "worker-other.iso");
    write_bytes(retail, g_retail, IMAGE_BYTES);
    write_bytes(modified, g_modified, IMAGE_BYTES);
    write_bytes(other, g_other, IMAGE_BYTES);

    /* No list imported yet: hashed, not checked, and it says so. */
    CHECK(nk_disc_verify_image(retail, root, NULL, "TEST70001", false, NULL, &check));
    CHECK(check.result == NK_DISC_VERIFY_HASHED && check.tier == NK_DISC_TIER_NOT_CHECKED);
    CHECK(strcmp(check.digest.sha1, g_retail_digest.sha1) == 0 && check.code[0] == '\0');
    CHECK(strstr(check.reason, "no Redump list") != NULL);

    path_in(source, sizeof(source), "worker.dat");
    char *text = build_dat(false); /* serial-less, as Redump's older PSP exports were */
    write_bytes(source, text, strlen(text));
    free(text);
    CHECK(nk_disc_dat_import(source, root, NULL, &status));

    Progress progress = {0, 0, 0, -1};
    NkDiscExpandCallbacks callbacks = {on_progress, on_cancel, &progress};
    CHECK(nk_disc_verify_image(retail, root, NULL, "TEST70001", false, &callbacks, &check));
    CHECK(check.tier == NK_DISC_TIER_VERIFIED_RETAIL && progress.last_done == IMAGE_BYTES);
    CHECK(strncmp(check.reason, "Verified retail", 15) == 0);
    /* A lone modified image is unknown; beside its verified retail disc, modified. */
    CHECK(nk_disc_verify_image(modified, root, NULL, "TEST70001", false, NULL, &check));
    CHECK(check.tier == NK_DISC_TIER_UNKNOWN && strncmp(check.reason, "Unknown", 7) == 0);
    CHECK(nk_disc_verify_image(modified, root, NULL, "TEST70001", true, NULL, &check));
    CHECK(check.tier == NK_DISC_TIER_MODIFIED && strncmp(check.reason, "Modified", 8) == 0);
    CHECK(nk_disc_verify_image(other, root, NULL, "TEST70009", false, NULL, &check));
    CHECK(check.tier == NK_DISC_TIER_UNKNOWN);

    NkGameEntry entry;
    memset(&entry, 0, sizeof(entry));
    CHECK(nk_disc_verify_image(retail, root, NULL, "TEST70001", false, NULL, &check));
    nk_disc_verification_to_entry(&check, &entry);
    CHECK(strcmp(entry.content_sha1, g_retail_digest.sha1) == 0 &&
          strcmp(entry.content_crc32, g_retail_digest.crc32) == 0 &&
          entry.disc_verification == NK_DISC_TIER_VERIFIED_RETAIL);

    /* A cancel or an unreadable image keeps nothing, and the entry stays unhashed. */
    Progress stop = {0, 0, 0, 0};
    NkDiscExpandCallbacks cancel = {on_progress, on_cancel, &stop};
    CHECK(!nk_disc_verify_image(retail, root, NULL, "TEST70001", false, &cancel, &check));
    CHECK(check.result == NK_DISC_VERIFY_CANCELLED && check.digest.sha1[0] == '\0');
    CHECK(strcmp(check.code, NK_DISC_CANCELLED) == 0);
    memset(&entry, 0, sizeof(entry));
    nk_disc_verification_to_entry(&check, &entry);
    CHECK(entry.content_sha1[0] == '\0' && entry.disc_verification == NK_DISC_TIER_NOT_CHECKED);
    char missing[NATIVE_TEST_PATH_MAX];
    path_in(missing, sizeof(missing), "worker-missing.iso");
    CHECK(!nk_disc_verify_image(missing, root, NULL, "TEST70001", false, NULL, &check));
    CHECK(check.result == NK_DISC_VERIFY_READ_FAILED && strcmp(check.code, NK_DISC_READ_FAILED) == 0);

    /* A damaged imported list: still hashed, not checked, with the list's code. */
    CHECK(nk_disc_dat_path(root, imported, sizeof(imported)));
    write_bytes(imported, "damaged", 7);
    CHECK(nk_disc_verify_image(retail, root, NULL, "TEST70001", false, NULL, &check));
    CHECK(check.result == NK_DISC_VERIFY_LIST_UNUSABLE && check.tier == NK_DISC_TIER_NOT_CHECKED);
    CHECK(strcmp(check.code, NK_DISC_DAT_NOT_DATAFILE) == 0 && check.digest.sha1[0] != '\0');
    printf("PASS one call for an add worker: tier, reason, cancel, damaged list\n");
}

/* ---- Redump's zip, entity refusal, and the bundled list ---------------- */

static size_t read_all(const char *path, uint8_t *out, size_t cap) {
    FILE *f = nk_fopen_utf8(path, "rb");
    CHECK(f != NULL);
    size_t got = fread(out, 1, cap, f);
    fclose(f);
    CHECK(got < cap);
    return got;
}

/* A stored zip of the given entries (name, text) at path. */
static void write_zip(const char *path, const char *const *names, const char *const *texts,
                      size_t count) {
    NkZipWriter *writer = NULL;
    NkZipError err;
    CHECK(nk_zip_writer_open(&writer, path, 0x0021u, 0u, &err) == NK_ZIP_OK);
    for (size_t i = 0; i < count; i++) {
        CHECK(nk_zip_writer_add_bytes(writer, names[i], texts[i], strlen(texts[i]), &err) ==
              NK_ZIP_OK);
    }
    CHECK(nk_zip_writer_finish(writer, &err) == NK_ZIP_OK);
}

static void expect_load_refused(const char *path, const char *code) {
    NkDiscDat dat;
    NkDiscExpandStatus status;
    CHECK(!nk_disc_dat_load(path, &dat, &status));
    if (strcmp(status.code, code) != 0) {
        fprintf(stderr, "expected %s, got %s: %s\n", code, status.code, status.message);
    }
    CHECK(strcmp(status.code, code) == 0);
    CHECK(dat.roms == NULL && dat.rom_count == 0);
}

static void test_dat_zip_and_entity_refusals(void) {
    NkDiscExpandStatus status;
    NkDiscDat dat;
    char zip[NATIVE_TEST_PATH_MAX], data_root[NATIVE_TEST_PATH_MAX];
    char imported[NATIVE_TEST_PATH_MAX];
    char *text = build_dat(true);
    path_in(zip, sizeof(zip), "list.zip");
    path_in(data_root, sizeof(data_root), "zip-data");

    /* Redump's zip (here stored; the bundled list below is deflated) is read
       and imported as the .dat inside it. */
    const char *one_name[] = {"Synthetic - Datfile (2) (2026-10-31).dat"};
    const char *one_text[] = {text};
    write_zip(zip, one_name, one_text, 1);
    CHECK(nk_disc_dat_load(zip, &dat, &status) && dat.rom_count == 2);
    nk_disc_dat_free(&dat);
    CHECK(nk_disc_dat_import(zip, data_root, NULL, &status));
    CHECK(nk_disc_dat_path(data_root, imported, sizeof(imported)));
    char *copy = read_text(imported);
    CHECK(strcmp(copy, text) == 0);
    free(copy);

    /* Not one .dat: two files, or a file of another name. */
    const char *two_names[] = {"a.dat", "b.dat"};
    const char *two_texts[] = {text, text};
    write_zip(zip, two_names, two_texts, 2);
    expect_load_refused(zip, NK_DISC_DAT_ZIP_INVALID);
    const char *txt_name[] = {"list.txt"};
    write_zip(zip, txt_name, one_text, 1);
    expect_load_refused(zip, NK_DISC_DAT_ZIP_INVALID);
    /* A failed import keeps the earlier one. */
    CHECK(!nk_disc_dat_import(zip, data_root, NULL, &status));
    copy = read_text(imported);
    CHECK(strcmp(copy, text) == 0);
    free(copy);

    /* A changed byte fails the CRC; a cut zip has no end record; a size past
       the cap is refused before anything is inflated. */
    static uint8_t bytes[16384];
    write_zip(zip, one_name, one_text, 1);
    size_t size = read_all(zip, bytes, sizeof(bytes));
    /* The local header's name and extra lengths place the stored data. */
    size_t data_at = 30u + (size_t)(bytes[26] | (bytes[27] << 8)) +
                     (size_t)(bytes[28] | (bytes[29] << 8));
    bytes[data_at + 100u] ^= 0x20u;
    write_bytes(zip, bytes, size);
    expect_load_refused(zip, NK_DISC_DAT_ZIP_INVALID);
    bytes[data_at + 100u] ^= 0x20u;
    write_bytes(zip, bytes, size - 10u);
    expect_load_refused(zip, NK_DISC_DAT_ZIP_INVALID);
    size_t central = SIZE_MAX;
    for (size_t at = 0; at + 4u <= size; at++) {
        if (memcmp(bytes + at, "PK\x01\x02", 4u) == 0) central = at;
    }
    CHECK(central != SIZE_MAX);
    const uint8_t huge[4] = {0x00, 0x00, 0x00, 0x05}; /* 80 MiB, past NK_DISC_DAT_MAX_BYTES */
    memcpy(bytes + central + 24u, huge, 4u);
    write_bytes(zip, bytes, size);
    expect_load_refused(zip, NK_DISC_DAT_TOO_LARGE);

    /* Entities: declared, an internal DTD subset, or one XML does not
       predefine are refused by name; the external DOCTYPE Redump's lists
       carry, and character references, are read. */
    char sha[48];
    snprintf(sha, sizeof(sha), "%s", g_retail_digest.sha1);
    /* Each case is the text before and after one good <rom>. */
    static const char *const refused[][2] = {
        {"<!DOCTYPE datafile [<!ENTITY big \"x\">]>\n<datafile><game name=\"&big;\">",
         "</game></datafile>"},
        {"<!DOCTYPE datafile [ ]>\n<datafile><game name=\"x\">", "</game></datafile>"},
        {"<datafile><game name=\"&xxe;\">", "</game></datafile>"},
        {"<datafile><!ENTITY x SYSTEM \"file:///etc/passwd\"><game name=\"x\">",
         "</game></datafile>"},
        {"<datafile><game name=\"A & B\">", "</game></datafile>"},
    };
    char rom[256], doc[1024];
    snprintf(rom, sizeof(rom), "<rom name=\"r.iso\" size=\"%u\" crc=\"%s\" sha1=\"%s\"/>",
             IMAGE_BYTES, g_retail_digest.crc32, sha);
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        snprintf(doc, sizeof(doc), "%s%s%s", refused[i][0], rom, refused[i][1]);
        CHECK(!nk_disc_dat_parse(doc, strlen(doc), &dat, &status));
        CHECK(strcmp(status.code, NK_DISC_DAT_ENTITY_REFUSED) == 0 && dat.roms == NULL);
    }
    snprintf(doc, sizeof(doc),
             "<!DOCTYPE datafile PUBLIC \"-//Logiqx//DTD ROM Management Datafile//EN\" "
             "\"http://www.logiqx.com/Dats/datafile.dtd\">\n"
             "<datafile><game name=\"A &amp; B &#38; C &#x26;\">%s</game></datafile>", rom);
    CHECK(nk_disc_dat_parse(doc, strlen(doc), &dat, &status) && dat.rom_count == 1);
    nk_disc_dat_free(&dat);
    /* The same refusal from a file and through an import. */
    path_in(zip, sizeof(zip), "entity.dat");
    snprintf(doc, sizeof(doc), "%s%s%s", refused[0][0], rom, refused[0][1]);
    write_bytes(zip, doc, strlen(doc));
    expect_load_refused(zip, NK_DISC_DAT_ENTITY_REFUSED);
    CHECK(!nk_disc_dat_import(zip, data_root, NULL, &status));
    CHECK(strcmp(status.code, NK_DISC_DAT_ENTITY_REFUSED) == 0);
    free(text);
    printf("PASS Redump's zip, and malformed, oversized and entity-bearing lists refused by name\n");
}

/* The list bundled with Nakagawa, read from the checkout (the tests run from
   its root): Redump's PSP datfile of 2026-11-12 16-02-53, pinned in
   assets/redump/PROVENANCE.md. A refreshed list changes these numbers with
   its pin; tools/test_disc_verify.py checks the SHA-256. */
#define BUNDLED_DATE "2026-11-12 16-02-53"
#define BUNDLED_DISCS 3546u
#define BUNDLED_SERIALS 3907u

static void test_bundled_list(void) {
    NkDiscExpandStatus status;
    NkDiscDat dat;
    char bundled[NATIVE_TEST_PATH_MAX], joined[NATIVE_TEST_PATH_MAX];
    char sep = nk_platform_path_separator();
    char expect[NATIVE_TEST_PATH_MAX];

    /* The path, with or without a separator after the folder. */
    snprintf(expect, sizeof(expect), "base%cassets%credump%credump-psp.zip", sep, sep, sep);
    CHECK(nk_disc_bundled_dat_path("base", joined, sizeof(joined)) && strcmp(joined, expect) == 0);
    char base[16];
    snprintf(base, sizeof(base), "base%c", sep);
    CHECK(nk_disc_bundled_dat_path(base, joined, sizeof(joined)) && strcmp(joined, expect) == 0);
    CHECK(!nk_disc_bundled_dat_path("base", joined, 8u));
    CHECK(!nk_disc_bundled_dat_path("", joined, sizeof(joined)));

    CHECK(nk_disc_bundled_dat_path(".", bundled, sizeof(bundled)));
    CHECK(nk_platform_file_exists(bundled));
    CHECK(nk_disc_dat_load(bundled, &dat, &status));
    CHECK(strcmp(dat.name, "Sony - PlayStation Portable") == 0);
    CHECK(strcmp(dat.date, BUNDLED_DATE) == 0 && strcmp(dat.version, BUNDLED_DATE) == 0);
    CHECK(dat.games == BUNDLED_DISCS && dat.rom_count == BUNDLED_DISCS && dat.rejected_roms == 0);
    CHECK(dat.serial_count == BUNDLED_SERIALS);
    /* A known entry: its SHA-1, CRC-32 and size verify; anything else does not. */
    NkDiscDigest known = {"3ba8af1354c8ae1e5119dbff19730793c21ad56e", "b71764f4", 1207468032u};
    CHECK(nk_disc_classify(&dat, "ULJM06340", &known, false) == NK_DISC_TIER_VERIFIED_RETAIL);
    NkDiscDigest changed = known;
    snprintf(changed.crc32, sizeof(changed.crc32), "%s", "b71764f5");
    /* The list names its disc IDs, so a changed copy of a listed disc is
       Modified with no verified image beside it; an unlisted ID is Unknown. */
    CHECK(nk_disc_classify(&dat, "ULJM06340", &changed, false) == NK_DISC_TIER_MODIFIED);
    /* "ULUS-10020" and "ULJM 06341" spellings both read as disc IDs. */
    CHECK(nk_disc_dat_has_serial(&dat, "ULUS10020") && nk_disc_dat_has_serial(&dat, "ULJM06341"));
    CHECK(nk_disc_classify(&dat, "TEST70009", &changed, false) == NK_DISC_TIER_UNKNOWN);
    nk_disc_dat_free(&dat);

    /* With no import, a check uses the bundled list and names it and its date.
       A synthetic image is not in it: Unknown, said not to mean bad. */
    char root[NATIVE_TEST_PATH_MAX], retail[NATIVE_TEST_PATH_MAX], source[NATIVE_TEST_PATH_MAX];
    NkDiscVerification check;
    path_in(root, sizeof(root), "bundled-data");
    path_in(retail, sizeof(retail), "bundled-retail.iso");
    write_bytes(retail, g_retail, IMAGE_BYTES);
    CHECK(nk_disc_verify_image(retail, root, bundled, "TEST70001", false, NULL, &check));
    CHECK(check.result == NK_DISC_VERIFY_HASHED && check.tier == NK_DISC_TIER_UNKNOWN);
    CHECK(check.list == NK_DISC_LIST_BUNDLED && strcmp(check.list_date, BUNDLED_DATE) == 0);
    CHECK(strstr(check.reason, "bundled with Nakagawa (dated " BUNDLED_DATE ", 3546 discs)") != NULL);
    CHECK(strstr(check.reason, "does not mean bad") != NULL);

    /* An import takes precedence over the bundled list. */
    path_in(source, sizeof(source), "bundled-override.dat");
    char *text = build_dat(false);
    write_bytes(source, text, strlen(text));
    free(text);
    CHECK(nk_disc_dat_import(source, root, NULL, &status));
    CHECK(nk_disc_verify_image(retail, root, bundled, "TEST70001", false, NULL, &check));
    CHECK(check.tier == NK_DISC_TIER_VERIFIED_RETAIL && check.list == NK_DISC_LIST_IMPORTED);
    CHECK(strstr(check.reason, "your imported Redump list (dated 2026-10-31, 2 discs)") != NULL);
    /* The import is older than the bundled list: still used (the user chose
       it), and the reason names both dates and how to use the bundled list
       again (#1731 review S3). */
    CHECK(strstr(check.reason, "older than the list bundled with Nakagawa (dated " BUNDLED_DATE
                               "): import a newer list, or delete verification/redump.dat") != NULL);
    {
        NkDiscDat active;
        NkDiscListSource which;
        CHECK(nk_disc_dat_load_active(root, bundled, &active, &which, &status));
        CHECK(which == NK_DISC_LIST_IMPORTED && strcmp(active.newer_bundled_date, BUNDLED_DATE) == 0);
        nk_disc_dat_free(&active);
    }
    /* A newer import gets no such clause. */
    text = build_dat(false);
    char *version = strstr(text, "2026-10-31");
    CHECK(version != NULL);
    memcpy(version, "2027-01-02", 10u);
    write_bytes(source, text, strlen(text));
    free(text);
    CHECK(nk_disc_dat_import(source, root, NULL, &status));
    CHECK(nk_disc_verify_image(retail, root, bundled, "TEST70001", false, NULL, &check));
    CHECK(check.list == NK_DISC_LIST_IMPORTED && strstr(check.reason, "older than") == NULL);
    CHECK(nk_disc_list_date_is_older("2026-10-31", BUNDLED_DATE) &&
          nk_disc_list_date_is_older("2026-11-12 16-02-52", BUNDLED_DATE) &&
          !nk_disc_list_date_is_older(BUNDLED_DATE, BUNDLED_DATE) &&
          !nk_disc_list_date_is_older("2026-11-12", BUNDLED_DATE) &&
          !nk_disc_list_date_is_older("2027-01-01", BUNDLED_DATE) &&
          !nk_disc_list_date_is_older("version 3", BUNDLED_DATE));
    /* A damaged import is reported, never replaced by the bundled list. */
    char imported[NATIVE_TEST_PATH_MAX];
    CHECK(nk_disc_dat_path(root, imported, sizeof(imported)));
    write_bytes(imported, "damaged", 7);
    CHECK(nk_disc_verify_image(retail, root, bundled, "TEST70001", false, NULL, &check));
    CHECK(check.result == NK_DISC_VERIFY_LIST_UNUSABLE && check.list == NK_DISC_LIST_IMPORTED);
    CHECK(check.tier == NK_DISC_TIER_NOT_CHECKED && strstr(check.reason, "imported") != NULL);

    /* A damaged bundled list: not checked, with its code; none at all: says so. */
    char other_root[NATIVE_TEST_PATH_MAX], damaged[NATIVE_TEST_PATH_MAX];
    path_in(other_root, sizeof(other_root), "bundled-none");
    /* A listed disc ID whose hashes match no dump of it: Modified, worded for
       what is known, a damaged dump or a changed copy, and what to do
       (#1731 review S2). */
    CHECK(nk_disc_verify_image(retail, other_root, bundled, "ULJM06340", false, NULL, &check));
    CHECK(check.tier == NK_DISC_TIER_MODIFIED && check.list == NK_DISC_LIST_BUNDLED);
    CHECK(strstr(check.reason, "its hashes differ from every dump of it in the Redump list bundled") != NULL);
    CHECK(strstr(check.reason, "A damaged dump does that as well as a changed copy") != NULL &&
          strstr(check.reason, "dump the disc again") != NULL);
    path_in(damaged, sizeof(damaged), "damaged-bundled.zip");
    write_bytes(damaged, "PK\x03\x04 not a zip", 15);
    CHECK(nk_disc_verify_image(retail, other_root, damaged, "TEST70001", false, NULL, &check));
    CHECK(check.result == NK_DISC_VERIFY_LIST_UNUSABLE && check.list == NK_DISC_LIST_BUNDLED);
    CHECK(strcmp(check.code, NK_DISC_DAT_ZIP_INVALID) == 0 &&
          strstr(check.reason, "bundled") != NULL);
    path_in(damaged, sizeof(damaged), "no-such-list.zip");
    CHECK(nk_disc_verify_image(retail, other_root, damaged, "TEST70001", false, NULL, &check));
    CHECK(check.result == NK_DISC_VERIFY_HASHED && check.list == NK_DISC_LIST_NONE);
    CHECK(check.tier == NK_DISC_TIER_NOT_CHECKED && strstr(check.reason, "no Redump list") != NULL);
    printf("PASS the bundled Redump list: pinned counts, a known entry, the import overrides it\n");
}

/* ---- the reader's cost is linear in its input (#1731 review B1, S1) ----- */

/* The most bytes the reader may look at for each byte of its input. It
   counts each byte every time it looks at it, so this bounds the work of the
   input, counted rather than timed (a time limit would be flaky).
   tools/test_disc_verify.py holds the Python reader to the same bound. */
#define COST_PER_BYTE 8u

/* A growing text buffer for the hostile lists. */
typedef struct {
    char *text;
    size_t length;
    size_t capacity;
} Text;

static void text_add(Text *t, const char *piece) {
    size_t n = strlen(piece);
    if (t->length + n + 1u > t->capacity) {
        size_t grown = t->capacity ? t->capacity : 4096u;
        while (grown < t->length + n + 1u) grown *= 2u;
        char *text = (char *)realloc(t->text, grown);
        CHECK(text != NULL);
        t->text = text;
        t->capacity = grown;
    }
    memcpy(t->text + t->length, piece, n + 1u);
    t->length += n;
}

static void text_repeat(Text *t, const char *piece, size_t repeats) {
    for (size_t i = 0; i < repeats; i++) text_add(t, piece);
}

enum {
    HOSTILE_DOCTYPE_AFTER_ROOT,
    HOSTILE_DOCTYPE_BEFORE_ROOT,
    HOSTILE_HEADER_TAGS,
    HOSTILE_HEADER_ELEMENTS,
    HOSTILE_UNCLOSED_GAMES,
    HOSTILE_GAME_OPENERS,
    HOSTILE_LONG_ATTRIBUTE_NAME,
    HOSTILE_MANY_ROMS,
    HOSTILE_REFERENCES,
    HOSTILE_LONG_REFERENCE,
    HOSTILE_LONG_SERIAL,
    HOSTILE_MANY_SERIALS,
    HOSTILE_OPEN_DECLARATION,
    HOSTILE_TAGS,
    HOSTILE_LESS_THAN,
    HOSTILE_AMPERSANDS,
    HOSTILE_SERIAL_COMMA_FLOODS,
    HOSTILE_NO_CLOSING,
    HOSTILE_COUNT
};

/* The shapes tools/test_disc_verify.py's hostile_lists builds: about
   `repeats` repetitions of a shape that once cost quadratic time, or could. */
static void build_hostile(int shape, size_t repeats, Text *t) {
    char rom[256];
    snprintf(rom, sizeof(rom), "<rom name=\"x\" size=\"%u\" crc=\"%s\" sha1=\"%s\"/>", IMAGE_BYTES,
             g_retail_digest.crc32, g_retail_digest.sha1);
    const char *head = "<?xml version=\"1.0\"?>\n<datafile>\n\t<header>\n\t\t<name>";
    const char *after_name = "</name>\n\t</header>\n";
    char game[512];
    snprintf(game, sizeof(game),
             "\t<game name=\"Retail\">\n\t\t<serial>%s</serial>\n\t\t%s\n\t</game>\n", "TEST-70001",
             rom);
    t->length = 0;
    text_add(t, "");
    switch (shape) {
    case HOSTILE_DOCTYPE_AFTER_ROOT:
        text_add(t, head); text_add(t, "N"); text_add(t, after_name);
        text_repeat(t, "<!DOCTYPE", repeats); text_add(t, game); break;
    case HOSTILE_DOCTYPE_BEFORE_ROOT:
        text_repeat(t, "<!DOCTYPE", repeats); text_add(t, "<datafile>"); text_add(t, game); break;
    case HOSTILE_HEADER_TAGS:
        text_add(t, "<datafile>"); text_repeat(t, "<header", repeats); text_add(t, game); break;
    case HOSTILE_HEADER_ELEMENTS:
        text_add(t, "<datafile>"); text_repeat(t, "<header>", repeats); text_add(t, game); break;
    case HOSTILE_UNCLOSED_GAMES:
        text_add(t, "<datafile>"); text_repeat(t, "<game name=\"a\">", repeats); text_add(t, game);
        break;
    case HOSTILE_GAME_OPENERS:
        text_add(t, "<datafile>"); text_repeat(t, "<game", repeats); text_add(t, game); break;
    case HOSTILE_LONG_ATTRIBUTE_NAME:
        text_add(t, "<datafile><game><rom "); text_repeat(t, "a", repeats);
        text_add(t, "/></game>"); text_add(t, game); break;
    case HOSTILE_MANY_ROMS:
        text_add(t, "<datafile><game>"); text_repeat(t, rom, repeats / 20u);
        text_add(t, "</game>"); text_add(t, game); break;
    case HOSTILE_REFERENCES:
        text_add(t, head); text_repeat(t, "&amp;", repeats); text_add(t, after_name);
        text_add(t, game); break;
    case HOSTILE_LONG_REFERENCE:
        text_add(t, head); text_add(t, "&#"); text_repeat(t, "1", repeats); text_add(t, ";");
        text_add(t, after_name); text_add(t, game); break;
    case HOSTILE_LONG_SERIAL:
        text_add(t, "<datafile><game><serial>"); text_repeat(t, "TEST-70001,", repeats);
        text_add(t, "</serial>"); text_add(t, rom); text_add(t, "</game>"); break;
    case HOSTILE_MANY_SERIALS:
        text_add(t, "<datafile><game>"); text_repeat(t, "<serial>TEST-70002</serial>", repeats / 10u);
        text_add(t, rom); text_add(t, "</game>"); break;
    case HOSTILE_OPEN_DECLARATION:
        text_add(t, "<datafile><?"); text_repeat(t, "x", repeats); break;
    case HOSTILE_TAGS:
        text_add(t, "<datafile>"); text_repeat(t, "<a>", repeats); text_add(t, game); break;
    case HOSTILE_LESS_THAN:
        text_add(t, "<datafile>"); text_repeat(t, "<", repeats); text_add(t, game); break;
    case HOSTILE_AMPERSANDS:
        text_add(t, head); text_repeat(t, "&", repeats); text_add(t, after_name);
        text_add(t, game); break;
    case HOSTILE_SERIAL_COMMA_FLOODS: {
        /* S2: serials at the length bound, all commas, in game after game. */
        char flood[NK_DISC_DAT_MAX_SERIAL_BYTES + 40u];
        memset(flood, ',', sizeof(flood));
        memcpy(flood, "<game><serial>", 14u);
        memcpy(flood + 14u + NK_DISC_DAT_MAX_SERIAL_BYTES, "</serial></game>", 17u);
        text_add(t, "<datafile>"); text_add(t, game); text_repeat(t, flood, repeats / 64u);
        break;
    }
    case HOSTILE_NO_CLOSING:
    default:
        text_add(t, "<datafile>"); text_add(t, game); text_repeat(t, "<x>", repeats); break;
    }
    if (shape != HOSTILE_OPEN_DECLARATION && shape != HOSTILE_NO_CLOSING) text_add(t, "</datafile>\n");
}

static uint64_t parse_cost(const Text *t, NkDiscExpandStatus *status, bool *ok) {
    NkDiscDat dat;
    uint64_t examined = 0;
    *ok = nk_disc_dat_parse_counted(t->text, t->length, &dat, status, &examined);
    if (*ok) {
        nk_disc_dat_free(&dat);
    } else {
        CHECK(strncmp(status->code, "DISC_DAT_", 9u) == 0 && dat.roms == NULL);
    }
    return examined;
}

static void test_reader_cost_is_linear(void) {
    Text t = {NULL, 0, 0};
    NkDiscExpandStatus status;
    bool ok;
    for (size_t repeats = 20000u; repeats <= 40000u; repeats *= 2u) {
        for (int shape = 0; shape < HOSTILE_COUNT; shape++) {
            build_hostile(shape, repeats, &t);
            uint64_t cost = parse_cost(&t, &status, &ok);
            if (cost > (uint64_t)COST_PER_BYTE * t.length) {
                fprintf(stderr, "shape %d: %llu bytes examined for %llu\n", shape,
                        (unsigned long long)cost, (unsigned long long)t.length);
            }
            CHECK(cost <= (uint64_t)COST_PER_BYTE * t.length);
        }
    }
    /* S2: a serial's splitting is counted (three passes over it, beside the
       two text passes, the search for '<' and the references check), and one
       past the bound is refused before it is split. */
    {
        char doc[1024];
        uint64_t with_commas, empty;
        char commas[NK_DISC_DAT_MAX_SERIAL_BYTES + 2u];
        memset(commas, ',', sizeof(commas));
        commas[NK_DISC_DAT_MAX_SERIAL_BYTES] = '\0';
        Text one = {doc, 0, sizeof(doc)};
        snprintf(doc, sizeof(doc), "<datafile><game><serial>%s</serial>%s</game></datafile>", commas,
                 "<rom size=\"1\" crc=\"00000000\" sha1=\"0000000000000000000000000000000000000000\"/>");
        one.length = strlen(doc);
        with_commas = parse_cost(&one, &status, &ok);
        CHECK(ok);
        snprintf(doc, sizeof(doc), "<datafile><game><serial>%s</serial>%s</game></datafile>", "",
                 "<rom size=\"1\" crc=\"00000000\" sha1=\"0000000000000000000000000000000000000000\"/>");
        one.length = strlen(doc);
        empty = parse_cost(&one, &status, &ok);
        CHECK(ok && with_commas - empty == 7u * NK_DISC_DAT_MAX_SERIAL_BYTES);
        commas[NK_DISC_DAT_MAX_SERIAL_BYTES] = ',';
        commas[NK_DISC_DAT_MAX_SERIAL_BYTES + 1u] = '\0';
        snprintf(doc, sizeof(doc), "<datafile><game><serial>%s</serial></game></datafile>", commas);
        one.length = strlen(doc);
        (void)parse_cost(&one, &status, &ok);
        CHECK(!ok && strstr(status.message, "longer than 256 bytes") != NULL);
    }
    /* The review's inputs, at 200,000 repeats: refused by name. */
    build_hostile(HOSTILE_DOCTYPE_AFTER_ROOT, 200000u, &t);
    (void)parse_cost(&t, &status, &ok);
    CHECK(!ok && strcmp(status.code, NK_DISC_DAT_ENTITY_REFUSED) == 0);
    build_hostile(HOSTILE_DOCTYPE_BEFORE_ROOT, 200000u, &t);
    (void)parse_cost(&t, &status, &ok);
    CHECK(!ok && strcmp(status.code, NK_DISC_DAT_ENTITY_REFUSED) == 0);
    build_hostile(HOSTILE_HEADER_TAGS, 200000u, &t);
    (void)parse_cost(&t, &status, &ok);
    CHECK(!ok && strcmp(status.code, NK_DISC_DAT_NOT_DATAFILE) == 0);
    /* The bundled list costs the same bound: its .dat, kept by an import. */
    char bundled[NATIVE_TEST_PATH_MAX], root[NATIVE_TEST_PATH_MAX], kept[NATIVE_TEST_PATH_MAX];
    CHECK(nk_disc_bundled_dat_path(".", bundled, sizeof(bundled)));
    path_in(root, sizeof(root), "cost-data");
    CHECK(nk_disc_dat_import(bundled, root, NULL, &status));
    CHECK(nk_disc_dat_path(root, kept, sizeof(kept)));
    free(t.text);
    t.text = read_text(kept);
    t.length = strlen(t.text);
    t.capacity = t.length + 1u;
    CHECK(parse_cost(&t, &status, &ok) <= (uint64_t)COST_PER_BYTE * t.length && ok);
    free(t.text);
    printf("PASS the datafile reader's work is linear in its input, hostile lists included\n");
}

/* A stored zip of one entry whose central-directory extra field is `extra`. */
static size_t stored_zip_with_extra(uint8_t *out, size_t cap, const char *name, const char *text,
                                    const uint8_t *extra, size_t extra_length) {
    size_t name_length = strlen(name), size = strlen(text);
    uint32_t crc = nk_crc32_update(0u, (const uint8_t *)text, size);
    size_t at = 0;
    CHECK(30u + name_length + size + 46u + name_length + extra_length + 22u <= cap);
#define PUT16(v) do { out[at++] = (uint8_t)((v) & 0xFFu); out[at++] = (uint8_t)(((v) >> 8) & 0xFFu); } while (0)
#define PUT32(v) do { PUT16((uint32_t)(v) & 0xFFFFu); PUT16(((uint32_t)(v) >> 16) & 0xFFFFu); } while (0)
    PUT32(0x04034b50u); PUT16(20u); PUT16(0u); PUT16(0u); PUT16(0u); PUT16(0u);
    PUT32(crc); PUT32(size); PUT32(size); PUT16(name_length); PUT16(0u);
    memcpy(out + at, name, name_length); at += name_length;
    memcpy(out + at, text, size); at += size;
    size_t central = at;
    PUT32(0x02014b50u); PUT16(20u); PUT16(20u); PUT16(0u); PUT16(0u); PUT16(0u); PUT16(0u);
    PUT32(crc); PUT32(size); PUT32(size); PUT16(name_length); PUT16(extra_length); PUT16(0u);
    PUT16(0u); PUT16(0u); PUT32(0u); PUT32(0u);
    memcpy(out + at, name, name_length); at += name_length;
    memcpy(out + at, extra, extra_length); at += extra_length;
    size_t central_size = at - central;
    PUT32(0x06054b50u); PUT16(0u); PUT16(0u); PUT16(1u); PUT16(1u);
    PUT32(central_size); PUT32(central); PUT16(0u);
#undef PUT32
#undef PUT16
    return at;
}

static void test_zip64_and_newer_markup_refusals(void) {
    char path[NATIVE_TEST_PATH_MAX];
    static uint8_t bytes[16384];
    char *text = build_dat(true);
    path_in(path, sizeof(path), "zip64.zip");
    /* The same zip loads with an ordinary extra block, and is refused with a
       ZIP64 one (header ID 0x0001), as disc_verify.py refuses it. */
    const uint8_t plain_extra[4] = {0x55, 0x54, 0x00, 0x00};
    size_t size = stored_zip_with_extra(bytes, sizeof(bytes), "list.dat", text, plain_extra, 4u);
    write_bytes(path, bytes, size);
    NkDiscDat dat;
    NkDiscExpandStatus status;
    CHECK(nk_disc_dat_load(path, &dat, &status) && dat.rom_count == 2);
    nk_disc_dat_free(&dat);
    const uint8_t zip64_extra[4] = {0x01, 0x00, 0x00, 0x00};
    size = stored_zip_with_extra(bytes, sizeof(bytes), "list.dat", text, zip64_extra, 4u);
    write_bytes(path, bytes, size);
    expect_load_refused(path, NK_DISC_DAT_ZIP_INVALID);
    /* A folder entry is an entry: one folder alone is not a .dat. */
    size = stored_zip_with_extra(bytes, sizeof(bytes), "list.dat/", "", plain_extra, 4u);
    write_bytes(path, bytes, size);
    expect_load_refused(path, NK_DISC_DAT_ZIP_INVALID);

    /* Markup a Redump list does not use is refused, not skipped: a lowercase
       entity, a second DOCTYPE, one after the root, markup inside one, a
       comment and CDATA. */
    const char *external = "<!DOCTYPE datafile PUBLIC \"-//Logiqx//DTD ROM Management Datafile//EN\" "
                           "\"http://www.logiqx.com/Dats/datafile.dtd\">";
    char rom[256], doc[2048];
    snprintf(rom, sizeof(rom), "<rom name=\"r.iso\" size=\"%u\" crc=\"%s\" sha1=\"%s\"/>",
             IMAGE_BYTES, g_retail_digest.crc32, g_retail_digest.sha1);
    const char *const cases[][3] = {
        {"", "<datafile><!entity a \"b\"><game>", "</game></datafile>"},
        {"", "<datafile><game>", "</game></datafile>"},  /* the DOCTYPE twice, below */
        {"", "<datafile>", "<game>"},                      /* the DOCTYPE after the root */
        {"<!DOCTYPE datafile <!ENTITY a \"b\">>", "<datafile><game>", "</game></datafile>"},
        {"", "<datafile><!-- a --><game>", "</game></datafile>"},
        {"", "<datafile><game><![CDATA[x]]>", "</game></datafile>"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (i == 1) {
            snprintf(doc, sizeof(doc), "%s%s%s%s%s", external, external, cases[i][1], rom, cases[i][2]);
        } else if (i == 2) {
            snprintf(doc, sizeof(doc), "<datafile>%s<game>%s</game></datafile>", external, rom);
        } else {
            snprintf(doc, sizeof(doc), "%s%s%s%s", cases[i][0], cases[i][1], rom, cases[i][2]);
        }
        CHECK(!nk_disc_dat_parse(doc, strlen(doc), &dat, &status));
        CHECK(strcmp(status.code, NK_DISC_DAT_ENTITY_REFUSED) == 0);
    }
    free(text);
    printf("PASS ZIP64, a folder entry and markup a Redump list does not use are refused by name\n");
}

/* ---- library keyed by disc ID plus content hash ------------------------ */

static void seed(NkGameEntry *entry, const char *disc_id, const char *iso,
                 const NkDiscDigest *digest, NkDiscTier tier) {
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->disc_id, sizeof(entry->disc_id), "%s", disc_id);
    snprintf(entry->title_name, sizeof(entry->title_name), "Title %s", disc_id);
    snprintf(entry->disc_version, sizeof(entry->disc_version), "1.00");
    snprintf(entry->title_id, sizeof(entry->title_id), "%s-id", disc_id);
    snprintf(entry->iso_path, sizeof(entry->iso_path), "games/%s", iso);
    snprintf(entry->boot_executable, sizeof(entry->boot_executable), "EBOOT.BIN");
    entry->status = NK_STATUS_IDENTIFIED;
    entry->iso_size_bytes = IMAGE_BYTES;
    if (digest) {
        memcpy(entry->content_sha1, digest->sha1, sizeof(entry->content_sha1));
        memcpy(entry->content_crc32, digest->crc32, sizeof(entry->content_crc32));
    }
    entry->disc_verification = (uint32_t)tier;
}

/* A library.json exactly as the writer before disc hashing wrote it. */
static const char k_old_library[] =
    "{\n  \"schema_version\": 1,\n  \"games\": [\n"
    "    {\n      \"disc_id\": \"TEST70001\",\n      \"title_name\": \"Old Retail\",\n"
    "      \"disc_version\": \"1.00\",\n      \"iso_path\": \"games/old.iso\",\n"
    "      \"prepared_root\": \"\",\n      \"title_id\": \"TEST70001-id\",\n"
    "      \"iso_size_bytes\": 8192,\n      \"status\": 1,\n      \"is_experimental\": false,\n"
    "      \"executable_eboot_kind\": 0,\n      \"executable_boot_kind\": 0,\n"
    "      \"executable_selection\": 0,\n      \"executable_boot_fallback\": false,\n"
    "      \"boot_executable\": \"EBOOT.BIN\",\n      \"selected_executable\": \"\",\n"
    "      \"is_prepared\": false,\n      \"assets_staged\": false,\n"
    "      \"extracted_asset_count\": 0,\n      \"extracted_audio_count\": 0,\n"
    "      \"extracted_visual_count\": 0,\n      \"extracted_layout_count\": 0,\n"
    "      \"last_played\": \"\"\n    },\n"
    "    {\n      \"disc_id\": \"TEST70002\",\n      \"title_name\": \"Old Other\",\n"
    "      \"disc_version\": \"1.01\",\n      \"iso_path\": \"games/other.iso\",\n"
    "      \"prepared_root\": \"\",\n      \"title_id\": \"TEST70002-id\",\n"
    "      \"iso_size_bytes\": 4096,\n      \"status\": 2,\n      \"is_experimental\": true,\n"
    "      \"executable_eboot_kind\": 1,\n      \"executable_boot_kind\": 0,\n"
    "      \"executable_selection\": 1,\n      \"executable_boot_fallback\": false,\n"
    "      \"boot_executable\": \"EBOOT.BIN\",\n      \"selected_executable\": \"EBOOT.BIN\",\n"
    "      \"is_prepared\": true,\n      \"assets_staged\": false,\n"
    "      \"extracted_asset_count\": 0,\n      \"extracted_audio_count\": 0,\n"
    "      \"extracted_visual_count\": 0,\n      \"extracted_layout_count\": 0,\n"
    "      \"last_played\": \"2026-10-01T00:00:00Z\"\n    }\n"
    "  ]\n}\n";

static NkLibrary g_lib;   /* too large for the stack (NK_MAX_GAMES) */
static NkLibrary g_loaded;

static void test_old_library_still_loads(void) {
    char path[NATIVE_TEST_PATH_MAX];
    path_in(path, sizeof(path), "old_library.json");
    write_bytes(path, k_old_library, strlen(k_old_library));
    CHECK(nk_library_load(&g_lib, path) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 2);
    const NkGameEntry *old = nk_library_find_by_disc_id(&g_lib, "TEST70001");
    CHECK(old && strcmp(old->title_name, "Old Retail") == 0);
    CHECK(old->content_sha1[0] == '\0' && old->disc_verification == NK_DISC_TIER_NOT_CHECKED);
    const NkGameEntry *other = nk_library_find_by_disc_id(&g_lib, "TEST70002");
    CHECK(other && other->is_prepared && other->is_experimental &&
          strcmp(other->last_played, "2026-10-01T00:00:00Z") == 0);

    /* Saved again, it writes no hash members for records never hashed, and
       reads back the same records. */
    CHECK(nk_library_save(&g_lib, path) == NK_OK);
    char *saved = read_text(path);
    CHECK(strstr(saved, "content_sha1") == NULL && strstr(saved, "disc_verification") == NULL);
    free(saved);
    CHECK(nk_library_load(&g_loaded, path) == NK_OK);
    CHECK(nk_library_count(&g_loaded) == 2);
    CHECK(memcmp(nk_library_get(&g_loaded, 0), nk_library_get(&g_lib, 0), sizeof(NkGameEntry)) == 0);
    CHECK(memcmp(nk_library_get(&g_loaded, 1), nk_library_get(&g_lib, 1), sizeof(NkGameEntry)) == 0);
    printf("PASS a library written before disc hashing still loads and saves the same\n");
}

static void test_library_keys_by_disc_id_and_hash(void) {
    NkGameEntry retail, translation, entry;
    nk_library_init(&g_lib);

    /* The fan translation first: unknown, since nothing shows its disc ID is
       retail yet. The retail disc beside it is verified, which labels the
       translation modified. Both stay. */
    seed(&translation, "TEST70001", "translation.iso", &g_modified_digest, NK_DISC_TIER_UNKNOWN);
    seed(&retail, "TEST70001", "retail.iso", &g_retail_digest, NK_DISC_TIER_VERIFIED_RETAIL);
    CHECK(nk_library_add_or_update(&g_lib, &translation) == NK_OK);
    CHECK(nk_library_add_or_update(&g_lib, &retail) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 2);
    const NkGameEntry *t = nk_library_find_by_key(&g_lib, "TEST70001", g_modified_digest.sha1);
    const NkGameEntry *r = nk_library_find_by_key(&g_lib, "TEST70001", g_retail_digest.sha1);
    CHECK(t && r && t != r);
    CHECK(t->disc_verification == NK_DISC_TIER_MODIFIED);
    CHECK(r->disc_verification == NK_DISC_TIER_VERIFIED_RETAIL);
    /* The disc-ID API still answers, with the first record. */
    CHECK(nk_library_find_by_disc_id(&g_lib, "TEST70001") == t);
    CHECK(nk_library_has_verified_disc_id(&g_lib, "TEST70001"));
    CHECK(!nk_library_has_verified_disc_id(&g_lib, "TEST70002"));

    /* The same image again replaces its own record only. */
    seed(&entry, "TEST70001", "retail.iso", &g_retail_digest, NK_DISC_TIER_VERIFIED_RETAIL);
    snprintf(entry.title_name, sizeof(entry.title_name), "Retail, renamed");
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 2);
    r = nk_library_find_by_key(&g_lib, "TEST70001", g_retail_digest.sha1);
    CHECK(r && strcmp(r->title_name, "Retail, renamed") == 0);

    /* An update of the same file made without hashing (a last-played time)
       keeps the hashes and tier of that file's record, not the first record's:
       another writer may have just recorded them. A fresh add of the file (a
       disc added again, which the player checks again) leaves it unhashed. */
    seed(&entry, "TEST70001", "retail.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
    snprintf(entry.last_played, sizeof(entry.last_played), "2026-10-31T00:00:00Z");
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 2);
    r = nk_library_find_by_key(&g_lib, "TEST70001", g_retail_digest.sha1);
    CHECK(r && strcmp(r->last_played, "2026-10-31T00:00:00Z") == 0 &&
          r->disc_verification == NK_DISC_TIER_VERIFIED_RETAIL);
    t = nk_library_find_by_key(&g_lib, "TEST70001", g_modified_digest.sha1);
    CHECK(t && t->last_played[0] == '\0');
    {
        NkGameEntry again;
        seed(&again, "TEST70001", "retail.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
        static NkLibrary fresh;
        fresh = g_lib;
        CHECK(nk_library_add_fresh(&fresh, &again) == NK_OK);
        CHECK(nk_library_count(&fresh) == 2 &&
              nk_library_find_by_key(&fresh, "TEST70001", g_retail_digest.sha1) == NULL);
    }

    /* Saved and read back: both records, their hashes and tiers. */
    char path[NATIVE_TEST_PATH_MAX];
    path_in(path, sizeof(path), "keyed_library.json");
    CHECK(nk_library_save(&g_lib, path) == NK_OK);
    CHECK(nk_library_load(&g_loaded, path) == NK_OK);
    CHECK(nk_library_count(&g_loaded) == 2);
    for (int i = 0; i < 2; i++) {
        CHECK(memcmp(nk_library_get(&g_loaded, i), nk_library_get(&g_lib, i),
                     sizeof(NkGameEntry)) == 0);
    }

    /* Removing by key takes the named image out and leaves the other. */
    CHECK(nk_library_remove_by_key(&g_lib, "TEST70001", g_modified_digest.sha1) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 1);
    CHECK(nk_library_find_by_key(&g_lib, "TEST70001", g_retail_digest.sha1) != NULL);
    CHECK(nk_library_remove_by_key(&g_lib, "TEST70001", g_modified_digest.sha1) ==
          NK_ERROR_FILE_NOT_FOUND);

    /* A record written before hashing (a library built before this change, its
       retail disc unhashed) is kept when a hashed image of another file
       arrives: nothing shows they are the same image. */
    nk_library_init(&g_lib);
    seed(&entry, "TEST70002", "old.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    seed(&entry, "TEST70002", "new.iso", &g_other_digest, NK_DISC_TIER_UNKNOWN);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 2);
    CHECK(nk_library_find_by_key(&g_lib, "TEST70002", "") ==
          nk_library_get(&g_lib, 0)); /* the old record, still there */
    /* The record of the same file is replaced: the file holds this image now
       (a compressed image expanded over the same path), and the old record of
       that file is hashed by the new one. */
    seed(&entry, "TEST70002", "new.iso", &g_retail_digest, NK_DISC_TIER_UNKNOWN);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 2 &&
          strcmp(nk_library_get(&g_lib, 1)->content_sha1, g_retail_digest.sha1) == 0);
    seed(&entry, "TEST70002", "old.iso", &g_modified_digest, NK_DISC_TIER_UNKNOWN);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 2 &&
          strcmp(nk_library_get(&g_lib, 0)->content_sha1, g_modified_digest.sha1) == 0);

    /* An unhashed entry of a new file replaces the record its disc ID alone
       picks out when that record's file is gone (the disc moved), as before
       hashing; with two hashed images of the disc ID it can pick out neither,
       so it is added and both images stay. */
    nk_library_init(&g_lib);
    seed(&entry, "TEST70003", "one.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    seed(&entry, "TEST70003", "moved.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
    CHECK(nk_library_find_replaced(&g_lib, &entry) == nk_library_get(&g_lib, 0));
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 1 &&
          strcmp(nk_library_get(&g_lib, 0)->iso_path, "games/moved.iso") == 0);
    /* While the first image's file is still there, an unhashed second image
       (added before either is hashed) is another card, not a move. */
    {
        char first_file[NATIVE_TEST_PATH_MAX];
        path_in(first_file, sizeof(first_file), "still_here.iso");
        write_bytes(first_file, "image", 5u);
        nk_library_init(&g_lib);
        seed(&entry, "TEST70003", "unused.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
        CHECK(strlen(first_file) < sizeof(entry.iso_path));
        memcpy(entry.iso_path, first_file, strlen(first_file) + 1u);
        CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
        seed(&entry, "TEST70003", "second.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
        CHECK(nk_library_find_replaced(&g_lib, &entry) == NULL);
        CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
        CHECK(nk_library_count(&g_lib) == 2);
        remove(first_file);
    }
    nk_library_init(&g_lib);
    seed(&entry, "TEST70001", "retail.iso", &g_retail_digest, NK_DISC_TIER_VERIFIED_RETAIL);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    seed(&entry, "TEST70001", "translation.iso", &g_modified_digest, NK_DISC_TIER_MODIFIED);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    seed(&entry, "TEST70001", "third.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 3);
    CHECK(nk_library_find_by_key(&g_lib, "TEST70001", g_retail_digest.sha1) != NULL);
    CHECK(nk_library_find_by_key(&g_lib, "TEST70001", g_modified_digest.sha1) != NULL);
    /* Nor does it replace the only record when that one is hashed: an image
       whose hash failed may be another image. */
    nk_library_init(&g_lib);
    seed(&entry, "TEST70001", "retail.iso", &g_retail_digest, NK_DISC_TIER_VERIFIED_RETAIL);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    seed(&entry, "TEST70001", "unread.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 2 &&
          nk_library_find_by_key(&g_lib, "TEST70001", g_retail_digest.sha1) != NULL);

    /* The image moved, then was added again: the new file's record, unhashed,
       stays beside the hashed one until it is hashed. Then it takes over the
       image's record, which now names the new file, and the record the new
       file got before hashing goes: one file holds one image. */
    nk_library_init(&g_lib);
    seed(&entry, "TEST70004", "first.iso", &g_retail_digest, NK_DISC_TIER_VERIFIED_RETAIL);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    seed(&entry, "TEST70004", "second.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 2);
    seed(&entry, "TEST70004", "second.iso", &g_retail_digest, NK_DISC_TIER_VERIFIED_RETAIL);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 1 &&
          strcmp(nk_library_get(&g_lib, 0)->iso_path, "games/second.iso") == 0 &&
          strcmp(nk_library_get(&g_lib, 0)->content_sha1, g_retail_digest.sha1) == 0);
    /* Another image of the disc ID is never dropped with it. */
    seed(&entry, "TEST70004", "translation.iso", &g_modified_digest, NK_DISC_TIER_UNKNOWN);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    seed(&entry, "TEST70004", "third.iso", &g_retail_digest, NK_DISC_TIER_VERIFIED_RETAIL);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 2 &&
          nk_library_find_by_key(&g_lib, "TEST70004", g_modified_digest.sha1) != NULL &&
          strcmp(nk_library_find_by_key(&g_lib, "TEST70004", g_retail_digest.sha1)->iso_path,
                 "games/third.iso") == 0);

    /* An update of a record a bring-up wrote, which records no size (0), keeps
       the bring-up's hashes; a fresh add of it (the player adding the disc
       again) leaves it unhashed until the player's check of the add ends. */
    nk_library_init(&g_lib);
    seed(&entry, "TEST70001", "retail.iso", &g_retail_digest, NK_DISC_TIER_VERIFIED_RETAIL);
    entry.iso_size_bytes = 0;
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    seed(&entry, "TEST70001", "retail.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 1);
    CHECK(strcmp(nk_library_get(&g_lib, 0)->content_sha1, g_retail_digest.sha1) == 0 &&
          nk_library_get(&g_lib, 0)->disc_verification == NK_DISC_TIER_VERIFIED_RETAIL);
    CHECK(nk_library_add_fresh(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 1);
    CHECK(nk_library_get(&g_lib, 0)->content_sha1[0] == '\0' &&
          nk_library_get(&g_lib, 0)->disc_verification == NK_DISC_TIER_NOT_CHECKED);
    printf("PASS library keyed by disc ID plus hash: both discs kept, the modified one labelled\n");
}

static void test_library_remove_never_takes_the_other_image(void) {
    NkGameEntry retail, translation;
    seed(&retail, "TEST70001", "retail.iso", &g_retail_digest, NK_DISC_TIER_VERIFIED_RETAIL);
    seed(&translation, "TEST70001", "translation.iso", &g_modified_digest, NK_DISC_TIER_MODIFIED);
    nk_library_init(&g_lib);
    CHECK(nk_library_add_or_update(&g_lib, &retail) == NK_OK);
    CHECK(nk_library_add_or_update(&g_lib, &translation) == NK_OK);
    /* The disc ID alone can't say which image to remove: refused, nothing changed. */
    CHECK(nk_library_remove(&g_lib, "TEST70001") == NK_ERROR_AMBIGUOUS);
    CHECK(nk_library_remove_by_key(&g_lib, "TEST70001", NULL) == NK_ERROR_AMBIGUOUS);
    /* An empty hash names the unhashed record, and neither image is one. */
    CHECK(nk_library_remove_by_key(&g_lib, "TEST70001", "") == NK_ERROR_FILE_NOT_FOUND);
    CHECK(nk_library_count(&g_lib) == 2);
    /* Beside an unhashed record of another file (a library built before
       hashing), removing a hashed card by disc ID alone still takes nothing,
       and the empty hash takes the unhashed record only. */
    NkGameEntry legacy;
    seed(&legacy, "TEST70001", "legacy.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
    g_lib.entries[g_lib.count++] = legacy;
    CHECK(nk_library_remove(&g_lib, "TEST70001") == NK_ERROR_AMBIGUOUS);
    CHECK(nk_library_remove_by_key(&g_lib, "TEST70001", "") == NK_OK);
    CHECK(nk_library_count(&g_lib) == 2 &&
          nk_library_find_by_key(&g_lib, "TEST70001", g_retail_digest.sha1) != NULL &&
          nk_library_find_by_key(&g_lib, "TEST70001", g_modified_digest.sha1) != NULL);
    char path[NATIVE_TEST_PATH_MAX];
    path_in(path, sizeof(path), "remove_ambiguous.json");
    CHECK(nk_library_save(&g_lib, path) == NK_OK);
    CHECK(nk_library_commit_remove(&g_lib, "TEST70001", path) == NK_ERROR_AMBIGUOUS);
    CHECK(nk_library_load(&g_loaded, path) == NK_OK && nk_library_count(&g_loaded) == 2);
    /* By key, the named image goes and the other stays, in memory and on disk. */
    CHECK(nk_library_remove_by_key(&g_lib, "TEST70001", g_modified_digest.sha1) == NK_OK);
    CHECK(nk_library_commit_remove_by_key(&g_lib, "TEST70001", g_modified_digest.sha1, path) ==
          NK_OK);
    CHECK(nk_library_count(&g_lib) == 1 &&
          strcmp(nk_library_get(&g_lib, 0)->content_sha1, g_retail_digest.sha1) == 0);
    CHECK(nk_library_load(&g_loaded, path) == NK_OK && nk_library_count(&g_loaded) == 1 &&
          strcmp(nk_library_get(&g_loaded, 0)->content_sha1, g_retail_digest.sha1) == 0);
    /* With one image left, the disc ID alone picks it out again. */
    CHECK(nk_library_commit_remove(&g_lib, "TEST70001", path) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 0);
    CHECK(nk_library_load(&g_loaded, path) == NK_OK && nk_library_count(&g_loaded) == 0);
    printf("PASS library remove never takes the other image of a disc ID\n");
}

/* Two unhashed images of one disc ID (added before either was hashed): the
   empty hash can't say which to remove, the file can, in memory and on disk. */
static void test_library_remove_by_file(void) {
    NkGameEntry one, two;
    char path[NATIVE_TEST_PATH_MAX];
    path_in(path, sizeof(path), "remove_by_file.json");
    seed(&one, "TEST70005", "one.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
    seed(&two, "TEST70005", "two.iso", NULL, NK_DISC_TIER_NOT_CHECKED);
    nk_library_init(&g_lib);
    g_lib.entries[g_lib.count++] = one;
    g_lib.entries[g_lib.count++] = two;
    CHECK(nk_library_save(&g_lib, path) == NK_OK);
    CHECK(nk_library_remove_by_key(&g_lib, "TEST70005", "") == NK_ERROR_AMBIGUOUS);
    CHECK(nk_library_remove_by_file(&g_lib, "TEST70005", "games/two.iso") == NK_OK);
    CHECK(nk_library_count(&g_lib) == 1 &&
          strcmp(nk_library_get(&g_lib, 0)->iso_path, "games/one.iso") == 0);
    CHECK(nk_library_remove_by_file(&g_lib, "TEST70005", "games/two.iso") ==
          NK_ERROR_FILE_NOT_FOUND);
    CHECK(nk_library_commit_remove_by_file(&g_lib, "TEST70005", "games/two.iso", path) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 1);
    CHECK(nk_library_load(&g_loaded, path) == NK_OK && nk_library_count(&g_loaded) == 1 &&
          strcmp(nk_library_get(&g_loaded, 0)->iso_path, "games/one.iso") == 0);
    remove(path);
    printf("PASS library remove by file names one of two unhashed images\n");
}

static void test_library_commit_keeps_both_images(void) {
    char path[NATIVE_TEST_PATH_MAX];
    path_in(path, sizeof(path), "commit_keyed.json");
    NkGameEntry retail, translation;
    seed(&retail, "TEST70001", "retail.iso", &g_retail_digest, NK_DISC_TIER_VERIFIED_RETAIL);
    seed(&translation, "TEST70001", "translation.iso", &g_modified_digest, NK_DISC_TIER_UNKNOWN);
    nk_library_init(&g_lib);
    CHECK(nk_library_commit_entry(&g_lib, &retail, path) == NK_OK);

    /* Another writer (a bring-up) adds the translation to the file. */
    CHECK(nk_library_load(&g_loaded, path) == NK_OK);
    CHECK(nk_library_add_or_update(&g_loaded, &translation) == NK_OK);
    CHECK(nk_library_save(&g_loaded, path) == NK_OK);

    /* This writer's next commit keeps it and adopts it once. */
    NkGameEntry other;
    seed(&other, "TEST70003", "third.iso", &g_other_digest, NK_DISC_TIER_UNKNOWN);
    CHECK(nk_library_commit_entry(&g_lib, &other, path) == NK_OK);
    CHECK(nk_library_count(&g_lib) == 3);
    CHECK(nk_library_load(&g_loaded, path) == NK_OK);
    CHECK(nk_library_count(&g_loaded) == 3);
    const NkGameEntry *t = nk_library_find_by_key(&g_loaded, "TEST70001", g_modified_digest.sha1);
    CHECK(t && t->disc_verification == NK_DISC_TIER_MODIFIED);
    CHECK(nk_library_find_by_key(&g_lib, "TEST70001", g_modified_digest.sha1) != NULL);

    /* Removing the translation by key leaves the retail record on disk. */
    CHECK(nk_library_commit_remove_by_key(&g_lib, "TEST70001", g_modified_digest.sha1, path) ==
          NK_OK);
    CHECK(nk_library_load(&g_loaded, path) == NK_OK);
    CHECK(nk_library_count(&g_loaded) == 2);
    CHECK(nk_library_find_by_key(&g_loaded, "TEST70001", g_retail_digest.sha1) != NULL);
    CHECK(nk_library_find_by_key(&g_loaded, "TEST70001", g_modified_digest.sha1) == NULL);
    CHECK(nk_library_count(&g_lib) == 2);
    printf("PASS library commits keep both images of one disc ID\n");
}

static void test_library_hash_members_are_checked(void) {
    char path[NATIVE_TEST_PATH_MAX];
    path_in(path, sizeof(path), "members.json");
    NkGameEntry entry;
    seed(&entry, "TEST70001", "retail.iso", &g_retail_digest, NK_DISC_TIER_VERIFIED_RETAIL);
    nk_library_init(&g_lib);
    CHECK(nk_library_add_or_update(&g_lib, &entry) == NK_OK);
    CHECK(nk_library_save(&g_lib, path) == NK_OK);
    char *text = read_text(path);

    /* A tier name a later build might add reads as not checked. */
    char *tier = strstr(text, "\"VERIFIED_RETAIL\"");
    CHECK(tier != NULL);
    char *changed = (char *)malloc(strlen(text) + 16u);
    CHECK(changed != NULL);
    size_t head = (size_t)(tier - text);
    memcpy(changed, text, head);
    strcpy(changed + head, "\"GOLD_MASTER\"");
    strcat(changed, tier + strlen("\"VERIFIED_RETAIL\""));
    write_bytes(path, changed, strlen(changed));
    CHECK(nk_library_load(&g_loaded, path) == NK_OK);
    CHECK(nk_library_get(&g_loaded, 0)->disc_verification == NK_DISC_TIER_NOT_CHECKED);
    CHECK(strcmp(nk_library_get(&g_loaded, 0)->content_sha1, g_retail_digest.sha1) == 0);

    /* A malformed hash is not a record this player wrote: the file is refused
       (and its .bak would be tried), never read with the hash dropped. */
    char bak[NATIVE_TEST_PATH_MAX + 8];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    (void)nk_remove_utf8(bak);
    char *sha = strstr(text, g_retail_digest.sha1);
    CHECK(sha != NULL);
    memcpy(changed, text, strlen(text) + 1u);
    changed[(size_t)(sha - text) + 3u] = 'x';
    write_bytes(path, changed, strlen(changed));
    CHECK(nk_library_load(&g_loaded, path) == NK_ERROR_GENERIC);
    CHECK(nk_library_count(&g_loaded) == 0);
    free(changed);
    free(text);
    printf("PASS library hash members: unknown tier reads unchecked, malformed hash refused\n");
}

int main(void) {
    test_conformance_corpus();
    native_test_create_root("disc-verify");
    native_test_isolate_user_data_roots();
    build_images();
    test_sha1_and_crc32_known_answers();
    test_hash_file();
    test_dat_reader_and_tiers();
    test_dat_import();
    test_verify_image_for_an_add_worker();
    test_dat_zip_and_entity_refusals();
    test_bundled_list();
    test_reader_cost_is_linear();
    test_zip64_and_newer_markup_refusals();
    test_old_library_still_loads();
    test_library_keys_by_disc_id_and_hash();
    test_library_remove_never_takes_the_other_image();
    test_library_remove_by_file();
    test_library_commit_keeps_both_images();
    test_library_hash_members_are_checked();
    printf("test_disc_verify: %d checks passed\n", g_checks);
    return 0;
}
