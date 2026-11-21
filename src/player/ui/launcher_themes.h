/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_PLAYER_LAUNCHER_THEMES_H
#define NK_PLAYER_LAUNCHER_THEMES_H

/* The launcher's colour themes. A theme is one data entry: its colours (a hand-listed set,
   or a seed the colours are derived from), an optional accent effect, and the rule that says
   when it can be chosen (always, once an unlock is found, or between two dates). The
   launcher draws with the theme launcher_theme_pick gives; a running game never sees one. */

#include "nk_ui_theme.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The longest theme id settings.json keeps, NUL included. */
#define LAUNCHER_THEME_ID_MAX 24u
/* The most unlocks settings.json keeps. */
#define LAUNCHER_THEME_UNLOCK_MAX 8u

/* The id of the theme the launcher draws with when none is chosen. */
#define LAUNCHER_THEME_STANDARD_ID "standard"
/* The theme the classic cheat code unlocks: its id (settings.json, which never changes) and
   the name Settings shows. The name is written only here, so renaming it is a one-line change. */
#define LAUNCHER_THEME_WAVE_ID "wave"
#define LAUNCHER_THEME_WAVE_NAME "Nami"
/* The unlock the classic cheat code records (settings.json "theme_unlocks"). It also shows
   the Love All button on About. */
#define LAUNCHER_THEME_UNLOCK_WAVE "wave"

typedef enum {
    LAUNCHER_THEME_ALWAYS = 0,  /* every launcher offers it */
    LAUNCHER_THEME_UNLOCKED,    /* offered once its unlock is recorded */
    LAUNCHER_THEME_DATES        /* offered between two dates of the local calendar */
} LauncherThemeRule;

/* What a theme adds beyond its colours. None today; a seasonal theme may add one. */
typedef enum {
    LAUNCHER_THEME_EFFECT_NONE = 0
} LauncherThemeEffect;

typedef struct {
    const char *id;           /* settings.json's name for it, at most LAUNCHER_THEME_ID_MAX - 1 */
    const char *name;         /* the name Settings shows */
    /* A hand-listed colour set, or NULL: the colours are derived from the seed below by
       launcher_theme_derive, which keeps every text colour at its contrast. */
    const NkUiTheme *(*colors)(void);
    uint32_t accent_rgba;     /* the seed's accent (0xRRGGBBAA) */
    uint32_t ground_rgba;     /* the seed's page colour */
    LauncherThemeEffect effect;
    LauncherThemeRule rule;
    const char *unlock;       /* LAUNCHER_THEME_UNLOCKED: the unlock that offers it */
    int from_mmdd;            /* LAUNCHER_THEME_DATES: first and last day, month * 100 + day, */
    int to_mmdd;              /* both offered; from > to runs over the new year */
} LauncherTheme;

/* What a rule is checked against: the unlocks recorded, and today's local date (month 1-12,
   day 1-31; 0 when unknown, which offers no dated theme). */
typedef struct {
    const char (*unlocks)[LAUNCHER_THEME_ID_MAX];
    size_t unlock_count;
    int month;
    int day;
} LauncherThemeContext;

size_t launcher_theme_count(void);
const LauncherTheme *launcher_theme_at(size_t index);
const LauncherTheme *launcher_theme_find(const char *id);
const LauncherTheme *launcher_theme_standard(void);
bool launcher_theme_has_unlock(const LauncherThemeContext *context, const char *unlock);
bool launcher_theme_available(const LauncherTheme *theme, const LauncherThemeContext *context);
/* How many themes the context offers: Settings shows its theme row only when more than one. */
size_t launcher_theme_available_count(const LauncherThemeContext *context);
/* The theme to draw with. The chosen id when the context offers it; otherwise, with no
   choice (NULL or ""), the first dated theme whose days include today; otherwise Standard. A
   chosen theme the context no longer offers draws Standard and keeps the choice. */
const LauncherTheme *launcher_theme_pick(const char *chosen, const LauncherThemeContext *context);
/* The offered theme after the one launcher_theme_pick gives, in table order, wrapping. */
const LauncherTheme *launcher_theme_next(const char *chosen, const LauncherThemeContext *context);

/* A theme's colours: its hand-listed set, or the set derived from its seed. */
void launcher_theme_colors(const LauncherTheme *theme, NkUiTheme *out);
/* A whole colour set from one accent and one page colour: the surfaces, lines and pressed
   states step from the page toward the accent, the text colours are moved until they keep
   their contrast on the surfaces (paper 7:1, secondary text 4.5:1, footnotes 3:1), the accent
   until it keeps 3:1 on the cards and the ink on it 4.5:1. The status colours (Ready, needs
   setup, experimental, failed) keep their meaning and are moved only for contrast. A user's
   own accent can be given to it unchecked. */
void launcher_theme_derive(uint32_t accent_rgba, uint32_t ground_rgba, NkUiTheme *out);
/* WCAG 2 contrast ratio of two opaque colours (1 to 21). */
double launcher_theme_contrast(uint32_t a_rgba, uint32_t b_rgba);

#endif /* NK_PLAYER_LAUNCHER_THEMES_H */
