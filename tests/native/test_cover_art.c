/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Unit tests for opt-in libretro cover art (src/player/cover_art.c, #2632).
 * All HTTP exchanges use an in-memory fake transport; no test here ever
 * touches the internet (AGENTS.md section 5). */

#if !defined(_WIN32) && !defined(_WIN64)
#define _POSIX_C_SOURCE 200809L
#endif

#include "cover_art.h"
#include "nk_disc_verify.h"
#include "nk_platform.h"
#include "native_test_isolation.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *url;
    const unsigned char *body;
    size_t size;
    bool reachable;
    bool has_content_length;
    uint64_t declared_length;
    bool fail_read;
    const char *final_url;   /* NULL: the request URL (no redirect) */
    bool no_final_url;       /* a transport that reports no final URL */
} FakeAnswer;

typedef struct {
    FakeAnswer answers[8];
    size_t answer_count;
    char requested[16][NK_COVER_ART_URL_MAX];
    char hosts[16][256];
    size_t request_count;
    const FakeAnswer *open;
    size_t offset;
    int closes;
    int reads;
    int cancel_after_reads;
} FakeNet;

static bool fake_read(void *context, unsigned char *buffer, size_t capacity, size_t *read) {
    FakeNet *net = (FakeNet *)context;
    net->reads++;
    if (net->open->fail_read) return false;
    size_t left = net->open->size - net->offset;
    size_t count = left < capacity ? left : capacity;
    if (count > 0u) memcpy(buffer, net->open->body + net->offset, count);
    net->offset += count;
    *read = count;
    return true;
}

static void fake_close(void *context) {
    ((FakeNet *)context)->closes++;
}

static bool fake_open(void *context, const char *url, const char *allowed_hosts,
                      PackageHttpResponse *response) {
    FakeNet *net = (FakeNet *)context;
    assert(net->request_count < 16u);
    snprintf(net->requested[net->request_count], sizeof(net->requested[0]), "%s", url ? url : "");
    snprintf(net->hosts[net->request_count], sizeof(net->hosts[0]), "%s",
             allowed_hosts ? allowed_hosts : "");
    net->request_count++;
    for (size_t i = 0u; i < net->answer_count; i++) {
        const FakeAnswer *answer = &net->answers[i];
        if (strcmp(answer->url, url) != 0) continue;
        if (!answer->reachable) return false;
        net->open = answer;
        net->offset = 0u;
        memset(response, 0, sizeof(*response));
        response->final_url = answer->no_final_url ? NULL
                              : answer->final_url ? answer->final_url : answer->url;
        response->has_content_length = answer->has_content_length;
        response->content_length = answer->declared_length;
        response->context = net;
        response->read = fake_read;
        response->close = fake_close;
        return true;
    }
    return false;
}

static void add_answer(FakeNet *net, const char *url, const void *body, size_t size) {
    FakeAnswer *answer = &net->answers[net->answer_count++];
    memset(answer, 0, sizeof(*answer));
    answer->url = url;
    answer->body = (const unsigned char *)body;
    answer->size = size;
    answer->reachable = true;
    answer->has_content_length = true;
    answer->declared_length = size;
}

static bool cancel_after_first_read(void *ctx) {
    FakeNet *net = (FakeNet *)ctx;
    return net->cancel_after_reads > 0 && net->reads >= net->cancel_after_reads;
}

typedef struct {
    bool called;
    char disc_id[32];
    char sha1[64];
    char crc32[16];
    const char *url;
    const char *hosts;
} FakeFallbackState;

static bool fake_ra_fallback(void *context,
                             const char *disc_id,
                             const char *sha1_hex,
                             const char *crc32_hex,
                             char *out_url, size_t out_url_size,
                             char *out_allowed_hosts, size_t out_hosts_size) {
    FakeFallbackState *st = (FakeFallbackState *)context;
    st->called = true;
    snprintf(st->disc_id, sizeof(st->disc_id), "%s", disc_id ? disc_id : "");
    snprintf(st->sha1, sizeof(st->sha1), "%s", sha1_hex ? sha1_hex : "");
    snprintf(st->crc32, sizeof(st->crc32), "%s", crc32_hex ? crc32_hex : "");
    if (!st->url || !st->hosts) return false;
    snprintf(out_url, out_url_size, "%s", st->url);
    snprintf(out_allowed_hosts, out_hosts_size, "%s", st->hosts);
    return true;
}

static bool file_exists(const char *path) {
    return nk_platform_file_exists(path);
}

static void test_sanitization_and_urls(void) {
    char out[256];
    char url[NK_COVER_ART_URL_MAX];

    /* Every libretro special character `& * / : < > ? \ | "` maps to `_`. */
    assert(nk_cover_art_sanitize_name("A&B*C/D:E<F>G?H\\I|J\"K", out, sizeof(out)));
    assert(strcmp(out, "A_B_C_D_E_F_G_H_I_J_K") == 0);

    assert(nk_cover_art_sanitize_name(
        "Ratchet & Clank - Size Matters (USA) (En,Fr,De,Es,It)", out, sizeof(out)));
    assert(strcmp(out, "Ratchet _ Clank - Size Matters (USA) (En,Fr,De,Es,It)") == 0);

    /* Valid non-ASCII UTF-8 preserved; controls, invalid UTF-8, empty and tight buffer rejected. */
    assert(nk_cover_art_sanitize_name("Pok\xC3\xA9mon (Japan)", out, sizeof(out)));
    assert(strcmp(out, "Pok\xC3\xA9mon (Japan)") == 0);
    assert(!nk_cover_art_sanitize_name(NULL, out, sizeof(out)));
    assert(!nk_cover_art_sanitize_name("", out, sizeof(out)));
    assert(!nk_cover_art_sanitize_name("Bad\nName", out, sizeof(out)));
    assert(!nk_cover_art_sanitize_name("Bad\x7FName", out, sizeof(out)));
    assert(!nk_cover_art_sanitize_name("Bad\xFFName", out, sizeof(out)));
    assert(!nk_cover_art_sanitize_name("Four", out, 4u));

    /* RFC 3986 encoding over the libretro Named_Boxarts base URL. */
    assert(nk_cover_art_boxart_url(
        "Daxter (USA) (En,Fr,De,Es,It)", url, sizeof(url)));
    assert(strcmp(url,
                  "https://thumbnails.libretro.com/Sony%20-%20PlayStation%20Portable/"
                  "Named_Boxarts/Daxter%20%28USA%29%20%28En%2CFr%2CDe%2CEs%2CIt%29.png") == 0);

    assert(nk_cover_art_boxart_url(
        "Ratchet & Clank: 'Test' ~ One_Two.v1", url, sizeof(url)));
    assert(strcmp(url,
                  "https://thumbnails.libretro.com/Sony%20-%20PlayStation%20Portable/"
                  "Named_Boxarts/Ratchet%20_%20Clank_%20%27Test%27%20~%20One_Two.v1.png") == 0);

    assert(!nk_cover_art_boxart_url("Abc", url, 16u));
    printf("[COVER_ART_TEST] sanitization_and_urls: PASS\n");
}

static void test_png_validation_and_fetch(void) {
    /* 512 x 885 PNG header (typical libretro PSP boxart dimensions exceed 512 px height). */
    static const uint8_t kValidPng[33] = {
        0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au,
        0u, 0u, 0u, 13u, 'I', 'H', 'D', 'R',
        0u, 0u, 0x02u, 0x00u, /* width = 512 */
        0u, 0u, 0x03u, 0x75u, /* height = 885 */
        8u, 6u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
    };
    static const uint8_t kHugePng[33] = {
        0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au,
        0u, 0u, 0u, 13u, 'I', 'H', 'D', 'R',
        0u, 0u, 0x08u, 0x01u, /* width = 2049 (> NK_COVER_ART_MAX_SIDE) */
        0u, 0u, 0x01u, 0x00u,
        8u, 6u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
    };
    static const uint8_t kNotPng[] = "<html>404 Not Found</html>";

    uint32_t w = 0u, h = 0u;
    assert(nk_cover_art_validate_png_header(kValidPng, sizeof(kValidPng),
                                            NK_COVER_ART_MAX_SIDE, &w, &h));
    assert(w == 512u && h == 885u);
    assert(!nk_cover_art_validate_png_header(kHugePng, sizeof(kHugePng),
                                             NK_COVER_ART_MAX_SIDE, &w, &h));
    assert(!nk_cover_art_validate_png_header(kNotPng, sizeof(kNotPng),
                                             NK_COVER_ART_MAX_SIDE, &w, &h));

    static const char *const kDatXml =
        "<?xml version=\"1.0\"?>\n"
        "<datafile>\n"
        "  <header><name>Synthetic Redump</name><version>1</version></header>\n"
        "  <game name=\"Daxter &amp; Friends &#39;Test&#39;: Edition (USA)\">\n"
        "    <serial>UCUS-98618</serial>\n"
        "    <rom name=\"daxter.iso\" size=\"2048\" crc=\"aabbccdd\" "
        "sha1=\"0123456789abcdef0123456789abcdef01234567\"/>\n"
        "  </game>\n"
        "  <game name=\"Missing Boxart (USA)\">\n"
        "    <serial>ULUS-99998</serial>\n"
        "    <rom name=\"missing.iso\" size=\"2048\" crc=\"11223344\" "
        "sha1=\"1111111111111111111111111111111111111111\"/>\n"
        "  </game>\n"
        "</datafile>\n";
    NkDiscDat dat;
    NkDiscExpandStatus dat_status;
    assert(nk_disc_dat_parse(kDatXml, strlen(kDatXml), &dat, &dat_status));

    char data_root[NK_MAX_PATH];
    assert(nk_platform_resolve_path(NK_PATH_DATA, data_root, sizeof(data_root)));
    uint64_t bytes = 999u;
    char size_label[32];
    /* Non-existent cache reports 0 bytes without error. */
    assert(nk_cover_art_cache_size(data_root, &bytes) && bytes == 0u);
    nk_cover_art_format_size(bytes, size_label, sizeof(size_label));
    assert(strcmp(size_label, "0 KB") == 0);
    nk_cover_art_format_size(1468006u, size_label, sizeof(size_label));
    assert(strcmp(size_label, "1.4 MB") == 0);

    FakeNet net;
    memset(&net, 0, sizeof(net));
    char expected_url[NK_COVER_ART_URL_MAX];
    assert(nk_cover_art_boxart_url("Daxter & Friends 'Test': Edition (USA)",
                                   expected_url, sizeof(expected_url)));
    add_answer(&net, expected_url, kValidPng, sizeof(kValidPng));

    NkCoverArtSource source = NK_COVER_ART_SOURCE_NONE;
    /* Unmatched disc makes zero HTTP calls. */
    assert(nk_cover_art_fetch_for_disc(
               fake_open, &net, &dat, data_root, "ULUS99999",
               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "deadbeef",
               NULL, NULL, NULL, NULL, &source) == NK_COVER_ART_NO_MATCH);
    assert(net.request_count == 0u);
    assert(source == NK_COVER_ART_SOURCE_NONE);

    /* Matched disc downloads once from thumbnails.libretro.com and stores <lowercase_sha1>.png. */
    assert(nk_cover_art_fetch_for_disc(
               fake_open, &net, &dat, data_root, "UCUS98618",
               "0123456789ABCDEF0123456789ABCDEF01234567", "AABBCCDD",
               NULL, NULL, NULL, NULL, &source) == NK_COVER_ART_OK);
    assert(source == NK_COVER_ART_SOURCE_LIBRETRO);
    assert(net.request_count == 1u);
    assert(strcmp(net.requested[0], expected_url) == 0);
    assert(strcmp(net.hosts[0], "thumbnails.libretro.com") == 0);

    char cache_file[NK_MAX_PATH];
    assert(nk_cover_art_cache_path(data_root, "0123456789ABCDEF0123456789ABCDEF01234567",
                                   cache_file, sizeof(cache_file)));
    assert(strstr(cache_file, "0123456789abcdef0123456789abcdef01234567.png") != NULL);
    assert(file_exists(cache_file));
    assert(nk_cover_art_cache_size(data_root, &bytes) && bytes == sizeof(kValidPng));

    /* Missing (404), non-PNG, oversize PNG/content-length, and cancellation never write a cache file. */
    char bad_file[NK_MAX_PATH];
    assert(nk_cover_art_cache_path(data_root, "1111111111111111111111111111111111111111",
                                   bad_file, sizeof(bad_file)));
    assert(nk_cover_art_fetch_for_disc(
               fake_open, &net, &dat, data_root, "ULUS99998",
               "1111111111111111111111111111111111111111", "11223344",
               NULL, NULL, NULL, NULL, &source) == NK_COVER_ART_NOT_FOUND);
    assert(!file_exists(bad_file));

    memset(&net, 0, sizeof(net));
    add_answer(&net, "https://thumbnails.libretro.com/Sony%20-%20PlayStation%20Portable/Named_Boxarts/NonPng.png",
               kNotPng, sizeof(kNotPng));
    assert(nk_cover_art_fetch_url(fake_open, &net, net.answers[0].url, "thumbnails.libretro.com",
                                  bad_file, NULL, NULL, NULL) == NK_COVER_ART_INVALID_PNG);
    assert(!file_exists(bad_file));

    memset(&net, 0, sizeof(net));
    add_answer(&net, "https://thumbnails.libretro.com/Sony%20-%20PlayStation%20Portable/Named_Boxarts/Huge.png",
               kHugePng, sizeof(kHugePng));
    assert(nk_cover_art_fetch_url(fake_open, &net, net.answers[0].url, "thumbnails.libretro.com",
                                  bad_file, NULL, NULL, NULL) == NK_COVER_ART_INVALID_PNG);

    memset(&net, 0, sizeof(net));
    add_answer(&net, "https://thumbnails.libretro.com/Sony%20-%20PlayStation%20Portable/Named_Boxarts/Big.png",
               kValidPng, sizeof(kValidPng));
    net.answers[0].declared_length = NK_COVER_ART_MAX_BYTES + 1u;
    assert(nk_cover_art_fetch_url(fake_open, &net, net.answers[0].url, "thumbnails.libretro.com",
                                  bad_file, NULL, NULL, NULL) == NK_COVER_ART_TOO_LARGE);

    /* Audit 2026-11-17: a connection that ends early. The server declared 4 KiB more than
       arrived and the read then reported a clean end. Failing before: the 33 bytes passed
       the header check and became the permanent cache entry. */
    memset(&net, 0, sizeof(net));
    add_answer(&net, "https://thumbnails.libretro.com/Sony%20-%20PlayStation%20Portable/Named_Boxarts/Short.png",
               kValidPng, sizeof(kValidPng));
    net.answers[0].declared_length = sizeof(kValidPng) + 4096u;
    assert(nk_cover_art_fetch_url(fake_open, &net, net.answers[0].url, "thumbnails.libretro.com",
                                  bad_file, NULL, NULL, NULL) == NK_COVER_ART_NOT_FOUND);
    assert(!file_exists(bad_file));
    assert(net.closes == 1);

    memset(&net, 0, sizeof(net));
    add_answer(&net, expected_url, kValidPng, sizeof(kValidPng));
    net.cancel_after_reads = 1;
    assert(nk_cover_art_fetch_url(fake_open, &net, expected_url, "thumbnails.libretro.com",
                                  bad_file, cancel_after_first_read, &net, NULL) == NK_COVER_ART_CANCELLED);

    /* RetroAchievements fallback seam: invoked when primary libretro has no match. */
    FakeFallbackState ra;
    memset(&ra, 0, sizeof(ra));
    ra.url = "http://insecure.retroachievements.org/bad.png";
    ra.hosts = "retroachievements.org";
    NkCoverArtProviders providers = {fake_ra_fallback, &ra};

    memset(&net, 0, sizeof(net));
    assert(nk_cover_art_fetch_for_disc(
               fake_open, &net, &dat, data_root, "ULUS99998",
               "1111111111111111111111111111111111111111", "11223344",
               &providers, NULL, NULL, NULL, &source) == NK_COVER_ART_NOT_FOUND);
    assert(ra.called);
    assert(strcmp(ra.disc_id, "ULUS99998") == 0);
    assert(net.request_count == 1u); /* Primary libretro tried once; HTTP fallback refused. */

    ra.called = false;
    ra.url = "https://media.retroachievements.org/Images/099999.png";
    ra.hosts = "media.retroachievements.org";
    add_answer(&net, ra.url, kValidPng, sizeof(kValidPng));
    assert(nk_cover_art_fetch_for_disc(
               fake_open, &net, &dat, data_root, "ULUS99998",
               "1111111111111111111111111111111111111111", "11223344",
               &providers, NULL, NULL, NULL, &source) == NK_COVER_ART_OK);
    assert(source == NK_COVER_ART_SOURCE_RETROACHIEVEMENTS);
    assert(file_exists(bad_file));

    /* Clear cache removes both cached PNG files and brings cache size back to 0. */
    char err_code[48];
    char err_msg[256];
    assert(nk_cover_art_clear_cache(data_root, err_code, sizeof(err_code), err_msg, sizeof(err_msg)));
    assert(!file_exists(cache_file));
    assert(!file_exists(bad_file));
    assert(nk_cover_art_cache_size(data_root, &bytes) && bytes == 0u);

    nk_disc_dat_free(&dat);
    printf("[COVER_ART_TEST] png_validation_and_fetch: PASS\n");
}

/* A cache lock that records how it is used; cancel_on_lock stands for a deleter
   that cancelled the fetch while the fetch waited for the lock. */
typedef struct {
    int locks;
    int unlocks;
    bool held;
    bool cancel_on_lock;
    bool cancelled;
    int checks_while_held;
} FakeCacheLock;

static void fake_cache_lock(void *context) {
    FakeCacheLock *lock = (FakeCacheLock *)context;
    assert(!lock->held);
    lock->locks++;
    lock->held = true;
    if (lock->cancel_on_lock) lock->cancelled = true;
}

static void fake_cache_unlock(void *context) {
    FakeCacheLock *lock = (FakeCacheLock *)context;
    assert(lock->held);
    lock->unlocks++;
    lock->held = false;
}

static bool fake_lock_cancelled(void *context) {
    FakeCacheLock *lock = (FakeCacheLock *)context;
    if (lock->held) lock->checks_while_held++;
    return lock->cancelled;
}

static const uint8_t kTinyPng[33] = {
    0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au,
    0u, 0u, 0u, 13u, 'I', 'H', 'D', 'R',
    0u, 0u, 0u, 2u, 0u, 0u, 0u, 2u,
    8u, 6u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
};

#define TEST_BOXART_URL \
    "https://thumbnails.libretro.com/Sony%20-%20PlayStation%20Portable/Named_Boxarts/Tiny.png"

/* The response's final URL (after redirects) must be on the allowlist too. */
static void test_redirect_refused(void) {
    char data_root[NK_MAX_PATH];
    char dest[NK_MAX_PATH];
    FakeNet net;
    assert(nk_platform_resolve_path(NK_PATH_DATA, data_root, sizeof(data_root)));
    assert(nk_cover_art_cache_path(data_root, "2222222222222222222222222222222222222222",
                                   dest, sizeof(dest)));

    memset(&net, 0, sizeof(net));
    add_answer(&net, TEST_BOXART_URL, kTinyPng, sizeof(kTinyPng));
    net.answers[0].final_url = "https://evil.example.com/Tiny.png";
    assert(nk_cover_art_fetch_url(fake_open, &net, TEST_BOXART_URL, NK_COVER_ART_HOST,
                                  dest, NULL, NULL, NULL) == NK_COVER_ART_REDIRECT_REFUSED);
    assert(net.reads == 0 && net.closes == 1);
    assert(!file_exists(dest));

    /* A look-alike host and an http:// final URL are refused the same way. */
    memset(&net, 0, sizeof(net));
    add_answer(&net, TEST_BOXART_URL, kTinyPng, sizeof(kTinyPng));
    net.answers[0].final_url = "https://thumbnails.libretro.com.example.com/Tiny.png";
    assert(nk_cover_art_fetch_url(fake_open, &net, TEST_BOXART_URL, NK_COVER_ART_HOST,
                                  dest, NULL, NULL, NULL) == NK_COVER_ART_REDIRECT_REFUSED);
    net.answers[0].final_url = "http://thumbnails.libretro.com/Tiny.png";
    assert(nk_cover_art_fetch_url(fake_open, &net, TEST_BOXART_URL, NK_COVER_ART_HOST,
                                  dest, NULL, NULL, NULL) == NK_COVER_ART_REDIRECT_REFUSED);
    assert(!file_exists(dest));

    /* No final URL at all is refused, not assumed to be the request URL. */
    memset(&net, 0, sizeof(net));
    add_answer(&net, TEST_BOXART_URL, kTinyPng, sizeof(kTinyPng));
    net.answers[0].no_final_url = true;
    assert(nk_cover_art_fetch_url(fake_open, &net, TEST_BOXART_URL, NK_COVER_ART_HOST,
                                  dest, NULL, NULL, NULL) == NK_COVER_ART_REDIRECT_REFUSED);
    assert(net.closes == 1);
    assert(!file_exists(dest));

    /* A redirect that stays on the allowed host (any case) is accepted. */
    memset(&net, 0, sizeof(net));
    add_answer(&net, TEST_BOXART_URL, kTinyPng, sizeof(kTinyPng));
    net.answers[0].final_url = "https://THUMBNAILS.libretro.com/mirror/Tiny.png";
    assert(nk_cover_art_fetch_url(fake_open, &net, TEST_BOXART_URL, NK_COVER_ART_HOST,
                                  dest, NULL, NULL, NULL) == NK_COVER_ART_OK);
    assert(file_exists(dest));
    assert(strcmp(nk_cover_art_status_name(NK_COVER_ART_REDIRECT_REFUSED),
                  "REDIRECT_REFUSED") == 0);
    assert(nk_remove_utf8(dest) == 0);
    printf("[COVER_ART_TEST] redirect_refused: PASS\n");
}

/* A fetch writes only while it holds the cache lock and checks its cancel flag
   again under it, so a deleter that cancels and then takes the lock never sees
   a late write (the player's Refresh, setting off and factory reset). */
static void test_cache_lock_and_late_cancel(void) {
    char data_root[NK_MAX_PATH];
    char dest[NK_MAX_PATH];
    FakeNet net;
    FakeCacheLock state;
    NkCoverArtCacheLock lock = {fake_cache_lock, fake_cache_unlock, &state};
    assert(nk_platform_resolve_path(NK_PATH_DATA, data_root, sizeof(data_root)));
    assert(nk_cover_art_cache_path(data_root, "3333333333333333333333333333333333333333",
                                   dest, sizeof(dest)));

    /* Cancelled while it waited for the lock: nothing is written, lock balanced. */
    memset(&net, 0, sizeof(net));
    memset(&state, 0, sizeof(state));
    state.cancel_on_lock = true;
    add_answer(&net, TEST_BOXART_URL, kTinyPng, sizeof(kTinyPng));
    assert(nk_cover_art_fetch_url(fake_open, &net, TEST_BOXART_URL, NK_COVER_ART_HOST, dest,
                                  fake_lock_cancelled, &state, &lock) == NK_COVER_ART_CANCELLED);
    assert(state.locks == 1 && state.unlocks == 1 && !state.held);
    assert(state.checks_while_held == 1);
    assert(!file_exists(dest));

    /* Not cancelled: written under the lock, which is released afterwards. */
    memset(&net, 0, sizeof(net));
    memset(&state, 0, sizeof(state));
    add_answer(&net, TEST_BOXART_URL, kTinyPng, sizeof(kTinyPng));
    assert(nk_cover_art_fetch_url(fake_open, &net, TEST_BOXART_URL, NK_COVER_ART_HOST, dest,
                                  fake_lock_cancelled, &state, &lock) == NK_COVER_ART_OK);
    assert(state.locks == 1 && state.unlocks == 1 && !state.held);
    assert(state.checks_while_held == 1);
    assert(file_exists(dest));

    /* A refused response never takes the lock. */
    memset(&net, 0, sizeof(net));
    memset(&state, 0, sizeof(state));
    add_answer(&net, TEST_BOXART_URL, "nope", 4u);
    assert(nk_cover_art_fetch_url(fake_open, &net, TEST_BOXART_URL, NK_COVER_ART_HOST, dest,
                                  fake_lock_cancelled, &state, &lock) == NK_COVER_ART_INVALID_PNG);
    assert(state.locks == 0 && state.unlocks == 0);

    char err_code[48];
    char err_msg[256];
    assert(nk_cover_art_clear_cache(data_root, err_code, sizeof(err_code),
                                    err_msg, sizeof(err_msg)));
    assert(!file_exists(dest));
    printf("[COVER_ART_TEST] cache_lock_and_late_cancel: PASS\n");
}

/* Discs with no cover art this session are remembered by SHA-1 (any case). */
static void test_session_misses(void) {
    NkCoverArtMisses *misses = (NkCoverArtMisses *)calloc(1u, sizeof(*misses));
    char sha1[41];
    assert(misses);
    assert(!nk_cover_art_misses_contains(misses, "4444444444444444444444444444444444444444"));
    assert(nk_cover_art_misses_add(misses, "ABCDEF0123456789ABCDEF0123456789ABCDEF01"));
    assert(nk_cover_art_misses_contains(misses, "abcdef0123456789abcdef0123456789abcdef01"));
    assert(nk_cover_art_misses_add(misses, "abcdef0123456789abcdef0123456789abcdef01"));
    assert(misses->count == 1u);
    assert(!nk_cover_art_misses_add(misses, "not-a-sha1"));
    assert(!nk_cover_art_misses_add(misses, NULL));
    assert(!nk_cover_art_misses_add(NULL, "abcdef0123456789abcdef0123456789abcdef01"));
    assert(!nk_cover_art_misses_contains(misses, "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"));

    /* Full: the oldest miss is forgotten first, every newer one is kept. */
    nk_cover_art_misses_clear(misses);
    assert(misses->count == 0u);
    for (size_t i = 0u; i <= NK_COVER_ART_MISS_SLOTS; i++) {
        snprintf(sha1, sizeof(sha1), "%040llx", (unsigned long long)i);
        assert(nk_cover_art_misses_add(misses, sha1));
    }
    assert(misses->count == NK_COVER_ART_MISS_SLOTS);
    snprintf(sha1, sizeof(sha1), "%040llx", 0ull);
    assert(!nk_cover_art_misses_contains(misses, sha1));
    snprintf(sha1, sizeof(sha1), "%040llx", 1ull);
    assert(nk_cover_art_misses_contains(misses, sha1));
    snprintf(sha1, sizeof(sha1), "%040llx", (unsigned long long)NK_COVER_ART_MISS_SLOTS);
    assert(nk_cover_art_misses_contains(misses, sha1));

    nk_cover_art_misses_clear(misses);
    assert(!nk_cover_art_misses_contains(misses, sha1));
    free(misses);
    printf("[COVER_ART_TEST] session_misses: PASS\n");
}

int main(void) {
    native_test_create_root("test_cover_art");
    native_test_capture_real_config_probe();
    native_test_isolate_user_data_roots();
    native_test_assert_cache_root_isolated();
    native_test_assert_config_root_isolated();

    test_sanitization_and_urls();
    test_png_validation_and_fetch();
    test_redirect_refused();
    test_cache_lock_and_late_cancel();
    test_session_misses();

    native_test_assert_real_config_untouched();
    printf("[COVER_ART_TEST] ALL COVER ART TESTS PASSED!\n");
    return 0;
}
