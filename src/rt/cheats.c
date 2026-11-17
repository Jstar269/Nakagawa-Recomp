/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Cheats in a run (#2639); cheats.h says when they apply and what is refused. This file binds
 * the reader and applier of src/core/nk_cheats.c to the run: the switch, the file, guest
 * memory, the translated code and the pad. */

#include "cheats.h"

#include "recomp.h"
#include "../core/nk_cheats.h"
#include "../core/nk_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The guest's main memory a cheat may touch: from the address CWCheat offsets count from to the
 * end of the arena's RAM (recomp.c). Kernel memory below it, the scratchpad and VRAM are not
 * cheat memory. */
#define CHEATS_USER_END 0x0C000000u

static NkCheatSet *s_set;
static int s_started;      /* the switch was read for this run */
static SrCheatsStatus s_status;
static int s_hardcore;
static int s_no_marker;
static char s_marker[SR_CHEATS_MARKER_MAX];

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
    s_status.applying = s_set && !s_hardcore && !s_no_marker && s_status.on > 0u;
    s_marker[0] = '\0';
    if (!s_set || s_hardcore || s_no_marker || (s_status.on == 0u && s_status.refused == 0u)) return;
    if (s_status.refused > 0u) {
        snprintf(s_marker, sizeof(s_marker), "Cheats on: %lu (%lu refused)", (unsigned long)s_status.on,
                 (unsigned long)s_status.refused);
    } else {
        snprintf(s_marker, sizeof(s_marker), "Cheats on: %lu", (unsigned long)s_status.on);
    }
}

void sr_cheats_reset(void) {
    nk_cheats_free(s_set);
    s_set = NULL;
    memset(&s_status, 0, sizeof(s_status));
    s_hardcore = 0;
    s_no_marker = 0;
    s_marker[0] = '\0';
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
    for (size_t i = 0u; i < nk_cheats_count(s_set); i++) {
        NkCheatInfo info;
        if (!nk_cheats_info(s_set, i, &info) || info.state != NK_CHEAT_REFUSED) continue;
        fprintf(stderr, "CHEATS: refused \"%s\" (line %lu): line %lu %s; no line of this cheat is applied\n",
                info.name, (unsigned long)info.file_line, (unsigned long)info.refused_line, info.reason);
    }
}

void sr_cheats_init(void) {
    const char *on = getenv("SR_CHEATS");
    const char *record, *replay;
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
    named = nk_platform_env_utf8("SR_CHEATS_FILE", path, sizeof(path));
    if (named <= 0 || !path[0]) {
        fprintf(stderr, "CHEATS: on, but SR_CHEATS_FILE %s; nothing is applied\n",
                named < 0 ? "is too long or not valid text" : "names no cheat file");
        return;
    }
    config.disc_id = getenv("SR_DISC_ID");
    config.user_base = NK_CHEATS_ADDRESS_BASE;
    config.user_end = CHEATS_USER_END;
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
    (void)nk_cheats_apply(s_set, &k_host);
}

const char *sr_cheats_marker(void) {
    return s_marker[0] ? s_marker : NULL;
}

void sr_cheats_marker_unavailable(const char *why) {
    if (!s_set || s_no_marker) return;
    s_no_marker = 1;
    fprintf(stderr, "CHEATS: refused: the window cannot show the cheats marker (%s), and cheats are "
                    "not applied without it\n", why && why[0] ? why : "no reason given");
    cheats_update();
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
