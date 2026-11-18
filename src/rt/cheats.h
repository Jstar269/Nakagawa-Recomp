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
 * refuses cheats by name, since a recording has to replay without a file beside it. So does
 * an acceptance checkpoint (SR_HEADLESS_ACCEPT_PRESENTED_FRAMES), which is evidence of the game
 * as it is: the launcher and the player refuse that combination before a run starts, and the
 * runtime refuses it here for a run started by hand or by a tool.
 *
 * When they apply. sr_vblank_tick (hle_display.c) calls sr_cheats_vblank once per delivered
 * VBLANK, on the scheduler's thread, after that VBLANK's handlers have returned and its pad
 * sample is latched: no guest thread is running, so guest memory is written between two guest
 * instructions. A button condition reads that latch (sr_ctrl_live_buttons). It is the sample
 * the guest reads next, with one exception: in an Ace run with late pad sampling (pad=late,
 * ace.h) ctrl_refresh_newest (hle_input.c) may replace the newest sample after the VBLANK,
 * while no guest call has read it. The condition was then tested on the latch and the game
 * reads the newer state: one frame's difference, in such a run only.
 *
 * How often. Every delivered VBLANK, 59.94 times a second, which is this project's choice and
 * not PPSSPP's. PPSSPP runs a cheat file every iCwCheatRefreshIntervalMs of emulated time: 77
 * ms unless the user changes it (Core/Config.cpp:378 at the revision nk_cheats.h names), about
 * 13 times a second. A code that sets a value has the same effect at either rate. A code that
 * changes a value each time it runs (the 0x3 add and subtract forms, a 0x7 XOR) changes it
 * about 4.6 times as often here as in PPSSPP at that default, and no setting here changes
 * the rate.
 *
 * Where an address is. A code names a PSP address, and every address is a whole span checked
 * before a byte is written. Which memory it is depends on where the host loader put the game's
 * image (sr_loaded_base, sr_loaded_end):
 *  - inside PSP user memory, 0x08800000 up (every title the package route places: a relocatable
 *    main module at 0x08804000, a fixed-address one at its own address): memory is laid out as
 *    a PSP's and the address is used as written, anywhere from 0x08800000 to the end of RAM;
 *  - below PSP user memory (a title manifest with "base": 0): the image byte a PSP has at
 *    0x08804000 + x is at base + x here, and the heap and the stacks are not where a PSP's are.
 *    An address inside the PSP's image is moved with the image; a code that names any other
 *    address refuses its cheat by name, because where that memory is in such a run is not
 *    known. The start says which layout it found on one line;
 *  - no host-loaded image, or the game started another program (LoadExec): nothing is applied,
 *    by name, for the rest of the run.
 * A write that would land in code the recompiler translated (sr_exec_span_overlaps, asked at
 * the address the span has in this run) is refused by name: a recompiled game does not read
 * its code again, so the write could not take effect. Refusals are stderr lines that start
 * "CHEATS:"; the start names at most 32 refused cheats one by one and counts the rest.
 *
 * What a VBLANK may cost. The cheats run between two frames of the game, so their time is the
 * game's. nk_cheats.h bounds what all the cheats that are on do in one VBLANK together
 * (NK_CHEATS_MAX_FRAME_WRITES, NK_CHEATS_MAX_FRAME_BYTES, NK_CHEATS_MAX_FRAME_CODES); the
 * budget is handed out in file order when the file is read, and a cheat that does not fit is
 * refused by name. Measured through this file's callbacks (hle_thread_selftest's "cheats
 * bounds" test, an unoptimized build): a VBLANK that does 16,381 of its 16,384 writes took
 * 0.8 to 1.9 ms in four runs on the development machine, of a frame's 16.68 ms.
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
 * (sr_cheats_marker_unavailable), and so does a present whose UI pass did not draw it
 * (sr_cheats_marker_drawn): from the next VBLANK on nothing is applied for the rest of the
 * run. A run without a window has stderr alone.
 *
 * Holds. A cheat that cannot apply whole on a VBLANK is held for it (nk_cheats.h). Each cause
 * is said once per cheat, and "applies again" once; sr_cheats_run_end gives the counts.
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
/* The UI pass's answer for a present whose frame asked for the marker (gui.c): 0 when it was
 * not drawn. May be called from the thread that presents. After a 0 the next VBLANK refuses
 * the cheats for the rest of the run, with one line: none is applied unmarked. */
void sr_cheats_marker_drawn(int drawn);
/* The end of the run: for each cheat that was held on some VBLANK, how many it was held on and
 * how many it applied on (the hold lines themselves are said once per cause). Called at main's
 * end (driver.c) and at sceKernelExitGame, which ends the process without it; said once a run. */
void sr_cheats_run_end(void);
/* The RetroAchievements hardcore seam: nonzero refuses cheats until it is cleared. */
void sr_cheats_set_hardcore(int on);
void sr_cheats_status(SrCheatsStatus *out);

#endif /* SR_CHEATS_H */
