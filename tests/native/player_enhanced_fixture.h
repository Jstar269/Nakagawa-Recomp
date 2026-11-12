/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The Enhanced page's parts as a new settings file shows them: each part's name, its row word
 * for the fresh value and its sentence. The screen-only target (test_player_screens.c) does not
 * link the player model, so it lays the page out from this table; test_player_state.c links the
 * model and checks every row against the shipped part table, so a changed sentence fails there
 * instead of leaving the screen test drawing stale text. */
#ifndef NK_TEST_PLAYER_ENHANCED_FIXTURE_H
#define NK_TEST_PLAYER_ENHANCED_FIXTURE_H

#include "player_state.h"

typedef struct {
    const char *name;
    const char *value;
    const char *sentence;
} EnhancedPartFixture;

/* One row per part, in PLAYER_PART_* order; the size is pinned so a new part cannot be left out. */
static const EnhancedPartFixture k_enhanced_parts[] = {
    {"Texture filtering", "16x",
     "Keeps slanted surfaces sharp. Save pictures keep the filtering."},
    {"Crisp system text", "On",
     "Draws the on-screen keyboard at your screen's resolution."},
    {"Speed readout", "Off", "The game's speed and frame rate in a corner, over its HUD."},
    {"Game badge", "On", "Names the mode for a few seconds when a game starts."},
    {"Widescreen", "Off", "Shows more 3D space; HUD stays shaped. Some objects pop in. Smooth "
                            "motion disables it."},
    {"Smooth motion", "Off", "Smooths steady 30 FPS scenes only where it can follow the motion. "
                              "Adds about 17 ms of delay."},
    {"Clean pitched sound", "Off",
     "Resamples pitched effects without aliasing. The game receives changed samples."},
    {"Finer patches", "Off",
     "More triangles per curved patch. Slower; alters GE timing; the game receives changed pictures."},
    {"Wider culling", "Off", "Ace only, with Widescreen, in games that have a culling build. The game's "
                             "memory changes."},
    {"Sharper enlarging", "Off", "AMD FSR 1 sharpens display and recordings; game memory and "
                                  "screenshots do not change."},
    {"Stick dead zone", "Off", "Ace only, in games that ignore part of the stick. Small pushes count; "
                               "the feel changes."},
    {"Short loads", "Off", "Ace only, in games with a loading-wait package. Skips those waits; changes "
                           "the game's timing."},
    {"Full 32-bit colour", "Off", "Shows 16-bit games in full colour, without dither. Uses more memory."},
    {"Supersampling", "Off", "Smooths jagged edges. 2x takes 4 times the GPU work, 4x 16 times."},
};
typedef char player_enhanced_fixture_matches_parts[
    sizeof(k_enhanced_parts) / sizeof(k_enhanced_parts[0]) == PLAYER_PART_COUNT ? 1 : -1];

#endif
