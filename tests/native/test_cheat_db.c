/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Unit tests for the cheat database download and import (src/player/cheat_db.c, #2639).
 * Every HTTP exchange goes through an in-memory fake transport; no test here touches the
 * internet (AGENTS.md section 5), and the database is a synthetic one written here. */

#if !defined(_WIN32) && !defined(_WIN64)
#define _POSIX_C_SOURCE 200809L
#endif

#include "cheat_db.h"
#include "nk_cheats.h"
#include "nk_platform.h"
#include "nk_shared_helpers.h"
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

#define COMMIT "0123456789abcdef0123456789abcdef01234567"
#define DB_URL "https://raw.githubusercontent.com/someone/some-db/" COMMIT "/cheat.db"

typedef struct {
    const char *url;
    const char *body;
    size_t size;
    bool reachable;
    int http_status;
    bool has_content_length;
    uint64_t declared_length;
    const char *final_url;
} FakeAnswer;

typedef struct {
    FakeAnswer answers[4];
    size_t answer_count;
    char requested[8][NK_CHEAT_DB_URL_MAX];
    char hosts[8][64];
    size_t request_count;
    const FakeAnswer *open;
    size_t offset;
    int closes;
} FakeNet;

static bool fake_read(void *context, unsigned char *buffer, size_t capacity, size_t *read) {
    FakeNet *net = (FakeNet *)context;
    size_t left = net->open->size - net->offset;
    size_t count = left < capacity ? left : capacity;
    if (count > 1000u) count = 1000u;   /* short reads, as a network gives */
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
    assert(net->request_count < 8u);
    snprintf(net->requested[net->request_count], sizeof(net->requested[0]), "%s", url);
    snprintf(net->hosts[net->request_count], sizeof(net->hosts[0]), "%s", allowed_hosts);
    net->request_count++;
    for (size_t i = 0u; i < net->answer_count; i++) {
        const FakeAnswer *answer = &net->answers[i];
        if (strcmp(answer->url, url) != 0) continue;
        if (!answer->reachable) return false;
        net->open = answer;
        net->offset = 0u;
        memset(response, 0, sizeof(*response));
        response->final_url = answer->final_url ? answer->final_url : answer->url;
        response->has_content_length = answer->has_content_length;
        response->content_length = answer->declared_length;
        response->http_status = answer->http_status;
        response->context = net;
        response->read = fake_read;
        response->close = fake_close;
        return true;
    }
    return false;
}

static FakeAnswer *add_answer(FakeNet *net, const char *url, const char *body, size_t size) {
    FakeAnswer *answer = &net->answers[net->answer_count++];
    memset(answer, 0, sizeof(*answer));
    answer->url = url;
    answer->body = body;
    answer->size = size;
    answer->reachable = true;
    answer->http_status = 200;
    answer->has_content_length = true;
    answer->declared_length = size;
    return answer;
}

static const char kDatabase[] =
    "// a synthetic database\n"
    ">>>Start Of CWCheat Database<<<\n"
    "_S TEST-00001\n"
    "_G Fixture One\n"
    "_C0 Infinite fixture\n"
    "_L 0x20001000 0x00000063\n"
    "_C0 Another\n"
    "_L 0x00001004 0x00000001\n"
    "_S TEST-00002\n"
    "_G Fixture Two\n"
    "_C0 Third\n"
    "_L 0x10002000 0x0000FFFF\n";

static void hex_of(const uint8_t digest[32], char out[65]) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0u; i < 32u; i++) {
        out[2u * i] = digits[digest[i] >> 4];
        out[2u * i + 1u] = digits[digest[i] & 15u];
    }
    out[64] = '\0';
}

static void sha256_hex(const char *text, size_t size, char out[65]) {
    NkSha256 sha;
    uint8_t digest[32];
    nk_sha256_init(&sha);
    nk_sha256_update(&sha, (const uint8_t *)text, size);
    nk_sha256_finish(&sha, digest);
    hex_of(digest, out);
}

static size_t make_list(char *out, size_t out_size, const char *url, const char *commit,
                        unsigned long long size, const char *sha) {
    int n = snprintf(out, out_size,
                     "{\"format\": \"nakagawa-cheat-db-sources\", \"schema_version\": 1, \"sources\": [{"
                     "\"id\": \"fixture-db\", \"name\": \"Fixture DB\", \"maintainer\": \"Someone\", "
                     "\"licence\": \"No licence stated\", \"homepage\": \"https://example.invalid/db\", "
                     "\"commit\": \"%s\", \"url\": \"%s\", \"size\": %llu, \"sha256\": \"%s\"}]}",
                     commit, url, size, sha);
    assert(n > 0 && (size_t)n < out_size);
    return (size_t)n;
}

static char *read_all(const char *path, size_t *size) {
    FILE *f = nk_fopen_utf8(path, "rb");
    char *data;
    if (!f) return NULL;
    data = (char *)malloc(1u << 16);
    assert(data);
    *size = fread(data, 1u, (1u << 16) - 1u, f);
    data[*size] = '\0';
    fclose(f);
    return data;
}

static NkCheatDbSource fixture_source(void) {
    NkCheatDbSources sources;
    char list[2048];
    char sha[65];
    size_t n;
    sha256_hex(kDatabase, sizeof(kDatabase) - 1u, sha);
    n = make_list(list, sizeof(list), DB_URL, COMMIT, sizeof(kDatabase) - 1u, sha);
    assert(nk_cheat_db_parse_sources(list, n, &sources, NULL, 0u) == NK_CHEAT_DB_OK);
    assert(sources.count == 1u);
    return sources.sources[0];
}

/* The repository's own list is valid, pins one commit by its 40 digits, and names its source,
 * maintainer and licence as the UI shows them. */
static void test_repository_list(void) {
    NkCheatDbSources sources;
    char error[256];
    size_t size = 0u;
    char *text = read_all("assets/cheat_db_sources.json", &size);
    assert(text != NULL);
    assert(nk_cheat_db_parse_sources(text, size, &sources, error, sizeof(error)) == NK_CHEAT_DB_OK);
    assert(sources.count == 1u);
    assert(strcmp(sources.sources[0].name, "CWCheat-Database-Plus") == 0);
    assert(strcmp(sources.sources[0].maintainer, "Saramagrean") == 0);
    assert(strcmp(sources.sources[0].licence, "No licence stated") == 0);
    assert(strncmp(sources.sources[0].url, "https://" NK_CHEAT_DB_HOST "/Saramagrean/",
                   strlen("https://" NK_CHEAT_DB_HOST "/Saramagrean/")) == 0);
    assert(strstr(sources.sources[0].url, sources.sources[0].commit) != NULL);
    assert(sources.sources[0].size > 0u && sources.sources[0].size <= NK_CHEATS_MAX_FILE_BYTES);
    free(text);
}

static void test_list_refusals(void) {
    NkCheatDbSources sources;
    char list[2048];
    char error[256];
    char sha[65];
    size_t n;
    memset(sha, 'a', 64u);
    sha[64] = '\0';
    /* A moving branch, another host, a commit the URL does not carry, a path that climbs, a
     * size past the reader's cap, a short hash: each refused, and nothing is kept. */
    static const struct { const char *url; const char *commit; unsigned long long size; } bad[] = {
        {"https://raw.githubusercontent.com/someone/some-db/refs/heads/master/cheat.db", COMMIT, 10u},
        {"https://example.com/someone/some-db/" COMMIT "/cheat.db", COMMIT, 10u},
        {"http://raw.githubusercontent.com/someone/some-db/" COMMIT "/cheat.db", COMMIT, 10u},
        {DB_URL, "1123456789abcdef0123456789abcdef01234567", 10u},
        {"https://raw.githubusercontent.com/someone/some-db/" COMMIT "/../cheat.db", COMMIT, 10u},
        {"https://raw.githubusercontent.com/someone/some-db/" COMMIT "/cheat.db?x=1", COMMIT, 10u},
        {"https://raw.githubusercontent.com@evil.example/a/b/" COMMIT "/cheat.db", COMMIT, 10u},
        {DB_URL, COMMIT, 0u},
        {DB_URL, COMMIT, (unsigned long long)NK_CHEATS_MAX_FILE_BYTES + 1u},
    };
    for (size_t i = 0u; i < sizeof(bad) / sizeof(bad[0]); i++) {
        n = make_list(list, sizeof(list), bad[i].url, bad[i].commit, bad[i].size, sha);
        assert(nk_cheat_db_parse_sources(list, n, &sources, error, sizeof(error)) ==
               NK_CHEAT_DB_LIST_INVALID);
        assert(sources.count == 0u);
        assert(strstr(error, "CHEAT_DB_LIST_INVALID") != NULL);
    }
    n = make_list(list, sizeof(list), DB_URL, COMMIT, 10u, "ABCDEF");
    assert(nk_cheat_db_parse_sources(list, n, &sources, error, sizeof(error)) == NK_CHEAT_DB_LIST_INVALID);
    {
        static const char newer[] =
            "{\"format\": \"nakagawa-cheat-db-sources\", \"schema_version\": 2, \"sources\": []}";
        assert(nk_cheat_db_parse_sources(newer, sizeof(newer) - 1u, &sources, error, sizeof(error)) ==
               NK_CHEAT_DB_LIST_VERSION);
        assert(strstr(error, "CHEAT_DB_LIST_VERSION") != NULL);
    }
    {
        static const char other[] = "{\"format\": \"something-else\", \"schema_version\": 1}";
        static const char broken[] = "{\"format\": ";
        assert(nk_cheat_db_parse_sources(other, sizeof(other) - 1u, &sources, NULL, 0u) ==
               NK_CHEAT_DB_LIST_INVALID);
        assert(nk_cheat_db_parse_sources(broken, sizeof(broken) - 1u, &sources, NULL, 0u) ==
               NK_CHEAT_DB_LIST_INVALID);
    }
    {
        char *big = (char *)malloc(NK_CHEAT_DB_SOURCES_MAX_BYTES + 2u);
        assert(big);
        memset(big, ' ', NK_CHEAT_DB_SOURCES_MAX_BYTES + 1u);
        big[0] = '{';
        big[NK_CHEAT_DB_SOURCES_MAX_BYTES] = '}';
        assert(nk_cheat_db_parse_sources(big, NK_CHEAT_DB_SOURCES_MAX_BYTES + 1u, &sources, NULL, 0u) ==
               NK_CHEAT_DB_TOO_LARGE);
        free(big);
    }
}

static void test_check_text(void) {
    NkCheatDbCounts counts;
    char error[256];
    assert(nk_cheat_db_check_text(kDatabase, sizeof(kDatabase) - 1u, &counts, error, sizeof(error)) ==
           NK_CHEAT_DB_OK);
    assert(counts.games == 2u && counts.cheats == 3u);
    {
        /* A cheat outside every _S section would apply to every game: refused by name. */
        static const char loose[] = "_C1 Everywhere\n_L 0x20001000 0x00000001\n_S TEST-00001\n_C0 x\n";
        assert(nk_cheat_db_check_text(loose, sizeof(loose) - 1u, &counts, error, sizeof(error)) ==
               NK_CHEAT_DB_NOT_A_DATABASE);
        assert(strstr(error, "every game") != NULL);
    }
    {
        static const char empty[] = "just some text\nwith no codes\n";
        assert(nk_cheat_db_check_text(empty, sizeof(empty) - 1u, &counts, error, sizeof(error)) ==
               NK_CHEAT_DB_NOT_A_DATABASE);
        assert(counts.games == 0u && counts.cheats == 0u);
    }
    assert(nk_cheat_db_check_text("x", (size_t)NK_CHEATS_MAX_FILE_BYTES + 1u, &counts, error,
                                  sizeof(error)) == NK_CHEAT_DB_TOO_LARGE);
}

static void test_fetch_sources(void) {
    FakeNet net;
    NkCheatDbSources sources;
    char list[2048];
    char error[256];
    char sha[65];
    size_t n;
    sha256_hex(kDatabase, sizeof(kDatabase) - 1u, sha);
    n = make_list(list, sizeof(list), DB_URL, COMMIT, sizeof(kDatabase) - 1u, sha);
    memset(&net, 0, sizeof(net));
    add_answer(&net, NK_CHEAT_DB_SOURCES_URL, list, n);
    assert(nk_cheat_db_fetch_sources(fake_open, &net, &sources, error, sizeof(error)) == NK_CHEAT_DB_OK);
    assert(sources.count == 1u && strcmp(sources.sources[0].name, "Fixture DB") == 0);
    /* The one request goes to the project's list on the one allowed host. */
    assert(net.request_count == 1u);
    assert(strcmp(net.requested[0], NK_CHEAT_DB_SOURCES_URL) == 0);
    assert(strcmp(net.hosts[0], NK_CHEAT_DB_HOST) == 0);
    assert(net.closes == 1);

    /* An answer redirected to another host is refused, whatever the transport did. */
    memset(&net, 0, sizeof(net));
    add_answer(&net, NK_CHEAT_DB_SOURCES_URL, list, n)->final_url = "https://example.com/list.json";
    assert(nk_cheat_db_fetch_sources(fake_open, &net, &sources, error, sizeof(error)) ==
           NK_CHEAT_DB_REDIRECT_REFUSED);
    assert(net.closes == 1);
    /* No answer, and an answer that is not 200. */
    memset(&net, 0, sizeof(net));
    assert(nk_cheat_db_fetch_sources(fake_open, &net, &sources, error, sizeof(error)) ==
           NK_CHEAT_DB_NOT_REACHED);
    memset(&net, 0, sizeof(net));
    add_answer(&net, NK_CHEAT_DB_SOURCES_URL, "gone", 4u)->http_status = 404;
    assert(nk_cheat_db_fetch_sources(fake_open, &net, &sources, error, sizeof(error)) ==
           NK_CHEAT_DB_HTTP_STATUS);
    assert(strstr(error, "404") != NULL);
    /* No transport: nothing is fetched at all. */
    assert(nk_cheat_db_fetch_sources(NULL, NULL, &sources, error, sizeof(error)) ==
           NK_CHEAT_DB_BAD_ARGUMENT);
}

static void test_download(const char *root) {
    FakeNet net;
    NkCheatDbSource source = fixture_source();
    NkCheatDbCounts counts;
    char error[256];
    char dest[NK_MAX_PATH];
    char *written;
    size_t size;
    const size_t db_size = sizeof(kDatabase) - 1u;
    assert(nk_cheat_db_path(root, dest, sizeof(dest)));
    assert(strstr(dest, "cheats") != NULL && strstr(dest, NK_CHEAT_DB_FILE_NAME) != NULL);

    memset(&net, 0, sizeof(net));
    add_answer(&net, DB_URL, kDatabase, db_size);
    assert(nk_cheat_db_download(fake_open, &net, &source, dest, false, NULL, NULL, &counts,
                                error, sizeof(error)) == NK_CHEAT_DB_OK);
    assert(counts.games == 2u && counts.cheats == 3u);
    assert(net.request_count == 1u && strcmp(net.requested[0], DB_URL) == 0);
    assert(strcmp(net.hosts[0], NK_CHEAT_DB_HOST) == 0);
    written = read_all(dest, &size);
    assert(written && size == db_size && memcmp(written, kDatabase, db_size) == 0);
    free(written);

    /* It is there now: without the confirmation nothing is requested or replaced. */
    memset(&net, 0, sizeof(net));
    add_answer(&net, DB_URL, kDatabase, db_size);
    assert(nk_cheat_db_download(fake_open, &net, &source, dest, false, NULL, NULL, &counts,
                                error, sizeof(error)) == NK_CHEAT_DB_EXISTS);
    assert(net.request_count == 0u);
    assert(strstr(error, "CHEAT_DB_EXISTS") != NULL);

    {
        /* A body of the pinned size whose bytes differ: refused by name, the file kept. */
        char *tampered = (char *)malloc(db_size);
        assert(tampered);
        memcpy(tampered, kDatabase, db_size);
        tampered[db_size - 2u] = 'E';
        memset(&net, 0, sizeof(net));
        add_answer(&net, DB_URL, tampered, db_size);
        assert(nk_cheat_db_download(fake_open, &net, &source, dest, true, NULL, NULL, &counts,
                                    error, sizeof(error)) == NK_CHEAT_DB_HASH_MISMATCH);
        assert(strstr(error, "CHEAT_DB_HASH_MISMATCH") != NULL);
        free(tampered);
    }
    {
        /* Longer and shorter than the pin, declared or not. */
        static char longer[sizeof(kDatabase) + 8u];
        memcpy(longer, kDatabase, db_size);
        memcpy(longer + db_size, "_C0 more", 8u);
        memset(&net, 0, sizeof(net));
        add_answer(&net, DB_URL, longer, db_size + 8u);
        assert(nk_cheat_db_download(fake_open, &net, &source, dest, true, NULL, NULL, &counts,
                                    error, sizeof(error)) == NK_CHEAT_DB_SIZE_MISMATCH);
        memset(&net, 0, sizeof(net));
        add_answer(&net, DB_URL, longer, db_size + 8u)->has_content_length = false;
        assert(nk_cheat_db_download(fake_open, &net, &source, dest, true, NULL, NULL, &counts,
                                    error, sizeof(error)) == NK_CHEAT_DB_SIZE_MISMATCH);
        memset(&net, 0, sizeof(net));
        add_answer(&net, DB_URL, kDatabase, db_size - 5u)->has_content_length = false;
        assert(nk_cheat_db_download(fake_open, &net, &source, dest, true, NULL, NULL, &counts,
                                    error, sizeof(error)) == NK_CHEAT_DB_SIZE_MISMATCH);
        memset(&net, 0, sizeof(net));
        add_answer(&net, DB_URL, kDatabase, db_size - 5u)->declared_length = db_size;
        assert(nk_cheat_db_download(fake_open, &net, &source, dest, true, NULL, NULL, &counts,
                                    error, sizeof(error)) == NK_CHEAT_DB_TRUNCATED);
    }
    /* Every refusal above left the first download in place. */
    written = read_all(dest, &size);
    assert(written && size == db_size && memcmp(written, kDatabase, db_size) == 0);
    free(written);
    /* With the confirmation it is replaced. */
    memset(&net, 0, sizeof(net));
    add_answer(&net, DB_URL, kDatabase, db_size);
    assert(nk_cheat_db_download(fake_open, &net, &source, dest, true, NULL, NULL, &counts,
                                error, sizeof(error)) == NK_CHEAT_DB_OK);
    assert(remove(dest) == 0);
}

static void test_import(const char *root) {
    NkCheatDbCounts counts;
    char error[256];
    char dest[NK_MAX_PATH];
    char mine[NK_MAX_PATH + 16];
    FILE *f;
    char *written;
    size_t size;
    assert(nk_cheat_db_path(root, dest, sizeof(dest)));
    snprintf(mine, sizeof(mine), "%s%cmine.db", root, nk_platform_path_separator());
    f = nk_fopen_utf8(mine, "wb");
    assert(f && fwrite(kDatabase, 1u, sizeof(kDatabase) - 1u, f) == sizeof(kDatabase) - 1u);
    assert(fclose(f) == 0);
    assert(nk_cheat_db_import_file(mine, dest, false, &counts, error, sizeof(error)) == NK_CHEAT_DB_OK);
    assert(counts.games == 2u && counts.cheats == 3u);
    written = read_all(dest, &size);
    assert(written && size == sizeof(kDatabase) - 1u);
    free(written);
    assert(nk_cheat_db_import_file(mine, dest, false, &counts, error, sizeof(error)) ==
           NK_CHEAT_DB_EXISTS);
    /* A file that is not a database replaces nothing, even with the confirmation. */
    f = nk_fopen_utf8(mine, "wb");
    assert(f && fputs("_C1 loose\n_L 0x20001000 0x00000001\n", f) >= 0);
    assert(fclose(f) == 0);
    assert(nk_cheat_db_import_file(mine, dest, true, &counts, error, sizeof(error)) ==
           NK_CHEAT_DB_NOT_A_DATABASE);
    written = read_all(dest, &size);
    assert(written && size == sizeof(kDatabase) - 1u && memcmp(written, kDatabase, size) == 0);
    free(written);
    assert(nk_cheat_db_import_file("no-such-file.db", dest, true, &counts, error, sizeof(error)) ==
           NK_CHEAT_DB_IO_ERROR);
    assert(remove(mine) == 0 && remove(dest) == 0);
}

int main(void) {
    char root[NK_MAX_PATH];
    native_test_create_root("test_cheat_db");
    native_test_capture_real_config_probe();
    native_test_isolate_user_data_roots();
    native_test_assert_cache_root_isolated();
    native_test_assert_config_root_isolated();
    snprintf(root, sizeof(root), "%s", native_test_get_root());
    assert(nk_platform_mkdir_p(root));
    test_repository_list();
    test_list_refusals();
    test_check_text();
    test_fetch_sources();
    test_download(root);
    test_import(root);
    printf("test_cheat_db: all passed\n");
    return 0;
}
