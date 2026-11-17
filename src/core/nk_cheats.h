/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2012- PPSSPP Project.
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * Cheat codes in the CWCheat text format (#2639): the interface of nk_cheats.c.
 *
 * The line types and code forms this header lists, and the address base
 * NK_CHEATS_ADDRESS_BASE, are translated from PPSSPP's CWCheat engine
 * (https://github.com/hrydgard/ppsspp), Core/CwCheat.cpp at revision
 * c437463bc5cb78a0236312ad23f9c3b56fb9ae67 (GPL-2.0-or-later): CheatFileParser::ParseLine,
 * CWCheatEngine::InterpretNextCwCheat and CWCheatEngine::ExecuteOp, with
 * CWCheatEngine::GetAddress in Core/CwCheat.h. Label: derived-translated
 * (docs/provenance/INDEPENDENCE_MODEL.md); PPSSPP-derived, not PSP-measured. This file is
 * conveyed as GPL-3.0-or-later.
 *
 * Core/CwCheat.cpp and Core/CwCheat.h carry no copyright or licence line of their own at that
 * revision. The PPSSPP line above is the notice PPSSPP's source files carry, copied from the
 * first line of Core/Core.cpp at the same revision; that file's notice and PPSSPP's LICENSE.TXT
 * give the licence, GPL version 2.0 or later.
 *
 * Modified for this project (2026-11-16; see src/core/PROVENANCE.md and the list in
 * nk_cheats.c):
 *   - The declarations below (NkCheatHost, NkCheatConfig, NkCheatInfo, NkCheatSummary, the
 *     nk_cheats_* functions and the limits) are this project's own; PPSSPP has no counterpart
 *     to them. No line of PPSSPP's code was copied.
 *   - The table of code forms gives each form's fields as InterpretNextCwCheat reads them,
 *     narrowed to the forms that run here. The refusals, the check made when a file is read
 *     and the whole-or-nothing rule are this project's own. */

#ifndef NK_CHEATS_H
#define NK_CHEATS_H

/* Cheat codes in the CWCheat text format (#2639): the reader, the checks made before a code may
 * run, and the applier. The runtime binds it to guest memory (src/rt/cheats.h) and applies the
 * cheats that are on once per delivered VBLANK. Nothing here reads the network, and nothing
 * here knows a game: the user supplies the file.
 *
 * The format has no specification. The meaning of each line type and code form is PPSSPP's,
 * the public reference for the format (the note at the top of this file names the revision
 * and the functions; PPSSPP-derived, not PSP-measured). The reader is stricter than PPSSPP
 * wherever PPSSPP skips, guesses or widens: a line it cannot run exactly as written refuses
 * its whole cheat.
 *
 * The file
 *   _S <disc ID>     the lines that follow are for this disc (hyphens and case do not count)
 *   _G <name>        the game's name; not read
 *   _C0 <name>       a cheat that is off; _C1 to _C9 <name>, one that is on
 *   _L 0xAAAAAAAA 0xVVVVVVVV   one code line of the cheat above it
 *   // ... or # ...  a comment, also after a code line's two values
 * A file with no _S line is for whatever disc it is read for. Cheats under another disc's _S
 * line are not for this run: they are counted, and nothing else of them is kept.
 *
 * Code forms that run. aaaaaaa is an offset from 0x08800000; every address a code reads or
 * writes must lie whole inside user memory (NkCheatConfig), checked before anything is written.
 *   0x0aaaaaaa 0x000000vv                   write 8 bits
 *   0x1aaaaaaa 0x0000vvvv                   write 16 bits
 *   0x2aaaaaaa 0xvvvvvvvv                   write 32 bits
 *   0x301000vv 0x0aaaaaaa                   add vv to 8 bits      (0x302: subtract)
 *   0x3030vvvv 0x0aaaaaaa                   add vvvv to 16 bits   (0x304: subtract)
 *   0x30500000 0x0aaaaaaa / 0xvvvvvvvv 0x00000000   add to 32 bits (0x306: subtract)
 *   0x4aaaaaaa 0xnnnnssss / 0xvvvvvvvv 0xiiiiiiii   nnnn 32-bit writes, ssss * 4 bytes apart,
 *                                                   the value growing by iiiiiiii each time
 *   0x8aaaaaaa 0xnnnnssss / 0x000000vv 0xiiiiiiii   the same in 8 bits, ssss bytes apart
 *   0x8aaaaaaa 0xnnnnssss / 0x1000vvvv 0xiiiiiiii   the same in 16 bits, ssss * 2 bytes apart
 *   0x5aaaaaaa 0xnnnnnnnn / 0x0bbbbbbb 0x00000000   copy nnnnnnnn bytes from a to b (no overlap)
 *   0x6aaaaaaa 0xvvvvvvvv / 0x000tnnnn 0xiiiiiiii   through the pointer at a: write v at
 *       pointer + i (t = 0, 1, 2 for 8, 16, 32 bits) or pointer - i (t = 3, 4, 5); nnnn - 1
 *       more lines follow, each 0x2ooooooo (or 0x3ooooooo, a negative offset) in its first
 *       word, and the same or 0 in its second: the pointer becomes the pointer stored at
 *       pointer +/- o, once or twice per line, before the write
 *   0x7aaaaaaa 0x000t00vv / 0x000tvvvv     OR (t = 0, 1), AND (2, 3), XOR (4, 5) on 8 or 16 bits
 *   0xDaaaaaaa 0x00c0vvvv / 0x20c000vv     run the next line only if 16 or 8 bits at a are
 *                                          equal (c = 0), not equal (1), less (2), greater (3)
 *   0xD00000nn 0x1bbbbbbb                  run the next nn + 1 lines only while every button
 *                                          of the mask is held (0x3bbbbbbb: unless they all are)
 *   0xDaaaaaaa 0x4bbbbbbb / 0xnnnnnnnn 0x0000000s   run the next n lines only if the values at
 *       a and b are equal (0x5: not equal, 0x6: a less, 0x7: a greater); s = 0, 1, 2 for 8, 16,
 *       32 bits, and the two orderings only in 8 and 16 bits
 *   0xE0nnvvvv 0xcaaaaaaa / 0xE1nn00vv 0xcaaaaaaa   run the next nn lines only if 16 or 8 bits
 *                                                   at a compare as c says (as for 0xD)
 * Comparisons are unsigned. A condition must guard whole codes that follow it in the same cheat.
 *
 * Refused by name, with the whole cheat (nk_cheats_info gives the line and the reason): a line
 * that is not two hexadecimal words; code types 0x9, 0xA, 0xB, 0xC and 0xF; _M and _N lines
 * (PSP Action Replay codes); the 0x6 lines 0x1ooooooo (copy) and 0x9ooooooo (multi-address) and
 * a 0x6 with a base offset; any form above with bits set where it shows zeros; a count of 0; a
 * condition that guards no line, runs past the end of its cheat or ends inside a multi-line
 * code; an address outside user memory; a cheat that could write more than
 * NK_CHEATS_MAX_FRAME_BYTES in one frame; and a write into code the recompiler translated
 * (NkCheatHost.is_code), because a recompiled game does not read its code again at run time.
 *
 * A cheat applies completely on a frame or not at all. A pointer that leads outside user memory
 * on some frame, or into translated code, holds its cheat for that frame: what the cheat had
 * already written on that frame is put back, and the reason is reported once per cause. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The address a code's 28-bit offset counts from: the start of PSP user memory. */
#define NK_CHEATS_ADDRESS_BASE 0x08800000u
/* The PPSSPP revision the code forms' meaning is translated from. A run that applies cheats
 * names it once, with the words "PPSSPP-derived, not PSP-measured" (src/rt/cheats.c). */
#define NK_CHEATS_PPSSPP_REVISION "c437463bc5cb78a0236312ad23f9c3b56fb9ae67"
/* A cheat file larger than this is refused whole. A full cheat.db is about 10 MiB. */
#define NK_CHEATS_MAX_FILE_BYTES (64u * 1024u * 1024u)
/* What the reader keeps for one disc: cheats (_C lines) and code lines (_L lines). More are left
 * out by name. A cheat record is about 420 bytes and a code line at most 60 once it is checked,
 * so the two bounds hold what any file can make the reader allocate to about 18 MiB, where the
 * file cap alone allowed several GiB. The largest list in a cheat.db is a few hundred cheats. */
#define NK_CHEATS_MAX_CHEATS 4096u
#define NK_CHEATS_MAX_LINES 262144u
/* What the cheats that are on may do in one frame, all of them together: separate writes, bytes
 * written, and codes run. The cheats run on the scheduler's thread between two frames of the
 * game, so their cost is the game's: a frame is 16.68 ms, and the bound keeps a file's worst
 * case to a small part of it (src/rt/cheats.h has the measured figure), where a 100 KiB file of
 * 65,535-write codes took a second per frame. The budget is handed out in file order when the
 * file is read; a cheat that does not fit is refused by name. A hand-made cheat is a few codes
 * and a table fill a few hundred writes. The bytes bound is also the undo record's size. */
#define NK_CHEATS_MAX_FRAME_WRITES 16384u
#define NK_CHEATS_MAX_FRAME_BYTES (1024u * 1024u)
#define NK_CHEATS_MAX_FRAME_CODES 16384u
/* Cheats nk_cheats_report_holds names one by one. */
#define NK_CHEATS_MAX_HOLD_TOTALS 16u
/* Bytes kept of a cheat's name, and of a refusal's reason, with the terminator. */
#define NK_CHEATS_NAME_MAX 96u
#define NK_CHEATS_REASON_MAX 224u
/* File notes kept (lines that belong to no cheat); the summary counts every one. */
#define NK_CHEATS_MAX_NOTES 32u

typedef enum {
    NK_CHEAT_OFF = 0,     /* _C0: in the file for this disc, not turned on */
    NK_CHEAT_ON,          /* turned on and every line accepted: it is applied */
    NK_CHEAT_REFUSED      /* turned on, refused at load: it is never applied */
} NkCheatState;

typedef struct {
    const char *name;       /* cleaned for display; "(unnamed)" when the file gives none */
    NkCheatState state;
    uint32_t file_line;     /* its _C line, counted from 1 */
    /* The first line that refuses the cheat and why; 0 and "" when every line is accepted. An
     * off cheat carries them too, so a list can say it would be refused. */
    uint32_t refused_line;
    const char *reason;
    /* Frames it applied on, and frames it was held on, since the load. */
    uint64_t applied_frames;
    uint64_t held_frames;
    bool held;              /* held on the last frame */
    bool over_frame_limit;  /* refused because the file's cheats together pass the frame limit */
} NkCheatInfo;

typedef struct {
    uint32_t on;
    uint32_t off;
    uint32_t refused;
    uint32_t other_game;    /* cheats under another disc's _S line */
    uint32_t notes;         /* every file note, kept or not */
    uint32_t over_frame_limit;       /* of the refused: those refused for the frame limit */
    uint32_t over_frame_limit_line;  /* the _C line of the first of them */
    uint32_t not_kept;      /* cheats past NK_CHEATS_MAX_CHEATS: not read, not in the list */
} NkCheatSummary;

/* What a cheat may touch. disc_id selects the _S sections; NULL or "" matches none, so only a
 * file with no _S line applies. [user_base, user_end) is the memory of this run that codes may
 * read and write, less [hole_base, hole_end) when that is not empty (memory inside the range
 * that is not the game's).
 *
 * A code names a PSP address. image_size 0 says the run's memory is laid out as a PSP's, and
 * the address is used as written. Otherwise the game's image is not where a PSP has it: the
 * image a PSP loads at [psp_image_base, psp_image_base + image_size) is at [image_base,
 * image_base + image_size) in this run. Then a span a code names must lie whole inside the PSP's
 * image and is moved with it; a code that names any other address refuses its cheat, because
 * where the rest of a PSP's memory (its heap, its stacks) lives in such a run is not known. A
 * pointer a code follows is the game's own, read from this run's memory, and is used as it is. */
typedef struct {
    const char *disc_id;
    uint32_t user_base;
    uint32_t user_end;
    uint32_t hole_base;
    uint32_t hole_end;
    uint32_t psp_image_base;
    uint32_t image_base;
    uint32_t image_size;
} NkCheatConfig;

/* The guest, as the applier sees it. read and write are called only for spans the applier has
 * checked against NkCheatConfig, with addresses in [user_base, user_end). */
typedef struct {
    void *ctx;
    void (*read)(void *ctx, uint32_t addr, void *out, uint32_t size);
    void (*write)(void *ctx, uint32_t addr, const void *in, uint32_t size);
    /* Whether any byte of [addr, addr + size) is code the recompiler translated. */
    bool (*is_code)(void *ctx, uint32_t addr, uint32_t size);
    /* The pad's buttons as the guest reads them (SceCtrlData.Buttons). */
    uint32_t (*buttons)(void *ctx);
    /* One line, without a newline: the first time a cheat is held for each cause, the first time
     * it applies again, and the counts nk_cheats_report_holds gives. May be NULL. */
    void (*report)(void *ctx, const char *line);
} NkCheatHost;

typedef struct NkCheatSet NkCheatSet;

/* Reads and checks a cheat file's text. host may be NULL: then no write is checked against
 * translated code at load (a list in the player; the runtime always passes its host). Returns
 * NULL, with the reason in error, only when the text cannot be held at all (too large, out of
 * memory, a bad config); a file whose every cheat is refused still loads, so each refusal can
 * be named. */
NkCheatSet *nk_cheats_load(const char *text, size_t size, const NkCheatConfig *config,
                           const NkCheatHost *host, char *error, size_t error_size);
#ifndef NK_CHEATS_NO_HOST_IO
/* The same for a file (a UTF-8 path). */
NkCheatSet *nk_cheats_load_file(const char *path, const NkCheatConfig *config,
                                const NkCheatHost *host, char *error, size_t error_size);
#endif
void nk_cheats_free(NkCheatSet *set);

size_t nk_cheats_count(const NkCheatSet *set);
bool nk_cheats_info(const NkCheatSet *set, size_t index, NkCheatInfo *out);
void nk_cheats_summary(const NkCheatSet *set, NkCheatSummary *out);
/* File notes ("line 12: a code line before any _C line"), at most NK_CHEATS_MAX_NOTES kept. */
size_t nk_cheats_note_count(const NkCheatSet *set);
const char *nk_cheats_note(const NkCheatSet *set, size_t index);

/* Applies every cheat that is on, once, in file order. Returns how many applied completely;
 * the others were held and left memory as they found it. */
uint32_t nk_cheats_apply(NkCheatSet *set, const NkCheatHost *host);
/* Reports, for each cheat held on some frame since the load, how many frames it was held on
 * and how many it applied on: the count behind the hold lines that are said once. At most
 * NK_CHEATS_MAX_HOLD_TOTALS cheats are named, and one more line counts the rest. */
void nk_cheats_report_holds(const NkCheatSet *set, const NkCheatHost *host);

#ifdef __cplusplus
}
#endif

#endif /* NK_CHEATS_H */
