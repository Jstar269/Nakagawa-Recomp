/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Unit tests for the Online play server list (src/player/adhoc_servers.c, #1139). Every download
 * goes to an in-memory fake transport written from the list's public form (PPSSPP's
 * assets/adhoc-servers.json); no test here touches the internet (AGENTS.md section 5). */

#if !defined(_WIN32) && !defined(_WIN64)
#define _POSIX_C_SOURCE 200809L
#endif

#include "adhoc_servers.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
#define CHECK(cond, ...)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);          \
            fprintf(stderr, __VA_ARGS__);                                 \
            fputc('\n', stderr);                                          \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

typedef struct {
    const char *body;
    size_t size;
    bool reachable;
    bool has_content_length;
    uint64_t declared_length;
    bool fail_read;
    const char *final_url;   /* NULL: the request URL */
    size_t chunk;            /* bytes per read; 0 = as many as asked */
} FakeAnswer;

typedef struct {
    FakeAnswer answer;
    int opens;
    int closes;
    char url[256];
    char hosts[256];
    size_t offset;
} FakeNet;

static bool fake_read(void *context, unsigned char *buffer, size_t capacity, size_t *got) {
    FakeNet *net = (FakeNet *)context;
    if (net->answer.fail_read) return false;
    size_t left = net->answer.size - net->offset;
    size_t count = left < capacity ? left : capacity;
    if (net->answer.chunk && count > net->answer.chunk) count = net->answer.chunk;
    if (count) memcpy(buffer, net->answer.body + net->offset, count);
    net->offset += count;
    *got = count;
    return true;
}

static void fake_close(void *context) { ((FakeNet *)context)->closes++; }

static bool fake_open(void *context, const char *url, const char *allowed_hosts,
                      PackageHttpResponse *response) {
    FakeNet *net = (FakeNet *)context;
    net->opens++;
    snprintf(net->url, sizeof(net->url), "%s", url);
    snprintf(net->hosts, sizeof(net->hosts), "%s", allowed_hosts);
    if (!net->answer.reachable) return false;
    net->offset = 0u;
    response->final_url = net->answer.final_url ? net->answer.final_url : url;
    response->has_content_length = net->answer.has_content_length;
    response->content_length = net->answer.declared_length;
    response->context = net;
    response->read = fake_read;
    response->close = fake_close;
    return true;
}

typedef struct {
    char lines[96][256];
    size_t count;
} LogSink;

static void sink(void *context, const char *line) {
    LogSink *log = (LogSink *)context;
    if (log->count < sizeof(log->lines) / sizeof(log->lines[0]))
        snprintf(log->lines[log->count++], sizeof(log->lines[0]), "%s", line);
}

static bool logged(const LogSink *log, const char *needle) {
    for (size_t i = 0; i < log->count; i++)
        if (strstr(log->lines[i], needle)) return true;
    return false;
}

static const char k_two[] =
    "{\"servers\":[{\"name\":\"Socom Adhoc Server\",\"host\":\"socom.cc\",\"discord\":\"https://x.invalid/a\","
    "\"web\":\"https://x.invalid/w\",\"location\":\"France\",\"location-emoji\":\"\\ud83c\\uddeb\\ud83c\\uddf7\","
    "\"description\":\"d\",\"data_mode\":\"AemuPostoffice\",\"status_xml\":\"https://x.invalid/s.xml\","
    "\"icon\":\"/favicon.ico\"},"
    "{\"name\":\"S\\u00e3o\",\"host\":\"185.183.157.234\",\"location\":\"Germany\",\"data_mode\":\"AemuPostoffice\"}]}";

static FakeNet net_with(const char *body, size_t size) {
    FakeNet net;
    memset(&net, 0, sizeof(net));
    net.answer.body = body;
    net.answer.size = size;
    net.answer.reachable = true;
    return net;
}

static void test_fetch_waits_for_online_play(void) {
    NkAdhocServerList list;
    LogSink log = {0};
    FakeNet net = net_with(k_two, sizeof(k_two) - 1u);
    CHECK(nk_adhoc_list_fetch(fake_open, &net, false, &list, sink, &log) == NK_ADHOC_LIST_OFF &&
              net.opens == 0 && list.count == 0u,
          "with Online play off nothing is fetched (opens=%d)", net.opens);
    CHECK(nk_adhoc_list_fetch(fake_open, &net, true, &list, sink, &log) == NK_ADHOC_LIST_OK &&
              net.opens == 1 && net.closes == 1,
          "with Online play on the list is fetched once and closed");
    CHECK(strcmp(net.url, "https://metadata.ppsspp.org/adhoc-servers.json") == 0 &&
              strcmp(net.hosts, "metadata.ppsspp.org") == 0,
          "the one URL is PPSSPP's list over HTTPS, and only its host is allowed (%s, %s)", net.url,
          net.hosts);
    CHECK(list.count == 2u && strcmp(list.entries[0].host, "socom.cc") == 0 &&
              strcmp(list.entries[0].name, "Socom Adhoc Server") == 0 &&
              strcmp(list.entries[0].location, "France") == 0 &&
              strcmp(list.entries[1].host, "185.183.157.234") == 0 &&
              strcmp(list.entries[1].name, "S\xc3\xa3o") == 0,
          "both entries are read with their name, host and place");
}

static void test_fetch_refusals(void) {
    NkAdhocServerList list;
    LogSink log = {0};
    FakeNet net = net_with(k_two, sizeof(k_two) - 1u);
    net.answer.reachable = false;
    CHECK(nk_adhoc_list_fetch(fake_open, &net, true, &list, sink, &log) == NK_ADHOC_LIST_UNREACHABLE &&
              logged(&log, "did not answer"),
          "an unreachable list host is a named state");
    net = net_with(k_two, sizeof(k_two) - 1u);
    net.answer.final_url = "https://evil.invalid/adhoc-servers.json";
    CHECK(nk_adhoc_list_fetch(fake_open, &net, true, &list, sink, &log) ==
                  NK_ADHOC_LIST_REDIRECT_REFUSED && net.closes == 1 && list.count == 0u,
          "an answer from another host is refused");
    net = net_with(k_two, sizeof(k_two) - 1u);
    net.answer.final_url = "http://metadata.ppsspp.org/adhoc-servers.json";
    CHECK(nk_adhoc_list_fetch(fake_open, &net, true, &list, sink, &log) ==
              NK_ADHOC_LIST_REDIRECT_REFUSED,
          "an answer over plain HTTP is refused");
    net = net_with(k_two, sizeof(k_two) - 1u);
    net.answer.has_content_length = true;
    net.answer.declared_length = NK_ADHOC_LIST_MAX_BYTES + 1u;
    CHECK(nk_adhoc_list_fetch(fake_open, &net, true, &list, sink, &log) == NK_ADHOC_LIST_TOO_LARGE,
          "a list announced over 64 KiB is refused before reading");
    char *big = (char *)malloc(NK_ADHOC_LIST_MAX_BYTES + 100u);
    if (big) {
        memset(big, ' ', NK_ADHOC_LIST_MAX_BYTES + 100u);
        net = net_with(big, NK_ADHOC_LIST_MAX_BYTES + 100u);
        net.answer.chunk = 1000u;
        CHECK(nk_adhoc_list_fetch(fake_open, &net, true, &list, sink, &log) ==
                      NK_ADHOC_LIST_TOO_LARGE && net.closes == 1,
              "a list that grows past 64 KiB without a length is refused while reading");
        free(big);
    }
    net = net_with(k_two, sizeof(k_two) - 1u);
    net.answer.fail_read = true;
    CHECK(nk_adhoc_list_fetch(fake_open, &net, true, &list, sink, &log) == NK_ADHOC_LIST_UNREACHABLE,
          "a failed read is a named state");
    for (int s = NK_ADHOC_LIST_OK; s <= NK_ADHOC_LIST_NO_MEMORY; s++) {
        const char *text = nk_adhoc_list_status_text((NkAdhocListStatus)s);
        CHECK(text && text[0], "status %d has a sentence", s);
    }
}

static void test_hostile_entries(void) {
    NkAdhocServerList list;
    LogSink log = {0};
    static const char text[] =
        "{\"servers\":["
        "{\"host\":\"good.example\",\"data_mode\":\"AemuPostoffice\"},"
        "{\"host\":\"bad host\",\"data_mode\":\"AemuPostoffice\"},"
        "{\"host\":\"a.example\",\"name\":\"tab\\there\",\"data_mode\":\"AemuPostoffice\"},"
        "{\"host\":\"b.example\",\"name\":\"evil\\u202egnp.exe\",\"data_mode\":\"AemuPostoffice\"},"
        "{\"host\":\"c.example\",\"hidden\":true,\"data_mode\":\"AemuPostoffice\"},"
        "{\"host\":\"d.example\",\"data_mode\":\"P2P\"},"
        "{\"host\":\"e.example\"},"
        "{\"host\":\"good.example\",\"data_mode\":\"AemuPostoffice\"},"
        "\"not an object\","
        "{\"host\":7,\"data_mode\":\"AemuPostoffice\"},"
        "{\"host\":\"f.example\",\"name\":\"0123456789012345678901234567890123456789012345678901234567890123456789\","
        "\"data_mode\":\"AemuPostoffice\"},"
        "{\"host\":\"g.example\",\"location\":[1],\"data_mode\":\"AemuPostoffice\"},"
        "{\"host\":\"h.example\",\"name\":\"ok \\u0085 c1\",\"data_mode\":\"AemuPostoffice\"},"
        "{\"host\":\"1.2.3\",\"data_mode\":\"AemuPostoffice\"},"
        "{\"host\":\"i.example:27312\",\"data_mode\":\"AemuPostoffice\"},"
        "{\"host\":\"j.example\",\"hidden\":\"yes\",\"data_mode\":\"AemuPostoffice\"}"
        "]}";
    CHECK(nk_adhoc_list_parse(text, sizeof(text) - 1u, &list, sink, &log) == NK_ADHOC_LIST_OK &&
              list.count == 1u && strcmp(list.entries[0].host, "good.example") == 0 &&
              strcmp(list.entries[0].name, "good.example") == 0 && list.dropped == 15u,
          "only the one clean relay entry is kept (count=%zu dropped=%zu)", list.count, list.dropped);
    CHECK(logged(&log, "entry 1 dropped: it has no valid host name") &&
              logged(&log, "entry 2 dropped: it holds a control character") &&
              logged(&log, "entry 3 dropped: it holds a bidirectional mark") &&
              logged(&log, "entry 4 dropped: it is marked hidden") &&
              logged(&log, "entry 5 dropped: it is a lobby-only (P2P) server") &&
              logged(&log, "entry 6 dropped: it is a lobby-only (P2P) server") &&
              logged(&log, "entry 7 dropped: it repeats an earlier host") &&
              logged(&log, "entry 8 dropped: it is not an object") &&
              logged(&log, "entry 10 dropped: it is too long") &&
              logged(&log, "entry 11 dropped: it has a name, location or data_mode") &&
              logged(&log, "entry 12 dropped: it holds a control character") &&
              logged(&log, "entry 13 dropped: it has no valid host name") &&
              logged(&log, "entry 14 dropped: it has no valid host name") &&
              logged(&log, "entry 15 dropped: it has a hidden flag"),
          "each dropped entry is named with its reason");
}

static void test_whole_list_refusals(void) {
    NkAdhocServerList list;
    LogSink log = {0};
    static const char nul[] =
        "{\"servers\":[{\"host\":\"socom.cc\",\"name\":\"a\\u0000b\",\"data_mode\":\"AemuPostoffice\"}]}";
    CHECK(nk_adhoc_list_parse(nul, sizeof(nul) - 1u, &list, sink, &log) == NK_ADHOC_LIST_MALFORMED &&
              list.count == 0u,
          "a string holding \\u0000 refuses the list");
    static const char bad_utf8[] = "{\"servers\":[{\"host\":\"socom.cc\",\"name\":\"\xff\"}]}";
    CHECK(nk_adhoc_list_parse(bad_utf8, sizeof(bad_utf8) - 1u, &list, sink, &log) ==
              NK_ADHOC_LIST_MALFORMED,
          "a list that is not UTF-8 is refused");
    static const char no_array[] = "{\"servers\":{}}";
    CHECK(nk_adhoc_list_parse(no_array, sizeof(no_array) - 1u, &list, sink, &log) ==
              NK_ADHOC_LIST_MALFORMED,
          "a list without a servers array is refused");
    static const char trailing[] = "{\"servers\":[]} x";
    CHECK(nk_adhoc_list_parse(trailing, sizeof(trailing) - 1u, &list, sink, &log) ==
              NK_ADHOC_LIST_MALFORMED,
          "text after the JSON value is refused");
    static const char empty[] = "{\"servers\":[]}";
    CHECK(nk_adhoc_list_parse(empty, sizeof(empty) - 1u, &list, sink, &log) == NK_ADHOC_LIST_EMPTY,
          "an empty list is a named state");
    char deep[400];
    size_t n = 0u;
    for (int i = 0; i < 150; i++) deep[n++] = '[';
    for (int i = 0; i < 150; i++) deep[n++] = ']';
    deep[n] = '\0';
    CHECK(nk_adhoc_list_parse(deep, n, &list, sink, &log) == NK_ADHOC_LIST_MALFORMED,
          "deep nesting is refused by the parser's depth limit");
    /* Seventy entries: the first 64 are read, the rest dropped by count. */
    size_t cap = 70u * 64u + 32u;
    char *many = (char *)malloc(cap);
    if (many) {
        size_t used = (size_t)snprintf(many, cap, "{\"servers\":[");
        for (int i = 0; i < 70; i++) {
            used += (size_t)snprintf(many + used, cap - used,
                                     "%s{\"host\":\"h%d.example\",\"data_mode\":\"AemuPostoffice\"}",
                                     i ? "," : "", i);
        }
        used += (size_t)snprintf(many + used, cap - used, "]}");
        CHECK(nk_adhoc_list_parse(many, used, &list, sink, &log) == NK_ADHOC_LIST_OK &&
                  list.count == NK_ADHOC_LIST_MAX_ENTRIES && list.dropped == 6u &&
                  logged(&log, "entries 64 to 69 dropped: past the 64-entry cap"),
              "entries past the cap are dropped by count (count=%zu)", list.count);
        free(many);
    }
}

static void test_shipped_copy(void) {
    NkAdhocServerList list;
    LogSink log = {0};
    CHECK(nk_adhoc_list_load_shipped(&list, sink, &log) == NK_ADHOC_LIST_OK && list.count == 22u &&
              list.dropped == 0u,
          "the shipped copy reads cleanly: 22 relay servers (count=%zu dropped=%zu)", list.count,
          list.dropped);
    for (size_t p = 0; p < nk_adhoc_server_preset_count(); p++) {
        bool found = false;
        for (size_t i = 0; i < list.count; i++)
            if (strcmp(list.entries[i].host, nk_adhoc_server_preset(p)->host) == 0) found = true;
        CHECK(found, "preset %s is also in the shipped copy", nk_adhoc_server_preset(p)->host);
    }
    CHECK(strcmp(list.entries[0].host, NK_ADHOC_DEFAULT_SERVER) == 0,
          "the shipped copy starts with the default server");
}

int main(void) {
    test_fetch_waits_for_online_play();
    test_fetch_refusals();
    test_hostile_entries();
    test_whole_list_refusals();
    test_shipped_copy();
    if (g_failures) {
        fprintf(stderr, "test_adhoc_servers: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_adhoc_servers: PASS\n");
    return 0;
}
