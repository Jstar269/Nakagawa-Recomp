/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The launcher's hidden moment and its themes (src/player/ui/launcher_secret.c,
   launcher_themes.c), SDL-free: the classic cheat code's detector, the greeting by the hour,
   the moment's frames with and without motion and sound, the pictures, the jingle, the theme
   rules and the derived colours' contrast, and the generated Love All table. The player's own
   path (keys, pad, settings.json, the routes) is in tests/native/test_player_ui.py. */

#include "launcher_secret.h"
#include "launcher_themes.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
            ++g_failures;                                                              \
        }                                                                              \
    } while (0)

static const LauncherSecretKey kCode[LAUNCHER_SECRET_STEPS] = {
    LAUNCHER_SECRET_KEY_UP, LAUNCHER_SECRET_KEY_UP, LAUNCHER_SECRET_KEY_DOWN,
    LAUNCHER_SECRET_KEY_DOWN, LAUNCHER_SECRET_KEY_LEFT, LAUNCHER_SECRET_KEY_RIGHT,
    LAUNCHER_SECRET_KEY_LEFT, LAUNCHER_SECRET_KEY_RIGHT, LAUNCHER_SECRET_KEY_B,
    LAUNCHER_SECRET_KEY_A};

/* Feeds keys 0.2 s apart from *now; returns the last verdict and counts the swallows. */
static LauncherSecretVerdict feed_keys(LauncherSecretDetector *det, const LauncherSecretKey *keys,
                                       size_t count, double *now, size_t *swallowed) {
    LauncherSecretVerdict verdict = LAUNCHER_SECRET_PASS;
    for (size_t i = 0u; i < count; ++i) {
        *now += 0.2;
        verdict = launcher_secret_feed(det, keys[i], *now);
        if (verdict == LAUNCHER_SECRET_SWALLOW && swallowed != NULL) ++*swallowed;
    }
    return verdict;
}

static void test_detector(void) {
    LauncherSecretDetector det;
    double now = 10.0;
    size_t swallowed = 0u;
    launcher_secret_reset(&det);

    /* The code, once: only the B before the A is swallowed, the A is FOUND. */
    for (size_t i = 0u; i < LAUNCHER_SECRET_STEPS; ++i) {
        LauncherSecretVerdict v;
        now += 0.2;
        v = launcher_secret_feed(&det, kCode[i], now);
        if (i < 8u) CHECK(v == LAUNCHER_SECRET_PASS);
        if (i == 8u) CHECK(v == LAUNCHER_SECRET_SWALLOW);
        if (i == 9u) CHECK(v == LAUNCHER_SECRET_FOUND);
        if (i < 9u) CHECK(launcher_secret_progress(&det) == i + 1u);
    }
    CHECK(launcher_secret_progress(&det) == 0u);

    /* A wrong press: the code is not found; Back and Select are not swallowed. */
    {
        const LauncherSecretKey wrong[] = {
            LAUNCHER_SECRET_KEY_UP, LAUNCHER_SECRET_KEY_UP, LAUNCHER_SECRET_KEY_DOWN,
            LAUNCHER_SECRET_KEY_DOWN, LAUNCHER_SECRET_KEY_LEFT, LAUNCHER_SECRET_KEY_LEFT,
            LAUNCHER_SECRET_KEY_LEFT, LAUNCHER_SECRET_KEY_RIGHT, LAUNCHER_SECRET_KEY_B,
            LAUNCHER_SECRET_KEY_A};
        swallowed = 0u;
        CHECK(feed_keys(&det, wrong, 10u, &now, &swallowed) == LAUNCHER_SECRET_PASS);
        CHECK(swallowed == 0u);
    }
    /* Another key in the middle starts it again. */
    {
        LauncherSecretKey other[LAUNCHER_SECRET_STEPS + 1u];
        memcpy(other, kCode, 5u * sizeof(other[0]));
        other[5] = LAUNCHER_SECRET_KEY_OTHER;
        memcpy(other + 6, kCode + 5, 5u * sizeof(other[0]));
        launcher_secret_reset(&det);
        CHECK(feed_keys(&det, other, 11u, &now, NULL) == LAUNCHER_SECRET_PASS);
    }
    /* Extra presses before it do not matter (Up, Up, Up, ... still matches). */
    {
        const LauncherSecretKey extra[] = {LAUNCHER_SECRET_KEY_UP, LAUNCHER_SECRET_KEY_DOWN,
                                           LAUNCHER_SECRET_KEY_UP};
        launcher_secret_reset(&det);
        (void)feed_keys(&det, extra, 3u, &now, NULL);
        CHECK(feed_keys(&det, kCode, 10u, &now, NULL) == LAUNCHER_SECRET_FOUND);
    }
    /* A pause longer than the gap starts it again; the rest of the code then is no code. */
    launcher_secret_reset(&det);
    (void)feed_keys(&det, kCode, 4u, &now, NULL);
    now += LAUNCHER_SECRET_GAP_S + 0.1;
    CHECK(feed_keys(&det, kCode + 4, 6u, &now, NULL) == LAUNCHER_SECRET_PASS);
    CHECK(launcher_secret_progress(&det) == 0u);
    /* A pause just inside the gap keeps it. */
    launcher_secret_reset(&det);
    (void)feed_keys(&det, kCode, 4u, &now, NULL);
    now += LAUNCHER_SECRET_GAP_S - 0.25;
    CHECK(feed_keys(&det, kCode + 4, 6u, &now, NULL) == LAUNCHER_SECRET_FOUND);
    /* No press (a release, a repeat) changes nothing, not even the clock. */
    launcher_secret_reset(&det);
    (void)feed_keys(&det, kCode, 9u, &now, NULL);
    CHECK(launcher_secret_feed(&det, LAUNCHER_SECRET_KEY_NONE, now + 0.1) == LAUNCHER_SECRET_PASS);
    CHECK(launcher_secret_progress(&det) == 9u);
    CHECK(launcher_secret_feed(&det, LAUNCHER_SECRET_KEY_A, now + 0.2) == LAUNCHER_SECRET_FOUND);
    CHECK(launcher_secret_feed(NULL, LAUNCHER_SECRET_KEY_A, now) == LAUNCHER_SECRET_PASS);
}

static void test_key_for_event(void) {
    NkUiEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.down = true;
    ev.type = NK_UI_EV_NAV;
    ev.dir = NK_UI_DIR_UP;
    CHECK(launcher_secret_key_for_event(&ev) == LAUNCHER_SECRET_KEY_UP);
    ev.dir = NK_UI_DIR_DOWN;
    CHECK(launcher_secret_key_for_event(&ev) == LAUNCHER_SECRET_KEY_DOWN);
    ev.dir = NK_UI_DIR_LEFT;
    CHECK(launcher_secret_key_for_event(&ev) == LAUNCHER_SECRET_KEY_LEFT);
    ev.dir = NK_UI_DIR_RIGHT;
    CHECK(launcher_secret_key_for_event(&ev) == LAUNCHER_SECRET_KEY_RIGHT);
    ev.type = NK_UI_EV_BACK;
    CHECK(launcher_secret_key_for_event(&ev) == LAUNCHER_SECRET_KEY_B);
    ev.type = NK_UI_EV_ACTIVATE;
    CHECK(launcher_secret_key_for_event(&ev) == LAUNCHER_SECRET_KEY_A);
    ev.type = NK_UI_EV_PAGE;
    CHECK(launcher_secret_key_for_event(&ev) == LAUNCHER_SECRET_KEY_OTHER);
    ev.type = NK_UI_EV_ACTIVATE;
    ev.repeat = true;
    CHECK(launcher_secret_key_for_event(&ev) == LAUNCHER_SECRET_KEY_NONE);
    ev.repeat = false;
    ev.down = false;
    CHECK(launcher_secret_key_for_event(&ev) == LAUNCHER_SECRET_KEY_NONE);
    ev.down = true;
    ev.type = NK_UI_EV_POINTER;
    CHECK(launcher_secret_key_for_event(&ev) == LAUNCHER_SECRET_KEY_NONE);
    ev.type = NK_UI_EV_WHEEL;
    CHECK(launcher_secret_key_for_event(&ev) == LAUNCHER_SECRET_KEY_NONE);
    CHECK(launcher_secret_key_for_event(NULL) == LAUNCHER_SECRET_KEY_NONE);
}

static void test_greeting(void) {
    struct { int h, m; const char *want; } cases[] = {
        {4, 59, "Good evening."},   {5, 0, "Good morning."},    {11, 59, "Good morning."},
        {12, 0, "Good afternoon."}, {17, 59, "Good afternoon."}, {18, 0, "Good evening."},
        {0, 0, "Good evening."},    {23, 59, "Good evening."},
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const char *got = launcher_secret_greeting(cases[i].h, cases[i].m);
        if (strcmp(got, cases[i].want) != 0) {
            fprintf(stderr, "FAIL greeting %02d:%02d: %s, want %s\n", cases[i].h, cases[i].m, got,
                    cases[i].want);
            ++g_failures;
        }
    }
    CHECK(strcmp(LAUNCHER_SECRET_FAREWELL, "Please come again soon!") == 0);
}

static void test_moment(void) {
    LauncherMoment m;
    LauncherMomentFrame f;
    bool opened = false;
    double prev = 100.0;

    /* With motion: ripple, confetti, the toast easing in, then Love All at the handover. */
    launcher_moment_start(&m, LAUNCHER_MOMENT_FOUND, 100.0, false, true, "Good morning.");
    CHECK(m.motion && m.sound);
    f = launcher_moment_frame(&m, 100.0, 100.5);
    CHECK(f.active && f.ripple && f.confetti && f.toast);
    CHECK(f.ripple_strength > 0.5f && f.toast_alpha > 0.9f);
    f = launcher_moment_frame(&m, 100.0, 100.2);
    CHECK(f.toast && f.toast_alpha < 0.5f && f.toast_rise > 0.0f);
    for (int i = 1; i <= 240; ++i) {
        double now = 100.0 + i / 60.0;
        f = launcher_moment_frame(&m, prev, now);
        if (f.open_love_all) {
            CHECK(!opened);
            CHECK(now >= 100.0 + LAUNCHER_MOMENT_RIPPLE_S);
            opened = true;
        }
        prev = now;
    }
    CHECK(opened);
    f = launcher_moment_frame(&m, 102.0, 102.0 + 0.01);
    CHECK(f.active && !f.ripple && f.confetti);
    f = launcher_moment_frame(&m, 104.0, 100.0 + LAUNCHER_MOMENT_TOAST_S + 0.01);
    CHECK(!f.active && !f.toast);

    /* Reduce motion: no ripple, no confetti, the toast alone at full strength, Love All at
       once. UI sounds off: no jingle. */
    launcher_moment_start(&m, LAUNCHER_MOMENT_FOUND, 200.0, true, false, "Good evening.");
    CHECK(!m.motion && !m.sound);
    f = launcher_moment_frame(&m, 199.99, 200.0);
    CHECK(f.active && !f.ripple && !f.confetti && f.toast && f.open_love_all);
    CHECK(f.toast_alpha == 1.0f && f.toast_rise == 0.0f);
    f = launcher_moment_frame(&m, 200.0, 201.0);
    CHECK(!f.ripple && !f.confetti && f.toast && !f.open_love_all && f.toast_alpha == 1.0f);

    /* The farewell: the toast alone, never a jingle, never Love All. */
    launcher_moment_start(&m, LAUNCHER_MOMENT_FAREWELL, 300.0, false, true, LAUNCHER_SECRET_FAREWELL);
    CHECK(!m.sound);
    f = launcher_moment_frame(&m, 300.0, 301.0);
    CHECK(f.active && f.toast && !f.ripple && !f.confetti && !f.open_love_all);
    f = launcher_moment_frame(&m, 301.0, 300.0 + LAUNCHER_MOMENT_FAREWELL_S + 0.01);
    CHECK(!f.active);
    f = launcher_moment_frame(NULL, 0.0, 1.0);
    CHECK(!f.active);
}

static bool shape_in(const NkUiShape *s, float x0, float y0, float x1, float y1) {
    if (s->kind == NK_UI_SHAPE_POLY) {
        for (unsigned i = 0u; i < s->count; ++i) {
            float x = s->v[2u * i], y = s->v[2u * i + 1u];
            if (x < x0 || x > x1 || y < y0 || y > y1) return false;
        }
    }
    return true;
}

static void test_pictures(void) {
    NkUiShape ripple[LAUNCHER_RIPPLE_SHAPES_MAX];
    NkUiShape confetti[LAUNCHER_CONFETTI_PIECES];
    const NkUiShapeArt *mark = nk_ui_art_n1_mark();
    const uint32_t colors[] = {0xFF0000FFu, 0x00FF00FFu};
    size_t n = launcher_ripple_shapes(0.6f, 1.0f, ripple, LAUNCHER_RIPPLE_SHAPES_MAX);
    CHECK(n > mark->count && n <= LAUNCHER_RIPPLE_SHAPES_MAX);
    for (size_t i = 0u; i < n; ++i) {
        CHECK(ripple[i].kind == NK_UI_SHAPE_POLY && ripple[i].count >= 3u &&
              ripple[i].count <= NK_UI_SHAPE_MAX_POINTS);
        CHECK(shape_in(&ripple[i], mark->view_x - 14.0f, mark->view_y - 14.0f,
                       mark->view_x + mark->view_w + 14.0f, mark->view_y + mark->view_h + 14.0f));
    }
    /* At rest the strips tile the mark unmoved: a strength of 0 is the mark itself. */
    CHECK(launcher_ripple_shapes(0.6f, 0.0f, ripple, LAUNCHER_RIPPLE_SHAPES_MAX) == n);
    CHECK(launcher_ripple_shapes(0.6f, 1.0f, ripple, 3u) <= 3u);

    n = launcher_confetti_shapes(1.0f, 1280.0f, 720.0f, colors, 2u, confetti,
                                 LAUNCHER_CONFETTI_PIECES);
    CHECK(n > 0u && n <= LAUNCHER_CONFETTI_PIECES);
    for (size_t i = 0u; i < n; ++i) {
        CHECK((confetti[i].rgba & 0xFFFFFF00u) == 0xFF000000u ||
              (confetti[i].rgba & 0xFFFFFF00u) == 0x00FF0000u);
    }
    {
        /* The same fall every run. */
        NkUiShape again[LAUNCHER_CONFETTI_PIECES];
        size_t m = launcher_confetti_shapes(1.0f, 1280.0f, 720.0f, colors, 2u, again,
                                            LAUNCHER_CONFETTI_PIECES);
        CHECK(m == n && memcmp(again, confetti, n * sizeof(again[0])) == 0);
    }
    CHECK(launcher_confetti_shapes(1.0f, 1280.0f, 720.0f, colors, 0u, confetti,
                                   LAUNCHER_CONFETTI_PIECES) == 0u);
}

static void test_jingle(void) {
    size_t total = launcher_jingle_pcm(NULL, 0u, LAUNCHER_JINGLE_RATE);
    int16_t *pcm;
    double energy = 0.0;
    int peak = 0;
    CHECK(total > (size_t)(LAUNCHER_JINGLE_RATE * 0.6) && total < (size_t)LAUNCHER_JINGLE_RATE * 2u);
    pcm = (int16_t *)calloc(total, sizeof(*pcm));
    CHECK(pcm != NULL);
    if (pcm == NULL) return;
    CHECK(launcher_jingle_pcm(pcm, total, LAUNCHER_JINGLE_RATE) == total);
    for (size_t i = 0u; i < total; ++i) {
        int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (v > peak) peak = v;
        energy += (double)pcm[i] * pcm[i];
    }
    CHECK(peak > 4000 && peak < 32767);
    CHECK(sqrt(energy / (double)total) > 800.0);
    /* It ends in silence: no click when the stream runs out. */
    CHECK(abs(pcm[total - 1u]) < 64);
    free(pcm);
}

static LauncherThemeContext context(const char (*unlocks)[LAUNCHER_THEME_ID_MAX], size_t count) {
    LauncherThemeContext ctx;
    ctx.unlocks = unlocks;
    ctx.unlock_count = count;
    ctx.month = 6;
    ctx.day = 15;
    return ctx;
}

static void test_theme_rules(void) {
    static const char unlocks[1][LAUNCHER_THEME_ID_MAX] = {LAUNCHER_THEME_UNLOCK_WAVE};
    LauncherThemeContext none = context(NULL, 0u);
    LauncherThemeContext found = context(unlocks, 1u);
    const LauncherTheme *wave = launcher_theme_find(LAUNCHER_THEME_WAVE_ID);
    const LauncherTheme *standard = launcher_theme_standard();
    CHECK(launcher_theme_count() >= 2u);
    CHECK(standard != NULL && strcmp(standard->id, LAUNCHER_THEME_STANDARD_ID) == 0);
    CHECK(wave != NULL && strcmp(wave->name, LAUNCHER_THEME_WAVE_NAME) == 0);
    CHECK(wave != NULL && wave->rule == LAUNCHER_THEME_UNLOCKED && wave->colors == NULL);
    if (wave == NULL || standard == NULL) return;

    /* Before the code: only Standard is offered, and a saved choice of the wave theme draws
       Standard. */
    CHECK(!launcher_theme_available(wave, &none));
    CHECK(launcher_theme_available_count(&none) == 1u);
    CHECK(launcher_theme_pick(LAUNCHER_THEME_WAVE_ID, &none) == standard);
    CHECK(launcher_theme_pick(NULL, &none) == standard);
    CHECK(launcher_theme_next(NULL, &none) == standard);
    /* After it: both are offered and cycle. */
    CHECK(launcher_theme_has_unlock(&found, LAUNCHER_THEME_UNLOCK_WAVE));
    CHECK(launcher_theme_available(wave, &found));
    CHECK(launcher_theme_available_count(&found) == 2u);
    CHECK(launcher_theme_pick(LAUNCHER_THEME_WAVE_ID, &found) == wave);
    CHECK(launcher_theme_pick("", &found) == standard);
    CHECK(launcher_theme_pick("no-such-theme", &found) == standard);
    CHECK(launcher_theme_next(LAUNCHER_THEME_STANDARD_ID, &found) == wave);
    CHECK(launcher_theme_next(LAUNCHER_THEME_WAVE_ID, &found) == standard);
    for (size_t i = 0u; i < launcher_theme_count(); ++i) {
        const LauncherTheme *t = launcher_theme_at(i);
        CHECK(t != NULL && strlen(t->id) < LAUNCHER_THEME_ID_MAX);
    }
    CHECK(launcher_theme_at(launcher_theme_count()) == NULL);
}

static void test_theme_date_rule(void) {
    /* A dated entry, checked by the rule alone (no seasonal theme ships). */
    LauncherTheme winter;
    LauncherThemeContext ctx = context(NULL, 0u);
    memset(&winter, 0, sizeof(winter));
    winter.id = "test-dates";
    winter.name = "Test";
    winter.rule = LAUNCHER_THEME_DATES;
    winter.from_mmdd = 1215;
    winter.to_mmdd = 105;
    ctx.month = 12; ctx.day = 14;
    CHECK(!launcher_theme_available(&winter, &ctx));
    ctx.day = 15;
    CHECK(launcher_theme_available(&winter, &ctx));
    ctx.month = 1; ctx.day = 5;
    CHECK(launcher_theme_available(&winter, &ctx));
    ctx.day = 6;
    CHECK(!launcher_theme_available(&winter, &ctx));
    ctx.month = 0; ctx.day = 0;
    CHECK(!launcher_theme_available(&winter, &ctx));
    winter.from_mmdd = 301;
    winter.to_mmdd = 331;
    ctx.month = 3; ctx.day = 31;
    CHECK(launcher_theme_available(&winter, &ctx));
    ctx.month = 4; ctx.day = 1;
    CHECK(!launcher_theme_available(&winter, &ctx));
}

static void check_contrast(const char *what, uint32_t fg, uint32_t bg, double want) {
    double got = launcher_theme_contrast(fg, bg);
    if (got + 1e-6 < want) {
        fprintf(stderr, "FAIL %s: %.2f:1 < %.2f:1 (%08X on %08X)\n", what, got, want, fg, bg);
        ++g_failures;
    }
}

static void check_derived(uint32_t accent, uint32_t ground) {
    NkUiTheme t;
    launcher_theme_derive(accent, ground, &t);
    check_contrast("paper on surface", t.paper, t.surface, 7.0);
    check_contrast("paper on surface_high", t.paper, t.surface_high, 7.0);
    check_contrast("muted on surface_high", t.muted, t.surface_high, 4.5);
    check_contrast("dim on surface_high", t.dim, t.surface_high, 3.0);
    check_contrast("accent on surface_high", t.accent_dark, t.surface_high, 3.0);
    check_contrast("ink on accent", t.ink, t.accent_dark, 4.5);
    check_contrast("ready on surface_high", t.ready, t.surface_high, 4.5);
    check_contrast("warning on surface_high", t.warning, t.surface_high, 4.5);
    check_contrast("info on surface_high", t.info, t.surface_high, 4.5);
    check_contrast("error on surface_high", t.error, t.surface_high, 4.5);
    check_contrast("art_label on surface", t.art_label, t.surface, 7.0);
    CHECK((t.surface & 0xFFu) == 0xFFu && (t.paper & 0xFFu) == 0xFFu);
}

static void test_theme_colors(void) {
    NkUiTheme out;
    double c = launcher_theme_contrast(0xFFFFFFFFu, 0x000000FFu);
    CHECK(c > 20.9 && c < 21.1);
    CHECK(fabs(launcher_theme_contrast(0x777777FFu, 0x777777FFu) - 1.0) < 1e-9);
    /* Standard is the N1 set itself. */
    launcher_theme_colors(launcher_theme_standard(), &out);
    CHECK(memcmp(&out, nk_ui_theme_n1(), sizeof(out)) == 0);
    /* The wave theme, and any accent a later Custom theme may give: every text colour keeps its
       contrast. */
    {
        const LauncherTheme *wave = launcher_theme_find(LAUNCHER_THEME_WAVE_ID);
        if (wave != NULL) check_derived(wave->accent_rgba, wave->ground_rgba);
    }
    {
        const uint32_t accents[] = {0xFF0000FFu, 0xFFFF00FFu, 0x00FF00FFu, 0x0000FFFFu,
                                    0x202020FFu, 0xFFFFFFFFu, 0x8A2BE2FFu, 0xFF8C00FFu};
        for (size_t i = 0u; i < sizeof(accents) / sizeof(accents[0]); ++i) {
            check_derived(accents[i], 0x0B0F14FFu);
            check_derived(accents[i], 0x20262EFFu);
        }
    }
}

static void test_credits_table(void) {
    bool heading = false, entry = false;
    size_t dedications = 0u;
    CHECK(g_love_all_credit_count >= 20u);
    CHECK(g_love_all_credits[0].kind == LOVE_ALL_HEADING);
    for (size_t i = 0u; i < g_love_all_credit_count; ++i) {
        const LoveAllCredit *c = &g_love_all_credits[i];
        CHECK(c->name != NULL && c->name[0] != '\0');
        if (c->kind == LOVE_ALL_HEADING) heading = true;
        if (c->kind == LOVE_ALL_ENTRY) entry = true;
        if (c->kind == LOVE_ALL_DEDICATION) ++dedications;
        if (c->kind != LOVE_ALL_ENTRY) CHECK(c->people == NULL);
    }
    CHECK(heading && entry && dedications == 1u);
    CHECK(g_love_all_credits[g_love_all_credit_count - 1u].kind == LOVE_ALL_DEDICATION);
}

int main(void) {
    test_detector();
    test_key_for_event();
    test_greeting();
    test_moment();
    test_pictures();
    test_jingle();
    test_theme_rules();
    test_theme_date_rule();
    test_theme_colors();
    test_credits_table();
    if (g_failures != 0) {
        fprintf(stderr, "test_launcher_secret: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_launcher_secret: all checks passed\n");
    return 0;
}
