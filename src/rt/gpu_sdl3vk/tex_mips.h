/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * tex_mips.h - host-built mip chains for textures the game gives one level (the Enhanced part
 * "Smooth far textures", SR_ACE key mips=on; docs/ENHANCEMENT_CONTRACT.md 10.16 and
 * 11.3.8). Pure C, header only: ge_gpu.c includes it for the renderer, and
 * tex_mips_selftest.c for the CPU checks. No GPU, no guest memory, no allocation.
 *
 * Input is level 0 as ge.c decodes it (ge_decode_tex_rgba: RGBA8, red in the low byte, after the
 * palette lookup). Each further level halves each side that is above 1 (the GE's sizes are
 * powers of two) down to 1x1, so a 128x128 texture gets levels 1 to 7.
 *
 * - Box filter in linear light. Each output texel is the mean of its 2x2 (or 2x1, 1x2) block,
 *   with the colour channels decoded through the sRGB curve (IEC 61966-2-1) before averaging and
 *   encoded back after, so a fine pattern keeps the brightness it shows at level 0 from a
 *   distance. Alpha is coverage and is averaged as it is.
 * - Alpha-weighted colour. A block's colour is the mean of its texels' colours weighted by their
 *   alpha, so a texel with alpha 0 (a colour key: the palette entry a game marks transparent)
 *   adds nothing to its neighbours. A block whose texels all have alpha 0 takes their plain mean.
 * - No key colour at an edge. A texel of a level above 0 that ends with alpha 0 takes the colour
 *   of the nearest texel of that level that has alpha (nk_tex_mips_fill_keyed), so a filter
 *   footprint that reaches past an edge, bilinear, trilinear or anisotropic, blends the edge's
 *   own colour, never the key's.
 * - Each level is made from the one above it in float, so the weighted means compose exactly
 *   (the mean of the block means is the mean over the larger block) and only the stored texels
 *   are rounded to 8 bits.
 *
 * Level 0 is never changed: a draw that magnifies or samples level 0 shows the game's texels
 * as before. Whether a draw may use the chain at all is ge_gpu.c's decision (tex_hm_applies).
 */
#ifndef NK_TEX_MIPS_H
#define NK_TEX_MIPS_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#define NK_TEX_MIPS_MAX_DIM 512u      /* the GE's largest texture side */
#define NK_TEX_MIPS_MAX_LEVELS 10u    /* 512 down to 1: levels 0..9 */

/* The levels of a full chain for a w x h texture, level 0 included: 1 + log2 of the longer side.
 * 0 for a size outside 1..NK_TEX_MIPS_MAX_DIM or not a power of two. */
static inline uint32_t nk_tex_mips_levels(uint32_t w, uint32_t h) {
    if (!w || !h || w > NK_TEX_MIPS_MAX_DIM || h > NK_TEX_MIPS_MAX_DIM || (w & (w - 1u)) || (h & (h - 1u)))
        return 0u;
    uint32_t n = 1u, m = w > h ? w : h;
    while (m > 1u) { m >>= 1; n++; }
    return n;
}

static inline uint32_t nk_tex_mips_dim(uint32_t d, uint32_t level) {
    d >>= level;
    return d ? d : 1u;
}

/* Texels in levels 1..levels-1 of a w x h chain (what the output buffer holds). */
static inline size_t nk_tex_mips_chain_texels(uint32_t w, uint32_t h) {
    const uint32_t n = nk_tex_mips_levels(w, h);
    size_t t = 0;
    for (uint32_t l = 1; l < n; l++) t += (size_t)nk_tex_mips_dim(w, l) * nk_tex_mips_dim(h, l);
    return t;
}

/* Scratch the builder needs: two float levels of 4 channels and one distance per texel, the
 * largest level being level 1. */
static inline size_t nk_tex_mips_work_floats(uint32_t w, uint32_t h) {
    return 9u * (size_t)nk_tex_mips_dim(w, 1u) * nk_tex_mips_dim(h, 1u);
}

/* The sRGB curve as 256 decoded values, and the 255 midpoints between neighbours that encoding
 * compares with, so encode(decode(v)) == v for every byte. `guess` is the encoded byte at each
 * of NK_TEX_MIPS_GUESS + 1 even steps of 0..1, where encoding starts its walk. */
#define NK_TEX_MIPS_GUESS 4096u
typedef struct {
    float lin[256];
    float mid[255];
    uint8_t guess[NK_TEX_MIPS_GUESS + 1u];
} NkTexMipsCurve;

/* The count of midpoints below x: the byte whose decoded value is nearest x. */
static inline uint32_t nk_tex_mips_encode_search(const NkTexMipsCurve *c, float x) {
    uint32_t lo = 0u, hi = 255u;
    while (lo < hi) {
        const uint32_t m = (lo + hi) >> 1;
        if (c->mid[m] < x) lo = m + 1u; else hi = m;
    }
    return lo;
}

static inline void nk_tex_mips_curve_init(NkTexMipsCurve *c) {
    for (int v = 0; v < 256; v++) {
        const double s = v / 255.0;
        c->lin[v] = (float)(s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4));
    }
    for (int v = 0; v < 255; v++) c->mid[v] = 0.5f * (c->lin[v] + c->lin[v + 1]);
    for (uint32_t i = 0; i <= NK_TEX_MIPS_GUESS; i++)
        c->guess[i] = (uint8_t)nk_tex_mips_encode_search(c, (float)i / (float)NK_TEX_MIPS_GUESS);
}

/* The byte whose decoded value is nearest x (0..1): the same count of midpoints below x as the
 * search gives, found by walking from the guess, a step or two at most. */
static inline uint32_t nk_tex_mips_encode(const NkTexMipsCurve *c, float x) {
    const float q = x <= 0.0f ? 0.0f : x >= 1.0f ? (float)NK_TEX_MIPS_GUESS : x * (float)NK_TEX_MIPS_GUESS;
    uint32_t v = c->guess[(uint32_t)q];
    while (v < 255u && c->mid[v] < x) v++;
    while (v > 0u && !(c->mid[v - 1u] < x)) v--;
    return v;
}

static inline uint32_t nk_tex_mips_alpha_byte(float a) {
    const float v = a * 255.0f + 0.5f;
    return v <= 0.0f ? 0u : v >= 255.0f ? 255u : (uint32_t)v;
}

/* The mean of `n` texels (4 floats each, consecutive): alpha-weighted colour, or the plain mean
 * when every alpha is 0, and the mean alpha. */
static inline void nk_tex_mips_mean(const float *t, uint32_t n, float *o) {
    float wr = 0, wg = 0, wb = 0, a = 0, pr = 0, pg = 0, pb = 0;
    for (uint32_t k = 0; k < n; k++, t += 4) {
        wr += t[0] * t[3]; wg += t[1] * t[3]; wb += t[2] * t[3]; a += t[3];
        pr += t[0]; pg += t[1]; pb += t[2];
    }
    if (a > 0.0f) { o[0] = wr / a; o[1] = wg / a; o[2] = wb / a; }
    else { o[0] = pr / (float)n; o[1] = pg / (float)n; o[2] = pb / (float)n; }
    o[3] = a / (float)n;
}

/* Every texel of a level with alpha 0 takes the colour of the nearest texel that has alpha, by
 * the 8-neighbour chamfer distance (1 along an axis, sqrt 2 on a diagonal) in two raster passes
 * (Rosenfeld and Pfaltz), so any filter footprint that reaches it, a wide anisotropic one
 * included, blends that colour and never the key's. It has no weight of its own, so the next
 * level's means do not change. Distance does not wrap: a repeating texture takes the nearest
 * such texel inside its own square. With no texel with alpha the level is left as it is.
 * `dist` holds w * h floats. */
static inline void nk_tex_mips_fill_keyed(float *lvl, uint32_t w, uint32_t h, float *dist) {
    static const int fx[4] = { -1, -1, 0, 1 }, fy[4] = { 0, -1, -1, -1 };
    static const float fc[4] = { 1.0f, 1.41421356f, 1.0f, 1.41421356f };
    const float far = 1e30f;
    int any = 0, holes = 0;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const int known = lvl[4u * i + 3] > 0.0f;
        dist[i] = known ? 0.0f : far;
        any |= known; holes |= !known;
    }
    if (!any || !holes) return;
    for (int pass = 0; pass < 2; pass++) {
        const int s = pass ? -1 : 1;
        for (uint32_t yy = 0; yy < h; yy++) {
            const int y = pass ? (int)(h - 1u - yy) : (int)yy;
            for (uint32_t xx = 0; xx < w; xx++) {
                const int x = pass ? (int)(w - 1u - xx) : (int)xx;
                const size_t p = (size_t)y * w + (uint32_t)x;
                if (dist[p] == 0.0f) continue;
                for (int k = 0; k < 4; k++) {
                    const int nx = x + s * fx[k], ny = y + s * fy[k];
                    if (nx < 0 || ny < 0 || nx >= (int)w || ny >= (int)h) continue;
                    const size_t q = (size_t)ny * w + (uint32_t)nx;
                    if (dist[q] + fc[k] < dist[p]) {
                        dist[p] = dist[q] + fc[k];
                        lvl[4u * p + 0] = lvl[4u * q + 0];
                        lvl[4u * p + 1] = lvl[4u * q + 1];
                        lvl[4u * p + 2] = lvl[4u * q + 2];
                    }
                }
            }
        }
    }
}

static inline void nk_tex_mips_store(const NkTexMipsCurve *c, const float *lvl, size_t texels,
                                     uint32_t *out) {
    for (size_t i = 0; i < texels; i++) {
        const float *t = lvl + 4u * i;
        out[i] = nk_tex_mips_encode(c, t[0]) | (nk_tex_mips_encode(c, t[1]) << 8) |
                 (nk_tex_mips_encode(c, t[2]) << 16) | (nk_tex_mips_alpha_byte(t[3]) << 24);
    }
}

/* Builds levels 1..nk_tex_mips_levels(w, h)-1 of `level0` (w x h RGBA8) into `out`, level after
 * level (nk_tex_mips_chain_texels texels), using `work` (nk_tex_mips_work_floats floats).
 * Returns the levels written, 0 for a size it does not take. *has_alpha (when given) says
 * whether any level-0 texel has alpha below 255. */
static inline uint32_t nk_tex_mips_build(const NkTexMipsCurve *c, const uint32_t *level0, uint32_t w,
                                         uint32_t h, float *work, uint32_t *out, int *has_alpha) {
    const uint32_t n = nk_tex_mips_levels(w, h);
    if (n < 2u || !c || !level0 || !work || !out) return 0u;
    const size_t l1 = (size_t)nk_tex_mips_dim(w, 1u) * nk_tex_mips_dim(h, 1u);
    float *cur = work, *next = work + 4u * l1, *dist = work + 8u * l1;
    int any_alpha = 0;
    uint32_t sw = w, sh = h;
    size_t off = 0;
    for (uint32_t l = 1; l < n; l++) {
        const uint32_t dw = nk_tex_mips_dim(w, l), dh = nk_tex_mips_dim(h, l);
        const uint32_t bx = sw > 1u ? 2u : 1u, by = sh > 1u ? 2u : 1u;
        float *dst = l == 1u ? cur : next;
        for (uint32_t y = 0; y < dh; y++) {
            for (uint32_t x = 0; x < dw; x++) {
                float blk[4u * 4u];
                uint32_t k = 0;
                for (uint32_t j = 0; j < by; j++) {
                    for (uint32_t i = 0; i < bx; i++, k++) {
                        const size_t at = (size_t)(y * by + j) * sw + (x * bx + i);
                        if (l == 1u) {          /* level 0 straight from the bytes */
                            const uint32_t p = level0[at];
                            blk[4u * k + 0] = c->lin[p & 0xFFu];
                            blk[4u * k + 1] = c->lin[(p >> 8) & 0xFFu];
                            blk[4u * k + 2] = c->lin[(p >> 16) & 0xFFu];
                            blk[4u * k + 3] = (float)(p >> 24) / 255.0f;
                            if ((p >> 24) != 0xFFu) any_alpha = 1;
                        } else {
                            const float *t = cur + 4u * at;
                            blk[4u * k + 0] = t[0]; blk[4u * k + 1] = t[1];
                            blk[4u * k + 2] = t[2]; blk[4u * k + 3] = t[3];
                        }
                    }
                }
                nk_tex_mips_mean(blk, k, dst + 4u * ((size_t)y * dw + x));
            }
        }
#ifndef NK_TEX_MIPS_MUTANT_NO_FILL   /* tex-mips-mutant: the key colour stays in the keyed texels */
        nk_tex_mips_fill_keyed(dst, dw, dh, dist);
#else
        (void)dist;
#endif
        nk_tex_mips_store(c, dst, (size_t)dw * dh, out + off);
        off += (size_t)dw * dh;
        if (l > 1u) { float *t = cur; cur = next; next = t; }
        sw = dw; sh = dh;
    }
    if (has_alpha) *has_alpha = any_alpha;
    return n - 1u;
}

#endif /* NK_TEX_MIPS_H */
