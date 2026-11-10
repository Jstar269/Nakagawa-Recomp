/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * texrepl_selftest.c - the texture replacement pack reader (src/rt/texrepl.h, #1880), no GPU and no game.
 *
 * Every texture, pack and PNG here is synthetic and written under the build folder. It checks:
 *   - the hashes against published xxHash vectors and against quick-hash values taken from PPSSPP's own
 *     implementation (a pack's file names depend on them);
 *   - a texture whose hash a pack lists is replaced by the pack's PNG; an unlisted one is not;
 *   - a malformed textures.ini is reported and loads nothing;
 *   - what is read but not applied is reported;
 *   - the decoded images stay under the memory budget;
 *   - a file name or [hashes] value that leaves the pack folder is refused;
 *   - every [hashes] key form PPSSPP reads (short keys, wildcards, "_<level>"), its ini line rules, and
 *     the mip chains it builds, with each level the size of level 0 halved;
 *   - ZIP packs read in place: textures.ini at the top or in one top-level folder, stored, DEFLATE
 *     (zlib's own output among them) and data-descriptor entries, a folder's textures.zip, a damaged
 *     image, and damaged, unsafe or ini-less archives refused whole.
 * With "--bench N" it writes an N-texture pack as a folder and as a ZIP and prints open, lookup,
 * first-decode and memory figures instead.
 */
#include "texrepl.h"

#include "nk_platform.h"
#include "nk_zip.h"
#include "synthetic_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SR_SELFTEST_BUILD_ROOT
#define SR_SELFTEST_BUILD_ROOT "."
#endif

static int g_failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); \
    g_failures++; } } while (0)

/* The synthetic PNG writer is shared with the GPU selftest (tests/native/synthetic_png.h). */
static void write_png(const char *path, int w, int h, const uint32_t *rgba) {
    if (!synpng_write(path, w, h, rgba)) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
}

static void write_text(const char *path, const char *text) {
    FILE *f = nk_fopen_utf8(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    fputs(text, f);
    fclose(f);
}

static uint32_t *solid(int w, int h, uint32_t colour) {
    uint32_t *p = (uint32_t *)malloc((size_t)w * h * 4u);
    for (int i = 0; i < w * h; i++) p[i] = colour;
    return p;
}

static void pack_path(char *out, size_t n, const char *pack, const char *file) {
    snprintf(out, n, "%s/%s/%s", SR_SELFTEST_BUILD_ROOT, pack, file);
}
static void make_pack(char *dir, size_t n, const char *pack) {
    snprintf(dir, n, "%s/%s", SR_SELFTEST_BUILD_ROOT, pack);
    nk_platform_mkdir_p(dir);
}

/* ------------------------------------------------------------------------------- hashes */

static void test_hashes(void) {
    uint8_t data[512];
    for (int i = 0; i < 512; i++) data[i] = (uint8_t)(i & 0xff);
    /* Published xxHash values (seed 0), cross-checked against the reference implementation. */
    CHECK(nk_texrepl_xxh32("", 0, 0) == 0x02cc5d05u, "xxh32 of nothing");
    CHECK(nk_texrepl_xxh32("a", 1, 0) == 0x550d7456u, "xxh32 of a");
    CHECK(nk_texrepl_xxh32("abc", 3, 0) == 0x32d153ffu, "xxh32 of abc");
    CHECK(nk_texrepl_xxh32(data, 512, 0) == 0xf319d2c7u, "xxh32 of 512 bytes (the 16-byte stripe loop)");
    CHECK(nk_texrepl_xxh32(data, 512, 0xBACD7814u) == 0xd92f8e6bu, "xxh32 with the pack seed");
    CHECK(nk_texrepl_xxh64("", 0, 0) == 0xef46db3751d8e999ull, "xxh64 of nothing");
    CHECK(nk_texrepl_xxh64("a", 1, 0) == 0xd24ec4f1a98c6e5bull, "xxh64 of a");
    CHECK(nk_texrepl_xxh64("abc", 3, 0) == 0x44bc2cf5ad770999ull, "xxh64 of abc");
    CHECK(nk_texrepl_xxh64(data, 512, 0) == 0x7b3bfcaac0348ac0ull, "xxh64 of 512 bytes (the 32-byte stripe loop)");
    CHECK(nk_texrepl_xxh64(data, 512, 0xBACD7814u) == 0x27fa7a3eb3a0649dull, "xxh64 with the pack seed");
    {
        const char *fox = "The quick brown fox jumps over the lazy dog";
        CHECK(nk_texrepl_xxh32(fox, 43, 0) == 0xe85ea4deu, "xxh32 of the fox (4-byte and 1-byte tails)");
        CHECK(nk_texrepl_xxh64(fox, 43, 0) == 0x0b242d361fda71bcull, "xxh64 of the fox (8, 4 and 1-byte tails)");
    }

    /* PPSSPP's quick hash, from its own implementation over a fixed buffer. */
    uint8_t buf[512 + 16];
    for (int i = 0; i < 512; i++) buf[i] = (uint8_t)((i * 37 + (i >> 3) * 11 + 5) & 0xff);
    static const struct { int off; uint32_t size; uint32_t want; } kQuick[] = {
        {0, 256, 0x261c4644u}, {0, 64, 0xd41f5060u}, {0, 192, 0x5a9bb894u}, {4, 256, 0x1b4838c0u},
        {0, 100, 0x65aa6078u}, {0, 0, 0x9535599cu},  {0, 448, 0xafb7acb4u}, {8, 64, 0x33fb1a90u},
    };
    for (size_t i = 0; i < sizeof(kQuick) / sizeof(kQuick[0]); i++) {
        /* The guest address carries the alignment: off 4 is not 16-byte aligned. */
        uint32_t got = nk_texrepl_quick_hash(buf + kQuick[i].off, kQuick[i].size, 0x08800000u + (uint32_t)kQuick[i].off);
        CHECK(got == kQuick[i].want, "quick hash case %zu: got %08x want %08x", i, got, kQuick[i].want);
    }

    /* Identity: address (30 bits) and size code, xor the palette hash for a palette format only. */
    CHECK(nk_texrepl_cache_key(0x08804000u, 3, 0x0808u, 0xdeadbeefu) == 0x0880400000000808ull, "key, direct colour");
    CHECK(nk_texrepl_cache_key(0x08804000u, 5, 0x0808u, 0xdeadbeefu) == (0x0880400000000808ull ^ 0xdeadbeefull),
          "key, palette format");
    CHECK(nk_texrepl_cache_key(0xC8804000u, 3, 0x0808u, 0) == 0x0880400000000808ull, "key masks the address to 30 bits");
    CHECK(nk_texrepl_bits_per_texel(8) == 4 && nk_texrepl_bits_per_texel(3) == 32 && nk_texrepl_bits_per_texel(12) == 0,
          "bits per texel");
}

/* ------------------------------------------------------------------------------- packs */

static int has_diag(const NkTexRepl *p, const char *needle) {
    for (size_t i = 0; i < nk_texrepl_diag_count(p); i++)
        if (strstr(nk_texrepl_diag(p, i), needle)) return 1;
    return 0;
}

static void test_synthetic_pack(void) {
    char dir[512], path[600], err[256];
    make_pack(dir, sizeof(dir), "texrepl_pack_a");
    /* A 2x2 texture's replacement is a 4x4 PNG: factor 2. */
    uint32_t *red = solid(4, 4, 0xff0000ffu);      /* R in the low byte: 0xAABBGGRR */
    uint32_t *wide = solid(8, 4, 0xff00ff00u);
    pack_path(path, sizeof(path), "texrepl_pack_a", "red.png");
    write_png(path, 4, 4, red);
    pack_path(path, sizeof(path), "texrepl_pack_a", "wide.png");
    write_png(path, 8, 4, wide);
    pack_path(path, sizeof(path), "texrepl_pack_a", "textures.ini");
    write_text(path,
        "# a synthetic pack\r\n"
        "[options]\r\n"
        "hash = quick\r\n"
        "version = 1\r\n"
        "[hashes]\r\n"
        "0880400000000202 11223344 = red.png\n"        /* a space inside: sscanf skips it, the same key */
        "088040000000020211223344 = red.png\n"
        "088040000000020255667788 = wide.png\n"
        "08804000000002029999aaaa =\n"                  /* leave this one alone */
        "08804000000002020000bbbb = missing.png\n"
        "08804000000002020000cccc = ../outside.png\n"
        "088040000000020211223344_1 = red.png\n"        /* mip level 1, but 4x4: not level 0 halved */
        "[filtering]\n"
        "088040000000020211223344 = nearest\n"
        "[mystery]\n"
        "x = y\n");
    NkTexRepl *p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL, "open: %s", err);
    if (!p) return;
    CHECK(nk_texrepl_hash_kind(p) == NK_TEXREPL_HASH_QUICK, "hash kind");
    CHECK(nk_texrepl_entry_count(p) == 4, "entries: %zu", nk_texrepl_entry_count(p));

    NkTexReplId id = { 0x08804000u, 3u, 0x0202u, 0u, 0x11223344u };
    NkTexReplHit hit;
    /* A listed texture is replaced by the pack's PNG, pixel for pixel. */
    CHECK(nk_texrepl_lookup(p, &id, &hit) == 1, "listed texture not replaced");
    CHECK(hit.w == 4 && hit.h == 4 && hit.rgba && hit.rgba[0] == 0xff0000ffu && hit.rgba[15] == 0xff0000ffu,
          "replacement pixels: %dx%d first %08x", hit.w, hit.h, hit.rgba ? hit.rgba[0] : 0u);
    CHECK(nk_texrepl_factor(&hit, 4, 4) == 1 && nk_texrepl_factor(&hit, 2, 2) == 2 && nk_texrepl_factor(&hit, 3, 3) == 0,
          "factor");
    nk_texrepl_unpin(p, &hit);
    /* A second lookup is served from memory. */
    NkTexReplStats st;
    nk_texrepl_stats(p, &st);
    uint64_t loaded = st.loaded;
    CHECK(nk_texrepl_lookup(p, &id, &hit) == 1, "second lookup");
    nk_texrepl_unpin(p, &hit);
    nk_texrepl_stats(p, &st);
    CHECK(st.loaded == loaded && loaded == 1, "decoded once: %llu", (unsigned long long)st.loaded);

    /* A different shape from the original: not a whole-number factor. */
    NkTexReplId wide_id = { 0x08804000u, 3u, 0x0202u, 0u, 0x55667788u };
    CHECK(nk_texrepl_lookup(p, &wide_id, &hit) == 1 && hit.w == 8 && hit.h == 4, "wide replacement");
    CHECK(nk_texrepl_factor(&hit, 2, 2) == 0, "8x4 over 2x2 is not one factor");
    nk_texrepl_unpin(p, &hit);

    /* Unlisted, and listed-but-left-alone, are untouched. */
    NkTexReplId other = id;
    other.hash = 0x12345678u;
    CHECK(nk_texrepl_lookup(p, &other, &hit) == 0 && hit.rgba == NULL, "an unlisted texture was replaced");
    other.hash = 0x9999aaaau;
    CHECK(nk_texrepl_lookup(p, &other, &hit) == 0, "a left-alone texture was replaced");
    /* Another address and size code do not match. */
    other = id;
    other.addr = 0x08805000u;
    CHECK(nk_texrepl_lookup(p, &other, &hit) == 0, "another address matched");
    other = id;
    other.dim = 0x0303u;
    CHECK(nk_texrepl_lookup(p, &other, &hit) == 0, "another size matched");
    /* A listed file that is not there, or leaves the folder, is not used, and says so. */
    other = id;
    other.hash = 0x0000bbbbu;
    CHECK(nk_texrepl_lookup(p, &other, &hit) == 0, "a missing file was replaced");
    other.hash = 0x0000ccccu;
    CHECK(nk_texrepl_lookup(p, &other, &hit) == 0, "a path outside the pack was read");

    nk_texrepl_stats(p, &st);
    CHECK(st.hits == 3 && st.left_alone == 1 && st.load_failed == 1, "stats hits=%llu alone=%llu failed=%llu",
          (unsigned long long)st.hits, (unsigned long long)st.left_alone, (unsigned long long)st.load_failed);

    /* Level 1 is listed, but an image of level 0's size is no level 1: the chain stops at level 0. */
    CHECK(nk_texrepl_level_count(p, &id) == 2, "levels listed: %d", nk_texrepl_level_count(p, &id));
    CHECK(nk_texrepl_lookup_level(p, &id, 1, &hit) == 0, "a level 1 the size of level 0 was used");
    CHECK(has_diag(p, "mip level 1 is 4x4, not 2x2"), "a wrong-size mip level not reported");
    CHECK(nk_texrepl_lookup_level(p, &id, 2, &hit) == 0, "an unlisted level 2 was found");

    /* What was read and not applied is reported. */
    CHECK(has_diag(p, "outside the pack"), "path outside the pack not reported");
    CHECK(has_diag(p, "[filtering]"), "[filtering] not reported");
    CHECK(has_diag(p, "unknown section [mystery]"), "unknown section not reported");
    CHECK(has_diag(p, "missing.png"), "missing file not reported");
    nk_texrepl_close(p);
    free(red);
    free(wide);
}

static void test_malformed_ini(void) {
    static const struct { const char *name; const char *text; const char *why; } kBad[] = {
        {"no_hash", "[options]\nversion = 1\n", "no hash type"},
        {"bad_hash", "[options]\nhash = crc32\n", "unsupported hash type"},
        {"no_equals", "[options]\nhash = quick\nthis is not a key\n", "not a section"},
        {"open_section", "[options\nhash = quick\n", "unfinished section"},
        {"outside", "hash = quick\n", "outside any section"},
        {"indented", "[options]\nhash = quick\n  [hashes]\n", "indented"},
        {"no_key", "[options]\nhash = quick\n= a.png\n", "not a section"},
        {"hash_in_key", "[options]\nhash = quick\n[hashes]\nabc # def = a.png\n", "not a section"},
        {"first_hash_counts", "[options]\nhash = crc\nhash = quick\n", "unsupported hash type 'crc'"},
    };
    for (size_t i = 0; i < sizeof(kBad) / sizeof(kBad[0]); i++) {
        char dir[512], path[600], err[256] = "";
        make_pack(dir, sizeof(dir), "texrepl_pack_bad");
        uint32_t *px = solid(4, 4, 0xff112233u);
        pack_path(path, sizeof(path), "texrepl_pack_bad", "0880400000000202aabbccdd.png");
        write_png(path, 4, 4, px);
        free(px);
        pack_path(path, sizeof(path), "texrepl_pack_bad", "textures.ini");
        write_text(path, kBad[i].text);
        NkTexRepl *p = nk_texrepl_open(dir, 0, err, sizeof(err));
        CHECK(p == NULL, "%s: a malformed ini loaded", kBad[i].name);
        CHECK(strstr(err, kBad[i].why) != NULL, "%s: message was '%s'", kBad[i].name, err);
        if (p) nk_texrepl_close(p);
    }
    char err[128] = "";
    CHECK(nk_texrepl_open(SR_SELFTEST_BUILD_ROOT "/texrepl_no_such_pack", 0, err, sizeof(err)) == NULL && err[0],
          "a missing folder");
}

/* A folder with hash-named PNGs and no ini is read with the quick hash, and a PNG that is not one is reported. */
static void test_ini_less_pack(void) {
    char dir[512], path[600], err[256];
    make_pack(dir, sizeof(dir), "texrepl_pack_plain");
    uint32_t *px = solid(2, 2, 0xff010203u);
    pack_path(path, sizeof(path), "texrepl_pack_plain", "0880400000000101a1a2a3a4.png");
    write_png(path, 2, 2, px);
    free(px);
    pack_path(path, sizeof(path), "texrepl_pack_plain", "0880400000000101b1b2b3b4.png");
    write_text(path, "not a png at all");
    pack_path(path, sizeof(path), "texrepl_pack_plain", "0880400000000101c1c2c3c4.dds");
    write_text(path, "DDS ");
    pack_path(path, sizeof(path), "texrepl_pack_plain", "0880400000000101d1d2d3d4_1.png");
    write_text(path, "x");
    NkTexRepl *p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL, "open: %s", err);
    if (!p) return;
    NkTexReplHit hit;
    NkTexReplId id = { 0x08804000u, 3u, 0x0101u, 0u, 0xa1a2a3a4u };
    CHECK(nk_texrepl_lookup(p, &id, &hit) == 1 && hit.rgba[0] == 0xff010203u, "hash-named file");
    nk_texrepl_unpin(p, &hit);
    id.hash = 0xb1b2b3b4u;
    CHECK(nk_texrepl_lookup(p, &id, &hit) == 0, "a file that is not a PNG replaced a texture");
    CHECK(has_diag(p, "b1b2b3b4.png: not a PNG"), "undecodable PNG not reported");
    /* A DDS file is listed, as PPSSPP lists one, so it matches; this build does not decode it. */
    CHECK(has_diag(p, "1 hash-named DDS, KTX2 or ZIM files listed"), "listed DDS not reported at open");
    id.hash = 0xc1c2c3c4u;
    CHECK(nk_texrepl_lookup(p, &id, &hit) == 0, "a DDS file replaced a texture");
    CHECK(has_diag(p, "c1c2c3c4.dds") && has_diag(p, "only PNG"), "DDS not reported");
    /* A level 1 file with no level 0: PPSSPP leaves that texture alone. */
    id.hash = 0xd1d2d3d4u;
    NkTexReplStats st;
    CHECK(nk_texrepl_lookup(p, &id, &hit) == 0 && nk_texrepl_level_count(p, &id) == 0, "a chain with no level 0");
    nk_texrepl_stats(p, &st);
    CHECK(st.left_alone == 1, "a chain with no level 0 is left alone: %llu", (unsigned long long)st.left_alone);
    CHECK(has_diag(p, "with no level 0 before it"), "a chain with no level 0 not reported");
    nk_texrepl_close(p);
}

/* The lookup falls back the way PPSSPP's does: palette hash only, address, and ignoreAddress. */
static void test_wildcards_and_palette(void) {
    char dir[512], path[600], err[256];
    make_pack(dir, sizeof(dir), "texrepl_pack_wild");
    uint32_t *px = solid(2, 2, 0xff0a0b0cu);
    pack_path(path, sizeof(path), "texrepl_pack_wild", "a.png");
    write_png(path, 2, 2, px);
    free(px);
    pack_path(path, sizeof(path), "texrepl_pack_wild", "textures.ini");
    write_text(path,
        "[options]\nhash = xxh32\n"
        "[hashes]\n"
        "0000000000000404deadbeef = a.png\n"             /* no address: this size code and data hash, anywhere */
        "0880500000000303000000000 = a.png\n"            /* 25 digits: sscanf reads 16 + 8, the rest is no level */
        "088060000000030300000000 = a.png\n");           /* this address and size code, any data hash */
    NkTexRepl *p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL, "open: %s", err);
    if (!p) return;
    NkTexReplHit hit;
    NkTexReplId id = { 0x08123000u, 3u, 0x0404u, 0u, 0xdeadbeefu };
    CHECK(nk_texrepl_lookup(p, &id, &hit) == 1, "no-address match");
    nk_texrepl_unpin(p, &hit);
    id.hash = 0xdeadbef0u;
    CHECK(nk_texrepl_lookup(p, &id, &hit) == 0, "another data hash matched");
    NkTexReplId at = { 0x08806000u, 3u, 0x0303u, 0u, 0x01020304u };
    CHECK(nk_texrepl_lookup(p, &at, &hit) == 1, "address and size code, any data hash");
    nk_texrepl_unpin(p, &hit);
    at.dim = 0x0505u;
    CHECK(nk_texrepl_lookup(p, &at, &hit) == 0, "the same address with another size code matched");
    /* The 25-digit line is this address and size code with data hash 0: any data hash. */
    NkTexReplId at25 = { 0x08805000u, 3u, 0x0303u, 0u, 0x55aa55aau };
    CHECK(nk_texrepl_lookup(p, &at25, &hit) == 1, "a 25-digit [hashes] name read as PPSSPP reads it");
    nk_texrepl_unpin(p, &hit);
    nk_texrepl_close(p);

    /* ignoreAddress (needs a safer hash): the address part of every line is dropped. */
    pack_path(path, sizeof(path), "texrepl_pack_wild", "textures.ini");
    write_text(path, "[options]\nhash = xxh64\nignoreAddress = true\n[hashes]\n088060000000030301020304 = a.png\n");
    p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL, "open: %s", err);
    if (!p) return;
    NkTexReplId moved = { 0x09999000u, 3u, 0x0303u, 0u, 0x01020304u };
    /* The pack names an address, so the lookup key (address zeroed) must not find a line that carries one. */
    CHECK(nk_texrepl_lookup(p, &moved, &hit) == 0, "ignoreAddress still matched a line with an address");
    nk_texrepl_close(p);
    write_text(path, "[options]\nhash = quick\nignoreAddress = true\n[hashes]\n088060000000030301020304 = a.png\n");
    p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL && has_diag(p, "ignoreAddress needs hash = xxh32 or xxh64"), "ignoreAddress with the quick hash not reported");
    nk_texrepl_close(p);

    /* A palette texture's identity includes its palette hash: the same data under another palette is another key. */
    write_text(path, "[options]\nhash = xxh32\n[hashes]\n");
    uint32_t pal_hash = nk_texrepl_clut_hash("\x01\x02\x03\x04\x05\x06\x07\x08", 8);
    char name[64], line[128];
    uint64_t key = nk_texrepl_cache_key(0x08807000u, 5u, 0x0303u, pal_hash);
    snprintf(name, sizeof(name), "%016llx%08x", (unsigned long long)key, 0x0badf00du);
    snprintf(line, sizeof(line), "[options]\nhash = xxh32\n[hashes]\n%s = a.png\n", name);
    write_text(path, line);
    p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL, "open: %s", err);
    if (!p) return;
    NkTexReplId pal = { 0x08807000u, 5u, 0x0303u, pal_hash, 0x0badf00du };
    CHECK(nk_texrepl_lookup(p, &pal, &hit) == 1, "palette texture, its own palette");
    nk_texrepl_unpin(p, &hit);
    pal.clut_hash = pal_hash ^ 1u;
    CHECK(nk_texrepl_lookup(p, &pal, &hit) == 0, "palette texture, another palette");
    nk_texrepl_close(p);
}

/* A palette texture's identity as PPSSPP computes it (TextureCacheCommon.cpp, UpdateCurrentClut and
 * "cluthash = clutHash_ ^ gstate.clutformat"): XXH32 (seed 0xC0108888) of min(last load + start
 * position, largest load) bytes, xor the CLUTFORMAT word with its 0xC5 opcode. The expected values
 * below were worked out with a separate XXH32 written from the algorithm's specification (checked
 * against the empty, "a" and "abc" vectors), not with this project's code, over a palette whose
 * byte i is (7 * i + 1) & 255. */
static void test_palette_identity(void) {
    uint8_t ram[2048];
    for (size_t i = 0; i < sizeof(ram); i++) ram[i] = (uint8_t)(7u * i + 1u);
    static const struct {
        uint32_t clutfmt, texfmt, last, max, want;
        const char *what;
    } kCases[] = {
        {0x010001u, 5u, 32u, 64u, 0x437f0adfu, "4444, start 16 colours (32 bytes): 32 + 32 = 64"},
        {0x000003u, 5u, 64u, 64u, 0x437e0addu, "8888, no start"},
        {0x020000u, 5u, 32u, 256u, 0xcf3f0178u, "start 32 colours (64 bytes): 32 + 64 = 96"},
        {0x010001u, 5u, 32u, 48u, 0xa6c28046u, "the start offset is capped at the largest load (48)"},
        {0x000000u, 4u, 0u, 0u, 0x171b4f5bu, "no palette loaded yet, CLUT4: 16 entries of 2 bytes"},
        {0x000000u, 5u, 0u, 0u, 0xd7cba41au, "no palette loaded yet, CLUT8: 256 entries of 2 bytes"},
        {0x000003u, 5u, 0u, 0u, 0xe9404e3fu, "no palette loaded yet, CLUT8, 8888: 1024 bytes"},
    };
    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        uint32_t got = nk_texrepl_clut_identity(ram, sizeof(ram), kCases[i].clutfmt, kCases[i].texfmt,
                                                kCases[i].last, kCases[i].max);
        CHECK(got == kCases[i].want, "palette identity (%s): got %08x want %08x", kCases[i].what, got,
              kCases[i].want);
    }
    /* A load larger than the palette RAM never reads past it. */
    (void)nk_texrepl_clut_identity(ram, sizeof(ram), 0x000003u, 5u, 5000u, 6000u);

    /* A pack file named the way PPSSPP names it (HashName: "%016llx%08x" of the 64-bit key and the data
     * hash), with the key written out by hand from the formula above: address 0x08807000 and size code
     * 0x0303 in the key, xor the identity 0xcf3f0178 in its low half. The identity this build computes
     * for the same palette must find it. */
    char dir[512], path[600], err[256];
    make_pack(dir, sizeof(dir), "texrepl_pack_palette");
    pack_path(path, sizeof(path), "texrepl_pack_palette", "textures.ini");
    write_text(path, "[options]\nhash = xxh32\n[hashes]\n08807000cf3f027b0badf00d = pal.png\n");
    pack_path(path, sizeof(path), "texrepl_pack_palette", "pal.png");
    uint32_t *img = solid(8, 8, 0xFF336699u);
    write_png(path, 8, 8, img);
    free(img);
    NkTexRepl *p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL, "open: %s", err);
    if (!p) return;
    NkTexReplHit hit;
    NkTexReplId id = {0x08807000u, 5u, 0x0303u,
                      nk_texrepl_clut_identity(ram, sizeof(ram), 0x020000u, 5u, 32u, 256u), 0x0badf00du};
    CHECK(nk_texrepl_lookup(p, &id, &hit) == 1, "a PPSSPP-named palette file was not found by the identity this build computes");
    nk_texrepl_unpin(p, &hit);
    /* The same palette bytes under another CLUTFORMAT word is another texture (the xor). */
    id.clut_hash = nk_texrepl_clut_identity(ram, sizeof(ram), 0x020004u, 5u, 32u, 256u);
    CHECK(nk_texrepl_lookup(p, &id, &hit) == 0, "another CLUTFORMAT word found the same palette file");
    nk_texrepl_close(p);
}

/* Hashing a guest texture: contiguous and strided rows, the pack's hash kind, hash ranges. */
static void test_texture_hash(void) {
    char dir[512], path[600], err[256];
    make_pack(dir, sizeof(dir), "texrepl_pack_hash");
    pack_path(path, sizeof(path), "texrepl_pack_hash", "textures.ini");
    write_text(path, "[options]\nhash = xxh32\n[hashranges]\n08804000,16,16 = 16,8\n");
    NkTexRepl *p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL, "open: %s", err);
    if (!p) return;
    uint8_t mem[16 * 16 * 4];
    for (size_t i = 0; i < sizeof(mem); i++) mem[i] = (uint8_t)(i * 7 + 3);
    /* 8888, 16 wide with bufw 16: contiguous, w*h*4 bytes. */
    uint32_t h1 = nk_texrepl_hash_texture(p, mem, sizeof(mem), 0x08800000u, 16, 16, 16, 3, 0);
    CHECK(h1 == nk_texrepl_xxh32(mem, 16 * 16 * 4, 0xBACD7814u), "contiguous 8888 hash");
    /* A hash range at this address shortens it to 16x8. */
    uint32_t h2 = nk_texrepl_hash_texture(p, mem, sizeof(mem), 0x08804000u, 16, 16, 16, 3, 0);
    CHECK(h2 == nk_texrepl_xxh32(mem, 16 * 8 * 4, 0xBACD7814u), "hash range");
    /* Rows with gaps (bufw 16, w 8): each row's 32 bytes hashed and folded as (acc*11) ^ row. */
    uint32_t h3 = nk_texrepl_hash_texture(p, mem, sizeof(mem), 0x08800000u, 16, 8, 4, 3, 0);
    uint32_t want = 0;
    for (int y = 0; y < 4; y++) want = (want * 11u) ^ nk_texrepl_xxh32(mem + y * 64, 32, 0xBACD7814u);
    CHECK(h3 == want, "strided rows hash %08x want %08x", h3, want);
    /* A texture that reaches past the readable bytes hashes to 0, as in PPSSPP. */
    CHECK(nk_texrepl_hash_texture(p, mem, 100, 0x08800000u, 16, 16, 16, 3, 0) == 0, "too short");
    CHECK(nk_texrepl_hash_texture(p, mem, sizeof(mem), 0x08800000u, 16, 16, 16, 11, 0) == 0, "invalid format");
    /* DXT1 (4 bits a texel). */
    uint32_t h4 = nk_texrepl_hash_texture(p, mem, sizeof(mem), 0x08800000u, 16, 16, 16, 8, 0);
    CHECK(h4 == nk_texrepl_xxh32(mem, 16 * 16 / 2, 0xBACD7814u), "DXT1 size");
    nk_texrepl_close(p);

    /* The quick hash, default with no ini. */
    make_pack(dir, sizeof(dir), "texrepl_pack_hash_quick");
    p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL, "open: %s", err);
    if (!p) return;
    uint32_t q = nk_texrepl_hash_texture(p, mem, sizeof(mem), 0x08800000u, 16, 16, 16, 3, 0);
    CHECK(q == nk_texrepl_quick_hash(mem, 16 * 16 * 4, 0x08800000u), "quick hash of a texture");
    /* reduceHash with the quick hash is refused and reported (it needs a safer hash). */
    nk_texrepl_close(p);
}

/* The decoded images stay under the budget, and an image in use is never dropped. */
static void test_budget(void) {
    char dir[512], path[600], err[256], name[64];
    make_pack(dir, sizeof(dir), "texrepl_pack_budget");
    /* Six 64x64 images, 16 KiB each. */
    for (int i = 0; i < 6; i++) {
        uint32_t *px = solid(64, 64, 0xff000000u | (uint32_t)(i + 1));
        snprintf(name, sizeof(name), "%016llx%08x.png",
                 (unsigned long long)nk_texrepl_cache_key(0x08800000u, 3u, 0x0606u, 0), (unsigned)i);
        pack_path(path, sizeof(path), "texrepl_pack_budget", name);
        write_png(path, 64, 64, px);
        free(px);
    }
    uint32_t *huge = solid(256, 256, 0xff0000ffu);   /* 256 KiB */
    snprintf(name, sizeof(name), "%016llx%08x.png",
             (unsigned long long)nk_texrepl_cache_key(0x08800000u, 3u, 0x0606u, 0), 0xfu);
    pack_path(path, sizeof(path), "texrepl_pack_budget", name);
    write_png(path, 256, 256, huge);
    free(huge);
    /* A budget of three images. */
    NkTexRepl *p = nk_texrepl_open(dir, 3u * 64u * 64u * 4u + 100u, err, sizeof(err));
    CHECK(p != NULL, "open: %s", err);
    if (!p) return;
    NkTexReplHit hit[6];
    NkTexReplStats st;
    /* Three images fill the budget and stay pinned: they are in use. */
    for (int i = 0; i < 3; i++) {
        NkTexReplId id = { 0x08800000u, 3u, 0x0606u, 0u, (uint32_t)i };
        CHECK(nk_texrepl_lookup(p, &id, &hit[i]) == 1, "image %d", i);
        nk_texrepl_stats(p, &st);
        CHECK(st.cache_bytes <= 3u * 64u * 64u * 4u + 100u, "image %d: cache %llu bytes is over the budget", i,
              (unsigned long long)st.cache_bytes);
    }
    /* With every image held and the budget full, another cannot be kept: it draws unreplaced, and is counted. */
    for (int i = 3; i < 6; i++) {
        NkTexReplId id = { 0x08800000u, 3u, 0x0606u, 0u, (uint32_t)i };
        CHECK(nk_texrepl_lookup(p, &id, &hit[i]) == 0, "image %d was kept past the budget", i);
    }
    nk_texrepl_stats(p, &st);
    CHECK(st.over_budget == 3, "over budget count %llu", (unsigned long long)st.over_budget);
    CHECK(st.cache_bytes <= 3u * 64u * 64u * 4u + 100u, "cache %llu bytes over the budget",
          (unsigned long long)st.cache_bytes);
    for (int i = 0; i < 3; i++) {
        CHECK(hit[i].rgba != NULL && hit[i].rgba[0] == (0xff000000u | (uint32_t)(i + 1)), "pinned image %d changed", i);
        nk_texrepl_unpin(p, &hit[i]);
    }
    /* Unpinned, the cache evicts the least recently used to take new ones, and stays under the budget. */
    for (int i = 3; i < 6; i++) {
        NkTexReplId id = { 0x08800000u, 3u, 0x0606u, 0u, (uint32_t)i };
        NkTexReplHit h;
        CHECK(nk_texrepl_lookup(p, &id, &h) == 1, "image %d after unpinning", i);
        nk_texrepl_unpin(p, &h);
        nk_texrepl_stats(p, &st);
        CHECK(st.cache_bytes <= 3u * 64u * 64u * 4u + 100u, "cache %llu bytes over the budget", (unsigned long long)st.cache_bytes);
    }
    nk_texrepl_stats(p, &st);
    CHECK(st.evicted >= 3, "evicted %llu", (unsigned long long)st.evicted);
    /* One image bigger than the whole budget is refused outright and reported. */
    NkTexReplId big = { 0x08800000u, 3u, 0x0606u, 0u, 0xfu };
    NkTexReplHit h;
    CHECK(nk_texrepl_lookup(p, &big, &h) == 0, "an image over the whole budget was kept");
    CHECK(has_diag(p, "texture budget"), "over-budget image not reported");
    nk_texrepl_close(p);
}

/* Every [hashes] key form PPSSPP reads (sscanf "%16llx%8x_%d", at least the first field), its ini line
 * rules, and the mip chains it builds from "_<level>" lines and files. */
static void test_index_forms(void) {
    char dir[512], path[600], err[256];
    make_pack(dir, sizeof(dir), "texrepl_pack_forms");
    uint32_t *l0 = solid(8, 8, 0xff000001u), *l1 = solid(4, 4, 0xff000002u), *l2 = solid(2, 2, 0xff000003u);
    uint32_t *odd = solid(3, 3, 0xff000004u);
    static const char *const k0[] = {"m0.png", "a#b.png", "q#.png", "bar0.png", "0880c00000000303aaaa0001.png"};
    for (size_t i = 0; i < sizeof(k0) / sizeof(k0[0]); i++) {
        pack_path(path, sizeof(path), "texrepl_pack_forms", k0[i]);
        write_png(path, 8, 8, l0);
    }
    static const char *const k1[] = {"m1.png", "bar1.png", "0880c00000000303aaaa0001_1.png"};
    for (size_t i = 0; i < sizeof(k1) / sizeof(k1[0]); i++) {
        pack_path(path, sizeof(path), "texrepl_pack_forms", k1[i]);
        write_png(path, 4, 4, l1);
    }
    pack_path(path, sizeof(path), "texrepl_pack_forms", "m2.png");
    write_png(path, 2, 2, l2);
    pack_path(path, sizeof(path), "texrepl_pack_forms", "0880c00000000303aaaa0001_2.png");
    write_png(path, 3, 3, odd);   /* not 2x2: the file chain stops at level 1 */
    pack_path(path, sizeof(path), "texrepl_pack_forms", "textures.ini");
    write_text(path,
        "[options] ; a comment after the header\n"
        "hash = xxh32   # an inline comment\n"
        "hash = quick\n"                                     /* a later line: not read */
        "ignoreAddress = maybe\n"                            /* not a bool: read as false */
        "[hashes]\n"
        "0880900000000404 = m0.png\n"                        /* 16 digits: this address and size, any data hash */
        "0000000000000000cafef00d = m0.png\n"                /* the data hash alone, anywhere */
        "0880a00000000202000000a1 = m0.png\n"                /* a chain of three from ini lines */
        "0880a00000000202000000a1_1 = m1.png\n"
        "0880a00000000202000000a1_2 = m2.png\n"
        "0880a00000000202000000a2 = m0.png\n"                /* a gap: _1 then _3, no _2 */
        "0880a00000000202000000a2_1 = m1.png\n"
        "0880a00000000202000000a2_3 = m2.png\n"
        "0880a00000000202000000a3 = bar0.png|bar1.png\n"     /* '|' makes two levels, as PPSSPP's alias string */
        "0880a00000000202000000a4 = m0.png\n"
        "0880a00000000202000000a4_-1 = m1.png\n"             /* a negative level sorts first: left alone */
        "0880a00000000202000000a5 = a" "\\#" "b.png\n"       /* an escaped '#' */
        "0880a00000000202000000a6 = \"q#.png\"\n"            /* a quoted value keeps its '#' */
        "0880a00000000202000000a7 =\n"                       /* blank level 0 and a level 1: no chain */
        "0880a00000000202000000a7_1 = m1.png\n"
        "0880a00000000202000000a8 = m0.png\n"
        "[hashes]\n"                                         /* a second section: not read */
        "0880b00000000202000000b1 = m0.png\n"
        "[ hashes ]\n"                                       /* not the name "hashes" */
        "0880b00000000202000000b2 = m0.png\n"
        "[hashranges]\n"
        "0880d000,16,16 = 32,8\n"                            /* larger than the texture: not read */
        "[reducehashranges]\n"
        "16,16 = 0\n"                                        /* 0: not read */
        "[indexed]\n"
        "0880a00000000202000000a8 = true\n");                /* an image of palette indices: not drawn */
    NkTexRepl *p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL, "open: %s", err);
    free(l0); free(l1); free(l2); free(odd);
    if (!p) return;
    NkTexReplHit hit;
    CHECK(nk_texrepl_hash_kind(p) == NK_TEXREPL_HASH_XXH32, "the first hash line and its inline comment");
    CHECK(has_diag(p, "a later 'hash' line is not read"), "a later option line not reported");
    CHECK(has_diag(p, "ignoreAddress = 'maybe' is not true or false"), "a bad bool not reported");
    CHECK(has_diag(p, "a second [hashes] section is not read"), "a second section not reported");
    CHECK(has_diag(p, "unknown section [ hashes ]"), "a spaced section name not reported");
    CHECK(has_diag(p, "larger than its texture"), "a hash range larger than its texture not reported");
    CHECK(has_diag(p, "[reducehashranges] line for '16' is 0"), "a zero reduce value not reported");

    /* A 16-digit key: this address and size code, any data hash. */
    NkTexReplId short_key = { 0x08809000u, 3u, 0x0404u, 0u, 0x01234567u };
    CHECK(nk_texrepl_lookup(p, &short_key, &hit) == 1 && hit.w == 8, "a 16-digit key");
    nk_texrepl_unpin(p, &hit);
    /* The data hash alone matches any address and size code: PPSSPP's last wildcard. */
    NkTexReplId anywhere = { 0x09900000u, 3u, 0x0707u, 0u, 0xcafef00du };
    CHECK(nk_texrepl_lookup(p, &anywhere, &hit) == 1, "the data-hash-alone wildcard");
    nk_texrepl_unpin(p, &hit);

    /* A chain from ini lines: three levels, each level 0 halved. */
    NkTexReplId chain = { 0x0880a000u, 3u, 0x0202u, 0u, 0x000000a1u };
    CHECK(nk_texrepl_level_count(p, &chain) == 3, "chain levels %d", nk_texrepl_level_count(p, &chain));
    for (int lv = 0; lv < 3; lv++) {
        CHECK(nk_texrepl_lookup_level(p, &chain, lv, &hit) == 1 && hit.w == (8 >> lv) && hit.h == (8 >> lv) &&
              hit.rgba[0] == (0xff000001u + (uint32_t)lv), "chain level %d", lv);
        nk_texrepl_unpin(p, &hit);
    }
    CHECK(nk_texrepl_lookup_level(p, &chain, 3, &hit) == 0, "an unlisted level 3");
    chain.hash = 0xa2u;
    CHECK(nk_texrepl_level_count(p, &chain) == 2, "a gap ends the chain: %d", nk_texrepl_level_count(p, &chain));
    CHECK(has_diag(p, "mip level 3 is listed with no level 2 before it"), "a gap not reported");
    chain.hash = 0xa3u;
    CHECK(nk_texrepl_level_count(p, &chain) == 2 && nk_texrepl_lookup_level(p, &chain, 1, &hit) == 1 && hit.w == 4,
          "'|' in a value names two levels");
    nk_texrepl_unpin(p, &hit);
    chain.hash = 0xa4u;
    CHECK(nk_texrepl_lookup(p, &chain, &hit) == 0 && nk_texrepl_level_count(p, &chain) == 0, "a negative level");
    chain.hash = 0xa5u;
    CHECK(nk_texrepl_lookup(p, &chain, &hit) == 1, "an escaped '#' in a file name");
    nk_texrepl_unpin(p, &hit);
    chain.hash = 0xa6u;
    CHECK(nk_texrepl_lookup(p, &chain, &hit) == 1, "a quoted file name with '#'");
    nk_texrepl_unpin(p, &hit);
    chain.hash = 0xa7u;
    CHECK(nk_texrepl_lookup(p, &chain, &hit) == 0 && nk_texrepl_level_count(p, &chain) == 0, "a blank level 0");
    chain.hash = 0xa8u;
    CHECK(nk_texrepl_lookup(p, &chain, &hit) == 0, "a texture [indexed] marks was replaced with its index image");
    CHECK(has_diag(p, "m0.png: [indexed] marks its texture indexed"), "an [indexed] texture not reported");
    NkTexReplId second = { 0x0880b000u, 3u, 0x0202u, 0u, 0x000000b1u };
    CHECK(nk_texrepl_lookup(p, &second, &hit) == 0, "a line in a second [hashes] section was read");
    second.hash = 0xb2u;
    CHECK(nk_texrepl_lookup(p, &second, &hit) == 0, "a line in [ hashes ] was read");

    /* A chain from hash-named files: level 2 is 3x3, not 2x2, so it stops at level 1. */
    NkTexReplId files = { 0x0880c000u, 3u, 0x0303u, 0u, 0xaaaa0001u };
    CHECK(nk_texrepl_level_count(p, &files) == 3, "file chain levels %d", nk_texrepl_level_count(p, &files));
    CHECK(nk_texrepl_lookup_level(p, &files, 1, &hit) == 1 && hit.w == 4, "file chain level 1");
    nk_texrepl_unpin(p, &hit);
    CHECK(nk_texrepl_lookup_level(p, &files, 2, &hit) == 0, "a 3x3 level 2 under an 8x8 level 0 was used");
    CHECK(has_diag(p, "mip level 2 is 3x3, not 2x2"), "a wrong-size file level not reported");

    /* Level 1 asked first, with level 0 never decoded: level 0's size is read, not its image. */
    nk_texrepl_close(p);
    p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL, "reopen: %s", err);
    if (!p) return;
    chain.hash = 0xa1u;
    NkTexReplStats st;
    CHECK(nk_texrepl_lookup_level(p, &chain, 2, &hit) == 1 && hit.w == 2, "level 2 first");
    nk_texrepl_unpin(p, &hit);
    nk_texrepl_stats(p, &st);
    CHECK(st.loaded == 1 && st.cache_bytes == 2u * 2u * 4u, "only level 2 decoded: loaded %llu, %llu bytes",
          (unsigned long long)st.loaded, (unsigned long long)st.cache_bytes);
    nk_texrepl_close(p);
}

/* ---------------------------------------------------------------------------------- ZIP packs */

/* A ZIP writer for the tests: stored entries, DEFLATE entries made of stored blocks (what zlib writes
 * for data it cannot shrink, such as a PNG), given DEFLATE bytes, and data descriptors. */
typedef struct {
    uint8_t *d, *cd;
    size_t n, cap, cdn, cdcap;
    uint32_t count;
} Zw;

static void zbuf(uint8_t **d, size_t *n, size_t *cap, const void *src, size_t len) {
    if (*n + len > *cap) {
        size_t c = *cap ? *cap : 65536;
        while (c < *n + len) c *= 2;
        uint8_t *g = (uint8_t *)realloc(*d, c);
        if (!g) { fprintf(stderr, "out of memory\n"); exit(2); }
        *d = g;
        *cap = c;
    }
    if (len) memcpy(*d + *n, src, len);
    *n += len;
}
static void le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) { le16(p, v & 0xffffu); le16(p + 2, v >> 16); }

typedef struct { int method; int descriptor; const uint8_t *packed; size_t packed_len; int corrupt; } ZwOpt;

static void zw_add(Zw *z, const char *name, const void *data, size_t size, ZwOpt o) {
    const uint8_t *src = (const uint8_t *)data;
    uint32_t crc = nk_zip_crc32(0u, data, size);
    uint8_t *packed = NULL;
    size_t pn = 0, pcap = 0;
    if (o.method == 8 && o.packed) {
        zbuf(&packed, &pn, &pcap, o.packed, o.packed_len);
    } else if (o.method == 8) {
        size_t at = 0;
        do {   /* stored blocks: header, LEN, NLEN, bytes */
            size_t len = size - at > 65535u ? 65535u : size - at;
            uint8_t h[5];
            h[0] = (uint8_t)(at + len == size ? 1u : 0u);
            le16(h + 1, (uint32_t)len);
            le16(h + 3, (uint32_t)(~len & 0xffffu));
            zbuf(&packed, &pn, &pcap, h, 5);
            zbuf(&packed, &pn, &pcap, src + at, len);
            at += len;
        } while (at < size);
    } else {
        zbuf(&packed, &pn, &pcap, data, size);
    }
    if (o.corrupt && pn) packed[pn - 1] ^= 0x5a;
    const size_t nl = strlen(name);
    const uint32_t local = (uint32_t)z->n;
    const uint16_t flags = (uint16_t)(o.descriptor ? 8u : 0u);
    uint8_t h[46];
    memset(h, 0, sizeof(h));
    le32(h, 0x04034b50u);
    le16(h + 4, 20u);
    le16(h + 6, flags);
    le16(h + 8, (uint32_t)o.method);
    if (!o.descriptor) {
        le32(h + 14, crc);
        le32(h + 18, (uint32_t)pn);
        le32(h + 22, (uint32_t)size);
    }
    le16(h + 26, (uint32_t)nl);
    zbuf(&z->d, &z->n, &z->cap, h, 30);
    zbuf(&z->d, &z->n, &z->cap, name, nl);
    zbuf(&z->d, &z->n, &z->cap, packed, pn);
    if (o.descriptor) {
        uint8_t dd[16];
        le32(dd, 0x08074b50u);
        le32(dd + 4, crc);
        le32(dd + 8, (uint32_t)pn);
        le32(dd + 12, (uint32_t)size);
        zbuf(&z->d, &z->n, &z->cap, dd, 16);
    }
    memset(h, 0, sizeof(h));
    le32(h, 0x02014b50u);
    le16(h + 4, 20u);
    le16(h + 6, 20u);
    le16(h + 8, flags);
    le16(h + 10, (uint32_t)o.method);
    le32(h + 16, crc);
    le32(h + 20, (uint32_t)pn);
    le32(h + 24, (uint32_t)size);
    le16(h + 28, (uint32_t)nl);
    le32(h + 42, local);
    zbuf(&z->cd, &z->cdn, &z->cdcap, h, 46);
    zbuf(&z->cd, &z->cdn, &z->cdcap, name, nl);
    z->count++;
    free(packed);
}

static void zw_add_text(Zw *z, const char *name, const char *text, int method) {
    ZwOpt o = { method, 0, NULL, 0, 0 };
    zw_add(z, name, text, strlen(text), o);
}

static uint8_t *read_all(const char *path, size_t *size) {
    FILE *f = nk_fopen_utf8(path, "rb");
    if (!f) { fprintf(stderr, "cannot read %s\n", path); exit(2); }
    size_t n = 0, cap = 0;
    uint8_t *d = NULL;
    uint8_t buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) zbuf(&d, &n, &cap, buf, got);
    fclose(f);
    *size = n;
    return d;
}

/* Adds the PNG at `file` (written by write_png) as `name`. */
static void zw_add_file(Zw *z, const char *name, const char *file, ZwOpt o) {
    size_t size;
    uint8_t *d = read_all(file, &size);
    zw_add(z, name, d, size, o);
    free(d);
}

static void zw_finish(Zw *z, const char *path, size_t cut) {
    uint8_t e[22];
    memset(e, 0, sizeof(e));
    le32(e, 0x06054b50u);
    le16(e + 8, z->count);
    le16(e + 10, z->count);
    le32(e + 12, (uint32_t)z->cdn);
    le32(e + 16, (uint32_t)z->n);
    zbuf(&z->d, &z->n, &z->cap, z->cd, z->cdn);
    zbuf(&z->d, &z->n, &z->cap, e, 22);
    FILE *f = nk_fopen_utf8(path, "wb");
    if (!f || fwrite(z->d, 1, z->n - cut, f) != z->n - cut) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    fclose(f);
    free(z->d);
    free(z->cd);
    memset(z, 0, sizeof(*z));
}

/* "# a ZIP pack, deflated by zlib\n[options]\nhash = xxh32 # comment after the value\n[hashes]\n
 * 088040000000020211223344 = Sub/Red.png\n" (128 bytes), DEFLATE-compressed by zlib (level 9, raw):
 * Huffman blocks, so the inflater's full path runs, not only stored blocks. */
static const char kZlibIniText[] =
    "# a ZIP pack, deflated by zlib\n[options]\nhash = xxh32 # comment after the value\n[hashes]\n"
    "088040000000020211223344 = Sub/Red.png\n";
static const uint8_t kZlibIni[] = {
    0x2d, 0xcc, 0xb1, 0x0e, 0xc2, 0x20, 0x14, 0x46, 0xe1, 0x9d, 0xa7, 0xf8, 0x93, 0xae, 0x46, 0x29,
    0x30, 0x74, 0xf1, 0x01, 0xdc, 0x4c, 0xdd, 0x6c, 0x1c, 0x2e, 0xe5, 0x56, 0x1a, 0x29, 0x10, 0x4b,
    0x4d, 0xf5, 0xe9, 0xad, 0x89, 0x67, 0xfe, 0x72, 0x2a, 0x10, 0xae, 0xa7, 0x33, 0x32, 0xf5, 0x8f,
    0x1d, 0x1c, 0x0f, 0x81, 0x0a, 0x3b, 0xd8, 0x37, 0x3e, 0x61, 0xb4, 0xa2, 0x4b, 0xb9, 0x8c, 0x29,
    0xce, 0x37, 0xe1, 0x69, 0xf6, 0x38, 0x62, 0x5d, 0xbd, 0x56, 0xa8, 0xd0, 0xa7, 0x69, 0xe2, 0x58,
    0x40, 0x43, 0xe1, 0x27, 0x8a, 0x67, 0xbc, 0x28, 0x2c, 0x2c, 0xba, 0x9f, 0xe3, 0xcd, 0xcb, 0xa6,
    0x91, 0x46, 0xfe, 0x53, 0x52, 0xd5, 0xb5, 0x52, 0x5a, 0x1b, 0xb3, 0x3d, 0x2e, 0x8b, 0x3d, 0xb4,
    0xec, 0xf6, 0x39, 0xde, 0xc5, 0x17};

static void test_zip_packs(void) {
    char dir[512], png[600], zip[600], err[400];
    make_pack(dir, sizeof(dir), "texrepl_zip");
    uint32_t *red = solid(4, 4, 0xff0000ffu), *blue = solid(4, 4, 0xffff0000u);
    pack_path(png, sizeof(png), "texrepl_zip", "red.png");
    write_png(png, 4, 4, red);
    char png2[600];
    pack_path(png2, sizeof(png2), "texrepl_zip", "blue.png");
    write_png(png2, 4, 4, blue);
    free(red);
    free(blue);
    const ZwOpt stored = { 0, 0, NULL, 0, 0 }, deflated = { 8, 0, NULL, 0, 0 }, described = { 8, 1, NULL, 0, 0 };
    const ZwOpt zlib_ini = { 8, 0, kZlibIni, sizeof(kZlibIni), 0 };
    NkTexReplId id = { 0x08804000u, 3u, 0x0202u, 0u, 0x11223344u };
    NkTexReplId named = { 0x08804000u, 3u, 0x0202u, 0u, 0x55667788u };
    NkTexReplHit hit;
    Zw z;

    /* textures.ini at the top (zlib-deflated); a file in a folder named with other case; a hash-named
     * file at the top in a data-descriptor entry. */
    memset(&z, 0, sizeof(z));
    zw_add(&z, "textures.ini", kZlibIniText, strlen(kZlibIniText), zlib_ini);
    zw_add_file(&z, "sub/red.png", png, deflated);
    zw_add_file(&z, "088040000000020255667788.png", png2, described);
    pack_path(zip, sizeof(zip), "texrepl_zip", "top.zip");
    zw_finish(&z, zip, 0);
    NkTexRepl *p = nk_texrepl_open(zip, 0, err, sizeof(err));
    CHECK(p != NULL, "a ZIP pack: %s", err);
    if (p) {
        CHECK(nk_texrepl_is_zip(p) && nk_texrepl_hash_kind(p) == NK_TEXREPL_HASH_XXH32 && nk_texrepl_entry_count(p) == 2,
              "ZIP pack read: entries %zu", nk_texrepl_entry_count(p));
        CHECK(nk_texrepl_lookup(p, &id, &hit) == 1 && hit.rgba[0] == 0xff0000ffu, "a ZIP pack's [hashes] file, case ignored");
        nk_texrepl_unpin(p, &hit);
        CHECK(nk_texrepl_lookup(p, &named, &hit) == 1 && hit.rgba[0] == 0xffff0000u, "a ZIP pack's hash-named file");
        nk_texrepl_unpin(p, &hit);
        nk_texrepl_close(p);
    }

    /* One top-level folder holding textures.ini (with macOS's folder beside it); stored entries. */
    memset(&z, 0, sizeof(z));
    zw_add_text(&z, "MyPack/textures.ini", "[options]\nhash = quick\n[hashes]\n088040000000020211223344 = t/red.png\n", 0);
    zw_add_file(&z, "MyPack/t/red.png", png, stored);
    zw_add_file(&z, "MyPack/088040000000020255667788.png", png2, deflated);
    zw_add_text(&z, "__MACOSX/MyPack/._textures.ini", "x", 0);
    pack_path(zip, sizeof(zip), "texrepl_zip", "folder.zip");
    zw_finish(&z, zip, 0);
    p = nk_texrepl_open(zip, 0, err, sizeof(err));
    CHECK(p != NULL, "a ZIP pack in one folder: %s", err);
    if (p) {
        CHECK(nk_texrepl_lookup(p, &id, &hit) == 1, "a foldered ZIP pack's [hashes] file");
        nk_texrepl_unpin(p, &hit);
        CHECK(nk_texrepl_lookup(p, &named, &hit) == 1, "a foldered ZIP pack's hash-named file");
        nk_texrepl_unpin(p, &hit);
        nk_texrepl_close(p);
    }

    /* A folder with textures.zip: the ZIP is the pack, as in PPSSPP; the folder's own PNGs are not read. */
    char fdir[512], fzip[600];
    make_pack(fdir, sizeof(fdir), "texrepl_zip_folder");
    memset(&z, 0, sizeof(z));
    zw_add_text(&z, "textures.ini", "[options]\nhash = quick\n[hashes]\n088040000000020211223344 = red.png\n", 8);
    zw_add_file(&z, "red.png", png, deflated);
    pack_path(fzip, sizeof(fzip), "texrepl_zip_folder", "textures.zip");
    zw_finish(&z, fzip, 0);
    {
        char loose[600];
        uint32_t *g = solid(4, 4, 0xff00ff00u);
        pack_path(loose, sizeof(loose), "texrepl_zip_folder", "088040000000020255667788.png");
        write_png(loose, 4, 4, g);
        free(g);
    }
    p = nk_texrepl_open(fdir, 0, err, sizeof(err));
    CHECK(p != NULL, "a folder with textures.zip: %s", err);
    if (p) {
        CHECK(nk_texrepl_is_zip(p) && has_diag(p, "textures.zip read in place"), "textures.zip not read or not reported");
        CHECK(nk_texrepl_lookup(p, &id, &hit) == 1, "textures.zip's file");
        nk_texrepl_unpin(p, &hit);
        CHECK(nk_texrepl_lookup(p, &named, &hit) == 0, "a loose file beside textures.zip was read");
        nk_texrepl_close(p);
    }

    /* A damaged image: the pack opens (its structure is whole), that image is not used, and says so. */
    memset(&z, 0, sizeof(z));
    zw_add_text(&z, "textures.ini", "[options]\nhash = quick\n[hashes]\n088040000000020211223344 = red.png\n", 0);
    ZwOpt bad = deflated;
    bad.corrupt = 1;
    zw_add_file(&z, "red.png", png, bad);
    pack_path(zip, sizeof(zip), "texrepl_zip", "crc.zip");
    zw_finish(&z, zip, 0);
    p = nk_texrepl_open(zip, 0, err, sizeof(err));
    CHECK(p != NULL, "a ZIP with one damaged file: %s", err);
    if (p) {
        CHECK(nk_texrepl_lookup(p, &id, &hit) == 0, "a damaged image was used");
        CHECK(has_diag(p, "red.png: damaged in the ZIP (ZIP_CRC)"), "a damaged image not reported");
        nk_texrepl_close(p);
    }

    /* Refused whole, with the reason. */
    static const struct { const char *name; const char *why; } kRefused[] = {
        {"no_ini.zip", "no textures.ini"},
        {"two_tops.zip", "no textures.ini"},
        {"cut.zip", "ZIP_"},
        {"parent.zip", "ZIP_NAME_PARENT"},
        {"bad_ini.zip", "not a section"},
        {"bzip2.zip", "ZIP_METHOD"},
    };
    for (size_t i = 0; i < sizeof(kRefused) / sizeof(kRefused[0]); i++) {
        memset(&z, 0, sizeof(z));
        size_t cut = 0;
        if (i == 0) {
            zw_add_file(&z, "088040000000020211223344.png", png, stored);
        } else if (i == 1) {
            zw_add_text(&z, "A/textures.ini", "[options]\nhash = quick\n", 0);
            zw_add_text(&z, "B/textures.ini", "[options]\nhash = quick\n", 0);
        } else if (i == 2) {
            zw_add_text(&z, "textures.ini", "[options]\nhash = quick\n", 0);
            cut = 10;
        } else if (i == 3) {
            zw_add_text(&z, "textures.ini", "[options]\nhash = quick\n", 0);
            zw_add_file(&z, "../evil.png", png, stored);
        } else if (i == 4) {
            zw_add_text(&z, "textures.ini", "[options]\nhash = quick\nnot a line\n", 8);
        } else {
            zw_add_text(&z, "textures.ini", "[options]\nhash = quick\n", 0);
            ZwOpt bz = { 12, 0, (const uint8_t *)"BZh", 3, 0 };
            zw_add(&z, "x.png", "abc", 3, bz);
        }
        pack_path(zip, sizeof(zip), "texrepl_zip", kRefused[i].name);
        zw_finish(&z, zip, cut);
        err[0] = '\0';
        p = nk_texrepl_open(zip, 0, err, sizeof(err));
        CHECK(p == NULL, "%s: a bad ZIP pack loaded", kRefused[i].name);
        CHECK(strstr(err, kRefused[i].why) != NULL, "%s: message was '%s'", kRefused[i].name, err);
        if (p) nk_texrepl_close(p);
    }
}

/* ------------------------------------------------------------------------------------ bench */

#if defined(_WIN32)
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
static double wall_s(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
static void mem_now(double *ws_mib, double *peak_mib) {
    PROCESS_MEMORY_COUNTERS pmc;
    memset(&pmc, 0, sizeof(pmc));
    K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    *ws_mib = (double)pmc.WorkingSetSize / 1048576.0;
    *peak_mib = (double)pmc.PeakWorkingSetSize / 1048576.0;
}
#else
#include <sys/resource.h>
#include <sys/time.h>
static double wall_s(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}
static void mem_now(double *ws_mib, double *peak_mib) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    *ws_mib = -1.0;   /* not read on this host */
    *peak_mib = (double)ru.ru_maxrss / 1024.0;
}
#endif

/* --bench N: a synthetic pack of N textures (32x32 PNGs, one textures.ini line each, files in a
 * subfolder) as a folder and as a ZIP of DEFLATE entries in one top-level folder; prints open,
 * lookup, first-decode and memory figures for each. */
static int bench(int n) {
    char dir[512], path[700], name[64], err[400];
    uint32_t *px = solid(32, 32, 0xff808080u);
    make_pack(dir, sizeof(dir), "texrepl_bench/folder");
    snprintf(path, sizeof(path), "%s/t", dir);
    nk_platform_mkdir_p(path);
    size_t ini_cap = (size_t)n * 64u + 64u, ini_n = 0;
    char *ini = (char *)malloc(ini_cap);
    ini_n += (size_t)snprintf(ini, ini_cap, "[options]\nhash = xxh32\n[hashes]\n");
    Zw z;
    memset(&z, 0, sizeof(z));
    const ZwOpt deflated = { 8, 0, NULL, 0, 0 };
    const double t_write = wall_s();
    for (int i = 0; i < n; i++) {
        px[0] = 0xff000000u | (uint32_t)i;   /* each image differs */
        snprintf(name, sizeof(name), "t/%d.png", i);
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        write_png(path, 32, 32, px);
        ini_n += (size_t)snprintf(ini + ini_n, ini_cap - ini_n, "%016llx%08x = %s\n",
                                  (unsigned long long)nk_texrepl_cache_key(0x08800000u + ((uint32_t)i << 4), 3u, 0x0505u, 0u),
                                  (unsigned)i, name);
        char zname[80];
        snprintf(zname, sizeof(zname), "BenchPack/%s", name);
        zw_add_file(&z, zname, path, deflated);
    }
    snprintf(path, sizeof(path), "%s/textures.ini", dir);
    write_text(path, ini);
    zw_add_text(&z, "BenchPack/textures.ini", ini, 8);
    char zip[600];
    snprintf(zip, sizeof(zip), "%s/texrepl_bench/pack.zip", SR_SELFTEST_BUILD_ROOT);
    zw_finish(&z, zip, 0);
    free(ini);
    free(px);
    printf("bench: wrote %d textures as a folder and a ZIP in %.2f s\n", n, wall_s() - t_write);

    const char *const paths[2] = { dir, zip };
    const char *const kinds[2] = { "folder", "zip" };
    for (int k = 0; k < 2; k++) {
        double ws0, pk0, ws1, pk1;
        mem_now(&ws0, &pk0);
        double t0 = wall_s();
        NkTexRepl *p = nk_texrepl_open(paths[k], 0, err, sizeof(err));
        double t_open = wall_s() - t0;
        mem_now(&ws1, &pk1);
        if (!p) { printf("bench %s: open failed: %s\n", kinds[k], err); return 1; }
        NkTexReplHit hit;
        NkTexReplId id = { 0, 3u, 0x0505u, 0u, 0 };
        int hits = 0;
        t0 = wall_s();
        for (int i = 0; i < n; i++) {   /* first use: read, unpack and decode each image */
            id.addr = 0x08800000u + ((uint32_t)i << 4);
            id.hash = (uint32_t)i;
            if (nk_texrepl_lookup(p, &id, &hit)) { hits++; nk_texrepl_unpin(p, &hit); }
        }
        double t_first = wall_s() - t0;
        t0 = wall_s();
        for (int i = 0; i < n; i++) {   /* held images: the lookup alone */
            id.addr = 0x08800000u + ((uint32_t)i << 4);
            id.hash = (uint32_t)i;
            if (nk_texrepl_lookup(p, &id, &hit)) nk_texrepl_unpin(p, &hit);
        }
        double t_held = wall_s() - t0;
        t0 = wall_s();
        for (int i = 0; i < n; i++) {   /* misses: every wildcard step tried */
            id.addr = 0x09000000u + ((uint32_t)i << 4);
            id.hash = 0x80000000u | (uint32_t)i;
            (void)nk_texrepl_lookup(p, &id, &hit);
        }
        double t_miss = wall_s() - t0;
        double ws2, pk2;
        mem_now(&ws2, &pk2);
        NkTexReplStats st;
        nk_texrepl_stats(p, &st);
        printf("bench %s: entries=%zu open=%.1f ms index=%.2f MiB open_ws_delta=%.2f MiB | first use %d/%d: %.1f us each"
               " | held lookup %.3f us | miss %.3f us | cache=%.2f MiB peak_ws=%.1f MiB\n",
               kinds[k], nk_texrepl_entry_count(p), t_open * 1e3, (double)st.index_bytes / 1048576.0,
               ws1 >= 0 ? ws1 - ws0 : -1.0, hits, n, t_first * 1e6 / n, t_held * 1e6 / n, t_miss * 1e6 / n,
               (double)st.cache_bytes / 1048576.0, pk2);
        nk_texrepl_close(p);
    }
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 3 && !strcmp(argv[1], "--bench")) return bench(atoi(argv[2]));
    test_hashes();
    test_synthetic_pack();
    test_malformed_ini();
    test_ini_less_pack();
    test_wildcards_and_palette();
    test_palette_identity();
    test_texture_hash();
    test_budget();
    test_index_forms();
    test_zip_packs();
    if (g_failures) {
        fprintf(stderr, "texrepl selftest: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("texrepl selftest: OK\n");
    return 0;
}
