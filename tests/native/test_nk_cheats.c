/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The CWCheat reader, its checks and its applier (#2639, src/core/nk_cheats.c), against a
 * synthetic guest: 64 KiB of user memory with a stretch marked as translated code. Every code
 * form nk_cheats.h lists is run and its bytes read back; every refusal is asked for by its
 * line and its reason; and a cheat that cannot finish a frame leaves memory as it found it.
 * The runtime's own path (the VBLANK, guest memory, the pad) is hle_thread_selftest's. */

#include "nk_cheats.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

/* ---- the synthetic guest -------------------------------------------------------------------- */

#define T_BASE 0x08800000u
#define T_SIZE 0x10000u
#define T_END (T_BASE + T_SIZE)
#define T_CODE_LO 0x08804000u
#define T_CODE_HI 0x08804100u

static uint8_t g_mem[T_SIZE];
static uint32_t g_buttons;
static char g_reports[8][512];
static int g_report_count;
static int g_bad_access;    /* a read or write the applier had not checked, or a write into code */

static void t_read(void *ctx, uint32_t addr, void *out, uint32_t size) {
    (void)ctx;
    if (addr < T_BASE || addr >= T_END || size > T_END - addr) {
        g_bad_access++;
        memset(out, 0, size);
        return;
    }
    memcpy(out, g_mem + (addr - T_BASE), size);
}

static bool t_is_code(void *ctx, uint32_t addr, uint32_t size) {
    (void)ctx;
    return size != 0u && addr < T_CODE_HI && (uint64_t)addr + size > T_CODE_LO;
}

static void t_write(void *ctx, uint32_t addr, const void *in, uint32_t size) {
    if (addr < T_BASE || addr >= T_END || size > T_END - addr || t_is_code(ctx, addr, size)) {
        g_bad_access++;
        return;
    }
    memcpy(g_mem + (addr - T_BASE), in, size);
}

static uint32_t t_buttons(void *ctx) {
    (void)ctx;
    return g_buttons;
}

static void t_report(void *ctx, const char *line) {
    (void)ctx;
    if (g_report_count < 8) snprintf(g_reports[g_report_count], sizeof(g_reports[0]), "%s", line);
    g_report_count++;
}

static const NkCheatHost k_host = { NULL, t_read, t_write, t_is_code, t_buttons, t_report };
static const NkCheatConfig k_config = { "ULUS-12345", T_BASE, T_END };

static uint8_t r8(uint32_t off) { return g_mem[off]; }
static uint16_t r16(uint32_t off) { return (uint16_t)(g_mem[off] | (g_mem[off + 1u] << 8)); }
static uint32_t r32(uint32_t off) {
    return (uint32_t)g_mem[off] | ((uint32_t)g_mem[off + 1u] << 8) | ((uint32_t)g_mem[off + 2u] << 16) |
           ((uint32_t)g_mem[off + 3u] << 24);
}
static void w8(uint32_t off, uint8_t v) { g_mem[off] = v; }
static void w16(uint32_t off, uint16_t v) { g_mem[off] = (uint8_t)v; g_mem[off + 1u] = (uint8_t)(v >> 8); }
static void w32(uint32_t off, uint32_t v) {
    w16(off, (uint16_t)v);
    w16(off + 2u, (uint16_t)(v >> 16));
}

static void guest_reset(void) {
    memset(g_mem, 0, sizeof(g_mem));
    g_buttons = 0u;
    g_report_count = 0;
}

static NkCheatSet *load_as(const char *text, const NkCheatConfig *config, const NkCheatHost *host) {
    char error[128];
    NkCheatSet *set = nk_cheats_load(text, strlen(text), config, host, error, sizeof(error));
    CHECK(set != NULL && error[0] == '\0', "a cheat file's text loads");
    return set;
}

static NkCheatSet *load(const char *text) {
    return load_as(text, &k_config, &k_host);
}

/* Loads one cheat that is on, made of these code lines, checks that it is accepted, and applies
 * it once. */
static void run_codes(const char *codes) {
    char text[2048];
    NkCheatInfo info;
    NkCheatSet *set;
    snprintf(text, sizeof(text), "_C1 test\n%s", codes);
    set = load(text);
    memset(&info, 0, sizeof(info));
    CHECK(nk_cheats_count(set) == 1u && nk_cheats_info(set, 0u, &info) && info.state == NK_CHEAT_ON,
          codes);
    if (info.state != NK_CHEAT_ON && info.reason) fprintf(stderr, "  refused: %s\n", info.reason);
    CHECK(nk_cheats_apply(set, &k_host) == 1u, "the accepted cheat applies");
    nk_cheats_free(set);
}

/* Loads one cheat that is on, made of these code lines, and checks that it is refused at file
 * line 'line' for a reason holding 'needle', and that applying it changes nothing. */
static void refused_at(const char *codes, uint32_t line, const char *needle) {
    char text[2048];
    uint8_t *before = (uint8_t *)malloc(sizeof(g_mem));
    NkCheatInfo info;
    NkCheatSet *set;
    snprintf(text, sizeof(text), "_C1 test\n%s", codes);
    set = load(text);
    memset(&info, 0, sizeof(info));
    CHECK(nk_cheats_count(set) == 1u && nk_cheats_info(set, 0u, &info) && info.state == NK_CHEAT_REFUSED,
          codes);
    CHECK(info.refused_line == line, codes);
    CHECK(info.reason && strstr(info.reason, needle) != NULL, codes);
    if (info.state != NK_CHEAT_REFUSED || info.refused_line != line || !info.reason ||
        !strstr(info.reason, needle)) {
        fprintf(stderr, "  wanted line %u and \"%s\"; got line %u and \"%s\"\n", (unsigned)line, needle,
                (unsigned)info.refused_line, info.reason ? info.reason : "");
    }
    if (before) memcpy(before, g_mem, sizeof(g_mem));
    CHECK(nk_cheats_apply(set, &k_host) == 0u, "a refused cheat does not apply");
    CHECK(before && memcmp(before, g_mem, sizeof(g_mem)) == 0, "a refused cheat writes nothing");
    free(before);
    nk_cheats_free(set);
}

/* ---- the file ------------------------------------------------------------------------------- */

static void test_file_lines(void) {
    static const char text[] =
        "\xEF\xBB\xBF" "// a comment\r\n"
        "_L 0x00000010 0x00000001\r\n"            /* before any cheat: a note */
        "_S ULUS-12345\r\n"
        "_G A game\r\n"
        "_C0 Off cheat\r\n"
        "_L 0x00000020 0x00000002\r\n"
        "_C1 On cheat  \r\n"
        "  _L 0x00000021 0x00000003   # trailing comment\r\n"
        "_L 00000022 4 // short words and no 0x\r\n"
        "# another comment\r\n"
        "\r\n"
        "_C9 Also on\r\n"
        "_L 0x00000023 0x00000005\r\n"
        "_S NPJH-99999\r\n"
        "_C1 Another game's\r\n"
        "_L 0x00000024 0x00000006\r\n"
        "this line is skipped with its section\r\n"
        "_C0 Another game's too\r\n"
        "_S ulus12345\r\n"                         /* the same disc, written another way */
        "_C1 Back again\r\n"
        "_L 0x00000025 0x00000007";
    NkCheatSummary sum;
    NkCheatInfo info;
    NkCheatSet *set;
    guest_reset();
    set = load(text);
    nk_cheats_summary(set, &sum);
    CHECK(nk_cheats_count(set) == 4u && sum.on == 3u && sum.off == 1u && sum.refused == 0u &&
              sum.other_game == 2u && sum.notes == 1u,
          "three cheats on, one off, two under another disc, one note");
    CHECK(nk_cheats_note_count(set) == 1u && strstr(nk_cheats_note(set, 0u), "line 2:") &&
              strstr(nk_cheats_note(set, 0u), "before any _C line"),
          "a code line before any cheat is noted with its line");
    CHECK(nk_cheats_info(set, 0u, &info) && info.state == NK_CHEAT_OFF &&
              strcmp(info.name, "Off cheat") == 0 && info.file_line == 5u,
          "_C0 is a cheat that is off, with its name and line");
    CHECK(nk_cheats_info(set, 1u, &info) && info.state == NK_CHEAT_ON && strcmp(info.name, "On cheat") == 0,
          "_C1 is on, and its name is trimmed");
    CHECK(nk_cheats_info(set, 2u, &info) && info.state == NK_CHEAT_ON, "_C9 is on, as in PPSSPP");
    CHECK(!nk_cheats_info(set, 4u, &info), "there is no fifth cheat");
    CHECK(nk_cheats_apply(set, &k_host) == 3u, "the three cheats that are on apply");
    CHECK(r8(0x10u) == 0u && r8(0x20u) == 0u && r8(0x24u) == 0u,
          "the stray line, the off cheat and the other disc's cheat write nothing");
    CHECK(r8(0x21u) == 3u && r8(0x22u) == 4u && r8(0x23u) == 5u && r8(0x25u) == 7u,
          "every cheat that is on wrote its bytes");
    CHECK(nk_cheats_info(set, 1u, &info) && info.applied_frames == 1u && info.held_frames == 0u && !info.held,
          "an applied frame is counted");
    nk_cheats_free(set);

    /* No _S line: the file is for the disc it is read for. No disc ID: only such a file applies. */
    {
        const NkCheatConfig no_disc = { NULL, T_BASE, T_END };
        set = load_as("_C1 a\n_L 0x00000030 0x00000009\n_S ULUS-12345\n_C1 b\n_L 0x00000031 0x1\n", &no_disc, &k_host);
        nk_cheats_summary(set, &sum);
        CHECK(set && sum.on == 1u && sum.other_game == 1u,
              "with no disc ID, a section that names a disc is another game's");
        nk_cheats_free(set);
    }

    /* Names: control characters and bad UTF-8 become '?', a quote an apostrophe, and a long name
     * is cut between characters. */
    {
        char long_name[400];
        char body[512];
        memset(long_name, 'n', sizeof(long_name) - 1u);
        long_name[sizeof(long_name) - 1u] = '\0';
        memcpy(long_name + 94u, "\xE6\x97\xA5", 3u);   /* a 3-byte character across the cut */
        snprintf(body, sizeof(body), "_C1 a\x01\"b\xFF\xC3\xA9\n_L 0x00000040 0x1\n_C1 %s\n_L 0x00000041 0x1\n_C1\n_L 0x00000042 0x1\n",
                 long_name);
        set = load(body);
        CHECK(nk_cheats_info(set, 0u, &info) && strcmp(info.name, "a?'b?\xC3\xA9") == 0,
              "a name is cleaned for a log line");
        CHECK(nk_cheats_info(set, 1u, &info) && strlen(info.name) == 94u,
              "a long name is cut before the character that does not fit");
        CHECK(nk_cheats_info(set, 2u, &info) && strcmp(info.name, "(unnamed)") == 0 && info.state == NK_CHEAT_ON,
              "a cheat with no name is still a cheat");
        nk_cheats_free(set);
    }

    /* What cannot be held at all. */
    {
        char error[128] = "";
        const NkCheatConfig empty = { "X", T_BASE, T_BASE };
        CHECK(nk_cheats_load("x", (size_t)NK_CHEATS_MAX_FILE_BYTES + 1u, &k_config, NULL, error, sizeof(error)) == NULL &&
                  strstr(error, "larger than 64 MiB"),
              "a file over the size limit is refused whole, by name");
        CHECK(nk_cheats_load("x", 1u, &empty, NULL, error, sizeof(error)) == NULL && error[0],
              "a reader with no user memory is refused");
        CHECK(nk_cheats_load("x", 1u, NULL, NULL, error, sizeof(error)) == NULL, "so is one with no config");
        set = nk_cheats_load(NULL, 0u, &k_config, NULL, error, sizeof(error));
        CHECK(set && nk_cheats_count(set) == 0u, "an empty file holds no cheat");
        CHECK(nk_cheats_apply(set, &k_host) == 0u && nk_cheats_apply(NULL, &k_host) == 0u &&
                  nk_cheats_apply(set, NULL) == 0u,
              "applying nothing applies nothing");
        nk_cheats_free(set);
        nk_cheats_free(NULL);
    }
}

/* ---- every code form that runs -------------------------------------------------------------- */

static void test_writes(void) {
    guest_reset();
    memset(g_mem + 0x100u, 0x77, 0x10u);
    run_codes("_L 0x00000101 0x000000AB\n_L 0x10000104 0x0000BEEF\n_L 0x20000108 0xDEADBEEF\n");
    CHECK(r8(0x100u) == 0x77u && r8(0x101u) == 0xABu && r8(0x102u) == 0x77u, "0x0 writes one byte");
    CHECK(r16(0x104u) == 0xBEEFu && r8(0x103u) == 0x77u && r8(0x106u) == 0x77u, "0x1 writes two bytes");
    CHECK(r32(0x108u) == 0xDEADBEEFu && r8(0x107u) == 0x77u && r8(0x10Cu) == 0x77u, "0x2 writes four bytes");
    /* The last bytes of user memory are writable; one past them is not (test_refusals). */
    run_codes("_L 0x0000FFFF 0x00000011\n_L 0x1000FFFE 0x00002233\n_L 0x2000FFFC 0x44556677\n");
    CHECK(r32(0xFFFCu) == 0x44556677u, "a write may end at the last byte of user memory");
}

static void test_add_and_subtract(void) {
    guest_reset();
    w8(0x110u, 0xFEu);
    w8(0x112u, 0x01u);
    w16(0x114u, 0xFFFFu);
    w16(0x116u, 0x0000u);
    w32(0x118u, 0xFFFFFFFFu);
    w32(0x11Cu, 5u);
    run_codes("_L 0x30100003 0x00000110\n"
              "_L 0x30200002 0x00000112\n"
              "_L 0x30300002 0x00000114\n"
              "_L 0x30400001 0x00000116\n"
              "_L 0x30500000 0x00000118\n_L 0x00000002 0x00000000\n"
              "_L 0x30600000 0x0000011C\n_L 0x00000007 0x00000000\n");
    CHECK(r8(0x110u) == 0x01u && r8(0x111u) == 0u, "0x301 adds to 8 bits and wraps inside them");
    CHECK(r8(0x112u) == 0xFFu && r8(0x113u) == 0u, "0x302 subtracts from 8 bits");
    CHECK(r16(0x114u) == 0x0001u, "0x303 adds to 16 bits");
    CHECK(r16(0x116u) == 0xFFFFu, "0x304 subtracts from 16 bits");
    CHECK(r32(0x118u) == 0x00000001u, "0x305 adds the next line's value to 32 bits");
    CHECK(r32(0x11Cu) == 0xFFFFFFFEu, "0x306 subtracts it");
}

static void test_multi_writes(void) {
    guest_reset();
    memset(g_mem + 0x200u, 0x55, 0x40u);
    run_codes("_L 0x40000200 0x00030002\n_L 0x00000010 0x00000001\n");
    CHECK(r32(0x200u) == 0x10u && r32(0x208u) == 0x11u && r32(0x210u) == 0x12u, "0x4 writes three growing values");
    CHECK(r32(0x204u) == 0x55555555u && r32(0x20Cu) == 0x55555555u && r32(0x214u) == 0x55555555u,
          "0x4 leaves the words between and after them");
    memset(g_mem + 0x300u, 0x55, 0x40u);
    run_codes("_L 0x80000300 0x00040003\n_L 0x000000F0 0x00000008\n");
    CHECK(r8(0x300u) == 0xF0u && r8(0x303u) == 0xF8u && r8(0x306u) == 0x00u && r8(0x309u) == 0x08u,
          "0x8 writes four bytes three apart, the value wrapping in 8 bits");
    CHECK(r8(0x301u) == 0x55u && r8(0x30Au) == 0x55u, "0x8 leaves the bytes between and after them");
    run_codes("_L 0x80000320 0x00020002\n_L 0x1000ABCD 0x00000001\n");
    CHECK(r16(0x320u) == 0xABCDu && r16(0x324u) == 0xABCEu && r16(0x322u) == 0x5555u,
          "0x8 with 0x1000vvvv writes 16-bit values four bytes apart");
    run_codes("_L 0x40000330 0x00020000\n_L 0x00000001 0x00000001\n");
    CHECK(r32(0x330u) == 2u, "a step of 0 writes the same word each time");
}

static void test_copy(void) {
    guest_reset();
    for (uint32_t i = 0u; i < 3000u; i++) g_mem[0x1000u + i] = (uint8_t)(i * 7u + 1u);
    memset(g_mem + 0x2000u, 0xEE, 3200u);
    run_codes("_L 0x50001000 0x00000BB8\n_L 0x00002010 0x00000000\n");
    CHECK(memcmp(g_mem + 0x2010u, g_mem + 0x1000u, 3000u) == 0, "0x5 copies every byte, across its blocks");
    CHECK(r8(0x200Fu) == 0xEEu && r8(0x2010u + 3000u) == 0xEEu, "0x5 writes nothing before or after");
}

static void test_pointers(void) {
    guest_reset();
    w32(0x600u, T_BASE + 0x700u);
    run_codes("_L 0x60000600 0x000000AA\n_L 0x00000001 0x00000010\n"
              "_L 0x60000600 0x0000BBBB\n_L 0x00010001 0x00000012\n"
              "_L 0x60000600 0xCCCCDDDD\n_L 0x00020001 0x00000014\n"
              "_L 0x60000600 0x000000A1\n_L 0x00030001 0x00000004\n"
              "_L 0x60000600 0x0000B2B2\n_L 0x00040001 0x00000008\n"
              "_L 0x60000600 0xC3C3C3C3\n_L 0x00050001 0x0000000C\n");
    CHECK(r8(0x710u) == 0xAAu && r16(0x712u) == 0xBBBBu && r32(0x714u) == 0xCCCCDDDDu,
          "0x6 types 0, 1, 2 write 8, 16, 32 bits at pointer + offset");
    CHECK(r8(0x6FCu) == 0xA1u && r16(0x6F8u) == 0xB2B2u && r32(0x6F4u) == 0xC3C3C3C3u,
          "0x6 types 3, 4, 5 write at pointer - offset");
    CHECK(r32(0x600u) == T_BASE + 0x700u, "the pointer itself is only read");

    /* A stored pointer with the kernel-mode address bit names the same memory. */
    w32(0x600u, 0x40000000u | (T_BASE + 0x700u));
    run_codes("_L 0x60000600 0x00000099\n_L 0x00000001 0x00000020\n");
    CHECK(r8(0x720u) == 0x99u, "a pointer with bit 30 set is followed to the same user memory");

    /* Pointer steps: one per word, forwards (0x2) or backwards (0x3). */
    w32(0x600u, T_BASE + 0x700u);
    w32(0x720u, T_BASE + 0x800u);
    w32(0x7F0u, T_BASE + 0x900u);
    run_codes("_L 0x60000600 0x000000CC\n_L 0x00000002 0x00000004\n_L 0x20000020 0x00000000\n");
    CHECK(r8(0x804u) == 0xCCu, "one pointer step: the pointer stored at pointer + 0x20 is followed");
    run_codes("_L 0x60000600 0x000000DD\n_L 0x00000002 0x00000004\n_L 0x20000020 0x30000010\n");
    CHECK(r8(0x904u) == 0xDDu, "two steps on one line, the second backwards");
    run_codes("_L 0x60000600 0x000000EE\n_L 0x00000003 0x00000008\n_L 0x20000020 0x00000000\n_L 0x30000010 0x00000000\n");
    CHECK(r8(0x908u) == 0xEEu, "two steps on two lines");
}

static void test_boolean(void) {
    guest_reset();
    w8(0xA00u, 0x0Fu);
    w16(0xA10u, 0x0F0Fu);
    run_codes("_L 0x70000A00 0x000000F0\n");
    CHECK(r8(0xA00u) == 0xFFu, "0x7 operation 0 is an 8-bit OR");
    run_codes("_L 0x70000A00 0x000200F0\n");
    CHECK(r8(0xA00u) == 0xF0u, "operation 2 is an 8-bit AND");
    run_codes("_L 0x70000A00 0x000400FF\n");
    CHECK(r8(0xA00u) == 0x0Fu && r8(0xA01u) == 0u, "operation 4 is an 8-bit XOR");
    run_codes("_L 0x70000A10 0x0001F000\n");
    CHECK(r16(0xA10u) == 0xFF0Fu, "operation 1 is a 16-bit OR");
    run_codes("_L 0x70000A10 0x00030FF0\n");
    CHECK(r16(0xA10u) == 0x0F00u, "operation 3 is a 16-bit AND");
    run_codes("_L 0x70000A10 0x0005FFFF\n");
    CHECK(r16(0xA10u) == 0xF0FFu && r8(0xA12u) == 0u, "operation 5 is a 16-bit XOR");
}

/* One test line and one guarded write: whether the write ran. */
static bool guarded_write_runs(const char *test_line) {
    char codes[256];
    w8(0xB10u, 0u);
    snprintf(codes, sizeof(codes), "%s\n_L 0x00000B10 0x00000001\n", test_line);
    run_codes(codes);
    return r8(0xB10u) == 1u;
}

static void test_value_conditions(void) {
    static const struct { const char *line; uint16_t memory; bool runs; const char *what; } cases[] = {
        { "_L 0xD0000B00 0x00001234", 0x1234u, true, "0xD 16-bit equal, equal" },
        { "_L 0xD0000B00 0x00001234", 0x1235u, false, "0xD 16-bit equal, different" },
        { "_L 0xD0000B00 0x00101234", 0x1235u, true, "0xD 16-bit not equal, different" },
        { "_L 0xD0000B00 0x00101234", 0x1234u, false, "0xD 16-bit not equal, equal" },
        { "_L 0xD0000B00 0x00201234", 0x1233u, true, "0xD 16-bit less: memory below the value" },
        { "_L 0xD0000B00 0x00201234", 0x1234u, false, "0xD 16-bit less: equal is not less" },
        { "_L 0xD0000B00 0x00208000", 0x7FFFu, true, "0xD 16-bit less is unsigned" },
        { "_L 0xD0000B00 0x00301234", 0x1235u, true, "0xD 16-bit greater: memory above the value" },
        { "_L 0xD0000B00 0x00301234", 0x1234u, false, "0xD 16-bit greater: equal is not greater" },
        { "_L 0xD0000B00 0x20000034", 0x1234u, true, "0xD 8-bit equal reads one byte" },
        { "_L 0xD0000B00 0x20000034", 0x1235u, false, "0xD 8-bit equal, different" },
        { "_L 0xD0000B00 0x20100034", 0x1235u, true, "0xD 8-bit not equal" },
        { "_L 0xD0000B00 0x20200034", 0xFF33u, true, "0xD 8-bit less" },
        { "_L 0xD0000B00 0x20300034", 0x0035u, true, "0xD 8-bit greater" },
        { "_L 0xD0000B00 0x20300034", 0xFF34u, false, "0xD 8-bit greater: equal is not greater" },
        { "_L 0xE0011234 0x00000B00", 0x1234u, true, "0xE0 16-bit equal" },
        { "_L 0xE0011234 0x00000B00", 0x0234u, false, "0xE0 16-bit equal, different" },
        { "_L 0xE0011234 0x10000B00", 0x0234u, true, "0xE0 16-bit not equal" },
        { "_L 0xE0011234 0x20000B00", 0x1233u, true, "0xE0 16-bit less" },
        { "_L 0xE0011234 0x30000B00", 0x1233u, false, "0xE0 16-bit greater, less" },
        { "_L 0xE1010034 0x00000B00", 0xFF34u, true, "0xE1 8-bit equal" },
        { "_L 0xE1010034 0x30000B00", 0x0035u, true, "0xE1 8-bit greater" },
        { "_L 0xE1010034 0x20000B00", 0x0035u, false, "0xE1 8-bit less, greater" },
    };
    guest_reset();
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        w16(0xB00u, cases[i].memory);
        CHECK(guarded_write_runs(cases[i].line) == cases[i].runs, cases[i].what);
    }

    /* 0xE guards its count of lines, and only those. */
    w16(0xB00u, 0x1111u);
    memset(g_mem + 0xB20u, 0, 4u);
    run_codes("_L 0xE0022222 0x00000B00\n_L 0x00000B20 0x1\n_L 0x00000B21 0x1\n_L 0x00000B22 0x1\n");
    CHECK(r8(0xB20u) == 0u && r8(0xB21u) == 0u && r8(0xB22u) == 1u,
          "a failed 0xE test skips its two lines and runs the third");
    w16(0xB00u, 0x2222u);
    run_codes("_L 0xE0022222 0x00000B00\n_L 0x00000B20 0x1\n_L 0x00000B21 0x1\n_L 0x00000B22 0x1\n");
    CHECK(r8(0xB20u) == 1u && r8(0xB21u) == 1u, "a passed 0xE test runs them");

    /* A test may guard another test, and a multi-line code counts as its lines. */
    memset(g_mem + 0xB20u, 0, 0x20u);
    w16(0xB00u, 0x2222u);
    run_codes("_L 0xE0042222 0x00000B00\n"
              "_L 0xD0000B00 0x00009999\n"       /* fails: skips the next line only */
              "_L 0x00000B20 0x1\n"
              "_L 0x40000B24 0x00020001\n_L 0x00000007 0x00000000\n"
              "_L 0x00000B30 0x1\n");
    CHECK(r8(0xB20u) == 0u && r32(0xB24u) == 7u && r32(0xB28u) == 7u && r8(0xB30u) == 1u,
          "nested tests each skip their own lines");
}

static void test_address_conditions(void) {
    static const struct { const char *test; uint32_t a, b; bool runs; const char *what; } cases[] = {
        { "_L 0xD0000C00 0x40000C04\n_L 0x00000001 0x00000002", 0x11223344u, 0x11223344u, true, "32-bit equal" },
        { "_L 0xD0000C00 0x40000C04\n_L 0x00000001 0x00000002", 0x11223344u, 0x91223344u, false, "32-bit equal, different" },
        { "_L 0xD0000C00 0x50000C04\n_L 0x00000001 0x00000002", 0x11223344u, 0x91223344u, true, "32-bit not equal" },
        { "_L 0xD0000C00 0x40000C04\n_L 0x00000001 0x00000001", 0xAAAA3344u, 0xBBBB3344u, true, "16-bit equal reads two bytes" },
        { "_L 0xD0000C00 0x60000C04\n_L 0x00000001 0x00000001", 0x00007FFFu, 0x00008000u, true, "16-bit less is unsigned" },
        { "_L 0xD0000C00 0x70000C04\n_L 0x00000001 0x00000001", 0x00007FFFu, 0x00008000u, false, "16-bit greater, less" },
        { "_L 0xD0000C00 0x40000C04\n_L 0x00000001 0x00000000", 0xAAAAAA44u, 0xBBBBBB44u, true, "8-bit equal reads one byte" },
        { "_L 0xD0000C00 0x70000C04\n_L 0x00000001 0x00000000", 0x000000FFu, 0x0000007Fu, true, "8-bit greater is unsigned" },
        { "_L 0xD0000C00 0x60000C04\n_L 0x00000001 0x00000000", 0x000000FFu, 0x0000007Fu, false, "8-bit less, greater" },
    };
    guest_reset();
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        w32(0xC00u, cases[i].a);
        w32(0xC04u, cases[i].b);
        CHECK(guarded_write_runs(cases[i].test) == cases[i].runs, cases[i].what);
    }
}

static void test_button_conditions(void) {
    static const char pressed[] = "_L 0xD0000001 0x10000110\n_L 0x00000D00 0x1\n_L 0x00000D01 0x1\n_L 0x00000D02 0x1\n";
    static const char unless[] = "_L 0xD0000000 0x30000110\n_L 0x00000D00 0x1\n_L 0x00000D02 0x1\n";
    guest_reset();
    g_buttons = 0x00000110u;
    run_codes(pressed);
    CHECK(r8(0xD00u) == 1u && r8(0xD01u) == 1u && r8(0xD02u) == 1u, "with every button of the mask held, its nn + 1 lines run");
    memset(g_mem + 0xD00u, 0, 4u);
    g_buttons = 0x00004110u;
    run_codes(pressed);
    CHECK(r8(0xD00u) == 1u, "other buttons held as well do not matter");
    memset(g_mem + 0xD00u, 0, 4u);
    g_buttons = 0x00000100u;
    run_codes(pressed);
    CHECK(r8(0xD00u) == 0u && r8(0xD01u) == 0u && r8(0xD02u) == 1u,
          "with one button of the mask up, the guarded lines are skipped and the next one runs");
    memset(g_mem + 0xD00u, 0, 4u);
    run_codes(unless);
    CHECK(r8(0xD00u) == 1u && r8(0xD02u) == 1u, "0x3: the line runs unless every button of the mask is held");
    memset(g_mem + 0xD00u, 0, 4u);
    g_buttons = 0x00000110u;
    run_codes(unless);
    CHECK(r8(0xD00u) == 0u && r8(0xD02u) == 1u, "0x3: held, the line is skipped");
}

/* ---- refusals ------------------------------------------------------------------------------- */

static void test_refusals(void) {
    guest_reset();
    memset(g_mem, 0x5A, sizeof(g_mem));
    /* Lines that are not two words. */
    refused_at("_L 0x00000100\n", 2u, "two 32-bit hexadecimal values");
    refused_at("_L 0x00000100 0x00000001 junk\n", 2u, "two 32-bit hexadecimal values");
    refused_at("_L 0x000001000 0x00000001\n", 2u, "two 32-bit hexadecimal values");
    refused_at("_L 0x00000100 0x0000000G\n", 2u, "two 32-bit hexadecimal values");
    refused_at("_L 0x00000100 0x00000001\n_L\n", 3u, "two 32-bit hexadecimal values");
    refused_at("_L 0x00000100 0x00000001\nL 0x00000101 0x00000001\n", 3u, "is not a _S, _G, _C or _L line");
    refused_at("_L 0x00000100 0x00000001\n_M 0x00000101 0x00000001\n", 3u, "PSP Action Replay");
    refused_at("_N 0x00000101 0x00000001\n", 2u, "PSP Action Replay");
    refused_at("_X what\n", 2u, "line type other than");
    refused_at("", 1u, "has no code lines");
    /* Values wider than the form. */
    refused_at("_L 0x00000100 0x00000100\n", 2u, "8-bit write (type 0x0) with a value wider");
    refused_at("_L 0x10000100 0x00010000\n", 2u, "16-bit write (type 0x1) with a value wider");
    /* 0x3 */
    refused_at("_L 0x30700000 0x00000100\n", 2u, "add or subtract code (type 0x3) in a form");
    refused_at("_L 0x30000001 0x00000100\n", 2u, "add or subtract code (type 0x3) in a form");
    refused_at("_L 0x31100001 0x00000100\n", 2u, "add or subtract code (type 0x3) in a form");
    refused_at("_L 0x30100001 0x10000100\n", 2u, "add or subtract code (type 0x3) in a form");
    refused_at("_L 0x30100101 0x00000100\n", 2u, "8-bit add or subtract with a value wider");
    refused_at("_L 0x30310001 0x00000100\n", 2u, "16-bit add or subtract with a value wider");
    refused_at("_L 0x30500000 0x00000100\n", 2u, "the line with its value is missing");
    refused_at("_L 0x30500001 0x00000100\n_L 0x00000001 0x00000000\n", 2u, "bits set where the form has zeros");
    refused_at("_L 0x30500000 0x00000100\n_L 0x00000001 0x00000001\n", 2u, "bits set where the form has zeros");
    /* 0x4 and 0x8 */
    refused_at("_L 0x40000100 0x00010001\n", 2u, "first line of a two-line code (type 0x4)");
    refused_at("_L 0x40000100 0x00000001\n_L 0x00000001 0x00000000\n", 2u, "count of 0");
    refused_at("_L 0x80000100 0x00010001\n_L 0x20000001 0x00000000\n", 3u, "neither 0x000000vv (8-bit) nor 0x1000vvvv");
    refused_at("_L 0x80000100 0x00010001\n_L 0x00000101 0x00000000\n", 3u, "neither 0x000000vv (8-bit) nor 0x1000vvvv");
    /* 0x5 */
    refused_at("_L 0x50000100 0x00000010\n", 2u, "first line of a two-line code (type 0x5)");
    refused_at("_L 0x50000100 0x00000010\n_L 0x10000200 0x00000000\n", 3u, "destination line of a copy");
    refused_at("_L 0x50000100 0x00000010\n_L 0x00000200 0x00000001\n", 3u, "destination line of a copy");
    refused_at("_L 0x50000100 0x00000000\n_L 0x00000200 0x00000000\n", 2u, "copy of 0 bytes");
    refused_at("_L 0x50000100 0x00000010\n_L 0x00000108 0x00000000\n", 2u, "source and destination overlap");
    refused_at("_L 0x50000108 0x00000010\n_L 0x00000100 0x00000000\n", 2u, "source and destination overlap");
    refused_at("_L 0x50000100 0x00000010\n_L 0x00000100 0x00000000\n", 2u, "source and destination overlap");
    /* 0x6 */
    refused_at("_L 0x60000600 0x00000001\n", 2u, "first line of a two-line code (type 0x6)");
    refused_at("_L 0x60000600 0x00000001\n_L 0x00100001 0x00000000\n", 3u, "base offset");
    refused_at("_L 0x60000600 0x00000001\n_L 0x00060001 0x00000000\n", 3u, "write type is not 0 to 5");
    refused_at("_L 0x60000600 0x00000001\n_L 0x00000000 0x00000000\n", 3u, "pointer count of 0");
    refused_at("_L 0x60000600 0x00000001\n_L 0x00000003 0x00000000\n_L 0x20000004 0x00000000\n", 3u,
               "needs 2 more line(s)");
    refused_at("_L 0x60000600 0x00000010\n_L 0x00000002 0x00000000\n_L 0x10000004 0x00000000\n", 4u,
               "pointer copy line (0x1)");
    refused_at("_L 0x60000600 0x00000010\n_L 0x00000002 0x00000000\n_L 0x90000004 0x00000001\n", 4u,
               "pointer multi-address line (0x9)");
    refused_at("_L 0x60000600 0x00000010\n_L 0x00000002 0x00000000\n_L 0x20000004 0x00000001\n", 4u,
               "not a pointer step");
    refused_at("_L 0x60000600 0x00000100\n_L 0x00000001 0x00000000\n", 2u, "8-bit pointer write with a value wider");
    refused_at("_L 0x60000600 0x00010000\n_L 0x00010001 0x00000000\n", 2u, "16-bit pointer write with a value wider");
    /* 0x7 */
    refused_at("_L 0x70000100 0x00060001\n", 2u, "operation is not 0 to 5");
    refused_at("_L 0x70000100 0x00000100\n", 2u, "8-bit boolean code with a value wider");
    /* Types with no meaning here. */
    refused_at("_L 0x90000100 0x00000001\n", 2u, "code type 0x9");
    refused_at("_L 0xA0000100 0x00000001\n", 2u, "code type 0xA");
    refused_at("_L 0xB0000000 0x00000010\n_L 0x00000100 0x00000001\n", 2u, "code type 0xB (delay)");
    refused_at("_L 0xC0000100 0x00000001\n_L 0x00000100 0x00000001\n", 2u, "code type 0xC");
    refused_at("_L 0xF0000100 0x00000001\n", 2u, "code type 0xF");
    /* 0xD and 0xE */
    refused_at("_L 0xD0000100 0x00400001\n_L 0x00000100 0x1\n", 2u, "16-bit test (type 0xD) in a form");
    refused_at("_L 0xD0000100 0x00010001\n_L 0x00000100 0x1\n", 2u, "16-bit test (type 0xD) in a form");
    refused_at("_L 0xD0000100 0x20000101\n_L 0x00000100 0x1\n", 2u, "8-bit test (type 0xD) in a form");
    refused_at("_L 0xD0000100 0x80000001\n_L 0x00000100 0x1\n", 2u, "test (type 0xD) in a form");
    refused_at("_L 0xD0000100 0x10000001\n_L 0x00000100 0x1\n", 2u, "button test (type 0xD) with bits set");
    refused_at("_L 0xD0000000 0x10000000\n_L 0x00000100 0x1\n", 2u, "names no button");
    refused_at("_L 0x00000100 0x1\n_L 0xD0000100 0x00000001\n", 3u, "guards 1 line(s) and only 0 follow");
    refused_at("_L 0xD0000001 0x10000001\n_L 0x00000100 0x1\n", 2u, "guards 2 line(s) and only 1 follow");
    refused_at("_L 0xD0000100 0x00000001\n_L 0x40000100 0x00010001\n_L 0x00000001 0x00000000\n", 2u,
               "end inside a multi-line code");
    refused_at("_L 0xD0000100 0x40000104\n", 2u, "first line of a two-address test");
    refused_at("_L 0xD0000100 0x40000104\n_L 0x00000001 0x00000003\n_L 0x00000100 0x1\n", 3u, "size is not 0, 1 or 2");
    refused_at("_L 0xD0000100 0x60000104\n_L 0x00000001 0x00000002\n_L 0x00000100 0x1\n", 2u,
               "32-bit less-than or greater-than");
    refused_at("_L 0xD0000100 0x40000104\n_L 0x00000000 0x00000002\n_L 0x00000100 0x1\n", 2u, "guards no line");
    refused_at("_L 0xE2010001 0x00000100\n_L 0x00000100 0x1\n", 2u, "neither 0xE0 (16-bit) nor 0xE1 (8-bit)");
    refused_at("_L 0xE1010101 0x00000100\n_L 0x00000100 0x1\n", 2u, "8-bit multi-line test (type 0xE) in a form");
    refused_at("_L 0xE0010001 0x40000100\n_L 0x00000100 0x1\n", 2u, "16-bit multi-line test (type 0xE) in a form");
    refused_at("_L 0xE0000001 0x00000100\n_L 0x00000100 0x1\n", 2u, "guards no line");
    refused_at("_L 0xE0030001 0x00000100\n_L 0x00000100 0x1\n", 2u, "guards 3 line(s) and only 1 follow");
    /* Addresses: every span is whole inside user memory. */
    refused_at("_L 0x00010000 0x00000001\n", 2u, "names 1 byte(s) at 0x08810000, outside user memory (0x08800000 to 0x0880FFFF)");
    refused_at("_L 0x1000FFFF 0x00000001\n", 2u, "names 2 byte(s) at 0x0880FFFF, outside user memory");
    refused_at("_L 0x2000FFFD 0x00000001\n", 2u, "names 4 byte(s) at 0x0880FFFD, outside user memory");
    refused_at("_L 0x0FFFFFFF 0x00000001\n", 2u, "outside user memory");
    refused_at("_L 0x4000FF00 0x00410001\n_L 0x00000001 0x00000000\n", 2u, "names 260 byte(s) at 0x0880FF00, outside user memory");
    refused_at("_L 0x40000100 0xFFFFFFFF\n_L 0x00000001 0x00000000\n", 2u, "outside user memory");
    refused_at("_L 0x50000100 0x00010000\n_L 0x00000200 0x00000000\n", 2u, "names 65536 byte(s) at 0x08800100, outside user memory");
    refused_at("_L 0x50000100 0xFFFFFFFF\n_L 0x00000200 0x00000000\n", 2u, "outside user memory");
    refused_at("_L 0x6000FFFD 0x00000001\n_L 0x00000001 0x00000000\n", 2u, "names 4 byte(s) at 0x0880FFFD, outside user memory");
    refused_at("_L 0xD000FFFF 0x00000001\n_L 0x00000100 0x1\n", 2u, "names 2 byte(s) at 0x0880FFFF, outside user memory");
    refused_at("_L 0xD0000100 0x4000FFFE\n_L 0x00000001 0x00000002\n_L 0x00000100 0x1\n", 2u, "outside user memory");
    refused_at("_L 0xE0010001 0x0000FFFF\n_L 0x00000100 0x1\n", 2u, "outside user memory");
    /* A _C line that says neither on nor off is shown as refused. */
    {
        NkCheatInfo info;
        NkCheatSet *set = load("_CX name\n_L 0x00000100 0x00000001\n_C1x name\n_C\n");
        CHECK(nk_cheats_count(set) == 3u && nk_cheats_info(set, 0u, &info) && info.state == NK_CHEAT_REFUSED &&
                  info.refused_line == 1u && strstr(info.reason, "_C0 (off) or _C1 (on)"),
              "a _C line with no 0 or 1 is refused by name");
        CHECK(nk_cheats_info(set, 1u, &info) && info.state == NK_CHEAT_REFUSED &&
                  nk_cheats_info(set, 2u, &info) && info.state == NK_CHEAT_REFUSED,
              "so are _C1x and a bare _C");
        nk_cheats_free(set);
    }
    /* One refused cheat does not stop the others, and an off cheat carries its reason too. */
    {
        NkCheatSummary sum;
        NkCheatInfo info;
        NkCheatSet *set = load("_C1 good\n_L 0x00000100 0x00000001\n"
                               "_C1 bad\n_L 0x00000101 0x00000002\n_L 0xB0000000 0x00000001\n"
                               "_C0 off and bad\n_L 0xB0000000 0x00000001\n"
                               "_C1 good too\n_L 0x00000102 0x00000003\n");
        nk_cheats_summary(set, &sum);
        CHECK(sum.on == 2u && sum.refused == 1u && sum.off == 1u, "two on, one refused, one off");
        CHECK(nk_cheats_apply(set, &k_host) == 2u && r8(0x100u) == 1u && r8(0x102u) == 3u,
              "the cheats around a refused one apply");
        CHECK(r8(0x101u) == 0x5Au, "no line of the refused cheat applies, the accepted one before the bad line included");
        CHECK(nk_cheats_info(set, 2u, &info) && info.state == NK_CHEAT_OFF && info.refused_line == 7u &&
                  strstr(info.reason, "0xB"),
              "an off cheat says it would be refused");
        nk_cheats_free(set);
    }
    CHECK(g_bad_access == 0, "no refusal test read or wrote outside what was checked");
}

/* The most one cheat may write in a frame. Checked at load, so no guest is needed. */
static void test_frame_limit(void) {
    const NkCheatConfig big = { NULL, T_BASE, T_BASE + 0x00800000u };
    NkCheatInfo info;
    NkCheatSet *set = load_as("_C1 at the limit\n_L 0x50000000 0x00100000\n_L 0x00200000 0x00000000\n"
                              "_C1 over it\n_L 0x50000000 0x00100001\n_L 0x00200000 0x00000000\n"
                              "_C1 over it in two codes\n_L 0x50000000 0x00100000\n_L 0x00200000 0x00000000\n"
                              "_L 0x00400000 0x00000001\n",
                              &big, NULL);
    CHECK(set && nk_cheats_count(set) == 3u, "the frame-limit file loads");
    CHECK(nk_cheats_info(set, 0u, &info) && info.state == NK_CHEAT_ON, "exactly 1 MiB in a frame is accepted");
    CHECK(nk_cheats_info(set, 1u, &info) && info.state == NK_CHEAT_REFUSED && info.refused_line == 5u &&
              strstr(info.reason, "more than 1048576 bytes"),
          "one byte more is refused by name");
    CHECK(nk_cheats_info(set, 2u, &info) && info.state == NK_CHEAT_REFUSED && info.refused_line == 10u,
          "the limit counts every code of the cheat, and names the line that passes it");
    nk_cheats_free(set);
}

/* ---- translated code ------------------------------------------------------------------------ */

static void test_code_writes(void) {
    guest_reset();
    memset(g_mem + 0x4000u, 0xC0, 0x100u);
    /* Named outright: refused when the file is read, and the cheat with it. */
    refused_at("_L 0x00000100 0x00000001\n_L 0x20004000 0x03E00008\n", 3u, "inside code the recompiler translated");
    refused_at("_L 0x200040FC 0x00000000\n", 2u, "writes 4 byte(s) at 0x088040FC, inside code");
    refused_at("_L 0x10003FFF 0x00000000\n", 2u, "inside code the recompiler translated");
    refused_at("_L 0x30500000 0x00004010\n_L 0x00000001 0x00000000\n", 2u, "inside code the recompiler translated");
    refused_at("_L 0x70004010 0x00000001\n", 2u, "inside code the recompiler translated");
    refused_at("_L 0x40003F00 0x00100020\n_L 0x00000000 0x00000000\n", 2u, "inside code the recompiler translated");
    refused_at("_L 0x80003FF0 0x00020010\n_L 0x00000000 0x00000000\n", 2u, "inside code the recompiler translated");
    refused_at("_L 0x50000100 0x00000010\n_L 0x00003FF8 0x00000000\n", 2u, "inside code the recompiler translated");
    CHECK(r8(0x100u) == 0u, "the data write beside a refused code write is not applied either");
    /* Around it is data. */
    run_codes("_L 0x20003FFC 0x11111111\n_L 0x20004100 0x22222222\n");
    CHECK(r32(0x3FFCu) == 0x11111111u && r32(0x4100u) == 0x22222222u, "the words on each side of the code are data");
    /* Code may be read: a test on a code word, and a copy out of code. */
    w32(0x4000u, 0x27BDFFF0u);
    run_codes("_L 0xD0004000 0x0000FFF0\n_L 0x00000120 0x00000001\n_L 0x50004000 0x00000004\n_L 0x00000124 0x00000000\n");
    CHECK(r8(0x120u) == 1u && r32(0x124u) == 0x27BDFFF0u, "a code word may be tested and copied from");
    /* Without a host the reader cannot know what is code; the applier still asks each frame. */
    {
        NkCheatInfo info;
        NkCheatSet *set = load_as("_C1 c\n_L 0x00000130 0x00000001\n_L 0x20004000 0x00000000\n", &k_config, NULL);
        g_report_count = 0;
        CHECK(set && nk_cheats_info(set, 0u, &info) && info.state == NK_CHEAT_ON,
              "read without a host, a code write is not known as one");
        CHECK(nk_cheats_apply(set, &k_host) == 0u && r32(0x4000u) == 0x27BDFFF0u && r8(0x130u) == 0u,
              "applied, it is held whole: the code is untouched and its data write is undone");
        CHECK(g_report_count == 1 && strstr(g_reports[0], "is held: line 3") &&
                  strstr(g_reports[0], "0x08804000 is inside code the recompiler translated"),
              "and the hold is reported with its line and address");
        nk_cheats_free(set);
    }
    CHECK(g_bad_access == 0, "no code-write test wrote into the code or outside what was checked");
}

/* ---- all of a cheat or none of it ----------------------------------------------------------- */

static void test_held_frames(void) {
    static const char text[] =
        "_C1 through a pointer\n"
        "_L 0x00000100 0x00000011\n"
        "_L 0x30100001 0x00000101\n"
        "_L 0x40000104 0x00020001\n_L 0x00000033 0x00000000\n"
        "_L 0x60000600 0x00000022\n_L 0x00000001 0x00000008\n"
        "_L 0x00000102 0x00000044\n"
        "_C1 beside it\n"
        "_L 0x00000110 0x00000055\n";
    NkCheatInfo info;
    NkCheatSet *set;
    guest_reset();
    memset(g_mem + 0x100u, 0x77, 0x10u);
    set = load(text);
    /* The pointer is null: the cheat is held and what it wrote before the pointer is undone. */
    CHECK(nk_cheats_apply(set, &k_host) == 1u, "the cheat beside the held one applies");
    CHECK(r8(0x100u) == 0x77u && r8(0x101u) == 0x77u && r32(0x104u) == 0x77777777u && r32(0x108u) == 0x77777777u &&
              r8(0x102u) == 0x77u,
          "a held cheat leaves every byte as it found it");
    CHECK(r8(0x110u) == 0x55u, "the other cheat's byte is written");
    CHECK(g_report_count == 1 && strstr(g_reports[0], "cheat \"through a pointer\" is held: line 6") &&
              strstr(g_reports[0], "pointer leads to 0x00000008, outside user memory"),
          "the hold names the cheat, the line and the address");
    CHECK(nk_cheats_info(set, 0u, &info) && info.held && info.held_frames == 1u && info.applied_frames == 0u,
          "a held frame is counted");
    CHECK(nk_cheats_apply(set, &k_host) == 1u && g_report_count == 1, "the same cause is not reported again");
    /* The pointer leads past the end of user memory: still held, the address in the report changes
     * nothing about the cause. */
    w32(0x600u, T_END - 4u);
    CHECK(nk_cheats_apply(set, &k_host) == 1u && g_report_count == 1 && r8(0x100u) == 0x77u,
          "a pointer that leads past user memory holds the cheat too");
    /* The pointer becomes valid: the whole cheat applies, and says so once. */
    w32(0x600u, T_BASE + 0x700u);
    CHECK(nk_cheats_apply(set, &k_host) == 2u, "with a valid pointer the cheat applies");
    CHECK(r8(0x100u) == 0x11u && r8(0x101u) == 0x78u && r32(0x104u) == 0x33u && r32(0x108u) == 0x33u &&
              r8(0x708u) == 0x22u && r8(0x102u) == 0x44u,
          "every code of the cheat ran, in order");
    CHECK(g_report_count == 2 && strstr(g_reports[1], "cheat \"through a pointer\" applies again"),
          "the cheat says once that it applies again");
    CHECK(nk_cheats_info(set, 0u, &info) && !info.held && info.held_frames == 3u && info.applied_frames == 1u,
          "held and applied frames are counted apart");
    /* The pointer leads into translated code: held for another cause, reported once more. */
    w32(0x600u, T_CODE_LO);
    memset(g_mem + 0x4000u, 0xC0, 0x100u);
    CHECK(nk_cheats_apply(set, &k_host) == 1u && r8(0x4008u) == 0xC0u && r8(0x101u) == 0x78u,
          "a pointer into translated code holds the cheat, and the add before it is undone");
    CHECK(g_report_count == 3 && strstr(g_reports[2], "0x08804008 is inside code the recompiler translated"),
          "the new cause is reported");
    /* A pointer step that leaves user memory. */
    nk_cheats_free(set);
    g_report_count = 0;
    w32(0x600u, T_BASE + 0x700u);
    w32(0x720u, 0x00000004u);
    set = load("_C1 walk\n_L 0x00000100 0x00000099\n_L 0x60000600 0x00000001\n_L 0x00000003 0x00000000\n"
               "_L 0x20000020 0x00000000\n_L 0x20000020 0x00000000\n");
    CHECK(nk_cheats_apply(set, &k_host) == 0u && r8(0x100u) == 0x11u && g_report_count == 1 &&
              strstr(g_reports[0], "pointer leads to 0x00000024"),
          "a pointer step that leaves user memory holds the cheat");
    nk_cheats_free(set);
    CHECK(g_bad_access == 0, "no held frame read or wrote outside what was checked");
}

#ifdef SR_SELFTEST_BUILD_ROOT
static void test_file_loader(void) {
    const char *path = SR_SELFTEST_BUILD_ROOT "/test_nk_cheats.ini";
    char error[128] = "";
    NkCheatSummary sum;
    NkCheatSet *set;
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL, "the test file is written");
    if (!f) return;
    fputs("_S ULUS-12345\n_G Synthetic\n_C1 from a file\n_L 0x00000140 0x00000042\n", f);
    for (int i = 0; i < 20000; i++) fputs("// padding past the first read block\n", f);
    fputs("_C0 last\n_L 0x00000141 0x00000001\n", f);
    fclose(f);
    guest_reset();
    set = nk_cheats_load_file(path, &k_config, &k_host, error, sizeof(error));
    nk_cheats_summary(set, &sum);
    CHECK(set && sum.on == 1u && sum.off == 1u && nk_cheats_apply(set, &k_host) == 1u && r8(0x140u) == 0x42u,
          "a cheat file is read whole and applied");
    nk_cheats_free(set);
    remove(path);
    CHECK(nk_cheats_load_file(path, &k_config, &k_host, error, sizeof(error)) == NULL &&
              strstr(error, "cannot be opened"),
          "a file that is not there is refused by name");
    CHECK(nk_cheats_load_file("", &k_config, &k_host, error, sizeof(error)) == NULL && strstr(error, "no cheat file"),
          "so is an empty path");
}
#endif

int main(void) {
    test_file_lines();
    test_writes();
    test_add_and_subtract();
    test_multi_writes();
    test_copy();
    test_pointers();
    test_boolean();
    test_value_conditions();
    test_address_conditions();
    test_button_conditions();
    test_refusals();
    test_frame_limit();
    test_code_writes();
    test_held_frames();
#ifdef SR_SELFTEST_BUILD_ROOT
    test_file_loader();
#endif
    CHECK(g_bad_access == 0, "the applier read and wrote only spans it had checked, and no code");
    if (g_failures) {
        fprintf(stderr, "test_nk_cheats: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("test_nk_cheats: %d checks passed\n", g_checks);
    return 0;
}
