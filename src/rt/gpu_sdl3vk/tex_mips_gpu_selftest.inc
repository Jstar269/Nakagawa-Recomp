/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * tex_mips_gpu_selftest.inc - gpu-mips-selftest: host-built mip chains (tex_mips.h, Enhanced
 * part mips=on) through ge_gpu.c's own hooks on a Vulkan device, read back at device resolution.
 * Included by ge_gpu.c in the SR_GPU_MIPS_SELFTEST build, after ge_gpu_selftest.inc.
 *
 * The fixture is a 64x64 CLUT8 texture with one level, filtered linearly, clamped, drawn as a
 * transformed quad. Cells:
 *  - minified: a one-texel checkerboard (palette entries 0 black, 1 white) drawn at 3.2 texels
 *    per device pixel samples the chain: every device pixel is the chain's linear-light grey
 *    (sRGB 188). Level 0 alone aliases: the pixels range from dark to light.
 *  - scale-aware: 2 texels per guest pixel is minified at 1x but not from 2x. From 2x the draw
 *    binds the chain and matches, pixel for pixel, the same draw on the entry's one-level view,
 *    which samples level 0 alone (the level of detail is per device pixel, with no bias); at 1x it
 *    is the chain's linear-light grey, where level 0 alone gives the sRGB blend (about 96).
 *  - palette: the same index image under another palette is another cache entry with its own
 *    chain (red and blue give sRGB 188, 0, 188); the first palette's entry is still its own.
 *  - rewrite: guest bytes rewritten under the same palette rebuild the chain at the next bind.
 *  - reuse: the checkerboard written back is contents the chain cache has seen: its chain is
 *    uploaded again with no build, and samples the same grey.
 *  - deferred: new contents (one-texel stripes) drawn in through mode only build nothing; the
 *    first transformed, minified draw of them, after everything recorded is submitted, decodes
 *    level 0 again and builds the chain once, and samples the same grey.
 *  - alpha-test: an alpha-tested draw of a texture with alpha binds level 0 (coverage, depth and
 *    stencil stay the game's own).
 *  - one-to-one: a transformed draw that maps texels 1:1 onto guest pixels binds level 0.
 *  - budget: past the guest frame's build budget a draw binds level 0 and builds nothing; with
 *    budget it builds. The budget counts level-0 texels, not host time, per guest flip, not host
 *    present: with room for two 64x64 textures, two new contents build in one frame and the third
 *    binds level 0 until the next, which guest flips that no host present follows still open.
 *  - stream: new contents at the texture's address in three guest frames in a row stop the builds
 *    (the third draw binds level 0). While it streams, contents the cache holds bind their kept
 *    chain, contents refused once are built when they come back, and contents new again keep
 *    level 0 until the address holds still, when the next draw builds.
 *  - key-edge: opaque red beside the colour key (entry 2, magenta, alpha 0), blended by source
 *    alpha over black: no device pixel takes any blue, at the edge or anywhere.
 *  - gates: a through-mode draw and a draw whose texture function ignores the alpha of a
 *    texture that has some bind level 0 alone, and a color-tested draw stays with the software
 *    renderer, which has no chain.
 *  - movie (#2111): a decoded picture drawn as a transformed, linearly filtered, minified quad over
 *    the whole frame is a movie frame, with the chain bound (mips on) or level 0 (mips off).
 * With mips off (`--ace` with a line that has no mips) the run checks the other side: no chain
 * is made, nothing is bound, and the minified cell shows the aliasing the chain removes.
 */

#define HM_TEX   0x04050000u                 /* 64x64 CLUT8 indices */
#define HM_CLUT  0x04052000u                 /* 256 entries of 8888 */
#define HM_LIST  0x00104000u
#define HM_FBA   0x00160000u
#define HM_Y     40

static uint32_t hm_rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return r | (g << 8) | (b << 16) | (a << 24);
}

static void hm_fill(int kind) {
    uint8_t *t = (uint8_t *)SR_HOST(HM_TEX);
    for (uint32_t v = 0; v < 64u; v++)
        for (uint32_t u = 0; u < 64u; u++)
            t[v * 64u + u] = kind == 0 ? (uint8_t)((u ^ v) & 1u) : kind == 1 ? (uint8_t)1u
                           : kind == 3 ? (uint8_t)(v & 1u)
                           : kind >= 20 ? (uint8_t)(((u * 3u + v * 5u + (uint32_t)kind) % 7u) == 0u)
                           : kind >= 10 ? (uint8_t)(((u * 7u + v * 13u + (uint32_t)kind) % 5u) == 0u)
                           : (uint8_t)(u < 32u ? 3u : 2u);
    sr_gpu_vram_dirty(HM_TEX, 64u * 64u);
}

/* `n` guest flips, as the display reports them (gegpu_note_guest_flip), with no host present. */
static void hm_flips(uint32_t n) {
    for (uint32_t i = 0; i < n; i++) gegpu_note_guest_flip();
}

static void hm_palette(uint32_t c0, uint32_t c1) {
    uint32_t *p = (uint32_t *)SR_HOST(HM_CLUT);
    for (uint32_t i = 0; i < 256u; i++) p[i] = hm_rgba(0, 0, 0, 255);
    p[0] = c0; p[1] = c1;
    p[2] = hm_rgba(255, 0, 255, 0);           /* the colour key */
    p[3] = hm_rgba(255, 0, 0, 255);
    sr_gpu_vram_dirty(HM_CLUT, 256u * 4u);
}

/* The state, no draw: TEXFUNC `tfunc` (bit 8 RGBA), blending by source alpha when `blend`, the
 * color test when `ctest`, and the palette loaded from HM_CLUT. */
static int s_hm_atest;                       /* hm_state: the alpha test, GREATER than 128 */
static float s_hm_u0 = 0.25f;                /* hm_quad: the texel coordinate at the quad's left/top */

static void hm_state(uint32_t tfunc, int blend, int ctest) {
    uint32_t *dl = (uint32_t *)SR_HOST(HM_LIST);
    int p = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x9C, HM_FBA);
    DL_EMIT(0x9D, 512u | (0x04000000u >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x4C, 0);
    DL_EMIT(0x4D, 0);
    DL_EMIT(0x23, 0);
    DL_EMIT(0x22, s_hm_atest ? 1u : 0u);                /* alpha test */
    DL_EMIT(0xDB, 6u | (128u << 8) | (0xFFu << 16));    /* greater than 128, every bit */
    DL_EMIT(0x21, blend ? 1u : 0u);
    DL_EMIT(0xDF, 2u | (3u << 4));                      /* src alpha, 1 - src alpha, add */
    DL_EMIT(0x24, 0);                                   /* stencil test off */
    DL_EMIT(0x27, ctest ? 1u : 0u);                     /* color test */
    DL_EMIT(0xD8, 2u);                                  /* equal */
    DL_EMIT(0xD9, 0x00FF00FFu);
    DL_EMIT(0xDA, 0x00FFFFFFu);
    DL_EMIT(0x15, 0);
    DL_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xB0, HM_CLUT & 0x00FFFFFFu);
    DL_EMIT(0xB1, (HM_CLUT & 0xFF000000u) >> 8);
    DL_EMIT(0xC4, 32u);                                 /* 256 entries of 8888 */
    DL_EMIT(0xC5, 3u | (0xFFu << 8));
    DL_EMIT(0xA0, HM_TEX & 0x00FFFFFFu);
    DL_EMIT(0xA8, 64u | ((HM_TEX & 0xFF000000u) >> 8));
    DL_EMIT(0xB8, (6u << 8) | 6u);
    DL_EMIT(0xC2, 0);                                   /* one level */
    DL_EMIT(0xC8, 0);
    DL_EMIT(0xC0, 0);
    DL_EMIT(0xC3, 5);                                   /* CLUT8 */
    DL_EMIT(0xC6, 0x101u);                              /* linear min and mag */
    DL_EMIT(0xC7, (1u << 8) | 1u);                      /* clamp u and v */
    DL_EMIT(0xC9, tfunc);
    DL_EMIT(0xCB, 0);
    DL_EMIT(0x1E, 1);
    DL_EMIT(0x12, (3u << 0) | (3u << 7) | (1u << 23));
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT
    ge_run_list(HM_LIST, 0);
    s_ge->minz = 0u;
    s_ge->maxz = 65535u;
}

/* A quad from guest x `x`, HM_Y, `size` guest pixels square, texel coordinates 0..`span` (+0.25),
 * transformed (`through` 0) or through mode. Returns 0 on a failed draw. */
static int hm_quad(int x, int size, float span, int through) {
    if (through) s_ge->vtype |= 1u << 23; else s_ge->vtype &= ~(1u << 23);
    const float x0 = (float)x, y0 = (float)HM_Y, x1 = x0 + (float)size, y1 = y0 + (float)size;
    const float u0 = s_hm_u0, u1 = s_hm_u0 + span;
    GeVtx a = { x0, y0, 1.0f, 1.0f, u0, u0, 1.0f, 255, 255, 255, 255 };
    GeVtx b = { x1, y0, 1.0f, 1.0f, u1, u0, 1.0f, 255, 255, 255, 255 };
    GeVtx d = { x0, y1, 1.0f, 1.0f, u0, u1, 1.0f, 255, 255, 255, 255 };
    GeVtx e = { x1, y1, 1.0f, 1.0f, u1, u1, 1.0f, 255, 255, 255, 255 };
    const unsigned long long fb0 = s_replay_stats.fallback_prims;
    if (through) return hook_sprite(&a, &e, 0) && s_replay_stats.fallback_prims == fb0;
    return hook_tri(&a, &b, &e, 1) && hook_tri(&a, &e, &d, 1) && s_replay_stats.fallback_prims == fb0;
}

typedef struct { uint32_t lo[4], hi[4]; unsigned n; } HmRange;

/* The channel ranges over the quad's device pixels, one device pixel in from each edge. */
static int hm_read(int x, int size, uint32_t *dev, HmRange *r) {
    if (!s_cur || !coherence_read_device(s_cur->img, VK_IMAGE_ASPECT_COLOR_BIT, &s_cur->layout,
                                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 4u, dev))
        return 0;
    const uint32_t s = (uint32_t)s_scale;
    for (int c = 0; c < 4; c++) { r->lo[c] = 255u; r->hi[c] = 0u; }
    r->n = 0;
    for (uint32_t y = HM_Y * s + 1u; y + 1u < (uint32_t)(HM_Y + size) * s; y++) {
        for (uint32_t xx = (uint32_t)x * s + 1u; xx + 1u < (uint32_t)(x + size) * s; xx++) {
            const uint32_t px = dev[(size_t)y * SCL_W + xx];
            for (int c = 0; c < 4; c++) {
                const uint32_t v = (px >> (8 * c)) & 0xFFu;
                if (v < r->lo[c]) r->lo[c] = v;
                if (v > r->hi[c]) r->hi[c] = v;
            }
            r->n++;
        }
    }
    return r->n > 0u;
}

static int hm_near(const HmRange *r, uint32_t want, uint32_t tol) {
    for (int c = 0; c < 3; c++) {
        const uint32_t w = (want >> (8 * c)) & 0xFFu;
        if (r->lo[c] + tol < w || r->hi[c] > w + tol) return 0;
    }
    return 1;
}

/* The largest colour difference between two reads over the quad's device pixels, one in from each edge. */
static uint32_t hm_maxdiff(int x, int size, const uint32_t *a, const uint32_t *b) {
    const uint32_t s = (uint32_t)s_scale;
    uint32_t m = 0u;
    for (uint32_t y = HM_Y * s + 1u; y + 1u < (uint32_t)(HM_Y + size) * s; y++)
        for (uint32_t xx = (uint32_t)x * s + 1u; xx + 1u < (uint32_t)(x + size) * s; xx++)
            for (int c = 0; c < 3; c++) {
                const uint32_t va = (a[(size_t)y * SCL_W + xx] >> (8 * c)) & 0xFFu;
                const uint32_t vb = (b[(size_t)y * SCL_W + xx] >> (8 * c)) & 0xFFu;
                const uint32_t d = va > vb ? va - vb : vb - va;
                if (d > m) m = d;
            }
    return m;
}

static void hm_say(const char *cell, const HmRange *r) {
    fprintf(stderr, "gpu mips selftest [%s]: %u device pixels, r %u-%u g %u-%u b %u-%u at scale %d\n", cell,
            r->n, r->lo[0], r->hi[0], r->lo[1], r->hi[1], r->lo[2], r->hi[2], s_scale);
}

/* movie (#2111): a decoded 8888 picture drawn over the whole frame as a transformed quad with a
   linear filter, minified (1024 x 544 texels over 480 x 272, repeating). The whole 512 x 512
   texture is opaque, so the chain's alpha rule does not keep level 0. Mips on, the draw binds the
   host chain's set and is still a movie frame; mips off, it binds level 0 and is one too. */
static int hm_movie_cell(int on) {
    coherence_reset_targets();
    memset(SR_HOST(MVC_BASE), 0, 512u * FB_H * 4u);
    sr_gpu_vram_dirty(MVC_BASE, 512u * FB_H * 4u);
    sr_movie_set_tracking(1);
    mvc_write_picture(MVC_PIC, 512u, 512u, 512u, 9u);
    sr_movie_note_picture(MVC_OWNER, MVC_PIC, 512u, 3u, 480u, 272u);
    mvc_state();
    s_ge->tex_enable = 1;
    s_ge->tex_filter = 0x101u;   /* linear min and mag */
    s_ge->vtype &= ~(1u << 23);  /* transformed */
    ge_state_note_external_write();
    s_tmpl_ok = 0;
    GeVtx a = { 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 255, 255, 255, 255 };
    GeVtx b = { 480.0f, 0.0f, 1.0f, 1.0f, 1024.0f, 0.0f, 1.0f, 255, 255, 255, 255 };
    GeVtx d = { 0.0f, 272.0f, 1.0f, 1.0f, 0.0f, 544.0f, 1.0f, 255, 255, 255, 255 };
    GeVtx e = { 480.0f, 272.0f, 1.0f, 1.0f, 1024.0f, 544.0f, 1.0f, 255, 255, 255, 255 };
    const int drew = hook_tri(&a, &b, &e, 1) && hook_tri(&a, &e, &d, 1);
    const int chain = s_tex_bound_hm;
    const int ok = drew && chain == on && mvc_present("movie", on ? "a transformed linear quad, mips on"
                                                         : "a transformed linear quad, mips off",
                                            SDL3VK_FRAME_MOVIE);
    sr_movie_set_tracking(0);
    if (!ok) {
        const TexEnt *me = s_tex_cur;
        fprintf(stderr, "gpu mips selftest [movie]: drew %d, chain bound %d (want %d); entry levels %u, "
                        "made %d, set %d, ready %d, alpha %d, skip %d; plain target %llu alpha %llu stream %llu "
                        "budget %llu\n", drew, chain, on, me ? me->hm_levels : 0u, me ? (int)me->hm_made : -1,
                me && me->hm_set, me ? me->hm_ready : -1, me ? me->hm_alpha : -1, me ? (int)me->hm_skip : -1,
                (unsigned long long)s_hm_stats.plain_target, (unsigned long long)s_hm_stats.plain_alpha,
                (unsigned long long)s_hm_stats.plain_stream, (unsigned long long)s_hm_stats.plain_budget);
    } else {
        printf("gpu mips selftest: PASS movie       a transformed, linear movie quad is a movie frame (%s)\n",
               on ? "the chain bound" : "level 0");
    }
    return ok;
}

int gegpu_mips_selftest(void) {
    const uint32_t base = 0x04000000u | HM_FBA;
    const int on = s_hm;
    uint32_t *dev = (uint32_t *)malloc((size_t)SCL_W * SCL_H * 4u);
    if (!dev) return 0;
    coherence_reset_targets();
    memset(SR_HOST(base), 0, 512u * FB_H * 4u);
    sr_gpu_vram_dirty(base, 512u * FB_H * 4u);
    ge_set_gpu_hooks(&k_hooks);
    /* Every cell but budget builds without a budget, so the cells do not depend on the host's
       speed; the budget cell sets its own. */
    s_hm_budget_texels = UINT64_MAX;
    const uint32_t grey = hm_rgba(188, 188, 188, 0);
    int ok = 1;
    HmRange r;

    /* minified: 64 texels over 5 guest pixels, 3.2 texels per device pixel at 4x. */
    hm_fill(0);
    hm_palette(hm_rgba(0, 0, 0, 255), hm_rgba(255, 255, 255, 255));
    hm_state(3u | (1u << 8), 0, 0);
    const uint64_t bound0 = s_hm_stats.bound;
    ok = hm_quad(20, 5, 64.0f, 0);
    /* Key off, the draw is the one the renderer made before the part: an image of the game's one
       level, no chain objects or builder buffers, the entry's own set bound and no F_HOSTMIP in
       the batch's flags, so the commands, descriptors and shader path are those of a run without
       it. */
    if (ok && !on) {
        const TexEnt *e = s_tex_cur;
        const Batch *b = s_nbatch ? &s_batch[s_nbatch - 1u] : NULL;
        const char *bad = !e ? "no entry bound" : e->hm_levels != 0u ? "the image has chain levels"
                        : (e->hm_set || e->hm_view || e->hm_smp || e->hm_made) ? "the entry has chain objects"
                        : (s_hm_out || s_hm_work) ? "the builder's buffers exist"
                        : !b ? "no batch recorded"
                        : ((((uint32_t)b->pc.cfg[0] >> 8) & F_HOSTMIP) != 0u) ? "the batch has F_HOSTMIP"
                        : b->dset != tex_entry_set(s_tex_cur) ? "the batch binds another set" : NULL;
        if (bad) {
            fprintf(stderr, "gpu mips selftest [key off]: %s\n", bad);
            ok = 0;
        } else {
            printf("gpu mips selftest: PASS key off    one level, no chain objects, the entry's own set, no flag\n");
        }
    }
    ok = ok && hm_read(20, 5, dev, &r);
    if (ok && on && (!hm_near(&r, grey, 1u) || s_hm_stats.bound != bound0 + 1u || s_hm_stats.entries != 1u)) {
        hm_say("minified", &r);
        fprintf(stderr, "gpu mips selftest [minified]: want every pixel 188 from the chain (bound %llu, entries %llu)\n",
                (unsigned long long)(s_hm_stats.bound - bound0), (unsigned long long)s_hm_stats.entries);
        ok = 0;
    }
    if (ok && !on && (r.hi[0] - r.lo[0] < 64u || s_hm_stats.entries != 0u || s_hm_stats.bound != 0u)) {
        hm_say("minified, mips off", &r);
        fprintf(stderr, "gpu mips selftest [minified, mips off]: want level 0's aliasing and no chain\n");
        ok = 0;
    }
    if (ok) printf("gpu mips selftest: PASS minified    r %u-%u (%s) scale=%d\n", r.lo[0], r.hi[0],
                   on ? "the chain's grey" : "level 0 aliases, mips off", s_scale);
    if (!on) {
        ok = ok && hm_movie_cell(on);
        free(dev);
        coherence_reset_targets();
        return ok;
    }

    /* scale-aware: 64 texels over 32 guest pixels: 2 texels a device pixel at 1x, 1 at 2x, half at
       4x. The reference is the same draw on the entry's one-level view: s_tex_noscale keeps the
       bind off, as it does for a render target read back, so it samples level 0 alone. */
    if (ok) {
        uint32_t *ref = (uint32_t *)malloc((size_t)SCL_W * SCL_H * 4u);
        HmRange r0;
        s_tex_noscale = 1;
        ok = ref && hm_quad(40, 32, 64.0f, 0) && !s_tex_bound_hm && hm_read(40, 32, ref, &r0);
        s_tex_noscale = 0;
        const uint64_t b1 = s_hm_stats.bound;
        ok = ok && hm_quad(40, 32, 64.0f, 0) && s_tex_bound_hm && s_hm_stats.bound == b1 + 1u &&
             hm_read(40, 32, dev, &r);
        const uint32_t diff = ok ? hm_maxdiff(40, 32, dev, ref) : 0u;
        if (!ok) {
            fprintf(stderr, "gpu mips selftest [scale-aware]: a draw failed, or the reference bound the chain "
                            "or the draw did not\n");
        } else if (s_scale >= 2 && diff > 1u) {
            hm_say("scale-aware", &r);
            fprintf(stderr, "gpu mips selftest [scale-aware]: want level 0 alone from 2x: the chain-bound draw "
                            "differs from the one-level view by up to %u\n", diff);
            ok = 0;
        } else if (s_scale == 1 && (!hm_near(&r, grey, 2u) || diff < 32u)) {
            /* Level 0 alone at 2 texels a pixel blends black and white in sRGB (about 96), or aliases. */
            hm_say("scale-aware", &r);
            fprintf(stderr, "gpu mips selftest [scale-aware]: want the chain's grey at 1x, unlike level 0 alone "
                            "(level 0 r %u-%u, max diff %u)\n", r0.lo[0], r0.hi[0], diff);
            ok = 0;
        }
        if (ok) printf("gpu mips selftest: PASS scale-aware %s (r %u-%u, level 0 alone r %u-%u, max diff %u) "
                       "scale=%d\n", s_scale >= 2 ? "level 0" : "the chain's grey", r.lo[0], r.hi[0],
                       r0.lo[0], r0.hi[0], diff, s_scale);
        free(ref);
    }

    /* palette: the same indices, red and blue. */
    if (ok) {
        const uint64_t builds0 = s_hm_stats.builds;
        hm_palette(hm_rgba(255, 0, 0, 255), hm_rgba(0, 0, 255, 255));
        hm_state(3u | (1u << 8), 0, 0);
        ok = hm_quad(80, 5, 64.0f, 0) && hm_read(80, 5, dev, &r);
        if (ok && (!hm_near(&r, hm_rgba(188, 0, 188, 0), 1u) || s_hm_stats.builds != builds0 + 1u ||
                   s_hm_stats.entries != 2u)) {
            hm_say("palette", &r);
            fprintf(stderr, "gpu mips selftest [palette]: want 188,0,188 from a new chain (builds +%llu, entries %llu)\n",
                    (unsigned long long)(s_hm_stats.builds - builds0), (unsigned long long)s_hm_stats.entries);
            ok = 0;
        }
        hm_palette(hm_rgba(0, 0, 0, 255), hm_rgba(255, 255, 255, 255));
        hm_state(3u | (1u << 8), 0, 0);
        ok = ok && hm_quad(90, 5, 64.0f, 0) && hm_read(90, 5, dev, &r);
        if (ok && (!hm_near(&r, grey, 1u) || s_hm_stats.builds != builds0 + 1u)) {
            hm_say("palette back", &r);
            fprintf(stderr, "gpu mips selftest [palette]: the first palette's entry should be drawn again unchanged\n");
            ok = 0;
        }
        if (ok) printf("gpu mips selftest: PASS palette    a second palette gets its own chain\n");
    }

    /* rewrite: every index 1 (white) under the same palette. */
    if (ok) {
        const uint64_t builds0 = s_hm_stats.builds;
        hm_fill(1);
        hm_state(3u | (1u << 8), 0, 0);
        ok = hm_quad(100, 5, 64.0f, 0) && hm_read(100, 5, dev, &r);
        if (ok && (!hm_near(&r, hm_rgba(255, 255, 255, 0), 0u) || s_hm_stats.builds != builds0 + 1u)) {
            hm_say("rewrite", &r);
            fprintf(stderr, "gpu mips selftest [rewrite]: want white from a rebuilt chain\n");
            ok = 0;
        }
        if (ok) printf("gpu mips selftest: PASS rewrite    rebuilt at the next bind\n");
    }

    /* reuse: the checkerboard written back into the same texture is contents the cache has seen,
       so its chain is uploaded again, not built, and samples the same grey. */
    if (ok) {
        const uint64_t builds0 = s_hm_stats.builds, reused0 = s_hm_stats.reused;
        hm_fill(0);
        hm_state(3u | (1u << 8), 0, 0);
        ok = hm_quad(110, 5, 64.0f, 0) && hm_read(110, 5, dev, &r);
        if (ok && (!hm_near(&r, grey, 1u) || s_hm_stats.builds != builds0 || s_hm_stats.reused != reused0 + 1u)) {
            hm_say("reuse", &r);
            fprintf(stderr, "gpu mips selftest [reuse]: want the cached chain's grey with no build (builds +%llu, "
                            "reused +%llu)\n", (unsigned long long)(s_hm_stats.builds - builds0),
                    (unsigned long long)(s_hm_stats.reused - reused0));
            ok = 0;
        }
        if (ok) printf("gpu mips selftest: PASS reuse      contents seen before upload their kept chain\n");
    }

    /* deferred: a through-mode draw of new contents builds nothing; the transformed draw after it
       finds no fresh decode, decodes level 0 again, and builds the chain once. */
    if (ok) {
        const uint64_t builds0 = s_hm_stats.builds, redecodes0 = s_hm_stats.redecodes;
        hm_fill(3);
        hm_state(3u | (1u << 8), 0, 0);
        int through = hm_quad(130, 5, 64.0f, 1) && !s_tex_bound_hm && s_hm_stats.builds == builds0;
        /* Submit what is recorded, as a present does: the late build then begins its own
           command buffer for the chain's levels (a closed one aborted the run in the loader). */
        through = through && submit_pending() && cmd_batch_flush() && s_cmd == VK_NULL_HANDLE;
        ok = through && hm_quad(130, 5, 64.0f, 0) && s_tex_bound_hm && hm_read(130, 5, dev, &r);
        if (!through || (ok && (!hm_near(&r, grey, 1u) || s_hm_stats.builds != builds0 + 1u ||
                                s_hm_stats.redecodes != redecodes0 + 1u))) {
            if (through) hm_say("deferred", &r);
            fprintf(stderr, "gpu mips selftest [deferred]: want no build for the through-mode draw (%s), then one "
                            "build from a fresh decode (builds +%llu, redecodes +%llu)\n", through ? "ok" : "BAD",
                    (unsigned long long)(s_hm_stats.builds - builds0),
                    (unsigned long long)(s_hm_stats.redecodes - redecodes0));
            ok = 0;
        }
        if (ok) printf("gpu mips selftest: PASS deferred   through mode builds nothing; the first minified draw "
                       "builds once\n");
    }

    /* alpha-test: the keyed texture, alpha-tested, minified. */
    if (ok) {
        const uint64_t at0 = s_hm_stats.plain_alpha_test, bound0 = s_hm_stats.bound;
        hm_fill(2);
        s_hm_atest = 1;
        hm_state(3u | (1u << 8), 0, 0);
        ok = hm_quad(170, 5, 64.0f, 0) && !s_tex_bound_hm;
        s_hm_atest = 0;
        if (!ok || s_hm_stats.plain_alpha_test != at0 + 1u || s_hm_stats.bound != bound0) {
            fprintf(stderr, "gpu mips selftest [alpha-test]: want level 0 for an alpha-tested draw (plain_alpha_test "
                            "+%llu, bound +%llu)\n", (unsigned long long)(s_hm_stats.plain_alpha_test - at0),
                    (unsigned long long)(s_hm_stats.bound - bound0));
            ok = 0;
        }
        if (ok) printf("gpu mips selftest: PASS alpha-test  an alpha-tested draw keeps level 0\n");
    }

    /* one-to-one: the checker drawn 64 pixels wide from texel 0, transformed. */
    if (ok) {
        const uint64_t one0 = s_hm_stats.plain_onetoone;
        hm_fill(0);
        hm_state(3u | (1u << 8), 0, 0);
        s_hm_u0 = 0.0f;
        ok = hm_quad(200, 64, 64.0f, 0) && !s_tex_bound_hm && s_hm_stats.plain_onetoone > one0;
        s_hm_u0 = 0.25f;
        if (!ok) {
            fprintf(stderr, "gpu mips selftest [one-to-one]: want level 0 for a 1:1 draw (plain_onetoone +%llu)\n",
                    (unsigned long long)(s_hm_stats.plain_onetoone - one0));
        } else {
            printf("gpu mips selftest: PASS one-to-one a transformed 1:1 draw binds level 0\n");
        }
    }

    /* budget: no budget left, then the budget back. */
    if (ok) {
        const uint64_t builds0 = s_hm_stats.builds, pb0 = s_hm_stats.plain_budget;
        const uint64_t reused0 = s_hm_stats.reused, ps0 = s_hm_stats.plain_stream, pa0 = s_hm_stats.plain_alpha;
        hm_fill(10);
        hm_state(3u | (1u << 8), 0, 0);
        s_hm_budget_texels = 0u;
        const int held = hm_quad(270, 5, 64.0f, 0) && !s_tex_bound_hm && s_hm_stats.builds == builds0 &&
                         s_hm_stats.plain_budget == pb0 + 1u;
        s_hm_budget_texels = UINT64_MAX;
        /* A submit ends the batch, as a present does: a draw with the same state would join it and
           keep its level-0 set. */
        const int q2 = held && submit_pending() && hm_quad(270, 5, 64.0f, 0);
        ok = q2 && s_tex_bound_hm && s_hm_stats.builds == builds0 + 1u;
        if (!ok) {
            fprintf(stderr, "gpu mips selftest [budget]: want level 0 and no build past the budget (%s), then one "
                            "build (builds +%llu, plain_budget +%llu; draw %d, bound %d, reused +%llu, "
                            "plain_stream +%llu, plain_alpha +%llu)\n", held ? "ok" : "BAD",
                    (unsigned long long)(s_hm_stats.builds - builds0),
                    (unsigned long long)(s_hm_stats.plain_budget - pb0), q2, s_tex_bound_hm,
                    (unsigned long long)(s_hm_stats.reused - reused0),
                    (unsigned long long)(s_hm_stats.plain_stream - ps0),
                    (unsigned long long)(s_hm_stats.plain_alpha - pa0));
        } else {
            printf("gpu mips selftest: PASS budget     past the budget level 0, then one build\n");
        }
    }
    /* budget in texels: room for two 64x64 builds in one guest frame; the third waits a frame. The
       frames are guest flips that no host present follows (a frame cap's slot rule dropped them),
       which must still open a new window: the windows may not depend on how fast the host is. */
    if (ok) {
        const uint64_t builds0 = s_hm_stats.builds, pb0 = s_hm_stats.plain_budget;
        const uint64_t presents0 = xs_now();
        int binds = 0;
        hm_flips(HM_STREAM_GAP + 1u);
        s_hm_budget_texels = 2u * 64u * 64u;
        for (int k = 0; k < 3 && ok; k++) {
            hm_fill(20 + k);
            hm_state(3u | (1u << 8), 0, 0);
            ok = hm_quad(270, 5, 64.0f, 0);
            binds += s_tex_bound_hm;
        }
        const int held = ok && binds == 2 && s_hm_stats.builds == builds0 + 2u &&
                         s_hm_stats.plain_budget == pb0 + 1u;
        hm_flips(HM_STREAM_GAP + 1u);
        /* A submit ends the batch, as a present does: a draw with the same state would join it. */
        ok = held && submit_pending() && hm_quad(270, 5, 64.0f, 0) && s_tex_bound_hm &&
             s_hm_stats.builds == builds0 + 3u && xs_now() == presents0;
        s_hm_budget_texels = UINT64_MAX;
        if (!ok) {
            fprintf(stderr, "gpu mips selftest [budget-texels]: want two builds and level 0 for the third in one "
                            "frame (%s), then a build after guest flips alone (builds +%llu, plain_budget +%llu, binds %d)\n",
                    held ? "ok" : "BAD", (unsigned long long)(s_hm_stats.builds - builds0),
                    (unsigned long long)(s_hm_stats.plain_budget - pb0), binds);
        } else {
            printf("gpu mips selftest: PASS budget-tx  two 64x64 builds fill a frame's texels; the third waits a flip\n");
        }
    }

    /* stream: new contents in three presents in a row; then contents the cache holds, contents
       that come back, contents new again, and the address held still. */
    if (ok) {
        const uint64_t builds0 = s_hm_stats.builds, ps0 = s_hm_stats.plain_stream, reused0 = s_hm_stats.reused;
        static const struct { int kind; int bound; const char *what; } k_steps[] = {
            {11, 1, "first new contents build"},        {12, 1, "second new contents build"},
            {13, 0, "third new contents keep level 0"}, {11, 1, "cached contents bind their chain"},
            {13, 1, "contents that come back build"},   {14, 0, "new contents again keep level 0"},
        };
        const char *bad = NULL;
        hm_flips(HM_STREAM_GAP);   /* the earlier cells' decodes are past the gap */
        for (size_t k = 0; k < sizeof(k_steps) / sizeof(k_steps[0]) && ok && !bad; k++) {
            hm_flips(1u);
            hm_fill(k_steps[k].kind);
            hm_state(3u | (1u << 8), 0, 0);
            ok = hm_quad(280, 5, 64.0f, 0);
            if (ok && s_tex_bound_hm != k_steps[k].bound) bad = k_steps[k].what;
        }
        if (ok && !bad && (s_hm_stats.builds != builds0 + 3u || s_hm_stats.reused != reused0 + 1u ||
                           s_hm_stats.plain_stream != ps0 + 2u))
            bad = "three builds, one kept chain and two refusals while it streams";
        hm_flips(HM_STREAM_GAP + 1u);
        if (ok && !bad &&
            !(submit_pending() && hm_quad(280, 5, 64.0f, 0) && s_tex_bound_hm && s_hm_stats.builds == builds0 + 4u))
            bad = "the address held still builds";
        if (!ok || bad) {
            fprintf(stderr, "gpu mips selftest [stream]: %s (builds +%llu, reused +%llu, plain_stream +%llu)\n",
                    bad ? bad : "a draw failed", (unsigned long long)(s_hm_stats.builds - builds0),
                    (unsigned long long)(s_hm_stats.reused - reused0),
                    (unsigned long long)(s_hm_stats.plain_stream - ps0));
            ok = 0;
        } else {
            printf("gpu mips selftest: PASS stream     new contents keep level 0 while it streams; kept and "
                   "returning ones bind\n");
        }
    }

    /* key-edge: red beside the key, blended over black. */
    if (ok) {
        hm_fill(2);
        hm_state(3u | (1u << 8), 1, 0);
        ok = hm_quad(120, 5, 64.0f, 0) && hm_read(120, 5, dev, &r);
        if (ok && (r.hi[2] > 0u || r.hi[0] < 100u || r.lo[0] > 0u)) {
            hm_say("key-edge", &r);
            fprintf(stderr, "gpu mips selftest [key-edge]: want red fading to black with no blue\n");
            ok = 0;
        }
        if (ok) printf("gpu mips selftest: PASS key-edge   r %u-%u, b max %u\n", r.lo[0], r.hi[0], r.hi[2]);
    }

    /* gates: each binds level 0 alone. */
    if (ok) {
        hm_fill(0);
        const uint64_t bound0 = s_hm_stats.bound, al0 = s_hm_stats.plain_alpha;
        hm_state(3u | (1u << 8), 0, 0);
        const int g1 = hm_quad(140, 5, 64.0f, 1) && !s_tex_bound_hm;                /* through mode */
        const unsigned long long fb0 = s_replay_stats.fallback_prims;
        hm_state(3u | (1u << 8), 0, 1);                                             /* color test */
        const int g2 = !hm_quad(150, 5, 64.0f, 0) && s_replay_stats.fallback_prims > fb0 &&
                       s_hm_stats.bound == bound0;
        hm_fill(2);
        hm_state(3u, 1, 0);                                                         /* RGB: alpha unused */
        const int g3 = hm_quad(160, 5, 64.0f, 0) && !s_tex_bound_hm && s_hm_stats.plain_alpha == al0 + 1u;
        if (!g1 || !g2 || !g3 || s_hm_stats.bound != bound0) {
            fprintf(stderr, "gpu mips selftest [gates]: through mode %s, color test %s, alpha unused %s (bound +%llu, "
                            "fallbacks +%llu)\n", g1 ? "ok" : "BAD", g2 ? "ok" : "BAD", g3 ? "ok" : "BAD",
                    (unsigned long long)(s_hm_stats.bound - bound0),
                    (unsigned long long)(s_replay_stats.fallback_prims - fb0));
            ok = 0;
        } else {
            printf("gpu mips selftest: PASS gates      through mode and unused alpha keep level 0, color test "
                   "stays in software\n");
        }
    }

    /* memory: each 64x64 chain is levels 1..6, 1365 texels. */
    if (ok && s_hm_stats.bytes != s_hm_stats.entries * 1365u * 4u) {
        fprintf(stderr, "gpu mips selftest [memory]: %llu bytes for %llu chains\n",
                (unsigned long long)s_hm_stats.bytes, (unsigned long long)s_hm_stats.entries);
        ok = 0;
    }
    ok = ok && hm_movie_cell(on);
    free(dev);
    coherence_reset_targets();
    return ok;
}
