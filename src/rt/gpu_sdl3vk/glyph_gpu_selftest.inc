/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * glyph_gpu_selftest.inc - sharp game text on ge_gpu.c's production path (glyph_gpu.inc, #2110;
 * included by ge_gpu.c in the SR_GPU_GLYPH_SELFTEST build, which is also the texture scaling
 * selftest build, so it shares that file's helpers; mk/selftests.mk gpu-glyph-selftest).
 * Synthetic pages only. The cells, at render scale 2 to 4 (the renderer's range, MAX_SCALE):
 *   sharp      a 1:1 sprite over a glyph cell of an anti-aliased page draws the rebuilt tile: every
 *              device pixel's coverage is the tile's alpha (nk_glyph_rebuild) and the cell beside it does not
 *              change them; without the feature the same draw is not the tile (not vacuous);
 *   refuse     draws that must keep the game's own picture byte for byte, whatever the feature: a page in
 *              VRAM, a hard-edged page, a two-colour page, a magnified sprite, a rectangle too small,
 *              a rectangle past the page;
 *   rewrite    a page the guest rewrites draws a rebuilt tile for the new texels and never the old
 *              one; a cell whose texels did not change keeps its tile;
 *   rewrite-gap a rewrite the cache's content hash does not see (it samples part of a page) draws its new tile;
 *   gradient   fade bars of one colour (shallow, steep, four texels, two) and a box shadow draw without a reversal along their rows and columns;
 *   cap        more new rectangles in one present than NK_GLYPH_PER_PRESENT: exactly that many are
 *              rebuilt, the rest draw as before and are rebuilt by the next present;
 *   pool       the texture descriptor pool has a set for every image an entry can hold;
 *   guest-bytes the page, the CLUT and VRAM outside the frame are unchanged by any of it.
 * `--authentic` (Ace Mode off, SR_GLYPHS whatever it is) draws the same scene and prints a hash of
 * the device image; mk/selftests.mk compares the hash with the feature unset and set.
 */

#define GST_PAGE   0x08500000u
#define GST_W      32
#define GST_H      16
#define GST_CW     12
#define GST_CH     14
/* The target keeps no alpha (the stencil takes that byte), so a draw shows its coverage by blending: white ink
 * with its texture alpha over a black target (source alpha, one minus source alpha), and the red channel is the
 * coverage. */
#define GST_ALPHA(px) ((int)((px) & 0xFFu))

/* A cell of anti-aliased coverage: kind 0 a ring, kind 1 a slanted bar, kind 2 a solid-ish disc. */
static float gst_cov(int kind, int x, int y) {
    int n = 0;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            const float px = (float)x + ((float)i + 0.5f) / 4.0f, py = (float)y + ((float)j + 0.5f) / 4.0f;
            const float dx = px - 6.0f, dy = (py - 7.0f) * 0.85f, r = sqrtf(dx * dx + dy * dy);
            int in;
            if (kind == 0) in = r <= 5.2f && r >= 2.6f;
            else if (kind == 1) in = fabsf((px - 6.0f) + (py - 7.0f) * 0.55f) <= 1.4f;
            else in = r <= 4.7f;
            n += in;
        }
    return (float)n / 16.0f;
}

/* The page: glyph `ka` in the cell at texel (0, 0), glyph `kb` at (16, 0); every other texel blank
 * with a stray colour. `rgb` is the shared ink colour. */
static void gst_page(uint32_t *p, int ka, int kb, uint32_t rgb) {
    for (int i = 0; i < GST_W * GST_H; i++) p[i] = 0x00123456u;
    for (int c = 0; c < 2; c++)
        for (int y = 0; y < GST_CH; y++)
            for (int x = 0; x < GST_CW; x++) {
                const int a = (int)floorf(gst_cov(c ? kb : ka, x, y) * 255.0f + 0.5f);
                if (a) p[y * GST_W + c * 16 + x] = ((uint32_t)a << 24) | rgb;
            }
}

static void gst_put(uint32_t addr, const uint32_t *p) {
    memcpy(SR_HOST(addr), p, sizeof(uint32_t) * GST_W * GST_H);
    sr_gpu_vram_dirty(addr, sizeof(uint32_t) * GST_W * GST_H);
    ge_state_note_external_write();
}

static void gst_state(uint32_t addr, int w, int h) {
    wrap_state(XST_BASE, XST_STRIDE);
    s_ge->tex_enable = 1;
    s_ge->tex_addr = addr;
    s_ge->tex_bufw = (uint32_t)w;
    s_ge->tex_fmt = 3;
    s_ge->tex_w = w;
    s_ge->tex_h = h;
    s_ge->tex_func = 3u | 0x100u;            /* REPLACE with the texture's alpha, which the blend then uses */
    s_ge->blend_enable = 1;
    s_ge->blend_mode = 2u | (3u << 4);       /* source alpha, one minus source alpha */
    s_ge->tex_filter = 1u | 0x100u;          /* linear, as text is drawn */
    s_ge->tex_wrap = 0x101u;
    s_ge->vtype = 1u << 23;                  /* through mode */
    ge_state_note_external_write();
}

/* A sprite at guest (x, y), size (sw, sh) guest pixels, over texels (u0, v0) to (u1, v1). */
static int gst_sprite(float x, float y, float sw, float sh, float u0, float v0, float u1, float v1) {
    GeVtx p0, p1;
    wrap_vtx(&p0, x, y, 0xFFFFFFFFu); p0.u = u0; p0.v = v0;
    wrap_vtx(&p1, x + sw, y + sh, 0xFFFFFFFFu); p1.u = u1; p1.v = v1;
    s_tmpl_ok = 0;
    return hook_sprite(&p0, &p1, 0);
}

/* The cell at texel (cx, 0) as a 1:1 sprite at (x, y). */
static int gst_cell(int cx, float x, float y) {
    return gst_sprite(x, y, (float)GST_CW, (float)GST_CH, (float)cx, 0.0f, (float)(cx + GST_CW), (float)GST_CH);
}

static uint32_t *gst_device(void) {
    uint32_t *dev = (uint32_t *)malloc((size_t)SCL_W * SCL_H * 4u);
    if (!dev || !s_cur || !xst_read_device(s_cur, dev)) {
        free(dev);
        return NULL;
    }
    return dev;
}

/* Device pixels of the w x h guest-pixel region at (x, y) whose alpha differs from `want` (a
 * (w * s) x (h * s) tile) by more than one step. */
static int s_gst_max_diff_seen;
static int gst_off_tile(const uint32_t *dev, int x, int y, int w, int h, const uint32_t *want) {
    const int s = s_scale;
    int off = 0;
    for (int ky = 0; ky < h * s; ky++)
        for (int kx = 0; kx < w * s; kx++) {
            const int got = GST_ALPHA(dev[((size_t)y * (size_t)s + (size_t)ky) * SCL_W + (size_t)x * (size_t)s + (size_t)kx]);
            const int exp = (int)(want[(size_t)ky * (size_t)(w * s) + (size_t)kx] >> 24);
            if (abs(got - exp) > s_gst_max_diff_seen) s_gst_max_diff_seen = abs(got - exp);
            if (abs(got - exp) > 1) off++;
        }
    return off;
}

static int gst_diff_region(const uint32_t *a, const uint32_t *b, int x, int y, int w, int h) {
    const int s = s_scale;
    int n = 0;
    for (int ky = 0; ky < h * s; ky++)
        for (int kx = 0; kx < w * s; kx++) {
            const size_t i = ((size_t)y * (size_t)s + (size_t)ky) * SCL_W + (size_t)x * (size_t)s + (size_t)kx;
            if (a[i] != b[i]) n++;
        }
    return n;
}

static void gst_reset(void) {
    xst_reset();   /* frees every entry, scaled copy and glyph image */
}

static int gst_fail(const char *cell, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "gpu glyph selftest: FAIL %s: ", cell);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return 0;
}

static int gst_cell_sharp(void) {
    static uint32_t page[GST_W * GST_H], tile[GST_CW * 6 * GST_CH * 6], tile_b[GST_CW * 6 * GST_CH * 6];
    gst_reset();
    gst_page(page, 0, 1, 0x00FFFFFFu);
    gst_put(GST_PAGE, page);
    gst_state(GST_PAGE, GST_W, GST_H);
    if (nk_glyph_rebuild(page, GST_W, GST_H, 0, 0, GST_CW, GST_CH, s_scale, tile) != NK_GLYPH_OK ||
        nk_glyph_rebuild(page, GST_W, GST_H, 16, 0, GST_CW, GST_CH, s_scale, tile_b) != NK_GLYPH_OK)
        return gst_fail("sharp", "the synthetic glyphs do not rebuild");
    const uint64_t bound0 = s_cnt_gl_bound, built0 = s_cnt_gl_built;
    const int was = s_glyphs;
    /* the feature off: the game's own picture, which is not the tile */
    s_glyphs = 0;
    int ok = gst_cell(0, 20.0f, 20.0f);
    uint32_t *plain = ok ? gst_device() : NULL;
    s_glyphs = was;
    if (!plain) return gst_fail("sharp", "draw or readback failed");
    const int plain_off = gst_off_tile(plain, 20, 20, GST_CW, GST_CH, tile);
    ok = gst_cell(0, 60.0f, 20.0f) && gst_cell(16, 100.0f, 20.0f);
    uint32_t *dev = ok ? gst_device() : NULL;
    if (!dev) { free(plain); return gst_fail("sharp", "draw or readback failed"); }
    s_gst_max_diff_seen = 0;
    const int off_a = gst_off_tile(dev, 60, 20, GST_CW, GST_CH, tile);
    const int off_b = gst_off_tile(dev, 100, 20, GST_CW, GST_CH, tile_b);
    const uint64_t bound = s_cnt_gl_bound - bound0, built = s_cnt_gl_built - built0;
    /* the same cell beside other content: only its own texels matter */
    gst_page(page, 0, 2, 0x00FFFFFFu);
    gst_put(GST_PAGE, page);
    gst_state(GST_PAGE, GST_W, GST_H);
    ok = gst_cell(0, 140.0f, 20.0f);
    uint32_t *dev2 = ok ? gst_device() : NULL;
    int moved = 0;
    if (dev2) {   /* the cell at x = 140 against the same cell at x = 60 */
        const int s = s_scale;
        for (int ky = 0; ky < GST_CH * s; ky++)
            for (int kx = 0; kx < GST_CW * s; kx++)
                moved += dev2[((size_t)20 * s + ky) * SCL_W + (size_t)140 * s + kx] !=
                         dev[((size_t)20 * s + ky) * SCL_W + (size_t)60 * s + kx];
    }
    free(plain); free(dev); free(dev2);
    if (!dev2) return gst_fail("sharp", "draw or readback failed");
    if (bound != 2u || built != 2u)
        return gst_fail("sharp", "two glyph draws: %llu bound, %llu tiles built (want 2, 2)",
                        (unsigned long long)bound, (unsigned long long)built);
    if (off_a || off_b)
        return gst_fail("sharp", "the draw is not the rebuilt tile: %d device pixels of glyph A and %d of glyph B differ "
                        "from nk_glyph_rebuild at scale %d (largest difference %d of 255)", off_a, off_b, s_scale,
                        s_gst_max_diff_seen);
    if (plain_off == 0)
        return gst_fail("sharp", "vacuous: the draw without the feature is already the tile at scale %d", s_scale);
    if (moved)
        return gst_fail("sharp", "%d device pixels of a glyph changed when the cell beside it did", moved);
    printf("gpu glyph selftest: PASS %-12s scale %d: the draw is the rebuilt tile (%d device pixels differ without it); "
           "the cell beside it changes nothing\n", "sharp", s_scale, plain_off);
    return 1;
}

/* One refused draw: the device image with the feature on equals the one with it off, and nothing is bound. */
static int gst_refused_draw(const char *what, int (*draw)(void)) {
    const uint64_t bound0 = s_cnt_gl_bound, images0 = s_cnt_gl_images;
    const int was = s_glyphs;
    s_glyphs = 0;
    xst_clear_fb();   /* the blend would otherwise draw the second picture over the first */
    int ok = draw();
    uint32_t *off = ok ? gst_device() : NULL;
    s_glyphs = was;
    xst_clear_fb();
    ok = off && draw();
    uint32_t *on = ok ? gst_device() : NULL;
    int diff = on ? gst_diff_region(on, off, 0, 0, SCL_W / s_scale, SCL_H / s_scale) : -1;
    free(on); free(off);
    if (diff != 0 || s_cnt_gl_bound != bound0 || s_cnt_gl_images != images0)
        return gst_fail("refuse", "%s: %d device pixels differ with the feature on, %llu draws bound, %llu images made (want 0, 0, 0)",
                        what, diff, (unsigned long long)(s_cnt_gl_bound - bound0),
                        (unsigned long long)(s_cnt_gl_images - images0));
    return 1;
}

static int gst_draw_default(void) { return gst_cell(0, 30.0f, 30.0f); }
static int gst_draw_magnified(void) {
    return gst_sprite(30.0f, 30.0f, 2.0f * GST_CW, 2.0f * GST_CH, 0.0f, 0.0f, (float)GST_CW, (float)GST_CH);
}
static int gst_draw_small(void) { return gst_sprite(30.0f, 30.0f, 3.0f, (float)GST_CH, 0.0f, 0.0f, 3.0f, (float)GST_CH); }
static int gst_draw_past(void) { return gst_sprite(30.0f, 30.0f, (float)GST_CW, (float)GST_CH, 24.0f, 0.0f, 24.0f + GST_CW, (float)GST_CH); }

static int gst_cell_refuse(void) {
    static uint32_t page[GST_W * GST_H];
    gst_reset();
    /* a page in VRAM */
    gst_page(page, 0, 1, 0x00FFFFFFu);
    gst_put(XST_TEX, page);
    gst_state(XST_TEX, GST_W, GST_H);
    if (!gst_refused_draw("a page in VRAM", gst_draw_default)) return 0;
    /* a hard-edged page */
    gst_reset();
    for (int i = 0; i < GST_W * GST_H; i++) page[i] = (page[i] >> 24) ? 0xFFFFFFFFu : 0x00123456u;
    gst_put(GST_PAGE, page);
    gst_state(GST_PAGE, GST_W, GST_H);
    if (!gst_refused_draw("a hard-edged page", gst_draw_default)) return 0;
    /* two inked colours */
    gst_reset();
    gst_page(page, 0, 1, 0x00FFFFFFu);
    for (int y = 0; y < GST_CH; y++)
        for (int x = 0; x < GST_CW; x++)
            if (x < 6 && (page[y * GST_W + x] >> 24)) page[y * GST_W + x] = (page[y * GST_W + x] & 0xFF000000u) | 0x000000FFu;
    gst_put(GST_PAGE, page);
    gst_state(GST_PAGE, GST_W, GST_H);
    if (!gst_refused_draw("a two-colour page", gst_draw_default)) return 0;
    /* a good page, a draw that is not a glyph's */
    gst_reset();
    gst_page(page, 0, 1, 0x00FFFFFFu);
    gst_put(GST_PAGE, page);
    gst_state(GST_PAGE, GST_W, GST_H);
    if (!gst_refused_draw("a magnified sprite", gst_draw_magnified)) return 0;
    if (!gst_refused_draw("a rectangle 3 texels wide", gst_draw_small)) return 0;
    if (!gst_refused_draw("a rectangle past the page", gst_draw_past)) return 0;
    printf("gpu glyph selftest: PASS %-12s scale %d: VRAM page, hard edge, two colours, magnified, too small, past the page "
           "all draw as without the feature\n", "refuse", s_scale);
    return 1;
}

/* A color-test sprite over a glyph page (#2318): the test keys on the guest's texels, so the draw
 * never binds the rebuilt tile. Nearest REPLACE, which psp.frag tests exactly; the same draw with
 * the test off binds the tile, so the cell is not vacuous. */
static int gst_cell_color_test(void) {
    static uint32_t page[GST_W * GST_H];
    gst_reset();
    gst_page(page, 0, 1, 0x00FFFFFFu);
    gst_put(GST_PAGE, page);
    gst_state(GST_PAGE, GST_W, GST_H);
    s_ge->tex_filter = 0u;
    ge_state_note_external_write();
    const uint64_t bound0 = s_cnt_gl_bound;
    if (!gst_cell(0, 20.0f, 20.0f)) return gst_fail("color-test", "draw failed");
    if (s_cnt_gl_bound == bound0)
        return gst_fail("color-test", "the nearest draw with the test off bound no tile (vacuous)");
    s_ge->color_test_enable = 1;
    s_ge->color_test = 3u;                   /* NOTEQUAL a colour the page lacks: every texel passes */
    s_ge->color_ref = 0x123456u;
    s_ge->color_test_mask = 0x00FFFFFFu;
    ge_state_note_external_write();
    const uint64_t fell0 = s_replay_stats.fallback_prims;
    int ok = gst_refused_draw("a color-tested sprite", gst_draw_default);
    const uint64_t fell = s_replay_stats.fallback_prims - fell0;
    s_ge->color_test_enable = 0;
    ge_state_note_external_write();
    if (!ok) return 0;
    if (fell) return gst_fail("color-test", "%llu primitives took the software fallback (want 0)", (unsigned long long)fell);
    printf("gpu glyph selftest: PASS %-12s scale %d: a color-tested sprite draws the guest's texels, never the tile\n",
           "color-test", s_scale);
    return 1;
}

static int gst_cell_rewrite(void) {
    static uint32_t page[GST_W * GST_H], want0[GST_CW * 6 * GST_CH * 6], want1[GST_CW * 6 * GST_CH * 6];
    gst_reset();
    gst_page(page, 0, 1, 0x00FFFFFFu);
    gst_put(GST_PAGE, page);
    gst_state(GST_PAGE, GST_W, GST_H);
    int ok = gst_cell(0, 20.0f, 20.0f) && gst_cell(16, 60.0f, 20.0f);
    const uint64_t built0 = s_cnt_gl_built, kept0 = s_cnt_gl_kept, stale0 = s_cnt_gl_stale;
    (void)nk_glyph_rebuild(page, GST_W, GST_H, 0, 0, GST_CW, GST_CH, s_scale, want0);
    gst_page(page, 2, 1, 0x00FFFFFFu);        /* the guest rewrites cell A; cell B is unchanged */
    gst_put(GST_PAGE, page);
    gst_state(GST_PAGE, GST_W, GST_H);
    if (nk_glyph_rebuild(page, GST_W, GST_H, 0, 0, GST_CW, GST_CH, s_scale, want1) != NK_GLYPH_OK)
        return gst_fail("rewrite", "the rewritten glyph does not rebuild");
    ok = ok && gst_cell(0, 100.0f, 20.0f) && gst_cell(16, 140.0f, 20.0f);
    uint32_t *dev = ok ? gst_device() : NULL;
    if (!dev) return gst_fail("rewrite", "draw or readback failed");
    static uint32_t want_b[GST_CW * 6 * GST_CH * 6];
    (void)nk_glyph_rebuild(page, GST_W, GST_H, 16, 0, GST_CW, GST_CH, s_scale, want_b);
    const int new_off = gst_off_tile(dev, 100, 20, GST_CW, GST_CH, want1);
    const int old_match = gst_off_tile(dev, 100, 20, GST_CW, GST_CH, want0) == 0;
    const int b_off = gst_off_tile(dev, 140, 20, GST_CW, GST_CH, want_b);
    const uint64_t built = s_cnt_gl_built - built0, kept = s_cnt_gl_kept - kept0, stale = s_cnt_gl_stale - stale0;
    free(dev);
    if (new_off || old_match)
        return gst_fail("rewrite", "after the guest rewrote the cell: %d device pixels differ from the new tile, old tile %s",
                        new_off, old_match ? "drawn" : "not drawn");
    if (b_off) return gst_fail("rewrite", "the unchanged cell differs from its tile in %d device pixels", b_off);
    if (built != 1u || kept != 1u || stale != 1u)
        return gst_fail("rewrite", "tiles rebuilt %llu, kept %llu, stale %llu (want 1, 1, 1)", (unsigned long long)built,
                        (unsigned long long)kept, (unsigned long long)stale);
    printf("gpu glyph selftest: PASS %-12s scale %d: the new texels draw their own tile, the old one is never drawn, "
           "the unchanged cell keeps its tile\n", "rewrite", s_scale);
    return 1;
}

/* A rewrite that misses the bytes tex_hash samples (it hashes 16 bytes in every 32 of this 2 KiB page, texels 4 to 7 of
 * each 8 are not read): the cache sees the same hash and finds the rewrite through its shadow copy, and the glyph
 * code must find it too and draw the new tile. */
static int gst_cell_rewrite_gap(void) {
    static uint32_t page[GST_W * GST_H], other[GST_W * GST_H], want0[GST_CW * 6 * GST_CH * 6], want1[GST_CW * 6 * GST_CH * 6];
    gst_reset();
    gst_page(page, 0, 1, 0x00FFFFFFu);
    gst_put(GST_PAGE, page);
    gst_state(GST_PAGE, GST_W, GST_H);
    const uint64_t hash0 = tex_hash();
    int ok = gst_cell(0, 20.0f, 20.0f);
    (void)nk_glyph_rebuild(page, GST_W, GST_H, 0, 0, GST_CW, GST_CH, s_scale, want0);
    gst_page(other, 2, 1, 0x00FFFFFFu);
    for (int y = 0; y < GST_CH; y++)
        for (int x = 4; x < 8; x++) page[y * GST_W + x] = other[y * GST_W + x];   /* texels no sample reads */
    gst_put(GST_PAGE, page);
    gst_state(GST_PAGE, GST_W, GST_H);
    if (tex_hash() != hash0) return gst_fail("rewrite-gap", "the rewrite changed the sampled bytes; the cell tests nothing");
    if (nk_glyph_rebuild(page, GST_W, GST_H, 0, 0, GST_CW, GST_CH, s_scale, want1) != NK_GLYPH_OK)
        return gst_fail("rewrite-gap", "the rewritten glyph does not rebuild");
    ok = ok && gst_cell(0, 100.0f, 20.0f);
    uint32_t *dev = ok ? gst_device() : NULL;
    if (!dev) return gst_fail("rewrite-gap", "draw or readback failed");
    const int new_off = gst_off_tile(dev, 100, 20, GST_CW, GST_CH, want1);
    const int old_match = gst_off_tile(dev, 100, 20, GST_CW, GST_CH, want0) == 0;
    free(dev);
    if (new_off || old_match)
        return gst_fail("rewrite-gap", "after a rewrite the hash does not see: %d device pixels differ from the new tile, old tile %s",
                        new_off, old_match ? "drawn" : "not drawn");
    printf("gpu glyph selftest: PASS %-12s scale %d: a rewrite the content hash does not see draws its new tile\n", "rewrite-gap", s_scale);
    return 1;
}

#define GST_BLOB_W 512   /* wide enough for NK_GLYPH_PER_PRESENT + 12 cells, 5 texels apart */
/* A 4 x 6 soft blob of ink at cell i of a wide page. */
static void gst_blob_page(uint32_t *p, int cells) {
    for (int i = 0; i < GST_BLOB_W * 16; i++) p[i] = 0x00123456u;
    for (int c = 0; c < cells; c++)
        for (int y = 0; y < 6; y++)
            for (int x = 0; x < 4; x++) {
                const int a = 40 + ((x == 0 || x == 3 || y == 0 || y == 5) ? 0 : 150) + (c % 7) * 5;
                p[y * GST_BLOB_W + c * 5 + x] = ((uint32_t)a << 24) | 0x00FFFFFFu;
            }
}

/* Fade bars of one colour, each rising across its width: a shallow one (alpha 64 to 126 over 32 texels), a steeper one
 * (20 to 200 over 16), a four-texel one (85 to 255) and a two-texel one (64, 191, 255). Whether a draw is a rebuilt tile or the game's own, no row may
 * reverse: a ribbed gradient is worse than the soft one. */
/* The deepest run-back along any row or column of the w x h device pixels at (x, y) of the target, against a unimodal
 * profile (rises to the line's peak, falls after it), in 8-bit steps. */
static int gst_runback(const uint32_t *dev, int x, int y, int w, int h) {
    int worst = 0;
    for (int line = 0; line < w + h; line++) {
        const int vert = line >= h, n = vert ? h : w, fixed = vert ? line - h : line;
        int pk = 0, pv = -1;
        for (int i = 0; i < n; i++) {
            const int v = GST_ALPHA(dev[(size_t)(y + (vert ? i : fixed)) * SCL_W + (size_t)(x + (vert ? fixed : i))]);
            if (v > pv) { pv = v; pk = i; }
        }
        int m = 0;
        for (int i = 0; i <= pk; i++) {
            const int v = GST_ALPHA(dev[(size_t)(y + (vert ? i : fixed)) * SCL_W + (size_t)(x + (vert ? fixed : i))]);
            if (v > m) m = v;
            if (m - v > worst) worst = m - v;
        }
        m = 0;
        for (int i = n - 1; i >= pk; i--) {
            const int v = GST_ALPHA(dev[(size_t)(y + (vert ? i : fixed)) * SCL_W + (size_t)(x + (vert ? fixed : i))]);
            if (v > m) m = v;
            if (m - v > worst) worst = m - v;
        }
    }
    return worst;
}

/* A box shadow: a 6-texel box blurred by a box 2 texels wide, on a 16 x 16 page, offset by a fraction of a texel. */
static float gst_shadow_cov(float x, float y) {
    const float q = 0.37f;
    x -= q; y -= q * 0.6f;
    const float p = 2.0f;
    float lx = (x - (5.0f - p * 0.5f)) / p, rx = ((11.0f + p * 0.5f) - x) / p;
    float ly = (y - (5.0f - p * 0.5f)) / p, ry = ((11.0f + p * 0.5f) - y) / p;
    lx = lx < 0 ? 0 : (lx > 1 ? 1 : lx); rx = rx < 0 ? 0 : (rx > 1 ? 1 : rx);
    ly = ly < 0 ? 0 : (ly > 1 ? 1 : ly); ry = ry < 0 ? 0 : (ry > 1 ? 1 : ry);
    return (lx < rx ? lx : rx) * (ly < ry ? ly : ry);
}

static int gst_cell_gradient(void) {
    static uint32_t page[GST_BLOB_W * 16];
    static const struct { int w, base, step; const char *what; } bars[] = {
        {32, 64, 2, "shallow"}, {16, 20, 12, "steep"}, {4, 85, 85, "four-texel"}, {4, 64, 127, "two-texel"}};
    const int s = s_scale;
    for (size_t b = 0; b < sizeof(bars) / sizeof(bars[0]); b++) {
        const int w = bars[b].w;
        for (int i = 0; i < GST_BLOB_W * 16; i++) page[i] = 0x00123456u;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < w; x++) {
                const int a = bars[b].base + x * bars[b].step;
                page[y * GST_BLOB_W + x] = ((uint32_t)(a > 255 ? 255 : a) << 24) | 0x00FFFFFFu;
            }
        gst_reset();
        memcpy(SR_HOST(GST_PAGE), page, sizeof(page));
        sr_gpu_vram_dirty(GST_PAGE, sizeof(page));
        gst_state(GST_PAGE, GST_BLOB_W, 16);
        xst_clear_fb();
        int ok = gst_sprite(20.0f, 20.0f, (float)w, 8.0f, 0.0f, 0.0f, (float)w, 8.0f);
        uint32_t *dev = ok ? gst_device() : NULL;
        if (!dev) return gst_fail("gradient", "%s fade bar: draw or readback failed", bars[b].what);
        int flips = 0, last = 0;
        for (int x = 1; x < w * s; x++) {
            const size_t row = ((size_t)20 * s + (size_t)(4 * s)) * SCL_W + (size_t)20 * s;
            const int d = GST_ALPHA(dev[row + (size_t)x]) - GST_ALPHA(dev[row + (size_t)x - 1]);
            const int sgn = d > 0 ? 1 : (d < 0 ? -1 : 0);
            if (!sgn) continue;
            if (last && sgn != last) flips++;
            last = sgn;
        }
        free(dev);
        if (flips)
            return gst_fail("gradient", "%s fade bar: %d reversals along a row of a bar that only rises", bars[b].what, flips);
    }
    /* and a box shadow, a ramp two texels wide with corners: no row or column of the draw runs back */
    {
        for (int i = 0; i < GST_BLOB_W * 16; i++) page[i] = 0x00123456u;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                float a = 0.0f;
                for (int j = 0; j < 8; j++)
                    for (int i = 0; i < 8; i++) a += gst_shadow_cov((float)x + ((float)i + 0.5f) / 8.0f, (float)y + ((float)j + 0.5f) / 8.0f);
                const int ai = (int)(a / 64.0f * 255.0f + 0.5f);
                page[y * GST_BLOB_W + x] = ai ? ((uint32_t)ai << 24) | 0x00FFFFFFu : 0x00123456u;
            }
        gst_reset();
        memcpy(SR_HOST(GST_PAGE), page, sizeof(page));
        sr_gpu_vram_dirty(GST_PAGE, sizeof(page));
        gst_state(GST_PAGE, GST_BLOB_W, 16);
        xst_clear_fb();
        int ok = gst_sprite(20.0f, 20.0f, 16.0f, 16.0f, 0.0f, 0.0f, 16.0f, 16.0f);
        uint32_t *dev = ok ? gst_device() : NULL;
        if (!dev) return gst_fail("gradient", "box shadow: draw or readback failed");
        const int rb = gst_runback(dev, 20 * s, 20 * s, 16 * s, 16 * s);
        free(dev);
        if (rb > 24)
            return gst_fail("gradient", "box shadow: a row or column runs back by %d of 255", rb);
    }
    printf("gpu glyph selftest: PASS %-12s scale %d: fade bars and a box shadow of one colour draw without a ribbed row or column\n", "gradient", s);
    return 1;
}

static int gst_cell_cap(void) {
    static uint32_t page[GST_BLOB_W * 16];
    const int cells = NK_GLYPH_PER_PRESENT + 12;
    gst_reset();
    gst_blob_page(page, cells);
    memcpy(SR_HOST(GST_PAGE), page, sizeof(page));
    sr_gpu_vram_dirty(GST_PAGE, sizeof(page));
    gst_state(GST_PAGE, GST_BLOB_W, 16);
    const uint64_t built0 = s_cnt_gl_built, bound0 = s_cnt_gl_bound, capped0 = s_cnt_gl_capped;
    int ok = 1;
    for (int c = 0; ok && c < cells; c++)
        ok = gst_sprite((float)(4 + (c % 20) * 12), (float)(10 + (c / 20) * 12), 4.0f, 6.0f, (float)(c * 5), 0.0f,
                        (float)(c * 5 + 4), 6.0f);
    const uint64_t built = s_cnt_gl_built - built0, bound = s_cnt_gl_bound - bound0, capped = s_cnt_gl_capped - capped0;
    s_cnt_present_cpu++;   /* the next present */
    for (int c = 0; ok && c < cells; c++)
        ok = gst_sprite((float)(4 + (c % 20) * 12), (float)(100 + (c / 20) * 12), 4.0f, 6.0f, (float)(c * 5), 0.0f,
                        (float)(c * 5 + 4), 6.0f);
    const uint64_t built2 = s_cnt_gl_built - built0, bound2 = s_cnt_gl_bound - bound0;
    if (!ok) return gst_fail("cap", "draw failed");
    if (built != (uint64_t)NK_GLYPH_PER_PRESENT || bound != (uint64_t)NK_GLYPH_PER_PRESENT || capped == 0u)
        return gst_fail("cap", "in one present %llu tiles built and %llu bound, %llu draws held back (want %d, %d, >0)",
                        (unsigned long long)built, (unsigned long long)bound, (unsigned long long)capped,
                        NK_GLYPH_PER_PRESENT, NK_GLYPH_PER_PRESENT);
    if (built2 != (uint64_t)cells)
        return gst_fail("cap", "after the next present %llu tiles built (want %d); %llu bound", (unsigned long long)built2,
                        cells, (unsigned long long)bound2);
    printf("gpu glyph selftest: PASS %-12s scale %d: %d new rectangles in a present rebuild %d, the rest the next present\n",
           "cap", s_scale, cells, NK_GLYPH_PER_PRESENT);
    return 1;
}

/* The device image of the scene every mode draws, and its hash. */
static uint64_t gst_scene_hash(int *ok) {
    static uint32_t page[GST_W * GST_H];
    gst_reset();
    gst_page(page, 0, 1, 0x00FFFFFFu);
    gst_put(GST_PAGE, page);
    gst_state(GST_PAGE, GST_W, GST_H);
    *ok = gst_cell(0, 20.0f, 20.0f) && gst_cell(16, 60.0f, 20.0f) && gst_cell(0, 100.0f, 20.0f);
    uint32_t *dev = *ok ? gst_device() : NULL;
    if (!dev) { *ok = 0; return 0; }
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < (size_t)SCL_W * SCL_H; i++) { h ^= dev[i]; h *= 1099511628211ull; }
    const uint32_t *fb = (const uint32_t *)SR_HOST(XST_BASE);
    (void)target_readback(s_cur);
    for (size_t i = 0; i < (size_t)XST_STRIDE * FB_H; i++) { h ^= fb[i]; h *= 1099511628211ull; }
    free(dev);
    return h;
}

/* Under --authentic: the run drew the scene, nothing was rebuilt, and the hash is printed. */
int gegpu_glyph_authentic_selftest(void) {
    int ok = 0;
    const uint64_t h = gst_scene_hash(&ok);
    if (!ok) return gst_fail("authentic", "draw or readback failed");
    if (s_glyphs || s_glyphs_asked || s_cnt_gl_bound || s_cnt_gl_built || s_cnt_gl_decodes)
        return gst_fail("authentic", "an authentic run rebuilt glyphs (bound %llu, built %llu, page decodes %llu)",
                        (unsigned long long)s_cnt_gl_bound, (unsigned long long)s_cnt_gl_built,
                        (unsigned long long)s_cnt_gl_decodes);
    printf("gpu glyph selftest: PASS %-12s no glyph was rebuilt in an authentic run\n", "authentic");
    printf("GLYPH-AUTH-HASH %016llx\n", (unsigned long long)h);
    return 1;
}

/* The texture descriptor pool holds a set for each image an entry can have: its own, a scaled copy's (and the
 * anisotropic ones), and a glyph image's. A full pool draws new textures white. */
static int gst_cell_pool(void) {
    const uint32_t per_af = s_ace_af ? 2u : 1u;
    const uint32_t need = (uint32_t)MAX_TEX * (per_af + (xs_enabled() ? per_af : 0u) + 1u);
    if (s_tex_pool_sets < need)
        return gst_fail("pool", "the pool has %u sets and an entry with every image needs %u in all", s_tex_pool_sets, need);
    printf("gpu glyph selftest: PASS %-12s %u descriptor sets for %d entries\n", "pool", s_tex_pool_sets, MAX_TEX);
    return 1;
}

int gegpu_glyph_selftest(void) {
    /* The renderer clamps the scale it was asked for; a cell that ran at another scale would prove nothing. */
    const char *asked = getenv("SR_GPU_SCALE");
    if (asked && atoi(asked) != s_scale)
        return gst_fail("setup", "render scale %s was asked for and the renderer runs at %d", asked, s_scale);
    if (!s_glyphs) {
        if (s_scale < NK_GLYPH_MIN_FACTOR || s_scale > NK_GLYPH_MAX_FACTOR) {
            /* Render scale 1: nothing to sharpen. The scene draws as without the feature. */
            int ok = 0;
            (void)gst_scene_hash(&ok);
            if (!ok) return gst_fail("scale-1", "draw or readback failed");
            if (s_glyphs_asked == 0 || s_cnt_gl_bound || s_cnt_gl_built)
                return gst_fail("scale-1", "the setting was %s and %llu glyph draws were bound at render scale 1",
                                s_glyphs_asked ? "on" : "off", (unsigned long long)s_cnt_gl_bound);
            printf("gpu glyph selftest: PASS %-12s render scale %d: the setting is idle, nothing rebuilt\n", "scale-1", s_scale);
            return 1;
        }
        /* The setting is off in an Ace run (--glyphs off): the game's own picture. The sharp cell is the one this
         * feature changes, and it fails here, which shows it is not vacuous. */
        if (s_glyphs_asked) return gst_fail("setup", "SR_GLYPHS is on but the feature is off at render scale %d", s_scale);
        return gst_cell_sharp();
    }
    int ok = gst_cell_pool() && gst_cell_sharp() && gst_cell_refuse() && gst_cell_color_test() && gst_cell_rewrite() && gst_cell_rewrite_gap() && gst_cell_gradient() &&
             gst_cell_cap();
    if (ok) {
        /* No guest byte moved: the page and the CLUT are what the CPU left. */
        static uint32_t page[GST_W * GST_H];
        uint8_t clut[sizeof(s_ge->clutram)];
        gst_reset();
        gst_page(page, 0, 1, 0x00FFFFFFu);
        gst_put(GST_PAGE, page);
        gst_state(GST_PAGE, GST_W, GST_H);
        memcpy(clut, s_ge->clutram, sizeof(clut));
        const size_t vram = 0x00200000u, fb0 = XST_BASE & 0x001FFFFFu, fb1 = fb0 + XST_STRIDE * FB_H * 4u;
        uint8_t *before = (uint8_t *)malloc(vram);
        if (!before) return 0;
        memcpy(before, SR_HOST(0x04000000u), vram);
        ok = gst_cell(0, 20.0f, 20.0f) && gst_cell(16, 60.0f, 20.0f);
        (void)hook_vram_read(XST_TEX, 4096u);
        const uint8_t *now = (const uint8_t *)SR_HOST(0x04000000u);
        size_t bad = vram;
        for (size_t b = 0; b < vram; b++)
            if ((b < fb0 || b >= fb1) && now[b] != before[b]) { bad = b; break; }
        free(before);
        if (ok && (bad != vram || memcmp(page, SR_HOST(GST_PAGE), sizeof(page)) != 0 ||
                   memcmp(clut, s_ge->clutram, sizeof(clut)) != 0))
            ok = gst_fail("guest-bytes", "a guest byte changed (VRAM offset 0x%zx)", bad);
        else if (ok)
            printf("gpu glyph selftest: PASS %-12s the page, the CLUT and VRAM outside the frame are unchanged\n", "guest-bytes");
    }
    gst_reset();
    return ok;
}
