/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * ace_ui_selftest.c - Ace Mode's in-game UI (ace_ui.c), with synthetic frames only.
 *
 *   layout  CPU only: the readout, the launch notice and the keyboard at the overlay's window
 *           sizes and two ultrawide windows: every element inside the window and its 16 px safe
 *           area, the two chips apart, the keyboard under them; an empty frame draws nothing.
 *           The render notice (#2151) at those sizes and at small and scaled windows: at the
 *           bottom left, inside the safe area, its words inside it, under the top chips, and
 *           out of the keyboard's way; alone in an authentic run's frame it is wanted.
 *           The launch notice with every part on (ace_every_part_line, so a part added to the
 *           key table is in it): at every window and display scale every word of every part
 *           is drawn, whole items on each line, inside the chip and the safe area, with no
 *           ellipsis; the keyboard or a list past the chip's lines ends in "and N more".
 *           The slowed-logic warning at every one of those windows and scales: under the
 *           readout, left-aligned with it and apart from it (it once sat under the readout's
 *           chip), its words whole, clear of the launch notice, the keyboard and the render
 *           notice; alone, in the top-left corner.
 *   gpu     on a headless Vulkan device: the pass loads an image cleared to one colour, draws
 *           the chips and the keyboard over it, and leaves every other pixel as it was. The
 *           image is written to <dir>/ace_ui_selftest.ppm for review. Then the render notice
 *           alone, as an authentic run draws it: its chip at the bottom left and every other
 *           pixel the frame's (<dir>/ace_ui_selftest_notice.ppm). Then the readout with the
 *           slowed-logic warning under it (<dir>/ace_ui_selftest_logic.ppm). Then the launch notice with
 *           every part on (<dir>/ace_ui_selftest_every_part.ppm). Exit 77 without Vulkan.
 *
 * Built twice. With ACE_UI_SELFTEST_S1 it is linked from ace_ui.c, osk_overlay.c, nk_ui and its
 * Vulkan backend alone: a reference from the UI pass to anything of the runtime fails that link
 * (invariant S1). The build that runs adds ace.c, which only this test's every-part cells read.
 * With --fonts <player folder> and SDL3_ttf the GPU frames and the every-part layout use the
 * launcher's fonts as well as block glyphs.
 */
#define VK_NO_PROTOTYPES 1
#include "ace_ui.h"
#if !defined(ACE_UI_SELFTEST_S1)
#include "../ace.h"
#endif

#include "nk_ui_draw.h"
#include "nk_ui_theme.h"
#if defined(NK_UI_WITH_SDL_TTF)
#include "backend/nk_ui_sdlr.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static int s_fail;

#define CHECK(cond, ...) do { \
        if (!(cond)) { s_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                       fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
    } while (0)

/* ---- a block-glyph text backend: 8 px advance, 16 px line ---------------------------------- */

static uint8_t s_glyph[16 * 8];

static NkUiTextSize text_measure(void *user, int font, int px, const char *s, size_t n) {
    NkUiTextSize size;
    (void)user; (void)font; (void)s;
    size.width = (float)n * (float)px * 0.55f;
    size.height = (float)px * 1.25f;
    size.baseline = (float)px;
    return size;
}

static int text_shape(void *user, int font, int px, const char *s, size_t n, NkUiGlyph *out, int max) {
    int i;
    (void)user; (void)font;
    for (i = 0; i < max && (size_t)i < n; ++i) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].glyph = (uint8_t)s[i];
        out[i].codepoint = (uint8_t)s[i];
        out[i].text_index = (uint32_t)i;
        out[i].x = (float)i * (float)px * 0.55f;
        out[i].advance_x = (float)px * 0.55f;
    }
    return i;
}

static bool text_raster(void *user, int font, int px, uint32_t glyph, NkUiGlyphBitmap *out) {
    (void)user; (void)font; (void)px;
    memset(out, 0, sizeof(*out));
    if (glyph == ' ') return false;
    memset(s_glyph, 0xFF, sizeof(s_glyph));
    out->pixels = s_glyph;
    out->width = 6;
    out->height = 10;
    out->pitch = 8;
    out->bearing_y = 10;
    out->advance_x = 7.0f;
    return true;
}

static const NkUiTextBackend k_blocks = {NULL, text_measure, text_shape, text_raster};

/* The same glyphs measured as a shaping font measures: a run shaped whole comes out wider than
 * its words measured one by one (here a twelfth of the type size for every eight bytes of a run).
 * A line the layout engine wrapped is sized from its words, so the backend's clip to the run's
 * box would cut its end; the render notice's lines must each be one run, measured whole. */
static NkUiTextSize text_measure_shaped(void *user, int font, int px, const char *s, size_t n) {
    NkUiTextSize size = text_measure(user, font, px, s, n);
    size.width += (float)(n / 8u) * (float)px / 12.0f;
    return size;
}

static const NkUiTextBackend k_shaped = {NULL, text_measure_shaped, text_shape, text_raster};

/* ---- frames ---------------------------------------------------------------------------------- */

static OskOverlay s_keyboard;

static void keyboard_open(void) {
    static const uint16_t desc[] = {'Y','o','u','r',' ','n','a','m','e',0};
    static const uint16_t initial[] = {'A','c','e',0};
    osk_overlay_reset(&s_keyboard);
    osk_overlay_open(&s_keyboard, desc, initial, 16, 0u);
}

/* The render notice as gui.c words it (render_notice_words, #2151): the longer of its two texts. */
static const char k_host_head[] = "Drawn in software";
static const char k_host_words[] =
    "The GPU renderer did not start on this PC, so the game is drawn in software and may run "
    "slower. Updating the graphics driver may help.";
static const char k_host_hint[] = "Esc or Guide opens the menu and hides this";

static AceUiFrame frame_full(int w, int h, float scale) {
    AceUiFrame f;
    memset(&f, 0, sizeof(f));
    f.pixel_w = w;
    f.pixel_h = h;
    f.display_scale = scale;
    f.now_s = 1.0;
    f.reduce_motion = 1;
    f.notice_head = "Enhanced";
    f.notice ="16x filtering \xC2\xB7 crisp keyboard \xC2\xB7 speed readout";
    f.notice_alpha = 1.0f;
    f.readout = "\xE2\x96\xB2 87%  26 FPS  PC LIMITED";
    f.readout_warning = 1;
    f.keyboard = &s_keyboard;
    return f;
}

/* The union of the bounds of every command an element drew, in device px; 0 when it drew none. */
static int element_bounds(const NkUiDrawList *list, NkUiId id, int out[4]) {
    int found = 0;
    for (uint32_t i = 0; i < list->ncmd; i++) {
        const NkUiDrawCmd *c = &list->cmd[i];
        if (c->element_id != id || c->kind == NK_UI_DRAW_SCISSOR_START || c->kind == NK_UI_DRAW_SCISSOR_END)
            continue;
        if (!found) { memcpy(out, c->bounds, sizeof(int) * 4u); found = 1; continue; }
        if (c->bounds[0] < out[0]) out[0] = c->bounds[0];
        if (c->bounds[1] < out[1]) out[1] = c->bounds[1];
        if (c->bounds[2] > out[2]) out[2] = c->bounds[2];
        if (c->bounds[3] > out[3]) out[3] = c->bounds[3];
    }
    return found;
}

static const NkUiDrawList *build(NkUi *ui, const AceUiFrame *f, float *density) {
    NkUiViewport vp;
    ace_ui_viewport(f, &vp);
    *density = nk_ui_scale_for_viewport(&vp).density;
    nk_ui_set_viewport(ui, &vp);
    nk_ui_begin(ui, f->now_s);
    ace_ui_layout(ui, f);
    return nk_ui_end(ui);
}

/* A chip's words as the list draws them, from the `count` line ids at `first`: the union of
 * their lines' bounds, the lines joined by single spaces (a no-break space read as a space), and
 * how many lines there are. Every line must be a run no wider, measured whole by `text`, than the
 * box it is clipped to, under the line before it. */
static int chip_words(const NkUiDrawList *list, NkUiId first, int count, const char *what,
                      const NkUiTextBackend *text, int bounds[4], char *joined, size_t cap, int w, int h) {
    int lines = 0;
    size_t used = 0;
    joined[0] = '\0';
    for (uint32_t i = 0; i < list->ncmd; i++) {
        const NkUiDrawCmd *c = &list->cmd[i];
        if (c->kind != NK_UI_DRAW_TEXT || c->element_id < first || c->element_id >= first + (NkUiId)count)
            continue;
        const NkUiTextRun *run = (const NkUiTextRun *)c->custom;
        const float whole = text->measure(text->user, run->font, run->size_px, run->text, run->length).width;
        CHECK(whole <= (float)(c->bounds[2] - c->bounds[0]) + 1.0f,
              "%dx%d: line %d of the %s is %.1f px shaped whole in a %d px box: its end "
              "would be clipped", w, h, lines, what, (double)whole, c->bounds[2] - c->bounds[0]);
        CHECK(!lines || c->bounds[1] >= bounds[3], "%dx%d: line %d of the %s overlaps the line above",
              w, h, lines, what);
        if (!lines) memcpy(bounds, c->bounds, sizeof(int) * 4u);
        if (c->bounds[0] < bounds[0]) bounds[0] = c->bounds[0];
        if (c->bounds[1] < bounds[1]) bounds[1] = c->bounds[1];
        if (c->bounds[2] > bounds[2]) bounds[2] = c->bounds[2];
        if (c->bounds[3] > bounds[3]) bounds[3] = c->bounds[3];
        if (lines && used + 1u < cap) joined[used++] = ' ';
        for (size_t k = 0; k < run->length && used + 1u < cap; k++) {
            if ((unsigned char)run->text[k] == 0xC2u && k + 1u < run->length &&
                (unsigned char)run->text[k + 1u] == 0xA0u) {
                joined[used++] = ' ';
                k++;
            } else {
                joined[used++] = run->text[k];
            }
        }
        joined[used] = '\0';
        lines++;
    }
    return lines;
}

static int host_words(const NkUiDrawList *list, int bounds[4], char *joined, size_t cap, int w, int h) {
    return chip_words(list, ACE_UI_ID_HOST_NOTICE_TEXT, ACE_UI_HOST_NOTICE_LINES, "render notice",
                      &k_shaped, bounds, joined, cap, w, h);
}

/* ---- layout ---------------------------------------------------------------------------------- */

static void cell_layout(void) {
    static const struct { int w, h; float scale; } sizes[] = {
        /* the overlay's three, a 1x and a 2x game window, 720p, and two ultrawide windows */
        {960, 600, 1.0f}, {1280, 800, 1.0f}, {1920, 1200, 1.5f}, {480, 272, 1.0f},
        {960, 544, 1.0f}, {1280, 720, 1.0f}, {2560, 1080, 1.0f}, {5120, 1440, 2.0f},
    };
    NkUiConfig config;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_OVERLAY;
    config.text = &k_blocks;
    NkUi *ui = nk_ui_create(&config);
    CHECK(ui != NULL, "nk_ui_create");
    if (!ui) return;
    keyboard_open();
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        AceUiFrame f = frame_full(sizes[s].w, sizes[s].h, sizes[s].scale);
        float density = 1.0f;
        const NkUiDrawList *list = build(ui, &f, &density);
        CHECK(list != NULL, "%dx%d: the frame was dropped", f.pixel_w, f.pixel_h);
        if (!list) continue;
        int readout[4] = { 0, 0, 0, 0 }, notice[4] = { 0, 0, 0, 0 }, keys[4] = { 0, 0, 0, 0 };
        int safe = (int)(16.0f * density) - 1;
        const int has_readout = element_bounds(list, ACE_UI_ID_READOUT, readout);
        const int has_notice = element_bounds(list, ACE_UI_ID_NOTICE, notice);
        const int has_keys = element_bounds(list, ACE_UI_ID_KEYBOARD, keys);
        CHECK(has_readout, "%dx%d: no readout", f.pixel_w, f.pixel_h);
        CHECK(has_notice, "%dx%d: no notice", f.pixel_w, f.pixel_h);
        CHECK(has_keys, "%dx%d: no keyboard", f.pixel_w, f.pixel_h);
        /* No element draws outside the window. */
        for (uint32_t i = 0; i < list->ncmd; i++) {
            const NkUiDrawCmd *c = &list->cmd[i];
            if (c->kind == NK_UI_DRAW_SCISSOR_START || c->kind == NK_UI_DRAW_SCISSOR_END) continue;
            CHECK(c->bounds[0] >= 0 && c->bounds[1] >= 0 && c->bounds[2] <= f.pixel_w &&
                  c->bounds[3] <= f.pixel_h, "%dx%d: command %u outside the window", f.pixel_w,
                  f.pixel_h, i);
        }
        /* The checks below compare the three rectangles: without one of them they have nothing to read. */
        if (!has_readout || !has_notice || !has_keys) continue;
        const int *rects[3] = { readout, notice, keys };
        for (int r = 0; r < 3; r++) {
            CHECK(rects[r][0] >= safe && rects[r][1] >= safe && rects[r][2] <= f.pixel_w - safe &&
                  rects[r][3] <= f.pixel_h - safe,
                  "%dx%d: element %d (%d,%d)-(%d,%d) leaves the safe area", f.pixel_w, f.pixel_h, r,
                  rects[r][0], rects[r][1], rects[r][2], rects[r][3]);
        }
        CHECK(readout[2] < notice[0], "%dx%d: the chips overlap", f.pixel_w, f.pixel_h);
        CHECK(readout[0] < f.pixel_w / 2 && notice[2] > f.pixel_w / 2,
              "%dx%d: readout top left, notice top right", f.pixel_w, f.pixel_h);
        CHECK(keys[1] >= readout[3] && keys[1] >= notice[3], "%dx%d: the keyboard covers a chip",
              f.pixel_w, f.pixel_h);
    }
    /* The render notice (#2151) at the sizes above and at small and scaled windows (a 1x game
     * window at 200% and 150% display scale, a window smaller than the game's, a tall one): at
     * the bottom left, inside the safe area, under the top chips, its words wrapped inside it;
     * the open keyboard takes its place. It is laid out twice: beside Ace Mode's chips, and
     * alone, as an authentic run's frame holds it. */
    static const struct { int w, h; float scale; } host_sizes[] = {
        {960, 600, 1.0f}, {1280, 800, 1.0f}, {1920, 1200, 1.5f}, {480, 272, 1.0f},
        {960, 544, 1.0f}, {1280, 720, 1.0f}, {2560, 1080, 1.0f}, {5120, 1440, 2.0f},
        {480, 272, 2.0f}, {720, 408, 1.5f}, {320, 180, 1.0f}, {640, 1136, 1.0f},
        {3840, 2160, 2.0f},
    };
    config.text = &k_shaped;
    NkUi *shaped = nk_ui_create(&config);
    CHECK(shaped != NULL, "nk_ui_create (shaped text)");
    for (size_t s = 0; shaped && s < sizeof(host_sizes) / sizeof(host_sizes[0]); s++) {
        for (int alone = 0; alone < 2; alone++) {
            AceUiFrame f = frame_full(host_sizes[s].w, host_sizes[s].h, host_sizes[s].scale);
            f.keyboard = NULL;
            if (alone) {
                f.readout = NULL;
                f.notice = NULL;
            }
            f.host_notice_head = k_host_head;
            f.host_notice = k_host_words;
            f.host_notice_hint = k_host_hint;
            float density = 1.0f;
            const NkUiDrawList *list = build(shaped, &f, &density);
            CHECK(list != NULL, "%dx%d host: the frame was dropped", f.pixel_w, f.pixel_h);
            if (!list) continue;
            int readout[4] = {0, 0, 0, 0}, notice[4] = {0, 0, 0, 0}, host[4] = {0, 0, 0, 0},
                head[4] = {0, 0, 0, 0}, text[4] = {0, 0, 0, 0}, hint[4] = {0, 0, 0, 0};
            char joined[512];
            int safe = (int)(16.0f * density) - 1;
            CHECK(element_bounds(list, ACE_UI_ID_HOST_NOTICE, host), "%dx%d: no render notice",
                  f.pixel_w, f.pixel_h);
            const int lines = host_words(list, text, joined, sizeof(joined), f.pixel_w, f.pixel_h);
            CHECK(element_bounds(list, ACE_UI_ID_HOST_NOTICE_HEAD, head) && lines >= 1 &&
                  element_bounds(list, ACE_UI_ID_HOST_NOTICE_HINT, hint),
                  "%dx%d: the render notice lacks its heading, words or hint", f.pixel_w, f.pixel_h);
            /* Every word is there, on two or three lines: nothing was cut or ended in an ellipsis. */
            CHECK(lines >= 2 && lines <= ACE_UI_HOST_NOTICE_LINES && strcmp(joined, k_host_words) == 0,
                  "%dx%d: the render notice's %d line(s) read \"%s\"", f.pixel_w, f.pixel_h, lines,
                  joined);
            CHECK(host[0] >= safe && host[1] >= safe && host[2] <= f.pixel_w - safe &&
                  host[3] <= f.pixel_h - safe, "%dx%d: the render notice (%d,%d)-(%d,%d) leaves the "
                  "safe area", f.pixel_w, f.pixel_h, host[0], host[1], host[2], host[3]);
            CHECK(host[0] < f.pixel_w / 2 && host[3] > f.pixel_h / 2 && host[3] >= f.pixel_h - safe - 4,
                  "%dx%d: the render notice is not at the bottom left", f.pixel_w, f.pixel_h);
            CHECK(head[0] >= host[0] && head[2] <= host[2] && head[1] >= host[1] &&
                  text[0] >= host[0] && text[2] <= host[2] && text[1] >= head[3] &&
                  hint[0] >= host[0] && hint[2] <= host[2] && hint[1] >= text[3] && hint[3] <= host[3],
                  "%dx%d: the heading, words and hint are not one under the other inside the render "
                  "notice", f.pixel_w, f.pixel_h);
            if (alone) {
                CHECK(!element_bounds(list, ACE_UI_ID_READOUT, readout) &&
                      !element_bounds(list, ACE_UI_ID_NOTICE, notice),
                      "%dx%d: an authentic frame drew an Ace chip", f.pixel_w, f.pixel_h);
            } else {
                CHECK(element_bounds(list, ACE_UI_ID_READOUT, readout) &&
                      element_bounds(list, ACE_UI_ID_NOTICE, notice), "%dx%d host: no top chips",
                      f.pixel_w, f.pixel_h);
                CHECK(host[1] >= readout[3] && host[1] >= notice[3],
                      "%dx%d: the render notice covers a chip", f.pixel_w, f.pixel_h);
            }
            for (uint32_t i = 0; i < list->ncmd; i++) {
                const NkUiDrawCmd *c = &list->cmd[i];
                if (c->kind == NK_UI_DRAW_SCISSOR_START || c->kind == NK_UI_DRAW_SCISSOR_END) continue;
                CHECK(c->bounds[0] >= 0 && c->bounds[1] >= 0 && c->bounds[2] <= f.pixel_w &&
                      c->bounds[3] <= f.pixel_h, "%dx%d host: command %u outside the window",
                      f.pixel_w, f.pixel_h, i);
            }
            f.keyboard = &s_keyboard;
            list = build(shaped, &f, &density);
            CHECK(list && !element_bounds(list, ACE_UI_ID_HOST_NOTICE, host) &&
                  element_bounds(list, ACE_UI_ID_KEYBOARD, host),
                  "%dx%d: the render notice did not step aside for the keyboard", f.pixel_w, f.pixel_h);
        }
    }
    /* Words too long for three lines end in an ellipsis on the third; nothing spills out. */
    if (shaped) {
        static const char k_long[] =
            "one two three four five six seven eight nine ten eleven twelve thirteen fourteen "
            "fifteen sixteen seventeen eighteen nineteen twenty twenty-one twenty-two twenty-three "
            "twenty-four twenty-five twenty-six twenty-seven twenty-eight twenty-nine thirty "
            "thirty-one thirty-two thirty-three thirty-four thirty-five thirty-six thirty-seven";
        AceUiFrame f = frame_full(960, 544, 1.0f);
        f.keyboard = NULL;
        f.host_notice_head = k_host_head;
        f.host_notice = k_long;
        f.host_notice_hint = k_host_hint;
        float density = 1.0f;
        int host[4] = {0, 0, 0, 0}, text[4] = {0, 0, 0, 0};
        char joined[512];
        const NkUiDrawList *list = build(shaped, &f, &density);
        CHECK(list && element_bounds(list, ACE_UI_ID_HOST_NOTICE, host), "long words: no render notice");
        if (list) {
            const int lines = host_words(list, text, joined, sizeof(joined), f.pixel_w, f.pixel_h);
            const size_t n = strlen(joined);
            CHECK(lines == ACE_UI_HOST_NOTICE_LINES && n > 3u && strcmp(joined + n - 3u, "\xE2\x80\xA6") == 0 &&
                  strncmp(joined, k_long, n - 3u) == 0,
                  "long words: %d line(s) read \"%s\"", lines, joined);
            CHECK(text[0] >= host[0] && text[2] <= host[2] && text[3] <= host[3],
                  "long words: the lines leave the render notice");
        }
        nk_ui_destroy(shaped);
    }
    /* Nothing to show: no frame is wanted, and a hidden notice draws nothing. */
    AceUiFrame empty;
    memset(&empty, 0, sizeof(empty));
    empty.pixel_w = 960;
    empty.pixel_h = 544;
    empty.display_scale = 1.0f;
    CHECK(!ace_ui_frame_wants(&empty), "an empty frame is wanted");
    empty.notice = "16x filtering";
    empty.notice_alpha = 0.0f;
    CHECK(!ace_ui_frame_wants(&empty), "a faded-out notice is wanted");
    empty.notice_alpha = 0.5f;
    CHECK(ace_ui_frame_wants(&empty), "a notice half faded in is not wanted");
    osk_overlay_reset(&s_keyboard);
    AceUiFrame closed = frame_full(960, 544, 1.0f);
    closed.readout = NULL;
    closed.notice = NULL;
    CHECK(!ace_ui_frame_wants(&closed), "a closed keyboard is wanted");
    /* An authentic run's frame holds the render notice alone: wanted then, and only then. */
    empty.notice = NULL;
    empty.host_notice_head = k_host_head;
    empty.host_notice_hint = k_host_hint;
    CHECK(!ace_ui_frame_wants(&empty), "a render notice with no words is wanted");
    empty.host_notice = "";
    CHECK(!ace_ui_frame_wants(&empty), "an empty render notice is wanted");
    empty.host_notice = k_host_words;
    CHECK(ace_ui_frame_wants(&empty), "the render notice alone is not wanted");
    nk_ui_destroy(ui);
}

/* ---- the slowed-logic warning under the readout ------------------------------------------------ */

static const char k_logic[] = "60 FPS logic: 52/s - SLOWED";

static int rects_meet(const int a[4], const int b[4]) {
    return a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3];
}

/* One frame's warning: its chip at the top left inside the safe area, under the readout when one
 * shows and left-aligned with it, its words whole inside it, and clear of every other element. */
static void logic_check(const NkUiDrawList *list, const AceUiFrame *f, float density, const char *what) {
    const int w = f->pixel_w, h = f->pixel_h;
    const double scale = (double)f->display_scale;
    const int safe = (int)(16.0f * density) - 1;
    int logic[4], words[4], other[4];
    CHECK(element_bounds(list, ACE_UI_ID_LOGIC, logic) && element_bounds(list, ACE_UI_ID_LOGIC_TEXT, words),
          "%s %dx%d@%.2f: no slowed-logic warning", what, w, h, scale);
    if (!element_bounds(list, ACE_UI_ID_LOGIC, logic) || !element_bounds(list, ACE_UI_ID_LOGIC_TEXT, words))
        return;
    CHECK(logic[0] >= safe && logic[1] >= safe && logic[2] <= w - safe && logic[3] <= h - safe &&
          logic[0] < w / 2, "%s %dx%d@%.2f: the warning (%d,%d)-(%d,%d) is not at the left inside the safe area",
          what, w, h, scale, logic[0], logic[1], logic[2], logic[3]);
    CHECK(words[0] >= logic[0] && words[1] >= logic[1] && words[2] <= logic[2] && words[3] <= logic[3],
          "%s %dx%d@%.2f: the warning's words leave its chip", what, w, h, scale);
    int runs = 0;
    for (uint32_t i = 0; i < list->ncmd; i++) {
        const NkUiDrawCmd *c = &list->cmd[i];
        if (c->kind != NK_UI_DRAW_TEXT || c->element_id != ACE_UI_ID_LOGIC_TEXT) continue;
        const NkUiTextRun *run = (const NkUiTextRun *)c->custom;
        char read[sizeof(k_logic) + 16];
        size_t used = 0;
        for (size_t k = 0; k < run->length && used + 1u < sizeof(read); k++) {
            const int nbsp = (unsigned char)run->text[k] == 0xC2u && k + 1u < run->length &&
                             (unsigned char)run->text[k + 1u] == 0xA0u;
            read[used++] = nbsp ? ' ' : run->text[k];
            k += (size_t)nbsp;
        }
        read[used] = '\0';
        CHECK(strcmp(read, k_logic) == 0, "%s %dx%d@%.2f: the warning reads \"%s\"", what, w, h, scale, read);
        runs++;
    }
    CHECK(runs == 1, "%s %dx%d@%.2f: the warning is %d runs", what, w, h, scale, runs);
    if (f->readout) {
        /* The bug: the warning drawn where the readout is, under its chip. */
        CHECK(element_bounds(list, ACE_UI_ID_READOUT, other) && !rects_meet(logic, other) &&
              logic[1] >= other[3] && logic[0] == other[0],
              "%s %dx%d@%.2f: the warning (%d,%d)-(%d,%d) is not under the readout (%d,%d)-(%d,%d)", what, w, h,
              scale, logic[0], logic[1], logic[2], logic[3], other[0], other[1], other[2], other[3]);
    } else {
        CHECK(logic[0] <= safe + 2 && logic[1] <= safe + 2, "%s %dx%d@%.2f: alone, the warning (%d,%d) is not "
              "in the top-left corner", what, w, h, scale, logic[0], logic[1]);
    }
    static const NkUiId others[] = { ACE_UI_ID_NOTICE, ACE_UI_ID_KEYBOARD, ACE_UI_ID_HOST_NOTICE };
    static const char *const names[] = { "launch notice", "keyboard", "render notice" };
    for (size_t o = 0; o < sizeof(others) / sizeof(others[0]); o++)
        if (element_bounds(list, others[o], other))
            CHECK(!rects_meet(logic, other), "%s %dx%d@%.2f: the warning (%d,%d)-(%d,%d) meets the %s "
                  "(%d,%d)-(%d,%d)", what, w, h, scale, logic[0], logic[1], logic[2], logic[3], names[o],
                  other[0], other[1], other[2], other[3]);
    if (element_bounds(list, ACE_UI_ID_KEYBOARD, other))
        CHECK(other[1] >= logic[3], "%s %dx%d@%.2f: the keyboard covers the warning", what, w, h, scale);
    if (element_bounds(list, ACE_UI_ID_HOST_NOTICE, other))
        CHECK(other[1] >= logic[3], "%s %dx%d@%.2f: the render notice covers the warning", what, w, h, scale);
    for (uint32_t i = 0; i < list->ncmd; i++) {
        const NkUiDrawCmd *c = &list->cmd[i];
        if (c->kind == NK_UI_DRAW_SCISSOR_START || c->kind == NK_UI_DRAW_SCISSOR_END) continue;
        CHECK(c->bounds[0] >= 0 && c->bounds[1] >= 0 && c->bounds[2] <= w && c->bounds[3] <= h,
              "%s %dx%d@%.2f: command %u outside the window", what, w, h, scale, i);
    }
}

/* Every window and display scale the cells here use, each with the warning beside the readout
 * and the launch notice, over the keyboard, over the render notice, and alone. */
static void cell_logic_notice(void) {
    static const struct { int w, h; float scale; } sizes[] = {
        {960, 600, 1.0f}, {1280, 800, 1.0f}, {1920, 1200, 1.5f}, {480, 272, 1.0f}, {960, 544, 1.0f},
        {1280, 720, 1.0f}, {2560, 1080, 1.0f}, {5120, 1440, 2.0f}, {480, 272, 2.0f}, {720, 408, 1.5f},
        {320, 180, 1.0f}, {640, 1136, 1.0f}, {3840, 2160, 2.0f},
        {960, 540, 1.0f}, {960, 540, 1.25f}, {960, 540, 1.5f}, {960, 540, 2.0f},
        {1280, 720, 1.25f}, {1280, 720, 1.5f}, {1280, 720, 2.0f},
        {1920, 1080, 1.0f}, {1920, 1080, 1.25f}, {1920, 1080, 1.5f}, {1920, 1080, 2.0f},
    };
    static const char *const variants[] = { "with the keyboard", "over the render notice", "with the chips",
                                            "alone" };
    NkUiConfig config;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_OVERLAY;
    config.text = &k_shaped;
    NkUi *ui = nk_ui_create(&config);
    CHECK(ui != NULL, "nk_ui_create (warning)");
    if (!ui) return;
    keyboard_open();
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        for (size_t v = 0; v < sizeof(variants) / sizeof(variants[0]); v++) {
            AceUiFrame f = frame_full(sizes[s].w, sizes[s].h, sizes[s].scale);
            f.logic_notice = k_logic;
            if (v != 0) f.keyboard = NULL;
            if (v == 1) {
                f.host_notice_head = k_host_head;
                f.host_notice = k_host_words;
                f.host_notice_hint = k_host_hint;
            }
            if (v == 3) {
                f.readout = NULL;
                f.notice = NULL;
            }
            float density = 1.0f;
            const NkUiDrawList *list = build(ui, &f, &density);
            CHECK(list != NULL, "warning %s %dx%d: the frame was dropped", variants[v], f.pixel_w, f.pixel_h);
            if (list) logic_check(list, &f, density, variants[v]);
        }
    }
    AceUiFrame alone;
    memset(&alone, 0, sizeof(alone));
    alone.pixel_w = 960;
    alone.pixel_h = 544;
    alone.display_scale = 1.0f;
    alone.logic_notice = k_logic;
    CHECK(ace_ui_frame_wants(&alone), "the warning alone is not wanted");
    alone.logic_notice = "";
    CHECK(!ace_ui_frame_wants(&alone), "an empty warning is wanted");
    osk_overlay_reset(&s_keyboard);
    nk_ui_destroy(ui);
}

/* ---- the cheats marker under the readout and the warning (#2639) ------------------------------- */

static const char k_cheats[] = "Cheats on: 2 (1 refused)";

/* One frame's marker: its chip at the top left inside the safe area, last in that column and
 * left-aligned with what is above it, its words whole and exact inside it, clear of every other
 * element. */
static void cheats_check(const NkUiDrawList *list, const AceUiFrame *f, float density, const char *what) {
    const int w = f->pixel_w, h = f->pixel_h;
    const double scale = (double)f->display_scale;
    const int safe = (int)(16.0f * density) - 1;
    int chip[4], words[4], other[4];
    int found = element_bounds(list, ACE_UI_ID_CHEATS, chip) && element_bounds(list, ACE_UI_ID_CHEATS_TEXT, words);
    CHECK(found, "%s %dx%d@%.2f: no cheats marker", what, w, h, scale);
    if (!found) return;
    CHECK(chip[0] >= safe && chip[1] >= safe && chip[2] <= w - safe && chip[3] <= h - safe && chip[0] < w / 2,
          "%s %dx%d@%.2f: the marker (%d,%d)-(%d,%d) is not at the left inside the safe area", what, w, h, scale,
          chip[0], chip[1], chip[2], chip[3]);
    CHECK(words[0] >= chip[0] && words[1] >= chip[1] && words[2] <= chip[2] && words[3] <= chip[3],
          "%s %dx%d@%.2f: the marker's words leave its chip", what, w, h, scale);
    int runs = 0;
    for (uint32_t i = 0; i < list->ncmd; i++) {
        const NkUiDrawCmd *c = &list->cmd[i];
        if (c->kind != NK_UI_DRAW_TEXT || c->element_id != ACE_UI_ID_CHEATS_TEXT) continue;
        const NkUiTextRun *run = (const NkUiTextRun *)c->custom;
        char read[sizeof(k_cheats) + 16];
        size_t used = 0;
        for (size_t k = 0; k < run->length && used + 1u < sizeof(read); k++) {
            const int nbsp = (unsigned char)run->text[k] == 0xC2u && k + 1u < run->length &&
                             (unsigned char)run->text[k + 1u] == 0xA0u;
            read[used++] = nbsp ? ' ' : run->text[k];
            k += (size_t)nbsp;
        }
        read[used] = '\0';
        CHECK(strcmp(read, k_cheats) == 0, "%s %dx%d@%.2f: the marker reads \"%s\"", what, w, h, scale, read);
        runs++;
    }
    CHECK(runs == 1, "%s %dx%d@%.2f: the marker is %d runs", what, w, h, scale, runs);
    static const NkUiId above[] = { ACE_UI_ID_READOUT, ACE_UI_ID_LOGIC };
    static const char *const above_names[] = { "readout", "slowed-logic warning" };
    int any_above = 0;
    for (size_t o = 0; o < sizeof(above) / sizeof(above[0]); o++) {
        if (!element_bounds(list, above[o], other)) continue;
        any_above = 1;
        CHECK(!rects_meet(chip, other) && chip[1] >= other[3] && chip[0] == other[0],
              "%s %dx%d@%.2f: the marker (%d,%d)-(%d,%d) is not under the %s (%d,%d)-(%d,%d)", what, w, h, scale,
              chip[0], chip[1], chip[2], chip[3], above_names[o], other[0], other[1], other[2], other[3]);
    }
    if (!any_above)
        CHECK(chip[0] <= safe + 2 && chip[1] <= safe + 2, "%s %dx%d@%.2f: alone, the marker (%d,%d) is not in "
              "the top-left corner", what, w, h, scale, chip[0], chip[1]);
    static const NkUiId others[] = { ACE_UI_ID_NOTICE, ACE_UI_ID_KEYBOARD, ACE_UI_ID_HOST_NOTICE };
    static const char *const names[] = { "launch notice", "keyboard", "render notice" };
    for (size_t o = 0; o < sizeof(others) / sizeof(others[0]); o++)
        if (element_bounds(list, others[o], other))
            CHECK(!rects_meet(chip, other), "%s %dx%d@%.2f: the marker (%d,%d)-(%d,%d) meets the %s "
                  "(%d,%d)-(%d,%d)", what, w, h, scale, chip[0], chip[1], chip[2], chip[3], names[o],
                  other[0], other[1], other[2], other[3]);
    for (uint32_t i = 0; i < list->ncmd; i++) {
        const NkUiDrawCmd *c = &list->cmd[i];
        if (c->kind == NK_UI_DRAW_SCISSOR_START || c->kind == NK_UI_DRAW_SCISSOR_END) continue;
        CHECK(c->bounds[0] >= 0 && c->bounds[1] >= 0 && c->bounds[2] <= w && c->bounds[3] <= h,
              "%s %dx%d@%.2f: command %u outside the window", what, w, h, scale, i);
    }
}

/* The marker in an authentic run (alone, and over the render notice) and in an Ace run (under
 * the readout and the warning, beside the launch notice, over the keyboard), at every window
 * and display scale the cells here use. A frame without it draws no marker. */
static void cell_cheats_marker(void) {
    static const struct { int w, h; float scale; } sizes[] = {
        {960, 600, 1.0f}, {1280, 800, 1.0f}, {1920, 1200, 1.5f}, {480, 272, 1.0f}, {960, 544, 1.0f},
        {1280, 720, 1.0f}, {2560, 1080, 1.0f}, {5120, 1440, 2.0f}, {480, 272, 2.0f}, {720, 408, 1.5f},
        {320, 180, 1.0f}, {640, 1136, 1.0f}, {3840, 2160, 2.0f},
        {960, 540, 1.25f}, {1280, 720, 1.5f}, {1920, 1080, 1.0f}, {1920, 1080, 2.0f},
    };
    static const char *const variants[] = { "alone", "over the render notice", "with the chips",
                                            "under the warning, with the keyboard" };
    NkUiConfig config;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_OVERLAY;
    config.text = &k_shaped;
    NkUi *ui = nk_ui_create(&config);
    CHECK(ui != NULL, "nk_ui_create (cheats marker)");
    if (!ui) return;
    keyboard_open();
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        for (size_t v = 0; v < sizeof(variants) / sizeof(variants[0]); v++) {
            AceUiFrame f = frame_full(sizes[s].w, sizes[s].h, sizes[s].scale);
            f.cheats_marker = k_cheats;
            if (v != 3) f.keyboard = NULL;
            if (v == 3) f.logic_notice = k_logic;
            if (v <= 1) {
                f.readout = NULL;
                f.notice = NULL;
            }
            if (v == 1) {
                f.host_notice_head = k_host_head;
                f.host_notice = k_host_words;
                f.host_notice_hint = k_host_hint;
            }
            float density = 1.0f;
            const NkUiDrawList *list = build(ui, &f, &density);
            CHECK(list != NULL, "marker %s %dx%d: the frame was dropped", variants[v], f.pixel_w, f.pixel_h);
            if (list) cheats_check(list, &f, density, variants[v]);
            /* The same frame without cheats draws no marker. */
            f.cheats_marker = NULL;
            list = build(ui, &f, &density);
            if (list) {
                int chip[4];
                CHECK(!element_bounds(list, ACE_UI_ID_CHEATS, chip) &&
                      !element_bounds(list, ACE_UI_ID_CHEATS_TEXT, chip),
                      "marker %s %dx%d: a run without cheats draws the marker", variants[v], f.pixel_w, f.pixel_h);
            }
        }
    }
    AceUiFrame alone;
    memset(&alone, 0, sizeof(alone));
    alone.pixel_w = 960;
    alone.pixel_h = 544;
    alone.display_scale = 1.0f;
    alone.cheats_marker = k_cheats;
    CHECK(ace_ui_frame_wants(&alone), "the cheats marker alone is not wanted");
    alone.cheats_marker = "";
    CHECK(!ace_ui_frame_wants(&alone), "an empty cheats marker is wanted");
    osk_overlay_reset(&s_keyboard);
    nk_ui_destroy(ui);
}

/* ---- gpu ------------------------------------------------------------------------------------- */

typedef struct {
    PFN_vkGetInstanceProcAddr gipa;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t family;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
    VkImage image;
    VkDeviceMemory image_memory;
    VkBuffer readback;
    VkDeviceMemory readback_memory;
    void *pixels;
} Gpu;

static Gpu g;

#define GPU_FN(name) static PFN_vk##name name
GPU_FN(CreateInstance); GPU_FN(DestroyInstance); GPU_FN(EnumeratePhysicalDevices);
GPU_FN(GetPhysicalDeviceQueueFamilyProperties); GPU_FN(GetPhysicalDeviceMemoryProperties);
GPU_FN(CreateDevice); GPU_FN(DestroyDevice); GPU_FN(GetDeviceProcAddr); GPU_FN(GetDeviceQueue);
GPU_FN(CreateCommandPool); GPU_FN(DestroyCommandPool); GPU_FN(AllocateCommandBuffers);
GPU_FN(CreateFence); GPU_FN(DestroyFence); GPU_FN(WaitForFences); GPU_FN(ResetFences);
GPU_FN(CreateImage); GPU_FN(DestroyImage); GPU_FN(GetImageMemoryRequirements);
GPU_FN(CreateBuffer); GPU_FN(DestroyBuffer); GPU_FN(GetBufferMemoryRequirements);
GPU_FN(AllocateMemory); GPU_FN(FreeMemory); GPU_FN(BindImageMemory); GPU_FN(BindBufferMemory);
GPU_FN(MapMemory); GPU_FN(BeginCommandBuffer); GPU_FN(EndCommandBuffer); GPU_FN(ResetCommandBuffer);
GPU_FN(CmdPipelineBarrier); GPU_FN(CmdClearColorImage); GPU_FN(CmdCopyImageToBuffer);
GPU_FN(QueueSubmit); GPU_FN(DeviceWaitIdle);
#undef GPU_FN

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    GetPhysicalDeviceMemoryProperties(g.physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static int gpu_open(char *why, size_t cap, int w, int h) {
#if defined(_WIN32)
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (lib) g.gipa = (PFN_vkGetInstanceProcAddr)(void (*)(void))GetProcAddress(lib, "vkGetInstanceProcAddr");
#else
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (lib) *(void **)&g.gipa = dlsym(lib, "vkGetInstanceProcAddr");
#endif
    if (!g.gipa) { snprintf(why, cap, "no Vulkan loader"); return 0; }
    CreateInstance = (PFN_vkCreateInstance)g.gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "ace_ui_selftest", 1, NULL, 0,
                              VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    if (!CreateInstance || CreateInstance(&ici, NULL, &g.instance) != VK_SUCCESS) {
        snprintf(why, cap, "no usable Vulkan driver");
        return 0;
    }
#define INST(name) name = (PFN_vk##name)g.gipa(g.instance, "vk" #name)
    INST(DestroyInstance); INST(EnumeratePhysicalDevices); INST(GetPhysicalDeviceQueueFamilyProperties);
    INST(GetPhysicalDeviceMemoryProperties); INST(CreateDevice); INST(GetDeviceProcAddr);
#undef INST
    VkPhysicalDevice devs[8];
    uint32_t nd = 8;
    if (EnumeratePhysicalDevices(g.instance, &nd, devs) < VK_SUCCESS || nd == 0) {
        snprintf(why, cap, "no Vulkan device");
        return 0;
    }
    for (uint32_t d = 0; d < nd && !g.physical; d++) {
        VkQueueFamilyProperties qf[16];
        uint32_t nq = 16;
        GetPhysicalDeviceQueueFamilyProperties(devs[d], &nq, qf);
        for (uint32_t q = 0; q < nq; q++)
            if (qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g.physical = devs[d]; g.family = q; break; }
    }
    if (!g.physical) { snprintf(why, cap, "no graphics queue"); return 0; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof(qci));
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = g.family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (CreateDevice(g.physical, &dci, NULL, &g.device) != VK_SUCCESS) {
        /* A device with the queue is there, and could not be made: a failure, not a skip. */
        snprintf(why, cap, "vkCreateDevice failed");
        return -1;
    }
#define DEV(name) name = (PFN_vk##name)GetDeviceProcAddr(g.device, "vk" #name)
    DEV(DestroyDevice); DEV(GetDeviceQueue); DEV(CreateCommandPool); DEV(DestroyCommandPool);
    DEV(AllocateCommandBuffers); DEV(CreateFence); DEV(DestroyFence); DEV(WaitForFences);
    DEV(ResetFences); DEV(CreateImage); DEV(DestroyImage); DEV(GetImageMemoryRequirements);
    DEV(CreateBuffer); DEV(DestroyBuffer); DEV(GetBufferMemoryRequirements); DEV(AllocateMemory);
    DEV(FreeMemory); DEV(BindImageMemory); DEV(BindBufferMemory); DEV(MapMemory);
    DEV(BeginCommandBuffer); DEV(EndCommandBuffer); DEV(ResetCommandBuffer); DEV(CmdPipelineBarrier);
    DEV(CmdClearColorImage); DEV(CmdCopyImageToBuffer); DEV(QueueSubmit); DEV(DeviceWaitIdle);
#undef DEV
    GetDeviceQueue(g.device, g.family, 0, &g.queue);
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g.family };
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL,
                                        VK_NULL_HANDLE, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
    if (CreateCommandPool(g.device, &pci, NULL, &g.pool) != VK_SUCCESS) return -1;
    cai.commandPool = g.pool;
    if (AllocateCommandBuffers(g.device, &cai, &g.cb) != VK_SUCCESS ||
        CreateFence(g.device, &fci, NULL, &g.fence) != VK_SUCCESS) return -1;
    /* The stand-in swapchain image: the formats and usage the presenter's images have. */
    VkImageCreateInfo ii;
    memset(&ii, 0, sizeof(ii));
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_B8G8R8A8_UNORM;
    ii.extent.width = (uint32_t)w;
    ii.extent.height = (uint32_t)h;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkMemoryRequirements mr;
    if (CreateImage(g.device, &ii, NULL, &g.image) != VK_SUCCESS) return -1;
    GetImageMemoryRequirements(g.device, g.image, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                 memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    if (mai.memoryTypeIndex == UINT32_MAX || AllocateMemory(g.device, &mai, NULL, &g.image_memory) != VK_SUCCESS ||
        BindImageMemory(g.device, g.image, g.image_memory, 0) != VK_SUCCESS) return -1;
    VkBufferCreateInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = (VkDeviceSize)w * (VkDeviceSize)h * 4u;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (CreateBuffer(g.device, &bi, NULL, &g.readback) != VK_SUCCESS) return -1;
    GetBufferMemoryRequirements(g.device, g.readback, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || AllocateMemory(g.device, &mai, NULL, &g.readback_memory) != VK_SUCCESS ||
        BindBufferMemory(g.device, g.readback, g.readback_memory, 0) != VK_SUCCESS ||
        MapMemory(g.device, g.readback_memory, 0, VK_WHOLE_SIZE, 0, &g.pixels) != VK_SUCCESS) return -1;
    return 1;
}

static void gpu_close(void) {
    if (g.device) {
        DeviceWaitIdle(g.device);
        if (g.readback) DestroyBuffer(g.device, g.readback, NULL);
        if (g.readback_memory) FreeMemory(g.device, g.readback_memory, NULL);
        if (g.image) DestroyImage(g.device, g.image, NULL);
        if (g.image_memory) FreeMemory(g.device, g.image_memory, NULL);
        if (g.fence) DestroyFence(g.device, g.fence, NULL);
        if (g.pool) DestroyCommandPool(g.device, g.pool, NULL);
        DestroyDevice(g.device, NULL);
    }
    if (g.instance) DestroyInstance(g.instance, NULL);
}

static void barrier(VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
                    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = g.image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    CmdPipelineBarrier(g.cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* One present: clear to the "game frame" colour, the pass, read back. Returns the pass's result. */
static int present(AceUi *ui, const AceUiFrame *f, int w, int h, uint32_t slot) {
    VkCommandBufferBeginInfo bbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                     VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    ResetCommandBuffer(g.cb, 0);
    BeginCommandBuffer(g.cb, &bbi);
    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue frame_colour = {{0.25f, 0.5f, 0.75f, 1.0f}};
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    CmdClearColorImage(g.cb, g.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &frame_colour, 1, &rng);
    VkImage images[1] = { g.image };
    AceUiTarget t;
    memset(&t, 0, sizeof(t));
    t.cmd = g.cb;
    t.frame_slot = slot;
    t.format = VK_FORMAT_B8G8R8A8_UNORM;
    t.extent.width = (uint32_t)w;
    t.extent.height = (uint32_t)h;
    t.images = images;
    t.image_count = 1;
    t.image_index = 0;
    t.swapchain_gen = 1;
    int drew = ace_ui_record(ui, &t, f);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)w;
    c.imageExtent.height = (uint32_t)h;
    c.imageExtent.depth = 1;
    CmdCopyImageToBuffer(g.cb, g.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.readback, 1, &c);
    EndCommandBuffer(g.cb);
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g.cb;
    ResetFences(g.device, 1, &g.fence);
    if (QueueSubmit(g.queue, 1, &si, g.fence) != VK_SUCCESS) return -1;
    WaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX);
    return drew;
}

static uint32_t pixel(int w, int x, int y) {
    const uint8_t *p = (const uint8_t *)g.pixels + ((size_t)y * (size_t)w + (size_t)x) * 4u;
    return ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];   /* BGRA -> 0xRRGGBB */
}

/* Whether the clear colour read back is the UNORM8 value of {0.25, 0.5, 0.75}, within one level for
 * the device's rounding. */
static int is_frame_colour(uint32_t c) {
    static const int want[3] = { 64, 128, 191 };   /* 0.25, 0.5 and 0.75 of 255, R G B */
    for (int k = 0; k < 3; k++) {
        const int got = (int)((c >> (16 - 8 * k)) & 0xFFu);
        if (got < want[k] - 1 || got > want[k] + 1) return 0;
    }
    return 1;
}

/* Every pixel of the read-back image that no command of frame f's draw list covers is the game
 * frame's colour: the pass drew nowhere else. The list is laid out again here, with the pass's text
 * backend, and a command covers its bounds. */
static void check_outside(const NkUiTextBackend *text, const AceUiFrame *f, int w, int h, uint32_t frame_colour,
                          const char *what) {
    NkUiConfig config;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_OVERLAY;
    config.text = text;
    NkUi *probe = nk_ui_create(&config);
    float density = 1.0f;
    const NkUiDrawList *list = probe ? build(probe, f, &density) : NULL;
    unsigned char *covered = (unsigned char *)calloc((size_t)w * (size_t)h, 1u);
    CHECK(list != NULL && covered != NULL, "%s: the frame could not be laid out to check the pixels outside it", what);
    if (list && covered) {
        for (uint32_t i = 0; i < list->ncmd; i++) {
            const NkUiDrawCmd *c = &list->cmd[i];
            if (c->kind == NK_UI_DRAW_SCISSOR_START || c->kind == NK_UI_DRAW_SCISSOR_END) continue;
            const int x0 = c->bounds[0] < 0 ? 0 : c->bounds[0], y0 = c->bounds[1] < 0 ? 0 : c->bounds[1];
            const int x1 = c->bounds[2] > w ? w : c->bounds[2], y1 = c->bounds[3] > h ? h : c->bounds[3];
            for (int y = y0; y < y1; y++)
                if (x1 > x0) memset(covered + (size_t)y * (size_t)w + (size_t)x0, 1, (size_t)(x1 - x0));
        }
        unsigned changed = 0;
        int first_x = -1, first_y = -1;
        uint32_t first = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                if (covered[(size_t)y * (size_t)w + (size_t)x] || pixel(w, x, y) == frame_colour) continue;
                if (!changed) { first_x = x; first_y = y; first = pixel(w, x, y); }
                changed++;
            }
        CHECK(changed == 0, "%s: %u pixel(s) outside every element changed, the first at (%d,%d) is %06x, "
              "not %06x", what, changed, first_x, first_y, first, frame_colour);
    }
    free(covered);
    nk_ui_destroy(probe);
}

static void write_ppm(const char *path, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = pixel(w, x, y);
            unsigned char rgb[3] = { (unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

#if defined(NK_UI_WITH_SDL_TTF)
/* Set when the SDL3_ttf library could not be loaded at run time: the font legs are a named SKIP
 * (exit 77), never a silent pass. */
static int s_fonts_skipped;

/* The launcher's fonts from a player folder, as the overlay host registers them. Without the
 * SDL3_ttf library the font legs skip by name; with it, a font that does not load from the folder
 * the run was given is a failure. */
static NkUiTtf *fonts_open(const char *root) {
    NkUiTtf *ttf = nk_ui_ttf_create(NULL, 0);
    if (!ttf) {
        if (!s_fonts_skipped)
            printf("SKIP ace-ui-selftest fonts: the SDL3_ttf library could not be loaded (%s); the launcher's "
                   "fonts were not laid out or drawn\n", nk_ui_ttf_last_error());
        s_fonts_skipped = 1;
        return NULL;
    }
    for (int font = 0; font < NK_UI_FONT_COUNT; font++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", root, nk_ui_font_path(font));
        if (!nk_ui_ttf_add_font(ttf, font, path)) {
            CHECK(0, "the launcher's font %s could not be loaded (%s)", path, nk_ui_ttf_last_error());
            nk_ui_ttf_destroy(ttf);
            return NULL;
        }
    }
    for (int font = 0; font < NK_UI_FONT_COUNT; font++) {
        if (font == NK_UI_FONT_CJK) continue;
        if (font != NK_UI_FONT_BODY) (void)nk_ui_ttf_set_fallback(ttf, font, NK_UI_FONT_BODY);
        (void)nk_ui_ttf_set_fallback(ttf, font, NK_UI_FONT_CJK);
    }
    return ttf;
}
#endif

#if !defined(ACE_UI_SELFTEST_S1)
/* ---- the launch notice with every part on ---------------------------------------------------- */

static const char k_join[] = " \xC2\xB7 ";

/* What a run shows with every part on, from the parser's key table (ace_every_part_line): the
 * longest list of parts this build can put in the notice. */
static char s_every[ACE_WORDS_MAX];

static int every_part_words(void) {
    char line[ACE_SPEC_MAX];
    size_t n = 0;
    if (ace_every_part_line(line, sizeof(line)) && ace_init_spec(line, NULL) == 1)
        n = ace_feature_words(s_every, sizeof(s_every));
    (void)ace_init_spec("off", NULL);
    return n > 0;
}

/* The items in the first `length` bytes of a list. */
static int list_items(const char *list, size_t length) {
    int items = length ? 1 : 0;
    for (const char *s = list; (s = strstr(s, k_join)) != NULL && s < list + length; s += strlen(k_join))
        items++;
    return items;
}

/* Whether `shown` is `all` shortened by the rule: its first items, each whole, then "and N more"
 * for the N it leaves out. */
static int ends_counted(const char *shown, const char *all) {
    const char *tail = NULL;
    int more = 0, used = 0;
    for (const char *s = shown; (s = strstr(s, "and ")) != NULL; s++) tail = s;
    if (!tail || sscanf(tail, "and %d more%n", &more, &used) != 1 || !used || tail[used] != '\0' || more < 1)
        return 0;
    size_t kept = (size_t)(tail - shown);
    if (kept) {
        if (kept < strlen(k_join) || strncmp(tail - strlen(k_join), k_join, strlen(k_join)) != 0) return 0;
        kept -= strlen(k_join);
        if (strncmp(shown, all, kept) != 0 || strncmp(all + kept, k_join, strlen(k_join)) != 0) return 0;
    }
    return list_items(all, kept) + more == list_items(all, strlen(all));
}

/* Block glyphs measured as a font measures a list: by the character, half the type size each,
 * so a no-break space or a dot is as wide as one letter and not two (k_shaped counts bytes, which
 * makes a list of short items half as wide again as the launcher's fonts make it), and a run
 * shaped whole is wider than its items measured one by one, as above. */
static NkUiTextSize text_measure_chars(void *user, int font, int px, const char *s, size_t n) {
    NkUiTextSize size = text_measure(user, font, px, s, n);
    size_t chars = 0;
    for (size_t i = 0; i < n; i++) chars += ((unsigned char)s[i] & 0xC0u) != 0x80u;
    size.width = (float)chars * (float)px * 0.5f + (float)(chars / 8u) * (float)px / 12.0f;
    return size;
}

static const NkUiTextBackend k_chars = {NULL, text_measure_chars, text_shape, text_raster};

static AceUiFrame frame_notice(int w, int h, float scale, const char *words) {
    AceUiFrame f = frame_full(w, h, scale);
    f.notice_head = "Ace";
    f.notice = words;
    f.readout = "100%  60 FPS";
    f.readout_warning = 0;
    f.keyboard = NULL;
    return f;
}

/* One frame's launch notice: its chip inside the safe area and clear of the readout, the render
 * notice and the keyboard; every line inside the chip and unclipped, whole items on each (a line
 * never starts with the dot, and ends with it while lines follow), nothing ending in an ellipsis,
 * nothing outside the window. Returns the lines and what they read. */
static int notice_lines(NkUi *ui, const NkUiTextBackend *text, const AceUiFrame *f, const char *what,
                        char *joined, size_t cap) {
    float density = 1.0f;
    const NkUiDrawList *list = build(ui, f, &density);
    const int w = f->pixel_w, h = f->pixel_h;
    const double scale = (double)f->display_scale;
    joined[0] = '\0';
    CHECK(list != NULL, "%s %dx%d@%.2f: the frame was dropped", what, w, h, scale);
    if (!list) return 0;
    int notice[4] = {0, 0, 0, 0}, words[4] = {0, 0, 0, 0}, other[4];
    const int safe = (int)(16.0f * density) - 1;
    const int lines = chip_words(list, ACE_UI_ID_NOTICE_TEXT, ACE_UI_NOTICE_LINES, "launch notice", text,
                                 words, joined, cap, w, h);
    CHECK(element_bounds(list, ACE_UI_ID_NOTICE, notice) && lines >= 1, "%s %dx%d@%.2f: no launch notice",
          what, w, h, scale);
    CHECK(notice[0] >= safe && notice[1] >= safe && notice[2] <= w - safe && notice[3] <= h - safe,
          "%s %dx%d@%.2f: the launch notice (%d,%d)-(%d,%d) leaves the safe area", what, w, h, scale,
          notice[0], notice[1], notice[2], notice[3]);
    CHECK(words[0] >= notice[0] && words[1] >= notice[1] && words[2] <= notice[2] && words[3] <= notice[3],
          "%s %dx%d@%.2f: the words (%d,%d)-(%d,%d) leave the launch notice (%d,%d)-(%d,%d)", what, w, h,
          scale, words[0], words[1], words[2], words[3], notice[0], notice[1], notice[2], notice[3]);
    CHECK(strstr(joined, "\xE2\x80\xA6") == NULL, "%s %dx%d@%.2f: the launch notice ends in an ellipsis: \"%s\"",
          what, w, h, scale, joined);
    int line = 0;
    for (uint32_t i = 0; i < list->ncmd; i++) {
        const NkUiDrawCmd *c = &list->cmd[i];
        if (c->kind == NK_UI_DRAW_SCISSOR_START || c->kind == NK_UI_DRAW_SCISSOR_END) continue;
        CHECK(c->bounds[0] >= 0 && c->bounds[1] >= 0 && c->bounds[2] <= w && c->bounds[3] <= h,
              "%s %dx%d@%.2f: command %u outside the window", what, w, h, scale, i);
        if (c->kind != NK_UI_DRAW_TEXT || c->element_id < ACE_UI_ID_NOTICE_TEXT ||
            c->element_id >= ACE_UI_ID_NOTICE_TEXT + ACE_UI_NOTICE_LINES) continue;
        const NkUiTextRun *run = (const NkUiTextRun *)c->custom;
        const int dot_first = run->length >= 2u && memcmp(run->text, "\xC2\xB7", 2u) == 0;
        const int dot_last = run->length >= 2u && memcmp(run->text + run->length - 2u, "\xC2\xB7", 2u) == 0;
        line++;
        CHECK(c->clip[0] <= c->bounds[0] && c->clip[1] <= c->bounds[1] && c->clip[2] >= c->bounds[2] &&
              c->clip[3] >= c->bounds[3], "%s %dx%d@%.2f: line %d of the launch notice is clipped", what, w,
              h, scale, line);
        CHECK(!dot_first && dot_last == (line < lines),
              "%s %dx%d@%.2f: line %d of %d of the launch notice splits an item: \"%.*s\"", what, w, h, scale,
              line, lines, (int)run->length, run->text);
    }
    if (f->readout)
        CHECK(element_bounds(list, ACE_UI_ID_READOUT, other) && other[2] < notice[0] && other[0] >= safe,
              "%s %dx%d@%.2f: the launch notice and the readout overlap", what, w, h, scale);
    if (f->host_notice)
        CHECK(element_bounds(list, ACE_UI_ID_HOST_NOTICE, other) && other[1] >= notice[3] &&
              other[3] <= h - safe, "%s %dx%d@%.2f: the launch notice (to y %d) and the render notice "
              "(%d to %d) do not both fit", what, w, h, scale, notice[3], other[1], other[3]);
    if (f->keyboard)
        CHECK(element_bounds(list, ACE_UI_ID_KEYBOARD, other) && other[1] >= notice[3] &&
              other[3] <= h - safe, "%s %dx%d@%.2f: the launch notice (to y %d) and the keyboard (%d to "
              "%d) do not both fit", what, w, h, scale, notice[3], other[1], other[3]);
    return lines;
}

/* The launch notice with the longest list this build can show, with `text` for its fonts. */
static void notice_every_part(const NkUiTextBackend *text, const char *fonts) {
    static const struct { int w, h; float scale; } sizes[] = {
        /* the default window, 720p and 1080p at each display scale the chip follows */
        {960, 540, 1.0f}, {960, 540, 1.25f}, {960, 540, 1.5f}, {960, 540, 2.0f},
        {1280, 720, 1.0f}, {1280, 720, 1.25f}, {1280, 720, 1.5f}, {1280, 720, 2.0f},
        {1920, 1080, 1.0f}, {1920, 1080, 1.25f}, {1920, 1080, 1.5f}, {1920, 1080, 2.0f},
        /* and the windows the other cells use, the smallest and the widest among them */
        {960, 600, 1.0f}, {1280, 800, 1.0f}, {1920, 1200, 1.5f}, {480, 272, 1.0f}, {960, 544, 1.0f},
        {2560, 1080, 1.0f}, {5120, 1440, 2.0f}, {480, 272, 2.0f}, {720, 408, 1.5f}, {320, 180, 1.0f},
        {640, 1136, 1.0f}, {3840, 2160, 2.0f},
    };
    /* More items than the chip's lines hold in any but the widest windows. */
    static char many[1024];
    size_t used = 0;
    for (int i = 1; i <= 90 && used + 16u < sizeof(many); i++)
        used += (size_t)snprintf(many + used, sizeof(many) - used, "%spart %02d", i > 1 ? k_join : "", i);
    NkUiConfig config;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_OVERLAY;
    config.text = text;
    NkUi *ui = nk_ui_create(&config);
    CHECK(ui != NULL, "nk_ui_create (%s)", fonts);
    if (!ui) return;
    keyboard_open();
    int most = 0, counted = 0;
    static char joined[2 * ACE_WORDS_MAX + sizeof(many)];
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        const int w = sizes[s].w, h = sizes[s].h;
        const double scale = (double)sizes[s].scale;
        /* Every word of every part, beside the readout: nothing cut, counted or left out. */
        AceUiFrame f = frame_notice(w, h, sizes[s].scale, s_every);
        int lines = notice_lines(ui, text, &f, fonts, joined, sizeof(joined));
        CHECK(lines <= ACE_UI_NOTICE_LINES && strcmp(joined, s_every) == 0,
              "%s %dx%d@%.2f: every part on reads, on %d line(s), \"%s\"; the parts are \"%s\"", fonts, w, h,
              scale, lines, joined, s_every);
        if (lines > most) most = lines;
        /* The same over the render notice (#2151): both whole, one under the other. */
        f.host_notice_head = k_host_head;
        f.host_notice = k_host_words;
        f.host_notice_hint = k_host_hint;
        (void)notice_lines(ui, text, &f, fonts, joined, sizeof(joined));
        CHECK(strcmp(joined, s_every) == 0, "%s %dx%d@%.2f: over the render notice every part on reads \"%s\"",
              fonts, w, h, scale, joined);
        /* The keyboard leaves one line: the list fits it, or its first items and the count. */
        f = frame_notice(w, h, sizes[s].scale, s_every);
        f.keyboard = &s_keyboard;
        lines = notice_lines(ui, text, &f, fonts, joined, sizeof(joined));
        CHECK(lines == 1 && (strcmp(joined, s_every) == 0 || ends_counted(joined, s_every)),
              "%s %dx%d@%.2f: beside the keyboard every part on reads, on %d line(s), \"%s\"", fonts, w, h,
              scale, lines, joined);
        /* A list past the chip's lines keeps them all and counts what it leaves out; a chip
         * that tall still leaves the render notice its place. */
        f = frame_notice(w, h, sizes[s].scale, many);
        f.host_notice_head = k_host_head;
        f.host_notice = k_host_words;
        f.host_notice_hint = k_host_hint;
        lines = notice_lines(ui, text, &f, fonts, joined, sizeof(joined));
        const int cut = strcmp(joined, many) != 0;
        CHECK(lines <= ACE_UI_NOTICE_LINES &&
              (!cut || (lines == ACE_UI_NOTICE_LINES && ends_counted(joined, many))),
              "%s %dx%d@%.2f: 90 parts read, on %d line(s), \"%s\"", fonts, w, h, scale, lines, joined);
        counted += cut;
    }
    CHECK(counted > 0, "%s: no window was too small for 90 parts, so the count was never drawn", fonts);
    /* A list that fits one line is the one run it always was. */
    static const char k_one[] = "16x\xC2\xA0" "filtering\xC2\xA0\xC2\xB7\xC2\xA0" "crisp\xC2\xA0keyboard"
                                "\xC2\xA0\xC2\xB7\xC2\xA0speed\xC2\xA0readout";
    AceUiFrame f = frame_full(1280, 720, 1.0f);
    f.keyboard = NULL;
    float density = 1.0f;
    const NkUiDrawList *list = build(ui, &f, &density);
    int runs = 0;
    for (uint32_t i = 0; list && i < list->ncmd; i++) {
        const NkUiDrawCmd *c = &list->cmd[i];
        if (c->kind != NK_UI_DRAW_TEXT || c->element_id < ACE_UI_ID_NOTICE_TEXT ||
            c->element_id >= ACE_UI_ID_NOTICE_TEXT + ACE_UI_NOTICE_LINES) continue;
        const NkUiTextRun *run = (const NkUiTextRun *)c->custom;
        runs++;
        CHECK(c->element_id == ACE_UI_ID_NOTICE_TEXT && run->length == strlen(k_one) &&
              memcmp(run->text, k_one, run->length) == 0, "%s: a short list reads \"%.*s\"", fonts,
              (int)run->length, run->text);
    }
    CHECK(runs == 1, "%s: a short list is %d runs, not one", fonts, runs);
    printf("ace-ui-selftest: every part on (%d parts, %zu bytes) takes at most %d of %d lines with %s\n",
           list_items(s_every, strlen(s_every)), strlen(s_every) + 1u, most, ACE_UI_NOTICE_LINES, fonts);
    nk_ui_destroy(ui);
}

static void cell_notice_every_part(const char *fonts_root) {
    CHECK(every_part_words(), "every part on is not a run this build takes");
    if (!s_every[0]) return;
    notice_every_part(&k_chars, "block glyphs");
#if defined(NK_UI_WITH_SDL_TTF)
    if (fonts_root) {
        NkUiTtf *ttf = fonts_open(fonts_root);
        if (ttf) {
            NkUiTextBackend ttf_text = nk_ui_ttf_backend(ttf);
            notice_every_part(&ttf_text, "the launcher's fonts");
            nk_ui_ttf_destroy(ttf);
        }
    }
#else
    (void)fonts_root;
#endif
}
#endif /* !ACE_UI_SELFTEST_S1 */

static int cell_gpu(const char *dir, const char *fonts_root) {
    const int w = 1280, h = 720;
    char why[128] = "";
    int opened = gpu_open(why, sizeof(why), w, h);
    if (opened <= 0) {
        gpu_close();
        if (opened == 0) {
            printf("ace-ui-selftest: gpu SKIP (%s)\n", why);
            return 77;
        }
        CHECK(0, "the headless device could not be set up (%s)", why[0] ? why : "no reason given");
        return 1;
    }
    const NkUiTextBackend *text = &k_blocks;
#if defined(NK_UI_WITH_SDL_TTF)
    NkUiTtf *ttf = NULL;
    NkUiTextBackend ttf_text;
    if (fonts_root) {
        ttf = fonts_open(fonts_root);
        if (ttf) { ttf_text = nk_ui_ttf_backend(ttf); text = &ttf_text; }
    }
#else
    (void)fonts_root;
#endif
    AceUiInit init;
    memset(&init, 0, sizeof(init));
    init.get_instance_proc_addr = g.gipa;
    init.instance = g.instance;
    init.physical_device = g.physical;
    init.device = g.device;
    init.queue = g.queue;
    init.queue_family = g.family;
    init.frames_in_flight = 1;
    init.text = text;
    char error[256];
    AceUi *ui = ace_ui_create(&init, error, sizeof(error));
    CHECK(ui != NULL, "ace_ui_create: %s", error);
    if (ui) {
        AceUiFrame empty;
        memset(&empty, 0, sizeof(empty));
        empty.pixel_w = w;
        empty.pixel_h = h;
        empty.display_scale = 1.0f;
        CHECK(present(ui, &empty, w, h, 0) == 0, "an empty frame drew");
        /* The clear colour as this device stores it, from a frame the pass left alone. */
        const uint32_t frame_colour = pixel(w, w / 2, h / 2);
        CHECK(is_frame_colour(frame_colour), "the cleared frame reads %06x, not {0.25, 0.5, 0.75}", frame_colour);
        CHECK(pixel(w, 22, 22) == frame_colour && pixel(w, w - 22, 22) == frame_colour &&
              pixel(w, w / 2, h - 60) == frame_colour, "an empty frame changed the image");
        check_outside(text, &empty, w, h, frame_colour, "an empty frame");
        keyboard_open();
        AceUiFrame f = frame_full(w, h, 1.0f);
        CHECK(present(ui, &f, w, h, 0) == 1, "the full frame did not draw");
        /* The game frame shows between the chips and above the keyboard. */
        CHECK(pixel(w, w / 2, 40) == frame_colour, "the frame between the chips changed: %06x",
              pixel(w, w / 2, 40));
        CHECK(pixel(w, w / 2, h / 4) == frame_colour, "the frame above the keyboard changed: %06x",
              pixel(w, w / 2, h / 4));
        /* Each chip and the keyboard covered the frame where it stands. */
        CHECK(pixel(w, 22, 22) != frame_colour, "no readout chip at the top left");
        CHECK(pixel(w, w - 22, 22) != frame_colour, "no notice chip at the top right");
        CHECK(pixel(w, w / 2, h - 60) != frame_colour, "no keyboard at the foot of the window");
        /* The 16 px safe area stays the game's. */
        CHECK(pixel(w, 8, 8) == frame_colour && pixel(w, w - 8, h - 8) == frame_colour,
              "the safe area changed");
        check_outside(text, &f, w, h, frame_colour, "the full frame");
        char path[600];
        snprintf(path, sizeof(path), "%s%sace_ui_selftest.ppm", dir ? dir : "", dir ? "/" : "");
        write_ppm(path, w, h);
        printf("ace-ui-selftest: gpu frame written to %s\n", path);
        /* An authentic run's render notice alone (#2151): its chip covers the bottom left; the
         * rest of the frame, the top corners and the safe area stay the game's. Hidden again,
         * the pass draws nothing and the frame is the game's everywhere. */
        AceUiFrame host;
        memset(&host, 0, sizeof(host));
        host.pixel_w = w;
        host.pixel_h = h;
        host.display_scale = 1.0f;
        host.host_notice_head = k_host_head;
        host.host_notice = k_host_words;
        host.host_notice_hint = k_host_hint;
        CHECK(present(ui, &host, w, h, 0) == 1, "the render notice did not draw");
        CHECK(pixel(w, 24, h - 24) != frame_colour, "no render notice at the bottom left");
        CHECK(pixel(w, 22, 22) == frame_colour && pixel(w, w - 22, 22) == frame_colour &&
              pixel(w, w / 2, h / 2) == frame_colour && pixel(w, w - 24, h - 24) == frame_colour,
              "the render notice changed the frame outside its chip");
        CHECK(pixel(w, 8, h - 8) == frame_colour && pixel(w, 8, 8) == frame_colour,
              "the render notice changed the safe area");
        check_outside(text, &host, w, h, frame_colour, "the render notice");
        snprintf(path, sizeof(path), "%s%sace_ui_selftest_notice.ppm", dir ? dir : "", dir ? "/" : "");
        write_ppm(path, w, h);
        printf("ace-ui-selftest: render notice frame written to %s\n", path);
        host.host_notice = NULL;
        CHECK(present(ui, &host, w, h, 0) == 0, "a hidden render notice drew");
        CHECK(pixel(w, 24, h - 24) == frame_colour, "a hidden render notice left its chip");
        check_outside(text, &host, w, h, frame_colour, "a hidden render notice");
        /* The slowed-logic warning under the readout: two chips, one under the other, with the
         * frame showing in the gap between them and beside the warning. */
        {
            AceUiFrame lf = frame_full(w, h, 1.0f);
            lf.keyboard = NULL;
            lf.logic_notice = k_logic;
            NkUiConfig config;
            memset(&config, 0, sizeof(config));
            config.host = NK_UI_HOST_OVERLAY;
            config.text = text;
            NkUi *probe = nk_ui_create(&config);
            float density = 1.0f;
            const NkUiDrawList *list = probe ? build(probe, &lf, &density) : NULL;
            int readout[4], logic[4];
            CHECK(list && element_bounds(list, ACE_UI_ID_READOUT, readout) &&
                  element_bounds(list, ACE_UI_ID_LOGIC, logic), "the warning frame did not lay out");
            if (list && element_bounds(list, ACE_UI_ID_READOUT, readout) &&
                element_bounds(list, ACE_UI_ID_LOGIC, logic)) {
                CHECK(present(ui, &lf, w, h, 0) == 1, "the warning frame did not draw");
                const int x = readout[0] + 4, gap_y = (readout[3] + logic[1]) / 2;
                CHECK(logic[1] > readout[3] + 1, "the warning is not apart from the readout");
                CHECK(pixel(w, x, (readout[1] + readout[3]) / 2) != frame_colour &&
                      pixel(w, x, (logic[1] + logic[3]) / 2) != frame_colour,
                      "no readout and warning chips at the top left");
                CHECK(pixel(w, x, gap_y) == frame_colour && pixel(w, logic[2] + 4, (logic[1] + logic[3]) / 2) ==
                      frame_colour, "the frame does not show between and beside the two chips");
                check_outside(text, &lf, w, h, frame_colour, "the warning under the readout");
                snprintf(path, sizeof(path), "%s%sace_ui_selftest_logic.ppm", dir ? dir : "", dir ? "/" : "");
                write_ppm(path, w, h);
                printf("ace-ui-selftest: the warning under the readout written to %s\n", path);
            }
            nk_ui_destroy(probe);
        }
        /* The cheats marker of an authentic run (#2639): alone at the top left, drawn through
         * the same pass, with the game's frame untouched beside and under it. */
        {
            AceUiFrame cf;
            memset(&cf, 0, sizeof(cf));
            cf.pixel_w = w;
            cf.pixel_h = h;
            cf.display_scale = 1.0f;
            cf.now_s = 1.0;
            cf.reduce_motion = 1;
            cf.cheats_marker = k_cheats;
            NkUiConfig config;
            memset(&config, 0, sizeof(config));
            config.host = NK_UI_HOST_OVERLAY;
            config.text = text;
            NkUi *probe = nk_ui_create(&config);
            float density = 1.0f;
            const NkUiDrawList *list = probe ? build(probe, &cf, &density) : NULL;
            int chip[4];
            CHECK(list && element_bounds(list, ACE_UI_ID_CHEATS, chip), "the cheats marker frame did not lay out");
            if (list && element_bounds(list, ACE_UI_ID_CHEATS, chip)) {
                CHECK(present(ui, &cf, w, h, 0) == 1, "the cheats marker frame did not draw");
                CHECK(pixel(w, chip[0] + 4, (chip[1] + chip[3]) / 2) != frame_colour,
                      "no cheats marker chip at the top left");
                CHECK(pixel(w, chip[2] + 4, (chip[1] + chip[3]) / 2) == frame_colour &&
                      pixel(w, chip[0] + 4, chip[3] + 4) == frame_colour && pixel(w, w / 2, h / 2) == frame_colour,
                      "the frame does not show beside and under the cheats marker");
                snprintf(path, sizeof(path), "%s%sace_ui_selftest_cheats.ppm", dir ? dir : "", dir ? "/" : "");
                write_ppm(path, w, h);
                printf("ace-ui-selftest: the cheats marker written to %s\n", path);
                cf.cheats_marker = NULL;
                CHECK(present(ui, &cf, w, h, 0) == 0, "a run without cheats drew the UI pass");
            }
            nk_ui_destroy(probe);
        }
#if !defined(ACE_UI_SELFTEST_S1)
        /* The launch notice with every part on: its chip grows down from the top right, and the
         * frame beside it, under it and in the safe area stays the game's. */
        if (s_every[0]) {
            AceUiFrame every = frame_notice(w, h, 1.0f, s_every);
            CHECK(present(ui, &every, w, h, 0) == 1, "the launch notice with every part on did not draw");
            CHECK(pixel(w, w - 22, 22) != frame_colour, "every part on: no notice chip at the top right");
            CHECK(pixel(w, w / 2 - 40, 40) == frame_colour && pixel(w, w - 22, h / 2) == frame_colour &&
                  pixel(w, w - 8, 8) == frame_colour && pixel(w, 8, h - 8) == frame_colour,
                  "every part on: the notice changed the frame outside its chip");
            check_outside(text, &every, w, h, frame_colour, "every part on");
            snprintf(path, sizeof(path), "%s%sace_ui_selftest_every_part.ppm", dir ? dir : "", dir ? "/" : "");
            write_ppm(path, w, h);
            printf("ace-ui-selftest: every part on written to %s\n", path);
        }
#endif
        ace_ui_destroy(ui);
    }
#if defined(NK_UI_WITH_SDL_TTF)
    nk_ui_ttf_destroy(ttf);
#endif
    gpu_close();
    return 0;
}

int main(int argc, char **argv) {
    const char *dir = NULL, *fonts = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fonts") == 0 && i + 1 < argc) fonts = argv[++i];
        else dir = argv[i];
    }
    cell_layout();
    cell_logic_notice();
    cell_cheats_marker();
#if !defined(ACE_UI_SELFTEST_S1)
    cell_notice_every_part(fonts);
#endif
    int gpu = cell_gpu(dir, fonts);
    if (s_fail) {
        fprintf(stderr, "ace-ui-selftest: %d check(s) FAILED\n", s_fail);
        return 1;
    }
    if (gpu == 77) {
        /* The layout passed; the GPU leg did not run. 77 keeps that visible. */
        printf("ace-ui-selftest: layout PASS; gpu SKIP\n");
        return 77;
    }
#if defined(NK_UI_WITH_SDL_TTF)
    if (s_fonts_skipped) {
        /* Everything else passed; the launcher's fonts were not laid out or drawn. */
        printf("ace-ui-selftest: layout and gpu PASS with block glyphs; the launcher's fonts SKIP\n");
        return 77;
    }
#endif
    printf("ace-ui-selftest: PASS (layout, gpu)\n");
    return 0;
}
