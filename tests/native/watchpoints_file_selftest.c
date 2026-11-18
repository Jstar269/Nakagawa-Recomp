// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// watchpoints_file_selftest.c — regression suite for the derived
// watchpoints.json parser (issue #188).
//
// The headline fixture exercises a canonical versioned watchpoint document
// with a fixed watchpoint set and timestamp. The rest of the suite covers the
// fail-closed contract: wrong
// version/format, malformed JSON, out-of-range/reversed/oversized spans,
// bad labels, duplicates, over-capacity lists, escapes, and the legacy
// bare-array form. No game inputs or private data required.

#include <stdio.h>
#include <string.h>

#include "watchpoints_file.h"
#include "strbuf.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, name)                                                      \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);           \
        }                                                                      \
    } while (0)

/* Canonical versioned watchpoint fixture with writtenAt pinned to
 * 2026-08-06T00:00:00.000Z and contentHash
 * a820c68f071a47e4197b85bdae23b70eb4e42a0c0d4ae7c2ad97f1a7809ab9c5. */
static const char *const CANONICAL_FIXTURE =
    "{\n"
    "  \"format\": \"nk-watchpoints\",\n"
    "  \"version\": 1,\n"
    "  \"profileId\": \"prof-integration\",\n"
    "  \"source\": \"db\",\n"
    "  \"writtenAt\": \"2026-08-06T00:00:00.000Z\",\n"
    "  \"contentHash\": \"a820c68f071a47e4197b85bdae23b70eb4e42a0c0d4ae7c2ad97f1a7809ab9c5\",\n"
    "  \"watchpoints\": [\n"
    "    {\n"
    "      \"start\": 134221824,\n"
    "      \"end\": 134222080,\n"
    "      \"label\": \"Font Engine\"\n"
    "    },\n"
    "    {\n"
    "      \"start\": 142606336,\n"
    "      \"end\": 142606592,\n"
    "      \"label\": \"Vertex Pool\"\n"
    "    }\n"
    "  ]\n"
    "}\n";

/* Legacy fixture with format "hst-watchpoints". Readers must continue to accept
 * this for backward compatibility (retirement tracked in #369). */
static const char *const LEGACY_HST_FIXTURE =
    "{\n"
    "  \"format\": \"hst-watchpoints\",\n"
    "  \"version\": 1,\n"
    "  \"profileId\": \"prof-integration\",\n"
    "  \"source\": \"db\",\n"
    "  \"writtenAt\": \"2026-08-06T00:00:00.000Z\",\n"
    "  \"contentHash\": \"a820c68f071a47e4197b85bdae23b70eb4e42a0c0d4ae7c2ad97f1a7809ab9c5\",\n"
    "  \"watchpoints\": [\n"
    "    {\n"
    "      \"start\": 134221824,\n"
    "      \"end\": 134222080,\n"
    "      \"label\": \"Font Engine\"\n"
    "    },\n"
    "    {\n"
    "      \"start\": 142606336,\n"
    "      \"end\": 142606592,\n"
    "      \"label\": \"Vertex Pool\"\n"
    "    }\n"
    "  ]\n"
    "}\n";

static void expect_watchpoint(const SrWatchpointEntry *e, uint32_t start, uint32_t end,
                              const char *label, const char *name) {
    CHECK(e->start == start && e->end == end && strcmp(e->label, label) == 0, name);
}

static void expect_parse(const char *json, int expected_count, const char *name) {
    SrWatchpointEntry out[16];
    char errbuf[256];
    int n = sr_parse_watchpoints_buffer(json, out, 16, errbuf, sizeof(errbuf));
    CHECK(n == expected_count, name);
}

static void expect_fail(const char *json, const char *name) {
    SrWatchpointEntry out[16];
    char errbuf[256];
    int n = sr_parse_watchpoints_buffer(json, out, 16, errbuf, sizeof(errbuf));
    CHECK(n == -1, name);
    if (n != -1) fprintf(stderr, "  unexpected success parsing: %.60s...\n", json);
}

/* Fill `out` with a pattern, parse a document that must fail, and require that `out` still
 * holds the pattern (the header's no-partial-fill rule). */
static void expect_untouched_on_failure(const char *json, const char *name) {
    SrWatchpointEntry out[16];
    SrWatchpointEntry pattern;
    char errbuf[256];
    memset(out, 0xA5, sizeof(out));
    memset(&pattern, 0xA5, sizeof(pattern));
    int n = sr_parse_watchpoints_buffer(json, out, 16, errbuf, sizeof(errbuf));
    CHECK(n == -1 && memcmp(&out[0], &pattern, sizeof(pattern)) == 0, name);
}

/* The scratch file lives next to this executable (the build directory under make and CI). */
static int scratch_path(char *out, size_t cap, const char *argv0) {
    static const char name[] = "watchpoints_file_selftest_scratch.json";
    size_t dir_len = 0;
    if (argv0) {
        const char *slash = strrchr(argv0, '/');
        const char *bslash = strrchr(argv0, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
        if (slash) dir_len = (size_t)(slash - argv0) + 1u;
    }
    if (dir_len + sizeof(name) > cap) return 0;
    if (dir_len > 0) memcpy(out, argv0, dir_len);
    memcpy(out + dir_len, name, sizeof(name));
    return 1;
}

static int write_scratch(const char *path, const char *bytes, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(bytes, 1, len, f) == len;
    if (fclose(f) != 0) ok = 0;
    return ok;
}

int main(int argc, char **argv) {
    SrWatchpointEntry out[16];
    char errbuf[256];

    /* 1. Canonical fixture parses to the expected runtime state. */
    {
        int n = sr_parse_watchpoints_buffer(CANONICAL_FIXTURE, out, 16, errbuf, sizeof(errbuf));
        CHECK(n == 2, "canonical fixture parses to 2 watchpoints");
        if (n == 2) {
            expect_watchpoint(&out[0], 0x08001000u, 0x08001100u, "Font Engine", "fixture watch 0");
            expect_watchpoint(&out[1], 0x08800000u, 0x08800100u, "Vertex Pool", "fixture watch 1");
        }
    }

    /* 1b. Legacy format reading compatibility (issue #369). */
    {
        int n = sr_parse_watchpoints_buffer(LEGACY_HST_FIXTURE, out, 16, errbuf, sizeof(errbuf));
        CHECK(n == 2, "legacy hst fixture parses to 2 watchpoints");
        if (n == 2) {
            expect_watchpoint(&out[0], 0x08001000u, 0x08001100u, "Font Engine", "legacy watch 0");
            expect_watchpoint(&out[1], 0x08800000u, 0x08800100u, "Vertex Pool", "legacy watch 1");
        }
    }

    /* 2. Legacy bare array (pre-#188 pretty-printed writer output). */
    expect_parse(
        "[\n  {\n    \"start\": 1,\n    \"end\": 17,\n    \"label\": \"a\"\n  },\n  {\n"
        "    \"start\": 32,\n    \"end\": 64,\n    \"label\": \"b c\"\n  }\n]\n",
        2, "legacy bare array");

    /* 3. Boundary semantics: start-inclusive / end-exclusive with start == 0
     * and end == UINT32_MAX are legal; zero span and reversed are not. */
    expect_parse("[{\"start\":0,\"end\":1,\"label\":\"zero-ok\"}]", 1, "start 0 legal");
    expect_parse("[{\"start\":4294967294,\"end\":4294967295,\"label\":\"top\"}]", 1, "end UINT32_MAX legal");
    expect_fail("[{\"start\":5,\"end\":5,\"label\":\"zero-span\"}]", "zero span rejected");
    expect_fail("[{\"start\":20,\"end\":10,\"label\":\"reversed\"}]", "reversed rejected");
    expect_fail("[{\"start\":0,\"end\":4294967296,\"label\":\"overflow\"}]", "> UINT32_MAX rejected");

    /* 4. Span bound (16 MiB) and label bounds. */
    expect_parse("[{\"start\":0,\"end\":16777216,\"label\":\"exact-span\"}]", 1, "span at exact bound");
    expect_fail("[{\"start\":0,\"end\":16777217,\"label\":\"over-span\"}]", "span over bound rejected");
    {
        /* 64 characters is the exact legal boundary; 65 is the first illegal. */
        char label64[SR_WATCHPOINT_LABEL_BUF];
        memset(label64, 'x', SR_WATCHPOINT_LABEL_MAX);
        label64[SR_WATCHPOINT_LABEL_MAX] = '\0';
        char buf64[192];
        snprintf(buf64, sizeof(buf64), "[{\"start\":1,\"end\":2,\"label\":\"%s\"}]", label64);
        expect_parse(buf64, 1, "64-char label accepted (exact boundary)");

        char label65[SR_WATCHPOINT_LABEL_BUF + 2];
        memset(label65, 'x', SR_WATCHPOINT_LABEL_MAX + 1);
        label65[SR_WATCHPOINT_LABEL_MAX + 1] = '\0';
        char buf65[192];
        snprintf(buf65, sizeof(buf65), "[{\"start\":1,\"end\":2,\"label\":\"%s\"}]", label65);
        expect_fail(buf65, "65-char label rejected (first illegal)");

        char big_label[80];
        memset(big_label, 'x', sizeof(big_label) - 1);
        big_label[sizeof(big_label) - 1] = '\0';
        char buf[160];
        snprintf(buf, sizeof(buf), "[{\"start\":1,\"end\":2,\"label\":\"%s\"}]", big_label);
        expect_fail(buf, "overlong label rejected");
    }
    expect_fail("[{\"start\":1,\"end\":2,\"label\":\"\"}]", "empty label rejected");
    expect_fail("[{\"start\":1,\"end\":2,\"label\":\"slash/bar\"}]", "non-canonical label rejected");
    expect_fail("[{\"start\":1,\"end\":2,\"label\":\"quote\\\"inject\"}]", "quote label rejected");

    /* 5. Escape decoding: \uXXXX may only produce canonical ASCII characters. */
    {
        SrWatchpointEntry e;
        const char *json = "[{\"start\":1,\"end\":2,\"label\":\"\\u005f\\u002dabc\"}]";
        int n = sr_parse_watchpoints_buffer(json, &e, 1, errbuf, sizeof(errbuf));
        CHECK(n == 1, "canonical \\u escapes parse");
        if (n == 1) CHECK(strcmp(e.label, "_-abc") == 0, "escape decoding");
        expect_fail("[{\"start\":1,\"end\":2,\"label\":\"\\u00e9\"}]", "non-ASCII escape rejected");
        expect_fail("[{\"start\":1,\"end\":2,\"label\":\"a\\\\b\\\"c\"}]", "escaped non-canonical chars rejected");
    }

    /* 6. Duplicates rejected. */
    expect_fail("[{\"start\":1,\"end\":10,\"label\":\"a\"},{\"start\":1,\"end\":10,\"label\":\"b\"}]",
                "exact duplicate range rejected");

    /* 7. Over-capacity rejected (no silent truncation). */
    {
        char buf[2048];
        size_t off = 0;
        buf[off++] = '[';
        for (int i = 0; i < 17; i++) {
            if (i > 0) buf[off++] = ',';
            off = sr_buf_append(buf, sizeof(buf), off,
                                "{\"start\":%d,\"end\":%d,\"label\":\"w%d\"}",
                                0x1000 + i * 0x100, 0x1000 + i * 0x100 + 0x10, i);
        }
        buf[off++] = ']';
        buf[off] = '\0';
        expect_fail(buf, "17 watchpoints rejected (capacity 16)");
        /* The 16-entry version is accepted. */
        size_t off16 = 0;
        buf[off16++] = '[';
        for (int i = 0; i < 16; i++) {
            if (i > 0) buf[off16++] = ',';
            off16 = sr_buf_append(buf, sizeof(buf), off16,
                                  "{\"start\":%d,\"end\":%d,\"label\":\"w%d\"}",
                                  0x2000 + i * 0x100, 0x2000 + i * 0x100 + 0x10, i);
        }
        buf[off16++] = ']';
        buf[off16] = '\0';
        expect_parse(buf, 16, "16 watchpoints accepted");
    }

    /* 8. Envelope contract: wrong version / wrong format / missing parts. */
    {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "{\"format\":\"nk-watchpoints\",\"version\":2,\"watchpoints\":[]}");
        expect_fail(buf, "unsupported version rejected");
        snprintf(buf, sizeof(buf),
                 "{\"format\":\"hst-watchpoints\",\"version\":2,\"watchpoints\":[]}");
        expect_fail(buf, "legacy unsupported version rejected");
        snprintf(buf, sizeof(buf),
                 "{\"format\":\"other\",\"version\":1,\"watchpoints\":[]}");
        expect_fail(buf, "wrong format rejected");
        snprintf(buf, sizeof(buf),
                 "{\"version\":1,\"watchpoints\":[]}");
        expect_fail(buf, "missing format rejected");
        snprintf(buf, sizeof(buf),
                 "{\"format\":\"nk-watchpoints\",\"version\":1}");
        expect_fail(buf, "missing watchpoints rejected");
    }

    /* 8b. An error after a non-empty "watchpoints" array still rejects the whole file: the
     * parsed count must not leak out as success. */
    expect_fail("{\"watchpoints\":[{\"start\":1,\"end\":2,\"label\":\"a\"}],"
                "\"version\":2,\"format\":\"nk-watchpoints\"}",
                "bad version after watchpoints rejected");
    expect_fail("{\"watchpoints\":[{\"start\":1,\"end\":2,\"label\":\"a\"}],"
                "\"version\":1,\"format\":\"bogus\"}",
                "bad format after watchpoints rejected");
    expect_fail("{\"format\":\"nk-watchpoints\",\"version\":1,"
                "\"watchpoints\":[{\"start\":1,\"end\":2,\"label\":\"a\"}] garbage}",
                "garbage after watchpoints inside envelope rejected");
    expect_fail("{\"format\":\"nk-watchpoints\",\"version\":1,"
                "\"watchpoints\":[{\"start\":1,\"end\":2,\"label\":\"a\"}]",
                "envelope missing its closing brace rejected");

    /* 8c. A repeated envelope key is rejected, never last-one-wins. */
    expect_fail("{\"format\":\"nk-watchpoints\",\"version\":1,"
                "\"watchpoints\":[{\"start\":1,\"end\":2,\"label\":\"a\"},"
                "{\"start\":3,\"end\":4,\"label\":\"b\"}],"
                "\"watchpoints\":[{\"start\":5,\"end\":6,\"label\":\"c\"}]}",
                "repeated watchpoints key rejected");
    expect_fail("{\"format\":\"nk-watchpoints\",\"format\":\"nk-watchpoints\",\"version\":1,"
                "\"watchpoints\":[]}",
                "repeated format key rejected");
    expect_fail("{\"format\":\"nk-watchpoints\",\"version\":1,\"version\":1,"
                "\"watchpoints\":[]}",
                "repeated version key rejected");

    /* 8d. A rejected file leaves `out` untouched, even when valid entries came first. */
    expect_untouched_on_failure("[{\"start\":1,\"end\":2,\"label\":\"a\"},"
                                "{\"start\":3,\"end\":4,\"label\":\"b\"},"
                                "{\"start\":9,\"end\":5,\"label\":\"reversed\"}]",
                                "bare array: out untouched when the third entry is bad");
    expect_untouched_on_failure("{\"format\":\"nk-watchpoints\","
                                "\"watchpoints\":[{\"start\":1,\"end\":2,\"label\":\"a\"}],"
                                "\"version\":2}",
                                "envelope: out untouched when the version is bad");

    /* 8e. A \u0000 escape is rejected: it would cut a label or key short before the
     * charset and unknown-key checks see the rest. */
    expect_fail("[{\"start\":1,\"end\":2,\"label\":\"ok\\u0000$$\"}]",
                "NUL escape in a label rejected");
    expect_fail("[{\"start\":1,\"end\\u0000junk\":2,\"label\":\"x\"}]",
                "NUL escape in a key rejected");

    /* 9. Malformed JSON shapes fail closed. */
    expect_fail("", "empty input rejected");
    expect_fail("{", "truncated envelope rejected");
    expect_fail("[{\"start\":1,\"end\":2}]", "missing label rejected");
    expect_fail("[{\"start\":1,\"end\":2,\"label\":\"x\",\"extra\":1}]", "unknown key rejected");
    expect_fail("[{\"start\":0x10,\"end\":32,\"label\":\"x\"}]", "unquoted hex number rejected (decimal only)");
    expect_fail("[{\"start\":\"0x10\",\"end\":2,\"label\":\"x\"}]", "string where a number is expected rejected");
    expect_fail("[{\"start\":-1,\"end\":2,\"label\":\"x\"}]", "negative number rejected");
    expect_fail("[{\"start\":1.5,\"end\":2,\"label\":\"x\"}]", "fractional number rejected");
    expect_fail("[{\"start\":1e3,\"end\":2000,\"label\":\"x\"}]", "exponent number rejected");
    expect_fail("[{\"start\":1,\"end\":2,\"label\":\"x\"}]extra", "trailing garbage rejected");

    /* 10. Empty list is valid in both canonical and legacy formats. */
    expect_parse("{\"format\":\"nk-watchpoints\",\"version\":1,\"watchpoints\":[]}",
                 0, "empty watchpoints list (canonical)");
    expect_parse("{\"format\":\"hst-watchpoints\",\"version\":1,\"watchpoints\":[]}",
                 0, "empty watchpoints list (legacy)");

    /* 11. File-level behavior: missing file and bounded read. */
    {
        int n = sr_parse_watchpoints_file(
            "watchpoints_selftest_does_not_exist.json", out, 16, errbuf, sizeof(errbuf));
        CHECK(n == -1, "missing file rejected");
        CHECK(strlen(errbuf) > 0, "error message populated");
    }

    /* 11b. The read path the runtime uses: a real file next to this executable. */
    {
        char path[1024];
        if (!scratch_path(path, sizeof(path), argc > 0 ? argv[0] : NULL)) {
            CHECK(0, "scratch path fits");
        } else {
            static char doc[SR_WATCHPOINTS_FILE_MAX_BYTES + 1u];
            size_t fixture_len = strlen(CANONICAL_FIXTURE);

            /* The canonical fixture read from disk parses to its two entries. */
            int n = -2;
            if (write_scratch(path, CANONICAL_FIXTURE, fixture_len))
                n = sr_parse_watchpoints_file(path, out, 16, errbuf, sizeof(errbuf));
            CHECK(n == 2, "canonical fixture read from a file");
            if (n == 2) {
                expect_watchpoint(&out[0], 0x08001000u, 0x08001100u, "Font Engine", "file watch 0");
                expect_watchpoint(&out[1], 0x08800000u, 0x08800100u, "Vertex Pool", "file watch 1");
            }

            /* Exactly the size bound (fixture padded with whitespace) is read. */
            memcpy(doc, CANONICAL_FIXTURE, fixture_len);
            memset(doc + fixture_len, ' ', SR_WATCHPOINTS_FILE_MAX_BYTES - fixture_len);
            n = -2;
            if (write_scratch(path, doc, SR_WATCHPOINTS_FILE_MAX_BYTES))
                n = sr_parse_watchpoints_file(path, out, 16, errbuf, sizeof(errbuf));
            CHECK(n == 2, "file of exactly the size bound accepted");

            /* One byte over the bound is refused by the size check. */
            doc[SR_WATCHPOINTS_FILE_MAX_BYTES] = ' ';
            n = -2;
            errbuf[0] = '\0';
            if (write_scratch(path, doc, SR_WATCHPOINTS_FILE_MAX_BYTES + 1u))
                n = sr_parse_watchpoints_file(path, out, 16, errbuf, sizeof(errbuf));
            CHECK(n == -1 && strstr(errbuf, "size bound") != NULL,
                  "file over the size bound refused");

            /* Bytes after an embedded NUL are not ignored: the file is refused. */
            memcpy(doc, CANONICAL_FIXTURE, fixture_len);
            doc[fixture_len] = '\0';
            memcpy(doc + fixture_len + 1, "{garbage", 8);
            n = -2;
            if (write_scratch(path, doc, fixture_len + 9))
                n = sr_parse_watchpoints_file(path, out, 16, errbuf, sizeof(errbuf));
            CHECK(n == -1, "file with an embedded NUL refused");

            remove(path);
        }
    }

    if (g_failures == 0) {
        printf("watchpoints_file_selftest: all %d checks passed\n", g_checks);
        return 0;
    }
    fprintf(stderr, "watchpoints_file_selftest: %d/%d checks FAILED\n", g_failures, g_checks);
    return 1;
}
