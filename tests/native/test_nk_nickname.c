/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The console nickname rule (#2197, src/core/nk_nickname.h) that the player's Nickname setting and
 * the runtime's SR_NICKNAME share: the neutral default, the 127-byte cap of the PSP's 128-byte
 * fields, and the refusal of an empty value, a control character and malformed UTF-8. */

#include "nk_nickname.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (what));     \
        }                                                                        \
    } while (0)

/* `count` copies of `fill`, then `tail`, in `out`. */
static const char *repeat(char *out, size_t cap, char fill, size_t count, const char *tail) {
    size_t tail_len = strlen(tail);
    if (count + tail_len + 1u > cap) return "";
    memset(out, fill, count);
    memcpy(out + count, tail, tail_len + 1u);
    return out;
}

static void test_default(void) {
    CHECK(strcmp(NK_NICKNAME_DEFAULT, "Player") == 0, "the default is the neutral \"Player\"");
    CHECK(nk_nickname_valid(NK_NICKNAME_DEFAULT), "the default passes the rule it is checked by");
}

static void test_length(void) {
    char text[300];
    CHECK(NK_NICKNAME_MAX_BYTES == 127u, "the cap is 127 bytes, a 128-byte field less its NUL");
    CHECK(nk_nickname_valid("A"), "one byte is enough");
    CHECK(nk_nickname_valid(repeat(text, sizeof(text), 'n', 127u, "")), "127 bytes are kept");
    CHECK(!nk_nickname_valid(repeat(text, sizeof(text), 'n', 128u, "")), "128 bytes are refused");
    CHECK(!nk_nickname_valid(repeat(text, sizeof(text), 'n', 200u, "")), "200 bytes are refused");
    /* The cap counts bytes, not characters: a two-byte character that ends at byte 127 fits,
       one that ends at byte 128 does not. */
    CHECK(nk_nickname_valid(repeat(text, sizeof(text), 'n', 125u, "\xC3\xA9")),
          "125 bytes and a two-byte character (127 bytes) are kept");
    CHECK(!nk_nickname_valid(repeat(text, sizeof(text), 'n', 126u, "\xC3\xA9")),
          "126 bytes and a two-byte character (128 bytes) are refused");
    CHECK(!nk_nickname_valid(repeat(text, sizeof(text), 'n', 124u, "\xF0\x9F\x8E\xAE")),
          "124 bytes and a four-byte character (128 bytes) are refused");
}

static void test_empty_and_null(void) {
    CHECK(!nk_nickname_valid(""), "an empty nickname is refused");
    CHECK(!nk_nickname_valid(NULL), "no nickname is refused");
}

static void test_control_characters(void) {
    static const char *const refused[] = {
        "a\x01" "b", "tab\tname", "line\nbreak", "\x1F", "del\x7F", "c1 \xC2\x80", "c1 \xC2\x9F",
    };
    for (size_t i = 0u; i < sizeof(refused) / sizeof(refused[0]); i++) {
        char what[96];
        snprintf(what, sizeof(what), "control-character case %u is refused", (unsigned)i);
        CHECK(!nk_nickname_valid(refused[i]), what);
    }
    CHECK(nk_nickname_valid("space ok"), "a space is not a control character");
    CHECK(nk_nickname_valid("nb\xC2\xA0sp"), "U+00A0, just past the C1 block, is kept");
}

static void test_utf8(void) {
    static const char *const refused[] = {
        "\x80", "a\xBF", "\xC3", "ab\xE3\x81", "\xC0\xAF", "\xC1\xBF", "\xE0\x80\xAF",
        "\xF0\x80\x80\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\xFF",
        "\xC3\x28",
    };
    for (size_t i = 0u; i < sizeof(refused) / sizeof(refused[0]); i++) {
        char what[96];
        snprintf(what, sizeof(what), "malformed UTF-8 case %u is refused", (unsigned)i);
        CHECK(!nk_nickname_valid(refused[i]), what);
    }
    CHECK(nk_nickname_valid("Ren\xC3\xA9"), "a two-byte character is kept");
    CHECK(nk_nickname_valid("\xE3\x83\x97\xE3\x83\xAC\xE3\x82\xA4\xE3\x83\xA4\xE3\x83\xBC"),
          "three-byte characters are kept");
    CHECK(nk_nickname_valid("\xF0\x9F\x8E\xAE"), "a four-byte character is kept");
    CHECK(nk_nickname_valid("\xF4\x8F\xBF\xBF"), "U+10FFFF is kept");
    CHECK(nk_nickname_valid("\xEE\x80\x80"), "a private-use character (U+E000) is kept");
}

int main(void) {
    test_default();
    test_length();
    test_empty_and_null();
    test_control_characters();
    test_utf8();
    if (g_failures != 0) {
        fprintf(stderr, "test_nk_nickname: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("test_nk_nickname: %d checks passed\n", g_checks);
    return 0;
}
