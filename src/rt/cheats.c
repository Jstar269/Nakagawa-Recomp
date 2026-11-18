/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Cheats in a run (#2639); cheats.h says when they apply and what is refused. This file binds
 * the reader and applier of src/core/nk_cheats.c to the run: the switch, the file, guest
 * memory, the translated code and the pad. */

#include "cheats.h"

#include "recomp.h"
#include "../core/nk_cheats.h"
#include "../core/nk_platform.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The guest's main memory a cheat may touch: from the address CWCheat offsets count from to the
 * end of the arena's RAM (recomp.c). Kernel memory below it, the scratchpad and VRAM are not
 * cheat memory. */
#define CHEATS_USER_END 0x0C000000u
/* Where a PSP loads a game's relocatable main module: the address a CWCheat written for the
 * game on a PSP, or in PPSSPP, counts the image from (PSP_DEFAULT_MAIN_LOAD_ADDRESS in
 * tools/nk_core/iso_inspect.py, which places every generic title's relocatable image there). */
#define CHEATS_PSP_IMAGE_BASE 0x08804000u
/* Refused cheats the start names one by one; the rest are counted on one line. */
#define CHEATS_MAX_REFUSAL_LINES 32u

static NkCheatSet *s_set;
static int s_started;      /* the switch was read for this run */
static SrCheatsStatus s_status;
static int s_hardcore;
static int s_no_marker;
static int s_program_gone; /* the program the file was read for is no longer the one running */
static uint32_t s_image_base, s_image_end;   /* the host-loaded image the file was read for */
static int s_origin_said;  /* this run has said where the code forms' meaning comes from */
static int s_run_ended;    /* sr_cheats_run_end has given the counts */
/* The marker's words, and what the thread that presents is given (sr_cheats_marker): s_marker
 * while the marker shows, else NULL. The words are written only when the counts they say
 * change, which is when a file is read, never while a presenter may hold them; stopping the
 * cheats only takes the pointer back. */
static char s_marker[SR_CHEATS_MARKER_MAX];
static _Atomic(const char *) s_marker_shown;
/* Set by the thread that presents (sr_cheats_marker_drawn), read at the next VBLANK on the
 * scheduler's thread, which is the only one that changes what is applied. */
static atomic_int s_marker_lost;

static void host_read(void *ctx, uint32_t addr, void *out, uint32_t size) {
    (void)ctx;
    if (!sr_guest_copy_in(out, addr, size)) memset(out, 0, size);
}

static void host_write(void *ctx, uint32_t addr, const void *in, uint32_t size) {
    (void)ctx;
    (void)sr_guest_copy_out(addr, in, size);
}

static bool host_is_code(void *ctx, uint32_t addr, uint32_t size) {
    (void)ctx;
    return sr_exec_span_overlaps(addr, size) != 0;
}

static uint32_t host_buttons(void *ctx) {
    (void)ctx;
    return sr_ctrl_live_buttons();
}

static void host_report(void *ctx, const char *line) {
    (void)ctx;
    fprintf(stderr, "CHEATS: %s\n", line);
}

static const NkCheatHost k_host = { NULL, host_read, host_write, host_is_code, host_buttons, host_report };

static void cheats_update(void) {
    const int stopped = s_hardcore || s_no_marker || s_program_gone;
    char words[SR_CHEATS_MARKER_MAX];
    s_status.applying = s_set && !stopped && s_status.on > 0u;
    if (!s_set || stopped || (s_status.on == 0u && s_status.refused == 0u)) {
        atomic_store_explicit(&s_marker_shown, NULL, memory_order_release);
        return;
    }
    if (s_status.refused > 0u) {
        snprintf(words, sizeof(words), "Cheats on: %lu (%lu refused)", (unsigned long)s_status.on,
                 (unsigned long)s_status.refused);
    } else {
        snprintf(words, sizeof(words), "Cheats on: %lu", (unsigned long)s_status.on);
    }
    if (strcmp(words, s_marker) != 0) {
        atomic_store_explicit(&s_marker_shown, NULL, memory_order_release);
        memcpy(s_marker, words, sizeof(words));
    }
    atomic_store_explicit(&s_marker_shown, (const char *)s_marker, memory_order_release);
}

void sr_cheats_run_end(void) {
    /* The counts behind the hold lines, each of which is said once (nk_cheats.h), and once per
     * run: main's end (driver.c) and sceKernelExitGame (hle_kernel_module.c) both ask. */
    if (s_set && !s_run_ended) nk_cheats_report_holds(s_set, &k_host);
    s_run_ended = 1;
}

void sr_cheats_reset(void) {
    nk_cheats_free(s_set);
    s_set = NULL;
    memset(&s_status, 0, sizeof(s_status));
    s_hardcore = 0;
    s_no_marker = 0;
    s_program_gone = 0;
    s_image_base = s_image_end = 0u;
    s_origin_said = 0;
    s_run_ended = 0;
    atomic_store_explicit(&s_marker_shown, NULL, memory_order_release);
    s_marker[0] = '\0';
    atomic_store_explicit(&s_marker_lost, 0, memory_order_release);
}

/* Where the game's memory is, for the reader (nk_cheats.h, NkCheatConfig). A code names a PSP
 * address. Three layouts reach a run:
 *  - the host loader put the image inside PSP user memory (0x08800000 up): the package route
 *    places a relocatable main module at 0x08804000 and a fixed-address one at its own address,
 *    as a PSP does, so memory is where a PSP has it and an address is used as written;
 *  - the host loader put the image below PSP user memory (a title manifest with "base": 0, the
 *    module's link addresses used as guest addresses): the image byte a PSP has at 0x08804000 +
 *    x is at base + x, the heap starts after the image and not where a PSP's is, and the
 *    translated code's spans are registered at base + x. An address inside the PSP's image is
 *    moved with it, and a code that names any other address refuses its cheat;
 *  - no host-loaded image (a program LoadExec started is a runtime-placed module): where a PSP
 *    address lives is not known, and every cheat is refused.
 * Returns 0 with the refusal said. */
static int cheats_layout(NkCheatConfig *config) {
    const uint32_t base = sr_loaded_base(), end = sr_loaded_end();
    if (base == UINT32_MAX || base >= end || end > CHEATS_USER_END) {
        fprintf(stderr, "CHEATS: refused: this run has no game image placed by the host loader, so where "
                        "a cheat's PSP address lives in it is not known; nothing is applied\n");
        return 0;
    }
    s_image_base = base;
    s_image_end = end;
    config->user_end = CHEATS_USER_END;
    if (base >= NK_CHEATS_ADDRESS_BASE) {
        config->user_base = NK_CHEATS_ADDRESS_BASE;
        return 1;
    }
    config->user_base = base;
    config->hole_base = SR_VRAM_CPU_BASE;
    config->hole_end = SR_VRAM_CPU_END;
    config->psp_image_base = CHEATS_PSP_IMAGE_BASE;
    config->image_base = base;
    config->image_size = end - base;
    fprintf(stderr, "CHEATS: this game's image is at 0x%08lX here and at 0x%08lX on a PSP: a cheat's address "
                    "inside the image (0x%08lX to 0x%08lX) is moved with it, and a cheat that names any "
                    "other address is refused\n",
            (unsigned long)base, (unsigned long)CHEATS_PSP_IMAGE_BASE, (unsigned long)CHEATS_PSP_IMAGE_BASE,
            (unsigned long)(CHEATS_PSP_IMAGE_BASE + (end - base - 1u)));
    return 1;
}

/* The file's own name: the log says which file was read, never the folders above it, which
 * name the user. */
static const char *cheats_file_name(const char *path) {
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') name = p + 1;
    }
    return name[0] ? name : path;
}

/* Says what the file holds: the counts, each line that belongs to no cheat, and each refused
 * cheat with its line and reason. */
static void cheats_announce(const char *path) {
    NkCheatSummary sum;
    nk_cheats_summary(s_set, &sum);
    s_status.on = sum.on;
    s_status.refused = sum.refused;
    s_status.off = sum.off;
    fprintf(stderr, "CHEATS: %s: %lu on, %lu refused, %lu off, %lu for other discs\n", cheats_file_name(path),
            (unsigned long)sum.on, (unsigned long)sum.refused, (unsigned long)sum.off,
            (unsigned long)sum.other_game);
    for (size_t i = 0u; i < nk_cheats_note_count(s_set); i++) {
        fprintf(stderr, "CHEATS: not part of a cheat: %s\n", nk_cheats_note(s_set, i));
    }
    if (sum.notes > nk_cheats_note_count(s_set)) {
        fprintf(stderr, "CHEATS: and %lu more lines that are not part of a cheat\n",
                (unsigned long)(sum.notes - nk_cheats_note_count(s_set)));
    }
    if (sum.not_kept > 0u) {
        fprintf(stderr, "CHEATS: %lu more cheat(s) for this disc are past the %lu the reader keeps and are "
                        "not read\n", (unsigned long)sum.not_kept, (unsigned long)NK_CHEATS_MAX_CHEATS);
    }
    /* Each refusal is named; a file of thousands is not allowed thousands of lines. The frame
     * limit is one cause however many cheats it refuses, and is said once. */
    {
        uint32_t named = 0u, unnamed = 0u;
        for (size_t i = 0u; i < nk_cheats_count(s_set); i++) {
            NkCheatInfo info;
            if (!nk_cheats_info(s_set, i, &info) || info.state != NK_CHEAT_REFUSED || info.over_frame_limit) continue;
            if (named == CHEATS_MAX_REFUSAL_LINES) {
                unnamed++;
                continue;
            }
            named++;
            fprintf(stderr, "CHEATS: refused \"%s\" (line %lu): line %lu %s; no line of this cheat is applied\n",
                    info.name, (unsigned long)info.file_line, (unsigned long)info.refused_line, info.reason);
        }
        if (unnamed > 0u) {
            fprintf(stderr, "CHEATS: and %lu more refused cheat(s), each for a line of its own\n",
                    (unsigned long)unnamed);
        }
    }
    if (sum.over_frame_limit > 0u) {
        fprintf(stderr, "CHEATS: refused %lu cheat(s), the first at line %lu: together the cheats that are on "
                        "may do %lu writes, %lu bytes and %lu codes in one VBLANK, and these did not fit; "
                        "turn other cheats off to use them\n",
                (unsigned long)sum.over_frame_limit, (unsigned long)sum.over_frame_limit_line,
                (unsigned long)NK_CHEATS_MAX_FRAME_WRITES, (unsigned long)NK_CHEATS_MAX_FRAME_BYTES,
                (unsigned long)NK_CHEATS_MAX_FRAME_CODES);
    }
}

void sr_cheats_init(void) {
    const char *on = getenv("SR_CHEATS");
    const char *record, *replay, *checkpoint;
    char path[4096];
    char error[128];
    NkCheatConfig config;
    int named;
    sr_cheats_reset();
    s_started = 1;
    if (!on || strcmp(on, "1") != 0) return;   /* off: nothing else is read */
    s_status.switched_on = 1;
    record = getenv("SR_RECORD");
    replay = getenv("SR_REPLAY");
    if ((record && record[0]) || (replay && replay[0])) {
        fprintf(stderr, "CHEATS: refused: this run records or replays (SR_RECORD, SR_REPLAY), and a "
                        "recording has to replay without a cheat file; nothing is applied\n");
        return;
    }
    /* The acceptance checkpoint is evidence of the game as it is. The launcher and the player
     * refuse the combination before they start a run; a runtime started by hand or by a tool
     * refuses it here, before a file is opened. */
    checkpoint = getenv("SR_HEADLESS_ACCEPT_PRESENTED_FRAMES");
    if (checkpoint && checkpoint[0]) {
        fprintf(stderr, "CHEATS: refused: this run is an acceptance checkpoint "
                        "(SR_HEADLESS_ACCEPT_PRESENTED_FRAMES), which needs the game without cheats; "
                        "nothing is applied\n");
        return;
    }
    named = nk_platform_env_utf8("SR_CHEATS_FILE", path, sizeof(path));
    if (named <= 0 || !path[0]) {
        fprintf(stderr, "CHEATS: on, but SR_CHEATS_FILE %s; nothing is applied\n",
                named < 0 ? "is too long or not valid text" : "names no cheat file");
        return;
    }
    memset(&config, 0, sizeof(config));
    config.disc_id = getenv("SR_DISC_ID");
    if (!cheats_layout(&config)) return;
    s_status.file_opened = 1;
    s_set = nk_cheats_load_file(path, &config, &k_host, error, sizeof(error));
    if (!s_set) {
        fprintf(stderr, "CHEATS: on, but %s (%s); nothing is applied\n", error, cheats_file_name(path));
        return;
    }
    cheats_announce(path);
    cheats_update();
}

void sr_cheats_vblank(void) {
    if (!s_started) sr_cheats_init();   /* a run with no window: gui_init did not read them */
    if (!s_status.applying) return;     /* off, refused or nothing on: the whole cost */
    if (atomic_load_explicit(&s_marker_lost, memory_order_acquire)) {
        sr_cheats_marker_unavailable("the in-game UI stopped drawing");
        return;
    }
    /* The file was read for one program's memory. LoadExec replaces the program and its memory
     * (driver.c), and the next one is a module the runtime places: its addresses are not the
     * file's, so the cheats stop for the rest of the run, by name. */
    if (sr_loaded_base() != s_image_base || sr_loaded_end() != s_image_end) {
        s_program_gone = 1;
        fprintf(stderr, "CHEATS: refused from here on: the game started another program, and the cheat file "
                        "was read for the first one's memory; nothing more is applied\n");
        cheats_update();
        return;
    }
    if (!s_origin_said) {
        /* The first use in this run of behaviour taken from an emulator and not measured on a
         * PSP is logged once (AGENTS.md section 7). */
        s_origin_said = 1;
        fprintf(stderr, "CHEATS: the code forms are read as PPSSPP reads them (Core/CwCheat.cpp at "
                        NK_CHEATS_PPSSPP_REVISION "): PPSSPP-derived, not PSP-measured\n");
    }
    (void)nk_cheats_apply(s_set, &k_host);
}

const char *sr_cheats_marker(void) {
    return atomic_load_explicit(&s_marker_shown, memory_order_acquire);
}

void sr_cheats_marker_unavailable(const char *why) {
    if (!s_set || s_no_marker) return;
    s_no_marker = 1;
    fprintf(stderr, "CHEATS: refused: the window cannot show the cheats marker (%s), and cheats are "
                    "not applied without it\n", why && why[0] ? why : "no reason given");
    cheats_update();
}

void sr_cheats_marker_drawn(int drawn) {
    if (!drawn) atomic_store_explicit(&s_marker_lost, 1, memory_order_release);
}

void sr_cheats_set_hardcore(int on) {
    on = on != 0;
    if (on == s_hardcore) return;
    s_hardcore = on;
    if (s_set) {
        fprintf(stderr, on ? "CHEATS: refused while RetroAchievements hardcore mode is on; nothing is applied\n"
                           : "CHEATS: hardcore mode is off; the cheats that are on apply again\n");
    }
    cheats_update();
}

void sr_cheats_status(SrCheatsStatus *out) {
    if (out) *out = s_status;
}
