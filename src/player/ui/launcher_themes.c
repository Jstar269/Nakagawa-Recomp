/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The launcher's colour themes (launcher_themes.h): the table, the rules that offer them,
   and the colour set derived from one accent and one page colour. */

#include "launcher_themes.h"

#include <math.h>
#include <string.h>

/* The wave theme: deep sea water under a sea-spray accent, the colour of the ripple that unlocks it. */
#define LAUNCHER_THEME_WAVE_ACCENT 0x4CC4E8FFu
#define LAUNCHER_THEME_WAVE_GROUND 0x061A22FFu

static const LauncherTheme g_launcher_themes[] = {
    {LAUNCHER_THEME_STANDARD_ID, NK_UI_STR(THEME_STANDARD), nk_ui_theme_n1, 0u, 0u, LAUNCHER_THEME_EFFECT_NONE,
     LAUNCHER_THEME_ALWAYS, NULL, 0, 0},
    {LAUNCHER_THEME_WAVE_ID, LAUNCHER_THEME_WAVE_NAME, NULL, LAUNCHER_THEME_WAVE_ACCENT,
     LAUNCHER_THEME_WAVE_GROUND, LAUNCHER_THEME_EFFECT_NONE, LAUNCHER_THEME_UNLOCKED,
     LAUNCHER_THEME_UNLOCK_WAVE, 0, 0}
};

#define LAUNCHER_THEME_TABLE_COUNT (sizeof(g_launcher_themes) / sizeof(g_launcher_themes[0]))

size_t launcher_theme_count(void) {
    return LAUNCHER_THEME_TABLE_COUNT;
}

const LauncherTheme *launcher_theme_at(size_t index) {
    return index < LAUNCHER_THEME_TABLE_COUNT ? &g_launcher_themes[index] : NULL;
}

const LauncherTheme *launcher_theme_standard(void) {
    return &g_launcher_themes[0];
}

const LauncherTheme *launcher_theme_find(const char *id) {
    if (id == NULL || id[0] == '\0') return NULL;
    for (size_t i = 0u; i < LAUNCHER_THEME_TABLE_COUNT; ++i) {
        if (strcmp(g_launcher_themes[i].id, id) == 0) return &g_launcher_themes[i];
    }
    return NULL;
}

bool launcher_theme_has_unlock(const LauncherThemeContext *context, const char *unlock) {
    if (context == NULL || context->unlocks == NULL || unlock == NULL || unlock[0] == '\0') {
        return false;
    }
    for (size_t i = 0u; i < context->unlock_count; ++i) {
        if (strncmp(context->unlocks[i], unlock, LAUNCHER_THEME_ID_MAX) == 0) return true;
    }
    return false;
}

static bool launcher_theme_today_within(const LauncherTheme *theme,
                                        const LauncherThemeContext *context) {
    int today;
    if (context == NULL || context->month < 1 || context->month > 12 || context->day < 1 ||
        context->day > 31) {
        return false;
    }
    today = context->month * 100 + context->day;
    if (theme->from_mmdd <= theme->to_mmdd) {
        return today >= theme->from_mmdd && today <= theme->to_mmdd;
    }
    return today >= theme->from_mmdd || today <= theme->to_mmdd;
}

bool launcher_theme_available(const LauncherTheme *theme, const LauncherThemeContext *context) {
    if (theme == NULL) return false;
    switch (theme->rule) {
        case LAUNCHER_THEME_ALWAYS: return true;
        case LAUNCHER_THEME_UNLOCKED: return launcher_theme_has_unlock(context, theme->unlock);
        case LAUNCHER_THEME_DATES: return launcher_theme_today_within(theme, context);
        default: return false;
    }
}

size_t launcher_theme_available_count(const LauncherThemeContext *context) {
    size_t count = 0u;
    for (size_t i = 0u; i < LAUNCHER_THEME_TABLE_COUNT; ++i) {
        if (launcher_theme_available(&g_launcher_themes[i], context)) ++count;
    }
    return count;
}

const LauncherTheme *launcher_theme_pick(const char *chosen, const LauncherThemeContext *context) {
    const LauncherTheme *theme = launcher_theme_find(chosen);
    if (theme != NULL) {
        return launcher_theme_available(theme, context) ? theme : launcher_theme_standard();
    }
    for (size_t i = 0u; i < LAUNCHER_THEME_TABLE_COUNT; ++i) {
        if (g_launcher_themes[i].rule == LAUNCHER_THEME_DATES &&
            launcher_theme_available(&g_launcher_themes[i], context)) {
            return &g_launcher_themes[i];
        }
    }
    return launcher_theme_standard();
}

const LauncherTheme *launcher_theme_next(const char *chosen, const LauncherThemeContext *context) {
    const LauncherTheme *current = launcher_theme_pick(chosen, context);
    size_t at = (size_t)(current - g_launcher_themes);
    for (size_t step = 1u; step <= LAUNCHER_THEME_TABLE_COUNT; ++step) {
        const LauncherTheme *candidate = &g_launcher_themes[(at + step) % LAUNCHER_THEME_TABLE_COUNT];
        if (launcher_theme_available(candidate, context)) return candidate;
    }
    return current;
}

/* ---- Colour derivation ------------------------------------------------------------------- */

#define RGBA_WHITE 0xFFFFFFFFu
#define RGBA_BLACK 0x000000FFu

static uint32_t rgba_channel(uint32_t rgba, unsigned shift) {
    return (rgba >> shift) & 0xFFu;
}

/* a toward b by t (0-1) in sRGB bytes, opaque. */
static uint32_t rgba_mix(uint32_t a, uint32_t b, double t) {
    uint32_t out = 0xFFu;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    for (unsigned shift = 8u; shift <= 24u; shift += 8u) {
        double value = (double)rgba_channel(a, shift) * (1.0 - t) +
                       (double)rgba_channel(b, shift) * t;
        out |= (uint32_t)lround(value) << shift;
    }
    return out;
}

static double srgb_linear(uint32_t channel) {
    double c = (double)channel / 255.0;
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double rgba_luminance(uint32_t rgba) {
    return 0.2126 * srgb_linear(rgba_channel(rgba, 24u)) +
           0.7152 * srgb_linear(rgba_channel(rgba, 16u)) +
           0.0722 * srgb_linear(rgba_channel(rgba, 8u));
}

double launcher_theme_contrast(uint32_t a_rgba, uint32_t b_rgba) {
    double a = rgba_luminance(a_rgba);
    double b = rgba_luminance(b_rgba);
    return a > b ? (a + 0.05) / (b + 0.05) : (b + 0.05) / (a + 0.05);
}

/* fg, moved toward white on a dark background (black on a light one) by the least step of 64
   that reaches `ratio` against bg. */
static uint32_t rgba_with_contrast(uint32_t fg, uint32_t bg, double ratio) {
    uint32_t toward = rgba_luminance(bg) < 0.18 ? RGBA_WHITE : RGBA_BLACK;
    for (unsigned step = 0u; step <= 64u; ++step) {
        uint32_t candidate = rgba_mix(fg, toward, (double)step / 64.0);
        if (launcher_theme_contrast(candidate, bg) >= ratio) return candidate;
    }
    return toward;
}

static uint32_t rgba_with_alpha(uint32_t rgba, uint32_t alpha) {
    return (rgba & 0xFFFFFF00u) | (alpha & 0xFFu);
}

void launcher_theme_derive(uint32_t accent_rgba, uint32_t ground_rgba, NkUiTheme *out) {
    const NkUiTheme *n1 = nk_ui_theme_n1();
    uint32_t ground = rgba_with_alpha(ground_rgba, 0xFFu);
    uint32_t accent = rgba_with_alpha(accent_rgba, 0xFFu);
    /* The surfaces step toward a quiet, greyed copy of the accent, as N1's step toward its
       blue-grey. */
    uint32_t tone = rgba_mix(accent, 0xC8D2DCFFu, 0.55);
    uint32_t paper;
    if (out == NULL) return;
    out->surface = ground;
    out->surface_raised = rgba_mix(ground, tone, 0.05);
    out->surface_high = rgba_mix(ground, tone, 0.09);
    out->line = rgba_mix(ground, tone, 0.11);
    out->line_strong = rgba_mix(ground, tone, 0.19);
    out->pressed = rgba_mix(ground, tone, 0.14);
    out->placeholder = rgba_mix(ground, tone, 0.24);
    /* Text is checked on the lightest surface it is drawn on, a focused row. */
    paper = rgba_with_contrast(rgba_mix(0xF5F7FAFFu, accent, 0.04), out->surface_high, 7.0);
    out->paper = paper;
    out->muted = rgba_with_contrast(rgba_mix(ground, paper, 0.60), out->surface_high, 4.5);
    out->dim = rgba_with_contrast(rgba_mix(ground, paper, 0.38), out->surface_high, 3.0);
    out->art_label = rgba_with_contrast(rgba_mix(paper, accent, 0.12), ground, 7.0);
    out->selected_neutral_edge =
        rgba_with_contrast(rgba_mix(ground, paper, 0.36), out->surface, 4.2);
    out->selected_neutral_edge_dimmed =
        rgba_with_contrast(rgba_mix(ground, paper, 0.30), out->surface, 3.1);
    /* The accent: a focus ring and primary fill that read on every card (3:1), with the ink
       on it at 4.5:1. A mid-tone accent is lightened until the ink reads on it. */
    out->accent_dark = rgba_with_contrast(accent, out->surface_high, 3.0);
    out->ink = rgba_mix(ground, RGBA_BLACK, 0.15);
    for (unsigned step = 0u; step < 64u &&
         launcher_theme_contrast(out->ink, out->accent_dark) < 4.5; ++step) {
        out->accent_dark = rgba_mix(out->accent_dark, RGBA_WHITE, 0.05);
    }
    out->focus = out->accent_dark;
    out->accent_hover = rgba_mix(out->accent_dark, RGBA_WHITE, 0.22);
    out->accent_pressed = rgba_mix(out->accent_dark, ground, 0.20);
    out->accent_light = rgba_mix(out->accent_dark, ground, 0.45);
    out->capture = rgba_mix(ground, out->accent_dark, 0.20);
    out->ready = rgba_with_contrast(n1->ready, out->surface_high, 4.5);
    out->warning = rgba_with_contrast(n1->warning, out->surface_high, 4.5);
    out->info = rgba_with_contrast(n1->info, out->surface_high, 4.5);
    out->error = rgba_with_contrast(n1->error, out->surface_high, 4.5);
    out->pressed_danger = rgba_mix(ground, out->error, 0.10);
    out->scrim = rgba_with_alpha(ground, rgba_channel(n1->scrim, 0u));
    out->art_scrim = rgba_with_alpha(ground, rgba_channel(n1->art_scrim, 0u));
}

void launcher_theme_colors(const LauncherTheme *theme, NkUiTheme *out) {
    if (out == NULL) return;
    if (theme == NULL) theme = launcher_theme_standard();
    if (theme->colors != NULL) {
        *out = *theme->colors();
    } else {
        launcher_theme_derive(theme->accent_rgba, theme->ground_rgba, out);
    }
}
