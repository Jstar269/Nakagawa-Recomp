/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_PLAYER_LAUNCHER_SECRET_H
#define NK_PLAYER_LAUNCHER_SECRET_H

/* The launcher's hidden moment. Entering the classic cheat code (Up, Up, Down, Down, Left,
   Right, Left, Right, B, A) in the launcher, on a controller or with the arrow keys and the
   launcher's own Back and Select keys, makes the Nakagawa mark ripple like a wave (the
   "kawa" in Nakagawa is a river), plays a short jingle synthesised here, drops a little
   confetti and greets by the time of day; then opens the Love All credits. It also unlocks
   the wave launcher theme (LAUNCHER_THEME_WAVE_NAME, launcher_themes.h). Nothing here is drawn or heard in a game.

   Everything in this file is SDL-free: the host feeds it events and its clock, and draws
   what it returns. */

#include "nk_ui.h"
#include "nk_ui_widgets.h"
#include "player_ui_nk.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- The code ------------------------------------------------------------------------------ */

#define LAUNCHER_SECRET_STEPS 10u
/* The longest pause between two presses of the code; a longer one starts it again. */
#define LAUNCHER_SECRET_GAP_S 1.5

typedef enum {
    LAUNCHER_SECRET_KEY_NONE = 0, /* not a press (a release, a repeat, the pointer): ignored */
    LAUNCHER_SECRET_KEY_UP,
    LAUNCHER_SECRET_KEY_DOWN,
    LAUNCHER_SECRET_KEY_LEFT,
    LAUNCHER_SECRET_KEY_RIGHT,
    LAUNCHER_SECRET_KEY_B,        /* the launcher's Back (the pad's B, Esc or Backspace) */
    LAUNCHER_SECRET_KEY_A,        /* the launcher's Select (the pad's A, Enter) */
    LAUNCHER_SECRET_KEY_OTHER     /* any other press: the code starts again */
} LauncherSecretKey;

typedef struct {
    LauncherSecretKey keys[LAUNCHER_SECRET_STEPS];
    size_t count;      /* presses kept, the newest last */
    double last_s;     /* when the newest was pressed */
} LauncherSecretDetector;

typedef enum {
    LAUNCHER_SECRET_PASS = 0,  /* the press goes on to the launcher as usual */
    LAUNCHER_SECRET_SWALLOW,   /* the B that follows the eight directions: not a Back */
    LAUNCHER_SECRET_FOUND      /* the A that completes the code: not a Select */
} LauncherSecretVerdict;

/* The key a launcher event is to the code. */
LauncherSecretKey launcher_secret_key_for_event(const NkUiEvent *event);
/* The player's door to the code (player_ui_nk.c): main.c gives it every key and pad press
   before its own Back, Quit and Select handling, so the code's B and A are never also a Back,
   a Quit or a Select. True when the press was the code's alone. */
bool player_ui_nk_take_secret(PlayerUiNk *player, const union SDL_Event *event);
void launcher_secret_reset(LauncherSecretDetector *detector);
LauncherSecretVerdict launcher_secret_feed(LauncherSecretDetector *detector, LauncherSecretKey key,
                                           double now_s);
/* How many presses of the code the newest presses already are (0 to 9). */
size_t launcher_secret_progress(const LauncherSecretDetector *detector);

/* ---- The words ------------------------------------------------------------------------------ */

/* The greeting for a local time: "Good morning." 05:00-11:59, "Good afternoon." 12:00-17:59,
   "Good evening." 18:00-04:59. */
const char *launcher_secret_greeting(int hour, int minute);
/* The toast leaving Love All shows. */
#define LAUNCHER_SECRET_FAREWELL "Please come again soon!"

/* ---- The moment ----------------------------------------------------------------------------- */

#define LAUNCHER_MOMENT_RIPPLE_S 1.8    /* the mark ripples, then Love All opens */
#define LAUNCHER_MOMENT_CONFETTI_S 2.6
#define LAUNCHER_MOMENT_TOAST_S 3.4     /* the greeting shows this long */
#define LAUNCHER_MOMENT_FAREWELL_S 2.6
#define LAUNCHER_MOMENT_FADE_S 0.25     /* the toast eases in and out */

typedef enum {
    LAUNCHER_MOMENT_NONE = 0,
    LAUNCHER_MOMENT_FOUND,     /* the code: ripple, jingle, confetti and the greeting */
    LAUNCHER_MOMENT_FAREWELL   /* leaving Love All: the farewell toast alone */
} LauncherMomentKind;

typedef struct {
    LauncherMomentKind kind;
    double start_s;
    bool motion;          /* false under Reduce motion: the toast alone, as a cut */
    bool sound;           /* the jingle plays (the launcher's menu sounds are on) */
    const char *toast;    /* the greeting or the farewell */
} LauncherMoment;

/* What one frame of a moment draws. */
typedef struct {
    bool active;          /* the moment has not ended */
    bool ripple;          /* draw the rippling mark */
    float ripple_t;       /* seconds into the ripple */
    float ripple_strength; /* 0-1: the wave's height, easing in and out */
    bool confetti;
    float confetti_t;     /* seconds into the confetti */
    bool toast;
    float toast_alpha;    /* 0-1; always 1 without motion */
    float toast_rise;     /* logical px the toast still has to rise; 0 without motion */
    bool open_love_all;   /* the frame the code's moment hands over to Love All */
} LauncherMomentFrame;

void launcher_moment_start(LauncherMoment *moment, LauncherMomentKind kind, double now_s,
                           bool reduce_motion, bool ui_sounds, const char *toast);
LauncherMomentFrame launcher_moment_frame(const LauncherMoment *moment, double prev_s,
                                          double now_s);

/* ---- The pictures --------------------------------------------------------------------------- */

/* The rippled mark: the N1 mark cut in vertical strips, each shifted by a wave travelling
   across it, in the mark's own view box (nk_ui_art_n1_mark). Returns the shapes written. */
#define LAUNCHER_RIPPLE_STRIPS 12u
#define LAUNCHER_RIPPLE_SHAPES_MAX 96u
size_t launcher_ripple_shapes(float t_s, float strength, NkUiShape *out, size_t cap);

/* Confetti over a w x h box: LAUNCHER_CONFETTI_PIECES small pieces falling and turning, in
   `colors` (count of them), from a fixed seed so every run draws the same fall. */
#define LAUNCHER_CONFETTI_PIECES 56u
size_t launcher_confetti_shapes(float t_s, float w, float h, const uint32_t *colors,
                                size_t color_count, NkUiShape *out, size_t cap);

/* ---- The jingle ----------------------------------------------------------------------------- */

/* The jingle, synthesised: pulse-wave notes over a triangle bass, 16-bit mono at `rate` Hz.
   Writes at most cap samples and returns how many the whole jingle has (call with out NULL
   to size a buffer). */
#define LAUNCHER_JINGLE_RATE 44100
size_t launcher_jingle_pcm(int16_t *out, size_t cap, int rate);

/* ---- Love All ------------------------------------------------------------------------------- */

typedef enum {
    LOVE_ALL_HEADING = 0,   /* a section of CREDITS.md */
    LOVE_ALL_ENTRY,         /* a project or a font, and who made it */
    LOVE_ALL_DEDICATION     /* the dedication line */
} LoveAllKind;

typedef struct {
    LoveAllKind kind;
    const char *name;
    const char *people;     /* NULL when CREDITS.md names no one apart from the project */
} LoveAllCredit;

/* love_all_credits.c, generated from CREDITS.md by tools/gen_credits_table.py. */
extern const LoveAllCredit g_love_all_credits[];
extern const size_t g_love_all_credit_count;

/* ---- Drawing (scr_love_all.c) ---------------------------------------------------------------- */

/* Love All: "love all" is the score at nothing all, and a thank-you to everyone. The page
   scrolls the credits; the host rolls it slowly (scroll_set with the offset it wants) until
   the user scrolls it, and never under Reduce motion. */
typedef struct {
    float logical_width;
    float logical_height;
    bool scroll_set;      /* move the list to scroll_offset this frame */
    float scroll_offset;
} PlayerUiLoveAllModel;

/* Returns PLAYER_UI_ACTION_BACK when Back is chosen. */
PlayerUiAction player_ui_screen_love_all(NkUi *ui, const PlayerUiLoveAllModel *model);
/* The scroll container that holds the credits. */
NkUiId player_ui_love_all_list_id(void);

/* The shapes a moment's frame draws; they must outlive the frame, so the host keeps them. */
typedef struct {
    NkUiShape ripple[LAUNCHER_RIPPLE_SHAPES_MAX];
    NkUiShape confetti[LAUNCHER_CONFETTI_PIECES];
} LauncherMomentShapes;

/* The launcher theme in Settings (player_ui_nk.c): the name of the theme drawn, or NULL while
   only one theme is offered; and the step to the next one offered, saved. */
const char *player_ui_launcher_theme_name(const PlayerApp *app);
void player_ui_cycle_launcher_theme(PlayerApp *app);

/* Draws a moment's frame over the screen (call it after the screen, before nk_ui_end): the
   rippling mark on its tile, the confetti and the toast. They take no pointer and no focus. */
void launcher_moment_draw(NkUi *ui, LauncherMomentShapes *shapes, const LauncherMomentFrame *frame,
                          const char *toast, float width, float height);

#endif /* NK_PLAYER_LAUNCHER_SECRET_H */
