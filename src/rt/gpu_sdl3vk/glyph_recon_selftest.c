/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * glyph_recon_selftest.c - the CPU side of sharp game text (glyph_recon.h, #2110), on synthetic
 * glyphs only: the setting and its Ace gate, every source the rebuild refuses by name, the
 * rectangle classifier, the rebuild at every factor for every pixel format it accepts, and the
 * downsample check against tampered and stale tiles. Every cell prints "FAIL <cell>: <what>"; the
 * glyph-recon-mutant target builds this file against a glyph_recon.c with one guard removed and
 * passes only when the named cell fails.
 */
#include "glyph_recon.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
static int g_cells;

static void check(int ok, const char *cell, const char *fmt, ...) {
    g_cells++;
    if (ok) return;
    g_fail++;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FAIL %s: ", cell);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ---- the synthetic corpus ------------------------------------------------------------------- */

#define PW 64
/* The most a row or column of a rebuilt soft shadow may run back: the Catmull-Rom kernel's own undershoot is at most 19 of
 * 255 of a full step, and the rest is for the quantizing and the second axis. */
#define NK_GLYPH_SOFT_RUNBACK 24
#define PH 48
#define CELL_W 12
#define CELL_H 14

/* Exact-area style coverage of a shape in a CELL_W x CELL_H cell, 8 x 8 samples per texel. */
static float shape_in(int kind, float x, float y) {
    switch (kind) {
    case 0: {   /* "O": a ring */
        const float dx = x - 6.0f, dy = (y - 7.0f) * 0.85f, r = sqrtf(dx * dx + dy * dy);
        return r <= 5.2f && r >= 2.6f;
    }
    case 1:     /* "T": a bar and a stem */
        return (y >= 2.0f && y < 4.4f && x >= 1.5f && x < 10.5f) || (x >= 5.0f && x < 7.4f && y >= 2.0f && y < 12.0f);
    case 2: {   /* "/": a slanted bar */
        const float d = (x - 6.0f) + (y - 7.0f) * 0.55f;
        return fabsf(d) <= 1.3f && y >= 1.5f && y < 12.5f;
    }
    default:    /* "L" */
        return (x >= 2.5f && x < 4.9f && y >= 2.0f && y < 12.0f) || (y >= 9.6f && y < 12.0f && x >= 2.5f && x < 10.0f);
    }
}

static void make_cell(int kind, float cov[CELL_W * CELL_H]) {
    for (int y = 0; y < CELL_H; y++)
        for (int x = 0; x < CELL_W; x++) {
            int n = 0;
            for (int j = 0; j < 8; j++)
                for (int i = 0; i < 8; i++)
                    n += shape_in(kind, (float)x + ((float)i + 0.5f) / 8.0f, (float)y + ((float)j + 0.5f) / 8.0f) != 0.0f;
            cov[y * CELL_W + x] = (float)n / 64.0f;
        }
}

typedef struct Fmt {
    const char *name;
    NkGlyphFormat id;
    int levels;          /* alpha steps: 15 for 4444, 255 otherwise (T4 through a 16-entry CLUT: 15) */
    uint32_t rgb;        /* the colour the decoder gives every inked texel */
} Fmt;

static const Fmt k_fmts[] = {
    { "4444", NK_GLYPH_FMT_4444, 15, 0x0055AAFFu },   /* R=0xFF G=0xAA B=0x55 */
    { "8888", NK_GLYPH_FMT_8888, 255, 0x00C08040u },
    { "T4",   NK_GLYPH_FMT_T4, 15, 0x00FFFFFFu },
    { "T8",   NK_GLYPH_FMT_T8, 255, 0x00FFFFFFu },
};

/* Decode a cell as the renderer's decoder would for a format: alpha quantized to the format's steps,
 * one colour for every inked texel, and a stray colour on a blank one. */
static uint32_t pixel(float c, const Fmt *f) {
    const int a = (int)floorf(c * (float)f->levels + 0.5f) * (255 / f->levels);
    return a ? ((uint32_t)a << 24) | f->rgb : 0x00123456u;
}

/* The page: four glyph cells in a row with a one-texel gap, each at (cx * 16, 4); the rest blank. */
static void make_page(uint32_t page[PW * PH], const Fmt *f, int variant) {
    for (int i = 0; i < PW * PH; i++) page[i] = 0x00123456u;
    for (int g = 0; g < 4; g++) {
        float cov[CELL_W * CELL_H];
        make_cell((g + variant) & 3, cov);
        for (int y = 0; y < CELL_H; y++)
            for (int x = 0; x < CELL_W; x++) page[(4 + y) * PW + g * 16 + x] = pixel(cov[y * CELL_W + x], f);
    }
}

#define RX(g) ((g) * 16)
#define RY 4

/* ---- cells ---------------------------------------------------------------------------------- */

static void cell_settings(void) {
    NkGlyphConfig c;
    char why[96];
    check(nk_glyph_parse(NULL, &c, why, sizeof why) && !c.on, "settings", "unset is off");
    check(nk_glyph_parse("off", &c, why, sizeof why) && !c.on, "settings", "off is off");
    check(nk_glyph_parse("on", &c, why, sizeof why) && c.on, "settings", "on is on");
    check(!nk_glyph_parse("yes", &c, why, sizeof why) && !c.on && strstr(why, "SR_GLYPHS"), "settings",
          "a stray value is refused and names the variable");
    check(!nk_glyph_parse("ON", &c, why, sizeof why), "settings", "values are exact");

#ifdef _WIN32
    _putenv("SR_GLYPHS=on");
#else
    setenv("SR_GLYPHS", "on", 1);
#endif
    const NkGlyphConfig *k = nk_glyph_init_from_env(0);
    check(!k->on, "settings", "SR_GLYPHS=on with Ace Mode off");
    check(strstr(nk_glyph_start_lines(), "GLYPHS: off (sharp game text needs Ace Mode)") != NULL, "settings",
          "an authentic run names the request it does not obey: '%s'", nk_glyph_start_lines());
    k = nk_glyph_init_from_env(1);
    check(k->on, "settings-on", "SR_GLYPHS=on in an Ace run");
    check(strstr(nk_glyph_start_lines(), "GLYPHS: on (per_present=48, tolerance=2)") != NULL &&
              strstr(nk_glyph_start_lines(), "guest-visible, on through SR_GLYPHS and not admitted under contract section 11.2") != NULL,
          "settings-on", "the start lines: '%s'", nk_glyph_start_lines());
#ifdef _WIN32
    _putenv("SR_GLYPHS=maybe");
#else
    setenv("SR_GLYPHS", "maybe", 1);
#endif
    k = nk_glyph_init_from_env(1);
    check(!k->on && strstr(nk_glyph_start_lines(), "GLYPHS: refused (SR_GLYPHS takes off or on)") != NULL,
          "settings-refused", "a stray value in an Ace run: '%s'", nk_glyph_start_lines());
#ifdef _WIN32
    _putenv("SR_GLYPHS=");
#else
    unsetenv("SR_GLYPHS");
#endif
    k = nk_glyph_init_from_env(1);
    check(!k->on && nk_glyph_start_lines()[0] == '\0', "settings", "unset prints nothing");
    char line[400];
    NkGlyphConfig off = { 0 };
    check(nk_glyph_admission_line(&off, line, sizeof line) == 0 && line[0] == '\0', "settings", "off has no admission line");
}

static void cell_sources(void) {
    NkGlyphSource s = { NK_GLYPH_FMT_8888, 0, 1, 4 };
    for (size_t i = 0; i < sizeof k_fmts / sizeof k_fmts[0]; i++)
        for (int f = NK_GLYPH_MIN_FACTOR; f <= NK_GLYPH_MAX_FACTOR; f++) {
            s.format = k_fmts[i].id;
            s.render_scale = f;
            check(nk_glyph_source_check(&s) == NK_GLYPH_OK, "accept-formats", "%s at scale %d is accepted", k_fmts[i].name, f);
        }
    s.render_scale = 4;
    s.format = NK_GLYPH_FMT_8888;
    s.in_vram = 1;
    check(nk_glyph_source_check(&s) == NK_GLYPH_REFUSE_VRAM, "refuse-vram", "a page in VRAM is refused");
    s.in_vram = 0;
    s.format = NK_GLYPH_FMT_T4_REVERSED;
    check(nk_glyph_source_check(&s) == NK_GLYPH_REFUSE_REVERSED, "refuse-reversed", "a 4-bit reversed buffer is refused");
    s.format = NK_GLYPH_FMT_RGB24;
    check(nk_glyph_source_check(&s) == NK_GLYPH_REFUSE_RGB24, "refuse-rgb24", "a 24-bit buffer is refused");
    static const NkGlyphFormat lossy[] = { NK_GLYPH_FMT_5650, NK_GLYPH_FMT_5551, NK_GLYPH_FMT_T16, NK_GLYPH_FMT_T32, NK_GLYPH_FMT_DXT };
    for (size_t i = 0; i < sizeof lossy / sizeof lossy[0]; i++) {
        s.format = lossy[i];
        check(nk_glyph_source_check(&s) == NK_GLYPH_REFUSE_FORMAT, "refuse-format", "format %d has no usable coverage", (int)lossy[i]);
    }
    s.format = NK_GLYPH_FMT_8888;
    s.mip_levels = 2;
    check(nk_glyph_source_check(&s) == NK_GLYPH_REFUSE_MIP, "refuse-mip", "a mip chain is refused");
    s.mip_levels = 1;
    s.render_scale = 1;
    check(nk_glyph_source_check(&s) == NK_GLYPH_REFUSE_SCALE, "refuse-scale", "render scale 1 is refused");
    s.render_scale = 0;
    check(nk_glyph_source_check(&s) == NK_GLYPH_REFUSE_SCALE, "refuse-scale", "render scale 0 is refused");
    s.render_scale = NK_GLYPH_MAX_FACTOR + 1;
    check(nk_glyph_source_check(&s) == NK_GLYPH_REFUSE_SCALE, "refuse-scale", "render scale past the maximum is refused");
    check(nk_glyph_source_check(NULL) == NK_GLYPH_REFUSE_FORMAT, "refuse-format", "no source is refused");
    check(strcmp(nk_glyph_verdict_name(NK_GLYPH_REFUSE_VRAM), "vram") == 0 &&
              strcmp(nk_glyph_verdict_name((NkGlyphVerdict)99), "unknown") == 0, "names", "verdict names");
}

static void cell_rects(void) {
    static uint32_t page[PW * PH];
    uint32_t col = 0;
    make_page(page, &k_fmts[1], 0);
    check(nk_glyph_rect_check(page, PW, PH, RX(0), RY, CELL_W, CELL_H, &col) == NK_GLYPH_OK && col == k_fmts[1].rgb,
          "classify", "a glyph cell classifies and reports its colour (got %06x)", (unsigned)col);
    /* size: too small, too large, outside the page */
    check(nk_glyph_rect_check(page, PW, PH, RX(0), RY, 3, CELL_H, &col) == NK_GLYPH_REFUSE_SIZE, "refuse-size", "3 texels wide");
    check(nk_glyph_rect_check(page, PW, PH, RX(0), RY, CELL_W, 3, &col) == NK_GLYPH_REFUSE_SIZE, "refuse-size", "3 texels high");
    check(nk_glyph_rect_check(page, PW, PH, 0, 0, NK_GLYPH_MAX_SIDE + 1, 8, &col) == NK_GLYPH_REFUSE_SIZE, "refuse-size", "65 texels wide");
    check(nk_glyph_rect_check(page, PW, PH, PW - 6, RY, CELL_W, CELL_H, &col) == NK_GLYPH_REFUSE_SIZE, "refuse-size", "past the right edge");
    check(nk_glyph_rect_check(page, PW, PH, -1, RY, CELL_W, CELL_H, &col) == NK_GLYPH_REFUSE_SIZE, "refuse-size", "left of the page");
    /* empty */
    check(nk_glyph_rect_check(page, PW, PH, 0, 30, CELL_W, CELL_H, &col) == NK_GLYPH_REFUSE_EMPTY, "refuse-empty", "a blank cell");
    /* two colours */
    static uint32_t art[PW * PH];
    memcpy(art, page, sizeof art);
    art[(RY + 6) * PW + RX(0) + 2] = (art[(RY + 6) * PW + RX(0) + 2] & 0xff000000u) | 0x000000ffu;
    art[(RY + 6) * PW + RX(0) + 2] |= 0x80000000u;
    check(nk_glyph_rect_check(art, PW, PH, RX(0), RY, CELL_W, CELL_H, &col) == NK_GLYPH_REFUSE_COLOUR, "refuse-colour", "two inked colours");
    /* hard edge: every inked texel fully covered */
    static uint32_t bitmap[PW * PH];
    memcpy(bitmap, page, sizeof bitmap);
    for (int y = 0; y < CELL_H; y++)
        for (int x = 0; x < CELL_W; x++) {
            uint32_t *p = &bitmap[(RY + y) * PW + RX(0) + x];
            if (*p >> 24) *p = 0xff000000u | k_fmts[1].rgb;
        }
    check(nk_glyph_rect_check(bitmap, PW, PH, RX(0), RY, CELL_W, CELL_H, &col) == NK_GLYPH_REFUSE_HARD_EDGE, "refuse-hard-edge", "a bitmap font");
    /* a solid block with one soft texel is not anti-aliased text either */
    for (int i = 0; i < PW * PH; i++) bitmap[i] = 0x00123456u;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) bitmap[(4 + y) * PW + 4 + x] = 0xff000000u | k_fmts[1].rgb;
    bitmap[4 * PW + 4] = 0x80000000u | k_fmts[1].rgb;
    check(nk_glyph_rect_check(bitmap, PW, PH, 4, 4, 8, 8, &col) == NK_GLYPH_REFUSE_HARD_EDGE, "refuse-hard-edge", "one soft texel in a solid block");
}

/* Texels of a tile with alpha in the soft band, the pixels a blur or a soft edge puts between ink and blank. */
static int soft_pixels(const uint32_t *t, int n) {
    int soft = 0;
    for (int i = 0; i < n; i++) {
        const int a = (int)(t[i] >> 24);
        soft += a >= 32 && a <= 223;
    }
    return soft;
}

/* A bilinear enlargement of the same rectangle: the picture the guest's own filter gives at this scale. */
static void bilinear(const uint32_t *page, int x0, int y0, int w, int h, int f, uint32_t *out) {
    for (int Y = 0; Y < h * f; Y++)
        for (int X = 0; X < w * f; X++) {
            const float sx = ((float)X + 0.5f) / (float)f - 0.5f, sy = ((float)Y + 0.5f) / (float)f - 0.5f;
            int ix = (int)floorf(sx), iy = (int)floorf(sy);
            const float tx = sx - (float)ix, ty = sy - (float)iy;
            float v = 0.0f;
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    int px = ix + i < 0 ? 0 : (ix + i > w - 1 ? w - 1 : ix + i);
                    int py = iy + j < 0 ? 0 : (iy + j > h - 1 ? h - 1 : iy + j);
                    v += (float)(page[(y0 + py) * PW + x0 + px] >> 24) * (i ? tx : 1.0f - tx) * (j ? ty : 1.0f - ty);
                }
            out[Y * w * f + X] = (uint32_t)floorf(v + 0.5f) << 24;
        }
}

static void cell_rebuild(void) {
    static uint32_t page[PW * PH];
    static uint32_t tile[CELL_W * 6 * CELL_H * 6], tile2[CELL_W * 6 * CELL_H * 6], base[CELL_W * 6 * CELL_H * 6];
    for (size_t fi = 0; fi < sizeof k_fmts / sizeof k_fmts[0]; fi++) {
        const Fmt *fmt = &k_fmts[fi];
        make_page(page, fmt, 0);
        for (int g = 0; g < 4; g++)
            for (int f = NK_GLYPH_MIN_FACTOR; f <= NK_GLYPH_MAX_FACTOR; f++) {
                char what[96];
                snprintf(what, sizeof what, "%s glyph %d at factor %d", fmt->name, g, f);
                memset(tile, 0xA5, sizeof tile);
                const NkGlyphVerdict v = nk_glyph_rebuild(page, PW, PH, RX(g), RY, CELL_W, CELL_H, f, tile);
                check(v == NK_GLYPH_OK, "rebuild", "%s: %s", what, nk_glyph_verdict_name(v));
                if (v != NK_GLYPH_OK) continue;
                check(nk_glyph_verify(page, PW, PH, RX(g), RY, CELL_W, CELL_H, f, tile) == NK_GLYPH_OK, "rebuild",
                      "%s: the tile fails its own downsample check", what);
                /* every f x f block averages to the guest's texel */
                int worst = 0;
                for (int y = 0; y < CELL_H; y++)
                    for (int x = 0; x < CELL_W; x++) {
                        long sum = 0;
                        for (int j = 0; j < f; j++)
                            for (int i = 0; i < f; i++) sum += (long)(tile[(y * f + j) * CELL_W * f + x * f + i] >> 24);
                        const long want = (long)(page[(RY + y) * PW + RX(g) + x] >> 24);
                        const long d = labs(sum - want * f * f);
                        if (d * 1 > worst) worst = (int)d;
                    }
                check(worst <= NK_GLYPH_CHECK_TOLERANCE * f * f, "rebuild", "%s: block mean off by %d/%d", what, worst, f * f);
                /* every inked pixel has the glyph's colour */
                int wrong = 0;
                for (int i = 0; i < CELL_W * f * CELL_H * f; i++)
                    if ((tile[i] >> 24) && (tile[i] & 0x00ffffffu) != fmt->rgb) wrong++;
                check(wrong == 0, "rebuild", "%s: %d pixels carry another colour", what, wrong);
                /* deterministic */
                memset(tile2, 0x5A, sizeof tile2);
                (void)nk_glyph_rebuild(page, PW, PH, RX(g), RY, CELL_W, CELL_H, f, tile2);
                check(memcmp(tile, tile2, sizeof(uint32_t) * (size_t)(CELL_W * f * CELL_H * f)) == 0, "rebuild", "%s: not repeatable", what);
                /* sharper than the guest's own filter at this scale */
                if (f >= 3 && fmt->levels == 255) {
                    bilinear(page, RX(g), RY, CELL_W, CELL_H, f, base);
                    const int n = CELL_W * f * CELL_H * f;
                    const int soft = soft_pixels(tile, n), soft_base = soft_pixels(base, n);
                    check(soft * 2 <= soft_base, "sharp", "%s: %d soft pixels against %d bilinear", what, soft, soft_base);
                }
            }
    }
}

static void cell_isolation(void) {
    /* a rectangle's tile depends on its own texels only: change the neighbours, the tile stays */
    static uint32_t a[PW * PH], b[PW * PH];
    static uint32_t ta[CELL_W * 4 * CELL_H * 4], tb[CELL_W * 4 * CELL_H * 4];
    make_page(a, &k_fmts[1], 0);
    memcpy(b, a, sizeof a);
    for (int y = 0; y < PH; y++)
        for (int x = 0; x < PW; x++) {
            const int inside = x >= RX(1) && x < RX(1) + CELL_W && y >= RY && y < RY + CELL_H;
            if (!inside) b[y * PW + x] = ((uint32_t)((x * 37 + y * 11) & 0xff) << 24) | 0x00ff00ffu;
        }
    check(nk_glyph_rebuild(a, PW, PH, RX(1), RY, CELL_W, CELL_H, 4, ta) == NK_GLYPH_OK &&
              nk_glyph_rebuild(b, PW, PH, RX(1), RY, CELL_W, CELL_H, 4, tb) == NK_GLYPH_OK &&
              memcmp(ta, tb, sizeof ta) == 0,
          "isolation", "a tile changed when only the cells around it changed");
}

static void cell_verify(void) {
    static uint32_t page[PW * PH];
    static uint32_t tile[CELL_W * 4 * CELL_H * 4], bad[CELL_W * 4 * CELL_H * 4];
    const int n = CELL_W * 4 * CELL_H * 4;
    make_page(page, &k_fmts[1], 0);
    check(nk_glyph_rebuild(page, PW, PH, RX(0), RY, CELL_W, CELL_H, 4, tile) == NK_GLYPH_OK, "verify", "setup");
    check(nk_glyph_verify(page, PW, PH, RX(0), RY, CELL_W, CELL_H, 4, tile) == NK_GLYPH_OK, "verify", "an untouched tile passes");
    /* one block gone blank */
    memcpy(bad, tile, sizeof(uint32_t) * (size_t)n);
    int bx = 0, by = 0;   /* the most covered texel */
    for (int y = 0; y < CELL_H; y++)
        for (int x = 0; x < CELL_W; x++)
            if ((page[(RY + y) * PW + RX(0) + x] >> 24) > (page[(RY + by) * PW + RX(0) + bx] >> 24)) { bx = x; by = y; }
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) bad[(by * 4 + j) * CELL_W * 4 + (bx * 4 + i)] = 0;
    check(nk_glyph_verify(page, PW, PH, RX(0), RY, CELL_W, CELL_H, 4, bad) == NK_GLYPH_REFUSE_DOWNSAMPLE, "downsample", "a tampered tile (a block blanked)");
    /* ink in a blank texel */
    memcpy(bad, tile, sizeof(uint32_t) * (size_t)n);
    for (int i = 0; i < 4; i++) bad[i] = 0xff000000u | k_fmts[1].rgb;
    check(nk_glyph_verify(page, PW, PH, RX(0), RY, CELL_W, CELL_H, 4, bad) == NK_GLYPH_REFUSE_DOWNSAMPLE, "downsample", "a tampered tile (ink in a blank texel)");
    /* a tile of another glyph: stale content */
    make_page(page, &k_fmts[1], 1);
    check(nk_glyph_verify(page, PW, PH, RX(0), RY, CELL_W, CELL_H, 4, tile) == NK_GLYPH_REFUSE_DOWNSAMPLE, "downsample", "a stale tile after the page changed");
    /* a wrong colour */
    make_page(page, &k_fmts[1], 0);
    memcpy(bad, tile, sizeof(uint32_t) * (size_t)n);
    for (int i = 0; i < n; i++)
        if (bad[i] >> 24) { bad[i] = (bad[i] & 0xff000000u) | 0x00010203u; break; }
    check(nk_glyph_verify(page, PW, PH, RX(0), RY, CELL_W, CELL_H, 4, bad) == NK_GLYPH_REFUSE_DOWNSAMPLE, "downsample", "a tile pixel of another colour");
    check(nk_glyph_verify(page, PW, PH, RX(0), RY, CELL_W, CELL_H, 1, tile) == NK_GLYPH_REFUSE_DOWNSAMPLE, "downsample", "factor 1");
    /* the rebuild refuses what its classifier refuses, and leaves the tile alone */
    memset(bad, 0x77, sizeof(uint32_t) * (size_t)n);
    check(nk_glyph_rebuild(page, PW, PH, 0, 30, CELL_W, CELL_H, 4, bad) == NK_GLYPH_REFUSE_EMPTY && bad[0] == 0x77777777u,
          "rebuild-refusal", "a refused rectangle leaves the tile untouched");
    check(nk_glyph_rebuild(page, PW, PH, RX(0), RY, CELL_W, CELL_H, 1, bad) == NK_GLYPH_REFUSE_SCALE, "refuse-scale", "factor 1 in the rebuild");
}

/* ---- soft single-colour art: shadows, glows, fade bars ----------------------------------------- */

/* Sign changes of the steps along row `y` of a tile `tw` wide, ignoring flat steps. */
static int row_reversals(const uint32_t *tile, int tw, int y, int x_from, int x_to) {
    int last = 0, flips = 0;
    for (int x = x_from + 1; x < x_to; x++) {
        const int d = (int)(tile[(size_t)y * (size_t)tw + (size_t)x] >> 24) - (int)(tile[(size_t)y * (size_t)tw + (size_t)x - 1] >> 24);
        const int sgn = d > 0 ? 1 : (d < 0 ? -1 : 0);
        if (!sgn) continue;
        if (last && sgn != last) flips++;
        last = sgn;
    }
    return flips;
}

/* A one-row profile of white coverage, `n` texels, centred in a rectangle `n + 2 * pad` wide and 8 high. */
static void profile_page(uint32_t *page, int stride, const int *prof, int n, int pad) {
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < n + 2 * pad; x++) {
            const int c = x >= pad && x < pad + n ? prof[x - pad] : 0;
            page[y * stride + x] = (uint32_t)c << 24 | 0x00ffffffu;
        }
}

/* Rebuild a profile at every factor. It rises from blank and falls back to blank: one rise and one fall at most. */
static void profile_cell(const char *what, const int *prof, int n, int pad) {
    static uint32_t page[32 * 8], tile[32 * 6 * 8 * 6];
    profile_page(page, 32, prof, n, pad);
    const int w = n + 2 * pad;
    for (int f = NK_GLYPH_MIN_FACTOR; f <= NK_GLYPH_MAX_FACTOR; f++) {
        NkGlyphVerdict v = nk_glyph_rebuild(page, 32, 8, 0, 0, w, 8, f, tile);
        check(v == NK_GLYPH_OK, "gradient", "%s at factor %d: %s", what, f, nk_glyph_verdict_name(v));
        if (v != NK_GLYPH_OK) continue;
        const int flips = row_reversals(tile, w * f, 4 * f, 0, w * f);
        check(flips <= 1, "gradient", "%s at factor %d: %d reversals along a row", what, f, flips);
    }
}

static void cell_gradient(void) {
    /* A gradient of one colour is not an edge: the rebuild keeps it smooth (or refuses it), and never turns it into
     * a sawtooth that restarts at every block. A stroke that is one texel wide and half covered is an edge. */
    static uint32_t shadow[32 * 32], fade[64 * 8], steep[16 * 8], cross[16 * 16];
    static uint32_t tile[64 * 6 * 8 * 6], big[32 * 6 * 32 * 6];
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            const float dx = (float)x - 15.5f, dy = (float)y - 15.5f, r = sqrtf(dx * dx + dy * dy);
            const int a = r >= 15.0f ? 0 : (int)(255.0f * (1.0f - r / 15.0f) + 0.5f);
            shadow[y * 32 + x] = (uint32_t)a << 24;   /* black */
        }
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 64; x++) fade[y * 64 + x] = (uint32_t)(32 + x * 3) << 24 | 0x00ffffffu;   /* white, 32 to 221 */
    for (int f = NK_GLYPH_MIN_FACTOR; f <= NK_GLYPH_MAX_FACTOR; f++) {
        NkGlyphVerdict v = nk_glyph_rebuild(fade, 64, 8, 0, 0, 64, 8, f, tile);
        check(v == NK_GLYPH_OK, "gradient", "fade bar at factor %d: %s", f, nk_glyph_verdict_name(v));
        if (v == NK_GLYPH_OK) {
            const int flips = row_reversals(tile, 64 * f, 4 * f, 0, 64 * f);
            check(flips == 0, "gradient", "fade bar at factor %d: %d reversals along a row that only rises", f, flips);
        }
        v = nk_glyph_rebuild(shadow, 32, 32, 0, 0, 32, 32, f, big);
        check(v == NK_GLYPH_OK, "gradient", "soft shadow at factor %d: %s", f, nk_glyph_verdict_name(v));
        if (v == NK_GLYPH_OK) {
            const int flips = row_reversals(big, 32 * f, 16 * f, 0, 32 * f);
            check(flips <= 1, "gradient", "soft shadow at factor %d: %d reversals along a row that rises once and falls once", f, flips);
        }
    }
    /* steeper profiles, down to the width where a gradient turns into an edge */
    static const int fade4[] = {85, 170, 255, 255};                   /* 4 texels from clear to solid: steps of 0.33 */
    static const int fade6[] = {40, 80, 120, 160, 200, 240};          /* 6 texels */
    static const int glow3[] = {85, 170, 255, 170, 85};               /* a 3-texel glow around a solid texel */
    static const int glow5[] = {51, 102, 153, 204, 255, 204, 153, 102, 51};
    static const int fade2[] = {64, 191, 255, 255};                   /* a 2-texel ramp: steps of 0.25, 0.5, 0.25 */
    static const int glow2[] = {64, 191, 255, 191, 64};               /* a 2-texel glow: the same ramp each side */
    static const int glow15[] = {40, 150, 255, 150, 40};              /* a 1.5-texel glow: steps of 0.43 */
    static const int gauss[] = {8, 68, 187, 247, 247, 187, 68, 8};    /* a blurred drop shadow, sigma 0.75 */
    profile_cell("4-texel fade", fade4, 4, 2);
    profile_cell("6-texel fade", fade6, 6, 2);
    profile_cell("3-texel glow", glow3, 5, 2);
    profile_cell("5-texel glow", glow5, 9, 2);
    profile_cell("2-texel fade", fade2, 4, 2);
    profile_cell("2-texel glow", glow2, 5, 2);
    profile_cell("1.5-texel glow", glow15, 5, 2);
    profile_cell("Gaussian shadow", gauss, 8, 2);
    /* a stroke one texel wide and half covered (0 128 128 0, steps of 0.5) is an edge, not a gradient: it is rebuilt
     * and keeps its coverage (the result is a peaked hump, softer than a stroke that falls in one texel; the cell pins that it is rebuilt) */
    {
        static const int stroke[] = {128, 128};
        static uint32_t page[32 * 8], hump[32 * 6 * 8 * 6];
        profile_page(page, 32, stroke, 2, 6);
        for (int f = NK_GLYPH_MIN_FACTOR; f <= NK_GLYPH_MAX_FACTOR; f++) {
            NkGlyphVerdict v = nk_glyph_rebuild(page, 32, 8, 0, 0, 14, 8, f, hump);
            check(v == NK_GLYPH_OK, "gradient", "half-covered stroke at factor %d: %s", f, nk_glyph_verdict_name(v));
        }
    }
    /* a thick-stroke junction: a plus with a solid core four texels wide and anti-aliased sides, as a bold face has */
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            const int ax = x >= 6 && x <= 9 ? 255 : (x == 5 || x == 10 ? 128 : 0);
            const int ay = y >= 6 && y <= 9 ? 255 : (y == 5 || y == 10 ? 128 : 0);
            const int a = ax > ay ? ax : ay;
            cross[y * 16 + x] = (uint32_t)a << 24 | 0x00ffffffu;
        }
    for (int f = NK_GLYPH_MIN_FACTOR; f <= NK_GLYPH_MAX_FACTOR; f++) {
        NkGlyphVerdict v = nk_glyph_rebuild(cross, 16, 16, 0, 0, 16, 16, f, big);
        check(v == NK_GLYPH_OK, "junction", "thick-stroke plus at factor %d: %s", f, nk_glyph_verdict_name(v));
    }
    /* a bold "A" (strokes 1 to 3 texels wide, four sub-texel offsets) has junctions where a texel is flat in its 3 x 3
     * and two texels from a full step: every one of them is rebuilt, at every factor */
    {
        static uint32_t glyph[16 * 16];
        static const float seg[3][4] = {{2.0f, 14.0f, 8.0f, 2.0f}, {8.0f, 2.0f, 14.0f, 14.0f}, {4.5f, 10.0f, 11.5f, 10.0f}};
        for (int si = 0; si < 9; si++)
            for (int oi = 0; oi < 4; oi++) {
                const float sw = 1.0f + 0.25f * (float)si, ox = 0.25f * (float)(oi & 1), oy = 0.25f * (float)(oi >> 1);
                for (int y = 0; y < 16; y++)
                    for (int x = 0; x < 16; x++) {
                        int hit = 0;
                        for (int sy = 0; sy < 8; sy++)
                            for (int sx = 0; sx < 8; sx++) {
                                const float px = (float)x + ((float)sx + 0.5f) / 8.0f - ox, py = (float)y + ((float)sy + 0.5f) / 8.0f - oy;
                                for (int k = 0; k < 3; k++) {
                                    const float ax = seg[k][0], ay = seg[k][1], bx = seg[k][2], by = seg[k][3];
                                    const float vx = bx - ax, vy = by - ay;
                                    float t = ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy);
                                    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                                    const float qx = px - (ax + t * vx), qy = py - (ay + t * vy);
                                    if (qx * qx + qy * qy <= 0.25f * sw * sw) { hit++; break; }
                                }
                            }
                        glyph[y * 16 + x] = (uint32_t)((hit * 255 + 32) / 64) << 24 | 0x00ffffffu;
                    }
                for (int f = 3; f <= 4; f++) {
                    NkGlyphVerdict v = nk_glyph_rebuild(glyph, 16, 16, 0, 0, 16, 16, f, big);
                    check(v == NK_GLYPH_OK, "junction", "bold A, stroke %.2f, offset %d, factor %d: %s", (double)sw, oi, f, nk_glyph_verdict_name(v));
                }
            }
    }
    /* a thick-stroke plus with a lighter ink (237 of 255, as a 4-bit font has): still rebuilt */
    {
        static uint32_t glyph[16 * 16];
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                const int ax = x >= 6 && x <= 9 ? 237 : (x == 5 || x == 10 ? 118 : 0);
                const int ay = y >= 6 && y <= 9 ? 237 : (y == 5 || y == 10 ? 118 : 0);
                glyph[y * 16 + x] = (uint32_t)(ax > ay ? ax : ay) << 24 | 0x00ffffffu;
            }
        for (int f = NK_GLYPH_MIN_FACTOR; f <= NK_GLYPH_MAX_FACTOR; f++) {
            NkGlyphVerdict v = nk_glyph_rebuild(glyph, 16, 16, 0, 0, 16, 16, f, big);
            check(v == NK_GLYPH_OK, "junction", "thick-stroke plus at ink 237, factor %d: %s", f, nk_glyph_verdict_name(v));
        }
    }
    (void)steep;
}

/* ---- soft art in two dimensions ----------------------------------------------------------------- */

/* The deepest run-back along any row or column of a w x h tile against a unimodal profile (rises to the line's
 * peak, falls after it), in 8-bit steps. */
static int tile_runback(const uint32_t *tile, int W, int H) {
    int worst = 0;
    for (int line = 0; line < H + W; line++) {
        const int vert = line >= H, n = vert ? H : W, fixed = vert ? line - H : line;
        int pk = 0, pv = -1;
        for (int i = 0; i < n; i++) {
            const int v = (int)(tile[vert ? (size_t)i * (size_t)W + (size_t)fixed : (size_t)fixed * (size_t)W + (size_t)i] >> 24);
            if (v > pv) { pv = v; pk = i; }
        }
        int m = 0;
        for (int i = 0; i <= pk; i++) {
            const int v = (int)(tile[vert ? (size_t)i * (size_t)W + (size_t)fixed : (size_t)fixed * (size_t)W + (size_t)i] >> 24);
            if (v > m) m = v;
            if (m - v > worst) worst = m - v;
        }
        m = 0;
        for (int i = n - 1; i >= pk; i--) {
            const int v = (int)(tile[vert ? (size_t)i * (size_t)W + (size_t)fixed : (size_t)fixed * (size_t)W + (size_t)i] >> 24);
            if (v > m) m = v;
            if (m - v > worst) worst = m - v;
        }
    }
    return worst;
}

static float ramp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

/* Coverage of a single-colour shadow at (x, y): kind 0 a 14-texel box blurred by a box `p` texels wide, kind 1 a disk of
 * radius 6 with a linear falloff `p` texels wide. `q` moves it by a fraction of a texel. */
static float soft_cov(int kind, float p, float q, float x, float y) {
    x -= q;
    y -= q * 0.6f;
    if (kind == 0) {
        const float lx = ramp01((x - (9.0f - p * 0.5f)) / p), rx = ramp01(((23.0f + p * 0.5f) - x) / p);
        const float ly = ramp01((y - (9.0f - p * 0.5f)) / p), ry = ramp01(((23.0f + p * 0.5f) - y) / p);
        return (lx < rx ? lx : rx) * (ly < ry ? ly : ry);
    }
    const float r = sqrtf((x - 16.0f) * (x - 16.0f) + (y - 16.0f) * (y - 16.0f));
    return r <= 6.0f ? 1.0f : ramp01(1.0f - (r - 6.0f) / p);
}

static void cell_soft2d(void) {
    /* A shadow or a glow is a ramp more than a texel wide in two dimensions, with corners and curves along it: the
     * rectangle is rebuilt smooth, and no row or column of the tile runs back by more than the Catmull-Rom undershoot. */
    static uint32_t page[32 * 32], tile[32 * 6 * 32 * 6];
    static const float falls[] = {1.5f, 2.0f, 3.0f};
    static const float offs[] = {0.0f, 0.37f};
    for (int kind = 0; kind < 2; kind++)
        for (size_t pi = 0; pi < sizeof(falls) / sizeof(falls[0]); pi++)
            for (size_t qi = 0; qi < sizeof(offs) / sizeof(offs[0]); qi++) {
                for (int y = 0; y < 32; y++)
                    for (int x = 0; x < 32; x++) {
                        float a = 0.0f;
                        for (int j = 0; j < 8; j++)
                            for (int i = 0; i < 8; i++)
                                a += soft_cov(kind, falls[pi], offs[qi], (float)x + ((float)i + 0.5f) / 8.0f, (float)y + ((float)j + 0.5f) / 8.0f);
                        const int ai = (int)(a / 64.0f * 255.0f + 0.5f);
                        page[y * 32 + x] = ai ? ((uint32_t)ai << 24) | 0x00ffffffu : 0u;
                    }
                for (int f = NK_GLYPH_MIN_FACTOR; f <= NK_GLYPH_MAX_FACTOR; f++) {
                    const NkGlyphVerdict v = nk_glyph_rebuild(page, 32, 32, 0, 0, 32, 32, f, tile);
                    check(v == NK_GLYPH_OK, "soft", "%s, falloff %.1f, offset %.2f, factor %d: %s", kind ? "disk glow" : "box shadow",
                          (double)falls[pi], (double)offs[qi], f, nk_glyph_verdict_name(v));
                    if (v != NK_GLYPH_OK) continue;
                    const int rb = tile_runback(tile, 32 * f, 32 * f);
                    check(rb <= NK_GLYPH_SOFT_RUNBACK, "soft", "%s, falloff %.1f, offset %.2f, factor %d: a row or column runs back by %d of 255",
                          kind ? "disk glow" : "box shadow", (double)falls[pi], (double)offs[qi], f, rb);
                }
            }
}

/* ---- text stays sharp --------------------------------------------------------------------------- */

/* A box-filtered "E" with strokes sw texels wide, shifted by (ox, oy) of a texel, at the given ink: three bars and a stem. */
static float text_e_cov(float sw, float ox, float oy, int x, int y) {
    int n = 0;
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++) {
            const float px = (float)x + ((float)i + 0.5f) / 8.0f - ox, py = (float)y + ((float)j + 0.5f) / 8.0f - oy;
            const int stem = px >= 2.0f && px < 2.0f + sw && py >= 2.0f && py < 12.0f;
            const int bar = px >= 2.0f && px < 10.0f &&
                            ((py >= 2.0f && py < 2.0f + sw) || (py >= 6.0f && py < 6.0f + sw) || (py >= 12.0f - sw && py < 12.0f));
            n += stem || bar;
        }
    return (float)n / 64.0f;
}

static void cell_text_sharp(void) {
    /* Real text, which a rectangle's soft-art test must leave to the edge rebuild: an E (strokes 1.2 to 2.4 texels, sixteen
     * sub-texel offsets, ink 255 and 237, factors 3 and 4: 256 draws) has to come out at least twice as sharp as the guest's
     * bilinear picture in nearly all of them. Some are rebuilt smooth by design (strokes 1.2 texels wide that straddle two
     * texels have a ratio above 1.2: 14 of the 256 here), and one offset is a hard edge and is refused (2); a threshold of
     * 1.0 instead of 1.2 makes 102 smooth and fails the count. */
    static uint32_t page[PW * PH];
    static uint32_t tile[CELL_W * 6 * CELL_H * 6], base[CELL_W * 6 * CELL_H * 6];
    static const float widths[] = {1.2f, 1.6f, 2.0f, 2.4f};
    static const int inks[] = {255, 237};
    int sharp = 0, total = 0;
    for (size_t ii = 0; ii < sizeof inks / sizeof inks[0]; ii++)
        for (size_t wi = 0; wi < sizeof widths / sizeof widths[0]; wi++)
            for (int o = 0; o < 16; o++) {
                const float ox = 0.25f * (float)(o & 3), oy = 0.25f * (float)(o >> 2);
                for (int i = 0; i < PW * PH; i++) page[i] = 0x00123456u;
                for (int y = 0; y < CELL_H; y++)
                    for (int x = 0; x < CELL_W; x++) {
                        const int a = (int)floorf(text_e_cov(widths[wi], ox, oy, x, y) * (float)inks[ii] + 0.5f);
                        page[(4 + y) * PW + 4 + x] = a ? ((uint32_t)a << 24) | 0x00ffffffu : 0x00123456u;
                    }
                for (int f = 3; f <= 4; f++) {
                    const NkGlyphVerdict v = nk_glyph_rebuild(page, PW, PH, 4, 4, CELL_W, CELL_H, f, tile);
                    total++;
                    check(v == NK_GLYPH_OK || v == NK_GLYPH_REFUSE_HARD_EDGE, "text", "E, stroke %.1f, offset %d, ink %d, factor %d: %s",
                          (double)widths[wi], o, inks[ii], f, nk_glyph_verdict_name(v));
                    if (v != NK_GLYPH_OK) continue;
                    bilinear(page, 4, 4, CELL_W, CELL_H, f, base);
                    const int n = CELL_W * f * CELL_H * f, soft = soft_pixels(tile, n), soft_base = soft_pixels(base, n);
                    sharp += soft * 2 <= soft_base;
                }
            }
    check(sharp >= 230, "text", "only %d of %d draws of an E are at least twice as sharp as bilinear (a soft-art threshold set too low)", sharp, total);
}

int main(void) {
    cell_settings();
    cell_sources();
    cell_rects();
    cell_rebuild();
    cell_isolation();
    cell_verify();
    cell_gradient();
    cell_soft2d();
    cell_text_sharp();
    if (g_fail) {
        fprintf(stderr, "glyph-recon-selftest: %d of %d checks failed\n", g_fail, g_cells);
        return 1;
    }
    printf("glyph-recon-selftest: PASS (%d checks)\n", g_cells);
    return 0;
}
