/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2012- PPSSPP Project.
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * CWCheat codes (#2639): nk_cheats.h says what is read, what runs and what is refused.
 *
 * The line types of a cheat file, the decode of each code form and what each form does are
 * translated from PPSSPP's CWCheat engine (https://github.com/hrydgard/ppsspp),
 * Core/CwCheat.cpp at revision c437463bc5cb78a0236312ad23f9c3b56fb9ae67 (GPL-2.0-or-later):
 * CheatFileParser::ParseLine and CheatFileParser::ParseDataLine, the CheatOp and CheatOperation
 * types, CWCheatEngine::InterpretNextCwCheat, and CWCheatEngine::ExecuteOp with
 * ApplyMemoryOperator, TestIf and TestIfAddr; the address base is CWCheatEngine::GetAddress in
 * Core/CwCheat.h. Label: derived-translated (docs/provenance/INDEPENDENCE_MODEL.md);
 * PPSSPP-derived, not PSP-measured. This file is conveyed as GPL-3.0-or-later.
 *
 * Core/CwCheat.cpp and Core/CwCheat.h carry no copyright or licence line of their own at that
 * revision. The PPSSPP line above is the notice PPSSPP's source files carry, copied from the
 * first line of Core/Core.cpp at the same revision; that file's notice and PPSSPP's LICENSE.TXT
 * give the licence, GPL version 2.0 or later.
 *
 * Modified for this project (2026-11-16; see src/core/PROVENANCE.md):
 *   - C99 over a host interface (NkCheatHost) instead of PPSSPP's memory map, JIT cache and
 *     controller state. No line of PPSSPP's code was copied. These parts follow its structure
 *     closely:
 *       OpKind        the operation kinds, in the order of PPSSPP's CheatOp. Its Vibration,
 *                     PostShader, Delay and Assert kinds are left out; its four value tests are
 *                     one kind with a comparison, as are its four address tests and its two
 *                     button tests.
 *       CheatOp       one record per code, as PPSSPP's CheatOperation: an address, a size and a
 *                     value, and a multi-write's count, step and add under PPSSPP's names.
 *       check_code    the switch over a code's top four bits, and the bit fields each form is
 *                     read from, are those of InterpretNextCwCheat.
 *       run_cheat     one case per kind, as ExecuteOp: the multi-write loop (the address grows
 *                     by step, the value by add), the copy, the pointer walk (a 0x2 or 0x3 step
 *                     in either word of a line, then the write at the pointer plus or minus the
 *                     offset) and the tests that skip lines when they fail.
 *       code_address, user_address    GetAddress's base 0x08800000 and its 0x3FFFFFFF mask.
 *       parse_text    the _S, _G, _C and _L line types, _C0 for off and _C1 to _C9 for on, and
 *                     a comment after a code line's two values, as ParseLine and ParseDataLine.
 *   - This project's own, with no counterpart in PPSSPP:
 *       the text reader's code (no sscanf; a name is cleaned for display; a line that belongs
 *       to no cheat becomes a note);
 *       the load-time check (check_cheat): a whole cheat is compiled to its operation list
 *       once, where PPSSPP decodes each code every time it runs. Every span a code names is
 *       proved to lie in user memory, each test's count of lines becomes the operation it
 *       continues at, and what one frame can write is bounded;
 *       the refusal of a write into code the recompiler translated, asked when the file is
 *       read and again before every write, where PPSSPP writes and invalidates its JIT cache;
 *       the whole-or-nothing apply: an undo journal puts back what a cheat wrote on a frame it
 *       cannot finish, and each cause of a hold is reported once.
 *   - Stricter than PPSSPP. Each of these refuses its whole cheat by name, where PPSSPP picks
 *     a meaning or skips the code: a value wider than its form holds (PPSSPP makes a type 0x0
 *     write 16 or 32 bits wide when the value has higher bits set); bits set where a form has
 *     zeros; a 0x3 form other than 1 to 6; a 0x8 value line that is neither 0x000000vv nor
 *     0x1000vvvv; a count of 0 and a copy that overlaps itself; a 0x6 code with a base offset,
 *     a write type above 5 or a 0x1 (copy) or 0x9 (multi-address) line; a 0x7 operation above
 *     5; a test with an unknown comparison, a 0xE test other than 0xE0 and 0xE1, a test that
 *     guards no line, runs past the end of its cheat or ends inside a multi-line code; a span
 *     a code names that leaves user memory (PPSSPP skips the access; a pointer that leads
 *     outside it holds the cheat for the frame); code types 0x9, 0xA, 0xB, 0xC and 0xF; and
 *     _M and _N lines (PPSSPP runs PSP Action Replay codes). Comparisons are unsigned
 *     here and signed in PPSSPP; the two agree for 8 and 16 bits, and the 32-bit orderings of
 *     two addresses, where they would not, are refused. A section applies only when its _S
 *     disc ID is the run's (PPSSPP also applies a file whose only section names another
 *     game). */

#include "nk_cheats.h"

#ifndef NK_CHEATS_NO_HOST_IO
#include "nk_shared_helpers.h"
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    OP_WRITE = 0,
    OP_ADD,
    OP_SUB,
    OP_OR,
    OP_AND,
    OP_XOR,
    OP_MULTI,
    OP_COPY,
    OP_POINTER,
    OP_IF_VALUE,
    OP_IF_ADDRESS,
    OP_IF_BUTTONS
} OpKind;

typedef enum { CMP_EQ = 0, CMP_NE, CMP_LT, CMP_GT } Compare;

/* One code, checked and ready to run. Static addresses are whole user-memory spans already. */
typedef struct {
    uint8_t kind;        /* OpKind */
    uint8_t size;        /* 1, 2 or 4 bytes */
    uint8_t compare;     /* Compare */
    uint8_t negate;      /* pointer: write at pointer - offset; buttons: unless held */
    uint32_t addr;
    uint32_t value;
    uint32_t other;      /* copy: destination; compare: second address; pointer: offset */
    uint32_t count;      /* multi-write: writes; copy: bytes; pointer: walk steps */
    uint32_t step;       /* multi-write: bytes between writes; pointer: first walk step */
    uint32_t add;        /* multi-write: how much the value grows per write */
    uint32_t skip_to;    /* a condition that fails continues at this code */
    uint32_t lines;      /* file lines the code takes */
    uint32_t line;       /* its first line in the file */
} CheatOp;

typedef struct {
    uint32_t offset;
    uint8_t negative;
} CheatWalk;

typedef struct {
    uint32_t a;
    uint32_t b;
    uint32_t file_line;
} CodeLine;

typedef enum { HOLD_NONE = 0, HOLD_SPAN, HOLD_CODE, HOLD_UNDO, HOLD_NULL } HoldWhy;

typedef struct {
    char name[NK_CHEATS_NAME_MAX];
    char reason[NK_CHEATS_REASON_MAX];
    uint32_t file_line;
    uint32_t refused_line;
    bool enabled;
    CodeLine *lines;         /* freed once the cheat is checked */
    size_t line_count;
    size_t line_cap;
    CheatOp *ops;
    uint32_t op_count;
    CheatWalk *walks;
    uint32_t walk_count;
    uint32_t undo_records;   /* the most one frame can write */
    uint32_t undo_bytes;
    uint64_t applied_frames;
    uint64_t held_frames;
    bool held;
    uint8_t held_why;
    uint32_t held_line;
    uint8_t said_why;        /* one bit per HoldWhy this cheat has reported a hold for */
    bool said_again;         /* "applies again" was reported */
    bool over_limit;         /* refused for the file's frame limit, not for a line of its own */
} Cheat;

typedef struct {
    uint32_t addr;
    uint32_t size;
    uint32_t at;             /* its old bytes in undo_data */
} UndoRecord;

struct NkCheatSet {
    uint32_t user_base;
    uint32_t user_end;
    uint32_t hole_base;      /* memory inside the range that is not the game's; none when equal */
    uint32_t hole_end;
    uint32_t psp_image_base; /* image_size != 0: the image a PSP has here is at image_base */
    uint32_t image_base;
    uint32_t image_size;
    uint32_t not_kept;       /* cheats past NK_CHEATS_MAX_CHEATS */
    uint32_t lines_total;    /* code lines kept, of every cheat */
    uint32_t over_limit;     /* cheats refused for the frame limit */
    uint32_t over_limit_line;
    char disc_id[32];        /* letters and digits only, upper case; "" matches no section */
    Cheat *cheats;
    size_t count;
    size_t cap;
    uint32_t other_game;
    char notes[NK_CHEATS_MAX_NOTES][144];   /* "line <n>: " and the longest note text, whole */
    uint32_t notes_kept;
    uint32_t notes_total;
    UndoRecord *undo;
    uint32_t undo_cap;
    uint32_t undo_count;
    uint8_t *undo_data;
    uint32_t undo_data_cap;
    uint32_t undo_used;
    bool out_of_memory;
};

static void set_error(char *error, size_t error_size, const char *text) {
    if (error && error_size > 0u) snprintf(error, error_size, "%s", text);
}

/* ---- the file ------------------------------------------------------------------------------ */

static bool is_blank(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* One 32-bit word: an optional 0x, then 1 to 8 hexadecimal digits, then a blank or the end. */
static bool parse_word(const char *s, size_t len, size_t *pos, uint32_t *out) {
    size_t i = *pos;
    uint32_t value = 0u;
    unsigned digits = 0u;
    while (i < len && is_blank(s[i])) i++;
    if (i + 1u < len && s[i] == '0' && (s[i + 1u] == 'x' || s[i + 1u] == 'X')) i += 2u;
    while (i < len && hex_digit(s[i]) >= 0) {
        if (++digits > 8u) return false;
        value = (value << 4) | (uint32_t)hex_digit(s[i]);
        i++;
    }
    if (digits == 0u || (i < len && !is_blank(s[i]))) return false;
    *pos = i;
    *out = value;
    return true;
}

/* A disc ID as sections are matched: its letters and digits, upper case. */
static void normal_disc_id(char *out, size_t cap, const char *s, size_t len) {
    size_t n = 0u;
    for (size_t i = 0u; i < len && n + 1u < cap; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out[n++] = c;
    }
    out[n] = '\0';
}

/* The length of the well-formed UTF-8 character at s (1 to 4), or 0. */
static size_t utf8_char(const unsigned char *s, size_t len) {
    unsigned long cp, min;
    size_t extra;
    if (len == 0u) return 0u;
    if (s[0] < 0x80u) return 1u;
    if (s[0] >= 0xC2u && s[0] <= 0xDFu) { extra = 1u; min = 0x80u; cp = s[0] & 0x1Fu; }
    else if (s[0] >= 0xE0u && s[0] <= 0xEFu) { extra = 2u; min = 0x800u; cp = s[0] & 0x0Fu; }
    else if (s[0] >= 0xF0u && s[0] <= 0xF4u) { extra = 3u; min = 0x10000u; cp = s[0] & 0x07u; }
    else return 0u;
    if (len <= extra) return 0u;
    for (size_t k = 1u; k <= extra; k++) {
        if ((s[k] & 0xC0u) != 0x80u) return 0u;
        cp = (cp << 6) | (s[k] & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFFul || (cp >= 0xD800ul && cp <= 0xDFFFul) || cp <= 0x9Ful) return 0u;
    return extra + 1u;
}

/* A cheat's name for a log line or a list: control characters and bytes that are not UTF-8
 * become '?', a '"' becomes an apostrophe (the reports quote the name), and a name too long is
 * cut between characters. */
static void clean_name(char *out, size_t cap, const char *s, size_t len) {
    const unsigned char *u = (const unsigned char *)s;
    size_t n = 0u;
    size_t i = 0u;
    while (i < len && is_blank(s[i])) i++;
    while (len > i && is_blank(s[len - 1u])) len--;
    while (i < len) {
        size_t width = utf8_char(u + i, len - i);
        if (width == 0u || u[i] < 0x20u || u[i] == 0x7Fu) {
            if (n + 2u > cap) break;
            out[n++] = '?';
            i++;
            continue;
        }
        if (n + width + 1u > cap) break;
        if (width == 1u && s[i] == '"') out[n++] = '\'';
        else {
            memcpy(out + n, s + i, width);
            n += width;
        }
        i += width;
    }
    out[n] = '\0';
    if (n == 0u) snprintf(out, cap, "(unnamed)");
}

static void note_add(NkCheatSet *set, uint32_t file_line, const char *text) {
    if (set->notes_total != UINT32_MAX) set->notes_total++;
    if (set->notes_kept >= NK_CHEATS_MAX_NOTES) return;
    snprintf(set->notes[set->notes_kept], sizeof(set->notes[0]), "line %u: %s",
             (unsigned)file_line, text);
    set->notes_kept++;
}

/* The first refusal is the one kept: it names the line a reader has to look at first. */
static void refuse(Cheat *c, uint32_t file_line, const char *fmt, ...) {
    va_list args;
    if (c->refused_line != 0u) return;
    c->refused_line = file_line != 0u ? file_line : c->file_line;
    va_start(args, fmt);
    vsnprintf(c->reason, sizeof(c->reason), fmt, args);
    va_end(args);
}

static Cheat *cheat_new(NkCheatSet *set, uint32_t file_line) {
    if (set->count == set->cap) {
        size_t next_cap = set->cap == 0u ? 16u : set->cap * 2u;
        Cheat *next;
        if (next_cap < set->cap || next_cap > SIZE_MAX / sizeof(*next)) {
            set->out_of_memory = true;
            return NULL;
        }
        next = (Cheat *)realloc(set->cheats, next_cap * sizeof(*next));
        if (!next) {
            set->out_of_memory = true;
            return NULL;
        }
        set->cheats = next;
        set->cap = next_cap;
    }
    Cheat *c = &set->cheats[set->count++];
    memset(c, 0, sizeof(*c));
    c->file_line = file_line;
    return c;
}

static void cheat_add_line(NkCheatSet *set, Cheat *c, uint32_t a, uint32_t b, uint32_t file_line) {
    if (c->line_count == c->line_cap) {
        size_t next_cap = c->line_cap == 0u ? 8u : c->line_cap * 2u;
        CodeLine *next;
        if (next_cap < c->line_cap || next_cap > SIZE_MAX / sizeof(*next)) {
            set->out_of_memory = true;
            return;
        }
        next = (CodeLine *)realloc(c->lines, next_cap * sizeof(*next));
        if (!next) {
            set->out_of_memory = true;
            return;
        }
        c->lines = next;
        c->line_cap = next_cap;
    }
    c->lines[c->line_count].a = a;
    c->lines[c->line_count].b = b;
    c->lines[c->line_count].file_line = file_line;
    c->line_count++;
}

/* A line that is not a code line of the current cheat: it refuses that cheat, since it may be
 * one of its codes written wrongly, or it is noted when no cheat is open. */
static void stray_line(NkCheatSet *set, Cheat *cur, uint32_t file_line, const char *what) {
    if (cur) refuse(cur, file_line, "%s", what);
    else note_add(set, file_line, what);
}

static void parse_text(NkCheatSet *set, const char *text, size_t size) {
    size_t pos = 0u;
    uint32_t file_line = 0u;
    Cheat *cur = NULL;
    bool ours = true;   /* before any _S line the file is for the disc it was read for */
    bool dropped = false;   /* inside a cheat past NK_CHEATS_MAX_CHEATS: its lines are not read */
    if (size >= 3u && (unsigned char)text[0] == 0xEFu && (unsigned char)text[1] == 0xBBu &&
        (unsigned char)text[2] == 0xBFu) {
        pos = 3u;
    }
    while (pos < size && !set->out_of_memory) {
        const char *line = text + pos;
        size_t len = 0u;
        while (pos + len < size && line[len] != '\n') len++;
        pos += len < size - pos ? len + 1u : len;
        if (file_line != UINT32_MAX) file_line++;
        while (len > 0u && is_blank(line[0])) { line++; len--; }
        while (len > 0u && is_blank(line[len - 1u])) len--;
        if (len == 0u || line[0] == '#' || (len >= 2u && line[0] == '/' && line[1] == '/')) continue;
        if (line[0] != '_' || len < 2u) {
            if (ours) stray_line(set, cur, file_line, "is not a _S, _G, _C or _L line or a comment");
            continue;
        }
        if (line[1] == 'S') {
            char id[sizeof(set->disc_id)];
            normal_disc_id(id, sizeof(id), line + 2u, len - 2u);
            ours = id[0] != '\0' && strcmp(id, set->disc_id) == 0;
            cur = NULL;
            dropped = false;
        } else if (line[1] == 'G') {
            /* The game's name: nothing to do. */
        } else if (line[1] == 'C' && len < 5u) {
            /* PPSSPP reads a _ line only from five characters up ("_C0 1" is the shortest), so a
             * _C line with no name starts no cheat there, on or off, and the code lines after it
             * stay with the cheat before it. Starting a cheat here would turn on lines PPSSPP
             * leaves as they were. */
            if (ours && !dropped) {
                note_add(set, file_line, "a _C line with no name starts no cheat, as in PPSSPP; the code "
                                         "lines after it stay with the cheat before");
            }
        } else if (!ours) {
            if (line[1] == 'C' && set->other_game != UINT32_MAX) set->other_game++;
        } else if (line[1] == 'C') {
            if (set->count >= NK_CHEATS_MAX_CHEATS) {
                if (set->not_kept == 0u) {
                    note_add(set, file_line, "more cheats for this disc than the reader keeps; this one "
                                             "and every one after it are not read");
                }
                if (set->not_kept != UINT32_MAX) set->not_kept++;
                cur = NULL;
                dropped = true;
                continue;
            }
            dropped = false;
            cur = cheat_new(set, file_line);
            if (!cur) break;
            clean_name(cur->name, sizeof(cur->name), len > 3u ? line + 3u : "", len > 3u ? len - 3u : 0u);
            if (len >= 3u && line[2] >= '0' && line[2] <= '9' && (len == 3u || is_blank(line[3]))) {
                cur->enabled = line[2] != '0';
            } else {
                /* On or off cannot be told, so it is shown as refused rather than left out. */
                cur->enabled = true;
                refuse(cur, file_line, "is not _C0 (off) or _C1 (on) and then a name");
            }
        } else if (line[1] == 'L') {
            size_t at = 2u;
            uint32_t a = 0u, b = 0u;
            bool ok = parse_word(line, len, &at, &a) && parse_word(line, len, &at, &b);
            while (ok && at < len && is_blank(line[at])) at++;
            if (ok && at < len && line[at] != '#' &&
                !(at + 1u < len && line[at] == '/' && line[at + 1u] == '/')) {
                ok = false;
            }
            if (dropped) continue;
            if (!cur) note_add(set, file_line, "a code line before any _C line; it is not applied");
            else if (!ok) refuse(cur, file_line, "is not _L and two 32-bit hexadecimal values");
            else if (set->lines_total >= NK_CHEATS_MAX_LINES) {
                refuse(cur, file_line, "is past the %lu code lines the reader keeps for one disc",
                       (unsigned long)NK_CHEATS_MAX_LINES);
            } else {
                cheat_add_line(set, cur, a, b, file_line);
                set->lines_total++;
            }
        } else if (dropped) {
            /* A line of a cheat that is not read. */
        } else if (line[1] == 'M' || line[1] == 'N') {
            stray_line(set, cur, file_line, "is a PSP Action Replay code line (_M or _N), which is not supported");
        } else {
            stray_line(set, cur, file_line, "is a line type other than _S, _G, _C and _L");
        }
    }
}

/* ---- checking a cheat ---------------------------------------------------------------------- */

typedef struct {
    NkCheatSet *set;
    Cheat *cheat;
    const NkCheatHost *host;
    uint64_t undo_records;
    uint64_t undo_bytes;
} Checker;

static const char *const k_size_words[5] = { "", "8-bit", "16-bit", "", "32-bit" };

/* The address of a code's 28-bit offset. It cannot wrap: the largest is 0x187FFFFF. */
static uint32_t code_address(uint32_t word) {
    return NK_CHEATS_ADDRESS_BASE + (word & 0x0FFFFFFFu);
}

static bool span_in_user_memory(const NkCheatSet *set, uint32_t addr, uint64_t size) {
    if (addr < set->user_base || addr >= set->user_end || size > (uint64_t)(set->user_end - addr)) return false;
    return !(set->hole_base < set->hole_end && addr < set->hole_end && (uint64_t)addr + size > set->hole_base);
}

/* A span a code names outright. *addr comes in as the PSP address the code names and goes out
 * as the address in this run: the same, unless the game's image is not where a PSP has it
 * (NkCheatConfig). Then a span inside the PSP's image moves with the image, and any other
 * refuses the cheat: where the rest of a PSP's memory lives in such a run is not known, and a
 * cheat is never applied to a guess. The span must lie whole inside user memory either way. */
static bool check_span(Checker *k, uint32_t file_line, uint32_t *addr, uint64_t size) {
    const NkCheatSet *set = k->set;
    uint32_t at = *addr;
    if (set->image_size != 0u) {
        const uint32_t into = at - set->psp_image_base;
        if (at < set->psp_image_base || into >= set->image_size || size > (uint64_t)(set->image_size - into)) {
            refuse(k->cheat, file_line,
                   "names %llu byte(s) at 0x%08lX, outside the game's image as a PSP loads it (0x%08lX to "
                   "0x%08lX); this run has the image at 0x%08lX, and where other PSP addresses are in it "
                   "is not known",
                   (unsigned long long)size, (unsigned long)at, (unsigned long)set->psp_image_base,
                   (unsigned long)(set->psp_image_base + (set->image_size - 1u)), (unsigned long)set->image_base);
            return false;
        }
        at = set->image_base + into;
    }
    if (!span_in_user_memory(set, at, size)) {
        refuse(k->cheat, file_line, "names %llu byte(s) at 0x%08lX, outside user memory (0x%08lX to 0x%08lX)",
               (unsigned long long)size, (unsigned long)at, (unsigned long)set->user_base,
               (unsigned long)(set->user_end - 1u));
        return false;
    }
    *addr = at;
    return true;
}

/* A span a code writes outright: also not code the recompiler translated, asked at the address
 * the span has in this run. */
static bool check_write(Checker *k, uint32_t file_line, uint32_t *addr, uint64_t size) {
    const uint32_t named = *addr;
    if (!check_span(k, file_line, addr, size)) return false;
    if (k->host && k->host->is_code && k->host->is_code(k->host->ctx, *addr, (uint32_t)size)) {
        char here[40] = "";
        if (*addr != named) snprintf(here, sizeof(here), " (0x%08lX in this run)", (unsigned long)*addr);
        refuse(k->cheat, file_line,
               "writes %llu byte(s) at 0x%08lX%s, inside code the recompiler translated; a recompiled "
               "game does not read its code again at run time",
               (unsigned long long)size, (unsigned long)named, here);
        return false;
    }
    return true;
}

static void count_undo(Checker *k, uint64_t records, uint64_t bytes) {
    k->undo_records += records;
    k->undo_bytes += bytes;
}

static bool compare_code(uint32_t code, uint8_t *out) {
    if (code > 3u) return false;
    *out = (uint8_t)code;
    return true;
}

/* Reads the code that starts at line i into op. Returns the lines it takes, or 0 with the
 * cheat refused. */
static uint32_t check_code(Checker *k, size_t i, CheatOp *op, CheatWalk *walks, uint32_t *walk_count) {
    Cheat *c = k->cheat;
    const CodeLine *l = &c->lines[i];
    const CodeLine *l2 = i + 1u < c->line_count ? &c->lines[i + 1u] : NULL;
    const uint32_t a = l->a, b = l->b, at = l->file_line;
    const unsigned type = a >> 28;
    memset(op, 0, sizeof(*op));
    op->line = at;
    op->lines = 1u;
    if ((type == 0x4u || type == 0x5u || type == 0x6u || type == 0x8u) && !l2) {
        refuse(c, at, "is the first line of a two-line code (type 0x%X) and the cheat ends there", type);
        return 0u;
    }
    switch (type) {
    case 0x0u:
    case 0x1u:
    case 0x2u:
        op->kind = OP_WRITE;
        op->size = type == 0x0u ? 1u : type == 0x1u ? 2u : 4u;
        op->addr = code_address(a);
        op->value = b;
        if (op->size < 4u && (b >> (8u * op->size)) != 0u) {
            refuse(c, at, "is a %s write (type 0x%X) with a value wider than that", k_size_words[op->size], type);
            return 0u;
        }
        if (!check_write(k, at, &op->addr, op->size)) return 0u;
        count_undo(k, 1u, op->size);
        return 1u;
    case 0x3u: {
        const unsigned form = (a >> 20) & 0xFFu;
        if (form < 1u || form > 6u || (b >> 28) != 0u) {
            refuse(c, at, "is an add or subtract code (type 0x3) in a form that is not supported");
            return 0u;
        }
        op->kind = (form & 1u) ? OP_ADD : OP_SUB;
        op->size = form <= 2u ? 1u : form <= 4u ? 2u : 4u;
        op->addr = code_address(b);
        if (op->size == 4u) {
            if (!l2) {
                refuse(c, at, "is a 32-bit add or subtract and the line with its value is missing");
                return 0u;
            }
            if ((a & 0x000FFFFFu) != 0u || l2->b != 0u) {
                refuse(c, at, "is a 32-bit add or subtract with bits set where the form has zeros");
                return 0u;
            }
            op->value = l2->a;
            op->lines = 2u;
        } else {
            op->value = a & (op->size == 1u ? 0xFFu : 0xFFFFu);
            if ((a & 0x000FFFFFu) != op->value) {
                refuse(c, at, "is a %s add or subtract with a value wider than that", k_size_words[op->size]);
                return 0u;
            }
        }
        if (!check_write(k, at, &op->addr, op->size)) return 0u;
        count_undo(k, 1u, op->size);
        return op->lines;
    }
    case 0x4u:
    case 0x8u: {
        uint64_t extent;
        op->kind = OP_MULTI;
        op->lines = 2u;
        op->addr = code_address(a);
        op->count = b >> 16;
        op->add = l2->b;
        if (type == 0x4u) {
            op->size = 4u;
            op->value = l2->a;
        } else if ((l2->a >> 16) == 0x1000u) {
            op->size = 2u;
            op->value = l2->a & 0xFFFFu;
        } else if ((l2->a & 0xFFFFFF00u) == 0u) {
            op->size = 1u;
            op->value = l2->a;
        } else {
            refuse(c, l2->file_line, "is the value line of a multi-write (type 0x8) and is neither "
                                     "0x000000vv (8-bit) nor 0x1000vvvv (16-bit)");
            return 0u;
        }
        op->step = (b & 0xFFFFu) * op->size;
        if (op->count == 0u) {
            refuse(c, at, "is a multi-write with a count of 0, which writes nothing");
            return 0u;
        }
        extent = (uint64_t)(op->count - 1u) * op->step + op->size;
        if (!check_write(k, at, &op->addr, extent)) return 0u;
        count_undo(k, op->count, (uint64_t)op->count * op->size);
        return 2u;
    }
    case 0x5u:
        op->kind = OP_COPY;
        op->lines = 2u;
        op->addr = code_address(a);
        op->count = b;
        op->other = code_address(l2->a);
        if ((l2->a >> 28) != 0u || l2->b != 0u) {
            refuse(c, l2->file_line, "is the destination line of a copy (type 0x5) and is not 0x0bbbbbbb 0x00000000");
            return 0u;
        }
        if (op->count == 0u) {
            refuse(c, at, "is a copy of 0 bytes, which copies nothing");
            return 0u;
        }
        if (!check_span(k, at, &op->addr, op->count) || !check_write(k, at, &op->other, op->count)) return 0u;
        if (op->addr < op->other ? op->other - op->addr < op->count : op->addr - op->other < op->count) {
            refuse(c, at, "is a copy whose source and destination overlap");
            return 0u;
        }
        count_undo(k, 1u, op->count);
        return 2u;
    case 0x6u: {
        const unsigned form = (l2->a >> 16) & 0xFu;
        const uint32_t points = l2->a & 0xFFFFu;
        op->kind = OP_POINTER;
        op->addr = code_address(a);
        op->value = b;
        op->other = l2->b;
        if ((l2->a >> 20) != 0u) {
            refuse(c, l2->file_line, "is a pointer code (type 0x6) with a base offset, which is not supported");
            return 0u;
        }
        if (form > 5u) {
            refuse(c, l2->file_line, "is a pointer code (type 0x6) whose write type is not 0 to 5");
            return 0u;
        }
        op->size = (form % 3u) == 0u ? 1u : (form % 3u) == 1u ? 2u : 4u;
        op->negate = form >= 3u;
        if (op->size < 4u && (b >> (8u * op->size)) != 0u) {
            refuse(c, at, "is a %s pointer write with a value wider than that", k_size_words[op->size]);
            return 0u;
        }
        if (points == 0u) {
            refuse(c, l2->file_line, "is a pointer code (type 0x6) with a pointer count of 0");
            return 0u;
        }
        if ((size_t)(points - 1u) > c->line_count - (i + 2u)) {
            refuse(c, l2->file_line, "is a pointer code that needs %lu more line(s) and the cheat ends first",
                   (unsigned long)(points - 1u));
            return 0u;
        }
        op->step = *walk_count;
        for (uint32_t n = 0u; n < points - 1u; n++) {
            const CodeLine *w = &c->lines[i + 2u + n];
            const uint32_t words[2] = { w->a, w->b };
            for (unsigned part = 0u; part < 2u; part++) {
                const unsigned kind = words[part] >> 28;
                if (part == 1u && words[1] == 0u) break;
                if (kind != 0x2u && kind != 0x3u) {
                    refuse(c, w->file_line,
                           kind == 0x1u && part == 0u ? "is a pointer copy line (0x1), which is not supported"
                           : kind == 0x9u && part == 0u ? "is a pointer multi-address line (0x9), which is not supported"
                           : "is a pointer code's extra line and is not a pointer step (0x2 or 0x3)");
                    return 0u;
                }
                walks[*walk_count].offset = words[part] & 0x0FFFFFFFu;
                walks[*walk_count].negative = kind == 0x3u;
                (*walk_count)++;
                op->count++;
            }
        }
        op->lines = 2u + (points - 1u);
        if (!check_span(k, at, &op->addr, 4u)) return 0u;
        count_undo(k, 1u, op->size);
        return op->lines;
    }
    case 0x7u: {
        const unsigned form = b >> 16;
        if (form > 5u) {
            refuse(c, at, "is a boolean code (type 0x7) whose operation is not 0 to 5");
            return 0u;
        }
        op->kind = form <= 1u ? OP_OR : form <= 3u ? OP_AND : OP_XOR;
        op->size = (form & 1u) ? 2u : 1u;
        op->addr = code_address(a);
        op->value = b & 0xFFFFu;
        if (op->size == 1u && op->value > 0xFFu) {
            refuse(c, at, "is an 8-bit boolean code with a value wider than that");
            return 0u;
        }
        if (!check_write(k, at, &op->addr, op->size)) return 0u;
        count_undo(k, 1u, op->size);
        return 1u;
    }
    case 0xDu: {
        const unsigned form = b >> 28;
        if (form == 0x0u || form == 0x2u) {
            op->kind = OP_IF_VALUE;
            op->size = form == 0x0u ? 2u : 1u;
            op->addr = code_address(a);
            op->value = b & (op->size == 1u ? 0xFFu : 0xFFFFu);
            op->skip_to = 1u;
            if (!compare_code((b >> 20) & 0xFFu, &op->compare) || (b & 0x000FFFFFu) != op->value) {
                refuse(c, at, "is a %s test (type 0xD) in a form that is not supported", k_size_words[op->size]);
                return 0u;
            }
            return check_span(k, at, &op->addr, op->size) ? 1u : 0u;
        }
        if (form == 0x1u || form == 0x3u) {
            op->kind = OP_IF_BUTTONS;
            op->negate = form == 0x3u;
            op->value = b & 0x0FFFFFFFu;
            op->skip_to = (a & 0xFFu) + 1u;
            if ((a & 0x0FFFFF00u) != 0u) {
                refuse(c, at, "is a button test (type 0xD) with bits set where the form has zeros");
                return 0u;
            }
            if (op->value == 0u) {
                refuse(c, at, "is a button test that names no button");
                return 0u;
            }
            return 1u;
        }
        if (form >= 0x4u && form <= 0x7u) {
            if (!l2) {
                refuse(c, at, "is the first line of a two-address test (type 0xD) and the cheat ends there");
                return 0u;
            }
            op->kind = OP_IF_ADDRESS;
            op->lines = 2u;
            op->compare = (uint8_t)(form - 0x4u);
            op->addr = code_address(a);
            op->other = code_address(b);
            op->skip_to = l2->a;
            if (l2->b > 2u) {
                refuse(c, l2->file_line, "is the second line of a two-address test and its size is not 0, 1 or 2");
                return 0u;
            }
            op->size = (uint8_t)(1u << l2->b);
            if (op->size == 4u && op->compare >= CMP_LT) {
                refuse(c, at, "is a 32-bit less-than or greater-than test of two addresses, which is not supported");
                return 0u;
            }
            if (!check_span(k, at, &op->addr, op->size) || !check_span(k, at, &op->other, op->size)) return 0u;
            return 2u;
        }
        refuse(c, at, "is a test (type 0xD) in a form that is not supported");
        return 0u;
    }
    case 0xEu: {
        const unsigned head = a >> 24;
        if (head != 0xE0u && head != 0xE1u) {
            refuse(c, at, "is a multi-line test (type 0xE) that is neither 0xE0 (16-bit) nor 0xE1 (8-bit)");
            return 0u;
        }
        op->kind = OP_IF_VALUE;
        op->size = head == 0xE0u ? 2u : 1u;
        op->addr = code_address(b);
        op->value = a & (op->size == 1u ? 0xFFu : 0xFFFFu);
        op->skip_to = (a >> 16) & 0xFFu;
        if (!compare_code(b >> 28, &op->compare) || (a & 0xFFFFu) != op->value) {
            refuse(c, at, "is a %s multi-line test (type 0xE) in a form that is not supported", k_size_words[op->size]);
            return 0u;
        }
        return check_span(k, at, &op->addr, op->size) ? 1u : 0u;
    }
    case 0xAu:
        refuse(c, at, "is code type 0xA (PPSSPP's vibration and shader codes), which is not supported");
        return 0u;
    case 0xBu:
        refuse(c, at, "is code type 0xB (delay), which is not supported");
        return 0u;
    case 0xCu:
        refuse(c, at, "is code type 0xC (32-bit equality stop), which is not supported");
        return 0u;
    default:
        refuse(c, at, "is code type 0x%X, which is not supported", type);
        return 0u;
    }
}

static bool op_is_test(const CheatOp *op) {
    return op->kind == OP_IF_VALUE || op->kind == OP_IF_ADDRESS || op->kind == OP_IF_BUTTONS;
}

/* Checks every line of a cheat and builds what the applier runs. A refusal leaves nothing to
 * run. */
static void check_cheat(NkCheatSet *set, Cheat *c, const NkCheatHost *host) {
    Checker k;
    const size_t n = c->line_count;
    CheatOp *ops = NULL;
    CheatWalk *walks = NULL;
    uint32_t *op_at_line = NULL;   /* the code that starts at each line, or UINT32_MAX */
    uint32_t op_count = 0u, walk_count = 0u;
    memset(&k, 0, sizeof(k));
    k.set = set;
    k.cheat = c;
    k.host = host;
    if (c->refused_line == 0u && n == 0u) refuse(c, c->file_line, "has no code lines");
    if (c->refused_line != 0u) goto done;
    if (n >= UINT32_MAX / 2u || n > SIZE_MAX / sizeof(*ops) / 2u) {
        refuse(c, c->file_line, "has too many lines");
        goto done;
    }
    ops = (CheatOp *)calloc(n, sizeof(*ops));
    walks = (CheatWalk *)calloc(n * 2u, sizeof(*walks));
    op_at_line = (uint32_t *)malloc((n + 1u) * sizeof(*op_at_line));
    if (!ops || !walks || !op_at_line) {
        set->out_of_memory = true;
        goto done;
    }
    for (size_t i = 0u; i <= n; i++) op_at_line[i] = UINT32_MAX;
    for (size_t i = 0u; i < n;) {
        uint32_t taken = check_code(&k, i, &ops[op_count], walks, &walk_count);
        if (taken == 0u) goto done;
        op_at_line[i] = op_count++;
        i += taken;
        if (k.undo_bytes > NK_CHEATS_MAX_FRAME_BYTES) {
            refuse(c, ops[op_count - 1u].line, "brings what the cheat can write in one frame to more than %lu bytes",
                   (unsigned long)NK_CHEATS_MAX_FRAME_BYTES);
            goto done;
        }
        if (k.undo_records > NK_CHEATS_MAX_FRAME_WRITES) {
            refuse(c, ops[op_count - 1u].line, "brings what the cheat does in one frame to more than %lu writes",
                   (unsigned long)NK_CHEATS_MAX_FRAME_WRITES);
            goto done;
        }
    }
    op_at_line[n] = op_count;
    /* A test's count of lines becomes the code it continues at when it fails. */
    for (size_t i = 0u; i < n; i++) {
        CheatOp *op;
        uint64_t target;
        if (op_at_line[i] == UINT32_MAX) continue;
        op = &ops[op_at_line[i]];
        if (!op_is_test(op)) continue;
        target = (uint64_t)i + op->lines + op->skip_to;
        if (op->skip_to == 0u) {
            refuse(c, op->line, "is a test that guards no line");
        } else if (target > n) {
            refuse(c, op->line, "is a test that guards %lu line(s) and only %lu follow in the cheat",
                   (unsigned long)op->skip_to, (unsigned long)(n - i - op->lines));
        } else if (op_at_line[target] == UINT32_MAX) {
            refuse(c, op->line, "is a test whose %lu guarded line(s) end inside a multi-line code",
                   (unsigned long)op->skip_to);
        } else {
            op->skip_to = op_at_line[target];
            continue;
        }
        goto done;
    }
    c->ops = ops;
    c->op_count = op_count;
    c->walks = walks;
    c->walk_count = walk_count;
    c->undo_records = (uint32_t)k.undo_records;
    c->undo_bytes = (uint32_t)k.undo_bytes;
    ops = NULL;
    walks = NULL;
done:
    free(ops);
    free(walks);
    free(op_at_line);
    free(c->lines);
    c->lines = NULL;
    c->line_count = c->line_cap = 0u;
}

/* ---- loading ------------------------------------------------------------------------------- */

static bool cheat_applies(const Cheat *c) {
    return c->enabled && c->refused_line == 0u;
}

NkCheatSet *nk_cheats_load(const char *text, size_t size, const NkCheatConfig *config,
                           const NkCheatHost *host, char *error, size_t error_size) {
    NkCheatSet *set;
    uint32_t undo_records = 0u, undo_bytes = 0u;
    uint64_t frame_writes = 0u, frame_bytes = 0u, frame_codes = 0u;
    set_error(error, error_size, "");
    if ((!text && size != 0u) || !config || config->user_base >= config->user_end) {
        set_error(error, error_size, "the cheat reader was given no text or no user memory");
        return NULL;
    }
    if (config->hole_base > config->hole_end ||
        (config->image_size != 0u && (config->image_size - 1u > UINT32_MAX - config->psp_image_base ||
                                      config->image_size - 1u > UINT32_MAX - config->image_base))) {
        set_error(error, error_size, "the cheat reader was given a memory layout that cannot be");
        return NULL;
    }
    if (size > NK_CHEATS_MAX_FILE_BYTES) {
        set_error(error, error_size, "the cheat file is larger than 64 MiB");
        return NULL;
    }
    set = (NkCheatSet *)calloc(1u, sizeof(*set));
    if (!set) {
        set_error(error, error_size, "out of memory");
        return NULL;
    }
    set->user_base = config->user_base;
    set->user_end = config->user_end;
    set->hole_base = config->hole_base;
    set->hole_end = config->hole_end;
    set->psp_image_base = config->psp_image_base;
    set->image_base = config->image_base;
    set->image_size = config->image_size;
    if (config->disc_id) {
        normal_disc_id(set->disc_id, sizeof(set->disc_id), config->disc_id, strlen(config->disc_id));
    }
    parse_text(set, text, size);
    for (size_t i = 0u; i < set->count && !set->out_of_memory; i++) {
        Cheat *c = &set->cheats[i];
        check_cheat(set, c, host);
        if (!cheat_applies(c)) continue;
        /* The frame's budget is handed out in file order. A cheat that does not fit is refused
         * whole, like any other refusal; a smaller one after it may still fit. */
        {
            const uint64_t codes = (uint64_t)c->op_count + c->walk_count;
            if (frame_writes + c->undo_records > NK_CHEATS_MAX_FRAME_WRITES ||
                frame_bytes + c->undo_bytes > NK_CHEATS_MAX_FRAME_BYTES ||
                frame_codes + codes > NK_CHEATS_MAX_FRAME_CODES) {
                refuse(c, c->file_line,
                       "would bring what the file's cheats do in one frame past the limit (%lu writes, %lu "
                       "bytes, %lu codes); turn another cheat off to use this one",
                       (unsigned long)NK_CHEATS_MAX_FRAME_WRITES, (unsigned long)NK_CHEATS_MAX_FRAME_BYTES,
                       (unsigned long)NK_CHEATS_MAX_FRAME_CODES);
                c->over_limit = true;
                if (set->over_limit++ == 0u) set->over_limit_line = c->file_line;
                free(c->ops);
                free(c->walks);
                c->ops = NULL;
                c->walks = NULL;
                c->op_count = c->walk_count = 0u;
                continue;
            }
            frame_writes += c->undo_records;
            frame_bytes += c->undo_bytes;
            frame_codes += codes;
        }
        if (c->undo_records > undo_records) undo_records = c->undo_records;
        if (c->undo_bytes > undo_bytes) undo_bytes = c->undo_bytes;
    }
    if (!set->out_of_memory && undo_records > 0u) {
        set->undo = (UndoRecord *)malloc((size_t)undo_records * sizeof(*set->undo));
        set->undo_data = (uint8_t *)malloc(undo_bytes > 0u ? undo_bytes : 1u);
        if (!set->undo || !set->undo_data) set->out_of_memory = true;
        set->undo_cap = undo_records;
        set->undo_data_cap = undo_bytes;
    }
    if (set->out_of_memory) {
        nk_cheats_free(set);
        set_error(error, error_size, "out of memory");
        return NULL;
    }
    return set;
}

#ifndef NK_CHEATS_NO_HOST_IO
NkCheatSet *nk_cheats_load_file(const char *path, const NkCheatConfig *config,
                                const NkCheatHost *host, char *error, size_t error_size) {
    NkCheatSet *set = NULL;
    char *text = NULL;
    size_t size = 0u, cap = 0u;
    FILE *f;
    set_error(error, error_size, "");
    if (!path || !path[0]) {
        set_error(error, error_size, "no cheat file is named");
        return NULL;
    }
    f = nk_fopen_utf8(path, "rb");
    if (!f) {
        set_error(error, error_size, "the cheat file cannot be opened");
        return NULL;
    }
    for (;;) {
        size_t got;
        if (size == cap) {
            size_t next_cap = cap == 0u ? 65536u : cap * 2u;
            char *next = next_cap > cap ? (char *)realloc(text, next_cap) : NULL;
            if (!next) {
                set_error(error, error_size, "out of memory");
                goto done;
            }
            text = next;
            cap = next_cap;
        }
        got = fread(text + size, 1u, cap - size, f);
        size += got;
        if (size > NK_CHEATS_MAX_FILE_BYTES) {
            set_error(error, error_size, "the cheat file is larger than 64 MiB");
            goto done;
        }
        if (got == 0u) break;
    }
    if (ferror(f)) {
        set_error(error, error_size, "the cheat file cannot be read");
        goto done;
    }
    set = nk_cheats_load(text, size, config, host, error, error_size);
done:
    fclose(f);
    free(text);
    return set;
}
#endif

void nk_cheats_free(NkCheatSet *set) {
    if (!set) return;
    for (size_t i = 0u; i < set->count; i++) {
        free(set->cheats[i].lines);
        free(set->cheats[i].ops);
        free(set->cheats[i].walks);
    }
    free(set->cheats);
    free(set->undo);
    free(set->undo_data);
    free(set);
}

size_t nk_cheats_count(const NkCheatSet *set) {
    return set ? set->count : 0u;
}

bool nk_cheats_info(const NkCheatSet *set, size_t index, NkCheatInfo *out) {
    const Cheat *c;
    if (!set || !out || index >= set->count) return false;
    c = &set->cheats[index];
    out->name = c->name;
    out->state = !c->enabled ? NK_CHEAT_OFF : c->refused_line != 0u ? NK_CHEAT_REFUSED : NK_CHEAT_ON;
    out->file_line = c->file_line;
    out->refused_line = c->refused_line;
    out->reason = c->reason;
    out->applied_frames = c->applied_frames;
    out->held_frames = c->held_frames;
    out->held = c->held;
    out->over_frame_limit = c->over_limit;
    return true;
}

void nk_cheats_summary(const NkCheatSet *set, NkCheatSummary *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!set) return;
    for (size_t i = 0u; i < set->count; i++) {
        const Cheat *c = &set->cheats[i];
        if (!c->enabled) out->off++;
        else if (c->refused_line != 0u) out->refused++;
        else out->on++;
    }
    out->other_game = set->other_game;
    out->notes = set->notes_total;
    out->over_frame_limit = set->over_limit;
    out->over_frame_limit_line = set->over_limit_line;
    out->not_kept = set->not_kept;
}

size_t nk_cheats_note_count(const NkCheatSet *set) {
    return set ? set->notes_kept : 0u;
}

const char *nk_cheats_note(const NkCheatSet *set, size_t index) {
    return set && index < set->notes_kept ? set->notes[index] : NULL;
}

/* ---- applying ------------------------------------------------------------------------------ */

typedef struct {
    NkCheatSet *set;
    const NkCheatHost *host;
    uint32_t buttons;
    uint8_t why;         /* HoldWhy, once a code cannot run on this frame */
    uint32_t why_addr;
} Frame;

/* A pointer a game stores may carry the kernel-mode address bits; guest memory is the same. */
static uint32_t user_address(uint32_t addr) {
    return addr & 0x3FFFFFFFu;
}

static uint32_t load(const Frame *f, uint32_t addr, uint32_t size) {
    uint8_t bytes[4] = { 0u, 0u, 0u, 0u };
    f->host->read(f->host->ctx, addr, bytes, size);
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static bool hold(Frame *f, HoldWhy why, uint32_t addr) {
    f->why = (uint8_t)why;
    f->why_addr = addr;
    return false;
}

/* Asked before every write, not only when the file is checked: a module can be loaded over an
 * address later, and a pointer's target is known only now. */
static bool writes_code(Frame *f, uint32_t addr, uint32_t size) {
    if (!f->host->is_code || !f->host->is_code(f->host->ctx, addr, size)) return false;
    (void)hold(f, HOLD_CODE, addr);
    return true;
}

/* Keeps a span's present bytes, so a cheat held later on this frame can be undone. */
static bool keep_for_undo(Frame *f, uint32_t addr, uint32_t size) {
    NkCheatSet *set = f->set;
    UndoRecord *r;
    if (set->undo_count == set->undo_cap || size > set->undo_data_cap - set->undo_used) {
        return hold(f, HOLD_UNDO, addr);
    }
    r = &set->undo[set->undo_count++];
    r->addr = addr;
    r->size = size;
    r->at = set->undo_used;
    f->host->read(f->host->ctx, addr, set->undo_data + set->undo_used, size);
    set->undo_used += size;
    return true;
}

/* Writes a value to a span already known not to be code. */
static bool store_data(Frame *f, uint32_t addr, uint32_t size, uint32_t value) {
    uint8_t bytes[4];
    if (!keep_for_undo(f, addr, size)) return false;
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
    f->host->write(f->host->ctx, addr, bytes, size);
    return true;
}

static bool store(Frame *f, uint32_t addr, uint32_t size, uint32_t value) {
    return !writes_code(f, addr, size) && store_data(f, addr, size, value);
}

static void undo_all(Frame *f) {
    NkCheatSet *set = f->set;
    while (set->undo_count > 0u) {
        const UndoRecord *r = &set->undo[--set->undo_count];
        f->host->write(f->host->ctx, r->addr, set->undo_data + r->at, r->size);
    }
    set->undo_used = 0u;
}

static bool compare(uint8_t how, uint32_t memory, uint32_t value) {
    switch (how) {
    case CMP_EQ: return memory == value;
    case CMP_NE: return memory != value;
    case CMP_LT: return memory < value;
    default: return memory > value;
    }
}

/* Runs one cheat's codes. False when a code cannot run on this frame: f->why says why, and
 * *held_line the code's line; the caller undoes what the cheat wrote. */
static bool run_cheat(Frame *f, const Cheat *c, uint32_t *held_line) {
    const NkCheatSet *set = f->set;
    for (uint32_t pc = 0u; pc < c->op_count;) {
        const CheatOp *op = &c->ops[pc];
        bool ok = true;
        bool pass = true;
        *held_line = op->line;
        switch (op->kind) {
        case OP_WRITE:
            ok = store(f, op->addr, op->size, op->value);
            break;
        case OP_ADD:
        case OP_SUB:
        case OP_OR:
        case OP_AND:
        case OP_XOR: {
            uint32_t v = load(f, op->addr, op->size);
            v = op->kind == OP_ADD ? v + op->value : op->kind == OP_SUB ? v - op->value
              : op->kind == OP_OR ? (v | op->value) : op->kind == OP_AND ? (v & op->value)
              : (v ^ op->value);
            ok = store(f, op->addr, op->size, v);
            break;
        }
        case OP_MULTI: {
            const uint64_t extent = (uint64_t)(op->count - 1u) * op->step + op->size;
            /* One question for the whole run: a run that reaches over code is held whole. */
            ok = !writes_code(f, op->addr, (uint32_t)extent);
            for (uint32_t n = 0u; ok && n < op->count; n++) {
                ok = store_data(f, op->addr + n * op->step, op->size, op->value + n * op->add);
            }
            break;
        }
        case OP_COPY: {
            uint8_t block[1024];
            ok = !writes_code(f, op->other, op->count) && keep_for_undo(f, op->other, op->count);
            for (uint32_t done = 0u; ok && done < op->count;) {
                uint32_t part = op->count - done < sizeof(block) ? op->count - done : (uint32_t)sizeof(block);
                f->host->read(f->host->ctx, op->addr + done, block, part);
                f->host->write(f->host->ctx, op->other + done, block, part);
                done += part;
            }
            break;
        }
        case OP_POINTER: {
            /* In a run whose image is not where a PSP has it (NkCheatConfig), a null pointer plus
             * an offset can land on the game's own bytes: an image placed at 0 starts there. On
             * a PSP the same address is below user memory and the cheat is held, so a pointer
             * that is null holds it here too. */
            const bool moved = set->image_size != 0u;
            uint32_t pointer = load(f, op->addr, 4u);
            uint32_t target;
            if (moved && pointer == 0u) ok = hold(f, HOLD_NULL, op->addr);
            for (uint32_t n = 0u; ok && n < op->count; n++) {
                const CheatWalk *w = &c->walks[op->step + n];
                uint32_t at = user_address(w->negative ? pointer - w->offset : pointer + w->offset);
                if (!span_in_user_memory(set, at, 4u)) {
                    ok = hold(f, HOLD_SPAN, at);
                } else {
                    pointer = load(f, at, 4u);
                    if (moved && pointer == 0u) ok = hold(f, HOLD_NULL, at);
                }
            }
            if (!ok) break;
            target = user_address(op->negate ? pointer - op->other : pointer + op->other);
            if (!span_in_user_memory(set, target, op->size)) ok = hold(f, HOLD_SPAN, target);
            else ok = store(f, target, op->size, op->value);
            break;
        }
        case OP_IF_VALUE:
            pass = compare(op->compare, load(f, op->addr, op->size), op->value);
            break;
        case OP_IF_ADDRESS:
            pass = compare(op->compare, load(f, op->addr, op->size), load(f, op->other, op->size));
            break;
        case OP_IF_BUTTONS:
            pass = ((f->buttons & op->value) == op->value) != (op->negate != 0u);
            break;
        default:
            ok = hold(f, HOLD_UNDO, 0u);
            break;
        }
        if (!ok) return false;
        pc = pass ? pc + 1u : op->skip_to;
    }
    return true;
}

static void report_hold(const Frame *f, const Cheat *c) {
    char line[NK_CHEATS_NAME_MAX + 256u];
    char why[96];
    if (!f->host->report) return;
    if (f->why == HOLD_CODE) {
        snprintf(why, sizeof(why), "its write at 0x%08lX is inside code the recompiler translated",
                 (unsigned long)f->why_addr);
    } else if (f->why == HOLD_SPAN) {
        snprintf(why, sizeof(why), "its pointer leads to 0x%08lX, outside user memory",
                 (unsigned long)f->why_addr);
    } else if (f->why == HOLD_NULL) {
        snprintf(why, sizeof(why), "the pointer it follows at 0x%08lX is null", (unsigned long)f->why_addr);
    } else {
        snprintf(why, sizeof(why), "it needs more undo space than was counted for it");
    }
    snprintf(line, sizeof(line), "cheat \"%s\" is held: line %lu: %s; nothing of the cheat is applied "
                                 "on a frame like this", c->name, (unsigned long)c->held_line, why);
    f->host->report(f->host->ctx, line);
}

uint32_t nk_cheats_apply(NkCheatSet *set, const NkCheatHost *host) {
    Frame f;
    uint32_t applied = 0u;
    if (!set || !host || !host->read || !host->write) return 0u;
    memset(&f, 0, sizeof(f));
    f.set = set;
    f.host = host;
    f.buttons = host->buttons ? host->buttons(host->ctx) : 0u;
    for (size_t i = 0u; i < set->count; i++) {
        Cheat *c = &set->cheats[i];
        uint32_t held_line = 0u;
        if (!cheat_applies(c)) continue;
        set->undo_count = 0u;
        set->undo_used = 0u;
        f.why = HOLD_NONE;
        if (run_cheat(&f, c, &held_line)) {
            applied++;
            c->applied_frames++;
            /* Said the first time only: a pointer that comes and goes every frame would
             * otherwise write a line a frame. nk_cheats_report_holds gives the counts. */
            if (c->held && !c->said_again && host->report) {
                char line[NK_CHEATS_NAME_MAX + 96u];
                c->said_again = true;
                snprintf(line, sizeof(line), "cheat \"%s\" applies again; a later hold for the same "
                                             "cause is counted, not said", c->name);
                host->report(host->ctx, line);
            }
            c->held = false;
            continue;
        }
        undo_all(&f);
        c->held_frames++;
        c->held = true;
        c->held_why = f.why;
        c->held_line = held_line;
        /* Said once per cause for each cheat, not once per frame and not once per change. */
        if ((c->said_why & (1u << f.why)) == 0u) {
            c->said_why |= (uint8_t)(1u << f.why);
            report_hold(&f, c);
        }
    }
    return applied;
}

void nk_cheats_report_holds(const NkCheatSet *set, const NkCheatHost *host) {
    uint32_t said = 0u, more = 0u;
    if (!set || !host || !host->report) return;
    for (size_t i = 0u; i < set->count; i++) {
        const Cheat *c = &set->cheats[i];
        char line[NK_CHEATS_NAME_MAX + 128u];
        if (c->held_frames == 0u) continue;
        if (said == NK_CHEATS_MAX_HOLD_TOTALS) {
            more++;
            continue;
        }
        said++;
        snprintf(line, sizeof(line), "cheat \"%s\" was held on %llu frame(s) and applied on %llu", c->name,
                 (unsigned long long)c->held_frames, (unsigned long long)c->applied_frames);
        host->report(host->ctx, line);
    }
    if (more != 0u) {
        char line[96];
        snprintf(line, sizeof(line), "and %lu more cheat(s) were held on some frames", (unsigned long)more);
        host->report(host->ctx, line);
    }
}
