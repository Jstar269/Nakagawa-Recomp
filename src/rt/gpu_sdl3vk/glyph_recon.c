/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * glyph_recon.c - sharp game text: the setting, the refusals, the rebuild of one glyph rectangle and
 * the check against the guest's own bytes (glyph_recon.h, #2110).
 *
 * The rebuild, for a w x h rectangle of coverage c (alpha / 255) at factor f:
 *   1. Upsample c with a Catmull-Rom bicubic to a (w f) x (h f) field h, replicating the rectangle's
 *      own edge texels past its border (a neighbour cell of the page is never read).
 *   2. For every texel, find the threshold t at which the block of f x f pixels, each set to
 *      clamp(0.5 + f * (h - t), 0, 1), averages to exactly that texel's coverage. The block mean is
 *      continuous and falls from 1 to 0 as t rises, so a bisection finds it. The ramp is about one
 *      device pixel wide at any factor, so the edge is sharp and still anti-aliased at the render
 *      resolution, and every block keeps the guest's coverage by construction.
 *   3. Colour every pixel with the one colour the rectangle's inked texels share, so a pixel the
 *      blur reaches into a blank texel is not darkened by that texel's stray colour.
 * Everything is float arithmetic in a fixed order; nk_glyph_verify then checks the quantized result
 * against the input in integers, so a wrong or stale tile cannot be shown.
 */
#include "glyph_recon.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the setting ---------------------------------------------------------------------------- */

static NkGlyphConfig s_cfg;
static char s_start_line[384];   /* what the last nk_glyph_init_from_env printed */

static void set_why(char *why, size_t cap, const char *text) {
    if (why && cap) snprintf(why, cap, "%s", text);
}

int nk_glyph_parse(const char *glyphs, NkGlyphConfig *out, char *why, size_t why_cap) {
    memset(out, 0, sizeof(*out));
    if (!glyphs || !glyphs[0] || strcmp(glyphs, "off") == 0) return 1;
    if (strcmp(glyphs, "on") != 0) {
        set_why(why, why_cap, "SR_GLYPHS takes off or on");
        return 0;
    }
    out->on = 1;
    return 1;
}

void nk_glyph_configure(const NkGlyphConfig *cfg) {
    s_cfg = cfg ? *cfg : (NkGlyphConfig){ 0 };
}

const NkGlyphConfig *nk_glyph_config(void) { return &s_cfg; }

size_t nk_glyph_admission_line(const NkGlyphConfig *cfg, char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!cfg || !cfg->on) return 0;
    const int n = snprintf(out, cap,
                           "GLYPHS: guest-visible, on through SR_GLYPHS and not admitted under contract section "
                           "11.2 (each present commits the frame to guest VRAM; see the contract's \"Sharp Game "
                           "Text\" entry)");
    return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}

static void start_line(const char *text) {
    const size_t used = strlen(s_start_line);
    if (used + 1 < sizeof(s_start_line)) snprintf(s_start_line + used, sizeof(s_start_line) - used, "%s\n", text);
    fprintf(stderr, "%s\n", text);
}

const char *nk_glyph_start_lines(void) { return s_start_line; }

const NkGlyphConfig *nk_glyph_init_from_env(int enhanced) {
    const char *glyphs = getenv("SR_GLYPHS");
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_start_line[0] = '\0';
    const int asked = glyphs && glyphs[0] && strcmp(glyphs, "off") != 0;
#ifdef NK_GLYPH_MUTANT_NO_ACE_GATE
    enhanced = 1;   /* glyph-recon-mutant: the Ace gate removed; the settings cell must fail */
#endif
    if (!enhanced) {
        /* Authentic: nothing is read further, and a request is named, not obeyed. */
        if (asked) start_line("GLYPHS: off (sharp game text needs Ace Mode)");
        return &s_cfg;
    }
    char why[96], line[320];
    NkGlyphConfig c;
    if (!nk_glyph_parse(glyphs, &c, why, sizeof(why))) {
        snprintf(line, sizeof(line), "GLYPHS: refused (%s)", why);
        start_line(line);
        return &s_cfg;
    }
    s_cfg = c;
    if (c.on) {
        snprintf(line, sizeof(line), "GLYPHS: on (per_present=%d, tolerance=%d)", NK_GLYPH_PER_PRESENT,
                 NK_GLYPH_CHECK_TOLERANCE);
        start_line(line);
        if (nk_glyph_admission_line(&c, line, sizeof(line))) start_line(line);
    }
    return &s_cfg;
}

/* ---- refusals -------------------------------------------------------------------------------- */

const char *nk_glyph_verdict_name(NkGlyphVerdict v) {
    static const char *const names[NK_GLYPH_VERDICT_COUNT] = {
        "ok", "vram", "format", "reversed", "rgb24", "mip", "scale", "size", "empty", "colour",
        "hard_edge", "downsample", "memory",
    };
    return (unsigned)v < NK_GLYPH_VERDICT_COUNT ? names[v] : "unknown";
}

NkGlyphVerdict nk_glyph_source_check(const NkGlyphSource *src) {
    if (!src) return NK_GLYPH_REFUSE_FORMAT;
#ifndef NK_GLYPH_MUTANT_ALLOW_VRAM
    if (src->in_vram) return NK_GLYPH_REFUSE_VRAM;
#endif
#ifndef NK_GLYPH_MUTANT_ALLOW_BUFFERS
    if (src->format == NK_GLYPH_FMT_T4_REVERSED) return NK_GLYPH_REFUSE_REVERSED;
    if (src->format == NK_GLYPH_FMT_RGB24) return NK_GLYPH_REFUSE_RGB24;
#endif
    switch (src->format) {
    case NK_GLYPH_FMT_4444:
    case NK_GLYPH_FMT_8888:
    case NK_GLYPH_FMT_T4:
    case NK_GLYPH_FMT_T8:
#ifdef NK_GLYPH_MUTANT_ALLOW_BUFFERS
    case NK_GLYPH_FMT_T4_REVERSED:
    case NK_GLYPH_FMT_RGB24:
#endif
        break;
    default:
        return NK_GLYPH_REFUSE_FORMAT;
    }
#ifndef NK_GLYPH_MUTANT_ALLOW_MIP
    if (src->mip_levels != 1) return NK_GLYPH_REFUSE_MIP;
#endif
#ifndef NK_GLYPH_MUTANT_ALLOW_SCALE1
    if (src->render_scale < NK_GLYPH_MIN_FACTOR || src->render_scale > NK_GLYPH_MAX_FACTOR)
        return NK_GLYPH_REFUSE_SCALE;
#else
    if (src->render_scale < 1 || src->render_scale > NK_GLYPH_MAX_FACTOR) return NK_GLYPH_REFUSE_SCALE;
#endif
    return NK_GLYPH_OK;
}

static int alpha_of(uint32_t px) { return (int)(px >> 24); }

NkGlyphVerdict nk_glyph_rect_check(const uint32_t *page, int pw, int ph, int x0, int y0, int w, int h,
                                   uint32_t *colour) {
    if (!page || pw <= 0 || ph <= 0) return NK_GLYPH_REFUSE_SIZE;
#ifndef NK_GLYPH_MUTANT_NO_SIZE_CHECK
    if (w < NK_GLYPH_MIN_SIDE || h < NK_GLYPH_MIN_SIDE || w > NK_GLYPH_MAX_SIDE || h > NK_GLYPH_MAX_SIDE)
        return NK_GLYPH_REFUSE_SIZE;
#endif
    if (w <= 0 || h <= 0 || x0 < 0 || y0 < 0 || x0 > pw - w || y0 > ph - h) return NK_GLYPH_REFUSE_SIZE;
    int inked = 0, partial = 0, differs = 0;
    uint32_t shared = 0;
    for (int y = 0; y < h; y++) {
        const uint32_t *row = page + (size_t)(y0 + y) * (size_t)pw + (size_t)x0;
        for (int x = 0; x < w; x++) {
            const int a = alpha_of(row[x]);
            if (a == 0) continue;
            const uint32_t rgb = row[x] & 0x00ffffffu;
            if (!inked) shared = rgb;
            else if (rgb != shared) differs = 1;
            inked++;
            if (a != 255) partial++;
        }
    }
    if (!inked) return NK_GLYPH_REFUSE_EMPTY;
#ifndef NK_GLYPH_MUTANT_NO_COLOUR_CHECK
    if (differs) return NK_GLYPH_REFUSE_COLOUR;
#else
    (void)differs;
#endif
#ifndef NK_GLYPH_MUTANT_NO_HARD_EDGE_CHECK
    if (partial < NK_GLYPH_MIN_PARTIAL || partial * NK_GLYPH_PARTIAL_DIV < inked) return NK_GLYPH_REFUSE_HARD_EDGE;
#endif
    if (colour) *colour = shared;
    return NK_GLYPH_OK;
}

/* The range of coverage over the 3 x 3 texels around texel (x, y) of the w x h rectangle at (x0, y0), the
 * rectangle's own edge replicated past its border. 0 is flat, 1 a full step. */
static float local_range(const uint32_t *page, int pw, int x0, int y0, int w, int h, int x, int y) {
    int lo = 255, hi = 0;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            const int xx = x + i < 0 ? 0 : (x + i > w - 1 ? w - 1 : x + i);
            const int yy = y + j < 0 ? 0 : (y + j > h - 1 ? h - 1 : y + j);
            const int a = alpha_of(page[(size_t)(y0 + yy) * (size_t)pw + (size_t)(x0 + xx)]);
            if (a < lo) lo = a;
            if (a > hi) hi = a;
        }
    return (float)(hi - lo) * (1.0f / 255.0f);
}

NkGlyphVerdict nk_glyph_verify(const uint32_t *page, int pw, int ph, int x0, int y0, int w, int h,
                               int factor, const uint32_t *tile) {
#ifdef NK_GLYPH_MUTANT_NO_DOWNSAMPLE_CHECK
    (void)page; (void)pw; (void)ph; (void)x0; (void)y0; (void)w; (void)h; (void)factor; (void)tile;
    return NK_GLYPH_OK;   /* glyph-recon-mutant: whatever was built is accepted */
#else
    uint32_t colour = 0;
    if (!page || !tile || factor < NK_GLYPH_MIN_FACTOR || factor > NK_GLYPH_MAX_FACTOR)
        return NK_GLYPH_REFUSE_DOWNSAMPLE;
    if (nk_glyph_rect_check(page, pw, ph, x0, y0, w, h, &colour) != NK_GLYPH_OK) return NK_GLYPH_REFUSE_DOWNSAMPLE;
    const int tw = w * factor;
    const int blockn = factor * factor;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            long sum = 0;
            for (int j = 0; j < factor; j++) {
                const uint32_t *row = tile + (size_t)(y * factor + j) * (size_t)tw + (size_t)(x * factor);
                for (int i = 0; i < factor; i++) {
                    const uint32_t px = row[i];
                    if (alpha_of(px) && (px & 0x00ffffffu) != colour) return NK_GLYPH_REFUSE_DOWNSAMPLE;
                    sum += alpha_of(px);
                }
            }
            const long want = (long)alpha_of(page[(size_t)(y0 + y) * (size_t)pw + (size_t)(x0 + x)]) * blockn;
            const long diff = sum > want ? sum - want : want - sum;
            if (diff > (long)NK_GLYPH_CHECK_TOLERANCE * blockn) return NK_GLYPH_REFUSE_DOWNSAMPLE;
#ifndef NK_GLYPH_MUTANT_NO_RIPPLE_CHECK
            /* Where the guest's coverage varies slowly the block must too: a smooth block spans about half the range of
             * the texels around it, and one that spans more is a ramp that starts over at the next block (a ribbed
             * gradient). */
            const float r = local_range(page, pw, x0, y0, w, h, x, y);
            if (r < NK_GLYPH_EDGE_LOW) {
                int lo = 255, hi = 0;
                for (int j = 0; j < factor; j++) {
                    const uint32_t *row = tile + (size_t)(y * factor + j) * (size_t)tw + (size_t)(x * factor);
                    for (int i = 0; i < factor; i++) {
                        const int a = alpha_of(row[i]);
                        if (a < lo) lo = a;
                        if (a > hi) hi = a;
                    }
                }
                if ((float)(hi - lo) > r * 255.0f * NK_GLYPH_RIPPLE_SHARE + (float)NK_GLYPH_RIPPLE_SLACK)
                    return NK_GLYPH_REFUSE_DOWNSAMPLE;
            }
#endif
        }
    }
    return NK_GLYPH_OK;
#endif
}

/* ---- the rebuild ------------------------------------------------------------------------------ */

/* Catmull-Rom weights for the four taps around fractional position t. */
static void cubic_weights(float t, float w[4]) {
    const float t2 = t * t, t3 = t2 * t;
    w[0] = -0.5f * t3 + t2 - 0.5f * t;
    w[1] = 1.5f * t3 - 2.5f * t2 + 1.0f;
    w[2] = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
    w[3] = 0.5f * t3 - 0.5f * t2;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* The bicubic field of a w x h coverage plane at factor f, into hi (w f x h f). */
static int upsample(const float *cov, int w, int h, int f, float *hi) {
    const int W = w * f, H = h * f;
    float *tmp = (float *)malloc(sizeof(float) * (size_t)W * (size_t)h);
    if (!tmp) return 0;
    for (int X = 0; X < W; X++) {
        const float sx = ((float)X + 0.5f) / (float)f - 0.5f;
        const int ix = (int)floorf(sx);
        float wt[4];
        cubic_weights(sx - (float)ix, wt);
        for (int y = 0; y < h; y++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) s += wt[k] * cov[(size_t)y * (size_t)w + (size_t)clampi(ix - 1 + k, 0, w - 1)];
            tmp[(size_t)y * (size_t)W + (size_t)X] = s;
        }
    }
    for (int Y = 0; Y < H; Y++) {
        const float sy = ((float)Y + 0.5f) / (float)f - 0.5f;
        const int iy = (int)floorf(sy);
        float wt[4];
        cubic_weights(sy - (float)iy, wt);
        for (int X = 0; X < W; X++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) s += wt[k] * tmp[(size_t)clampi(iy - 1 + k, 0, h - 1) * (size_t)W + (size_t)X];
            hi[(size_t)Y * (size_t)W + (size_t)X] = clampf(s, 0.0f, 1.0f);
        }
    }
    free(tmp);
    return 1;
}

/* The mean of a block of ramp values at threshold t. */
static float block_mean(const float *v, int n, float slope, float t) {
    float s = 0.0f;
    for (int i = 0; i < n; i++) s += clampf(0.5f + slope * (v[i] - t), 0.0f, 1.0f);
    return s / (float)n;
}

/* 0 below NK_GLYPH_EDGE_LOW, 1 above NK_GLYPH_EDGE_HIGH, smooth between. */
static float edge_weight(float range) {
    const float t = clampf((range - NK_GLYPH_EDGE_LOW) / (NK_GLYPH_EDGE_HIGH - NK_GLYPH_EDGE_LOW), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

NkGlyphVerdict nk_glyph_rebuild(const uint32_t *page, int pw, int ph, int x0, int y0, int w, int h,
                                int factor, uint32_t *tile) {
    uint32_t colour = 0;
    if (factor < NK_GLYPH_MIN_FACTOR || factor > NK_GLYPH_MAX_FACTOR) return NK_GLYPH_REFUSE_SCALE;
    NkGlyphVerdict v = nk_glyph_rect_check(page, pw, ph, x0, y0, w, h, &colour);
    if (v != NK_GLYPH_OK) return v;
    const int W = w * factor, H = h * factor, blockn = factor * factor;
    float *cov = (float *)malloc(sizeof(float) * (size_t)w * (size_t)h);
    float *hi = (float *)malloc(sizeof(float) * (size_t)W * (size_t)H);
    uint32_t *out = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)W * (size_t)H);
    float *blk = (float *)malloc(sizeof(float) * (size_t)blockn);
    if (!cov || !hi || !out || !blk) {
        free(cov); free(hi); free(out); free(blk);
        return NK_GLYPH_REFUSE_MEMORY;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            cov[(size_t)y * (size_t)w + (size_t)x] =
                (float)alpha_of(page[(size_t)(y0 + y) * (size_t)pw + (size_t)(x0 + x)]) * (1.0f / 255.0f);
    if (!upsample(cov, w, h, factor, hi)) {
        free(cov); free(hi); free(out); free(blk);
        return NK_GLYPH_REFUSE_MEMORY;
    }
    const float slope = (float)factor;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const float c = cov[(size_t)y * (size_t)w + (size_t)x];
            for (int j = 0; j < factor; j++)
                for (int i = 0; i < factor; i++)
                    blk[j * factor + i] = hi[(size_t)(y * factor + j) * (size_t)W + (size_t)(x * factor + i)];
            float t = 0.0f, off = 0.0f;
            /* How much of the block is an edge: a step of coverage within the texels around it sharpens, a slow
             * change does not (a steep ramp over a shallow gradient restarts at every block and ribs it). */
#ifdef NK_GLYPH_MUTANT_NO_SMOOTH_MODE
            const float wedge = 1.0f;   /* glyph-recon-mutant: every block steepened */
#else
            const float wedge = edge_weight(local_range(page, pw, x0, y0, w, h, x, y));
#endif
#ifdef NK_GLYPH_MUTANT_PLAIN_BICUBIC
            /* glyph-recon-mutant: the bicubic field itself, with no threshold fitted to the texel */
            for (int k = 0; k < blockn; k++) blk[k] = clampf(blk[k], 0.0f, 1.0f);
#else
            if (c > 0.0f && c < 1.0f) {
                if (wedge > 0.0f) {
                    float lo = -1.0f, up = 2.0f;   /* mean(lo) = 1, mean(up) = 0 for slope >= 1 */
                    for (int it = 0; it < 24; it++) {
                        t = 0.5f * (lo + up);
                        if (block_mean(blk, blockn, slope, t) > c) lo = t; else up = t;
                    }
                    t = 0.5f * (lo + up);
                }
                if (wedge < 1.0f) {
                    float lo = -1.0f, up = 1.0f;   /* mean of the field shifted by off, clamped, rises with off */
                    for (int it = 0; it < 24; it++) {
                        off = 0.5f * (lo + up);
                        if (block_mean(blk, blockn, 1.0f, 0.5f - off) < c) lo = off; else up = off;
                    }
                    off = 0.5f * (lo + up);
                }
            } else {
                t = c <= 0.0f ? 2.0f : -1.0f;
                off = c <= 0.0f ? -1.0f : 1.0f;
            }
#endif
            for (int j = 0; j < factor; j++) {
                for (int i = 0; i < factor; i++) {
#ifdef NK_GLYPH_MUTANT_PLAIN_BICUBIC
                    const float val = blk[j * factor + i];
#else
                    const float field = blk[j * factor + i];
                    const float sharp = clampf(0.5f + slope * (field - t), 0.0f, 1.0f);
                    const float soft = clampf(field + off, 0.0f, 1.0f);
                    const float val = wedge * sharp + (1.0f - wedge) * soft;
#endif
                    const int a = (int)floorf(val * 255.0f + 0.5f);
                    out[(size_t)(y * factor + j) * (size_t)W + (size_t)(x * factor + i)] =
                        a ? ((uint32_t)a << 24) | colour : 0u;
                }
            }
        }
    }
    v = nk_glyph_verify(page, pw, ph, x0, y0, w, h, factor, out);
    if (v == NK_GLYPH_OK) memcpy(tile, out, sizeof(uint32_t) * (size_t)W * (size_t)H);
    (void)H;
    free(cov); free(hi); free(out); free(blk);
    return v;
}
