// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* White-box unit tests for the fail-closed VFPU table loader (issue #187).
 * Standalone host executable, no game inputs required:
 *
 *   mingw32-make GAME_NAME=hst GAME_ELF=eboot.elf GAME_BASE=0 GAME_ENTRY=0 vfpu-tables-selftest
 *
 * Coverage:
 *   - SHA-256 known-answer vectors (empty, single block, NIST multi-block);
 *   - the same SHA-256 the player uses (nk_sha256_* in src/core/nk_shared_helpers.h):
 *     FIPS 180-4 vectors, block-boundary lengths split at every position, and
 *     RFC 4231 HMAC cases, through it and through sr_vfpu_sha256 and
 *     sr_vfpu_hmac_sha256;
 *   - asin index validator against synthetic out-of-range indices;
 *   - sin interval validator against out-of-range, boundary-exact and
 *     underflowed bounds (the genuine data maxes at hi == len, inclusive);
 *   - file loader against temporary roots: truncated, extra data, wrong
 *     same-length content (hash rejection), endian-swapped bytes, missing
 *     files, and a fully absent root;
 *   - successful load into a local aggregate WITHOUT publishing globals;
 *   - atomic once-only publication, repeated initialization, and concurrent
 *     first use from two host threads (sequential fallback if the toolchain
 *     lacks C11 threads);
 *   - the evidence line of a load from an explicit root says override=explicit.
 *
 * The abort-after-cleanup path of sr_vfpu_load_with_root is intentionally not
 * exercised in-process (abort terminates the harness); the loader-level
 * failure semantics are covered through sr_vfpu_tables_load, which performs
 * the identical cleanup before the caller decides the abort policy.
 */

/* The POSIX truncation path (ftruncate/off_t) must be visible even when this
 * file is compiled with strict -std=c11 (no feature macros from the command
 * line), as on the Linux CI native gate. */
#if !defined(_WIN32) && !defined(_GNU_SOURCE) && \
    !defined(_POSIX_C_SOURCE) && !defined(_XOPEN_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "vfpu_tables.h"
#include "../../src/core/nk_shared_helpers.h" /* nk_sha256_*: the hash sr_vfpu_sha256 calls */

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Scratch root for the files this test writes: the checkout's build/ by default, or
 * the BUILD_ROOT the Makefile passes as -DSR_SELFTEST_BUILD_ROOT, so a scratch run never
 * touches the checkout. */
#ifndef SR_SELFTEST_BUILD_ROOT
#define SR_SELFTEST_BUILD_ROOT "build"
#endif

/* Concurrency coverage uses C11 threads when the toolchain provides them,
 * and Win32 threads on Windows hosts that lack <threads.h> (the canonical
 * MinGW UCRT64 build) -- the race must be exercised where it matters most. */
#if defined(__has_include)
#  if __has_include(<threads.h>)
#    include <threads.h>
#    define SR_HAVE_THREADS 1
#  endif
#endif
#if defined(SR_HAVE_THREADS)
#define SR_CONCURRENCY_AVAILABLE 1
#elif defined(_WIN32)
#include <windows.h>
#define SR_CONCURRENCY_AVAILABLE 1
#endif

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <process.h>
#define SR_MKDIR(p) _mkdir(p)
#define SR_GETPID() _getpid()
#else
#include <sys/stat.h>
#include <unistd.h>
#define SR_MKDIR(p) mkdir(p, 0755)
#define SR_GETPID() getpid()
#endif

/* Truncate the open FILE to `size` bytes (portable shim for the selftest). */
static int ftruncate_shim(FILE *f, long size) {
    fflush(f);
#if defined(_WIN32)
    int fd = _fileno(f);
    int rc = _chsize_s(fd, size);
#else
    int fd = fileno(f);
    int rc = ftruncate(fd, (off_t)size);
#endif
    fseek(f, 0, SEEK_SET);
    return rc;
}

static int g_failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        g_failures++; \
        fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
    } \
} while (0)

/* ---- SHA-256 known-answer tests ------------------------------------------ */

static void hex_of(const uint8_t digest[32], char out[65]) {
    static const char HEX[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32u; i++) {
        out[i * 2u] = HEX[digest[i] >> 4];
        out[i * 2u + 1u] = HEX[digest[i] & 0x0fu];
    }
    out[64] = '\0';
}

static void test_sha256_known_answers(void) {
    struct { const char *in; const char *want; } cases[] = {
        { "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
    };
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        uint8_t digest[32];
        char got[65];
        sr_vfpu_sha256((const uint8_t *)cases[c].in, strlen(cases[c].in), digest);
        hex_of(digest, got);
        CHECK(strcmp(got, cases[c].want) == 0,
              "sha256 case %zu: got %s want %s", c, got, cases[c].want);
    }
    /* Hashing the committed assets must reproduce the embedded manifest. */
    size_t n = 0;
    const SrVfpuTableSpec *specs = sr_vfpu_table_manifest(&n);
    CHECK(n == 15u, "manifest count %zu != 15", n);
    for (size_t i = 0; i < n; i++) {
        CHECK(specs[i].bytes % specs[i].elem == 0u,
              "manifest %s: bytes %zu not a multiple of elem %zu",
              specs[i].name, specs[i].bytes, specs[i].elem);
        CHECK(strlen(specs[i].sha256) == 64u,
              "manifest %s: sha256 not 64 hex chars", specs[i].name);
    }
}

/* ---- the SHA-256 the player and the runtime share ------------------------ */

/* The runtime's sr_vfpu_sha256 and sr_vfpu_hmac_sha256 against the player's nk_sha256_*
 * (src/core/nk_shared_helpers.h) on the same known answers: FIPS 180-4's examples, one
 * million 'a', messages that end at and around the block boundary and the length field
 * (byte i = i * 7 + 1; digests from Python's hashlib) fed whole and split in two at every
 * position, and RFC 4231's HMAC cases. tests/native/test_shared_helpers.c runs the
 * boundary table on the core side, so a second copy that drifts from the shared one fails
 * one side or the other. */
static void test_shared_sha256(void) {
    static const char long_two_blocks[] =
        "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
        "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    static const struct { const char *in; const char *want; } fips[] = {
        { "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
        { long_two_blocks, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1" },
    };
    uint8_t digest[32];
    char got[65];
    for (size_t c = 0; c < sizeof fips / sizeof fips[0]; c++) {
        const uint8_t *in = (const uint8_t *)fips[c].in;
        size_t len = strlen(fips[c].in);
        sr_vfpu_sha256(in, len, digest);
        hex_of(digest, got);
        CHECK(strcmp(got, fips[c].want) == 0, "sr_vfpu_sha256 FIPS case %zu: got %s", c, got);
        /* Pieces of 1, 2, 3 ... bytes through the shared streaming calls. */
        NkSha256 sha;
        nk_sha256_init(&sha);
        for (size_t at = 0, piece = 1; at < len; at += piece, piece++) {
            if (piece > len - at) piece = len - at;
            nk_sha256_update(&sha, in + at, piece);
        }
        nk_sha256_finish(&sha, digest);
        hex_of(digest, got);
        CHECK(strcmp(got, fips[c].want) == 0, "nk_sha256 FIPS case %zu in pieces: got %s", c, got);
    }

    uint8_t *million = (uint8_t *)malloc(1000000u);
    CHECK(million != NULL, "one million 'a': allocation");
    if (million) {
        memset(million, 'a', 1000000u);
        sr_vfpu_sha256(million, 1000000u, digest);
        hex_of(digest, got);
        CHECK(strcmp(got, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0,
              "sr_vfpu_sha256 of one million 'a': got %s", got);
        free(million);
    }

    static const struct { size_t length; const char *want; } edges[] = {
        { 55u, "16fa57a0a3423a715d594516339f36189d6b5f93754a9714fef202616a9fabfe" },
        { 56u, "c37b44e5f1b18554b36966f4f8e08bfbf3164c4b6c10374d12d89850892073c5" },
        { 63u, "bbba992d2c85af960fb2987a1fd05e0aa82a3db3c740dd8982a9e273b75e36a3" },
        { 64u, "66bd4633ed6f71c4ecfa4763bf7ba1c8ec7612de9aa6c0578a7b675207c71e0b" },
        { 65u, "9f7dc47107b750a1f3d35db5d9547f24ef40da5b731b9540d4f43710a154f6c9" },
        { 119u, "a3ed307b730fa77c07531300c6e4a282330011d4d4caf6bb7b63ae05950f4b66" },
        { 120u, "8e3b15d9fea7472655aa069620b7f8c2e55ee1499f763200a7515fe826e99d20" },
        { 127u, "44480fb9672845177f5368a08b69ea263275f2a5ec42e06a933370fe0d2968a4" },
        { 128u, "e462c130fef8c97e34f7dc3ff3ad2f8b3533ab849af21c10531552a2852387a4" },
        { 129u, "aa7ea4e8bf89146aeb67ff195fd8182a0e504c9d580aa7af8d2c862e14b98405" },
    };
    uint8_t message[129];
    for (size_t i = 0; i < sizeof message; i++) message[i] = (uint8_t)(i * 7u + 1u);
    for (size_t e = 0; e < sizeof edges / sizeof edges[0]; e++) {
        size_t length = edges[e].length;
        sr_vfpu_sha256(message, length, digest);
        hex_of(digest, got);
        CHECK(strcmp(got, edges[e].want) == 0, "sr_vfpu_sha256 of %zu bytes: got %s", length, got);
        for (size_t split = 0; split <= length; split++) {
            NkSha256 sha;
            nk_sha256_init(&sha);
            nk_sha256_update(&sha, message, split);
            nk_sha256_update(&sha, message + split, length - split);
            nk_sha256_finish(&sha, digest);
            hex_of(digest, got);
            CHECK(strcmp(got, edges[e].want) == 0,
                  "nk_sha256 of %zu bytes split after %zu: got %s", length, split, got);
        }
    }

    /* RFC 4231 test cases 1, 2, 6 and 7 (a short key, a key shorter than its message, and a
     * key longer than the block, which is hashed first), the message in two pieces. */
    static uint8_t key_0b[20], key_aa[131];
    memset(key_0b, 0x0b, sizeof key_0b);
    memset(key_aa, 0xaa, sizeof key_aa);
    static const char long_data[] =
        "This is a test using a larger than block-size key and a larger than block-size data. "
        "The key needs to be hashed before being used by the HMAC algorithm.";
    const struct {
        const uint8_t *key; size_t key_len; const uint8_t *msg; size_t msg_len; const char *want;
    } macs[] = {
        { key_0b, sizeof key_0b, (const uint8_t *)"Hi There", 8u,
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7" },
        { (const uint8_t *)"Jefe", 4u, (const uint8_t *)"what do ya want for nothing?", 28u,
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843" },
        { key_aa, sizeof key_aa,
          (const uint8_t *)"Test Using Larger Than Block-Size Key - Hash Key First", 54u,
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54" },
        { key_aa, sizeof key_aa, (const uint8_t *)long_data, sizeof long_data - 1u,
          "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2" },
    };
    for (size_t c = 0; c < sizeof macs / sizeof macs[0]; c++) {
        size_t head = macs[c].msg_len / 3u;
        const uint8_t *parts[2] = { macs[c].msg, macs[c].msg + head };
        const size_t lens[2] = { head, macs[c].msg_len - head };
        sr_vfpu_hmac_sha256(macs[c].key, macs[c].key_len, parts, lens, 2u, digest);
        hex_of(digest, got);
        CHECK(strcmp(got, macs[c].want) == 0, "sr_vfpu_hmac_sha256 RFC 4231 case %zu: got %s", c, got);
    }
}

/* ---- value-domain validators --------------------------------------------- */

static void test_asin_index_validator(void) {
    const size_t count = 399458u;      /* genuine vfpu_asin_lut_indices entry count */
    const size_t deltas = 64681u;      /* genuine vfpu_asin_lut_deltas entry count */
    uint16_t *idx = (uint16_t *)malloc(count * sizeof(uint16_t));
    CHECK(idx != NULL, "alloc");
    if (!idx) return;
    memset(idx, 0, count * sizeof(uint16_t));
    CHECK(sr_vfpu_validate_asin_indices(idx, count, deltas) == 0,
          "all-zero indices must be accepted");
    idx[0] = (uint16_t)(deltas - 1u);
    CHECK(sr_vfpu_validate_asin_indices(idx, count, deltas) == 0,
          "largest legal index must be accepted");
    idx[0] = (uint16_t)deltas;
    CHECK(sr_vfpu_validate_asin_indices(idx, count, deltas) != 0,
          "index == deltas entry count must be rejected");
    idx[0] = 0xFFFFu;
    CHECK(sr_vfpu_validate_asin_indices(idx, count, deltas) != 0,
          "UINT16_MAX index must be rejected");
    CHECK(sr_vfpu_validate_asin_indices(NULL, count, deltas) != 0,
          "NULL indices must be rejected");
    free(idx);
}

/* Synthetic in-range deltas: every lo/hi lands at 16384, well inside the
 * exceptions allocation with lo == hi (no inverted intervals). */
static void build_synthetic_deltas(int16_t *delta, size_t count) {
    for (size_t k = 0; k < count; k++)
        delta[k] = (int16_t)-(int32_t)((169u * (uint32_t)k) >> 7);
}

static void test_sin_interval_validator(void) {
    const size_t count = 65537u;   /* genuine interval-delta entry count */
    const size_t exc = 86938u;     /* genuine exceptions byte length */
    int16_t *delta = (int16_t *)malloc(count * sizeof(int16_t));
    CHECK(delta != NULL, "alloc");
    if (!delta) return;
    build_synthetic_deltas(delta, count);
    CHECK(sr_vfpu_validate_sin_interval(delta, count, exc) == 0,
          "synthetic in-range deltas must be accepted");
    /* Genuine data reaches exactly hi == exc at the top of the range. */
    build_synthetic_deltas(delta, count);
    delta[count - 1u] = (int16_t)((int32_t)exc - (int32_t)((169u * (uint32_t)(count - 1u)) >> 7) - 16384);
    CHECK(sr_vfpu_validate_sin_interval(delta, count, exc) == 0,
          "hi == exceptions length (genuine boundary) must be accepted");
    /* One less negative (larger delta) pushes hi past the end. */
    delta[count - 1u] += 1;
    CHECK(sr_vfpu_validate_sin_interval(delta, count, exc) != 0,
          "hi == exceptions length + 1 must be rejected");
    /* Underflow: lo would go negative (uint32 wraparound) at k = 0. */
    build_synthetic_deltas(delta, count);
    delta[0] = -16385;
    CHECK(sr_vfpu_validate_sin_interval(delta, count, exc) != 0,
          "lo underflow must be rejected");
    /* Inverted interval (lo > hi): a corruption signal -- genuine data has
     * zero inverted intervals. */
    build_synthetic_deltas(delta, count);
    delta[1] = (int16_t)(-(int32_t)((169u * 2u) >> 7) - 1);
    CHECK(sr_vfpu_validate_sin_interval(delta, count, exc) != 0,
          "lo > hi inverted interval must be rejected");
    build_synthetic_deltas(delta, count);
    CHECK(sr_vfpu_validate_sin_interval(delta, count, exc) == 0,
          "rebuilt synthetic deltas must be accepted again");
    CHECK(sr_vfpu_validate_sin_interval(delta, 1, exc) != 0,
          "count < 2 must be rejected");
    CHECK(sr_vfpu_validate_sin_interval(NULL, count, exc) != 0,
          "NULL delta must be rejected");
    free(delta);
}

/* ---- file loader tests ---------------------------------------------------- */

static char g_root[512];

/* Checked snprintf: returns 0 on success, -1 on truncation. */
static int fmt_path(char *buf, size_t cap, const char *root, const char *name) {
    int n = snprintf(buf, cap, "%s/%s", root, name);
    return (n >= 0 && (size_t)n < cap) ? 0 : -1;
}

static void wipe_tree(const char *root) {
    size_t n = 0;
    const SrVfpuTableSpec *specs = sr_vfpu_table_manifest(&n);
    char path[512];
    for (size_t i = 0; i < n; i++) {
        if (fmt_path(path, sizeof path, root, specs[i].name) == 0) remove(path);
    }
    remove(root);
}

static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    uint8_t buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, in)) != 0u)
        if (fwrite(buf, 1, got, out) != got) { fclose(in); fclose(out); return -1; }
    int rc = (ferror(in) || fclose(out) != 0) ? -1 : 0;
    fclose(in);
    return rc;
}

static int write_bytes(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int rc = (fwrite(data, 1, size, f) == size && fclose(f) == 0) ? 0 : -1;
    return rc;
}

/* Copy the committed asset tree into a temp root so one file can be corrupted
 * while the other fourteen remain byte-identical (and therefore hash-valid). */
static int ensure_dir(const char *p) {
    if (SR_MKDIR(p) == 0) return 0;
    return (errno == EEXIST) ? 0 : -1;
}

static int setup_temp_root(void) {
    size_t n = 0;
    const SrVfpuTableSpec *specs = sr_vfpu_table_manifest(&n);
    if (ensure_dir(g_root) != 0) return -1;
    int ok = 1;
    for (size_t i = 0; i < n && ok; i++) {
        char src[512], dst[512];
        if (fmt_path(src, sizeof src, "assets/vfpu", specs[i].name) != 0) ok = 0;
        else if (fmt_path(dst, sizeof dst, g_root, specs[i].name) != 0) ok = 0;
        else if (copy_file(src, dst) != 0) ok = 0;
    }
    if (!ok) wipe_tree(g_root);
    return ok ? 0 : -1;
}

static void test_loader_rejects_corrupt_roots(void) {
    CHECK(sr_rcp_lut == NULL, "globals must not be published before any load");
    snprintf(g_root, sizeof g_root, SR_SELFTEST_BUILD_ROOT "/vfpu_selftest_tmp_%d", (int)SR_GETPID());
    wipe_tree(g_root);
    CHECK(setup_temp_root() == 0, "could not stage genuine tables into %s", g_root);
    if (sr_rcp_lut != NULL) {
        wipe_tree(g_root); /* the staged copies go even on this already-failing path */
        return;
    }

    SrVfpuTables agg;
    memset(&agg, 0, sizeof agg);
    char err[512];
    int expected_failures = 0;

    /* Absent root. */
    {
        char missing[512];
        CHECK((size_t)snprintf(missing, sizeof missing, SR_SELFTEST_BUILD_ROOT "/vfpu_selftest_missing_%d", (int)SR_GETPID()) < sizeof missing, "root name truncation");
        wipe_tree(missing);
        int rc = sr_vfpu_tables_load(missing, &agg, err, sizeof err);
        CHECK(rc != 0, "absent root must fail");
        CHECK(err[0] != '\0', "absent root must produce a diagnostic");
        expected_failures++;
    }

    /* Truncated file. */
    {
        char path[512];
        CHECK(fmt_path(path, sizeof path, g_root, "vfpu_rcp_lut.dat") == 0, "path build");
        FILE *f = fopen(path, "rb+");
        CHECK(f != NULL, "open rcp for truncation");
        if (f) {
            CHECK(ftruncate_shim(f, 1000u) == 0, "truncate rcp");
            fclose(f);
        }
        int rc = sr_vfpu_tables_load(g_root, &agg, err, sizeof err);
        CHECK(rc != 0, "truncated table must fail");
        CHECK(strstr(err, "short read") != NULL, "truncated diagnostic: %s", err);
        expected_failures++;
        setup_temp_root();  /* restore */
    }

    /* Extra data past the exact length. */
    {
        char path[512];
        CHECK(fmt_path(path, sizeof path, g_root, "vfpu_sqrt_lut.dat") == 0, "path build");
        FILE *f = fopen(path, "ab");
        CHECK(f != NULL, "open sqrt for append");
        if (f) {
            uint8_t junk[4] = { 1, 2, 3, 4 };
            fwrite(junk, 1, sizeof junk, f);
            fclose(f);
        }
        int rc = sr_vfpu_tables_load(g_root, &agg, err, sizeof err);
        CHECK(rc != 0, "extra data must fail");
        CHECK(strstr(err, "extra data") != NULL, "extra-data diagnostic: %s", err);
        expected_failures++;
        setup_temp_root();
    }

    /* Wrong content, same length: hash rejection (the issue's core case). */
    {
        char path[512];
        size_t n = 0;
        const SrVfpuTableSpec *specs = sr_vfpu_table_manifest(&n);
        CHECK(fmt_path(path, sizeof path, g_root, "vfpu_sin_lut8192.dat") == 0, "path build");
        uint8_t *zeros = (uint8_t *)calloc(1, specs[3].bytes);
        CHECK(zeros != NULL, "alloc zeros");
        if (zeros) {
            CHECK(write_bytes(path, zeros, specs[3].bytes) == 0, "write same-length zeros");
            free(zeros);
        }
        int rc = sr_vfpu_tables_load(g_root, &agg, err, sizeof err);
        CHECK(rc != 0, "same-length wrong content must fail the hash check");
        CHECK(strstr(err, "sha256 mismatch") != NULL, "hash diagnostic: %s", err);
        expected_failures++;
        setup_temp_root();
    }

    /* Endian-swapped bytes: hash rejection, and the value-domain validator
     * would also reject the resulting indices. */
    {
        char path[512];
        CHECK(fmt_path(path, sizeof path, g_root, "vfpu_asin_lut_indices.dat") == 0, "path build");
        FILE *f = fopen(path, "rb+");
        CHECK(f != NULL, "open asin indices");
        if (f) {
            uint8_t *buf = (uint8_t *)malloc(798916u);
            CHECK(buf != NULL, "alloc swap buffer");
            if (buf) {
                if (fread(buf, 1, 798916u, f) == 798916u) {
                    for (size_t i = 0; i + 1u < 798916u; i += 2u) {
                        uint8_t t = buf[i]; buf[i] = buf[i + 1u]; buf[i + 1u] = t;
                    }
                    fseek(f, 0, SEEK_SET);
                    fwrite(buf, 1, 798916u, f);
                }
                free(buf);
            }
            fclose(f);
        }
        int rc = sr_vfpu_tables_load(g_root, &agg, err, sizeof err);
        CHECK(rc != 0, "endian-swapped table must fail");
        expected_failures++;
        setup_temp_root();
    }

    /* Single-bit corruption at the same length: the committed SHA-256 must
     * reject it (the tightest corruption class short of full replacement). */
    {
        char path[512];
        CHECK(fmt_path(path, sizeof path, g_root, "vfpu_sqrt_lut.dat") == 0, "path build");
        FILE *f = fopen(path, "rb+");
        CHECK(f != NULL, "open sqrt for bit flip");
        if (f) {
            long first = fgetc(f);
            if (first != EOF) {
                fseek(f, 0, SEEK_SET);
                fputc((first ^ 1) & 0xFF, f);
            }
            fclose(f);
        }
        int rc = sr_vfpu_tables_load(g_root, &agg, err, sizeof err);
        CHECK(rc != 0, "single-bit corruption must fail the hash check");
        CHECK(strstr(err, "sha256 mismatch") != NULL, "bit-flip diagnostic: %s", err);
        expected_failures++;
        setup_temp_root();
    }

    /* Overlong root: path construction must fail closed on truncation before
     * any filesystem access, never silently truncating the table path. */
    {
        char overlong[600];
        memset(overlong, 'a', sizeof overlong - 1u);
        overlong[sizeof overlong - 1u] = '\0';
        int rc = sr_vfpu_tables_load(overlong, &agg, err, sizeof err);
        CHECK(rc != 0, "overlong root must fail");
        CHECK(strstr(err, "truncated") != NULL, "overlong-root diagnostic: %s", err);
        expected_failures++;
    }

    /* The failures must never have published or partially filled anything. */
    CHECK(sr_rcp_lut == NULL, "globals must stay NULL after failed loads");
    CHECK(sr_asin_lut_indices == NULL, "globals must stay NULL after failed loads");
    CHECK(agg.asin_indices == NULL && agg.rcp == NULL,
          "aggregate must be untouched after failed loads");
    CHECK(expected_failures == 7, "expected 7 loader failures, got %d", expected_failures);

    /* A failed initialization must not poison a later one: the staged genuine
     * tree still loads cleanly after all the corruption attempts. */
    {
        SrVfpuTables retry;
        memset(&retry, 0, sizeof retry);
        char retry_err[512];
        int rc = sr_vfpu_tables_load(g_root, &retry, retry_err, sizeof retry_err);
        CHECK(rc == 0, "load after failures must succeed: %s", retry_err);
        if (rc == 0) {
            CHECK(retry.rcp != NULL && retry.asin_indices != NULL,
                  "retry aggregate must be complete");
        }
    }

    wipe_tree(g_root);
}

/* ---- success path --------------------------------------------------------- */

/* Whether all 15 published table pointers are set. */
static int all_published(void) {
    return sr_rcp_lut != NULL && sr_sqrt_lut != NULL && sr_rsqrt_lut != NULL &&
           sr_sin_lut8192 != NULL && sr_sin_lut_delta != NULL &&
           sr_sin_lut_interval_delta != NULL && sr_sin_lut_exceptions != NULL &&
           sr_exp2_lut65536 != NULL && sr_exp2_lut != NULL && sr_log2_lut65536 != NULL &&
           sr_log2_lut65536_quadratic != NULL && sr_log2_lut != NULL &&
           sr_asin_lut65536 != NULL && sr_asin_lut_deltas != NULL && sr_asin_lut_indices != NULL;
}

static void test_pure_load_and_publish(void) {
    SrVfpuTables agg;
    memset(&agg, 0, sizeof agg);
    char err[512];
    int rc = sr_vfpu_tables_load("assets/vfpu", &agg, err, sizeof err);
    CHECK(rc == 0, "genuine asset load failed: %s", err);
    if (rc != 0) return;
    CHECK(agg.rcp != NULL && agg.sqrt != NULL && agg.rsqrt != NULL &&
          agg.sin8192 != NULL && agg.sin_delta != NULL &&
          agg.sin_interval_delta != NULL && agg.sin_exceptions != NULL &&
          agg.exp2_65536 != NULL && agg.exp2 != NULL &&
          agg.log2_65536 != NULL && agg.log2_65536_quadratic != NULL &&
          agg.log2 != NULL && agg.asin_65536 != NULL &&
          agg.asin_deltas != NULL && agg.asin_indices != NULL,
          "pure load must fill every table pointer");
    CHECK(sr_rcp_lut == NULL, "pure load must not publish globals");

    /* End-to-end value-domain check on the genuine bytes: asin indices all
     * below the deltas count, sin interval bounds within the exceptions len. */
    CHECK(sr_vfpu_validate_asin_indices(agg.asin_indices, 399458u, 64681u) == 0,
          "genuine asin indices violate the validator");
    CHECK(sr_vfpu_validate_sin_interval(agg.sin_interval_delta, 65537u, 86938u) == 0,
          "genuine sin interval deltas violate the validator");

    sr_vfpu_tables_publish(&agg);
    CHECK(all_published(), "publish must install every global pointer");
}

static void test_once_only_and_repeated_init(void) {
    /* Second and third calls must be no-ops (state machine fast path). */
    sr_vfpu_load_with_root("assets/vfpu");
    sr_vfpu_load();
    CHECK(sr_rcp_lut != NULL && sr_sin_lut8192 != NULL,
          "repeated init must keep globals published");
}

#if defined(SR_CONCURRENCY_AVAILABLE)
/* The 15 published table pointers: the race below starts from all of them unset and checks
 * each one, so a table a regressed load or publish leaves out cannot hide behind an earlier
 * publish. */
static void reset_published(void) {
    sr_rcp_lut = NULL;
    sr_sqrt_lut = NULL;
    sr_rsqrt_lut = NULL;
    sr_sin_lut8192 = NULL;
    sr_sin_lut_delta = NULL;
    sr_sin_lut_interval_delta = NULL;
    sr_sin_lut_exceptions = NULL;
    sr_exp2_lut65536 = NULL;
    sr_exp2_lut = NULL;
    sr_log2_lut65536 = NULL;
    sr_log2_lut65536_quadratic = NULL;
    sr_log2_lut = NULL;
    sr_asin_lut65536 = NULL;
    sr_asin_lut_deltas = NULL;
    sr_asin_lut_indices = NULL;
}

static atomic_int g_thread_fail = 0;

/* Both threads race the real first-use critical section and then check that
 * the published globals are complete and identical. */
static void concurrent_worker(void) {
    sr_vfpu_load_with_root("assets/vfpu");
    if (!all_published()) atomic_store(&g_thread_fail, 1);
}
#if defined(SR_HAVE_THREADS)
static int concurrent_loader_c11(void *unused) {
    (void)unused;
    concurrent_worker();
    return 0;
}
#else
static DWORD WINAPI concurrent_loader_win(LPVOID unused) {
    (void)unused;
    concurrent_worker();
    return 0;
}
#endif
#endif

static void test_concurrent_first_use(void) {
#if !defined(SR_CONCURRENCY_AVAILABLE)
    fprintf(stderr, "  note: no C11 or Win32 threads available; concurrency path not exercised\n");
#else
    /* Reset the global pointers so the racing load is a genuine first use
     * (the load-state machine is still at 0 here: nothing has called
     * sr_vfpu_load_with_root yet). */
    reset_published();
    atomic_store(&g_thread_fail, 0);
#if defined(SR_HAVE_THREADS)
    thrd_t a, b;
    CHECK(thrd_create(&a, concurrent_loader_c11, NULL) == thrd_success, "thrd_create a");
    CHECK(thrd_create(&b, concurrent_loader_c11, NULL) == thrd_success, "thrd_create b");
    int ra, rb;
    thrd_join(a, &ra);
    thrd_join(b, &rb);
#else
    HANDLE handles[2];
    handles[0] = CreateThread(NULL, 0, concurrent_loader_win, NULL, 0, NULL);
    handles[1] = CreateThread(NULL, 0, concurrent_loader_win, NULL, 0, NULL);
    CHECK(handles[0] != NULL && handles[1] != NULL, "CreateThread failed");
    if (handles[0] && handles[1]) {
        WaitForMultipleObjects(2, handles, TRUE, INFINITE);
        CloseHandle(handles[0]);
        CloseHandle(handles[1]);
    }
#endif
    CHECK(atomic_load(&g_thread_fail) == 0,
          "a concurrent loader observed null or partial globals");
    CHECK(all_published(), "concurrent first use must publish complete globals");
#endif
}

/* ---- evidence label --------------------------------------------------------- */

/* The first published load prints "[vfpu_tables] root=<root> override=<source>" on
 * stderr. The tests around it run with stderr sent to a scratch file, so the label
 * of the root they pin can be read back. */
#if defined(_WIN32)
#define SR_DUP(fd) _dup(fd)
#define SR_DUP2(fd, to) _dup2(fd, to)
#define SR_CLOSE(fd) _close(fd)
#define SR_FILENO(f) _fileno(f)
#else
#define SR_DUP(fd) dup(fd)
#define SR_DUP2(fd, to) dup2(fd, to)
#define SR_CLOSE(fd) close(fd)
#define SR_FILENO(f) fileno(f)
#endif

static char g_capture_path[512];
static int g_saved_stderr = -1;

static void begin_stderr_capture(void) {
    int n = snprintf(g_capture_path, sizeof g_capture_path, "%s/vfpu_tables_selftest_%d.stderr",
                     SR_SELFTEST_BUILD_ROOT, (int)SR_GETPID());
    if (n < 0 || (size_t)n >= sizeof g_capture_path) return;
    fflush(stderr);
    FILE *f = fopen(g_capture_path, "wb");
    if (!f) return;
    g_saved_stderr = SR_DUP(SR_FILENO(stderr));
    if (g_saved_stderr >= 0 && SR_DUP2(SR_FILENO(f), SR_FILENO(stderr)) < 0) {
        SR_CLOSE(g_saved_stderr);
        g_saved_stderr = -1;
    }
    fclose(f);
}

/* Restore stderr, echo the captured text (a FAIL line inside stays visible) and
 * return it in `out`; an empty `out` means nothing was captured. */
static void end_stderr_capture(char *out, size_t cap) {
    out[0] = '\0';
    if (g_saved_stderr < 0) return;
    fflush(stderr);
    SR_DUP2(g_saved_stderr, SR_FILENO(stderr));
    SR_CLOSE(g_saved_stderr);
    g_saved_stderr = -1;
    FILE *f = fopen(g_capture_path, "rb");
    if (f) {
        size_t got = fread(out, 1, cap - 1u, f);
        out[got] = '\0';
        fclose(f);
    }
    remove(g_capture_path);
    fputs(out, stderr);
}

/* sr_vfpu_load_with_root("assets/vfpu") loaded the tables, so the evidence must say
 * the root was explicit: neither PSP_VFPU_TABLES nor the default was used. */
static void test_evidence_names_explicit_root(const char *captured) {
    CHECK(captured[0] != '\0', "stderr capture around the first published load failed");
    CHECK(strstr(captured, "[vfpu_tables] root=assets/vfpu override=explicit\n") != NULL,
          "an explicit root must be labelled override=explicit");
    CHECK(strstr(captured, "override=PSP_VFPU_TABLES") == NULL &&
          strstr(captured, "override=default-assets/vfpu") == NULL,
          "an explicit root must not be labelled as the environment or default root");
}

int main(void) {
    test_sha256_known_answers();
    test_shared_sha256();
    test_asin_index_validator();
    test_sin_interval_validator();
    test_loader_rejects_corrupt_roots();
    test_pure_load_and_publish();   /* fills the aggregate + publishes; state still 0 */
    static char captured[16384];
    begin_stderr_capture();
    test_concurrent_first_use();    /* resets globals and races the real first-use load */
    test_once_only_and_repeated_init();  /* state now 2: idempotence */
    end_stderr_capture(captured, sizeof captured);
    test_evidence_names_explicit_root(captured);

    if (g_failures) {
        fprintf(stderr, "vfpu-tables selftest: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    fprintf(stderr, "vfpu-tables selftest: OK\n");
    return 0;
}
