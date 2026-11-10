/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * tex_mips_selftest.c - the CPU half of host-built mip chains (tex_mips.h): the level count, the
 * linear-light box filter, alpha weighting, the colour-key fill, and a texture with no alpha at
 * all. No GPU; gpu-mips-selftest runs the renderer's side on a Vulkan device.
 *
 * Built with -DNK_TEX_MIPS_MUTANT_NO_FILL (tex-mips-mutant), the key-edge cell must fail: the
 * key colour then reaches every level's edge.
 */
#include "tex_mips.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static NkTexMipsCurve s_curve;

static uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return r | (g << 8) | (b << 16) | (a << 24);
}

/* Builds the chain of `tex`; returns the output (caller frees) or NULL. */
static uint32_t *build(const uint32_t *tex, uint32_t w, uint32_t h, int *has_alpha, uint32_t *levels) {
    const size_t texels = nk_tex_mips_chain_texels(w, h);
    float *work = (float *)malloc(nk_tex_mips_work_floats(w, h) * sizeof(float));
    uint32_t *out = (uint32_t *)malloc((texels ? texels : 1u) * sizeof(uint32_t));
    if (!work || !out) { free(work); free(out); return NULL; }
    *levels = nk_tex_mips_build(&s_curve, tex, w, h, work, out, has_alpha);
    free(work);
    return out;
}

static int check_levels(void) {
    static const struct { uint32_t w, h, n; } cells[] = {
        { 128, 128, 8 }, { 64, 32, 7 }, { 1, 1, 1 }, { 512, 512, 10 }, { 1, 8, 4 },
        { 3, 4, 0 }, { 1024, 1, 0 }, { 0, 4, 0 },
    };
    int ok = 1;
    for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
        const uint32_t n = nk_tex_mips_levels(cells[i].w, cells[i].h);
        if (n != cells[i].n) {
            fprintf(stderr, "tex mips selftest: FAIL levels(%ux%u)=%u, want %u\n", cells[i].w,
                    cells[i].h, n, cells[i].n);
            ok = 0;
        }
    }
    /* 64x32: 32x16 + 16x8 + 8x4 + 4x2 + 2x1 + 1x1 */
    if (nk_tex_mips_chain_texels(64, 32) != 512u + 128u + 32u + 8u + 2u + 1u) {
        fprintf(stderr, "tex mips selftest: FAIL chain texels of 64x32 = %zu\n", nk_tex_mips_chain_texels(64, 32));
        ok = 0;
    }
    for (int v = 0; ok && v < 256; v++) {
        if (nk_tex_mips_encode(&s_curve, s_curve.lin[v]) != (uint32_t)v) {
            fprintf(stderr, "tex mips selftest: FAIL encode(decode(%d)) = %u\n", v,
                    nk_tex_mips_encode(&s_curve, s_curve.lin[v]));
            ok = 0;
        }
    }
    /* The walk from the guess gives the search's byte for every value: each midpoint, a float
       either side of it, and a sweep of 0..1 (with the ends just past it). */
    for (int v = 0; ok && v < 255; v++) {
        const float m = s_curve.mid[v];
        const float probe[3] = { m, nextafterf(m, 0.0f), nextafterf(m, 2.0f) };
        for (int k = 0; ok && k < 3; k++)
            if (nk_tex_mips_encode(&s_curve, probe[k]) != nk_tex_mips_encode_search(&s_curve, probe[k])) {
                fprintf(stderr, "tex mips selftest: FAIL encode(%.9g) = %u, the search gives %u\n", (double)probe[k],
                        nk_tex_mips_encode(&s_curve, probe[k]), nk_tex_mips_encode_search(&s_curve, probe[k]));
                ok = 0;
            }
    }
    for (int i = -2; ok && i <= 1000002; i++) {
        const float x = (float)i / 1000000.0f;
        if (nk_tex_mips_encode(&s_curve, x) != nk_tex_mips_encode_search(&s_curve, x)) {
            fprintf(stderr, "tex mips selftest: FAIL encode(%.9g) = %u, the search gives %u\n", (double)x,
                    nk_tex_mips_encode(&s_curve, x), nk_tex_mips_encode_search(&s_curve, x));
            ok = 0;
        }
    }
    if (ok) printf("tex mips selftest: PASS levels, chain size, sRGB round trip, encode equals the search\n");
    return ok;
}

/* A black and white checkerboard keeps its brightness: every level is the linear-light mean,
 * sRGB 188 (0.5 linear), not the 128 a mean of the encoded bytes gives. */
static int check_linear_light(void) {
    uint32_t tex[8 * 8];
    for (uint32_t y = 0; y < 8; y++)
        for (uint32_t x = 0; x < 8; x++) tex[y * 8 + x] = ((x ^ y) & 1u) ? rgba(255, 255, 255, 255) : rgba(0, 0, 0, 255);
    int has_alpha = -1;
    uint32_t levels = 0;
    uint32_t *out = build(tex, 8, 8, &has_alpha, &levels);
    int ok = out && levels == 3u && has_alpha == 0;
    const uint32_t want = rgba(188, 188, 188, 255);
    for (size_t i = 0; ok && i < nk_tex_mips_chain_texels(8, 8); i++) {
        if (out[i] != want) {
            fprintf(stderr, "tex mips selftest: FAIL checker texel %zu = %08x, want %08x\n", i, out[i], want);
            ok = 0;
        }
    }
    if (!out || levels != 3u || has_alpha != 0)
        fprintf(stderr, "tex mips selftest: FAIL checker levels=%u has_alpha=%d\n", levels, has_alpha);
    free(out);
    if (ok) printf("tex mips selftest: PASS linear-light checker (every level %08x)\n", want);
    return ok;
}

/* One opaque blue texel among three transparent red ones: the colour is the blue texel's alone
 * and the alpha the block's mean (255 / 4, rounded). */
static int check_alpha_weighting(void) {
    const uint32_t tex[4] = { rgba(0, 0, 255, 255), rgba(255, 0, 0, 0), rgba(255, 0, 0, 0), rgba(255, 0, 0, 0) };
    int has_alpha = 0;
    uint32_t levels = 0;
    uint32_t *out = build(tex, 2, 2, &has_alpha, &levels);
    const uint32_t want = rgba(0, 0, 255, 64);
    const int ok = out && levels == 1u && has_alpha == 1 && out[0] == want;
    if (!ok) fprintf(stderr, "tex mips selftest: FAIL alpha weighting: levels=%u has_alpha=%d texel=%08x, want %08x\n",
                     levels, has_alpha, out ? out[0] : 0u, want);
    else printf("tex mips selftest: PASS alpha-weighted colour %08x\n", out[0]);
    free(out);
    return ok;
}

/* A colour-keyed texture: the left three columns opaque red, the rest the key (magenta, alpha 0),
 * as a palette entry marked transparent gives. No level above 0 may hold any blue: every texel,
 * opaque or not, is red, so no filter footprint at an edge can blend the key in. */
static int check_key_edge(void) {
    enum { W = 16, H = 8 };
    uint32_t tex[W * H];
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) tex[y * W + x] = x < 3u ? rgba(255, 0, 0, 255) : rgba(255, 0, 255, 0);
    int has_alpha = 0;
    uint32_t levels = 0;
    uint32_t *out = build(tex, W, H, &has_alpha, &levels);
    int ok = out && levels == 4u && has_alpha == 1;
    size_t keyed = 0, partial = 0;
    for (size_t i = 0; ok && i < nk_tex_mips_chain_texels(W, H); i++) {
        const uint32_t p = out[i], a = p >> 24;
        if ((p & 0x00FFFFFFu) != rgba(255, 0, 0, 0)) {
            fprintf(stderr, "tex mips selftest: FAIL key edge texel %zu = %08x: the key's colour reached it\n", i, p);
            ok = 0;
        }
        keyed += a == 0u;
        partial += a != 0u && a != 255u;
    }
    /* Level 1 (8x4): column 1 covers source columns 2 and 3, half opaque. */
    if (ok && (out[1] >> 24) != 128u) {
        fprintf(stderr, "tex mips selftest: FAIL key edge level-1 column 1 alpha %u, want 128\n", out[1] >> 24);
        ok = 0;
    }
    if (ok && (!keyed || !partial)) {
        fprintf(stderr, "tex mips selftest: FAIL key edge fixture: %zu keyed, %zu partial texels\n", keyed, partial);
        ok = 0;
    }
    if (!out || levels != 4u || has_alpha != 1)
        fprintf(stderr, "tex mips selftest: FAIL key edge levels=%u has_alpha=%d\n", levels, has_alpha);
    free(out);
    if (ok) printf("tex mips selftest: PASS key edge (%zu keyed and %zu edge texels, all red)\n", keyed, partial);
    return ok;
}

/* A texture with alpha 0 everywhere (drawn without its alpha) has no texel to take a colour from:
 * each level is the plain linear-light mean, alpha 0. */
static int check_all_keyed(void) {
    uint32_t tex[4 * 4];
    for (uint32_t i = 0; i < 16u; i++) tex[i] = (i & 1u) ? rgba(255, 255, 255, 0) : rgba(0, 0, 0, 0);
    int has_alpha = 0;
    uint32_t levels = 0;
    uint32_t *out = build(tex, 4, 4, &has_alpha, &levels);
    int ok = out && levels == 2u && has_alpha == 1;
    for (size_t i = 0; ok && i < nk_tex_mips_chain_texels(4, 4); i++) {
        if (out[i] != rgba(188, 188, 188, 0)) {
            fprintf(stderr, "tex mips selftest: FAIL all-keyed texel %zu = %08x, want %08x\n", i, out[i],
                    rgba(188, 188, 188, 0));
            ok = 0;
        }
    }
    free(out);
    if (ok) printf("tex mips selftest: PASS no alpha anywhere (plain mean kept)\n");
    return ok;
}

int main(void) {
    nk_tex_mips_curve_init(&s_curve);
    int ok = 1;
    ok &= check_levels();
    ok &= check_linear_light();
    ok &= check_alpha_weighting();
    ok &= check_key_edge();
    ok &= check_all_keyed();
    puts(ok ? "tex mips selftest: OK" : "tex mips selftest: FAILED");
    return ok ? 0 : 1;
}
