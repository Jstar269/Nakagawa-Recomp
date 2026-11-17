/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef SR_CHEATS_H
#define SR_CHEATS_H

/* Cheats in a run (#2639): the runtime's side of src/core/nk_cheats.h, which reads and checks
 * the user's CWCheat file and says which code forms run. Off unless SR_CHEATS is exactly 1.
 *
 * The switch and the file. SR_CHEATS=1 turns cheats on for the run and SR_CHEATS_FILE names
 * the file; the launcher exports both for every game it starts (src/core/nk_launch.c), so an
 * inherited value never turns them on. With the switch off nothing else is read: no file is
 * opened, and a VBLANK costs one branch. A run that records or replays (SR_RECORD, SR_REPLAY)
 * refuses cheats by name, since a recording has to replay without a file beside it.
 *
 * Where they apply. sr_vblank_tick (hle_display.c) calls sr_cheats_vblank once per delivered
 * VBLANK, on the scheduler's thread, after that VBLANK's handlers have returned and its pad
 * sample is latched: no guest thread is running, so guest memory is written between two guest
 * instructions, and a button condition reads the sample the guest reads next. Every address is
 * a whole span of the guest's main memory from 0x08800000 up, checked before a byte is written,
 * and a write that would land in code the recompiler translated (sr_exec_span_overlaps) is
 * refused by name: a recompiled game does not read its code again, so the write could not take
 * effect. Refusals are stderr lines that start "CHEATS:".
 *
 * Where the meaning comes from. The code forms are read as PPSSPP reads them
 * (src/core/nk_cheats.h): PPSSPP-derived, not PSP-measured. The first VBLANK of a run that
 * applies cheats says so on one "CHEATS:" line, with the PPSSPP revision; a run that applies
 * none never says it, and sr_cheats_init starts a new run.
 *
 * The marker. While any cheat is on, or one was refused, the window shows a short line
 * ("Cheats on: 2 (1 refused)") through the in-game UI pass (gui.c, gpu_sdl3vk/ace_ui.h). It is
 * on the window only, like everything that pass draws. In a windowed run cheats are applied
 * only while it can be drawn: a window that cannot draw it refuses them by name
 * (sr_cheats_marker_unavailable). A run without a window has stderr alone.
 *
 * RetroAchievements (#1316) has no hardcore mode on this runtime yet. sr_cheats_set_hardcore
 * is the seam for it: while it is set nothing is applied and no marker shows. */

#include <stdint.h>

/* The marker's longest text, with its terminator. */
#define SR_CHEATS_MARKER_MAX 64

typedef struct {
    int switched_on;     /* SR_CHEATS was 1 when the run started */
    int file_opened;     /* the named cheat file was asked for (never while the switch is off) */
    int applying;        /* the cheats that are on are applied each VBLANK */
    uint32_t on;         /* cheats applied each VBLANK */
    uint32_t refused;    /* cheats turned on in the file and refused at load */
    uint32_t off;        /* cheats in the file that are not turned on */
} SrCheatsStatus;

/* Reads the switch and, when it is on, the file. gui_init calls it before it makes the window,
 * and a run with no window reads them at its first VBLANK; both are after the generated code
 * has registered its executable spans, and before any cheat applies. A second call starts over
 * from the environment as it is then. */
void sr_cheats_init(void);
/* Forgets the file; cheats are off until the next sr_cheats_init. */
void sr_cheats_reset(void);
/* Applies the cheats that are on. Called by sr_vblank_tick only. */
void sr_cheats_vblank(void);
/* The marker's words, or NULL while there is nothing to show. */
const char *sr_cheats_marker(void);
/* The window cannot draw the marker: cheats are refused for the run, with the reason. */
void sr_cheats_marker_unavailable(const char *why);
/* The RetroAchievements hardcore seam: nonzero refuses cheats until it is cleared. */
void sr_cheats_set_hardcore(int on);
void sr_cheats_status(SrCheatsStatus *out);

#endif /* SR_CHEATS_H */
