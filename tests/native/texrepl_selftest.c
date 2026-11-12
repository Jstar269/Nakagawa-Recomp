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
 *     image, and damaged, unsafe or ini-less archives refused whole;
 *   - PNG bytes checked before decoding: every truncation and byte flip of a small PNG, image-data
 *     flips with their CRC made right, and IHDR sizes over 2048 or over an allocation cap; bad
 *     images in a pack skipped, noted and counted;
 *   - files read only through a handle that is a regular file inside the pack folder: device names,
 *     trailing dots, links and junctions out of the pack, FIFOs (POSIX) and a textures.ini over 4 MiB;
 *   - the reader's own PNG decoder against stb_image on every pixel format, Adam7, tRNS, Huffman and
 *     several IDAT chunks, and image data that unpacks past the image (a 64 MiB bomb under 81 keys);
 *   - the project sort on McIlroy's adversary, with packs in the order that costs qsort most;
 *   - [hashranges] indexed and capped; DEFLATE padding refused.
 * With "--bench N" it writes an N-texture pack as a folder and as a ZIP and prints open, lookup,
 * first-decode and memory figures instead.
 */
#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700   /* mkfifo, symlink, realpath */
#endif
#include "texrepl.h"

#include "nk_platform.h"
#include "nk_psp_inflate.h"
#include "nk_zip.h"
#include "synthetic_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* stb_image (vendored under third_party/stb) as a second, independent PNG decoder: the pack reader
 * decodes PNG itself, and the seed corpus checks the two agree. */
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_THREAD_LOCALS
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wpedantic"
#if !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#endif
#include "../../third_party/stb/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#ifndef NK_SORT_COUNT_COMPARES
#error "build with -DNK_SORT_COUNT_COMPARES (mk/selftests.mk texrepl-selftest): the sort cells count comparisons"
#endif
/* The most the largest image's data may take to refuse when it is all empty dynamic blocks, the
 * bound of the other work cells. With the dynamic-block budget (NK_INFLATE_DYNAMIC_FREE) it takes
 * 77 ms in the Windows build (-O2) and 76 ms under AddressSanitizer (-O1) in WSL; before, 3.1 to
 * 3.4 s and 9.1 to 12.9 s. */
#define NK_PNG_WORK_SECONDS 2.0
/* The most the largest image may take to decode with every dynamic block the budget allows
 * (524,384 at one per 64 bytes). Its work is pinned exactly by the table count the cell checks; this
 * bound only catches a block grown dearer. Measured: 722 ms in the Windows build (-O2) and 2.05 s
 * under AddressSanitizer (-O1) in WSL, so a slow or loaded hosted runner (the coverage build, -j2)
 * stays well inside it. */
#define NK_PNG_ALLOWED_SECONDS 5.0
#ifndef NK_INFLATE_COUNT_BUILDS
#error "build with -DNK_INFLATE_COUNT_BUILDS (mk/selftests.mk texrepl-selftest): the work cells count code tables"
#endif
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
    const int written = snprintf(out, n, "%s/%s/%s", SR_SELFTEST_BUILD_ROOT, pack, file);
    if (written < 0 || (size_t)written >= n) { fprintf(stderr, "path too long: %s/%s\n", pack, file); exit(2); }
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

/* `declared`, when not 0, is the unpacked size the headers give in place of the data's own. */
typedef struct { int method; int descriptor; const uint8_t *packed; size_t packed_len; int corrupt; uint32_t declared; } ZwOpt;

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
    const uint32_t usize = o.declared ? o.declared : (uint32_t)size;
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
        le32(h + 22, usize);
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
        le32(dd + 12, usize);
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
    le32(h + 24, usize);
    le16(h + 28, (uint32_t)nl);
    le32(h + 42, local);
    zbuf(&z->cd, &z->cdn, &z->cdcap, h, 46);
    zbuf(&z->cd, &z->cdn, &z->cdcap, name, nl);
    z->count++;
    free(packed);
}

static void zw_add_text(Zw *z, const char *name, const char *text, int method) {
    ZwOpt o = { method, 0, NULL, 0, 0, 0 };
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
    const ZwOpt stored = { 0, 0, NULL, 0, 0, 0 }, deflated = { 8, 0, NULL, 0, 0, 0 }, described = { 8, 1, NULL, 0, 0, 0 };
    const ZwOpt zlib_ini = { 8, 0, kZlibIni, sizeof(kZlibIni), 0, 0 };
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
            ZwOpt bz = { 12, 0, (const uint8_t *)"BZh", 3, 0, 0 };
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
#include <errno.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
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


/* ------------------------------------------------------------------------- ZIP refusals */

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t rd16le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

static void file_save(const char *path, const uint8_t *d, size_t n) {
    FILE *f = nk_fopen_utf8(path, "wb");
    if (!f || fwrite(d, 1, n, f) != n) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    fclose(f);
}

/* The offset of the idx-th central header of a ZIP the writer made (no comment, no extra fields). */
static size_t central_at(const uint8_t *d, size_t n, int idx) {
    size_t at = rd32le(d + n - 22 + 16);
    for (int i = 0; i < idx; i++) at += 46u + rd16le(d + at + 28) + rd16le(d + at + 30) + rd16le(d + at + 32);
    return at;
}

/* Every refusal the ZIP reader makes for a pack, through nk_texrepl_open: each refuses the pack
 * whole, with the reader's code and no folder above the archive in the message. Then the caps:
 * entries declaring more than NK_TEXREPL_MAX_PACK_BYTES in all are refused whole; an image over
 * 64 MiB, or one whose data unpacks past its declared size, is skipped without unpacking more. */
static void test_zip_refusals(void) {
    char dir[512], zip[600], pngp[600], err[300];
    make_pack(dir, sizeof(dir), "texrepl_zip_refuse");
    uint32_t *px = solid(4, 4, 0xff0000ffu);
    pack_path(pngp, sizeof(pngp), "texrepl_zip_refuse", "red.png");
    write_png(pngp, 4, 4, px);
    free(px);
    const ZwOpt stored = { 0, 0, NULL, 0, 0, 0 };
    static const char kIni[] = "[options]\nhash = quick\n[hashes]\n088040000000020211223344 = a.png\n";
    enum { P_NONE, P_ZIP64, P_ENCRYPTED, P_LOCAL, P_OVERLAP, P_CD_OUTSIDE };
    static const struct { const char *name, *entry, *entry2, *why; int patch; } kCells[] = {
        { "absolute.zip", "/a.png", NULL, "ZIP_NAME_ABSOLUTE", P_NONE },
        { "drive.zip", "C:/a.png", NULL, "ZIP_NAME_DRIVE", P_NONE },
        { "backslash.zip", "sub\\a.png", NULL, "ZIP_NAME_BACKSLASH", P_NONE },
        { "parent.zip", "sub/../../a.png", NULL, "ZIP_NAME_PARENT", P_NONE },
        { "reserved.zip", "sub/NUL.png", NULL, "ZIP_NAME_RESERVED", P_NONE },
        { "duplicate.zip", "a.png", "a.png", "ZIP_NAME_DUPLICATE", P_NONE },
        { "case.zip", "a.png", "A.PNG", "ZIP_NAME_DUPLICATE", P_NONE },
        { "zip64.zip", "a.png", NULL, "ZIP_ZIP64", P_ZIP64 },
        { "encrypted.zip", "a.png", NULL, "ZIP_ENCRYPTED", P_ENCRYPTED },
        { "local.zip", "a.png", NULL, "ZIP_LOCAL_MISMATCH", P_LOCAL },
        { "overlap.zip", "a.png", "b.png", "ZIP_OVERLAP", P_OVERLAP },
        { "cd_outside.zip", "a.png", NULL, "ZIP_CENTRAL_DIRECTORY", P_CD_OUTSIDE },
    };
    for (size_t i = 0; i < sizeof(kCells) / sizeof(kCells[0]); i++) {
        Zw z;
        memset(&z, 0, sizeof(z));
        zw_add_text(&z, "textures.ini", kIni, 0);
        zw_add_file(&z, kCells[i].entry, pngp, stored);
        if (kCells[i].entry2) zw_add_file(&z, kCells[i].entry2, pngp, stored);
        pack_path(zip, sizeof(zip), "texrepl_zip_refuse", kCells[i].name);
        zw_finish(&z, zip, 0);
        size_t n = 0;
        uint8_t *d = read_all(zip, &n);
        const size_t c1 = central_at(d, n, 1), l1 = rd32le(d + c1 + 42);
        switch (kCells[i].patch) {
        case P_ZIP64: le16(d + n - 22 + 10, 0xffffu); break;              /* "look in the ZIP64 record" */
        case P_ENCRYPTED: d[c1 + 8] |= 1u; d[l1 + 6] |= 1u; break;        /* flag bit 0 */
        case P_LOCAL: d[l1 + 14] ^= 1u; break;                            /* the local CRC */
        case P_OVERLAP: {                                                /* a.png's data runs into b.png */
            const uint32_t grow = rd32le(d + c1 + 20) + 8u;
            le32(d + c1 + 20, grow); le32(d + c1 + 24, grow);
            le32(d + l1 + 18, grow); le32(d + l1 + 22, grow);
            break;
        }
        case P_CD_OUTSIDE: le32(d + n - 22 + 16, 0x7ffffff0u); break;
        default: break;
        }
        file_save(zip, d, n);
        free(d);
        err[0] = '\0';
        NkTexRepl *p = nk_texrepl_open(zip, 0, err, sizeof(err));
        CHECK(p == NULL && strstr(err, kCells[i].why), "%s: refused with '%s'", kCells[i].name, err);
        CHECK(!strstr(err, "texrepl_zip_refuse"), "%s: the message names folders: '%s'", kCells[i].name, err);
        if (p) nk_texrepl_close(p);
    }

    /* A cut archive, at every length: refused whole each time. */
    {
        Zw z;
        memset(&z, 0, sizeof(z));
        zw_add_text(&z, "textures.ini", kIni, 8);
        zw_add_file(&z, "a.png", pngp, stored);
        pack_path(zip, sizeof(zip), "texrepl_zip_refuse", "whole.zip");
        zw_finish(&z, zip, 0);
        size_t n = 0;
        uint8_t *d = read_all(zip, &n);
        NkTexRepl *p = nk_texrepl_open(zip, 0, err, sizeof(err));
        CHECK(p != NULL, "the uncut archive: %s", err);
        if (p) nk_texrepl_close(p);
        pack_path(zip, sizeof(zip), "texrepl_zip_refuse", "cut.zip");
        int opened = 0, no_code = 0;
        for (size_t cut = 1; cut < n; cut++) {
            file_save(zip, d, n - cut);
            err[0] = '\0';
            p = nk_texrepl_open(zip, 0, err, sizeof(err));
            if (p) { opened++; nk_texrepl_close(p); }
            else if (!strstr(err, "ZIP_")) no_code++;
        }
        CHECK(opened == 0 && no_code == 0, "cut archives: %d of %zu opened, %d refused without a ZIP code", opened,
              n - 1, no_code);
        free(d);
    }

    /* Entries declaring more than 8 GiB unpacked in all: refused whole, nothing unpacked. */
    {
        Zw z;
        memset(&z, 0, sizeof(z));
        zw_add_text(&z, "textures.ini", kIni, 0);
        static const uint8_t junk[4] = { 0x03, 0x00, 0x00, 0x00 };
        const ZwOpt huge = { 8, 0, junk, sizeof(junk), 0, 0xfffffff0u };
        zw_add(&z, "a.png", "x", 1, huge);
        zw_add(&z, "b.png", "x", 1, huge);
        zw_add(&z, "c.png", "x", 1, huge);
        pack_path(zip, sizeof(zip), "texrepl_zip_refuse", "total.zip");
        zw_finish(&z, zip, 0);
        err[0] = '\0';
        NkTexRepl *p = nk_texrepl_open(zip, 0, err, sizeof(err));
        CHECK(p == NULL && strstr(err, "ZIP_TOO_LARGE"), "12 GiB declared: '%s'", err);
        if (p) nk_texrepl_close(p);
    }

    /* Per image: one declaring 100 MiB is skipped before any unpacking (its data is not even
     * DEFLATE); one whose data unpacks past the 1,000 bytes it declares stops there and is skipped:
     * past them the stream turns invalid, so unpacking on would say ZIP_INFLATE, not ZIP_SIZE_MISMATCH. */
    {
        Zw z;
        memset(&z, 0, sizeof(z));
        zw_add_text(&z, "textures.ini",
                    "[options]\nhash = quick\n[hashes]\n088040000000020211223344 = big.png\n"
                    "088040000000020211223345 = bomb.png\n088040000000020211223346 = a.png\n", 0);
        static const uint8_t junk[4] = { 0xff, 0xff, 0xff, 0xff };
        const ZwOpt big = { 8, 0, junk, sizeof(junk), 0, 100u << 20 };
        zw_add(&z, "big.png", "x", 1, big);
        uint8_t *stream = (uint8_t *)calloc(1, 5u + 1100u + 1u);
        stream[0] = 0u;                          /* a stored block of 1,100 bytes, not final */
        le16(stream + 1, 1100u);
        le16(stream + 3, (uint32_t)(~1100u & 0xffffu));
        stream[5 + 1100] = 0x07u;                /* then a final block of the reserved type 3 */
        uint8_t zeros[1000];
        memset(zeros, 0, sizeof(zeros));
        const ZwOpt bomb = { 8, 0, stream, 5u + 1100u + 1u, 0, 0 };
        zw_add(&z, "bomb.png", zeros, sizeof(zeros), bomb);
        free(stream);
        zw_add_file(&z, "a.png", pngp, stored);
        pack_path(zip, sizeof(zip), "texrepl_zip_refuse", "bombs.zip");
        zw_finish(&z, zip, 0);
        NkTexRepl *p = nk_texrepl_open(zip, 0, err, sizeof(err));
        CHECK(p != NULL, "a ZIP with oversized entries: %s", err);
        if (p) {
            NkTexReplHit hit;
            NkTexReplId id = { 0x08804000u, 3u, 0x0202u, 0u, 0x11223344u };
            CHECK(nk_texrepl_lookup(p, &id, &hit) == 0, "a 100 MiB entry was used");
            id.hash = 0x11223345u;
            CHECK(nk_texrepl_lookup(p, &id, &hit) == 0, "an entry unpacking past its size was used");
            id.hash = 0x11223346u;
            CHECK(nk_texrepl_lookup(p, &id, &hit) == 1, "the good entry beside them was not used");
            nk_texrepl_unpin(p, &hit);
            CHECK(has_diag(p, "big.png: not used: it is larger than the cap of 64 MiB"), "100 MiB entry not noted");
            CHECK(has_diag(p, "bomb.png: damaged in the ZIP (ZIP_SIZE_MISMATCH)"), "unpacking past the size not noted");
            NkTexReplStats st;
            nk_texrepl_stats(p, &st);
            CHECK(st.skipped == 2, "skipped=%llu", (unsigned long long)st.skipped);
            nk_texrepl_close(p);
        }
    }
}

/* ------------------------------------------------------------------------------ hardening */

static uint32_t be32_at(const uint8_t *b) {
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | (uint32_t)b[3];
}
static void put_be32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v;
}
/* Rewrites the CRC of the chunk starting at `at`. */
static void png_fix_crc(uint8_t *d, size_t at) {
    uint32_t len = be32_at(d + at);
    put_be32(d + at + 8 + len, nk_zip_crc32(0, d + at + 4, (size_t)len + 4u));
}

/* ------------------------------------------------------------------ PNG writer for the corpus */

/* A byte buffer and a DEFLATE bit writer (LSB first, Huffman codes MSB first, RFC 1951). */
typedef struct { uint8_t *d; size_t n, cap; uint32_t acc; int bits; } Bw;
static void bw_byte(Bw *b, uint8_t v) { zbuf(&b->d, &b->n, &b->cap, &v, 1); }
static void bw_bits(Bw *b, uint32_t v, int n) {
    for (int i = 0; i < n; i++) {
        b->acc |= ((v >> i) & 1u) << b->bits;
        if (++b->bits == 8) { bw_byte(b, (uint8_t)b->acc); b->acc = 0; b->bits = 0; }
    }
}
static void bw_code(Bw *b, uint32_t code, int len) {   /* a Huffman code, its first bit the code's top bit */
    for (int i = len - 1; i >= 0; i--) bw_bits(b, (code >> i) & 1u, 1);
}
static void bw_flush(Bw *b) { if (b->bits) { bw_byte(b, (uint8_t)b->acc); b->acc = 0; b->bits = 0; } }
static void fixed_sym(Bw *b, unsigned sym) {   /* the fixed literal/length code (RFC 1951 3.2.6) */
    if (sym < 144) bw_code(b, 0x30u + sym, 8);
    else if (sym < 256) bw_code(b, 0x190u + sym - 144u, 9);
    else if (sym < 280) bw_code(b, sym - 256u, 7);
    else bw_code(b, 0xc0u + sym - 280u, 8);
}
/* A match of `len` (3 to 10, or 258) at distance 1. */
static void fixed_match(Bw *b, unsigned len) {
    fixed_sym(b, len == 258 ? 285u : 254u + len);
    bw_code(b, 0, 5);   /* distance code 0: distance 1 */
}

/* `data` as one fixed-Huffman block (runs of a byte as matches at distance 1) or as stored blocks. */
static void deflate_into(Bw *b, const uint8_t *data, size_t n, int huffman) {
    if (!huffman) {
        size_t at = 0;
        do {
            size_t len = n - at > 65535u ? 65535u : n - at;
            bw_bits(b, at + len == n ? 1u : 0u, 1);
            bw_bits(b, 0, 2);
            bw_flush(b);
            uint8_t h[4];
            le16(h, (uint32_t)len);
            le16(h + 2, (uint32_t)(~len & 0xffffu));
            zbuf(&b->d, &b->n, &b->cap, h, 4);
            zbuf(&b->d, &b->n, &b->cap, data + at, len);
            at += len;
        } while (at < n);
        return;
    }
    bw_bits(b, 1, 1);   /* final */
    bw_bits(b, 1, 2);   /* fixed Huffman */
    size_t i = 0;
    while (i < n) {
        fixed_sym(b, data[i]);
        size_t run = 0;
        while (i + 1 + run < n && data[i + 1 + run] == data[i]) run++;
        i++;
        while (run >= 3) {
            const unsigned len = run >= 258 ? 258u : run > 10 ? 10u : (unsigned)run;
            fixed_match(b, len);
            run -= len;
            i += len;
        }
    }
    fixed_sym(b, 256);
    bw_flush(b);
}

static uint32_t t_adler32(const uint8_t *d, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + d[i]) % 65521u; b = (b + a) % 65521u; }
    return b << 16 | a;
}

static void png_chunk_out(Bw *o, const char *type, const uint8_t *data, size_t n) {
    uint8_t h[8];
    put_be32(h, (uint32_t)n);
    memcpy(h + 4, type, 4);
    zbuf(&o->d, &o->n, &o->cap, h, 8);
    if (n) zbuf(&o->d, &o->n, &o->cap, data, n);
    uint32_t crc = nk_zip_crc32(nk_zip_crc32(0, type, 4), data, n);
    put_be32(h, crc);
    zbuf(&o->d, &o->n, &o->cap, h, 4);
}

typedef struct {
    int w, h, depth, color, interlace, trns, huffman, idat_parts;
    size_t extra;        /* bytes of image data past the image's end */
    uint32_t seed;
} PngSpec;

static uint32_t t_rand(uint32_t *s) { *s = *s * 1103515245u + 12345u; return *s >> 8; }

static void put_sample(uint8_t *row, size_t i, unsigned depth, unsigned v) {
    if (depth == 16) { row[2 * i] = (uint8_t)(v >> 8); row[2 * i + 1] = (uint8_t)v; return; }
    if (depth == 8) { row[i] = (uint8_t)v; return; }
    const size_t bit = i * depth;
    row[bit / 8] |= (uint8_t)(v << (8u - depth - (unsigned)(bit % 8u)));
}

static unsigned t_paeth(unsigned a, unsigned b, unsigned c) {
    const int p = (int)a + (int)b - (int)c;
    const int pa = abs(p - (int)a), pb = abs(p - (int)b), pc = abs(p - (int)c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* Writes a PNG of the given form with random samples (seeded), every filter type in turn, Adam7 if
 * asked, palette and tRNS as the format allows, the image data in `idat_parts` chunks. */
static uint8_t *png_build(const PngSpec *sp, size_t *out_n) {
    static const unsigned char adam7[7][4] = {
        { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 }, { 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 },
    };
    const unsigned ch = sp->color == 2 ? 3u : sp->color == 4 ? 2u : sp->color == 6 ? 4u : 1u;
    const unsigned maxv = sp->depth == 16 ? 0xffffu : (1u << sp->depth) - 1u;
    uint32_t seed = sp->seed;
    const unsigned pal_n = sp->color == 3 ? 1u + t_rand(&seed) % (maxv + 1u) : 0u;
    /* every pixel's samples, full image */
    unsigned *px = (unsigned *)malloc((size_t)sp->w * sp->h * ch * sizeof(unsigned));
    for (size_t i = 0; i < (size_t)sp->w * sp->h; i++)
        for (unsigned c = 0; c < ch; c++) px[i * ch + c] = sp->color == 3 ? t_rand(&seed) % pal_n : t_rand(&seed) & maxv;
    unsigned key[3] = { 0, 0, 0 };
    if (sp->trns && sp->color != 3) for (unsigned c = 0; c < ch; c++) key[c] = px[c];   /* the first pixel's colour */
    Bw raw = { 0 };
    int row_no = 0;
    for (int k = 0; k < (sp->interlace ? 7 : 1); k++) {
        const unsigned x0 = sp->interlace ? adam7[k][0] : 0, y0 = sp->interlace ? adam7[k][1] : 0;
        const unsigned dx = sp->interlace ? adam7[k][2] : 1, dy = sp->interlace ? adam7[k][3] : 1;
        const unsigned pw = (unsigned)sp->w > x0 ? ((unsigned)sp->w - x0 + dx - 1) / dx : 0;
        const unsigned ph = (unsigned)sp->h > y0 ? ((unsigned)sp->h - y0 + dy - 1) / dy : 0;
        if (!pw || !ph) continue;
        const size_t rb = ((size_t)pw * sp->depth * ch + 7) / 8, bpp = (sp->depth * ch + 7) / 8;
        uint8_t *prev = (uint8_t *)calloc(rb + 1, 1), *cur = (uint8_t *)calloc(rb + 1, 1);
        for (unsigned y = 0; y < ph; y++) {
            memset(cur, 0, rb);
            for (unsigned x = 0; x < pw; x++)
                for (unsigned c = 0; c < ch; c++)
                    put_sample(cur, (size_t)x * ch + c, (unsigned)sp->depth,
                               px[(((size_t)y0 + (size_t)y * dy) * sp->w + x0 + (size_t)x * dx) * ch + c]);
            const unsigned f = (unsigned)(row_no++ % 5);
            bw_byte(&raw, (uint8_t)f);
            for (size_t i = 0; i < rb; i++) {
                const unsigned a = i >= bpp ? cur[i - bpp] : 0, b = y ? prev[i] : 0, c = (y && i >= bpp) ? prev[i - bpp] : 0;
                const unsigned pred = f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2 : f == 4 ? t_paeth(a, b, c) : 0;
                bw_byte(&raw, (uint8_t)(cur[i] - pred));
            }
            memcpy(prev, cur, rb);
        }
        free(prev);
        free(cur);
    }
    for (size_t i = 0; i < sp->extra; i++) bw_byte(&raw, (uint8_t)(i * 7u));
    Bw z = { 0 };
    bw_byte(&z, 0x78);
    bw_byte(&z, 0x01);
    deflate_into(&z, raw.d, raw.n, sp->huffman);
    uint8_t ad[4];
    put_be32(ad, t_adler32(raw.d, raw.n));
    zbuf(&z.d, &z.n, &z.cap, ad, 4);
    Bw o = { 0 };
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    zbuf(&o.d, &o.n, &o.cap, sig, 8);
    uint8_t ihdr[13];
    put_be32(ihdr, (uint32_t)sp->w);
    put_be32(ihdr + 4, (uint32_t)sp->h);
    ihdr[8] = (uint8_t)sp->depth;
    ihdr[9] = (uint8_t)sp->color;
    ihdr[10] = ihdr[11] = 0;
    ihdr[12] = (uint8_t)sp->interlace;
    png_chunk_out(&o, "IHDR", ihdr, 13);
    if (sp->color == 3) {
        uint8_t pal[768];
        for (unsigned i = 0; i < pal_n * 3; i++) pal[i] = (uint8_t)t_rand(&seed);
        png_chunk_out(&o, "PLTE", pal, pal_n * 3);
        if (sp->trns) {
            uint8_t t[256];
            const unsigned tn = 1 + t_rand(&seed) % pal_n;
            for (unsigned i = 0; i < tn; i++) t[i] = (uint8_t)t_rand(&seed);
            png_chunk_out(&o, "tRNS", t, tn);
        }
    } else if (sp->trns && (sp->color == 0 || sp->color == 2)) {
        uint8_t t[6];
        for (unsigned c = 0; c < ch; c++) { t[2 * c] = (uint8_t)(key[c] >> 8); t[2 * c + 1] = (uint8_t)key[c]; }
        png_chunk_out(&o, "tRNS", t, ch * 2);
    }
    const int parts = sp->idat_parts > 0 ? sp->idat_parts : 1;
    size_t at = 0;
    for (int k = 0; k < parts; k++) {
        const size_t len = k == parts - 1 ? z.n - at : z.n / (size_t)parts;
        png_chunk_out(&o, "IDAT", z.d + at, len);
        at += len;
    }
    png_chunk_out(&o, "IEND", NULL, 0);
    free(px);
    free(raw.d);
    free(z.d);
    *out_n = o.n;
    return o.d;
}

/* A 1x1 RGBA PNG whose image data unpacks to `mib` MiB: a filter byte, then runs of zeros. */
static uint8_t *png_bomb(unsigned mib, unsigned side, size_t *out_n) {
    Bw z = { 0 };
    bw_byte(&z, 0x78);
    bw_byte(&z, 0x01);
    bw_bits(&z, 1, 1);
    bw_bits(&z, 1, 2);
    fixed_sym(&z, 0);
    const uint64_t total = (uint64_t)mib << 20;
    for (uint64_t made = 1; made + 258 <= total; made += 258) fixed_match(&z, 258);
    fixed_sym(&z, 256);
    bw_flush(&z);
    uint8_t ad[4] = { 0, 0, 0, 1 };
    zbuf(&z.d, &z.n, &z.cap, ad, 4);
    Bw o = { 0 };
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    zbuf(&o.d, &o.n, &o.cap, sig, 8);
    const uint8_t ihdr[13] = { 0, 0, (uint8_t)(side >> 8), (uint8_t)side, 0, 0, (uint8_t)(side >> 8), (uint8_t)side,
                               8, 6, 0, 0, 0 };
    png_chunk_out(&o, "IHDR", ihdr, 13);
    png_chunk_out(&o, "IDAT", z.d, z.n);
    png_chunk_out(&o, "IEND", NULL, 0);
    free(z.d);
    *out_n = o.n;
    return o.d;
}

/* Truncated, flipped, bad-CRC and oversized PNGs are refused before decoding, and every
 * byte of the image data flipped with its CRC made right again decodes or is refused, never more. */
static void test_png_corpus(void) {
    char dir[512], path[600], why[96];
    make_pack(dir, sizeof(dir), "texrepl_png_corpus");
    uint32_t *px = solid(8, 8, 0xff336699u);
    px[5] = 0xff000000u;
    pack_path(path, sizeof(path), "texrepl_png_corpus", "good.png");
    write_png(path, 8, 8, px);
    size_t n = 0;
    uint8_t *png = read_all(path, &n);
    uint32_t *rgba = NULL;
    int w = 0, h = 0;
    CHECK(nk_texrepl_decode_png(png, n, 0, &rgba, &w, &h, why, sizeof(why)) && w == 8 && h == 8 && rgba &&
              rgba[5] == 0xff000000u && rgba[6] == 0xff336699u,
          "a good PNG did not decode: %s", why);
    free(rgba);
    uint8_t *m = (uint8_t *)malloc(n + 16);
    /* Every truncation. */
    int bad = 0;
    for (size_t cut = 0; cut < n; cut++) {
        memcpy(m, png, cut);
        rgba = NULL;
        if (nk_texrepl_decode_png(m, cut, 0, &rgba, &w, &h, why, sizeof(why)) || rgba) bad++;
        free(rgba);
    }
    CHECK(bad == 0, "%d truncations of %zu decoded", bad, n);
    /* Every byte flipped, CRC left as it was: the checks refuse each one. */
    bad = 0;
    for (size_t i = 0; i < n; i++) {
        memcpy(m, png, n);
        m[i] ^= 0xffu;
        rgba = NULL;
        if (nk_texrepl_decode_png(m, n, 0, &rgba, &w, &h, why, sizeof(why)) || rgba) bad++;
        free(rgba);
    }
    CHECK(bad == 0, "%d single-byte flips with the CRC unchanged decoded", bad);
    memcpy(m, png, n);
    m[8 + 8 + 13 + 3] ^= 1u;   /* IHDR's CRC */
    CHECK(!nk_texrepl_decode_png(m, n, 0, &rgba, &w, &h, why, sizeof(why)) && strstr(why, "chunk IHDR has a wrong CRC"),
          "a bad IHDR CRC: '%s'", why);
    /* Image-data bytes flipped with the CRC made right: stb_image decodes 8x8 or refuses. */
    size_t at = 8, idat_at = 0;
    while (at + 12 <= n) {
        if (!memcmp(png + at + 4, "IDAT", 4)) { idat_at = at; break; }
        at += be32_at(png + at) + 12u;
    }
    CHECK(idat_at != 0, "no IDAT in the synthetic PNG");
    int decoded = 0, refused = 0, wrong = 0;
    static const uint8_t kFlips[] = { 0x01u, 0x10u, 0x80u, 0xffu };
    for (uint32_t k = 0; idat_at && k < be32_at(png + idat_at); k++) {
        for (size_t f = 0; f < sizeof(kFlips); f++) {
            memcpy(m, png, n);
            m[idat_at + 8 + k] ^= kFlips[f];
            png_fix_crc(m, idat_at);
            rgba = NULL;
            if (nk_texrepl_decode_png(m, n, 0, &rgba, &w, &h, why, sizeof(why))) {
                decoded++;
                if (w != 8 || h != 8 || !rgba) wrong++;
            } else {
                refused++;
                if (rgba) wrong++;
            }
            free(rgba);
        }
    }
    CHECK(wrong == 0 && decoded + refused > 0, "IDAT flips: %d decoded, %d refused, %d wrong", decoded, refused, wrong);
    printf("png corpus: %zu truncations and %zu flips refused; %d IDAT flips with fixed CRCs decoded, %d refused\n",
           n, n, decoded, refused);
    /* Every IHDR pixel format (colour type, depth, interlace) over the same image data, CRC made
     * right: stb_image decodes or refuses each, never more than the IHDR size. */
    int formats = 0;
    for (unsigned color = 0; color <= 6; color++) {
        for (unsigned depth = 1; depth <= 16; depth <<= 1) {
            for (unsigned lace = 0; lace <= 1; lace++) {
                memcpy(m, png, n);
                m[24] = (uint8_t)depth;
                m[25] = (uint8_t)color;
                m[28] = (uint8_t)lace;
                png_fix_crc(m, 8);
                rgba = NULL;
                const int fmt_ok = nk_texrepl_decode_png(m, n, 0, &rgba, &w, &h, why, sizeof(why));
                if (fmt_ok && (w != 8 || h != 8 || !rgba)) wrong++;
                const int defined = (color == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) ||
                                    (color == 3 && depth <= 8) || ((color == 2 || color == 4 || color == 6) && depth >= 8);
                if (!defined && (fmt_ok || !strstr(why, "IHDR names no PNG pixel format"))) wrong++;
                if (color == 1 || color == 5) {   /* not PNG colour types at all */
                    if (fmt_ok || !strstr(why, "IHDR names no PNG pixel format")) wrong++;
                }
                free(rgba);
                formats++;
            }
        }
    }
    CHECK(wrong == 0, "IHDR formats: %d wrong", wrong);
    /* A seeded random pass: 1 to 8 bytes after the signature set at random, then every chunk's CRC
     * made right where the lengths still allow, over three source images. */
    {
        static const int kSizes[3][2] = { { 8, 8 }, { 1, 1 }, { 33, 5 } };
        uint8_t *src[3];
        size_t sn[3];
        for (int k = 0; k < 3; k++) {
            uint32_t *img = solid(kSizes[k][0], kSizes[k][1], 0x80402010u + (uint32_t)k);
            for (int t = 0; t < kSizes[k][0] * kSizes[k][1]; t++) img[t] ^= (uint32_t)t * 2654435761u;
            pack_path(path, sizeof(path), "texrepl_png_corpus", "fuzz_src.png");
            write_png(path, kSizes[k][0], kSizes[k][1], img);
            free(img);
            src[k] = read_all(path, &sn[k]);
        }
        uint8_t *fm = (uint8_t *)malloc(sn[0] + sn[1] + sn[2]);
        uint32_t seed = 0x2365u;
        const int kRuns = 30000;
        int fz_ok = 0, fz_no = 0, fz_wrong = 0;
        for (int r = 0; r < kRuns; r++) {
            const int k = r % 3;
            memcpy(fm, src[k], sn[k]);
            seed = seed * 1103515245u + 12345u;
            const int flips = 1 + (int)((seed >> 16) % 8u);
            for (int j = 0; j < flips; j++) {
                seed = seed * 1103515245u + 12345u;
                const size_t pos = 8u + (size_t)(seed >> 8) % (sn[k] - 8u);
                seed = seed * 1103515245u + 12345u;
                fm[pos] = (uint8_t)(seed >> 16);
            }
            for (size_t at = 8; at + 12u <= sn[k];) {   /* every CRC made right while the lengths allow */
                const uint32_t len = be32_at(fm + at);
                if (len > sn[k] - at - 12u) break;
                png_fix_crc(fm, at);
                at += (size_t)len + 12u;
            }
            rgba = NULL;
            if (nk_texrepl_decode_png(fm, sn[k], 0, &rgba, &w, &h, why, sizeof(why))) {
                fz_ok++;
                if (!rgba || w < 1 || h < 1 || w > 2048 || h > 2048) fz_wrong++;
            } else {
                fz_no++;
                if (rgba) fz_wrong++;
            }
            free(rgba);
        }
        CHECK(fz_wrong == 0, "fuzz: %d results out of bounds", fz_wrong);
        printf("png fuzz: %d IHDR formats; %d seeded runs over 3 images (seed 0x2365): %d decoded, %d refused, "
               "%d out of bounds, no crash\n", formats, kRuns, fz_ok, fz_no, fz_wrong);
        free(fm);
        for (int k = 0; k < 3; k++) free(src[k]);
    }
    /* Sizes in IHDR: 4096 a side and 0 refused; 2048x2048 refused over an allocation cap. */
    static const struct { uint32_t w, h; uint64_t cap; const char *why; } kDims[] = {
        { 4096u, 8u, 0u, "not 1 to 2048" }, { 8u, 4096u, 0u, "not 1 to 2048" }, { 0u, 8u, 0u, "not 1 to 2048" },
        { 0x80000000u, 0x80000000u, 0u, "not 1 to 2048" }, { 2048u, 2048u, 1u << 20, "more than the 1024 KiB" },
    };
    for (size_t i = 0; i < sizeof(kDims) / sizeof(kDims[0]); i++) {
        memcpy(m, png, n);
        put_be32(m + 16, kDims[i].w);
        put_be32(m + 20, kDims[i].h);
        png_fix_crc(m, 8);
        rgba = NULL;
        CHECK(!nk_texrepl_decode_png(m, n, kDims[i].cap, &rgba, &w, &h, why, sizeof(why)) && !rgba &&
                  strstr(why, kDims[i].why),
              "IHDR %lux%lu: '%s'", (unsigned long)kDims[i].w, (unsigned long)kDims[i].h, why);
    }
    /* Bytes after IEND are ignored, as decoders ignore them. */
    memcpy(m, png, n);
    memcpy(m + n, "trailing bytes!!", 16);
    rgba = NULL;
    CHECK(nk_texrepl_decode_png(m, n + 16, 0, &rgba, &w, &h, why, sizeof(why)) && w == 8, "bytes after IEND: %s", why);
    free(rgba);

    /* In a pack: a 2048x2048 IHDR over a 1 MiB budget, a bad CRC and a truncation are skipped, each
     * noted and counted once in stats.skipped. */
    memcpy(m, png, n);
    put_be32(m + 16, 2048u);
    put_be32(m + 20, 2048u);
    png_fix_crc(m, 8);
    pack_path(path, sizeof(path), "texrepl_png_corpus", "huge.png");
    FILE *f = nk_fopen_utf8(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    fwrite(m, 1, n, f);
    fclose(f);
    memcpy(m, png, n);
    m[n - 1] ^= 1u;
    pack_path(path, sizeof(path), "texrepl_png_corpus", "crc.png");
    f = nk_fopen_utf8(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    fwrite(m, 1, n, f);
    fclose(f);
    pack_path(path, sizeof(path), "texrepl_png_corpus", "cut.png");
    f = nk_fopen_utf8(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    fwrite(png, 1, n / 2, f);
    fclose(f);
    pack_path(path, sizeof(path), "texrepl_png_corpus", "textures.ini");
    write_text(path, "[options]\nhash = quick\n[hashes]\n"
                     "0880400000000202c0000001 = huge.png\n0880400000000202c0000002 = crc.png\n"
                     "0880400000000202c0000003 = cut.png\n0880400000000202c0000004 = good.png\n");
    char err[256] = "";
    NkTexRepl *p = nk_texrepl_open(dir, 1u << 20, err, sizeof(err));
    CHECK(p != NULL, "png corpus pack: %s", err);
    if (p) {
        NkTexReplHit hit;
        for (int round = 0; round < 2; round++) {
            for (uint32_t i = 1; i <= 3; i++) {
                NkTexReplId id = { 0x08804000u, 3u, 0x0202u, 0u, 0xc0000000u + i };
                CHECK(nk_texrepl_lookup(p, &id, &hit) == 0, "bad PNG %u replaced a texture", (unsigned)i);
            }
        }
        NkTexReplId good = { 0x08804000u, 3u, 0x0202u, 0u, 0xc0000004u };
        CHECK(nk_texrepl_lookup(p, &good, &hit) == 1, "the good PNG beside bad ones was not used");
        nk_texrepl_unpin(p, &hit);
        NkTexReplStats st;
        nk_texrepl_stats(p, &st);
        CHECK(st.skipped == 3 && st.over_budget == 1 && st.loaded == 1, "skipped=%llu over=%llu loaded=%llu",
              (unsigned long long)st.skipped, (unsigned long long)st.over_budget, (unsigned long long)st.loaded);
        CHECK(has_diag(p, "huge.png: 2048x2048 needs 16384 KiB"), "huge image not noted");
        CHECK(has_diag(p, "crc.png: not a PNG this module decodes (chunk IEND has a wrong CRC)"), "bad CRC not noted");
        CHECK(has_diag(p, "cut.png: not a PNG this module decodes (cut short"), "truncation not noted");
        nk_texrepl_close(p);
    }
    free(m);
    free(png);
    free(px);
}

#if defined(_WIN32)
static int make_dir_link(const char *link, const char *target) {
    char cmd[1600], full[700], name[700];
    if (!GetFullPathNameA(target, sizeof(full), full, NULL)) return 0;
    snprintf(name, sizeof(name), "%s", link);
    for (char *c = name; *c; c++) if (*c == '/') *c = '\\';   /* the link's name only, so "/J" stays */
    snprintf(cmd, sizeof(cmd), "mklink /J \"%s\" \"%s\" >NUL 2>&1", name, full);
    return system(cmd) == 0;
}
static int make_file_link(const char *link, const char *target) {
    WCHAR wl[700], wt[700];
    if (!MultiByteToWideChar(CP_UTF8, 0, link, -1, wl, 700) || !MultiByteToWideChar(CP_UTF8, 0, target, -1, wt, 700))
        return 0;
    for (WCHAR *c = wl; *c; c++) if (*c == L'/') *c = L'\\';
    for (WCHAR *c = wt; *c; c++) if (*c == L'/') *c = L'\\';
    WCHAR full[700];
    if (!GetFullPathNameW(wt, 700, full, NULL)) return 0;
    return CreateSymbolicLinkW(wl, full, 0x2 /* SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE */) != 0;
}
#else
static int make_abs(const char *target, char *out, size_t cap) {
    char *r = realpath(target, NULL);
    if (!r) return 0;
    snprintf(out, cap, "%s", r);
    free(r);
    return 1;
}
static int make_dir_link(const char *link, const char *target) {
    char abs[4096];
    return make_abs(target, abs, sizeof(abs)) && symlink(abs, link) == 0;
}
static int make_file_link(const char *link, const char *target) { return make_dir_link(link, target); }
#endif

/* A [hashes] value with a device name or a name Windows changes is not opened; a link or junction
 * that leaves the pack folder is not followed; a FIFO is refused without blocking (POSIX); a
 * textures.ini over 4 MiB refuses the pack; a pack's error names only its last path component. */
static void test_file_checks(void) {
    char dir[512], path[600], target[600], err[256] = "";
    make_pack(dir, sizeof(dir), "texrepl_pack_files");
    char outside[512];
    make_pack(outside, sizeof(outside), "texrepl_pack_outside");
    uint32_t *px = solid(4, 4, 0xff445566u);
    pack_path(path, sizeof(path), "texrepl_pack_outside", "out.png");
    write_png(path, 4, 4, px);
    pack_path(path, sizeof(path), "texrepl_pack_files", "in.png");
    write_png(path, 4, 4, px);
    pack_path(path, sizeof(path), "texrepl_pack_files", "sub");
    nk_platform_mkdir_p(path);
    pack_path(path, sizeof(path), "texrepl_pack_files", "folder.png");   /* a folder named as an image */
    nk_platform_mkdir_p(path);

    /* Links: a junction (Windows) or symlink to a folder outside, a file link outside and one inside. */
    pack_path(path, sizeof(path), "texrepl_pack_files", "jdir");
    int have_dir_link = nk_platform_dir_exists(path) || make_dir_link(path, outside);
    pack_path(path, sizeof(path), "texrepl_pack_files", "flink.png");
    pack_path(target, sizeof(target), "texrepl_pack_outside", "out.png");
    int have_file_link = nk_platform_file_exists(path) || make_file_link(path, target);
    pack_path(path, sizeof(path), "texrepl_pack_files", "inlink.png");
    pack_path(target, sizeof(target), "texrepl_pack_files", "in.png");
    int have_in_link = nk_platform_file_exists(path) || make_file_link(path, target);
    if (!have_dir_link) printf("SKIP folder link out of the pack: the link could not be made on this host\n");
    if (!have_file_link || !have_in_link)
        printf("SKIP file links: a symbolic link could not be made here (Windows needs Developer Mode)\n");
#if !defined(_WIN32)
    pack_path(path, sizeof(path), "texrepl_pack_files", "fifo.png");
    int have_fifo = mkfifo(path, 0600) == 0 || errno == EEXIST;
    if (!have_fifo) printf("SKIP FIFO: mkfifo failed\n");
#endif
    pack_path(path, sizeof(path), "texrepl_pack_files", "textures.ini");
    write_text(path, "[options]\nhash = quick\n[hashes]\n"
                     "0880400000000202d0000001 = CON.png\n0880400000000202d0000002 = sub/COM1.png\n"
                     "0880400000000202d0000003 = sub./in.png\n0880400000000202d0000004 = jdir/out.png\n"
                     "0880400000000202d0000005 = flink.png\n0880400000000202d0000006 = inlink.png\n"
                     "0880400000000202d0000007 = fifo.png\n0880400000000202d0000008 = in.png\n"
                     "0880400000000202d0000009 = sub\n0880400000000202d000000a = NUL.png\n"
                     "0880400000000202d000000b = folder.png\n0880400000000202d000000c = CON .png\n");
    NkTexRepl *p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL, "file-check pack: %s", err);
    if (p) {
        NkTexReplHit hit;
        int expect[13] = { 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0 };
        for (uint32_t i = 1; i <= 12; i++) {
            if ((i == 4 && !have_dir_link) || ((i == 5 || i == 6) && (!have_file_link || !have_in_link))) continue;
#if defined(_WIN32)
            if (i == 7) continue;
#else
            if (i == 7 && !have_fifo) continue;
#endif
            NkTexReplId id = { 0x08804000u, 3u, 0x0202u, 0u, 0xd0000000u + i };
            int got = nk_texrepl_lookup(p, &id, &hit);
            CHECK(got == expect[i], "file check %u: lookup gave %d", (unsigned)i, got);
            if (got) nk_texrepl_unpin(p, &hit);
        }
        CHECK(has_diag(p, "CON.png: not a file name this module opens (ZIP_NAME_RESERVED"), "CON.png not refused");
        CHECK(has_diag(p, "sub/COM1.png: not a file name this module opens (ZIP_NAME_RESERVED"), "COM1.png not refused");
        CHECK(has_diag(p, "sub./in.png: not a file name this module opens (ZIP_NAME_CHARACTER"), "trailing dot not refused");
        CHECK(has_diag(p, "NUL.png: not a file name this module opens (ZIP_NAME_RESERVED"), "NUL.png not refused");
        CHECK(has_diag(p, "CON .png: not a file name this module opens (ZIP_NAME_RESERVED"), "CON .png not refused");
#if defined(_WIN32)
        /* A folder does not open as a file without backup semantics, so it is never read. */
        CHECK(has_diag(p, "folder.png: not used: it is missing or cannot be opened"), "a folder image not refused");
#else
        CHECK(has_diag(p, "folder.png: not used: it is not a regular file"), "a folder image not refused");
#endif
        if (have_dir_link)
            CHECK(has_diag(p, "jdir/out.png: not used: it resolves outside the pack folder"), "folder link followed out");
        if (have_file_link && have_in_link)
            CHECK(has_diag(p, "flink.png: not used: it resolves outside the pack folder"), "file link followed out");
#if !defined(_WIN32)
        if (have_fifo) CHECK(has_diag(p, "fifo.png: not used: it is not a regular file"), "FIFO not refused");
#endif
        nk_texrepl_close(p);
    }

    /* A pack whose images go away after it opened (a drive unplugged, files deleted): every lookup
     * returns at once, unreplaced, and each image is skipped with a note. */
    char gone_dir[512];
    make_pack(gone_dir, sizeof(gone_dir), "texrepl_pack_gone");
    pack_path(path, sizeof(path), "texrepl_pack_gone", "088040000000020211223344.png");
    write_png(path, 4, 4, px);
    pack_path(target, sizeof(target), "texrepl_pack_gone", "088040000000020211223345.png");
    write_png(target, 4, 4, px);
    err[0] = '\0';
    p = nk_texrepl_open(gone_dir, 0, err, sizeof(err));
    CHECK(p != NULL, "the pack before its files went: %s", err);
    if (p) {
        CHECK(remove(path) == 0 && remove(target) == 0, "cannot remove the pack's files");
        NkTexReplHit hit;
        for (uint32_t i = 0; i < 2; i++) {
            NkTexReplId id = { 0x08804000u, 3u, 0x0202u, 0u, 0x11223344u + i };
            CHECK(nk_texrepl_lookup(p, &id, &hit) == 0, "an image that went away replaced a texture");
        }
        NkTexReplStats st;
        nk_texrepl_stats(p, &st);
        CHECK(st.skipped == 2 && has_diag(p, "088040000000020211223344.png: not used: it is missing"),
              "images that went away: skipped=%llu", (unsigned long long)st.skipped);
        nk_texrepl_close(p);
    }

    /* A textures.ini one byte over 4 MiB. */
    char big_dir[512];
    make_pack(big_dir, sizeof(big_dir), "texrepl_pack_bigini");
    pack_path(path, sizeof(path), "texrepl_pack_bigini", "textures.ini");
    FILE *f = nk_fopen_utf8(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    fputs("[options]\nhash = quick\n", f);
    long left = (4L << 20) + 1 - (long)strlen("[options]\nhash = quick\n");
    while (left > 0) { fputc(left % 64 == 0 ? '\n' : '#', f); left--; }
    fclose(f);
    err[0] = '\0';
    p = nk_texrepl_open(big_dir, 0, err, sizeof(err));
    CHECK(p == NULL && strstr(err, "larger than 4 MiB"), "a textures.ini over 4 MiB: '%s'", err);
    if (p) nk_texrepl_close(p);

#if !defined(_WIN32)
    /* A FIFO named as a ZIP pack is refused, and one named textures.zip in a folder is not read: the
     * folder opens as a folder pack, and neither waits for a writer. */
    char fifo_dir[512];
    make_pack(fifo_dir, sizeof(fifo_dir), "texrepl_pack_fifozip");
    pack_path(path, sizeof(path), "texrepl_pack_fifozip", "textures.ini");
    write_text(path, "[options]\nhash = quick\n");
    pack_path(path, sizeof(path), "texrepl_pack_fifozip", "textures.zip");
    if (mkfifo(path, 0600) == 0 || errno == EEXIST) {
        err[0] = '\0';
        p = nk_texrepl_open(fifo_dir, 0, err, sizeof(err));
        CHECK(p != NULL && !nk_texrepl_is_zip(p), "FIFO textures.zip: '%s'", err);
        if (p) nk_texrepl_close(p);
        err[0] = '\0';
        p = nk_texrepl_open(path, 0, err, sizeof(err));
        CHECK(p == NULL && err[0], "FIFO ZIP pack opened");
        if (p) nk_texrepl_close(p);
    } else {
        printf("SKIP FIFO ZIP: mkfifo failed\n");
    }
#endif

    /* Errors carry the pack's last path component only. */
    err[0] = '\0';
    CHECK(nk_texrepl_open(SR_SELFTEST_BUILD_ROOT "/texrepl_no_such_pack/", 0, err, sizeof(err)) == NULL &&
              strstr(err, "'texrepl_no_such_pack'") && !strchr(err, '/') && !strchr(err, '\\'),
          "a missing pack's message: '%s'", err);
    free(px);
}

/* ----------------------------------------------------------- PNG seeds, decoded two ways */

/* Every PNG pixel format, plain and Adam7, with and without tRNS, in stored and fixed-Huffman
 * DEFLATE, the image data in one to three IDAT chunks, every filter type: the pack reader's own
 * decoder and stb_image (vendored, used here only as a second decoder) must give the same pixels.
 * Image data longer than the image is refused here (stb_image takes it). Then a seeded mutation
 * pass over all of them: wherever both decode, they must agree. */
/* stb_image v2.30 reads a zero-length first IDAT with memcpy into a buffer it has not allocated
 * yet, a NULL pointer (stbi__getn), which UndefinedBehaviorSanitizer stops on. Such an input is not
 * given to it: `*not_run` is set and NULL returned. The pack reader decodes no PNG with stb_image. */
static unsigned char *stb_png(const uint8_t *d, size_t n, int *w, int *h, int *not_run) {
    int comp = 0;
    *not_run = 0;
    for (size_t at = 8; at + 12u <= n;) {
        const uint32_t len = be32_at(d + at);
        if (len == 0 && memcmp(d + at + 4, "IDAT", 4) == 0) { *not_run = 1; return NULL; }
        if (len > n - at - 12u) break;
        at += (size_t)len + 12u;
    }
    return stbi_load_from_memory(d, (int)n, w, h, &comp, 4);
}

static void test_png_seeds(void) {
    static const int kForms[][2] = { { 0, 1 }, { 0, 2 }, { 0, 4 }, { 0, 8 }, { 0, 16 }, { 2, 8 }, { 2, 16 }, { 3, 1 },
                                     { 3, 2 }, { 3, 4 }, { 3, 8 }, { 4, 8 }, { 4, 16 }, { 6, 8 }, { 6, 16 } };
    static const int kSizes[][2] = { { 1, 1 }, { 7, 5 }, { 33, 9 } };
    enum { kMaxSeeds = 200 };
    uint8_t *seed[kMaxSeeds];
    size_t seed_n[kMaxSeeds];
    int seeds = 0, agree = 0;
    char why[96];
    for (size_t f = 0; f < sizeof(kForms) / sizeof(kForms[0]); f++) {
        const int color = kForms[f][0], depth = kForms[f][1];
        const int trns_ok = color == 0 || color == 2 || color == 3;
        for (int lace = 0; lace <= 1; lace++) {
            for (size_t s = 0; s < sizeof(kSizes) / sizeof(kSizes[0]); s++) {
                for (int trns = 0; trns <= trns_ok; trns++) {
                    PngSpec sp = { kSizes[s][0], kSizes[s][1], depth, color, lace, trns, seeds % 2, 1 + seeds % 3, 0,
                                   0x9e3779b9u * (uint32_t)(seeds + 1) };
                    size_t n = 0;
                    uint8_t *png = png_build(&sp, &n);
                    uint32_t *ours = NULL;
                    int w = 0, h = 0, sw = 0, sh = 0, not_run = 0;
                    const int ok = nk_texrepl_decode_png(png, n, 0, &ours, &w, &h, why, sizeof(why));
                    unsigned char *theirs = stb_png(png, n, &sw, &sh, &not_run);
                    const int same = ok && theirs && w == sw && h == sh && w == sp.w && h == sp.h &&
                                     memcmp(ours, theirs, (size_t)w * h * 4) == 0;
                    CHECK(same, "seed colour %d depth %d %s %dx%d tRNS %d %s: ours %d (%s), stb %d", color, depth,
                          lace ? "Adam7" : "plain", sp.w, sp.h, trns, sp.huffman ? "Huffman" : "stored", ok, why,
                          theirs != NULL);
                    agree += same;
                    free(ours);
                    if (theirs) stbi_image_free(theirs);
                    if (seeds < kMaxSeeds) { seed[seeds] = png; seed_n[seeds] = n; seeds++; }
                    else free(png);
                }
            }
        }
    }
    /* Image data longer than the image: refused, however little longer. */
    int longer = 0, stb_took_longer = 0;
    static const size_t kExtra[] = { 1, 7, 1000, 100000 };
    for (size_t i = 0; i < sizeof(kExtra) / sizeof(kExtra[0]); i++) {
        PngSpec sp = { 7, 5, 8, 6, 0, 0, (int)(i % 2), 1, kExtra[i], 77u + (uint32_t)i };
        size_t n = 0;
        uint8_t *png = png_build(&sp, &n);
        uint32_t *ours = NULL;
        int w = 0, h = 0, sw = 0, sh = 0, not_run = 0;
        if (!nk_texrepl_decode_png(png, n, 0, &ours, &w, &h, why, sizeof(why)) &&
            (strstr(why, "unpacks to more than the image") || strstr(why, "can need")))
            longer++;
        free(ours);
        unsigned char *theirs = stb_png(png, n, &sw, &sh, &not_run);
        if (theirs) { stb_took_longer++; stbi_image_free(theirs); }
        free(png);
    }
    CHECK(longer == 4, "image data longer than the image: %d of 4 refused", longer);
    printf("png seeds: %d formats x layouts, %d decoded the same by both decoders; longer image data refused 4 of 4 "
           "(stb_image took %d)\n", seeds, agree, stb_took_longer);

    /* The mutation pass: 1 to 8 bytes after the signature set at random, every CRC made right. */
    uint32_t rs = 0x2365u;
    const int kRuns = 60000;
    int both = 0, only_ours = 0, only_stb = 0, neither = 0, differ = 0, bounds = 0, stb_not_run = 0;
    size_t biggest = 0;
    for (int k = 0; k < seeds; k++) if (seed_n[k] > biggest) biggest = seed_n[k];
    uint8_t *m = (uint8_t *)malloc(biggest);
    for (int r = 0; r < kRuns; r++) {
        const int k = r % seeds;
        const size_t n = seed_n[k];
        memcpy(m, seed[k], n);
        const int flips = 1 + (int)(t_rand(&rs) % 8u);
        for (int j = 0; j < flips; j++) m[8u + t_rand(&rs) % (n - 8u)] = (uint8_t)t_rand(&rs);
        for (size_t at = 8; at + 12u <= n;) {
            const uint32_t len = be32_at(m + at);
            if (len > n - at - 12u) break;
            png_fix_crc(m, at);
            at += (size_t)len + 12u;
        }
        uint32_t *ours = NULL;
        int w = 0, h = 0, sw = 0, sh = 0, not_run = 0;
        const int ok = nk_texrepl_decode_png(m, n, 0, &ours, &w, &h, why, sizeof(why));
        if (ok && (!ours || w < 1 || h < 1 || w > 2048 || h > 2048)) bounds++;
        unsigned char *theirs = stb_png(m, n, &sw, &sh, &not_run);
        if (not_run) {
            stb_not_run++;
        } else if (ok && theirs) {
            both++;
            if (w != sw || h != sh || memcmp(ours, theirs, (size_t)w * h * 4) != 0) differ++;
        } else if (ok) only_ours++;
        else if (theirs) only_stb++;
        else neither++;
        free(ours);
        if (theirs) stbi_image_free(theirs);
    }
    CHECK(differ == 0 && bounds == 0, "mutations: %d decoded differently, %d out of bounds", differ, bounds);
    CHECK(only_ours == 0, "mutations: %d decoded here that stb_image refused", only_ours);
    printf("png mutations: %d runs over %d seeds (seed 0x2365): %d decoded by both and the same, %d refused here only, "
           "%d refused by both, %d differ, %d with an empty IDAT not given to stb_image, no crash\n", kRuns, seeds,
           both, only_stb, neither, differ, stb_not_run);
    free(m);
    for (int k = 0; k < seeds; k++) free(seed[k]);
}

/* `d` with its PLTE (if any) dropped and one of `entries` colours put right after IHDR. */
static uint8_t *png_with_plte(const uint8_t *d, size_t n, unsigned entries, size_t *out_n) {
    Bw o = { 0 };
    zbuf(&o.d, &o.n, &o.cap, d, 8);
    for (size_t at = 8; at + 12u <= n;) {
        const uint32_t len = be32_at(d + at);
        if (memcmp(d + at + 4, "PLTE", 4) != 0) zbuf(&o.d, &o.n, &o.cap, d + at, (size_t)len + 12u);
        if (memcmp(d + at + 4, "IHDR", 4) == 0) {
            uint8_t pal[768];
            for (unsigned k = 0; k < 768u; k++) pal[k] = (uint8_t)(k * 37u + 11u);
            png_chunk_out(&o, "PLTE", pal, (size_t)entries * 3u);
        }
        at += (size_t)len + 12u;
    }
    *out_n = o.n;
    return o.d;
}

/* PNGs libpng takes though the standard does not allow them: a 4-bit palette image with 256 PLTE
 * entries, and a grey image with a PLTE. Both decode, the same as stb_image decodes them. */
static void test_png_plte_lenient(void) {
    static const PngSpec specs[2] = { { 9, 7, 4, 3, 0, 0, 1, 1, 0, 0x51u }, { 9, 7, 8, 0, 1, 0, 1, 1, 0, 0x52u } };
    for (int k = 0; k < 2; k++) {
        size_t n = 0, m = 0;
        uint8_t *base = png_build(&specs[k], &n);
        uint8_t *png = png_with_plte(base, n, 256u, &m);
        uint32_t *ours = NULL;
        int w = 0, h = 0, sw = 0, sh = 0, not_run = 0;
        char why[96] = "";
        const int ok = nk_texrepl_decode_png(png, m, 0, &ours, &w, &h, why, sizeof(why));
        unsigned char *theirs = stb_png(png, m, &sw, &sh, &not_run);
        CHECK(ok && theirs && w == 9 && h == 7 && sw == 9 && sh == 7 && !memcmp(ours, theirs, 9u * 7u * 4u),
              "%s with a 256-entry PLTE: ours %d (%s), stb %d", k ? "an 8-bit grey image" : "a 4-bit palette image", ok,
              why, theirs != NULL);
        free(ours);
        if (theirs) stbi_image_free(theirs);
        free(png);
        free(base);
    }
}

/* A PNG of the given rows (each with its filter byte), its image data one stored DEFLATE block with
 * the right Adler-32 (xor `adler_xor`), and the chunks `pre` (raw chunk bytes, CRCs included) between
 * IHDR and IDAT; with `split`, the image data in two IDAT chunks with `mid` between them. */
static uint8_t *png_raw(unsigned w, unsigned h, unsigned depth, unsigned color, const uint8_t *raw, size_t raw_n,
                        const Bw *pre, const Bw *mid, uint32_t adler_xor, size_t *out_n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
    const uint32_t adler = (b << 16 | a) ^ adler_xor;
    Bw z = { 0 };
    const uint8_t zh[2] = { 0x78, 0x01 };
    const uint8_t blk[5] = { 1, (uint8_t)raw_n, (uint8_t)(raw_n >> 8), (uint8_t)~raw_n, (uint8_t)(~raw_n >> 8) };
    const uint8_t ad[4] = { (uint8_t)(adler >> 24), (uint8_t)(adler >> 16), (uint8_t)(adler >> 8), (uint8_t)adler };
    zbuf(&z.d, &z.n, &z.cap, zh, 2);
    zbuf(&z.d, &z.n, &z.cap, blk, 5);
    zbuf(&z.d, &z.n, &z.cap, raw, raw_n);
    zbuf(&z.d, &z.n, &z.cap, ad, 4);
    Bw o = { 0 };
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    zbuf(&o.d, &o.n, &o.cap, sig, 8);
    const uint8_t ihdr[13] = { 0, 0, (uint8_t)(w >> 8), (uint8_t)w, 0, 0, (uint8_t)(h >> 8), (uint8_t)h, (uint8_t)depth,
                               (uint8_t)color, 0, 0, 0 };
    png_chunk_out(&o, "IHDR", ihdr, 13);
    if (pre && pre->n) zbuf(&o.d, &o.n, &o.cap, pre->d, pre->n);
    if (mid) {
        png_chunk_out(&o, "IDAT", z.d, z.n / 2);
        zbuf(&o.d, &o.n, &o.cap, mid->d, mid->n);
        png_chunk_out(&o, "IDAT", z.d + z.n / 2, z.n - z.n / 2);
    } else {
        png_chunk_out(&o, "IDAT", z.d, z.n);
    }
    png_chunk_out(&o, "IEND", NULL, 0);
    free(z.d);
    *out_n = o.n;
    return o.d;
}

/* The decoder's own checks, each met by a PNG whose CRCs and Adler-32 are right, so that check and
 * no other is what refuses it; and the same PNGs made good, which decode. */
static void test_png_checks_pinned(void) {
    static const uint8_t pal2[6] = { 10, 20, 30, 40, 50, 60 };
    static const uint8_t pal4b[4] = { 1, 2, 3, 4 };
    static const uint8_t trns4[4] = { 0, 1, 0, 2 };
    static const uint8_t text[5] = { 'k', 0, 'v', 'a', 'l' };
    uint8_t pal257[257 * 3];
    memset(pal257, 7, sizeof(pal257));
    Bw plte2 = { 0 }, plte_bad = { 0 }, plte_big = { 0 }, trns_bad = { 0 }, mid = { 0 };
    png_chunk_out(&plte2, "PLTE", pal2, 6);
    png_chunk_out(&plte_bad, "PLTE", pal4b, 4);
    png_chunk_out(&plte_big, "PLTE", pal257, sizeof(pal257));
    png_chunk_out(&trns_bad, "tRNS", trns4, 4);
    png_chunk_out(&mid, "tEXt", text, 5);
    Bw plte_twice = { 0 };
    png_chunk_out(&plte_twice, "PLTE", pal2, 6);
    png_chunk_out(&plte_twice, "PLTE", pal2, 6);
    const uint8_t idx_ok[5] = { 0, 0, 1, 1, 0 }, idx_bad[5] = { 0, 0, 1, 5, 0 };
    const uint8_t rgba_ok[5] = { 0, 1, 2, 3, 4 }, rgba_f5[5] = { 5, 1, 2, 3, 4 };
    const uint8_t rgb_ok[4] = { 0, 9, 8, 7 };
    const struct {
        const char *what;
        unsigned w, depth, color;
        const uint8_t *raw;
        size_t raw_n;
        const Bw *pre, *mid;
        uint32_t adler_xor;
        const char *want;   /* NULL: decodes */
    } cells[] = {
        { "palette indices in range", 4, 8, 3, idx_ok, 5, &plte2, NULL, 0, NULL },
        { "a palette index past PLTE", 4, 8, 3, idx_bad, 5, &plte2, NULL, 0, "names palette entry 5" },
        { "an RGBA row", 1, 8, 6, rgba_ok, 5, NULL, NULL, 0, NULL },
        { "filter type 5", 1, 8, 6, rgba_f5, 5, NULL, NULL, 0, "filter type 5" },
        { "a wrong Adler-32", 1, 8, 6, rgba_ok, 5, NULL, NULL, 1u, "Adler-32 is wrong" },
        { "a PLTE of 4 bytes", 4, 8, 3, idx_ok, 5, &plte_bad, NULL, 0, "PLTE is out of place or of a wrong length" },
        { "a PLTE of 257 entries", 4, 8, 3, idx_ok, 5, &plte_big, NULL, 0, "PLTE is out of place or of a wrong length" },
        { "an RGB tRNS of 4 bytes", 1, 8, 2, rgb_ok, 4, &trns_bad, NULL, 0, "tRNS is out of place or of a wrong length" },
        { "image data in two IDATs", 1, 8, 6, rgba_ok, 5, NULL, NULL, 0, NULL },
        { "a tEXt between two IDATs", 1, 8, 6, rgba_ok, 5, NULL, &mid, 0, "the IDAT chunks do not follow one another" },
        { "two PLTEs", 4, 8, 3, idx_ok, 5, &plte_twice, NULL, 0, "PLTE is out of place or of a wrong length" },
        { "two PLTEs in a grey image", 4, 8, 0, idx_ok, 5, &plte_twice, NULL, 0, "PLTE is out of place or of a wrong length" },
        { "one PLTE in a grey image", 4, 8, 0, idx_ok, 5, &plte2, NULL, 0, NULL },
    };
    for (size_t k = 0; k < sizeof(cells) / sizeof(cells[0]); k++) {
        size_t n = 0;
        const int two = k == 8;
        Bw none = { 0 };
        uint8_t *png = png_raw(cells[k].w, 1, cells[k].depth, cells[k].color, cells[k].raw, cells[k].raw_n, cells[k].pre,
                               two ? &none : cells[k].mid, cells[k].adler_xor, &n);
        uint32_t *px = NULL;
        int w = 0, h = 0;
        char why[96] = "";
        const int ok = nk_texrepl_decode_png(png, n, 0, &px, &w, &h, why, sizeof(why));
        if (cells[k].want)
            CHECK(!ok && strstr(why, cells[k].want), "%s: decoded %d, '%s'", cells[k].what, ok, why);
        else
            CHECK(ok && w == (int)cells[k].w && h == 1, "%s: decoded %d, '%s'", cells[k].what, ok, why);
        if (ok && k == 0)
            CHECK(px[0] == (uint32_t)(10u | 20u << 8 | 30u << 16 | 0xffu << 24) &&
                      px[1] == (uint32_t)(40u | 50u << 8 | 60u << 16 | 0xffu << 24),
                  "palette colours wrong");
        free(px);
        free(png);
    }
    free(plte2.d);
    free(plte_bad.d);
    free(plte_big.d);
    free(trns_bad.d);
    free(mid.d);
    free(plte_twice.d);
}

static void save_png_spec(const char *pack, const char *name, int w, int h, uint32_t seed) {
    const PngSpec sp = { w, h, 8, 6, 0, 0, 1, 1, 0, seed };
    size_t n = 0;
    uint8_t *png = png_build(&sp, &n);
    char path[600];
    pack_path(path, sizeof(path), pack, name);
    file_save(path, png, n);
    free(png);
}

static int lookup_unpin(NkTexRepl *p, NkTexReplId id, int level) {
    NkTexReplHit hit;
    if (!nk_texrepl_lookup_level(p, &id, level, &hit)) return 0;
    nk_texrepl_unpin(p, &hit);
    return 1;
}

/* The shared images' guards. A full budget: an image the budget cannot hold now is not read again
 * on each lookup while the held ones stay pinned. A mip level whose image is the wrong size is not
 * decoded, nor read again once its size is known. A chain whose level 0 image proved unusable
 * through another key serves no level above it. */
static void test_image_guards(void) {
    char dir[512], path[600], err[256] = "";
    make_pack(dir, sizeof(dir), "texrepl_img_budget");
    for (int k = 1; k <= 3; k++) {
        char name[24];
        snprintf(name, sizeof(name), "b%d.png", k);
        save_png_spec("texrepl_img_budget", name, 32, 32, 0x100u + (uint32_t)k);
    }
    pack_path(path, sizeof(path), "texrepl_img_budget", "textures.ini");
    write_text(path, "[options]\nhash = quick\n[hashes]\n0880c0000000050500000001 = b1.png\n"
                     "0880c0000000050500000002 = b2.png\n0880c0000000050500000003 = b3.png\n");
    NkTexRepl *p = nk_texrepl_open(dir, 2u * 32u * 32u * 4u, err, sizeof(err));
    CHECK(p != NULL, "the budget pack: %s", err);
    if (p) {
        NkTexReplHit h1, h2;
        NkTexReplId i1 = { 0x0880c000u, 3u, 0x0505u, 0u, 1u }, i2 = i1, i3 = i1;
        i2.hash = 2u;
        i3.hash = 3u;
        const int pinned = nk_texrepl_lookup(p, &i1, &h1) + nk_texrepl_lookup(p, &i2, &h2);
        int hits3 = 0;
        for (int k = 0; k < 11; k++) hits3 += lookup_unpin(p, i3, 0);
        NkTexReplStats st;
        nk_texrepl_stats(p, &st);
        CHECK(pinned == 2 && hits3 == 0 && st.reads == 3,
              "a full budget: %d pinned, %d hits on the third image, %llu reads (3: no read again while full)", pinned,
              hits3, (unsigned long long)st.reads);
        nk_texrepl_unpin(p, &h1);
        nk_texrepl_unpin(p, &h2);
        CHECK(lookup_unpin(p, i3, 0), "the third image once the others are unpinned");
        nk_texrepl_close(p);
    }

    /* F: r (4x4), then s (2x2). D: q (4x4), then r, the wrong size. G: t (4x4), then u (4x4), the wrong
     * size. A: x (2x1, its Adler-32 wrong), then y (1x1). B: x. */
    make_pack(dir, sizeof(dir), "texrepl_img_chain");
    save_png_spec("texrepl_img_chain", "q.png", 4, 4, 0x201u);
    save_png_spec("texrepl_img_chain", "r.png", 4, 4, 0x202u);
    save_png_spec("texrepl_img_chain", "s.png", 2, 2, 0x203u);
    save_png_spec("texrepl_img_chain", "t.png", 4, 4, 0x204u);
    save_png_spec("texrepl_img_chain", "u.png", 4, 4, 0x205u);
    save_png_spec("texrepl_img_chain", "y.png", 1, 1, 0x206u);
    const uint8_t row[9] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
    size_t xn = 0;
    uint8_t *x = png_raw(2, 1, 8, 6, row, sizeof(row), NULL, NULL, 1u, &xn);
    pack_path(path, sizeof(path), "texrepl_img_chain", "x.png");
    file_save(path, x, xn);
    free(x);
    pack_path(path, sizeof(path), "texrepl_img_chain", "textures.ini");
    write_text(path, "[options]\nhash = quick\n[hashes]\n"
                     "0880d000000002020000000f = r.png\n0880d000000002020000000f_1 = s.png\n"
                     "0880d000000002020000000d = q.png\n0880d000000002020000000d_1 = r.png\n"
                     "0880d000000002020000000e = t.png\n0880d000000002020000000e_1 = u.png\n"
                     "0880d000000002020000000a = x.png\n0880d000000002020000000a_1 = y.png\n"
                     "0880d000000002020000000b = x.png\n");
    err[0] = '\0';
    p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL, "the chain pack: %s", err);
    if (p) {
        const NkTexReplId f = { 0x0880d000u, 3u, 0x0202u, 0u, 0xfu }, d = { 0x0880d000u, 3u, 0x0202u, 0u, 0xdu },
                          g = { 0x0880d000u, 3u, 0x0202u, 0u, 0xeu }, a = { 0x0880d000u, 3u, 0x0202u, 0u, 0xau },
                          b = { 0x0880d000u, 3u, 0x0202u, 0u, 0xbu };
        NkTexReplStats s0, s1, s2;
        CHECK(lookup_unpin(p, f, 1), "F level 1 (s.png)");
        nk_texrepl_stats(p, &s0);   /* r's size is known now, its image not held */
        CHECK(!lookup_unpin(p, d, 1), "D level 1 is r.png, 4x4 where 2x2 is needed");
        nk_texrepl_stats(p, &s1);
        CHECK(s1.reads == s0.reads + 1 && s1.loaded == s0.loaded,
              "D level 1: %llu more reads (1: q only; r's size was known), %llu more decodes (0)",
              (unsigned long long)(s1.reads - s0.reads), (unsigned long long)(s1.loaded - s0.loaded));
        CHECK(!lookup_unpin(p, g, 1), "G level 1 is u.png, 4x4 where 2x2 is needed");
        nk_texrepl_stats(p, &s2);
        CHECK(s2.loaded == s1.loaded, "a mip level of the wrong size was decoded (%llu more)",
              (unsigned long long)(s2.loaded - s1.loaded));
        CHECK(lookup_unpin(p, a, 1), "A level 1 (y.png), x.png's size read");
        CHECK(!lookup_unpin(p, b, 0) && has_diag(p, "x.png: not a PNG this module decodes"), "B: x.png decodes");
        CHECK(!lookup_unpin(p, a, 1), "A level 1 served after its level 0 image (x.png) proved unusable");
        nk_texrepl_close(p);
    }
}

/* A 1x1 PNG whose image data unpacks to 64 MiB, listed under 81 keys that every texture of up to
 * 256 texels a side reaches by the "size only" wildcard: its 400 KiB of image data are more than a
 * 1x1 image can need, so each lookup refuses it before unpacking a byte, nothing is held, and the
 * most one decode held is the file. The same data under a 1024x1024 IHDR is within that bound and
 * is refused by the inflater's own limit, after unpacking the 4 MiB the image implies. */
static void test_png_bomb(void) {
    char dir[512], path[600], err[256] = "", why[96];
    size_t n = 0;
    uint8_t *png = png_bomb(64, 1, &n);
    double t0 = wall_s();
    uint32_t *rgba = NULL;
    int w = 0, h = 0;
    CHECK(!nk_texrepl_decode_png(png, n, 0, &rgba, &w, &h, why, sizeof(why)) && !rgba && strstr(why, "more than 5 can need"),
          "a 64 MiB image data bomb under a 1x1 IHDR: '%s'", why);
    const double t_one = wall_s() - t0;
    {
        size_t n2 = 0;
        uint8_t *big = png_bomb(64, 1024, &n2);
        const double t1 = wall_s();
        CHECK(!nk_texrepl_decode_png(big, n2, 0, &rgba, &w, &h, why, sizeof(why)) && !rgba &&
                  strstr(why, "unpacks to more than the image") && wall_s() - t1 < 5.0,
              "a 64 MiB image data bomb under a 1024x1024 IHDR: '%s' in %.2f s", why, wall_s() - t1);
        free(big);
    }
    make_pack(dir, sizeof(dir), "texrepl_png_bomb");
    pack_path(path, sizeof(path), "texrepl_png_bomb", "bomb.png");
    file_save(path, png, n);
    char ini[4096];
    size_t at = (size_t)snprintf(ini, sizeof(ini), "[options]\nhash = quick\n[hashes]\n");
    for (unsigned hh = 1; hh <= 9; hh++)
        for (unsigned ww = 1; ww <= 9; ww++)
            at += (size_t)snprintf(ini + at, sizeof(ini) - at, "%016llx = bomb.png\n", (unsigned long long)(hh << 8 | ww));
    pack_path(path, sizeof(path), "texrepl_png_bomb", "textures.ini");
    write_text(path, ini);
    NkTexRepl *p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL, "the bomb pack: %s", err);
    if (p) {
        t0 = wall_s();
        int hits = 0;
        for (unsigned hh = 1; hh <= 9; hh++) {
            for (unsigned ww = 1; ww <= 9; ww++) {
                NkTexReplHit hit;
                NkTexReplId id = { 0x08804000u, 3u, hh << 8 | ww, 0u, 0x12345678u + hh * 16u + ww };
                if (nk_texrepl_lookup(p, &id, &hit)) { hits++; nk_texrepl_unpin(p, &hit); }
            }
        }
        const double t_all = wall_s() - t0;
        NkTexReplStats st;
        nk_texrepl_stats(p, &st);
        CHECK(hits == 0 && st.skipped == 81 && st.cache_peak == 0, "bomb pack: hits %d skipped %llu cache_peak %llu", hits,
              (unsigned long long)st.skipped, (unsigned long long)st.cache_peak);
        CHECK(st.decode_peak <= (uint64_t)n + 4096u, "one decode held %llu bytes for a %zu-byte file",
              (unsigned long long)st.decode_peak, n);
        CHECK(st.reads == 1 && st.load_failed == 1, "the bomb named by 81 keys was read %llu times, refused %llu times",
              (unsigned long long)st.reads, (unsigned long long)st.load_failed);
        CHECK(has_diag(p, "bomb.png: not a PNG this module decodes (its image data is"), "the bomb not noted");
        CHECK(t_all < 10.0, "81 bomb lookups took %.2f s", t_all);
        printf("png bomb: %zu-byte 1x1 PNG unpacking to 64 MiB refused unread in %.1f ms; 81 wildcard lookups %.1f ms, "
               "most held by one decode %llu bytes, cache peak %llu\n",
               n, t_one * 1e3, t_all * 1e3, (unsigned long long)st.decode_peak, (unsigned long long)st.cache_peak);
        nk_texrepl_close(p);
    }
    free(png);

    /* A good image under the same 81 keys: read and decoded once, held once, every lookup a hit. */
    const PngSpec sp = { 32, 32, 8, 6, 0, 0, 1, 1, 0, 0x2365u };
    png = png_build(&sp, &n);
    make_pack(dir, sizeof(dir), "texrepl_png_shared");
    pack_path(path, sizeof(path), "texrepl_png_shared", "good.png");
    file_save(path, png, n);
    at = (size_t)snprintf(ini, sizeof(ini), "[options]\nhash = quick\n[hashes]\n");
    for (unsigned hh = 1; hh <= 9; hh++)
        for (unsigned ww = 1; ww <= 9; ww++)
            at += (size_t)snprintf(ini + at, sizeof(ini) - at, "%016llx = good.png\n", (unsigned long long)(hh << 8 | ww));
    pack_path(path, sizeof(path), "texrepl_png_shared", "textures.ini");
    write_text(path, ini);
    err[0] = '\0';
    p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL, "the shared-image pack: %s", err);
    if (p) {
        int hits = 0;
        const uint32_t *first = NULL;
        int same = 1;
        for (unsigned hh = 1; hh <= 9; hh++) {
            for (unsigned ww = 1; ww <= 9; ww++) {
                NkTexReplHit hit;
                NkTexReplId id = { 0x08804000u, 3u, hh << 8 | ww, 0u, 0x12345678u + hh * 16u + ww };
                if (nk_texrepl_lookup(p, &id, &hit)) {
                    hits++;
                    if (!first) first = hit.rgba;
                    if (hit.rgba != first || hit.w != 32 || hit.h != 32) same = 0;
                    nk_texrepl_unpin(p, &hit);
                }
            }
        }
        NkTexReplStats st;
        nk_texrepl_stats(p, &st);
        CHECK(hits == 81 && same, "the shared image: %d hits, one image %d", hits, same);
        CHECK(st.reads == 1 && st.loaded == 1 && st.cache_peak == 32u * 32u * 4u,
              "one file under 81 keys: read %llu times, decoded %llu times, %llu bytes held",
              (unsigned long long)st.reads, (unsigned long long)st.loaded, (unsigned long long)st.cache_peak);
        nk_texrepl_close(p);
    }

    /* The same file under 81 names that reach it: in a ZIP, spellings that differ in case (its lookups
     * ignore case); in a folder, the same on Windows, and links to it on POSIX. Read and decoded once. */
    char names[81][16];
    for (unsigned k = 0; k < 81; k++) {
        static const char base[] = "good.png";
        for (unsigned c = 0, bit = 0; c < sizeof(base); c++) {
            const char ch = base[c];
            names[k][c] = (ch >= 'a' && ch <= 'z' && ((k >> bit++) & 1u)) ? (char)(ch - 32) : ch;
        }
    }
    for (int as_zip = 1; as_zip >= 0; as_zip--) {   /* the ZIP first: on POSIX the folder renames to links */
        at = (size_t)snprintf(ini, sizeof(ini), "[options]\nhash = quick\n[hashes]\n");
        for (unsigned k = 0; k < 81; k++) {
#if !defined(_WIN32)
            if (!as_zip) snprintf(names[k], sizeof(names[k]), "l%02u.png", k);
#endif
            at += (size_t)snprintf(ini + at, sizeof(ini) - at, "%016llx = %s\n",
                                   (unsigned long long)((k / 9u + 1u) << 8 | (k % 9u + 1u)), names[k]);
        }
        char where[600];
        if (as_zip) {
            Zw z;
            memset(&z, 0, sizeof(z));
            const ZwOpt stored = { 0, 0, NULL, 0, 0, 0 };
            zw_add(&z, "good.png", png, n, stored);
            zw_add_text(&z, "textures.ini", ini, 0);
            snprintf(where, sizeof(where), "%s/texrepl_png_spellings.zip", SR_SELFTEST_BUILD_ROOT);
            zw_finish(&z, where, 0);
        } else {
            make_pack(where, sizeof(where), "texrepl_png_names");
            pack_path(path, sizeof(path), "texrepl_png_names", "good.png");
            file_save(path, png, n);
#if !defined(_WIN32)
            for (unsigned k = 0; k < 81; k++) {
                pack_path(path, sizeof(path), "texrepl_png_names", names[k]);
                remove(path);
                CHECK(symlink("good.png", path) == 0, "cannot link %s", path);
            }
#endif
            pack_path(path, sizeof(path), "texrepl_png_names", "textures.ini");
            write_text(path, ini);
        }
        err[0] = '\0';
        p = nk_texrepl_open(where, 0, err, sizeof(err));
        CHECK(p != NULL, "the %s naming one file 81 ways: %s", as_zip ? "ZIP" : "folder", err);
        if (!p) continue;
        int hits = 0;
        for (unsigned k = 0; k < 81; k++) {
            NkTexReplHit hit;
            NkTexReplId id = { 0x08804000u, 3u, (k / 9u + 1u) << 8 | (k % 9u + 1u), 0u, 0x2365u + k };
            if (nk_texrepl_lookup(p, &id, &hit)) { hits++; nk_texrepl_unpin(p, &hit); }
        }
        NkTexReplStats st;
        nk_texrepl_stats(p, &st);
        CHECK(hits == 81 && st.reads == 1 && st.loaded == 1 && st.cache_peak == 32u * 32u * 4u,
              "%s, one file by 81 names: %d hits, read %llu times, decoded %llu times, %llu bytes held",
              as_zip ? "ZIP" : "folder", hits, (unsigned long long)st.reads, (unsigned long long)st.loaded,
              (unsigned long long)st.cache_peak);
        nk_texrepl_close(p);
    }

    /* A damaged ZIP entry (its CRC wrong) under 81 spellings: unpacked and refused once, not once a
     * spelling. */
    {
        at = (size_t)snprintf(ini, sizeof(ini), "[options]\nhash = quick\n[hashes]\n");
        for (unsigned k = 0; k < 81; k++) {
            static const char base[] = "damaged.png";
            char bad[sizeof(base)];
            for (unsigned c = 0, bit = 0; c < sizeof(base); c++) {
                const char ch = base[c];
                bad[c] = (ch >= 'a' && ch <= 'z' && ((k >> bit++) & 1u)) ? (char)(ch - 32) : ch;
            }
            at +=(size_t)snprintf(ini + at, sizeof(ini) - at, "%016llx = %s\n",
                                   (unsigned long long)((k / 9u + 1u) << 8 | (k % 9u + 1u)), bad);
        }
        Zw z;
        memset(&z, 0, sizeof(z));
        const ZwOpt damaged = { 8, 0, NULL, 0, 1, 0 };
        zw_add(&z, "damaged.png", png, n, damaged);
        zw_add_text(&z, "textures.ini", ini, 0);
        char where[600];
        snprintf(where, sizeof(where), "%s/texrepl_png_damaged_spellings.zip", SR_SELFTEST_BUILD_ROOT);
        zw_finish(&z, where, 0);
        err[0] = '\0';
        p = nk_texrepl_open(where, 0, err, sizeof(err));
        CHECK(p != NULL, "the ZIP naming one damaged entry 81 ways: %s", err);
        if (p) {
            int hits = 0;
            for (unsigned k = 0; k < 81; k++) {
                NkTexReplHit hit;
                NkTexReplId id = { 0x08804000u, 3u, (k / 9u + 1u) << 8 | (k % 9u + 1u), 0u, 0x2365u + k };
                if (nk_texrepl_lookup(p, &id, &hit)) { hits++; nk_texrepl_unpin(p, &hit); }
            }
            NkTexReplStats st;
            nk_texrepl_stats(p, &st);
            CHECK(hits == 0 && st.reads == 1 && st.load_failed == 1 && has_diag(p, "damaged in the ZIP"),
                  "one damaged entry by 81 names: %d hits, unpacked %llu times, %llu noted", hits,
                  (unsigned long long)st.reads, (unsigned long long)st.load_failed);
            nk_texrepl_close(p);
        }
    }
    free(png);
}

/* An empty DEFLATE block: no output, only work. The fixed kind is 10 bits; the dynamic kind is 60
 * (257 literal lengths and one distance length, through five code-length codes). */
static void empty_fixed_block(Bw *b) {
    bw_bits(b, 0, 1);
    bw_bits(b, 1, 2);
    bw_code(b, 0, 7);   /* end of block */
}
static void empty_dynamic_block(Bw *b) {
    bw_bits(b, 0, 1);
    bw_bits(b, 2, 2);
    bw_bits(b, 0, 5);   /* 257 literal/length codes */
    bw_bits(b, 0, 5);   /* 1 distance code */
    bw_bits(b, 1, 4);   /* 5 code-length codes: 16 17 18 0 8 */
    static const uint32_t cl[5] = { 0, 0, 1, 2, 2 };
    for (int i = 0; i < 5; i++) bw_bits(b, cl[i], 3);
    bw_code(b, 0, 1);   /* 18: 138 zeros */
    bw_bits(b, 127, 7);
    bw_code(b, 0, 1);   /* 18: 118 zeros */
    bw_bits(b, 107, 7);
    bw_code(b, 3, 2);   /* 8 for symbol 256 */
    bw_code(b, 2, 2);   /* 0 for distance 0 */
    bw_code(b, 0, 8);   /* end of block */
}

/* A zlib stream of about `bytes` of empty blocks, then `raw` in a final stored block and its
 * Adler-32. */
static Bw empty_blocks(size_t bytes, int dynamic, const uint8_t *raw, size_t raw_n) {
    Bw z = { 0 };
    bw_byte(&z, 0x78);
    bw_byte(&z, 0x01);
    while (z.n + 16 < bytes) {
        if (dynamic) empty_dynamic_block(&z);
        else empty_fixed_block(&z);
    }
    bw_bits(&z, 1, 1);
    bw_bits(&z, 0, 2);
    bw_flush(&z);
    const uint8_t hdr[4] = { (uint8_t)raw_n, (uint8_t)(raw_n >> 8), (uint8_t)~raw_n, (uint8_t)(~raw_n >> 8) };
    zbuf(&z.d, &z.n, &z.cap, hdr, 4);
    zbuf(&z.d, &z.n, &z.cap, raw, raw_n);
    uint32_t a = 1, s2 = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521u; s2 = (s2 + a) % 65521u; }
    const uint32_t ad = s2 << 16 | a;
    const uint8_t t[4] = { (uint8_t)(ad >> 24), (uint8_t)(ad >> 16), (uint8_t)(ad >> 8), (uint8_t)ad };
    zbuf(&z.d, &z.n, &z.cap, t, 4);
    return z;
}

/* A PNG: IHDR, the chunks in `pre` (raw chunk bytes), one IDAT of `z`, IEND. */
static uint8_t *png_with_idat(unsigned w, unsigned h, unsigned depth, unsigned color, const Bw *pre, const uint8_t *z,
                              size_t zn, size_t *out_n) {
    Bw o = { 0 };
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    zbuf(&o.d, &o.n, &o.cap, sig, 8);
    const uint8_t ihdr[13] = { 0, 0, (uint8_t)(w >> 8), (uint8_t)w, 0, 0, (uint8_t)(h >> 8), (uint8_t)h,
                               (uint8_t)depth, (uint8_t)color, 0, 0, 0 };
    png_chunk_out(&o, "IHDR", ihdr, 13);
    if (pre && pre->n) zbuf(&o.d, &o.n, &o.cap, pre->d, pre->n);
    png_chunk_out(&o, "IDAT", z, zn);
    png_chunk_out(&o, "IEND", NULL, 0);
    *out_n = o.n;
    return o.d;
}

/* The work one PNG may cost, from the Fable review of 8bbc2987dc23 (B-1): before, 64 MiB of empty
 * blocks in a 1x1 image's data, which a ZIP packs into 65 KiB, held the decoder for tens of seconds,
 * and 5.6 million empty chunks for seconds. Now image data past what DEFLATE can need for the image
 * is refused before it is unpacked, the inflater builds the fixed codes once a stream, and a chunk's
 * CRC costs its bytes and no table. */
/* A raw DEFLATE stream of `count` empty dynamic blocks, then a final stored block of `raw`. */
static Bw dynamic_run(unsigned count, const uint8_t *raw, size_t raw_n) {
    Bw z = { 0 };
    for (unsigned i = 0; i < count; i++) empty_dynamic_block(&z);
    bw_bits(&z, 1, 1);
    bw_bits(&z, 0, 2);
    bw_flush(&z);
    const uint8_t hdr[4] = { (uint8_t)raw_n, (uint8_t)(raw_n >> 8), (uint8_t)~raw_n, (uint8_t)(~raw_n >> 8) };
    zbuf(&z.d, &z.n, &z.cap, hdr, 4);
    zbuf(&z.d, &z.n, &z.cap, raw, raw_n);
    return z;
}

/* `n` bytes of `data` as one dynamic-Huffman block of literals only, the way zlib writes a block of
 * text it found few repeats in: literals 0 to 254 have 8-bit codes, literal 255 and the end of block
 * 9-bit ones, and no distance code is used. The 258 code lengths go through a code-length code of
 * three symbols: 8 (one bit), 0 and 9 (two bits each). */
static void literal_dynamic_block(Bw *b, const uint8_t *data, size_t n, int final) {
    bw_bits(b, final ? 1u : 0u, 1);
    bw_bits(b, 2, 2);
    bw_bits(b, 0, 5);   /* 257 literal/length codes */
    bw_bits(b, 0, 5);   /* 1 distance code */
    bw_bits(b, 3, 4);   /* 7 code-length codes: 16 17 18 0 8 7 9 */
    static const uint32_t cl[7] = { 0, 0, 0, 2, 1, 0, 2 };
    for (int i = 0; i < 7; i++) bw_bits(b, cl[i], 3);
    for (int i = 0; i < 255; i++) bw_code(b, 0, 1);   /* 8 for literals 0 to 254 */
    bw_code(b, 3, 2);   /* 9 for literal 255 */
    bw_code(b, 3, 2);   /* 9 for the end of block */
    bw_code(b, 2, 2);   /* 0 for distance 0 */
    for (size_t i = 0; i < n; i++) {
        if (data[i] < 255u) bw_code(b, data[i], 8);
        else bw_code(b, 510u, 9);
    }
    bw_code(b, 511u, 9);   /* end of block */
}

/* `n` bytes in literal dynamic blocks of `piece` bytes, each followed, when `flush`, by the empty
 * stored block a zlib sync flush writes; then an empty final stored block. */
static Bw literal_pieces(const uint8_t *data, size_t n, size_t piece, int flush) {
    Bw z = { 0 };
    for (size_t at = 0; at < n; at += piece) {
        literal_dynamic_block(&z, data + at, n - at < piece ? n - at : piece, 0);
        if (flush) {
            bw_bits(&z, 0, 1);
            bw_bits(&z, 0, 2);
            bw_flush(&z);
            const uint8_t none[4] = { 0x00, 0x00, 0xff, 0xff };
            zbuf(&z.d, &z.n, &z.cap, none, 4);
        }
    }
    bw_bits(&z, 1, 1);
    bw_bits(&z, 0, 2);
    bw_flush(&z);
    const uint8_t none[4] = { 0x00, 0x00, 0xff, 0xff };
    zbuf(&z.d, &z.n, &z.cap, none, 4);
    return z;
}

/* The cold read of 7ff4d3e849f7 (M-3): an empty dynamic block costs three code tables and writes
 * nothing, so the largest image's bounded data, all such blocks, held the decoder for 3.1 to 3.4 s,
 * and a pack of a few hundred such images for minutes. A stream may now have 64 dynamic blocks and
 * one more for every 64 bytes it has written. The Fable review of a7dd986554a0 (S-1) measured one
 * more for every KiB refusing valid zlib output: at memLevel 1 or 2 its blocks hold 127 or 255
 * symbols, and a writer that flushes every 513 bytes ends a block each time; 64 is half zlib's
 * smallest block (the coordinator's decision on the cold read's n-2). */
static void test_inflate_dynamic_budget(void) {
    static const uint8_t two[2] = { 0, 0 };
    uint8_t into[4096];
    size_t on = 0, used = 0;
    char err[96] = "";
    Bw z = dynamic_run(NK_INFLATE_DYNAMIC_FREE, two, 2);
    nk_inflate_table_builds = 0;
    int rc = nk_psp_inflate_raw_into(z.d, z.n, into, sizeof(into), &on, &used, err, sizeof(err));
    CHECK(rc == NK_INFLATE_OK && on == 2 && nk_inflate_table_builds == 3u * NK_INFLATE_DYNAMIC_FREE,
          "%u empty dynamic blocks: rc %d, %zu bytes, %llu tables, '%s'", NK_INFLATE_DYNAMIC_FREE, rc, on,
          nk_inflate_table_builds, err);
    free(z.d);
    z = dynamic_run(NK_INFLATE_DYNAMIC_FREE + 1u, two, 2);
    nk_inflate_table_builds = 0;
    rc = nk_psp_inflate_raw_into(z.d, z.n, into, sizeof(into), &on, &used, err, sizeof(err));
    CHECK(rc == NK_INFLATE_ERR_FORMAT && strstr(err, "more dynamic blocks") &&
              nk_inflate_table_builds == 3u * NK_INFLATE_DYNAMIC_FREE,
          "%u empty dynamic blocks: rc %d, %llu tables, '%s'", NK_INFLATE_DYNAMIC_FREE + 1u, rc,
          nk_inflate_table_builds, err);
    uint8_t *out = NULL;
    rc = nk_psp_inflate_raw(z.d, z.n, &out, &on, 2, 2, err, sizeof(err));
    CHECK(rc == NK_INFLATE_ERR_FORMAT && out == NULL, "the same unpacked to a new buffer: rc %d", rc);
    free(z.d);
    /* Output earns blocks, at the rate and no faster: 2 KiB written in a stored block earns 32 more
     * empty dynamic blocks, not 33. */
    for (unsigned extra = 0; extra <= 1u; extra++) {
        Bw w = { 0 };
        static uint8_t kib2[2048];
        const unsigned count = NK_INFLATE_DYNAMIC_FREE + 32u + extra;   /* 2,048 / 64 */
        bw_bits(&w, 0, 1);
        bw_bits(&w, 0, 2);
        bw_flush(&w);
        const uint8_t hdr[4] = { 0x00, 0x08, 0xff, 0xf7 };
        zbuf(&w.d, &w.n, &w.cap, hdr, 4);
        zbuf(&w.d, &w.n, &w.cap, kib2, sizeof(kib2));
        for (unsigned i = 0; i < count; i++) empty_dynamic_block(&w);
        bw_bits(&w, 1, 1);
        bw_bits(&w, 0, 2);
        bw_flush(&w);
        const uint8_t none[4] = { 0x00, 0x00, 0xff, 0xff };
        zbuf(&w.d, &w.n, &w.cap, none, 4);
        rc = nk_psp_inflate_raw_into(w.d, w.n, into, sizeof(into), &on, &used, err, sizeof(err));
        if (extra) {
            CHECK(rc == NK_INFLATE_ERR_FORMAT && strstr(err, "more dynamic blocks"),
                  "2 KiB, then %u empty dynamic blocks: rc %d, '%s'", count, rc, err);
        } else {
            CHECK(rc == NK_INFLATE_OK && on == sizeof(kib2), "2 KiB, then %u empty dynamic blocks: rc %d, %zu bytes, '%s'",
                  count, rc, on, err);
        }
        free(w.d);
    }
    /* Valid streams of small dynamic blocks unpack whole: 400 KiB in blocks of 300 literals (zlib at
     * memLevel 1 wrote 367 to 456 bytes a block on average, measured), and in blocks of 513 each
     * followed by a sync flush's empty stored block. One more block a KiB refused both. */
    {
        const size_t n = (size_t)400 << 10;
        uint8_t *text = (uint8_t *)malloc(n);
        uint8_t *back = (uint8_t *)malloc(n);
        uint32_t x = 1880u;
        for (size_t i = 0; i < n; i++) {
            x = x * 1103515245u + 12345u;
            text[i] = (uint8_t)(x >> 23);
        }
        for (int shape = 0; shape <= 1; shape++) {
            const size_t piece = shape ? 513u : 300u;
            Bw z = literal_pieces(text, n, piece, shape);
            on = 0;
            rc = nk_psp_inflate_raw_into(z.d, z.n, back, n, &on, &used, err, sizeof(err));
            CHECK(rc == NK_INFLATE_OK && on == n && memcmp(back, text, n) == 0 && used == z.n,
                  "400 KiB in dynamic blocks of %zu bytes%s: rc %d, %zu bytes, '%s'", piece,
                  shape ? ", each flushed" : "", rc, on, err);
            free(z.d);
        }
        free(back);
        free(text);
    }
}

static void test_png_work(void) {
    char why[128];
    static const uint8_t raw1[2] = { 0, 0 };   /* a 1x1 grey image: its row's filter byte and sample */
    for (int dyn = 0; dyn <= 1; dyn++) {
        Bw z = empty_blocks((size_t)64 << 20, dyn, raw1, 2);
        size_t n = 0;
        uint8_t *png = png_with_idat(1, 1, 8, 0, NULL, z.d, z.n, &n);
        free(z.d);
        uint32_t *px = NULL;
        int w = 0, h = 0;
        const double t0 = wall_s();
        const int ok = nk_texrepl_decode_png(png, n, 0, &px, &w, &h, why, sizeof(why));
        const double t = wall_s() - t0;
        CHECK(!ok && strstr(why, "more than 2 can need") && t < 2.0, "64 MiB of empty %s blocks in a 1x1 PNG: %d in %.2f s, '%s'",
              dyn ? "dynamic" : "fixed", ok, t, why);
        printf("png work: 64 MiB of empty %s blocks in a 1x1 PNG refused in %.1f ms\n", dyn ? "dynamic" : "fixed", t * 1e3);
        free(px);
        free(png);
    }
    {   /* 5.6 million empty ancillary chunks before a good 1x1 image's data */
        Bw one = { 0 }, pre = { 0 };
        png_chunk_out(&one, "aaaa", NULL, 0);
        const size_t count = ((size_t)64 << 20) / 12u - 8u;
        pre.cap = count * 12u;
        pre.d = (uint8_t *)malloc(pre.cap);
        for (size_t i = 0; i < count; i++) memcpy(pre.d + i * 12u, one.d, 12u);
        pre.n = pre.cap;
        Bw z = empty_blocks(0, 0, raw1, 2);
        size_t n = 0;
        uint8_t *png = png_with_idat(1, 1, 8, 0, &pre, z.d, z.n, &n);
        uint32_t *px = NULL;
        int w = 0, h = 0;
        const double t0 = wall_s();
        const int ok = nk_texrepl_decode_png(png, n, 0, &px, &w, &h, why, sizeof(why));
        const double t = wall_s() - t0;
        CHECK(ok && w == 1 && h == 1 && t < 2.0, "%zu empty chunks before a 1x1 image: %d in %.2f s, '%s'", count, ok, t, why);
        printf("png work: %zu empty chunks (%.0f MiB) checked in %.1f ms\n", count, n / 1048576.0, t * 1e3);
        free(px);
        free(png);
        free(z.d);
        free(pre.d);
        free(one.d);
    }
    {   /* the fixed codes are built once a stream: 1 MiB of empty fixed blocks is about 840,000 */
        Bw z = empty_blocks((size_t)1 << 20, 0, raw1, 2);
        uint8_t *out = NULL, into[2];
        size_t on = 0, used = 0;
        nk_inflate_table_builds = 0;
        int rc = nk_psp_inflate_raw(z.d + 2, z.n - 6, &out, &on, 2, 2, NULL, 0);
        const unsigned long long b1 = nk_inflate_table_builds;
        CHECK(rc == NK_INFLATE_OK && on == 2 && b1 == 2, "1 MiB of empty fixed blocks: rc %d, %zu bytes, %llu tables built",
              rc, on, b1);
        free(out);
        nk_inflate_table_builds = 0;
        rc = nk_psp_inflate_raw_into(z.d + 2, z.n - 2, into, sizeof(into), &on, &used, NULL, 0);
        CHECK(rc == NK_INFLATE_OK && on == 2 && used == z.n - 6 && nk_inflate_table_builds == 2,
              "the same into a buffer: rc %d, %zu bytes, %zu of %zu used, %llu tables", rc, on, used, z.n - 6,
              nk_inflate_table_builds);
        rc = nk_psp_inflate_raw_into(z.d + 2, z.n - 6, into, 1, &on, NULL, NULL, 0);
        CHECK(rc == NK_INFLATE_ERR_OVERFLOW && on == 1, "into a buffer one byte short: rc %d, %zu bytes", rc, on);
        free(z.d);
    }
    {   /* the Adler-32 must follow the stream: bytes between are refused, not skipped */
        Bw z = empty_blocks(0, 0, raw1, 2);
        Bw g = { 0 };
        zbuf(&g.d, &g.n, &g.cap, z.d, z.n - 4);
        const uint8_t junk[3] = { 1, 2, 3 };
        zbuf(&g.d, &g.n, &g.cap, junk, 3);
        zbuf(&g.d, &g.n, &g.cap, z.d + z.n - 4, 4);
        size_t n = 0;
        uint8_t *png = png_with_idat(1, 1, 8, 0, NULL, g.d, g.n, &n);
        uint32_t *px = NULL;
        int w = 0, h = 0;
        CHECK(!nk_texrepl_decode_png(png, n, 0, &px, &w, &h, why, sizeof(why)) && strstr(why, "bytes after its Adler-32"),
              "3 bytes between the stream and its Adler-32: '%s'", why);
        free(png);
        png = png_with_idat(1, 1, 8, 0, NULL, z.d, z.n, &n);
        CHECK(nk_texrepl_decode_png(png, n, 0, &px, &w, &h, why, sizeof(why)) && w == 1, "the same PNG made right: '%s'", why);
        free(px);
        free(png);
        free(g.d);
        free(z.d);
    }
    {   /* within the bound, the most work an image can ask for: the largest image's data, 9/8 of what
         * it unpacks to, all empty dynamic blocks. Measured, and bounded by a few times an honest
         * decode of that size. */
        const size_t raw_n = (1u + 2048u * 8u) * 2048u;
        Bw z = empty_blocks(raw_n + raw_n / 8u + 64u - 32u, 1, raw1, 0);
        size_t n = 0;
        uint8_t *png = png_with_idat(2048, 2048, 16, 6, NULL, z.d, z.n, &n);
        free(z.d);
        uint32_t *px = NULL;
        int w = 0, h = 0;
        const double t0 = wall_s();
        const int ok = nk_texrepl_decode_png(png, n, 0, &px, &w, &h, why, sizeof(why));
        const double t = wall_s() - t0;
        CHECK(!ok && strstr(why, "does not unpack (more dynamic blocks than its output needs)") && t < NK_PNG_WORK_SECONDS,
              "%.1f MiB of empty dynamic blocks in a 2048x2048 RGBA16 PNG: %d in %.2f s, '%s'", n / 1048576.0, ok, t, why);
        printf("png work: the largest image's data, %.1f MiB of empty dynamic blocks, refused in %.0f ms\n", n / 1048576.0,
               t * 1e3);
        free(px);
        free(png);
    }
    {   /* the most dynamic blocks the budget lets the largest image have: the 64 free ones first,
         * then after every 64 bytes it writes (a fixed block of matches) one more, 524,384 in all;
         * then the image decodes. Measured, and bounded like the cell above. */
        const size_t raw_n = (1u + 2048u * 8u) * 2048u;
        Bw z = { 0 };
        bw_byte(&z, 0x78);
        bw_byte(&z, 0x01);
        for (unsigned i = 0; i < NK_INFLATE_DYNAMIC_FREE; i++) empty_dynamic_block(&z);
        for (size_t at = 0; at < raw_n; at += NK_INFLATE_DYNAMIC_BYTES) {
            unsigned left = NK_INFLATE_DYNAMIC_BYTES;
            bw_bits(&z, 0, 1);
            bw_bits(&z, 1, 2);
            if (at == 0) {
                fixed_sym(&z, 0);
                left--;
            }
            while (left >= 13u) {
                fixed_match(&z, 10);
                left -= 10u;
            }
            if (left > 10u) {
                fixed_match(&z, left - 5u);
                left = 5u;
            }
            fixed_match(&z, left);
            bw_code(&z, 0, 7);   /* end of block */
            empty_dynamic_block(&z);
        }
        bw_bits(&z, 1, 1);
        bw_bits(&z, 0, 2);
        bw_flush(&z);
        const uint8_t none[4] = { 0x00, 0x00, 0xff, 0xff };
        zbuf(&z.d, &z.n, &z.cap, none, 4);
        const uint32_t ad = (uint32_t)(raw_n % 65521u) << 16 | 1u;   /* Adler-32 of zeros */
        const uint8_t t4[4] = { (uint8_t)(ad >> 24), (uint8_t)(ad >> 16), (uint8_t)(ad >> 8), (uint8_t)ad };
        zbuf(&z.d, &z.n, &z.cap, t4, 4);
        size_t n = 0;
        uint8_t *png = png_with_idat(2048, 2048, 16, 6, NULL, z.d, z.n, &n);
        free(z.d);
        uint32_t *px = NULL;
        int w = 0, h = 0;
        nk_inflate_table_builds = 0;
        const double t0 = wall_s();
        const int ok = nk_texrepl_decode_png(png, n, 0, &px, &w, &h, why, sizeof(why));
        const double t = wall_s() - t0;
        const size_t blocks = NK_INFLATE_DYNAMIC_FREE + raw_n / NK_INFLATE_DYNAMIC_BYTES;
        CHECK(ok && w == 2048 && h == 2048 && nk_inflate_table_builds == 3u * blocks + 2u && t < NK_PNG_ALLOWED_SECONDS,
              "the largest image with %zu empty dynamic blocks, 64 then one after every 64 bytes: %d in %.2f s, %llu tables, '%s'", blocks,
              ok, t, nk_inflate_table_builds, why);
        printf("png work: the largest image with 64 empty dynamic blocks, then one after every %u bytes (%zu) decoded in %.0f ms\n",
               NK_INFLATE_DYNAMIC_BYTES, blocks, t * 1e3);
        free(px);
        free(png);
    }
}

/* The cache under churn (the Fable review's S-1 and S-2). Two images taking turns in a budget that
 * holds one: before, each of 2,000 lookups decoded its image again; now an image evicted and wanted
 * again is decoded after twice as many lookups each time, so 1,000 lookups cost it 10 decodes. Every
 * lookup is either a hit (a decode, or the image still held) or deferred.
 * Eviction takes the least recently used image from a list: 40,000 held 1x1 images all make room
 * for one at once, where a scan of every image for each would take 1.6 billion steps. A DEFLATE ZIP
 * entry's read counts its packed bytes beside the file in decode_peak. */
static void test_cache_churn(void) {
    char dir[512], path[600], err[256] = "";
    make_pack(dir, sizeof(dir), "texrepl_churn");
    save_png_spec("texrepl_churn", "a.png", 32, 32, 0x301u);
    save_png_spec("texrepl_churn", "b.png", 32, 32, 0x302u);
    pack_path(path, sizeof(path), "texrepl_churn", "textures.ini");
    write_text(path, "[options]\nhash = quick\n[hashes]\n0880f00000000505000000a1 = a.png\n"
                     "0880f00000000505000000b1 = b.png\n");
    NkTexRepl *p = nk_texrepl_open(dir, 32u * 32u * 4u, err, sizeof(err));
    CHECK(p != NULL, "the churn pack: %s", err);
    if (p) {
        const NkTexReplId a = { 0x0880f000u, 3u, 0x0505u, 0u, 0xa1u }, b = { 0x0880f000u, 3u, 0x0505u, 0u, 0xb1u };
        int hits = 0;
        for (int k = 0; k < 1000; k++) hits += lookup_unpin(p, a, 0) + lookup_unpin(p, b, 0);
        NkTexReplStats st;
        nk_texrepl_stats(p, &st);
        CHECK(st.loaded >= 16 && st.loaded <= 24 && (uint64_t)hits + st.deferred == 2000 &&
                  has_diag(p, "a.png: evicted and decoded again"),
              "two images taking turns in a budget for one: %llu decodes, %d hits, %llu deferred of 2000 lookups",
              (unsigned long long)st.loaded, hits, (unsigned long long)st.deferred);
        printf("cache churn: 2000 lookups of two images in a budget for one: %llu decodes, %llu deferred\n",
               (unsigned long long)st.loaded, (unsigned long long)st.deferred);
        nk_texrepl_close(p);
    }

    {   /* 40,000 held, then one image that needs all their room */
        const unsigned kN = 40000u;
        const uint8_t row[5] = { 0, 9, 8, 7, 255 };
        size_t tn = 0, bn = 0;
        uint8_t *tiny = png_raw(1, 1, 8, 6, row, sizeof(row), NULL, NULL, 0u, &tn);
        const PngSpec bs = { 200, 200, 8, 6, 0, 0, 1, 1, 0, 0x303u };
        uint8_t *big = png_build(&bs, &bn);
        const size_t icap = (size_t)(kN + 1u) * 48u + 64u;
        char *ini = (char *)malloc(icap);
        size_t at = (size_t)snprintf(ini, icap, "[options]\nhash = quick\n[hashes]\n");
        Zw z;
        memset(&z, 0, sizeof(z));
        const ZwOpt stored = { 0, 0, NULL, 0, 0, 0 };
        for (unsigned k = 0; k < kN; k++) {
            char name[16];
            snprintf(name, sizeof(name), "i%05u.png", k);
            zw_add(&z, name, tiny, tn, stored);
            at += (size_t)snprintf(ini + at, icap - at, "0891000000000101%08x = %s\n", k, name);
        }
        zw_add(&z, "big.png", big, bn, stored);
        at += (size_t)snprintf(ini + at, icap - at, "0891000000000101ffffffff = big.png\n");
        zw_add_text(&z, "textures.ini", ini, 0);
        char zip[600];
        snprintf(zip, sizeof(zip), "%s/texrepl_churn_many.zip", SR_SELFTEST_BUILD_ROOT);
        zw_finish(&z, zip, 0);
        free(ini);
        free(tiny);
        free(big);
        err[0] = '\0';
        p = nk_texrepl_open(zip, (uint64_t)kN * 4u, err, sizeof(err));
        CHECK(p != NULL, "the 40,000-image pack: %s", err);
        if (p) {
            int held = 0;
            for (unsigned k = 0; k < kN; k++) {
                const NkTexReplId id = { 0x08910000u, 3u, 0x0101u, 0u, k };
                held += lookup_unpin(p, id, 0);
            }
            const NkTexReplId bid = { 0x08910000u, 3u, 0x0101u, 0u, 0xffffffffu };
            const double t0 = wall_s();
            const int got = lookup_unpin(p, bid, 0);
            const double t = wall_s() - t0;
            NkTexReplStats st;
            nk_texrepl_stats(p, &st);
            CHECK(held == (int)kN && got && st.evicted == kN && t < 0.25,
                  "40,000 held, then one needing their room: %d held, %d, %llu evicted in %.3f s", held, got,
                  (unsigned long long)st.evicted, t);
            printf("cache churn: 40000 held 1x1 images evicted for one 200x200 in %.1f ms\n", t * 1e3);
            nk_texrepl_close(p);
        }
    }

    {   /* a DEFLATE entry's read: the file and its packed bytes at once */
        Bw pre = { 0 };
        uint8_t *zeros = (uint8_t *)calloc(1u << 20, 1);
        png_chunk_out(&pre, "aaaa", zeros, 1u << 20);
        free(zeros);
        const uint8_t row[5] = { 0, 1, 2, 3, 4 };
        size_t n = 0;
        uint8_t *png = png_raw(1, 1, 8, 6, row, sizeof(row), &pre, NULL, 0u, &n);
        free(pre.d);
        Zw z;
        memset(&z, 0, sizeof(z));
        const ZwOpt deflated = { 8, 0, NULL, 0, 0, 0 };
        zw_add(&z, "wide.png", png, n, deflated);
        zw_add_text(&z, "textures.ini", "[options]\nhash = quick\n[hashes]\n0892000000000101000000a1 = wide.png\n", 0);
        char zip[600];
        snprintf(zip, sizeof(zip), "%s/texrepl_read_peak.zip", SR_SELFTEST_BUILD_ROOT);
        zw_finish(&z, zip, 0);
        free(png);
        err[0] = '\0';
        p = nk_texrepl_open(zip, 0, err, sizeof(err));
        CHECK(p != NULL, "the read-peak pack: %s", err);
        if (p) {
            const NkTexReplId id = { 0x08920000u, 3u, 0x0101u, 0u, 0xa1u };
            const int got = lookup_unpin(p, id, 0);
            NkTexReplStats st;
            nk_texrepl_stats(p, &st);
            CHECK(got && st.decode_peak >= 2u * (uint64_t)n && st.decode_peak <= 2u * (uint64_t)n + 4096u,
                  "a %zu-byte DEFLATE entry: decode_peak %llu (the file and its packed bytes: at least %zu)", n,
                  (unsigned long long)st.decode_peak, 2u * n);
            nk_texrepl_close(p);
        }
    }
}

/* Text from a pack reaches the terminal with its control characters as '?' (N7); a NUL byte in
 * textures.ini refuses the pack, where the text after it went unread (N8); a level past an int's
 * range is read as INT_MAX, defined, where sscanf's %d is not (N9). */
static void test_ini_text(void) {
    char dir[512], path[600], err[256] = "";
    make_pack(dir, sizeof(dir), "texrepl_ini_text");
    save_png_spec("texrepl_ini_text", "a.png", 4, 4, 0x311u);
    pack_path(path, sizeof(path), "texrepl_ini_text", "textures.ini");
    write_text(path, "[options]\nhash = quick\n[hashes]\n\x1b[31mzz\xc2\x9b\x9b" "2J\xe0\x80 = a.png\n"
                     "0880e00000000202000000aa = a.png\n0880e00000000202000000aa_99999999999 = a.png\n");
    NkTexRepl *p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL, "the ini text pack: %s", err);
    if (p) {
        int control = 0;
        for (size_t i = 0; i < nk_texrepl_diag_count(p); i++)
            for (const unsigned char *c = (const unsigned char *)nk_texrepl_diag(p, i); *c; c++)
                if (*c < 0x20u || *c == 0x7fu || (*c >= 0x80u && *c <= 0x9fu && (c == (const unsigned char *)nk_texrepl_diag(p, i) || c[-1] != 0xc2u)) ||
                    (*c == 0xc2u && c[1] >= 0x80u && c[1] <= 0x9fu))
                    control++;
        CHECK(control == 0 && has_diag(p, "'?[31mzz?" "?" "?2J?" "?" "'"),
              "a key with ESC, a UTF-8 CSI, a raw CSI and a cut sequence: %d control bytes in the notes", control);
        CHECK(has_diag(p, "mip level 2147483647 is listed with no level 1"), "a level past an int's range not read as INT_MAX");
        nk_texrepl_close(p);
    }
    write_text(path, "[options]\nhash = quick\n\x1b]0;pwn\x07\n");
    err[0] = '\0';
    p = nk_texrepl_open(dir, 0, err, sizeof(err));
    int control = 0;
    for (const unsigned char *c = (const unsigned char *)err; *c; c++) control += *c < 0x20u || *c == 0x7fu;
    CHECK(p == NULL && control == 0 && strstr(err, "'?]0;pwn?'"), "an OSC sequence in a refused line: '%s'", err);
    if (p) nk_texrepl_close(p);
    static const char with_nul[] = "[options]\nhash = quick\n[hashes]\n\0zz = a.png\n";
    file_save(path, (const uint8_t *)with_nul, sizeof(with_nul) - 1);
    err[0] = '\0';
    p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p == NULL && strstr(err, "textures.ini line 4 holds a NUL byte"), "a NUL byte in textures.ini: '%s'", err);
    if (p) nk_texrepl_close(p);
}

/* McIlroy's adversary ("A Killer Adversary for Quicksort", 1999) run against this C library's own
 * qsort gives the order that costs it most; the pack's sorts must not care. The order is a function
 * of n and the C library, so it is kept under the build folder once made. */
static int *g_adv_val;
static int g_adv_solid, g_adv_candidate, g_adv_gas;
static unsigned long long g_cmps;
static int adv_cmp(const void *px, const void *py) {
    const int x = *(const int *)px, y = *(const int *)py;
    g_cmps++;
    if (g_adv_val[x] == g_adv_gas && g_adv_val[y] == g_adv_gas) {
        if (x == g_adv_candidate) g_adv_val[x] = g_adv_solid++;
        else g_adv_val[y] = g_adv_solid++;
    }
    if (g_adv_val[x] == g_adv_gas) g_adv_candidate = x;
    else if (g_adv_val[y] == g_adv_gas) g_adv_candidate = y;
    return (g_adv_val[x] > g_adv_val[y]) - (g_adv_val[x] < g_adv_val[y]);
}
static int int_cmp(const void *a, const void *b) {
    g_cmps++;
    return (*(const int *)a > *(const int *)b) - (*(const int *)a < *(const int *)b);
}
/* The adversary's values for n, against `sorter` (the C library's qsort, or the project's sort). */
static int *adversary(int n, void (*sorter)(void *, size_t, size_t, int (*)(const void *, const void *)),
                      unsigned long long *cmps) {
    int *ptr = (int *)malloc(sizeof(int) * (size_t)n);
    g_adv_val = (int *)malloc(sizeof(int) * (size_t)n);
    g_adv_gas = n - 1;
    g_adv_solid = 0;
    g_adv_candidate = 0;
    for (int i = 0; i < n; i++) { ptr[i] = i; g_adv_val[i] = g_adv_gas; }
    g_cmps = 0;
    sorter(ptr, (size_t)n, sizeof(int), adv_cmp);
    *cmps = g_cmps;
    free(ptr);
    return g_adv_val;
}
static int *libc_adversary(int n) {
    char path[600];
    snprintf(path, sizeof(path), "%s/texrepl_adversary_%d.txt", SR_SELFTEST_BUILD_ROOT, n);
    int *v = (int *)malloc(sizeof(int) * (size_t)n);
    FILE *f = nk_fopen_utf8(path, "rb");
    int got = 0;
    if (f) {
        while (got < n && fscanf(f, "%d", &v[got]) == 1) got++;
        fclose(f);
    }
    if (got == n) return v;
    free(v);
    unsigned long long cmps = 0;
    const double t0 = wall_s();
    v = adversary(n, qsort, &cmps);
    printf("adversary: n=%d costs this C library's qsort %llu comparisons (n*n/4 = %llu), made in %.1f s\n", n, cmps,
           (unsigned long long)n * (unsigned long long)n / 4u, wall_s() - t0);
    f = nk_fopen_utf8(path, "wb");
    if (f) {
        for (int i = 0; i < n; i++) fprintf(f, "%d\n", v[i]);
        fclose(f);
    }
    return v;
}

/* What sorting n items may cost nk_zip_sort: 2 n log2 n + 2 n comparisons. */
static double sort_bound(int n) {
    int lg = 0;
    while ((1 << lg) < n) lg++;
    return 2.0 * n * lg + 2.0 * n;
}

/* Opens `path` and returns the comparisons every sort made on the way (NK_SORT_COUNT_COMPARES counts
 * the comparison functions themselves, so a sort site that went back to the C library's qsort is
 * counted too); `*ok` says whether it opened and `*secs` how long it took. */
static unsigned long long open_counted(const char *path, int *ok, double *secs) {
    char err[256] = "";
    nk_sort_compares = 0;
    const double t0 = wall_s();
    NkTexRepl *p = nk_texrepl_open(path, 0, err, sizeof(err));
    *secs = wall_s() - t0;
    const unsigned long long c = nk_sort_compares;
    *ok = p != NULL;
    if (!p) printf("  open %s: %s\n", path, err);
    nk_texrepl_close(p);
    return c;
}

/* The project sort: at most 2 n log2 n + 2 n comparisons on the worst orders it can be given, and
 * right on every order of up to 7 items. Then packs in the order that costs the C library's qsort
 * most (n * n / 4 comparisons on Windows) open within a bound on the comparisons their sorts make,
 * counted at the comparison functions: the [hashes] lines, the [indexed] lines, and a ZIP whose names
 * and local-header offsets come in that order. On a C library whose qsort has no such order (glibc
 * sorts by merging) the bound holds either way. */
static void test_sort_adversary(void) {
    /* every permutation of up to 7 items */
    for (int n = 0; n <= 7; n++) {
        int perm[7], fact = 1;
        for (int i = 2; i <= n; i++) fact *= i;
        for (int k = 0; k < fact; k++) {
            int left[7], c = k;
            for (int i = 0; i < n; i++) left[i] = i;
            for (int i = 0; i < n; i++) {
                const int pick = c % (n - i);
                c /= n - i;
                perm[i] = left[pick];
                memmove(left + pick, left + pick + 1, sizeof(int) * (size_t)(n - i - pick - 1));
            }
            nk_zip_sort(perm, (size_t)n, sizeof(int), int_cmp);
            for (int i = 0; i < n; i++) CHECK(perm[i] == i, "nk_zip_sort on a permutation of %d", n);
        }
    }
    const int kN = 200000;
    const double bound = sort_bound(kN);
    unsigned long long cmps = 0;
    free(adversary(kN, nk_zip_sort, &cmps));
    CHECK((double)cmps <= bound, "the adversary against nk_zip_sort: %llu comparisons", cmps);
    int *copy = (int *)malloc(sizeof(int) * (size_t)kN);
    for (int i = 0; i < kN; i++) copy[i] = kN - i;
    g_cmps = 0;
    nk_zip_sort(copy, (size_t)kN, sizeof(int), int_cmp);
    CHECK((double)g_cmps <= bound, "a reversed order: %llu comparisons", g_cmps);
    for (int i = 0; i < kN; i++) copy[i] = 5;
    g_cmps = 0;
    nk_zip_sort(copy, (size_t)kN, sizeof(int), int_cmp);
    CHECK((double)g_cmps <= bound, "all equal: %llu comparisons", g_cmps);

    /* The C library's worst order for 16,384 items (n * n / 4 = 67,108,864 comparisons on Windows). */
    const int kL = 16384;
    const double lbound = sort_bound(kL);
    int *v = libc_adversary(kL);
    memcpy(copy, v, sizeof(int) * (size_t)kL);
    g_cmps = 0;
    nk_zip_sort(copy, (size_t)kL, sizeof(int), int_cmp);
    const unsigned long long cmps_libc_order = g_cmps;
    for (int i = 0; i < kL; i++) if (copy[i] != i) { CHECK(0, "nk_zip_sort left the adversary's order unsorted"); break; }
    CHECK((double)g_cmps <= lbound, "qsort's worst order through nk_zip_sort: %llu comparisons", g_cmps);
    free(copy);
    printf("sort: n=%d, at most %.0f comparisons allowed; McIlroy's adversary against nk_zip_sort %llu; qsort's worst "
           "order (n=%d) %llu of %.0f\n", kN, bound, cmps, kL, cmps_libc_order, lbound);

    /* textures.ini with kL [hashes] lines in that order: the lines are sorted once, and their kL
     * levels once more by file name. Then kL [indexed] lines in that order. */
    char dir[512], path[600];
    size_t cap = (size_t)kL * 40u + 256u, at = 0;
    char *ini = (char *)malloc(cap);
    double t_ini = 0, t_idx = 0;
    int ok = 0;
    make_pack(dir, sizeof(dir), "texrepl_adv_ini");
    at += (size_t)snprintf(ini, cap, "[options]\nhash=quick\n[hashes]\n");
    /* line i names file v[v[i]]: once the lines are sorted by key, the file names come in the
     * adversary's order too, for the sort that gives each distinct file its one image */
    for (int i = 0; i < kL; i++) at += (size_t)snprintf(ini + at, cap - at, "%x=%05d.png\n", (unsigned)v[i], v[v[i]]);
    pack_path(path, sizeof(path), "texrepl_adv_ini", "textures.ini");
    write_text(path, ini);
    const unsigned long long c_ini = open_counted(dir, &ok, &t_ini);
    CHECK(ok && (double)c_ini <= 2.0 * lbound, "the adversary's [hashes] lines: %llu comparisons (at most %.0f)", c_ini,
          2.0 * lbound);
    at = (size_t)snprintf(ini, cap, "[options]\nhash=quick\n[hashes]\n1=a\n[indexed]\n");
    for (int i = 0; i < kL; i++) at += (size_t)snprintf(ini + at, cap - at, "%x=true\n", (unsigned)v[i]);
    write_text(path, ini);
    const unsigned long long c_idx = open_counted(dir, &ok, &t_idx);
    CHECK(ok && (double)c_idx <= lbound + 64.0, "the adversary's [indexed] lines: %llu comparisons (at most %.0f)", c_idx,
          lbound + 64.0);
    /* kL [hashranges] lines and kL [reducehashranges] lines in that order: two sorts of kL. */
    at = (size_t)snprintf(ini, cap, "[options]\nhash=xxh32\n[hashranges]\n");
    for (int i = 0; i < kL; i++)
        at += (size_t)snprintf(ini + at, cap - at, "%08x,16,16 = 16,8\n", 0x08800000u + (unsigned)v[i] * 0x40u);
    at += (size_t)snprintf(ini + at, cap - at, "[reducehashranges]\n");
    for (int i = 0; i < kL; i++) at += (size_t)snprintf(ini + at, cap - at, "%u,4 = 0.5\n", (unsigned)v[i] + 1u);
    write_text(path, ini);
    double t_rng = 0;
    const unsigned long long c_rng = open_counted(dir, &ok, &t_rng);
    /* at least 2 (kL - 1): both sorts ran, over every line */
    CHECK(ok && (double)c_rng <= 2.0 * lbound && c_rng >= 2ull * (unsigned long long)(kL - 1),
          "the adversary's range lines: %llu comparisons (from %d to %.0f)", c_rng, 2 * (kL - 1), 2.0 * lbound);
    printf("sort: qsort's worst order (n=%d) as [hashranges] and [reducehashranges] lines: %llu comparisons in %.3f s\n",
           kL, c_rng, t_rng);
    free(ini);

    /* A kL-entry ZIP in that order: 200-byte names, local headers laid out in value order too so the
     * offset sort sees it, textures.ini the greatest name. Three sorts of kL: the reader's names, its
     * offsets, and the pack's names under its root. */
    const int zn = kL;
    int *zv = v;
    static const char kZini[] = "[options]\nhash = quick\n";
    char (*names)[201] = (char (*)[201])malloc((size_t)zn * 201u);
    uint32_t *offs = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)zn);
    int *order = (int *)malloc(sizeof(int) * (size_t)zn);
    for (int i = 0; i < zn; i++) {
        if (zv[i] == zn - 1) snprintf(names[i], 201, "textures.ini");
        else {
            memset(names[i], 'a', 191);
            snprintf(names[i] + 191, 10, "%05d.png", zv[i]);
        }
        order[zv[i]] = i;
    }
    Bw body = { 0 }, cd = { 0 };
    for (int k = 0; k < zn; k++) {
        const int i = order[k];
        const int is_ini = zv[i] == zn - 1;
        const size_t dl = is_ini ? sizeof(kZini) - 1 : 0, nl = strlen(names[i]);
        const uint32_t crc = nk_zip_crc32(0, kZini, dl);
        uint8_t h[30];
        memset(h, 0, sizeof(h));
        le32(h, 0x04034b50u);
        le16(h + 4, 20u);
        le16(h + 12, 0x21u);
        le32(h + 14, crc);
        le32(h + 18, (uint32_t)dl);
        le32(h + 22, (uint32_t)dl);
        le16(h + 26, (uint32_t)nl);
        offs[i] = (uint32_t)body.n;
        zbuf(&body.d, &body.n, &body.cap, h, 30);
        zbuf(&body.d, &body.n, &body.cap, names[i], nl);
        if (dl) zbuf(&body.d, &body.n, &body.cap, kZini, dl);
    }
    for (int i = 0; i < zn; i++) {
        const int is_ini = zv[i] == zn - 1;
        const size_t dl = is_ini ? sizeof(kZini) - 1 : 0, nl = strlen(names[i]);
        uint8_t h[46];
        memset(h, 0, sizeof(h));
        le32(h, 0x02014b50u);
        le16(h + 4, 20u);
        le16(h + 6, 20u);
        le16(h + 14, 0x21u);
        le32(h + 16, nk_zip_crc32(0, kZini, dl));
        le32(h + 20, (uint32_t)dl);
        le32(h + 24, (uint32_t)dl);
        le16(h + 28, (uint32_t)nl);
        le32(h + 42, offs[i]);
        zbuf(&cd.d, &cd.n, &cd.cap, h, 46);
        zbuf(&cd.d, &cd.n, &cd.cap, names[i], nl);
    }
    uint8_t e[22];
    memset(e, 0, sizeof(e));
    le32(e, 0x06054b50u);
    le16(e + 8, (uint32_t)zn);
    le16(e + 10, (uint32_t)zn);
    le32(e + 12, (uint32_t)cd.n);
    le32(e + 16, (uint32_t)body.n);
    zbuf(&body.d, &body.n, &body.cap, cd.d, cd.n);
    zbuf(&body.d, &body.n, &body.cap, e, 22);
    char zip[600];
    snprintf(zip, sizeof(zip), "%s/texrepl_adv.zip", SR_SELFTEST_BUILD_ROOT);
    file_save(zip, body.d, body.n);
    free(body.d);
    free(cd.d);
    free(names);
    free(offs);
    free(order);
    free(v);
    double t_zip = 0;
    const unsigned long long c_zip = open_counted(zip, &ok, &t_zip);
    CHECK(ok && (double)c_zip <= 3.0 * lbound, "the adversary's %d-entry ZIP: %llu comparisons (at most %.0f)", zn,
          c_zip, 3.0 * lbound);
    printf("sort: qsort's worst order (n=%d) opens with %llu comparisons in %.3f s as [hashes] lines, %llu in %.3f s as "
           "[indexed] lines, %llu in %.3f s as a ZIP\n", kL, c_ini, t_ini, c_idx, t_idx, c_zip, t_zip);
}

/* [hashranges]: the last line for a texture wins after the sort; 175,000 lines in reverse order (a
 * linear scan per line would make that 1.5 * 10^10 steps) open within 5 s and the sort's comparison
 * bound, and hash quickly. [reducehashranges] NaN and infinity are not read. */
static void test_hashranges_index(void) {
    char dir[512], path[600], err[256] = "";
    make_pack(dir, sizeof(dir), "texrepl_ranges");
    pack_path(path, sizeof(path), "texrepl_ranges", "textures.ini");
    write_text(path, "[options]\nhash = xxh32\n[hashranges]\n08804000,16,16 = 16,16\n"
                     "08804000,16,16 = 16,8\n[reducehashranges]\n128,64 = nan\n64,64 = inf\n32,32 = -nan\n");
    NkTexRepl *p = nk_texrepl_open(dir, 0, err, sizeof(err));
    CHECK(p != NULL, "ranges pack: %s", err);
    uint8_t mem[16 * 16 * 4];
    for (size_t i = 0; i < sizeof(mem); i++) mem[i] = (uint8_t)(i * 7 + 3);
    if (p) {
        CHECK(nk_texrepl_hash_texture(p, mem, sizeof(mem), 0x08804000u, 16, 16, 16, 3, 0) ==
                  nk_texrepl_xxh32(mem, 16 * 8 * 4, 0xBACD7814u),
              "the later [hashranges] line for a texture did not win");
        CHECK(has_diag(p, "line for '128' is not a finite number") && has_diag(p, "line for '64' is not a finite number"),
              "NaN or infinity in [reducehashranges] not refused");
        nk_texrepl_close(p);
    }
    /* 175,000 lines, addresses descending, near the 4 MiB the ini may hold. */
    const unsigned n = 175000u;   /* 23 bytes a line: 4.0 MB */
    size_t cap = (size_t)(n + 1) * 24u + 2048u, at = 0;
    char *ini = (char *)malloc(cap);
    at += (size_t)snprintf(ini, cap, "[options]\nhash = xxh32\n[hashranges]\n");
    for (unsigned i = 0; i < n; i++)
        at += (size_t)snprintf(ini + at, cap - at, "%08x,16,16 = 16,8\n", 0x08ffc000u - i * 0x40u);
    at += (size_t)snprintf(ini + at, cap - at, "[reducehashranges]\n");
    for (unsigned i = 0; i < 64; i++) at += (size_t)snprintf(ini + at, cap - at, "%u,%u = 0.5\n", 512u - i, 4u);
    write_text(path, ini);
    nk_sort_compares = 0;
    double t0 = wall_s();
    p = nk_texrepl_open(dir, 0, err, sizeof(err));
    const double t_open = wall_s() - t0;
    const unsigned long long c_open = nk_sort_compares;
    CHECK(p != NULL && t_open < 5.0, "%u [hashranges] lines: %s, %.2f s", n, err, t_open);
    CHECK((double)c_open <= sort_bound((int)n) + sort_bound(64), "%u [hashranges] lines: %llu comparisons", n, c_open);
    double t_hash = 0;
    if (p) {
        t0 = wall_s();
        uint32_t acc = 0;
        for (unsigned i = 0; i < 1000; i++)
            acc ^= nk_texrepl_hash_texture(p, mem, sizeof(mem), 0x08ffc000u - (i * 13u % n) * 0x40u, 16, 16, 16, 3, 0);
        t_hash = wall_s() - t0;
        CHECK(nk_texrepl_hash_texture(p, mem, sizeof(mem), 0x08ffc000u - 77u * 0x40u, 16, 16, 16, 3, 0) ==
                  nk_texrepl_xxh32(mem, 16 * 8 * 4, 0xBACD7814u),
              "a range found by the index");
        (void)acc;
        nk_texrepl_close(p);
    }
    free(ini);
    printf("hashranges: %u lines open in %.1f ms with %llu comparisons, 1000 hashes %.2f ms\n", n, t_open * 1e3,
           c_open, t_hash * 1e3);
}

/* DEFLATE padding: an entry declaring 1 byte packed as 1 MiB of empty stored blocks is refused at
 * open, as an image and as textures.ini. */
static void test_zip_padding(void) {
    char dir[512], zip[600], err[300];
    make_pack(dir, sizeof(dir), "texrepl_zip_pad");
    const size_t pad = 1u << 20;
    uint8_t *blocks = (uint8_t *)malloc(pad + 6);
    for (size_t i = 0; i + 5 <= pad; i += 5) { blocks[i] = 0; blocks[i + 1] = 0; blocks[i + 2] = 0; blocks[i + 3] = 0xff; blocks[i + 4] = 0xff; }
    const size_t bn = pad / 5 * 5;
    blocks[bn] = 1; blocks[bn + 1] = 1; blocks[bn + 2] = 0; blocks[bn + 3] = 0xfe; blocks[bn + 4] = 0xff; blocks[bn + 5] = 'x';
    for (int as_ini = 0; as_ini <= 1; as_ini++) {
        Zw z;
        memset(&z, 0, sizeof(z));
        const ZwOpt padded = { 8, 0, blocks, bn + 6, 0, 0 };
        if (as_ini) zw_add(&z, "textures.ini", "x", 1, padded);
        else {
            zw_add_text(&z, "textures.ini", "[options]\nhash = quick\n[hashes]\n088040000000020211223344 = a.png\n", 0);
            zw_add(&z, "a.png", "x", 1, padded);
        }
        pack_path(zip, sizeof(zip), "texrepl_zip_pad", as_ini ? "pad_ini.zip" : "pad_png.zip");
        zw_finish(&z, zip, 0);
        err[0] = '\0';
        NkTexRepl *p = nk_texrepl_open(zip, 0, err, sizeof(err));
        CHECK(p == NULL && strstr(err, "ZIP_SIZE_MISMATCH"), "1 byte packed in 1 MiB (%s): '%s'", as_ini ? "ini" : "image", err);
        if (p) nk_texrepl_close(p);
    }
    free(blocks);
}

/* --bench N: a synthetic pack of N textures (32x32 PNGs, one textures.ini line each, files in a
 * subfolder) as a folder and as a ZIP of DEFLATE entries in one top-level folder, and N hash-named
 * PNGs in one folder with no [hashes] lines (the listing then finds every texture); prints open,
 * listing, lookup, first-decode and memory figures for each. --bench-only N folder|zip|named
 * measures one pack --bench wrote, in a fresh process, so its peak working set is that pack's. */
static int bench_measure(int n, const char *kind);

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
    const ZwOpt deflated = { 8, 0, NULL, 0, 0, 0 };
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
    char named[512];
    make_pack(named, sizeof(named), "texrepl_bench/named");
    for (int i = 0; i < n; i++) {
        px[0] = 0xff000000u | (uint32_t)i;
        snprintf(path, sizeof(path), "%s/%016llx%08x.png", named,
                 (unsigned long long)nk_texrepl_cache_key(0x08800000u + ((uint32_t)i << 4), 3u, 0x0505u, 0u), (unsigned)i);
        write_png(path, 32, 32, px);
    }
    snprintf(path, sizeof(path), "%s/textures.ini", named);
    write_text(path, "[options]\nhash = xxh32\n");
    free(ini);
    free(px);
    printf("bench: wrote %d textures as a folder, a ZIP and a hash-named folder in %.2f s\n", n, wall_s() - t_write);
    (void)err;
    return bench_measure(n, "folder") || bench_measure(n, "zip") || bench_measure(n, "named");
}

static int bench_measure(int n, const char *kind) {
    char path[600], err[400];
    if (!strcmp(kind, "zip")) snprintf(path, sizeof(path), "%s/texrepl_bench/pack.zip", SR_SELFTEST_BUILD_ROOT);
    else snprintf(path, sizeof(path), "%s/texrepl_bench/%s", SR_SELFTEST_BUILD_ROOT, kind);
    {
        const char *const kinds[1] = { kind };
        const int k = 0;
        double ws0, pk0, ws1, pk1;
        mem_now(&ws0, &pk0);
        double t0 = wall_s();
        NkTexRepl *p = nk_texrepl_open(path, 0, err, sizeof(err));
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
        printf("bench %s: entries=%zu open=%.1f ms (listing %.1f ms) index=%.2f MiB ws_before=%.1f MiB "
               "open_ws_delta=%.2f MiB peak_after_open=%.1f MiB | first use %d/%d: %.1f us each"
               " | held lookup %.3f us | miss %.3f us | cache=%.2f MiB peak_ws=%.1f MiB\n",
               kinds[k], nk_texrepl_entry_count(p), t_open * 1e3, (double)st.list_ns / 1e6,
               (double)st.index_bytes / 1048576.0, ws0, ws1 >= 0 ? ws1 - ws0 : -1.0, pk1, hits, n,
               t_first * 1e6 / n, t_held * 1e6 / n, t_miss * 1e6 / n, (double)st.cache_bytes / 1048576.0, pk2);
        nk_texrepl_close(p);
    }
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 3 && !strcmp(argv[1], "--bench")) return bench(atoi(argv[2]));
    if (argc == 4 && !strcmp(argv[1], "--bench-only")) return bench_measure(atoi(argv[2]), argv[3]);
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
    test_zip_refusals();
    test_png_corpus();
    test_png_seeds();
    test_png_plte_lenient();
    test_png_checks_pinned();
    test_image_guards();
    test_png_bomb();
    test_png_work();
    test_inflate_dynamic_budget();
    test_cache_churn();
    test_ini_text();
    test_zip_padding();
    test_hashranges_index();
    test_sort_adversary();
    test_file_checks();
    if (g_failures) {
        fprintf(stderr, "texrepl selftest: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("texrepl selftest: OK\n");
    return 0;
}
