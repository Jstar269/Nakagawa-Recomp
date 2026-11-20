/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Unit tests for the Online play server list (src/player/adhoc_servers.c, #1139). Every download
 * goes to an in-memory fake transport written from the list's public form (PPSSPP's
 * assets/adhoc-servers.json); no test here touches the internet (AGENTS.md section 5). */

#if !defined(_WIN32) && !defined(_WIN64)
#define _POSIX_C_SOURCE 200809L
#endif

#include "adhoc_servers.h"
#include "native_test_isolation.h"
#include "nk_platform.h"

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

static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ---- console ID (New console ID) ------------------------------------------------------------ */

static const char k_registry_good[] =
    "{\n  \"schema\": \"nakagawa-sceReg-overlay\",\n  \"schema_version\": 1,\n"
    "  \"categories\": [\n    \"/CONFIG/SYSTEM\"\n  ],\n  \"keys\": [\n"
    "    {\"path\": \"/CONFIG/SYSTEM\", \"name\": \"owner_name\", \"type\": \"str\", "
    "\"value_hex\": \"506c6179657200\"}\n  ],\n"
    "  \"console\": {\"wlan_mac_hex\": \"A4A1B2C3D4E5\", \"open_psid_hex\": "
    "\"00112233445566778899aabbccddeeff\", \"wlan_switch\": 1}\n}\n";

static bool write_file(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    bool ok = f != NULL && fwrite(data, 1u, len, f) == len;
    if (f && fclose(f) != 0) ok = false;
    return ok;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    long size;
    *len = 0u;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        buf = (char *)malloc((size_t)size + 1u);
        if (buf && fread(buf, 1u, (size_t)size, f) == (size_t)size) {
            buf[size] = '\0';
            *len = (size_t)size;
        } else {
            free(buf);
            buf = NULL;
        }
    }
    fclose(f);
    return buf;
}

/* The file must read back byte for byte as written: a refusal leaves it alone. */
static bool file_is(const char *path, const char *data, size_t len) {
    size_t got = 0u;
    char *now = read_file(path, &got);
    bool same = now != NULL && got == len && memcmp(now, data, len) == 0;
    free(now);
    return same;
}

static void test_console_id(void) {
    char root[NATIVE_TEST_PATH_MAX], dir[NATIVE_TEST_PATH_MAX + 16], path[NATIVE_TEST_PATH_MAX + 32];
    char hex[NK_CONSOLE_ID_HEX_LEN + 1u], fresh[NK_CONSOLE_ID_HEX_LEN + 1u], shown[32];
    const char sep = nk_platform_path_separator();
    snprintf(root, sizeof(root), "%s", native_test_get_root());
    snprintf(dir, sizeof(dir), "%s%cregistry", root, sep);
    CHECK(nk_adhoc_console_registry_path(root, path, sizeof(path)) &&
              strstr(path, "registry") != NULL && strstr(path, "system.json") != NULL,
          "the registry path is <root>/registry/system.json (%s)", path);
    CHECK(!nk_adhoc_console_registry_path("", shown, sizeof(shown)) && shown[0] == '\0' &&
              !nk_adhoc_console_registry_path(root, shown, 8u),
          "an empty root or a short buffer gives no path");

    /* No file yet: the runtime makes the ID the first time a game asks. */
    CHECK(nk_adhoc_console_id_read(path, hex) == NK_CONSOLE_ID_NONE && hex[0] == '\0' &&
              nk_adhoc_console_id_renew(path, fresh) == NK_CONSOLE_ID_NONE && fresh[0] == '\0' &&
              !nk_platform_file_exists(path),
          "with no file, nothing is read and nothing is written");
    CHECK(nk_platform_mkdir_p_private(dir), "the registry folder is made");
    static const char no_console[] = "{\n  \"schema\": \"nakagawa-sceReg-overlay\",\n  \"keys\": []\n}\n";
    CHECK(write_file(path, no_console, sizeof(no_console) - 1u) &&
              nk_adhoc_console_id_read(path, hex) == NK_CONSOLE_ID_NONE &&
              nk_adhoc_console_id_renew(path, fresh) == NK_CONSOLE_ID_NONE &&
              file_is(path, no_console, sizeof(no_console) - 1u),
          "a file without a console object has no ID yet and is left alone");

    /* The form the runtime writes: read, then replaced in place. */
    CHECK(write_file(path, k_registry_good, sizeof(k_registry_good) - 1u) &&
              nk_adhoc_console_id_read(path, hex) == NK_CONSOLE_ID_OK &&
              strcmp(hex, "a4a1b2c3d4e5") == 0,
          "the console ID is read as lower-case hex (%s)", hex);
    CHECK(nk_adhoc_console_id_display(hex, shown, sizeof(shown)) &&
              strcmp(shown, "a4:a1:b2:c3:d4:e5") == 0 &&
              !nk_adhoc_console_id_display("a4a1b2c3d4", shown, sizeof(shown)) &&
              !nk_adhoc_console_id_display("02a1b2c3d4zz", shown, sizeof(shown)),
          "the ID shows as six octets, and only a 12-digit one does");
    CHECK(nk_adhoc_console_id_renew(path, fresh) == NK_CONSOLE_ID_OK &&
              strlen(fresh) == NK_CONSOLE_ID_HEX_LEN && strcmp(fresh, hex) != 0,
          "New console ID makes a different one (%s)", fresh);
    {
        size_t len = 0u;
        char *now = read_file(path, &len);
        const char *at = strstr(k_registry_good, "A4A1B2C3D4E5");
        const size_t off = (size_t)(at - k_registry_good);
        bool rest_same = now != NULL && len == sizeof(k_registry_good) - 1u &&
                         memcmp(now, k_registry_good, off) == 0 &&
                         memcmp(now + off, fresh, NK_CONSOLE_ID_HEX_LEN) == 0 &&
                         memcmp(now + off + NK_CONSOLE_ID_HEX_LEN, at + NK_CONSOLE_ID_HEX_LEN,
                                len - off - NK_CONSOLE_ID_HEX_LEN) == 0;
        CHECK(rest_same, "only the 12 digits change; the OpenPSID and every other byte stay");
        CHECK((hex_digit_value(fresh[1]) & 0x3) == 0,
              "the new ID's first octet has its two low bits clear, as the runtime makes it");
        free(now);
    }
    {
        char again[NK_CONSOLE_ID_HEX_LEN + 1u], leftover[NATIVE_TEST_PATH_MAX + 40];
        snprintf(leftover, sizeof(leftover), "%s.new", path);
        CHECK(nk_adhoc_console_id_read(path, again) == NK_CONSOLE_ID_OK &&
                  strcmp(again, fresh) == 0 && !nk_platform_file_exists(leftover),
              "the file reads back with the new ID and no temporary file is left");
    }

    /* Anything but the runtime's own form is left exactly as it is. */
    static const char twice[] =
        "{\"console\": {\"wlan_mac_hex\": \"a4a1b2c3d4e5\", \"open_psid_hex\": "
        "\"00112233445566778899aabbccddeeff\"}, \"copy\": {\"wlan_mac_hex\": \"a4a1b2c3d4e5\"}}";
    static const char odd_mac[] =
        "{\"console\": {\"wlan_mac_hex\": \"03a1b2c3d4e5\", \"open_psid_hex\": "
        "\"00112233445566778899aabbccddeeff\"}}";
    static const char zero_psid[] =
        "{\"console\": {\"wlan_mac_hex\": \"a4a1b2c3d4e5\", \"open_psid_hex\": "
        "\"00000000000000000000000000000000\"}}";
    static const char short_mac[] =
        "{\"console\": {\"wlan_mac_hex\": \"a4a1b2c3d4\", \"open_psid_hex\": "
        "\"00112233445566778899aabbccddeeff\"}}";
    static const char not_json[] = "{\"console\": {\"wlan_mac_hex\": \"a4a1b2c3d4e5\",";
    static const char escaped_key[] =
        "{\"console\": {\"wlan_mac_he\\u0078\": \"a4a1b2c3d4e5\", \"open_psid_hex\": "
        "\"00112233445566778899aabbccddeeff\"}}";
    const struct {
        const char *text;
        size_t len;
        const char *what;
    } bad[] = {
        {twice, sizeof(twice) - 1u, "a second wlan_mac_hex key"},
        {odd_mac, sizeof(odd_mac) - 1u, "a multicast first octet"},
        {zero_psid, sizeof(zero_psid) - 1u, "an all-zero OpenPSID"},
        {short_mac, sizeof(short_mac) - 1u, "a short ID"},
        {not_json, sizeof(not_json) - 1u, "text that is not JSON"},
        {escaped_key, sizeof(escaped_key) - 1u, "the key written with an escape"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        const NkConsoleIdStatus renew = write_file(path, bad[i].text, bad[i].len)
            ? nk_adhoc_console_id_renew(path, fresh) : NK_CONSOLE_ID_OK;
        CHECK(renew == NK_CONSOLE_ID_UNEXPECTED && fresh[0] == '\0' &&
                  file_is(path, bad[i].text, bad[i].len),
              "%s is refused as unexpected and the file is left alone", bad[i].what);
    }
    {
        const size_t big = 1024u * 1024u + 1u;
        char *huge = (char *)malloc(big);
        if (huge) {
            memset(huge, ' ', big);
            huge[0] = '{';
            huge[big - 1u] = '}';
            CHECK(write_file(path, huge, big) &&
                      nk_adhoc_console_id_renew(path, fresh) == NK_CONSOLE_ID_UNREADABLE &&
                      nk_adhoc_console_id_read(path, hex) == NK_CONSOLE_ID_UNREADABLE,
                  "a file over 1 MiB is not read");
            free(huge);
        }
    }
    for (int s = NK_CONSOLE_ID_OK; s <= NK_CONSOLE_ID_WRITE_FAILED; s++) {
        const char *text = nk_adhoc_console_id_status_text((NkConsoleIdStatus)s);
        CHECK(text && text[0], "console ID status %d has a sentence", s);
    }
    (void)nk_remove_utf8(path);
}

int main(void) {
    native_test_create_root("test_adhoc_servers");
    test_fetch_waits_for_online_play();
    test_fetch_refusals();
    test_hostile_entries();
    test_whole_list_refusals();
    test_shipped_copy();
    test_console_id();
    if (g_failures) {
        fprintf(stderr, "test_adhoc_servers: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_adhoc_servers: PASS\n");
    return 0;
}
