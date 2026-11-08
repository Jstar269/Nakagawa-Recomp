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

int main(void) {
    cell_settings();
    cell_sources();
    cell_rects();
    cell_rebuild();
    cell_isolation();
    cell_verify();
    if (g_fail) {
        fprintf(stderr, "glyph-recon-selftest: %d of %d checks failed\n", g_fail, g_cells);
        return 1;
    }
    printf("glyph-recon-selftest: PASS (%d checks)\n", g_cells);
    return 0;
}
