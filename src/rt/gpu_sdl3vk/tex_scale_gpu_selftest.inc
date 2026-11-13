/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * tex_scale_gpu_selftest.inc - texture scaling and the smart 2D filter on ge_gpu.c's production
 * path (included by ge_gpu.c in the SR_GPU_TEXSCALE_SELFTEST build; mk/selftests.mk
 * gpu-texscale-selftest). Synthetic textures only. The cells:
 *   scaled-draw      a 1:1 sprite draws the xBRZ copy, sampled on the copy's own lattice;
 *   large            a 512x512 texture at 4x (16 MiB) uploads in bands and draws the copy;
 *   not-ready        a texture whose scale has not finished draws unscaled, without waiting;
 *   rewrite          a texture the guest rewrites draws unscaled until its own copy lands, then
 *                    the new copy; never the old one (the build with
 *                    NK_TEX_SCALE_MUTANT_SKIP_CONTENT_CHECK binds any copy and must fail here,
 *                    for that reason);
 *   clut             a CLUT texture is scaled after the palette lookup and again for a new
 *                    palette;
 *   budget           with room for two copies, a third texture stays unscaled while both are
 *                    in use; once they are idle it frees them and its copy lands, and a freed
 *                    texture's copy comes back on a later draw;
 *   rtt              a render target sampled as a texture, bound directly or read back through
 *                    the cache, is never scaled, and its draws are byte-identical with
 *                    scaling on and off;
 *   onetoone         a 1:1 draw binds the unscaled texture even with a copy ready: without that
 *                    rule a cell of an atlas shows its neighbour at its edge (device pixels at a
 *                    render scale above 1), with it the draw is the unscaled one; a magnified
 *                    draw keeps the copy, identical with the rule on and off. The cells above
 *                    check copies through 1:1 sprites, so they run with the rule off;
 *   replace          a texture a synthetic replacement pack lists (texrepl.h, #1880) is drawn from the
 *                    pack's image from the draw that finds it, even at 1:1 with the 1:1 rule on; a rewrite to
 *                    unlisted content never draws the old image; an image of no whole factor is
 *                    counted and the texture drawn as made; an authentic start reads no pack;
 *   replace-pal      a CLUT8 texture a pack file named the way PPSSPP names it lists (key and palette
 *                    identity written out from PPSSPP's formula in the cell) is drawn from the pack's
 *                    image; the build with NK_TEXREPL_MUTANT_NO_CLUTFORMAT_XOR fails here;
 *   replace-only     (`--repl-only`) SR_TEX_REPLACE alone, with no texture scaling: the pack is read at
 *                    start, a listed texture is drawn from its image, an unlisted one as made;
 *   dump             the texture dump (gegpu_texdump_start), into a folder whose name is not ASCII, is
 *                    refused in an authentic run and writes nothing while off; once on, a texture already
 *                    in the cache and a newly decoded one are written under the names a pack gives them
 *                    (cache key and quick hash, worked out here), the folder reads back as a pack holding
 *                    the textures' own pixels (its own [games] line not reported), a file already there
 *                    (the player's own included) is never written again, a movie picture is skipped while
 *                    the movie writes there and written once it no longer does, the budget, the file
 *                    limit and the first failed write each stop the dump and say why on the row, an
 *                    address new in every present stops being dumped after its run while another goes
 *                    on, and is dumped again once it has held still, SR_TEX_DUMP_BUDGET_MB and SR_TEX_DUMP_MAX_FILES take a whole number
 *                    in range only, and a refusal names no full path;
 *   dump-xxh         under a textures.ini with xxh32 or xxh64 and ignoreAddress, files are named without
 *                    the address and found by the pack at another address;
 *   dump-pal         a CLUT8 texture is dumped under PPSSPP's name with its palette's colours;
 *   dump-mip         a mip-mapped texture's levels are dumped each at its size, level 1 as "_1";
 *   guest-bytes      after every cell, no guest VRAM byte outside the drawn frame and no CLUT
 *                    byte changes while the scales land;
 *   smart2d          at a render scale above 1, a 1:1 linear sprite samples nearest (every
 *                    device pixel is a whole texel), linear blurs without the filter, and a
 *                    perspective triangle keeps its filter.
 */

#include "../../../tests/native/synthetic_png.h"   /* the pack files the replace cell writes */

#define XST_BASE   (0x04000000u | 0x00160000u)
#define XST_RTT    (0x04000000u | 0x000C8000u)
#define XST_STRIDE 512u
#define XST_TEX    0x04040000u
#define XST_W      16
#define XST_H      16
#define XST_RGB    0x00FFFFFFu

static uint32_t xst_hash(uint32_t v) {
    v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16;
    return v;
}

/* A texture xBRZ changes: a field, two diagonals, a block and lone texels, in colours that
 * depend on `seed`. */
static void xst_pattern(uint32_t *t, uint32_t seed) {
    const uint32_t field = 0xFF000000u | (xst_hash(seed) & 0x7F7F7Fu);
    const uint32_t ink = 0xFF000000u | ((xst_hash(seed + 1u) & 0x7F7F7Fu) + 0x808080u);
    const uint32_t spot = 0xFF000000u | (xst_hash(seed + 2u) & 0xFFFFFFu);
    for (int y = 0; y < XST_H; y++)
        for (int x = 0; x < XST_W; x++) {
            uint32_t c = field;
            if (x == y || x == 2 * y - 6) c = ink;
            if (x >= 10 && x < 14 && y >= 2 && y < 6) c = ink;
            if ((x * 5 + y * 3) % 23 == 0) c = spot;
            t[y * XST_W + x] = c;
        }
}

static void xst_upload(const uint32_t *t) {
    memcpy(SR_HOST(XST_TEX), t, sizeof(uint32_t) * XST_W * XST_H);
    sr_gpu_vram_dirty(XST_TEX, sizeof(uint32_t) * XST_W * XST_H);
}

/* GE state for a 16x16 8888 VRAM texture drawn by REPLACE into the 8888 target at `base`. */
static void xst_state(uint32_t base, uint32_t tex_addr, uint32_t fmt, int linear, int clamp) {
    wrap_state(base, XST_STRIDE);
    s_ge->tex_enable = 1;
    s_ge->tex_addr = tex_addr;
    s_ge->tex_bufw = XST_W;
    s_ge->tex_fmt = fmt;
    s_ge->tex_w = XST_W;
    s_ge->tex_h = XST_H;
    s_ge->tex_func = 3;                      /* REPLACE: the framebuffer shows the texel */
    s_ge->tex_filter = linear ? (1u | 0x100u) : 0u;
    s_ge->tex_wrap = clamp ? 0x101u : 0u;
    s_ge->vtype = 1u << 23;                  /* through mode */
    ge_state_note_external_write();
}

/* A 1:1 sprite of the whole texture at guest (x, y), read back to guest VRAM. */
static int xst_draw(float x, float y) {
    GeVtx p0, p1;
    wrap_vtx(&p0, x, y, 0xFFFFFFFFu); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, x + XST_W, y + XST_H, 0xFFFFFFFFu); p1.u = (float)XST_W; p1.v = (float)XST_H;
    return hook_sprite(&p0, &p1, 0) && s_cur && target_readback(s_cur);
}

/* The coordinate device pixel k samples on an axis of a 1:1 through-mode sprite that spans
 * coordinates c0 .. c0 + n: its own centre, c0 + (k + 0.5) / scale (hook_sprite, #2104), clamped
 * above 1x to the first and last guest pixel centres (uv_edge_axis). At 1x it is the pixel centre
 * c0 + k + 0.5, so every value below is the one the PSP samples there. */
static float xst_axis_u(float c0, int k, int n) {
    float u = ((float)k + 0.5f) / (float)s_scale;
    if (s_scale > 1) {
        if (u < 0.5f) u = 0.5f;
        if (u > (float)n - 0.5f) u = (float)n - 0.5f;
    }
    return c0 + u;
}

/* The texel of a factor-`f` copy (f = 1: the texture) that guest pixel i reads back: the one its
 * top-left device sample, device pixel i * scale, falls in. */
static size_t xst_readback_texel(float c0, int i, int n, int f) {
    return (size_t)floorf((float)f * xst_axis_u(c0, i * s_scale, n));
}

/* What a 1:1 nearest sprite of texture `t` reads back from a factor-`f` copy (f = 0: the
 * texture itself). */
static void xst_expect(const uint32_t *t, int f, int clamp, uint32_t *want) {
    if (f < 2) { memcpy(want, t, sizeof(uint32_t) * XST_W * XST_H); return; }
    uint32_t *big = (uint32_t *)malloc(sizeof(uint32_t) * XST_W * XST_H * (size_t)f * (size_t)f);
    if (!big || !nk_tex_scale_image(t, XST_W, XST_H, f, clamp, clamp, big)) {
        memset(want, 0, sizeof(uint32_t) * XST_W * XST_H);
        free(big);
        return;
    }
    for (int y = 0; y < XST_H; y++)
        for (int x = 0; x < XST_W; x++)
            want[y * XST_W + x] = big[xst_readback_texel(0.0f, y, XST_H, f) * XST_W * (size_t)f +
                                     xst_readback_texel(0.0f, x, XST_W, f)];
    free(big);
}

static int xst_matches(int x0, int y0, const uint32_t *want) {
    const uint32_t *fb = (const uint32_t *)SR_HOST(XST_BASE);
    for (int y = 0; y < XST_H; y++)
        for (int x = 0; x < XST_W; x++)
            if ((fb[(y0 + y) * XST_STRIDE + x0 + x] & XST_RGB) != (want[y * XST_W + x] & XST_RGB)) return 0;
    return 1;
}

static int xst_first_diff(int x0, int y0, const uint32_t *want, int *dx, int *dy, uint32_t *got) {
    const uint32_t *fb = (const uint32_t *)SR_HOST(XST_BASE);
    for (int y = 0; y < XST_H; y++)
        for (int x = 0; x < XST_W; x++) {
            uint32_t g = fb[(y0 + y) * XST_STRIDE + x0 + x] & XST_RGB;
            if (g != (want[y * XST_W + x] & XST_RGB)) { *dx = x; *dy = y; *got = g; return 1; }
        }
    return 0;
}

static int xst_report(const char *cell, int x0, int y0, const uint32_t *want, const char *what) {
    int dx = 0, dy = 0;
    uint32_t got = 0;
    if (!xst_first_diff(x0, y0, want, &dx, &dy, &got)) return 1;
    fprintf(stderr, "gpu texscale selftest: FAIL %s: %s (texel %d,%d shows %06x, want %06x)\n",
            cell, what, dx, dy, got, want[dy * XST_W + dx] & XST_RGB);
    return 0;
}

static void xst_clear_fb(void) {
    memset(SR_HOST(XST_BASE), 0, XST_STRIDE * FB_H * 4u);
    sr_gpu_vram_dirty(XST_BASE, XST_STRIDE * FB_H * 4u);
}

/* The scaled copies and their jobs, from a clean cache. */
static void xst_reset(void) {
    coherence_reset_targets();
    (void)submit_pending();
    (void)cmd_drain(GEGPU_BOUNDARY_LIFETIME);
    while (s_tex_n) {                       /* the whole cache, as tex_evict_lru frees an entry */
        TexEnt *e = &s_tex[--s_tex_n];
        tex_shadow_release(e);
        vkFreeDescriptorSets(s_dev, s_dpool_tex, 1, &e->set);
        vkDestroySampler(s_dev, e->smp, NULL);
        if (e->set_af) vkFreeDescriptorSets(s_dev, s_dpool_tex, 1, &e->set_af);
        if (e->smp_af) vkDestroySampler(s_dev, e->smp_af, NULL);
        vkDestroyImageView(s_dev, e->view, NULL);
        vkDestroyImage(s_dev, e->img, NULL);
        vkFreeMemory(s_dev, e->mem, NULL);
        tex_xs_free(e);
        tex_gl_free(e);
    }
    nk_tex_scale_set_hold(0);
    nk_tex_scale_wait_idle();
    NkTexScaleResult r;
    while (nk_tex_scale_take(&r)) nk_tex_scale_release(&r);
    xst_clear_fb();
    s_cnt_present_cpu++;   /* a new present: the per-present upload bytes start again */
}

/* Read a target's whole device-resolution image (RGBA8, R in the low byte). */
static int xst_read_device(Target *t, uint32_t *out) {
    if (s_nbatch && !submit_pending()) return 0;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void *map = NULL;
    const VkDeviceSize bytes = (VkDeviceSize)SCL_W * SCL_H * 4u;
    if (!make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &buf, &mem, &map)) return 0;
    int ok = cmd_begin();
    if (ok) {
        to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c;
        memset(&c, 0, sizeof(c));
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.layerCount = 1;
        c.imageExtent.width = SCL_W; c.imageExtent.height = SCL_H; c.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(s_cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
        ok = cmd_submit_wait(SR_PERF_GE_TARGET_READBACK_TRANSITION);
    }
    if (ok) memcpy(out, map, (size_t)bytes);
    vkDestroyBuffer(s_dev, buf, NULL);
    vkFreeMemory(s_dev, mem, NULL);
    return ok;
}

/* Device pixels of the 1:1 sprite drawn at guest (x0, y0) that are not the nearest texel of
 * `t`, an image of the texture at `f` times its size (f = 1: the texture itself). Device pixel k
 * samples coordinate xst_axis_u(0, k, n), which is texel floor(f * u) of the copy. */
static int xst_device_blends(const uint32_t *dev, int x0, int y0, const uint32_t *t, int f) {
    const int s = s_scale, tw = XST_W * f, th = XST_H * f;
    int blends = 0;
    for (int ky = 0; ky < XST_H * s; ky++)
        for (int kx = 0; kx < XST_W * s; kx++) {
            int tu = (int)floorf((float)f * xst_axis_u(0.0f, kx, XST_W));
            int tv = (int)floorf((float)f * xst_axis_u(0.0f, ky, XST_H));
            if (tu > tw - 1) tu = tw - 1;
            if (tv > th - 1) tv = th - 1;
            uint32_t got = dev[((size_t)y0 * (size_t)s + (size_t)ky) * SCL_W + (size_t)x0 * (size_t)s + (size_t)kx] & XST_RGB;
            if (got != (t[(size_t)tv * tw + tu] & XST_RGB)) blends++;
        }
    return blends;
}

static int xst_cell_scaled_draw(int f) {
    uint32_t tex[XST_W * XST_H], want[XST_W * XST_H], plain[XST_W * XST_H];
    xst_reset();
    nk_tex_scale_set_sync(1);
    xst_pattern(tex, 11u);
    xst_upload(tex);
    xst_expect(tex, f, 1, want);
    xst_expect(tex, 0, 1, plain);
    /* At 1x the guest pixel samples the copy at its texel centre's lattice point (f*i + f/2). From 3x
     * xBRZ often keeps that point, so only the device image at a render scale above 1 can tell
     * the copy from the texture. */
    const int guest_tells = memcmp(want, plain, sizeof(want)) != 0;
    if (!guest_tells && s_scale < 2) {
        fprintf(stderr, "gpu texscale selftest: FAIL scaled-draw: at factor %d and render scale 1 the copy and "
                        "the texture draw alike (vacuous); run this factor at a render scale above 1\n", f);
        return 0;
    }
    xst_state(XST_BASE, XST_TEX, 3u, 1, 1);
    /* The first draw decodes and (sync) scales; the copy is stored at the next lookup. */
    const uint64_t bound0 = s_cnt_xs_bound;
    int ok = xst_draw(20.0f, 20.0f);
    s_tmpl_ok = 0;   /* force a fresh state build, as the next draw of a new list would */
    ok = ok && xst_draw(60.0f, 20.0f);
    if (!ok) { fprintf(stderr, "gpu texscale selftest: FAIL scaled-draw: draw or readback failed\n"); return 0; }
    if (!xst_report("scaled-draw", 20, 20, plain, "the first draw, before the copy was stored, is not the texture"))
        return 0;
    if (!xst_report("scaled-draw", 60, 20, want, "the second draw is not the xBRZ copy on its own lattice"))
        return 0;
    if (s_cnt_xs_bound == bound0) {
        fprintf(stderr, "gpu texscale selftest: FAIL scaled-draw: no scaled bind was counted\n");
        return 0;
    }
    int device_diff = -1;
    if (s_scale > 1) {
        uint32_t *dev = (uint32_t *)malloc((size_t)SCL_W * SCL_H * 4u);
        uint32_t *big = (uint32_t *)malloc(sizeof(uint32_t) * XST_W * XST_H * (size_t)f * (size_t)f);
        ok = dev && big && s_cur && xst_read_device(s_cur, dev) && nk_tex_scale_image(tex, XST_W, XST_H, f, 1, 1, big);
        const int first_plain = ok ? xst_device_blends(dev, 20, 20, tex, 1) : -1;
        const int second_copy = ok ? xst_device_blends(dev, 60, 20, big, f) : -1;
        device_diff = ok ? xst_device_blends(dev, 60, 20, tex, 1) : -1;
        free(dev);
        free(big);
        if (!ok || first_plain != 0 || second_copy != 0 || device_diff == 0) {
            fprintf(stderr, "gpu texscale selftest: FAIL scaled-draw: device pixels off the expected texel: %d in the "
                            "unscaled draw, %d in the scaled draw; %d differ from the texture (want 0, 0, some)\n",
                    first_plain, second_copy, device_diff);
            return 0;
        }
    }
    printf("gpu texscale selftest: PASS %-12s factor=%d render-scale=%d device-pixels-from-copy=%d\n", "scaled-draw",
           f, s_scale, device_diff);
    return 1;
}

/* The largest copy: a 512x512 texture at factor 4 is 2048x2048, 16 MiB, more than the upload ring
 * slot and the shared staging buffer hold (a title's 512x512 font page crashed the runtime here
 * before the copy went up in bands). Main RAM, so no target or depth buffer can overlap it: the
 * kernel partition, which the self-test owns and no game address names. */
#define XST_BIG_ADDR 0x08400000u
#define XST_BIG      512
static int xst_cell_large(int f) {
    (void)f;
    const int factor = 4;
    uint32_t *tex = (uint32_t *)SR_HOST(XST_BIG_ADDR);
    for (int y = 0; y < XST_BIG; y++)
        for (int x = 0; x < XST_BIG; x++) {
            uint32_t c = 0xFF000000u | (xst_hash(0x5000u + (uint32_t)((x / 3) * 977 + (y / 5))) & 0xFFFFFFu);
            if (x == y || x + y == XST_BIG - 1) c = 0xFFFFFFFFu;
            tex[y * XST_BIG + x] = c;
        }
    uint32_t *big = (uint32_t *)malloc(sizeof(uint32_t) * XST_BIG * XST_BIG * (size_t)factor * (size_t)factor);
    if (!big || !nk_tex_scale_image(tex, XST_BIG, XST_BIG, factor, 1, 1, big)) {
        free(big);
        fprintf(stderr, "gpu texscale selftest: FAIL large: reference scale failed\n");
        return 0;
    }
    const int saved = s_xs_factor;
    xst_reset();
    s_xs_factor = factor;
    nk_tex_scale_set_sync(1);
    xst_state(XST_BASE, XST_BIG_ADDR, 3u, 1, 1);
    s_ge->tex_bufw = XST_BIG;
    s_ge->tex_w = XST_BIG;
    s_ge->tex_h = XST_BIG;
    ge_state_note_external_write();
    /* 1:1 sprites of the top-left and bottom-right 16x16 corners: the bands' first and last. */
    const uint64_t uploads0 = s_cnt_xs_uploads;
    int ok = 1;
    for (int pass = 0; ok && pass < 2; pass++) {
        GeVtx p0, p1;
        const float u0 = pass ? (float)(XST_BIG - XST_W) : 0.0f, v0 = pass ? (float)(XST_BIG - XST_H) : 0.0f;
        wrap_vtx(&p0, 20.0f, 160.0f, 0xFFFFFFFFu); p0.u = u0; p0.v = v0;
        wrap_vtx(&p1, 20.0f + XST_W, 160.0f + XST_H, 0xFFFFFFFFu); p1.u = u0 + XST_W; p1.v = v0 + XST_H;
        s_tmpl_ok = 0;
        ok = hook_sprite(&p0, &p1, 0) && s_cur && target_readback(s_cur);   /* decodes, scales, queues */
        s_tmpl_ok = 0;
        ok = ok && hook_sprite(&p0, &p1, 0) && s_cur && target_readback(s_cur);   /* stores, binds */
        if (!ok) break;
        uint32_t want[XST_W * XST_H];
        for (int y = 0; y < XST_H; y++)
            for (int x = 0; x < XST_W; x++)
                want[y * XST_W + x] = big[xst_readback_texel(v0, y, XST_H, factor) * XST_BIG * (size_t)factor +
                                          xst_readback_texel(u0, x, XST_W, factor)];
        ok = xst_report("large", 20, 160, want, pass ? "the last band of the 16 MiB copy is wrong"
                                                     : "the first band of the 16 MiB copy is wrong");
    }
    free(big);
    const uint64_t uploads = s_cnt_xs_uploads - uploads0;
    s_xs_factor = saved;
    xst_reset();
    if (!ok) return 0;
    if (uploads != 1u) {
        fprintf(stderr, "gpu texscale selftest: FAIL large: %llu scaled uploads, want 1\n", (unsigned long long)uploads);
        return 0;
    }
    printf("gpu texscale selftest: PASS %-12s 512x512 at 4x (16 MiB) uploaded in bands and drawn\n", "large");
    return 1;
}

/* The store bound: at most XS_UPLOAD_BYTES_PER_PRESENT of copies are stored per present, the first
 * always. Three 512x512 textures at 2x (4 MiB copies) are scaled in one present: the first two
 * copies land, the third waits in the job table through further draws of that present and lands at
 * the next one. Without the bound the third lands at once. */
#define XST_UP_ADDR(i) (XST_BIG_ADDR + (uint32_t)(i) * 0x00100000u)
static TexEnt *xst_entry_at(uint32_t addr) {
    for (int i = 0; i < s_tex_n; i++)
        if (s_tex[i].addr == addr) return &s_tex[i];
    return NULL;
}
/* A colour-keyed sprite (#2318): the GE color test keys on the guest's texels, so a color-test draw
 * binds the texture, never its copy, whose texels are other colours and blend the key into their
 * edges. Nearest REPLACE through a 1:1 sprite, which psp.frag tests exactly, with the copy ready. */
static int xst_cell_color_test(int f) {
    uint32_t tex[XST_W * XST_H], want[XST_W * XST_H];
    xst_reset();
    nk_tex_scale_set_sync(1);
    xst_pattern(tex, 11u);
    xst_upload(tex);
    const uint32_t key = tex[0] & XST_RGB;   /* the field colour */
    unsigned keyed = 0;
    for (int i = 0; i < XST_W * XST_H; i++) {
        const int k = (tex[i] & XST_RGB) == key;
        keyed += (unsigned)k;
        want[i] = k ? 0u : tex[i];           /* a dropped pixel keeps the cleared frame */
    }
    xst_state(XST_BASE, XST_TEX, 3u, 0, 1);
    s_ge->color_test_enable = 1;
    s_ge->color_test = 3u;                   /* NOTEQUAL: the key colour is dropped */
    s_ge->color_ref = key;
    s_ge->color_test_mask = XST_RGB;
    ge_state_note_external_write();
    const uint64_t bound0 = s_cnt_xs_bound, fell0 = s_replay_stats.fallback_prims;
    int ok = xst_draw(20.0f, 20.0f);         /* decodes and (sync) scales */
    s_tmpl_ok = 0;
    ok = ok && xst_draw(60.0f, 20.0f);       /* the copy is stored at this lookup */
    s_tmpl_ok = 0;
    ok = ok && xst_draw(100.0f, 20.0f);      /* and would be bound here */
    const TexEnt *e = xst_entry_at(XST_TEX);
    const int ready = e && tex_xs_bindable(e) && tex_xs_eligible(e, 1u);
    s_ge->color_test_enable = 0;
    ge_state_note_external_write();
    if (!ok) { fprintf(stderr, "gpu texscale selftest: FAIL color-test: draw or readback failed\n"); return 0; }
    if (!ready || keyed == 0u || keyed == (unsigned)(XST_W * XST_H)) {
        fprintf(stderr, "gpu texscale selftest: FAIL color-test: no copy was ready (%d) or the key matched %u of %d "
                        "texels (vacuous)\n", ready, keyed, XST_W * XST_H);
        return 0;
    }
    if (s_replay_stats.fallback_prims != fell0) {
        fprintf(stderr, "gpu texscale selftest: FAIL color-test: %llu primitives took the software fallback (want 0)\n",
                (unsigned long long)(s_replay_stats.fallback_prims - fell0));
        return 0;
    }
    if (s_cnt_xs_bound != bound0) {
        fprintf(stderr, "gpu texscale selftest: FAIL color-test: a color-test draw bound the scaled copy %llu times "
                        "(want 0)\n", (unsigned long long)(s_cnt_xs_bound - bound0));
        return 0;
    }
    if (!xst_report("color-test", 20, 20, want, "the first draw is not the keyed texture") ||
        !xst_report("color-test", 100, 20, want, "the draw with the copy ready is not the keyed texture"))
        return 0;
    printf("gpu texscale selftest: PASS %-12s factor=%d render-scale=%d: with the copy ready, the keyed draw is the "
           "texture (%u texels keyed)\n", "color-test", f, s_scale, keyed);
    return 1;
}

static int xst_up_draw(uint32_t addr) {
    xst_state(XST_BASE, addr, 3u, 1, 1);
    s_ge->tex_bufw = XST_BIG;
    s_ge->tex_w = XST_BIG;
    s_ge->tex_h = XST_BIG;
    ge_state_note_external_write();
    GeVtx p0, p1;
    wrap_vtx(&p0, 20.0f, 160.0f, 0xFFFFFFFFu); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 20.0f + XST_W, 160.0f + XST_H, 0xFFFFFFFFu); p1.u = (float)XST_W; p1.v = (float)XST_H;
    s_tmpl_ok = 0;
    return hook_sprite(&p0, &p1, 0);
}
static int xst_up_ready(uint32_t addr) {
    const TexEnt *e = xst_entry_at(addr);
    return e && e->xs.state == XS_READY;
}
static int xst_cell_upload_cap(int f) {
    (void)f;
    const int factor = 2, saved = s_xs_factor;
    const uint64_t copy = (uint64_t)XST_BIG * XST_BIG * (uint64_t)factor * (uint64_t)factor * 4u;
    xst_reset();
    s_xs_factor = factor;
    nk_tex_scale_set_sync(1);
    for (uint32_t i = 0; i < 3u; i++) {
        uint32_t *t = (uint32_t *)SR_HOST(XST_UP_ADDR(i));
        for (int k = 0; k < XST_BIG * XST_BIG; k++)
            t[k] = 0xFF000000u | (xst_hash(0x7000u + i * 0x40000u + (uint32_t)(k / 7)) & 0xFFFFFFu);
    }
    const uint64_t up0 = s_cnt_xs_uploads, cap0 = s_cnt_xs_upload_capped;
    /* One present: each draw stores the copy the draw before it scaled (sync), then scales its own. */
    int ok = xst_up_draw(XST_UP_ADDR(0)) && xst_up_draw(XST_UP_ADDR(1)) && xst_up_draw(XST_UP_ADDR(2));
    ok = ok && xst_up_draw(XST_UP_ADDR(2)) && xst_up_draw(XST_UP_ADDR(1));   /* C's copy is done, and waits */
    const int a1 = xst_up_ready(XST_UP_ADDR(0)), b1 = xst_up_ready(XST_UP_ADDR(1)), c1 = xst_up_ready(XST_UP_ADDR(2));
    const uint64_t up1 = s_cnt_xs_uploads - up0, cap1 = s_cnt_xs_upload_capped - cap0;
    s_cnt_present_cpu++;   /* the next present */
    ok = ok && xst_up_draw(XST_UP_ADDR(2));
    const int c2 = xst_up_ready(XST_UP_ADDR(2));
    const uint64_t up2 = s_cnt_xs_uploads - up0;
    s_xs_factor = saved;
    xst_reset();
    if (!ok) { fprintf(stderr, "gpu texscale selftest: FAIL upload-cap: draw failed\n"); return 0; }
    if (!a1 || !b1 || c1 || up1 != 2u || cap1 != 1u || !c2 || up2 != 3u) {
        fprintf(stderr, "gpu texscale selftest: FAIL upload-cap: in one present A/B/C stored %d/%d/%d (want 1/1/0), "
                        "%llu copies stored (want 2), capped %llu (want 1); at the next present C stored %d, %llu "
                        "copies (want 1, 3)\n", a1, b1, c1, (unsigned long long)up1, (unsigned long long)cap1, c2,
                (unsigned long long)up2);
        return 0;
    }
    printf("gpu texscale selftest: PASS %-12s %llu MiB copies: two stored in one present, the third at the next\n",
           "upload-cap", (unsigned long long)(copy >> 20));
    return 1;
}

static int xst_cell_not_ready(int f) {
    uint32_t tex[XST_W * XST_H], want[XST_W * XST_H];
    xst_reset();
    nk_tex_scale_set_sync(0);
    nk_tex_scale_set_hold(1);
    xst_pattern(tex, 23u);
    xst_upload(tex);
    xst_state(XST_BASE, XST_TEX, 3u, 1, 1);
    const uint64_t t0 = SDL_GetTicksNS();
    int ok = xst_draw(20.0f, 60.0f);
    s_tmpl_ok = 0;
    ok = ok && xst_draw(60.0f, 60.0f);
    const uint64_t spent_ms = (SDL_GetTicksNS() - t0) / 1000000u;
    xst_expect(tex, 0, 1, want);
    if (!ok || !xst_report("not-ready", 20, 60, want, "a texture with its scale queued is not drawn unscaled") ||
        !xst_report("not-ready", 60, 60, want, "a held scale was drawn"))
        return 0;
    /* Released, the copy lands at a later lookup. */
    nk_tex_scale_set_hold(0);
    nk_tex_scale_wait_idle();
    s_tmpl_ok = 0;
    ok = xst_draw(100.0f, 60.0f);
    xst_expect(tex, f, 1, want);
    if (!ok || !xst_report("not-ready", 100, 60, want, "the copy did not land once its scale finished")) return 0;
    printf("gpu texscale selftest: PASS %-12s held draws unscaled in %llu ms, then the copy\n", "not-ready",
           (unsigned long long)spent_ms);
    return 1;
}

static int xst_cell_rewrite(int f) {
    uint32_t a[XST_W * XST_H], b[XST_W * XST_H], want[XST_W * XST_H], stale[XST_W * XST_H];
    xst_reset();
    nk_tex_scale_set_sync(1);
    xst_pattern(a, 31u);
    xst_pattern(b, 47u);
    xst_upload(a);
    xst_state(XST_BASE, XST_TEX, 3u, 1, 1);
    int ok = xst_draw(20.0f, 100.0f);
    s_tmpl_ok = 0;
    ok = ok && xst_draw(20.0f, 100.0f);
    xst_expect(a, f, 1, want);
    if (!ok || !xst_report("rewrite", 20, 100, want, "the first content's copy is not drawn")) return 0;
    /* The guest rewrites the texture; its scale is held, so only the content check stands
     * between the draw and the old copy. */
    nk_tex_scale_set_sync(0);
    nk_tex_scale_set_hold(1);
    xst_upload(b);
    s_tmpl_ok = 0;
    ok = xst_draw(60.0f, 100.0f);
    xst_expect(b, 0, 1, want);
    xst_expect(a, f, 1, stale);
    if (!ok) { fprintf(stderr, "gpu texscale selftest: FAIL rewrite: draw failed\n"); return 0; }
    if (xst_matches(60, 100, stale)) {
        fprintf(stderr, "gpu texscale selftest: FAIL rewrite: stale scaled copy of the previous content was drawn "
                        "after the guest rewrote the texture\n");
        return 0;
    }
    if (!xst_report("rewrite", 60, 100, want, "the rewritten texture is not drawn unscaled while its scale waits"))
        return 0;
    nk_tex_scale_set_hold(0);
    nk_tex_scale_wait_idle();
    s_tmpl_ok = 0;
    ok = xst_draw(100.0f, 100.0f);
    xst_expect(b, f, 1, want);
    if (!ok || !xst_report("rewrite", 100, 100, want, "the rewritten texture's own copy did not land")) return 0;
    printf("gpu texscale selftest: PASS %-12s rewritten texture rescaled, the old copy never drawn\n", "rewrite");
    return 1;
}

static int xst_cell_clut(int f) {
    uint8_t *idx = (uint8_t *)SR_HOST(XST_TEX);
    uint32_t pat[XST_W * XST_H], lut[XST_W * XST_H], want[XST_W * XST_H];
    xst_reset();
    nk_tex_scale_set_sync(1);
    xst_pattern(pat, 5u);
    for (int i = 0; i < XST_W * XST_H; i++) idx[i] = (uint8_t)(pat[i] & 0x0Fu);   /* 16 indices */
    sr_gpu_vram_dirty(XST_TEX, XST_W * XST_H);
    for (int pass = 0; pass < 2; pass++) {
        xst_state(XST_BASE, XST_TEX, 5u, 1, 1);   /* CLUT8; the state reset clears the palette */
        for (uint32_t j = 0; j < 256u; j++) {
            uint32_t entry = 0xFF000000u | (xst_hash(0x900u + j * 3u + (uint32_t)pass * 977u) & 0xFFFFFFu);
            memcpy(&s_ge->clutram[j * 4u], &entry, 4);
        }
        s_ge->clut_fmt = 3u | (0xFFu << 8);       /* 8888, shift 0, mask 0xFF */
        ge_state_note_external_write();
        for (int i = 0; i < XST_W * XST_H; i++) memcpy(&lut[i], &s_ge->clutram[idx[i] * 4u], 4);
        const float y = 140.0f + 40.0f * (float)pass;
        int ok = xst_draw(20.0f, y);
        s_tmpl_ok = 0;
        ok = ok && xst_draw(60.0f, y);
        xst_expect(lut, f, 1, want);
        if (!ok || !xst_report(pass ? "clut-new-palette" : "clut", 60, (int)y, want,
                               "the copy is not the scale of the palette lookup"))
            return 0;
    }
    printf("gpu texscale selftest: PASS %-12s scaled after the lookup, rescaled for a new palette\n", "clut");
    return 1;
}

/* A render target sampled as a texture: drawn the same with scaling on and off, and no scaled
 * copy is made or bound for it. */
static int xst_rtt_pass(int factor, int swizzle, uint8_t *out) {
    uint32_t tex[XST_W * XST_H];
    xst_reset();
    nk_tex_scale_set_sync(1);
    s_xs_factor = factor;
    memset(SR_HOST(XST_RTT), 0, XST_STRIDE * FB_H * 4u);
    sr_gpu_vram_dirty(XST_RTT, XST_STRIDE * FB_H * 4u);
    xst_pattern(tex, 71u);
    xst_upload(tex);
    /* Render into the second target: it becomes a GPU-valid render target. */
    xst_state(XST_RTT, XST_TEX, 3u, 1, 1);
    GeVtx p0, p1;
    wrap_vtx(&p0, 0.0f, 0.0f, 0xFFFFFFFFu); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 64.0f, 64.0f, 0xFFFFFFFFu); p1.u = (float)XST_W; p1.v = (float)XST_H;
    s_xs_factor = 0;                         /* the same target content in both passes */
    int ok = hook_sprite(&p0, &p1, 0) && submit_pending();
    s_xs_factor = factor;
    /* Sample it from the first target: bound directly (8888, linear), or, swizzled, read back
     * and decoded through the texture cache. */
    xst_state(XST_BASE, XST_RTT, 3u, 1, 1);
    s_ge->tex_bufw = XST_STRIDE;
    s_ge->tex_swizzle = (uint32_t)swizzle;
    ge_state_note_external_write();
    ok = ok && xst_draw(20.0f, 20.0f) && xst_draw(20.0f, 60.0f);
    if (ok) memcpy(out, SR_HOST(XST_BASE), XST_STRIDE * FB_H * 4u);
    for (int i = 0; i < s_tex_n; i++)
        if (s_tex[i].addr == XST_RTT && (s_tex[i].xs.state != XS_NONE || s_tex[i].xs.factor)) {
            fprintf(stderr, "gpu texscale selftest: FAIL rtt: a render target has a scaled copy (state %d)\n",
                    s_tex[i].xs.state);
            ok = 0;
        }
    return ok;
}

/* With room for two copies: A and B are scaled and in use, so C stays unscaled and no copy in use
 * is freed. Once A and B have been idle XS_IDLE_PRESENTS presents, C's next draw frees them and its
 * copy lands; A, whose copy was freed, is scaled again on its next draws. */
#define XST_TEX_B (XST_TEX + 0x1000u)
#define XST_TEX_C (XST_TEX + 0x2000u)
static int xst_budget_draw(uint32_t addr, const uint32_t *t, float x, int f, const char *what) {
    uint32_t want[XST_W * XST_H];
    xst_state(XST_BASE, addr, 3u, 1, 1);
    s_tmpl_ok = 0;
    if (!xst_draw(x, 230.0f)) {
        fprintf(stderr, "gpu texscale selftest: FAIL budget: draw or readback failed\n");
        return 0;
    }
    xst_expect(t, f, 1, want);
    return xst_report("budget", (int)x, 230, want, what);
}

static int xst_cell_budget(int f) {
    uint32_t ta[XST_W * XST_H], tb[XST_W * XST_H], tc[XST_W * XST_H];
    xst_reset();
    nk_tex_scale_set_sync(1);
    const NkTexScaleConfig saved = *nk_tex_scale_config();
    NkTexScaleConfig two = saved;
    const uint64_t copy = (uint64_t)XST_W * XST_H * (uint64_t)f * (uint64_t)f * 4u;
    two.budget_bytes = 2u * copy;
    nk_tex_scale_configure(&two);
    xst_pattern(ta, 61u);
    xst_pattern(tb, 67u);
    xst_pattern(tc, 71u);
    const uint32_t addr[3] = { XST_TEX, XST_TEX_B, XST_TEX_C };
    const uint32_t *tex[3] = { ta, tb, tc };
    for (int i = 0; i < 3; i++) {
        memcpy(SR_HOST(addr[i]), tex[i], sizeof(uint32_t) * XST_W * XST_H);
        sr_gpu_vram_dirty(addr[i], sizeof(uint32_t) * XST_W * XST_H);
    }
    const uint64_t short0 = s_cnt_xs_budget_short, reclaimed0 = s_cnt_xs_reclaimed;
    /* A and B: decoded and scaled at the first draw, the copy at the second. */
    int ok = xst_budget_draw(XST_TEX, ta, 20.0f, 0, "A's first draw is not the texture") &&
             xst_budget_draw(XST_TEX, ta, 20.0f, f, "A's copy is not drawn") &&
             xst_budget_draw(XST_TEX_B, tb, 60.0f, 0, "B's first draw is not the texture") &&
             xst_budget_draw(XST_TEX_B, tb, 60.0f, f, "B's copy is not drawn");
    if (ok && nk_tex_scale_budget_used() != 2u * copy) {
        fprintf(stderr, "gpu texscale selftest: FAIL budget: %llu bytes held after two copies, want %llu\n",
                (unsigned long long)nk_tex_scale_budget_used(), (unsigned long long)(2u * copy));
        ok = 0;
    }
    /* C: the budget is full of copies in use, so C stays unscaled and A keeps its copy. */
    ok = ok && xst_budget_draw(XST_TEX_C, tc, 100.0f, 0, "C, past the budget, is not drawn unscaled") &&
         xst_budget_draw(XST_TEX_C, tc, 100.0f, 0, "C, past the budget, is not drawn unscaled the second time") &&
         xst_budget_draw(XST_TEX, ta, 140.0f, f, "A's copy, in use, was freed for C");
    const uint64_t short1 = s_cnt_xs_budget_short - short0, reclaimed1 = s_cnt_xs_reclaimed - reclaimed0;
    if (ok && (short1 != 1u || reclaimed1 != 0u)) {
        fprintf(stderr, "gpu texscale selftest: FAIL budget: with the copies in use, %llu budget waits (want 1, once "
                        "per present) and %llu copies freed (want 0)\n",
                (unsigned long long)short1, (unsigned long long)reclaimed1);
        ok = 0;
    }
    /* A and B go idle; C's next draw frees both and offers C, whose copy the draw after shows. */
    s_cnt_present_cpu += XS_IDLE_PRESENTS;
    ok = ok && xst_budget_draw(XST_TEX_C, tc, 180.0f, 0, "C is not drawn unscaled while its copy is made") &&
         xst_budget_draw(XST_TEX_C, tc, 220.0f, f, "C's copy did not land once the idle copies were freed");
    const uint64_t reclaimed2 = s_cnt_xs_reclaimed - reclaimed0;
    if (ok && (reclaimed2 != 2u || nk_tex_scale_budget_used() != copy)) {
        fprintf(stderr, "gpu texscale selftest: FAIL budget: %llu idle copies freed (want 2), %llu bytes held (want "
                        "%llu)\n", (unsigned long long)reclaimed2, (unsigned long long)nk_tex_scale_budget_used(),
                (unsigned long long)copy);
        ok = 0;
    }
    /* A, its copy freed, is offered again on its next draw and scaled. */
    ok = ok && xst_budget_draw(XST_TEX, ta, 260.0f, 0, "A, its copy freed, is not drawn unscaled") &&
         xst_budget_draw(XST_TEX, ta, 300.0f, f, "A's copy did not come back after it was freed");
    if (ok && nk_tex_scale_budget_used() != 2u * copy) {
        fprintf(stderr, "gpu texscale selftest: FAIL budget: %llu bytes held at the end, want %llu\n",
                (unsigned long long)nk_tex_scale_budget_used(), (unsigned long long)(2u * copy));
        ok = 0;
    }
    nk_tex_scale_configure(&saved);
    if (!ok) return 0;
    printf("gpu texscale selftest: PASS %-12s in-use copies kept, idle ones freed (%llu), a freed copy rescaled\n",
           "budget", (unsigned long long)reclaimed2);
    return 1;
}

static int xst_cell_rtt(int f) {
    const size_t bytes = XST_STRIDE * FB_H * 4u;
    uint8_t *on = (uint8_t *)malloc(bytes), *off = (uint8_t *)malloc(bytes);
    int ok = on && off;
    for (int swizzle = 0; ok && swizzle < 2; swizzle++) {
        const uint64_t bound0 = s_cnt_xs_bound;
        ok = xst_rtt_pass(f, swizzle, on);
        const uint64_t bound = s_cnt_xs_bound - bound0;
        ok = ok && xst_rtt_pass(0, swizzle, off);
        s_xs_factor = f;
        if (!ok) { fprintf(stderr, "gpu texscale selftest: FAIL rtt: draw failed (swizzle %d)\n", swizzle); break; }
        if (bound) {
            fprintf(stderr, "gpu texscale selftest: FAIL rtt: %llu scaled binds for a render target (swizzle %d)\n",
                    (unsigned long long)bound, swizzle);
            ok = 0;
            break;
        }
        if (memcmp(on, off, bytes) != 0) {
            fprintf(stderr, "gpu texscale selftest: FAIL rtt: a render-to-texture draw differs with scaling on "
                            "(swizzle %d)\n", swizzle);
            ok = 0;
        }
    }
    free(on);
    free(off);
    s_xs_factor = f;
    if (ok) printf("gpu texscale selftest: PASS %-12s direct and read back: never scaled, identical on/off\n", "rtt");
    return ok;
}

/* A two-cell atlas, like a font page: glyph A in columns 0-7 and, when `with_b`, glyph B in
 * columns 8-15 with a stroke on the shared edge (else the field). */
static void xst_atlas(uint32_t *t, int with_b) {
    const uint32_t field = 0xFF202020u, ink_a = 0xFFE0E0E0u, ink_b = 0xFF30C0F0u;
    for (int y = 0; y < XST_H; y++)
        for (int x = 0; x < XST_W; x++) {
            uint32_t c = field;
            if (x < 8 && (x == y / 2 || (y == 12 && x >= 1)))
                c = ink_a;
            if (with_b && x >= 8 && (x == 8 || x - 8 == 15 - y || (y >= 3 && y <= 5)))
                c = ink_b;
            t[y * XST_W + x] = c;
        }
}

/* A sprite of the 8x16 texel cell at (0, 0), `zoom` guest pixels per texel, at (x, y). */
static int xst_cell_sprite(float x, float y, int zoom) {
    GeVtx p0, p1;
    wrap_vtx(&p0, x, y, 0xFFFFFFFFu); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, x + 8.0f * (float)zoom, y + 16.0f * (float)zoom, 0xFFFFFFFFu); p1.u = 8.0f; p1.v = 16.0f;
    s_tmpl_ok = 0;
    return hook_sprite(&p0, &p1, 0) && s_cur && target_readback(s_cur);
}

/* Device pixels that differ between the w x h guest-pixel regions at (ax, ay) and (bx, by). */
static int xst_device_diff(const uint32_t *dev, int ax, int ay, int bx, int by, int w, int h) {
    const int s = s_scale;
    int n = 0;
    for (int ky = 0; ky < h * s; ky++)
        for (int kx = 0; kx < w * s; kx++)
            if ((dev[((size_t)ay * (size_t)s + (size_t)ky) * SCL_W + (size_t)ax * (size_t)s + (size_t)kx] & XST_RGB) !=
                (dev[((size_t)by * (size_t)s + (size_t)ky) * SCL_W + (size_t)bx * (size_t)s + (size_t)kx] & XST_RGB))
                n++;
    return n;
}

/* Draws of one atlas cell, `pass` 0 with the 1:1 rule off and 1 with it on: for each atlas
 * (with glyph B, then without), one draw that decodes and scales and one that finds the copy. */
static int xst_onetoone_pass(float x0, uint64_t *bound, uint64_t *plain) {
    uint32_t tex[XST_W * XST_H];
    int ok = 1;
    for (int with_b = 1; ok && with_b >= 0; with_b--) {
        xst_atlas(tex, with_b);
        xst_upload(tex);
        ge_state_note_external_write();
        const float x = x0 + (with_b ? 0.0f : 40.0f);
        ok = xst_cell_sprite(x, 20.0f, 1);
        const uint64_t b0 = s_cnt_xs_bound, p0 = s_cnt_xs_plain;
        ok = ok && xst_cell_sprite(x + 20.0f, 20.0f, 1);
        *bound += s_cnt_xs_bound - b0;
        *plain += s_cnt_xs_plain - p0;
    }
    return ok;
}

/* The 1:1 rule. With it off, a 1:1 draw of one cell of an atlas from the scaled copy shows the
 * neighbouring cell at its edge: the cell draws differently beside glyph B than beside the field
 * (the marks under LittleBigPlanet's text). With it on, the same draw is the unscaled texture's,
 * beside either neighbour. A magnified draw keeps the copy and is identical with the rule on or
 * off. Nearest sampling, so the unscaled texture itself never reads past the cell. */
static int xst_cell_onetoone(int f) {
    (void)f;
    const int smart = s_xs_smart2d;
    xst_reset();
    s_xs_smart2d = 0;                 /* the copy alone; smart 2D has its own cell */
    nk_tex_scale_set_sync(1);
    xst_state(XST_BASE, XST_TEX, 3u, 0, 1);
    uint64_t off_bound = 0, off_plain = 0, on_bound = 0, on_plain = 0;
    s_xs_onetoone_plain = 0;
    int ok = xst_onetoone_pass(20.0f, &off_bound, &off_plain);      /* rule off: x 20..100 */
    s_xs_onetoone_plain = 1;
    ok = ok && xst_onetoone_pass(120.0f, &on_bound, &on_plain);      /* rule on: x 120..200 */
    /* Magnified 2x from the copy of the field-only atlas, the rule on and then off. */
    const uint64_t mag_p0 = s_cnt_xs_plain, mag_b0 = s_cnt_xs_bound;
    ok = ok && xst_cell_sprite(20.0f, 60.0f, 2);
    s_xs_onetoone_plain = 0;
    ok = ok && xst_cell_sprite(60.0f, 60.0f, 2);
    s_xs_onetoone_plain = 1;
    const uint64_t mag_plain = s_cnt_xs_plain - mag_p0, mag_bound = s_cnt_xs_bound - mag_b0;
    s_xs_smart2d = smart;
    if (!ok) { fprintf(stderr, "gpu texscale selftest: FAIL onetoone: draw or readback failed\n"); return 0; }
    if (off_bound != 2u || off_plain != 0u || on_bound != 2u || on_plain != 2u || mag_bound != 2u || mag_plain != 0u) {
        fprintf(stderr, "gpu texscale selftest: FAIL onetoone: copies found %llu/%llu/%llu times (want 2/2/2), "
                        "drawn unscaled %llu/%llu/%llu (want 0/2/0) with the rule off, on, magnified\n",
                (unsigned long long)off_bound, (unsigned long long)on_bound, (unsigned long long)mag_bound,
                (unsigned long long)off_plain, (unsigned long long)on_plain, (unsigned long long)mag_plain);
        return 0;
    }
    uint32_t *dev = (uint32_t *)malloc((size_t)SCL_W * SCL_H * 4u);
    if (!dev || !s_cur || !xst_read_device(s_cur, dev)) {
        free(dev);
        fprintf(stderr, "gpu texscale selftest: FAIL onetoone: device readback failed\n");
        return 0;
    }
    /* Positions: rule off at 20 (unscaled, with B), 40 (copy, with B), 60 (unscaled), 80 (copy);
     * rule on at 120, 140, 160, 180 likewise; magnified at (20, 60) on and (60, 60) off. The
     * neighbour is measured over guest columns 0-6: above 1x the last guest column's right half
     * samples past u = 8 (hook_sprite's pixel-centre shift), so even the unscaled texture reads
     * column 8 there. Over columns 0-6 only a copy can carry the neighbour in. */
    const int bleed_off = xst_device_diff(dev, 40, 20, 80, 20, 7, 16);
    const int copy_differs = xst_device_diff(dev, 40, 20, 20, 20, 8, 16);
    const int bleed_on = xst_device_diff(dev, 140, 20, 180, 20, 7, 16);
    const int on_vs_plain = xst_device_diff(dev, 140, 20, 20, 20, 8, 16) + xst_device_diff(dev, 180, 20, 60, 20, 8, 16);
    const int mag_diff = xst_device_diff(dev, 20, 60, 60, 60, 16, 32);
    free(dev);
    if (bleed_on != 0 || on_vs_plain != 0 || mag_diff != 0) {
        fprintf(stderr, "gpu texscale selftest: FAIL onetoone: with the rule on, %d device pixels of the cell depend on "
                        "its neighbour and %d differ from the unscaled draw; the magnified draw differs in %d with the "
                        "rule on and off (want 0, 0, 0)\n", bleed_on, on_vs_plain, mag_diff);
        return 0;
    }
    if (s_scale > 1 && bleed_off == 0) {
        fprintf(stderr, "gpu texscale selftest: FAIL onetoone: the copy shows no neighbour at the cell's edge at render "
                        "scale %d (vacuous: %d device pixels differ from the texture)\n", s_scale, copy_differs);
        return 0;
    }
    printf("gpu texscale selftest: PASS %-12s 1:1 draws unscaled (%d device pixels of neighbour bleed without the "
           "rule); magnified draws keep the copy\n", "onetoone", bleed_off);
    return 1;
}

static int xst_cell_smart2d(void) {
    uint32_t tex[XST_W * XST_H];
    const int factor = s_xs_factor;
    xst_reset();
    s_xs_factor = 0;   /* the filter alone, on the guest's own texture */
    xst_pattern(tex, 89u);
    xst_upload(tex);
    xst_state(XST_BASE, XST_TEX, 3u, 1, 1);
    const uint64_t n0 = s_cnt_smart2d;
    int ok = xst_draw(20.0f, 20.0f);                 /* smart 2D on */
    const uint64_t on_count = s_cnt_smart2d - n0;
    s_xs_smart2d = 0;
    s_tmpl_ok = 0;
    ok = ok && xst_draw(60.0f, 20.0f);               /* off */
    s_xs_smart2d = 1;
    /* A perspective triangle keeps its filter. */
    s_tmpl_ok = 0;
    GeVtx a, b, c;
    wrap_vtx(&a, 100.0f, 20.0f, 0xFFFFFFFFu); a.rw = 1.0f;   a.u = 0.0f;          a.v = 0.0f;
    wrap_vtx(&b, 140.0f, 24.0f, 0xFFFFFFFFu); b.rw = 0.5f;   b.u = 16.0f * 0.5f;  b.v = 0.0f;
    wrap_vtx(&c, 104.0f, 60.0f, 0xFFFFFFFFu); c.rw = 0.25f;  c.u = 0.0f;          c.v = 16.0f * 0.25f;
    s_ge->vtype = 0;
    ge_state_note_external_write();
    const uint64_t n1 = s_cnt_smart2d;
    ok = ok && hook_tri(&a, &b, &c, 1);
    const uint64_t tri_count = s_cnt_smart2d - n1;
    s_xs_factor = factor;
    if (!ok) { fprintf(stderr, "gpu texscale selftest: FAIL smart2d: draw failed\n"); return 0; }
    if (on_count != 1u || tri_count != 0u) {
        fprintf(stderr, "gpu texscale selftest: FAIL smart2d: the 1:1 sprite was classified %llu times (want 1), "
                        "the perspective triangle %llu (want 0)\n",
                (unsigned long long)on_count, (unsigned long long)tri_count);
        return 0;
    }
    /* The sprite smart 2D samples nearest reads whole texels at every render scale. At the PSP's
     * resolution the linear one does too, so the guest frame is the texture with the filter on or
     * off. Above 1x each device pixel samples at its own position (hook_sprite, #2104), so the
     * linear sprite's guest pixels blend their neighbours; the device image below checks that. */
    uint32_t want[XST_W * XST_H];
    xst_expect(tex, 0, 1, want);
    if (!xst_report("smart2d", 20, 20, want, "the filtered sprite's guest pixels are not the texture") ||
        (s_scale == 1 && !xst_report("smart2d", 60, 20, want, "the linear sprite's guest pixels are not the texture")))
        return 0;
    if (s_scale > 1) {
        uint32_t *dev = (uint32_t *)malloc((size_t)SCL_W * SCL_H * 4u);
        if (!dev || !s_cur || !xst_read_device(s_cur, dev)) {
            free(dev);
            fprintf(stderr, "gpu texscale selftest: FAIL smart2d: device readback failed\n");
            return 0;
        }
        const int nearest_blends = xst_device_blends(dev, 20, 20, tex, 1);
        const int linear_blends = xst_device_blends(dev, 60, 20, tex, 1);
        free(dev);
        if (nearest_blends != 0 || linear_blends == 0) {
            fprintf(stderr, "gpu texscale selftest: FAIL smart2d: %d blended device pixels with the filter (want 0), "
                            "%d without it (want some)\n", nearest_blends, linear_blends);
            return 0;
        }
        printf("gpu texscale selftest: PASS %-12s nearest at %dx (%d blended device pixels without it)\n",
               "smart2d", s_scale, linear_blends);
    } else {
        printf("gpu texscale selftest: PASS %-12s classified at 1x; frames identical\n", "smart2d");
    }
    return 1;
}

/* ---- texture replacement (#1880) ----------------------------------------------------------- */

static int xst_setenv(const char *name, const char *value) {
#ifdef _WIN32
    return _putenv_s(name, value ? value : "") == 0;
#else
    return value ? setenv(name, value, 1) == 0 : unsetenv(name) == 0;
#endif
}

/* The file name a pack gives the 16x16 8888 texture `t` at XST_TEX: cache key, then quick hash. */
static void xst_repl_name(const uint32_t *t, char *out, size_t cap) {
    xst_upload(t);
    const uint64_t key = nk_texrepl_cache_key(XST_TEX, 3u, 4u | (4u << 8), 0u);
    snprintf(out, cap, "%016llx%08x.png", (unsigned long long)key,
             nk_texrepl_quick_hash(SR_HOST(XST_TEX), sizeof(uint32_t) * XST_W * XST_H, XST_TEX));
}

/* What a 1:1 sprite of a texture replaced by `img` (factor rf) reads back: the copy's own lattice. */
static void xst_repl_expect(const uint32_t *img, int rf, uint32_t *want) {
    for (int y = 0; y < XST_H; y++)
        for (int x = 0; x < XST_W; x++)
            want[y * XST_W + x] = img[xst_readback_texel(0.0f, y, XST_H, rf) * XST_W * (size_t)rf +
                                      xst_readback_texel(0.0f, x, XST_W, rf)];
}

/* A pack's image replaces a texture whose hash it lists, from the draw that finds it; a rewritten
 * texture the pack does not list is never drawn from the old image (it falls back to xBRZ, which
 * scales what the pack leaves); the replaced content coming back is replaced again; an image that
 * is no whole factor of the texture is counted and the texture drawn as the game made it; the 1:1
 * rule that binds guest-resolution textures never applies to a replacement; and an authentic start
 * reads no pack, so what it draws is what a start without SR_TEX_REPLACE draws. */
static int xst_cell_replace(int f) {
    const int rf = 3, rw = XST_W * rf, rh = XST_H * rf, ww = 24;
    uint32_t a[XST_W * XST_H], b[XST_W * XST_H], d[XST_W * XST_H];
    uint32_t want[XST_W * XST_H], plain[XST_W * XST_H];
    uint32_t *rimg = (uint32_t *)malloc((size_t)rw * rh * 4u), *wide = (uint32_t *)malloc((size_t)ww * XST_H * 4u);
    if (!rimg || !wide) { free(rimg); free(wide); return 0; }
    for (int i = 0; i < rw * rh; i++) rimg[i] = 0xFF000000u | (xst_hash(0x9000u + (uint32_t)i) & 0xFFFFFFu);
    for (int i = 0; i < ww * XST_H; i++) wide[i] = 0xFF000000u | (xst_hash(0xA000u + (uint32_t)i) & 0xFFFFFFu);
    xst_pattern(a, 61u);
    xst_pattern(b, 67u);
    xst_pattern(d, 71u);
    char dir[400], path[480], name_a[64], name_d[64], err[200];
    snprintf(dir, sizeof(dir), "%s/texrepl_gpu_pack", SR_SELFTEST_BUILD_ROOT);
    nk_platform_mkdir_p(dir);
    xst_repl_name(a, name_a, sizeof(name_a));
    xst_repl_name(d, name_d, sizeof(name_d));
    snprintf(path, sizeof(path), "%s/%s", dir, name_a);
    int ok = synpng_write(path, rw, rh, rimg);
    snprintf(path, sizeof(path), "%s/%s", dir, name_d);
    ok = ok && synpng_write(path, ww, XST_H, wide);
    if (!ok) { fprintf(stderr, "gpu texscale selftest: FAIL replace: cannot write the synthetic pack\n"); free(rimg); free(wide); return 0; }

    /* The Ace gate: an authentic start (enhanced = 0) names the request and reads no pack. */
    NkTexRepl *saved = s_repl;
    s_repl = NULL;
    xst_setenv("SR_TEX_REPLACE", dir);
    tex_repl_init(0);
    const int authentic_read = s_repl != NULL;
    /* Contract 11.2 item 1: the listed texture drawn after that authentic start leaves the guest
     * framebuffer byte for byte as a start without SR_TEX_REPLACE does. Section 1 below makes the
     * same draw with the pack read and requires other bytes, so the comparison can fail. The draws
     * run in this harness's scaling state; only the start is authentic. */
    const size_t fb_bytes = (size_t)XST_STRIDE * FB_H * 4u;
    uint8_t *fb_named = (uint8_t *)malloc(fb_bytes);
    int authentic_same = 0;
    if (fb_named) {
        const int onetoone_was = s_xs_onetoone_plain;
        s_xs_onetoone_plain = 1;
        nk_tex_scale_set_sync(1);
        xst_reset();
        xst_upload(a);
        xst_state(XST_BASE, XST_TEX, 3u, 1, 1);
        s_tmpl_ok = 0;
        int drew = xst_draw(20.0f, 140.0f);
        memcpy(fb_named, SR_HOST(XST_BASE), fb_bytes);
        tex_repl_shutdown();
        xst_setenv("SR_TEX_REPLACE", NULL);
        tex_repl_init(0);
        xst_reset();
        xst_upload(a);
        xst_state(XST_BASE, XST_TEX, 3u, 1, 1);
        s_tmpl_ok = 0;
        drew = drew && xst_draw(20.0f, 140.0f);
        authentic_same = drew && memcmp(fb_named, SR_HOST(XST_BASE), fb_bytes) == 0;
        s_xs_onetoone_plain = onetoone_was;
        xst_setenv("SR_TEX_REPLACE", dir);
    }
    tex_repl_shutdown();
    /* The off switch (contract 11.2 item 2): this harness's Enhanced line has no texpack=on, so an
     * Enhanced start with SR_TEX_REPLACE set reads no pack either. */
    const int off_read = !ace_requested()->texpack && (tex_repl_init(1), s_repl != NULL);
    tex_repl_shutdown();
    xst_setenv("SR_TEX_REPLACE", NULL);
    s_repl = saved;
    if (authentic_read || !authentic_same || off_read || ace_requested()->texpack) {
        fprintf(stderr, "gpu texscale selftest: FAIL replace: %s\n",
                authentic_read ? "an authentic start read SR_TEX_REPLACE"
                : !authentic_same ? "an authentic start with SR_TEX_REPLACE set drew other framebuffer bytes "
                                    "than one without it"
                : off_read ? "an Enhanced start without texpack=on read SR_TEX_REPLACE"
                : "the harness line carries texpack=on, so the off switch is not tested");
        free(fb_named); free(rimg); free(wide);
        return 0;
    }

    xst_reset();
    nk_tex_scale_set_sync(1);
    s_repl = nk_texrepl_open(dir, 0, err, sizeof(err));
    if (!s_repl) { fprintf(stderr, "gpu texscale selftest: FAIL replace: %s\n", err); free(fb_named); free(rimg); free(wide); return 0; }
    NkTexReplStats st0;
    nk_texrepl_stats(s_repl, &st0);
    const uint64_t bound0 = s_cnt_repl_bound;
    const int onetoone_saved = s_xs_onetoone_plain;

    /* 1. Listed content, with the 1:1 rule on: the replacement, from the very first draw. */
    s_xs_onetoone_plain = 1;
    xst_upload(a);
    xst_state(XST_BASE, XST_TEX, 3u, 1, 1);
    xst_repl_expect(rimg, rf, want);
    xst_expect(a, 0, 1, plain);
    ok = xst_draw(20.0f, 140.0f);
    const int pack_differs = memcmp(fb_named, SR_HOST(XST_BASE), fb_bytes) != 0;
    s_tmpl_ok = 0;
    ok = ok && xst_draw(60.0f, 140.0f);
    s_xs_onetoone_plain = onetoone_saved;
    if (!ok || !xst_report("replace", 20, 140, want, "the first draw of a listed texture is not its replacement") ||
        !xst_report("replace", 60, 140, want, "a later draw of a listed texture is not its replacement")) goto fail;
    if (memcmp(want, plain, sizeof(want)) == 0 || s_cnt_repl_bound <= bound0) {
        fprintf(stderr, "gpu texscale selftest: FAIL replace: the replacement is the texture (vacuous) or no replaced "
                        "bind was counted\n");
        goto fail;
    }
    if (!pack_differs) {
        fprintf(stderr, "gpu texscale selftest: FAIL replace: the listed texture drawn with the pack read left the "
                        "framebuffer as without it (the authentic comparison is vacuous)\n");
        goto fail;
    }

    /* 2. The guest rewrites the texture to content the pack does not list: the old image is never
     * drawn; the first draw shows the new texture as it is, the next xBRZ's copy of it. */
    xst_upload(b);
    s_tmpl_ok = 0;
    ok = xst_draw(100.0f, 140.0f);
    xst_expect(b, 0, 1, plain);
    if (!ok) goto fail;
    if (xst_matches(100, 140, want)) {
        fprintf(stderr, "gpu texscale selftest: FAIL replace: the replacement of the previous content was drawn "
                        "after the guest rewrote the texture\n");
        goto fail;
    }
    if (!xst_report("replace", 100, 140, plain, "an unlisted rewritten texture is not drawn as the game made it")) goto fail;
    s_tmpl_ok = 0;
    ok = xst_draw(140.0f, 140.0f);
    xst_expect(b, f, 1, want);
    if (!ok || !xst_report("replace", 140, 140, want, "an unlisted rewritten texture did not take xBRZ's copy")) goto fail;

    /* 3. The listed content returns: replaced again at once. */
    xst_upload(a);
    s_tmpl_ok = 0;
    ok = xst_draw(180.0f, 140.0f);
    xst_repl_expect(rimg, rf, want);
    if (!ok || !xst_report("replace", 180, 140, want, "listed content that came back is not replaced again")) goto fail;

    /* 4. An image that is not a whole factor (48 wide for 16): counted, the texture drawn as made. */
    xst_upload(d);
    s_tmpl_ok = 0;
    ok = xst_draw(20.0f, 180.0f);
    xst_expect(d, 0, 1, plain);
    NkTexReplStats st1;
    nk_texrepl_stats(s_repl, &st1);
    if (!ok || !xst_report("replace", 20, 180, plain, "a texture whose replacement is no whole factor is not drawn as made")) goto fail;
    if (st1.bad_factor <= st0.bad_factor || st1.hits < 2u) {
        fprintf(stderr, "gpu texscale selftest: FAIL replace: bad_factor %llu -> %llu (want a rise), hits %llu (want 2 or more)\n",
                (unsigned long long)st0.bad_factor, (unsigned long long)st1.bad_factor, (unsigned long long)st1.hits);
        goto fail;
    }

    nk_texrepl_close(s_repl);
    s_repl = saved;
    free(fb_named); free(rimg); free(wide);
    printf("gpu texscale selftest: PASS %-12s listed texture replaced at once, an unlisted rewrite never from the old image, "
           "a bad factor counted\n", "replace");
    return 1;
fail:
    nk_texrepl_close(s_repl);
    s_repl = saved;
    free(fb_named); free(rimg); free(wide);
    return 0;
}

/* A replacement image for a 16x16 texture at factor rf, the same bytes wherever a cell needs it. */
static uint32_t *xst_repl_image(int rf, uint32_t salt) {
    const int n = XST_W * rf * XST_H * rf;
    uint32_t *img = (uint32_t *)malloc((size_t)n * 4u);
    if (img)
        for (int i = 0; i < n; i++) img[i] = 0xFF000000u | (xst_hash(salt + (uint32_t)i) & 0xFFFFFFu);
    return img;
}

/* A palette texture (CLUT8) listed by a pack file named the way PPSSPP names it: the key and the
 * palette identity are written out here from PPSSPP's formula (TextureCacheCommon.cpp: XXH32 of the
 * loaded palette bytes, seed 0xC0108888, xor the CLUTFORMAT word with its 0xC5 opcode) and the
 * data hash is the quick hash, none of it through nk_texrepl_clut_identity or nk_texrepl_cache_key.
 * The texture is drawn from the pack's image; the build with NK_TEXREPL_MUTANT_NO_CLUTFORMAT_XOR
 * leaves the xor out of the identity and must fail here, for that reason. */
static int xst_cell_replace_palette(int f) {
    (void)f;
    const int rf = 2, rw = XST_W * rf, rh = XST_H * rf;
    uint32_t pat[XST_W * XST_H], want[XST_W * XST_H];
    uint8_t *idx = (uint8_t *)SR_HOST(XST_TEX);
    uint32_t *rimg = xst_repl_image(rf, 0xB000u);
    if (!rimg) return 0;
    xst_reset();
    nk_tex_scale_set_sync(1);
    xst_pattern(pat, 9u);
    for (int i = 0; i < XST_W * XST_H; i++) idx[i] = (uint8_t)(pat[i] & 0x0Fu);
    sr_gpu_vram_dirty(XST_TEX, XST_W * XST_H);
    xst_state(XST_BASE, XST_TEX, 5u, 1, 1);          /* CLUT8; the state reset clears the palette */
    for (uint32_t j = 0; j < 256u; j++) {
        uint32_t entry = 0xFF000000u | (xst_hash(0xC00u + j * 5u) & 0xFFFFFFu);
        memcpy(&s_ge->clutram[j * 4u], &entry, 4);
    }
    s_ge->clut_fmt = 3u | (0xFFu << 8);              /* 8888, shift 0, mask 0xFF, start 0 */
    ge_clut_note_load(256u * 4u);                    /* what GE_LOADCLUT records for a 1 KiB load */
    ge_state_note_external_write();

    uint32_t last = 0, max = 0;
    ge_clut_load_extent(&last, &max);
    const uint32_t loaded = last < max ? last : max;
    const uint32_t cluthash = nk_texrepl_xxh32(s_ge->clutram, loaded, 0xC0108888u) ^ (0xC5000000u | s_ge->clut_fmt);
    const uint64_t key = (((uint64_t)(XST_TEX & 0x3FFFFFFFu) << 32) | (4u | (4u << 8))) ^ cluthash;
    char dir[400], path[480], name[64], err[200];
    snprintf(dir, sizeof(dir), "%s/texrepl_gpu_pal_pack", SR_SELFTEST_BUILD_ROOT);
    nk_platform_mkdir_p(dir);
    snprintf(name, sizeof(name), "%016llx%08x.png", (unsigned long long)key,
             nk_texrepl_quick_hash(idx, XST_W * XST_H, XST_TEX));
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (!synpng_write(path, rw, rh, rimg)) {
        fprintf(stderr, "gpu texscale selftest: FAIL replace-palette: cannot write the synthetic pack\n");
        free(rimg);
        return 0;
    }
    NkTexRepl *saved = s_repl;
    s_repl = nk_texrepl_open(dir, 0, err, sizeof(err));
    if (!s_repl) {
        fprintf(stderr, "gpu texscale selftest: FAIL replace-palette: %s\n", err);
        s_repl = saved;
        free(rimg);
        return 0;
    }
    xst_repl_expect(rimg, rf, want);
    int ok = xst_draw(20.0f, 140.0f);
    s_tmpl_ok = 0;
    ok = ok && xst_draw(60.0f, 140.0f);
    NkTexReplStats st;
    nk_texrepl_stats(s_repl, &st);
    nk_texrepl_close(s_repl);
    s_repl = saved;
    free(rimg);
    if (!ok || !xst_report("replace-palette", 20, 140, want, "a CLUT8 texture a PPSSPP-named file lists is not drawn from the pack's image")) return 0;
    if (st.hits < 1u) {
        fprintf(stderr, "gpu texscale selftest: FAIL replace-palette: no pack hit was counted\n");
        return 0;
    }
    printf("gpu texscale selftest: PASS %-12s a CLUT8 texture a PPSSPP-named file lists is drawn from the pack\n",
           "replace-pal");
    return 1;
}

/* ---- the pack alone (SR_TEX_REPLACE with no texture scaling), the documented default use ------
 * `--repl-only` in the harness: gegpu_texrepl_only_prepare writes the pack and names it before the
 * renderer starts, so gegpu_init's own tex_repl_init reads it with s_xs_factor 0 (the pool doubling,
 * the scaled-copy budget fallback and the shutdown path are what make that work); the cell then
 * draws a listed texture, which an address-only file lists, and an unlisted one. */
#define XST_ONLY_TEX_B (XST_TEX + 0x1000u)

/* The dump cells write into folders whose names hold an accented letter and a CJK ideograph, as a
 * user data folder under a user name like that does: every file call is a wide-path one. */
#define XST_DUMP_NONASCII " \xC3\xA9\xE6\x97\xA5"   /* " ", U+00E9, U+65E5 in UTF-8 */
#define XST_DUMP_VIDEO (XST_TEX + 0x20000u)       /* a movie picture's address (dump cell only) */
#define XST_DUMP_OTHER (XST_TEX + 0x2000u)        /* a second texture address */
void ge_set_frame(uint32_t frame);   /* ge.c: the VBLANK a movie span's age is counted in */

static int xst_dump_has(const char *dir, const char *name) {
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return nk_platform_file_exists(path);
}

static int xst_dump_fail(const char *why) {
    fprintf(stderr, "gpu texscale selftest: FAIL dump: %s\n", why);
    gegpu_texdump_stop();
    return 0;
}

/* A clean folder `<build root>/<leaf>` with the non-ASCII suffix, into `dir`. */
static void xst_dump_folder(char *dir, size_t cap, const char *leaf) {
    snprintf(dir, cap, "%s/%s" XST_DUMP_NONASCII, SR_SELFTEST_BUILD_ROOT, leaf);
    (void)nk_platform_remove_tree(dir, NULL);   /* an earlier run's files */
}

/* The 16x16 8888 texture `t` at `addr`, with the state that draws it. */
static void xst_dump_upload_at(uint32_t addr, const uint32_t *t) {
    memcpy(SR_HOST(addr), t, sizeof(uint32_t) * XST_W * XST_H);
    sr_gpu_vram_dirty(addr, sizeof(uint32_t) * XST_W * XST_H);
    xst_state(XST_BASE, addr, 3u, 1, 1);
}

static int xst_dump_draw(void) {
    s_tmpl_ok = 0;
    return xst_draw(20.0f, 140.0f);
}

/* The files in `dir`. */
static bool xst_dump_count_file(const char *name, void *ctx) {
    (void)name;
    ++*(int *)ctx;
    return true;
}
static int xst_dump_files(const char *dir) {
    int n = 0;
    return nk_platform_list_files(dir, xst_dump_count_file, &n) ? n : -1;
}

/* The width and height a PNG's IHDR gives, or 0. */
static int xst_dump_png_size(const char *dir, const char *name, uint32_t *w, uint32_t *h) {
    char path[600];
    unsigned char b[24];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *fp = nk_fopen_utf8(path, "rb");
    const size_t got = fp ? fread(b, 1, sizeof(b), fp) : 0u;
    if (fp) fclose(fp);
    if (got != sizeof(b) || memcmp(b + 12, "IHDR", 4u) != 0) return 0;
    *w = ((uint32_t)b[16] << 24) | ((uint32_t)b[17] << 16) | ((uint32_t)b[18] << 8) | b[19];
    *h = ((uint32_t)b[20] << 24) | ((uint32_t)b[21] << 16) | ((uint32_t)b[22] << 8) | b[23];
    return 1;
}

/* Back to a dump that goes on, for the next check: the run's budget and file limit as at start. */
static void xst_dump_resume(void) {
    s_dump.stop = TEX_DUMP_GOING;
    s_dump.budget = (uint64_t)TEX_DUMP_DEFAULT_BUDGET_MB << 20;
    s_dump.max_files = TEX_DUMP_DEFAULT_MAX_FILES;
}

static int xst_dump_start(const char *dir) {
    char err[200];
    if (gegpu_texdump_start(dir, err, sizeof(err))) return 1;
    fprintf(stderr, "gpu texscale selftest: FAIL dump: the dump did not start (%s)\n", err);
    return 0;
}

static int xst_cell_dump(int f) {
    (void)f;
    uint32_t a[XST_W * XST_H], b[XST_W * XST_H], c[XST_W * XST_H], d[XST_W * XST_H];
    xst_pattern(a, 101u);
    xst_pattern(b, 103u);
    xst_pattern(c, 107u);
    xst_pattern(d, 109u);
    char dir[400], path[600], name_a[64], name_b[64], name_c[64], name_d[64], err[200];
    xst_dump_folder(dir, sizeof(dir), "texdump_gpu");
    xst_repl_name(a, name_a, sizeof(name_a));
    xst_repl_name(b, name_b, sizeof(name_b));
    xst_repl_name(c, name_c, sizeof(name_c));
    xst_repl_name(d, name_d, sizeof(name_d));
    xst_reset();
    nk_tex_scale_set_sync(1);
    memset(&s_dump, 0, sizeof(s_dump));   /* a run of its own: the budget and file count from zero */

    /* 1. An authentic run refuses the dump. */
    const int ace_saved = g_ace_on;
    g_ace_on = 0;
    const int authentic_started = gegpu_texdump_start(dir, err, sizeof(err));
    const int authentic_row = gegpu_texdump_row();
    g_ace_on = ace_saved;
    if (authentic_started || authentic_row != 0) return xst_dump_fail("an authentic run started the dump or offered its row");

    /* 2. Off: a drawn texture is not written. */
    xst_upload(b);
    xst_state(XST_BASE, XST_TEX, 3u, 1, 1);
    if (!xst_dump_draw()) return xst_dump_fail("a draw failed");
    if (gegpu_texdump_row() != 1 || xst_dump_has(dir, name_b)) return xst_dump_fail("a texture was written while the dump was off");

    /* 3. On: the texture already in the cache (drawn again, a cache hit) and new content at the
     * same address (a decode) are both written, under the names a pack gives them, in a folder
     * whose name is not ASCII. */
    xst_setenv("SR_DISC_ID", "TEST00042");
    const int started = gegpu_texdump_start(dir, err, sizeof(err));
    xst_setenv("SR_DISC_ID", NULL);
    if (!started) return xst_dump_fail(err);
    if (gegpu_texdump_row() != 2 || !xst_dump_has(dir, "textures.ini")) return xst_dump_fail("the dump did not start with a textures.ini");
    {
        /* Its [games] names the run's game, so the player takes the folder as this game's pack. */
        char ini[600], text[600];
        snprintf(ini, sizeof(ini), "%s/textures.ini", dir);
        FILE *fi = nk_fopen_utf8(ini, "rb");
        const size_t got = fi ? fread(text, 1, sizeof(text) - 1u, fi) : 0u;
        if (fi) fclose(fi);
        text[got] = '\0';
        if (!strstr(text, "[games]\nTEST00042 = textures.ini\n"))
            return xst_dump_fail("the dump's textures.ini does not name the run's game in [games]");
    }
    if (!xst_dump_draw()) return xst_dump_fail("a draw failed");
    if (!xst_dump_has(dir, name_b)) return xst_dump_fail("a texture already in the cache was not written when drawn again");
    xst_upload(a);
    if (!xst_dump_draw()) return xst_dump_fail("a draw failed");
    if (!xst_dump_has(dir, name_a)) return xst_dump_fail("a newly decoded texture was not written");
    {
        unsigned long long saved = 0, changing = 9;
        const char *stopped = "x";
        gegpu_texdump_status(&saved, &changing, &stopped);
        if (saved != 2u || changing != 0u || stopped != NULL) return xst_dump_fail("the row's status does not count the two files");
    }

    /* 4. The folder reads back as a pack holding each texture's own pixels, and its own [games]
     * line is not reported as an override that is not applied. */
    NkTexRepl *pack = nk_texrepl_open(dir, 0, err, sizeof(err));
    if (!pack) return xst_dump_fail(err);
    int same = 1, games_note = 0;
    for (int k = 0; k < 2 && same; k++) {
        const uint32_t *t = k ? b : a;
        NkTexReplId id;
        memset(&id, 0, sizeof(id));
        id.addr = XST_TEX;
        id.format = 3u;
        id.dim = 4u | (4u << 8);
        id.hash = nk_texrepl_quick_hash(t, sizeof(uint32_t) * XST_W * XST_H, XST_TEX);
        NkTexReplHit hit;
        if (!nk_texrepl_lookup(pack, &id, &hit)) { same = 0; break; }
        same = hit.w == XST_W && hit.h == XST_H && memcmp(hit.rgba, t, sizeof(uint32_t) * XST_W * XST_H) == 0;
        nk_texrepl_unpin(pack, &hit);
    }
    for (size_t i = 0; i < nk_texrepl_diag_count(pack); i++)
        if (strstr(nk_texrepl_diag(pack, i), "[games]")) games_note = 1;
    nk_texrepl_close(pack);
    if (!same) return xst_dump_fail("the dump folder, read as a pack, does not give the textures' own pixels");
    if (games_note) return xst_dump_fail("the reader reports the dump's own [games] line as not applied");

    /* 5. Started again: content already in the folder is counted, not written again. */
    gegpu_texdump_stop();
    const unsigned long long written0 = s_dump.written, present0 = s_dump.present;
    if (!xst_dump_start(dir)) return 0;
    if (!xst_dump_draw()) return xst_dump_fail("a draw failed");
    if (s_dump.written != written0 || s_dump.present <= present0)
        return xst_dump_fail("a texture the folder already holds was written again");

    /* 6. A file the player put there (an enlarged image saved in place) is never replaced. */
    {
        static const char mine[] = "the player's own image";
        char buf[64];
        snprintf(path, sizeof(path), "%s/%s", dir, name_d);
        FILE *fo = nk_fopen_utf8(path, "wb");
        const int put = fo && fwrite(mine, 1, sizeof(mine) - 1u, fo) == sizeof(mine) - 1u;
        if (fo) fclose(fo);
        if (!put) return xst_dump_fail("the test could not write the player's file");
        const unsigned long long w0 = s_dump.written, p0 = s_dump.present;
        xst_upload(d);
        if (!xst_dump_draw()) return xst_dump_fail("a draw failed");
        FILE *fi = nk_fopen_utf8(path, "rb");
        const size_t got = fi ? fread(buf, 1, sizeof(buf) - 1u, fi) : 0u;
        if (fi) fclose(fi);
        buf[got] = '\0';
        if (strcmp(buf, mine) != 0 || s_dump.written != w0 || s_dump.present != p0 + 1u)
            return xst_dump_fail("a file already in the folder was replaced");
    }

    /* 7. A movie picture is skipped: the movie decoders note where they write. */
    {
        const unsigned long long w0 = s_dump.written, k0 = s_dump.skipped;
        sr_video_frame_note(XST_DUMP_VIDEO, sizeof(uint32_t) * XST_W * XST_H);
        xst_dump_upload_at(XST_DUMP_VIDEO, c);
        if (!xst_dump_draw()) return xst_dump_fail("a draw failed");
        if (s_dump.written != w0 || s_dump.skipped != k0 + 1u) return xst_dump_fail("a movie picture was dumped");
        s_cnt_present_gpu++;
        if (!xst_dump_draw()) return xst_dump_fail("a draw failed");
        if (s_dump.written != w0 || s_dump.skipped != k0 + 1u)
            return xst_dump_fail("a movie picture drawn again was dumped, or counted twice");
        /* Once the movie no longer writes there (its span is past ge.c's 120 VBLANKs), the same
         * entry's texture is a texture the game loaded at that address: it is written. This cell
         * runs at VBLANK 0 (nothing else here sets the frame), so 1000 is well past the span. */
        ge_set_frame(1000u);
        s_cnt_present_gpu++;
        if (!xst_dump_draw()) return xst_dump_fail("a draw failed");
        if (s_dump.written != w0 + 1u || s_dump.skipped != k0 + 1u)
            return xst_dump_fail("a texture at a movie's old address was not dumped once the movie ended");
        xst_state(XST_BASE, XST_TEX, 3u, 1, 1);
    }

    /* 8. The budget: a file that would pass it is not written, and the dump stops and says why. */
    s_dump.budget = s_dump.written_bytes;
    xst_upload(c);
    if (!xst_dump_draw()) return xst_dump_fail("a draw failed");
    if (xst_dump_has(dir, name_c) || s_dump.stop != TEX_DUMP_STOP_BUDGET) return xst_dump_fail("a file past the budget was written");
    {
        const char *stopped = NULL;
        gegpu_texdump_status(NULL, NULL, &stopped);
        if (!stopped || !strstr(stopped, "budget")) return xst_dump_fail("the row does not say the budget stopped the dump");
        /* The budget is the run's: turned on again, the dump stays stopped and says so. */
        gegpu_texdump_stop();
        if (!xst_dump_start(dir)) return 0;
        stopped = NULL;
        gegpu_texdump_status(NULL, NULL, &stopped);
        if (!stopped || s_dump.stop != TEX_DUMP_STOP_BUDGET) return xst_dump_fail("a spent budget was forgotten when the dump was turned on again");
    }
    gegpu_texdump_stop();
    if (gegpu_texdump_row() != 1) return xst_dump_fail("the row does not read off after the dump stops");
    xst_dump_resume();

    /* 9. The file limit: one more file is written, then the dump stops and says why. */
    if (!xst_dump_start(dir)) return 0;
    s_dump.max_files = s_dump.written + 1u;
    {
        uint32_t e1[XST_W * XST_H], e2[XST_W * XST_H];
        xst_pattern(e1, 211u);
        xst_pattern(e2, 223u);
        const unsigned long long w0 = s_dump.written;
        xst_upload(e1);
        if (!xst_dump_draw()) return xst_dump_fail("a draw failed");
        xst_upload(e2);
        if (!xst_dump_draw()) return xst_dump_fail("a draw failed");
        const char *stopped = NULL;
        gegpu_texdump_status(NULL, NULL, &stopped);
        if (s_dump.written != w0 + 1u || s_dump.stop != TEX_DUMP_STOP_COUNT || !stopped || !strstr(stopped, "file limit"))
            return xst_dump_fail("the file limit did not stop the dump after one more file");
    }
    gegpu_texdump_stop();
    xst_dump_resume();

    /* 10. The first write that fails (here the folder is gone) stops the whole dump; no texture
     * after it is tried. Turned on again, it tries again. */
    if (!xst_dump_start(dir)) return 0;
    if (!nk_platform_remove_tree(dir, NULL)) return xst_dump_fail("the test could not remove the dump folder");
    {
        uint32_t g1[XST_W * XST_H], g2[XST_W * XST_H];
        xst_pattern(g1, 227u);
        xst_pattern(g2, 229u);
        const unsigned long long w0 = s_dump.written, f0 = s_dump.failed;
        xst_upload(g1);
        if (!xst_dump_draw()) return xst_dump_fail("a draw failed");
        const char *stopped = NULL;
        gegpu_texdump_status(NULL, NULL, &stopped);
        if (s_dump.stop != TEX_DUMP_STOP_WRITE || s_dump.failed != f0 + 1u || !stopped || !strstr(stopped, "could not be written"))
            return xst_dump_fail("a failed write did not stop the dump and say why");
        if (!nk_platform_mkdir_p(dir)) return xst_dump_fail("the test could not make the folder again");
        xst_upload(g2);
        if (!xst_dump_draw()) return xst_dump_fail("a draw failed");
        if (s_dump.failed != f0 + 1u || s_dump.written != w0 || xst_dump_files(dir) != 0)
            return xst_dump_fail("a texture was tried after the dump stopped on a failed write");
        gegpu_texdump_stop();
        if (!xst_dump_start(dir)) return 0;
        if (s_dump.stop != TEX_DUMP_GOING) return xst_dump_fail("turned on again, the dump stays stopped by an earlier write");
        xst_upload(g2);
        if (!xst_dump_draw()) return xst_dump_fail("a draw failed");
        if (s_dump.written != w0 + 1u) return xst_dump_fail("turned on again, the dump does not write");
    }

    /* 11. An address whose texture is new in every present (an animation) stops being dumped after
     * TEX_DUMP_CHURN_RUN presents in a row, with one note; another address goes on. */
    {
        /* Past the gap first: the texture written just above does not start the run. */
        s_cnt_present_gpu += TEX_DUMP_CHURN_GAP + 1u;
        const unsigned long long w0 = s_dump.written;
        uint32_t frame[XST_W * XST_H];
        for (uint32_t k = 0; k < TEX_DUMP_CHURN_RUN + 5u; k++) {
            xst_pattern(frame, 1000u + k);
            xst_upload(frame);
            s_cnt_present_gpu++;   /* a present between the frames */
            if (!xst_dump_draw()) return xst_dump_fail("a draw failed");
        }
        unsigned long long changing = 0;
        gegpu_texdump_status(NULL, &changing, NULL);
        if (s_dump.written != w0 + (TEX_DUMP_CHURN_RUN - 1u) || s_dump.churn_addrs != 1u || changing != 1u ||
            s_dump.changing != 6u || s_dump.stop != TEX_DUMP_GOING)
            return xst_dump_fail("an animated texture was not cut off after its run, or not counted");
        xst_pattern(frame, 3001u);
        xst_dump_upload_at(XST_DUMP_OTHER, frame);
        s_cnt_present_gpu++;
        if (!xst_dump_draw()) return xst_dump_fail("a draw failed");
        if (s_dump.written != w0 + TEX_DUMP_CHURN_RUN) return xst_dump_fail("another address stopped being dumped with the animated one");
        xst_state(XST_BASE, XST_TEX, 3u, 1, 1);
        /* Within TEX_DUMP_CHURN_QUIET presents of its last change, the cut-off address stays cut off. */
        s_cnt_present_gpu += TEX_DUMP_CHURN_QUIET - 2u;
        if (!xst_dump_draw()) return xst_dump_fail("a draw failed");
        gegpu_texdump_status(NULL, &changing, NULL);
        if (s_dump.written != w0 + TEX_DUMP_CHURN_RUN || s_dump.churn_addrs != 1u || changing != 1u)
            return xst_dump_fail("an animated texture was dumped, or no longer counted, before it held still");
        /* Once it has held still that long, its last content (skipped while it changed) is written,
         * and the address is no longer counted as cut off. */
        s_cnt_present_gpu += 4u;
        if (!xst_dump_draw()) return xst_dump_fail("a draw failed");
        gegpu_texdump_status(NULL, &changing, NULL);
        if (s_dump.written != w0 + TEX_DUMP_CHURN_RUN + 1u || s_dump.churn_addrs != 0u || changing != 0u)
            return xst_dump_fail("an animated texture that held still was not dumped again");
    }
    gegpu_texdump_stop();

    /* 12. SR_TEX_DUMP_BUDGET_MB and SR_TEX_DUMP_MAX_FILES take a whole number in range, nothing else. */
    {
        static const char *const bad_mb[] = { "0", "65537", "12abc", "+5", " 5", "-1", "99999999999999999999" };
        for (size_t i = 0; i < sizeof(bad_mb) / sizeof(bad_mb[0]); i++) {
            xst_setenv("SR_TEX_DUMP_BUDGET_MB", bad_mb[i]);
            const int took = gegpu_texdump_start(dir, err, sizeof(err));
            gegpu_texdump_stop();
            if (took || !strstr(err, "SR_TEX_DUMP_BUDGET_MB")) {
                xst_setenv("SR_TEX_DUMP_BUDGET_MB", NULL);
                return xst_dump_fail("a malformed or out-of-range SR_TEX_DUMP_BUDGET_MB was taken");
            }
        }
        static const struct { const char *v; uint64_t mb; } good_mb[] = { {"1", 1u}, {"65536", 65536u}, {"", 4096u}, {NULL, 4096u} };
        for (size_t i = 0; i < sizeof(good_mb) / sizeof(good_mb[0]); i++) {
            xst_setenv("SR_TEX_DUMP_BUDGET_MB", good_mb[i].v);
            const int took = xst_dump_start(dir);
            const uint64_t budget = s_dump.budget;
            gegpu_texdump_stop();
            if (!took || budget != good_mb[i].mb << 20) {
                xst_setenv("SR_TEX_DUMP_BUDGET_MB", NULL);
                return xst_dump_fail("a valid SR_TEX_DUMP_BUDGET_MB was not taken as given");
            }
        }
        xst_setenv("SR_TEX_DUMP_BUDGET_MB", NULL);
        static const char *const bad_n[] = { "0", "1000001", "7x", "+7" };
        for (size_t i = 0; i < sizeof(bad_n) / sizeof(bad_n[0]); i++) {
            xst_setenv("SR_TEX_DUMP_MAX_FILES", bad_n[i]);
            const int took = gegpu_texdump_start(dir, err, sizeof(err));
            gegpu_texdump_stop();
            if (took || !strstr(err, "SR_TEX_DUMP_MAX_FILES")) {
                xst_setenv("SR_TEX_DUMP_MAX_FILES", NULL);
                return xst_dump_fail("a malformed or out-of-range SR_TEX_DUMP_MAX_FILES was taken");
            }
        }
        xst_setenv("SR_TEX_DUMP_MAX_FILES", "7");
        const int took = xst_dump_start(dir);
        const unsigned long long max_files = s_dump.max_files;
        gegpu_texdump_stop();
        xst_setenv("SR_TEX_DUMP_MAX_FILES", NULL);
        if (!took || max_files != 7u) return xst_dump_fail("a valid SR_TEX_DUMP_MAX_FILES was not taken as given");
    }

    /* 13. A refusal names no full path: a folder whose textures.ini does not read refuses the
     * start with the reader's reason, and the folder itself is named by its last component. */
    {
        char bad[400], ini[600];
        xst_dump_folder(bad, sizeof(bad), "texdump_gpu_badini");
        snprintf(ini, sizeof(ini), "%s/textures.ini", bad);
        if (!nk_platform_mkdir_p(bad) || sr_file_publish_new(ini, "[options]\nversion = 1\n", 22u) != SR_PUBLISH_WRITTEN)
            return xst_dump_fail("the test could not write the unreadable ini");
        const int took = gegpu_texdump_start(bad, err, sizeof(err));
        gegpu_texdump_stop();
        if (took || !strstr(err, "no hash type") || strstr(err, SR_SELFTEST_BUILD_ROOT))
            return xst_dump_fail("an unreadable textures.ini did not refuse the start with its reason alone");
        char text[300];
        snprintf(text, sizeof(text), "texture pack folder '%s' cannot be read", bad);
        tex_dump_redact(text, sizeof(text), bad);
        if (strcmp(text, "texture pack folder 'texdump_gpu_badini" XST_DUMP_NONASCII "' cannot be read") != 0)
            return xst_dump_fail("a reason that names the folder keeps more than its last component");
    }
    memset(&s_dump, 0, sizeof(s_dump));
    printf("gpu texscale selftest: PASS %-12s refused when authentic, nothing while off, cached and new textures "
           "written under their pack names in a non-ASCII folder and read back, none twice, a player's file kept, "
           "a movie skipped, the budget, file limit and a failed write stopping it, an animation cut off, the "
           "variables parsed, refusals without paths\n", "dump");
    return 1;
}

/* Under an edited textures.ini (xxh32 or xxh64 with ignoreAddress), the dump names each file the
 * way that ini asks, without the address, so the pack made from the folder finds the texture at
 * any address. */
static int xst_cell_dump_xxh(int f) {
    (void)f;
    static const char *const kinds[] = { "xxh32", "xxh64" };
    for (size_t k = 0; k < 2u; k++) {
        uint32_t t[XST_W * XST_H];
        xst_pattern(t, 401u + (uint32_t)k);
        char dir[400], ini[600], text[160], name[64], full[64], err[200];
        xst_dump_folder(dir, sizeof(dir), "texdump_gpu_xxh");
        snprintf(ini, sizeof(ini), "%s/textures.ini", dir);
        const int n = snprintf(text, sizeof(text), "[options]\nversion = 1\nhash = %s\nignoreAddress = true\n", kinds[k]);
        if (!nk_platform_mkdir_p(dir) || sr_file_publish_new(ini, text, (size_t)n) != SR_PUBLISH_WRITTEN)
            return xst_dump_fail("the test could not write the edited textures.ini");
        NkTexRepl *pk = nk_texrepl_open(dir, 0, err, sizeof(err));
        if (!pk) return xst_dump_fail(err);
        const uint32_t hash = nk_texrepl_hash_texture(pk, (const uint8_t *)t, sizeof(t), XST_TEX, XST_W, XST_W, XST_H, 3u, 0);
        nk_texrepl_close(pk);
        const uint64_t key = nk_texrepl_cache_key(XST_TEX, 3u, 4u | (4u << 8), 0u);
        snprintf(name, sizeof(name), "%016llx%08x.png", (unsigned long long)(key & 0xFFFFFFFFull), hash);
        snprintf(full, sizeof(full), "%016llx%08x.png", (unsigned long long)key, hash);
        xst_reset();
        nk_tex_scale_set_sync(1);
        memset(&s_dump, 0, sizeof(s_dump));
        if (!xst_dump_start(dir)) return 0;
        xst_upload(t);
        xst_state(XST_BASE, XST_TEX, 3u, 1, 1);
        if (!xst_dump_draw()) return xst_dump_fail("a draw failed");
        gegpu_texdump_stop();
        if (!xst_dump_has(dir, name) || xst_dump_has(dir, full))
            return xst_dump_fail("under ignoreAddress the file is not named without the address");
        pk = nk_texrepl_open(dir, 0, err, sizeof(err));
        if (!pk) return xst_dump_fail(err);
        NkTexReplId id;
        memset(&id, 0, sizeof(id));
        id.addr = XST_TEX + 0x10000u;   /* the same content somewhere else */
        id.format = 3u;
        id.dim = 4u | (4u << 8);
        id.hash = hash;
        NkTexReplHit hit;
        const int found = nk_texrepl_lookup(pk, &id, &hit);
        const int same = found && hit.w == XST_W && hit.h == XST_H && memcmp(hit.rgba, t, sizeof(t)) == 0;
        if (found) nk_texrepl_unpin(pk, &hit);
        nk_texrepl_close(pk);
        if (!same) return xst_dump_fail("a file dumped under ignoreAddress is not found by the pack at another address");
    }
    memset(&s_dump, 0, sizeof(s_dump));
    printf("gpu texscale selftest: PASS %-12s under xxh32 and xxh64 with ignoreAddress the files are named "
           "without the address and found at any address\n", "dump-xxh");
    return 1;
}

/* A palette (CLUT8) texture is dumped under the name PPSSPP gives it (the key and palette identity
 * written out from PPSSPP's formula here), holding the colours the palette gives each texel. */
static int xst_cell_dump_palette(int f) {
    (void)f;
    uint32_t pat[XST_W * XST_H], lut[XST_W * XST_H];
    uint8_t *idx = (uint8_t *)SR_HOST(XST_TEX);
    char dir[400], name[64], err[200];
    xst_dump_folder(dir, sizeof(dir), "texdump_gpu_pal");
    xst_reset();
    nk_tex_scale_set_sync(1);
    memset(&s_dump, 0, sizeof(s_dump));
    xst_pattern(pat, 19u);
    for (int i = 0; i < XST_W * XST_H; i++) idx[i] = (uint8_t)(pat[i] & 0x0Fu);
    sr_gpu_vram_dirty(XST_TEX, XST_W * XST_H);
    xst_state(XST_BASE, XST_TEX, 5u, 1, 1);
    for (uint32_t j = 0; j < 256u; j++) {
        uint32_t entry = 0xFF000000u | (xst_hash(0xD00u + j * 7u) & 0xFFFFFFu);
        memcpy(&s_ge->clutram[j * 4u], &entry, 4);
    }
    s_ge->clut_fmt = 3u | (0xFFu << 8);
    ge_clut_note_load(256u * 4u);
    ge_state_note_external_write();
    for (int i = 0; i < XST_W * XST_H; i++) memcpy(&lut[i], &s_ge->clutram[idx[i] * 4u], 4);
    uint32_t last = 0, max = 0;
    ge_clut_load_extent(&last, &max);
    const uint32_t loaded = last < max ? last : max;
    const uint32_t cluthash = nk_texrepl_xxh32(s_ge->clutram, loaded, 0xC0108888u) ^ (0xC5000000u | s_ge->clut_fmt);
    const uint64_t key = (((uint64_t)(XST_TEX & 0x3FFFFFFFu) << 32) | (4u | (4u << 8))) ^ cluthash;
    const uint32_t hash = nk_texrepl_quick_hash(idx, XST_W * XST_H, XST_TEX);
    snprintf(name, sizeof(name), "%016llx%08x.png", (unsigned long long)key, hash);
    if (!xst_dump_start(dir)) return 0;
    if (!xst_dump_draw()) return xst_dump_fail("a draw failed");
    gegpu_texdump_stop();
    if (!xst_dump_has(dir, name)) return xst_dump_fail("a CLUT8 texture is not dumped under the name PPSSPP gives it");
    NkTexRepl *pk = nk_texrepl_open(dir, 0, err, sizeof(err));
    if (!pk) return xst_dump_fail(err);
    NkTexReplId id;
    memset(&id, 0, sizeof(id));
    id.addr = XST_TEX;
    id.format = 5u;
    id.dim = 4u | (4u << 8);
    id.clut_hash = cluthash;
    id.hash = hash;
    NkTexReplHit hit;
    const int found = nk_texrepl_lookup(pk, &id, &hit);
    const int same = found && hit.w == XST_W && hit.h == XST_H && memcmp(hit.rgba, lut, sizeof(lut)) == 0;
    if (found) nk_texrepl_unpin(pk, &hit);
    nk_texrepl_close(pk);
    memset(&s_dump, 0, sizeof(s_dump));
    if (!same) return xst_dump_fail("a dumped CLUT8 texture does not hold the colours its palette gives");
    printf("gpu texscale selftest: PASS %-12s a CLUT8 texture is dumped under PPSSPP's name with its palette's colours\n",
           "dump-pal");
    return 1;
}

/* A mip-mapped texture is dumped level by level: level 0 under the pack name, level 1 as "_1",
 * each at its own size. */
static int xst_cell_dump_mip(int f) {
    (void)f;
    uint32_t t0[XST_W * XST_H], t1[(XST_W / 2) * (XST_H / 2)];
    char dir[400], name0[64], name1[72];
    xst_dump_folder(dir, sizeof(dir), "texdump_gpu_mip");
    xst_reset();
    nk_tex_scale_set_sync(1);
    memset(&s_dump, 0, sizeof(s_dump));
    xst_pattern(t0, 501u);
    for (int i = 0; i < (XST_W / 2) * (XST_H / 2); i++) t1[i] = 0xFF000000u | xst_hash(600u + (uint32_t)i);
    xst_repl_name(t0, name0, sizeof(name0));   /* also uploads level 0 */
    const uint32_t l1 = XST_TEX + sizeof(t0);
    memcpy(SR_HOST(l1), t1, sizeof(t1));
    sr_gpu_vram_dirty(l1, sizeof(t1));
    snprintf(name1, sizeof(name1), "%.*s_1.png", (int)(strlen(name0) - 4u), name0);
    xst_state(XST_BASE, XST_TEX, 3u, 0, 1);
    s_ge->tex_filter |= 4u;          /* mipmapped minification: the levels are sampled */
    s_ge->tex_max_level = 1u;
    s_ge->tex_level_mode = 1u;       /* constant level */
    s_ge->tex_lod_bias = 0.0f;
    s_ge->tex_mip_addr[0] = l1;
    s_ge->tex_mip_bufw[0] = XST_W / 2;
    s_ge->tex_mip_wlog2[0] = 3u;
    s_ge->tex_mip_hlog2[0] = 3u;
    ge_state_note_external_write();
    int ok = xst_dump_start(dir) && xst_dump_draw();
    gegpu_texdump_stop();
    s_ge->tex_filter &= ~4u;
    s_ge->tex_max_level = 0u;
    s_ge->tex_level_mode = 0u;
    ge_state_note_external_write();
    memset(&s_dump, 0, sizeof(s_dump));
    if (!ok) return xst_dump_fail("the mip-mapped draw failed");
    uint32_t w0 = 0, h0 = 0, w1 = 0, h1 = 0;
    if (!xst_dump_png_size(dir, name0, &w0, &h0) || !xst_dump_png_size(dir, name1, &w1, &h1) ||
        w0 != XST_W || h0 != XST_H || w1 != XST_W / 2 || h1 != XST_H / 2)
        return xst_dump_fail("a mip-mapped texture's levels are not each dumped at their own size");
    printf("gpu texscale selftest: PASS %-12s a mip-mapped texture's levels are dumped, level 1 as _1\n", "dump-mip");
    return 1;
}

int gegpu_texrepl_only_prepare(const char *build_root) {
    char dir[400], path[480];
    snprintf(dir, sizeof(dir), "%s/texrepl_gpu_only_pack", build_root);
    if (!nk_platform_mkdir_p(dir)) return 0;
    uint32_t *rimg = xst_repl_image(3, 0xD000u);
    if (!rimg) return 0;
    /* no hash in the name: the address and size code alone (PPSSPP's "no data hash" alias) */
    const uint64_t key = ((uint64_t)(XST_TEX & 0x3FFFFFFFu) << 32) | (4u | (4u << 8));
    snprintf(path, sizeof(path), "%s/%016llx%08x.png", dir, (unsigned long long)key, 0u);
    const int ok = synpng_write(path, XST_W * 3, XST_H * 3, rimg);
    free(rimg);
    return ok && xst_setenv("SR_TEX_REPLACE", dir);
}

int gegpu_texrepl_only_selftest(void) {
    const int rf = 3;
    uint32_t a[XST_W * XST_H], b[XST_W * XST_H], want[XST_W * XST_H], plain[XST_W * XST_H];
    uint32_t *rimg = xst_repl_image(rf, 0xD000u);
    if (!rimg) return 0;
    if (!s_repl || s_xs_factor != 0 || !nk_tex_scale_config()->budget_bytes) {
        fprintf(stderr, "gpu texscale selftest: FAIL replace-only: the pack was not read at start (pack %p, "
                        "texture scale factor %d, copy budget %llu)\n", (void *)s_repl, s_xs_factor,
                (unsigned long long)nk_tex_scale_config()->budget_bytes);
        free(rimg);
        return 0;
    }
    xst_reset();
    xst_pattern(a, 61u);
    xst_pattern(b, 67u);
    memcpy(SR_HOST(XST_ONLY_TEX_B), b, sizeof(b));
    sr_gpu_vram_dirty(XST_ONLY_TEX_B, sizeof(b));
    xst_upload(a);
    xst_state(XST_BASE, XST_TEX, 3u, 1, 1);
    xst_repl_expect(rimg, rf, want);
    xst_expect(a, 0, 1, plain);
    free(rimg);
    int ok = xst_draw(20.0f, 140.0f);                /* the first draw that finds the listed texture */
    if (!ok || !xst_report("replace-only", 20, 140, want, "with SR_TEX_REPLACE alone, the first draw of a listed texture is not its replacement")) return 0;
    nk_tex_scale_wait_idle();
    s_tmpl_ok = 0;
    ok = xst_draw(60.0f, 140.0f);
    if (!ok || !xst_report("replace-only", 60, 140, want, "with SR_TEX_REPLACE alone, a later draw of a listed texture is not its replacement")) return 0;
    if (memcmp(want, plain, sizeof(want)) == 0) {
        fprintf(stderr, "gpu texscale selftest: FAIL replace-only: the replacement is the texture (vacuous)\n");
        return 0;
    }
    /* A texture the pack does not list (another address): drawn as the game made it, no xBRZ. */
    xst_state(XST_BASE, XST_ONLY_TEX_B, 3u, 1, 1);
    s_tmpl_ok = 0;
    ok = xst_draw(100.0f, 140.0f);
    if (!ok || !xst_report("replace-only", 100, 140, b, "with SR_TEX_REPLACE alone, an unlisted texture is not drawn as made")) return 0;
    printf("gpu texscale selftest: PASS %-12s SR_TEX_REPLACE alone replaces a listed texture, leaves an unlisted one\n",
           "replace-only");

    /* The refusals of a texpack=on start (tex_repl_init): no folder named, a budget outside 1 to
     * 8192 MB, a stored folder that is gone, a folder whose textures.ini names no hash or a hash type
     * the reader does not know, and a pack whose [games] names another
     * game than SR_DISC_ID (or a run with no SR_DISC_ID) each read no pack, so the run draws the
     * game's own textures; the same pack is read for the game it names. The pack this cell drew
     * from is put back afterwards. */
    if (!ace_requested()->texpack) {
        fprintf(stderr, "gpu texscale selftest: FAIL replace-only: the harness line has no texpack=on\n");
        return 0;
    }
    char bad[400], ini[480];
    snprintf(bad, sizeof(bad), "%s/texrepl_gpu_bad_pack", SR_SELFTEST_BUILD_ROOT);
    snprintf(ini, sizeof(ini), "%s/textures.ini", bad);
    FILE *fini = nk_platform_mkdir_p(bad) ? fopen(ini, "wb") : NULL;
    const int ini_ok = fini && fputs("[options]\nversion = 1\n", fini) >= 0;
    if (fini) fclose(fini);
    char other[400], other_ini[480];
    snprintf(other, sizeof(other), "%s/texrepl_gpu_other_game_pack", SR_SELFTEST_BUILD_ROOT);
    snprintf(other_ini, sizeof(other_ini), "%s/textures.ini", other);
    FILE *fother = nk_platform_mkdir_p(other) ? fopen(other_ini, "wb") : NULL;
    const int other_ok =
        fother && fputs("[options]\nhash = quick\n[games]\nULUS-99999 = textures.ini\n", fother) >= 0;
    if (fother) fclose(fother);
    char gone[400], unknown_hash[400], unknown_ini[480];
    snprintf(gone, sizeof(gone), "%s/texrepl_gpu_gone_pack", SR_SELFTEST_BUILD_ROOT);
    snprintf(unknown_hash, sizeof(unknown_hash), "%s/texrepl_gpu_crc32_pack", SR_SELFTEST_BUILD_ROOT);
    snprintf(unknown_ini, sizeof(unknown_ini), "%s/textures.ini", unknown_hash);
    FILE *funknown = nk_platform_mkdir_p(unknown_hash) ? fopen(unknown_ini, "wb") : NULL;
    const int unknown_ok = funknown && fputs("[options]\nhash = crc32\n", funknown) >= 0;
    if (funknown) fclose(funknown);
    const char *disc_env = getenv("SR_DISC_ID");
    char disc_copy[64];
    snprintf(disc_copy, sizeof(disc_copy), "%s", disc_env ? disc_env : "");
    const char *pack_dir = getenv("SR_TEX_REPLACE");
    char pack_copy[400];
    snprintf(pack_copy, sizeof(pack_copy), "%s", pack_dir ? pack_dir : "");
    NkTexRepl *drawn = s_repl;
    s_repl = NULL;
    xst_setenv("SR_TEX_REPLACE", NULL);
    tex_repl_init(1);
    const int unnamed_read = s_repl != NULL;
    tex_repl_shutdown();
    xst_setenv("SR_TEX_REPLACE", pack_copy);
    xst_setenv("SR_TEX_REPLACE_BUDGET_MB", "0");
    tex_repl_init(1);
    const int budget_read = s_repl != NULL;
    tex_repl_shutdown();
    xst_setenv("SR_TEX_REPLACE_BUDGET_MB", NULL);
    xst_setenv("SR_TEX_REPLACE", bad);
    tex_repl_init(1);
    const int malformed_read = s_repl != NULL;
    tex_repl_shutdown();
    xst_setenv("SR_TEX_REPLACE", gone);
    tex_repl_init(1);
    const int gone_read = s_repl != NULL;
    tex_repl_shutdown();
    xst_setenv("SR_TEX_REPLACE", unknown_hash);
    tex_repl_init(1);
    const int unknown_read = s_repl != NULL;
    tex_repl_shutdown();
    xst_setenv("SR_TEX_REPLACE", other);
    xst_setenv("SR_DISC_ID", "NPJH50001");
    tex_repl_init(1);
    const int other_game_read = s_repl != NULL;
    tex_repl_shutdown();
    xst_setenv("SR_DISC_ID", NULL);
    tex_repl_init(1);
    const int no_disc_read = s_repl != NULL;
    tex_repl_shutdown();
    xst_setenv("SR_DISC_ID", "ULUS99999");
    tex_repl_init(1);
    const int named_game_read = s_repl != NULL;
    tex_repl_shutdown();
    /* A folder whose name is outside any one ANSI code page (an accented letter and a CJK
     * ideograph), named the way the launcher names it (the wide environment on Windows), is read:
     * the runtime does not take the folder through the narrow getenv's code-page bytes. */
    char wide_dir[400], wide_ini[480];
    snprintf(wide_dir, sizeof(wide_dir), "%s/texrepl_gpu_pack_\xC3\xA9\xE6\xBC\xA2", SR_SELFTEST_BUILD_ROOT);
    snprintf(wide_ini, sizeof(wide_ini), "%s/textures.ini", wide_dir);
    FILE *fwide = nk_platform_mkdir_p(wide_dir) ? nk_fopen_utf8(wide_ini, "wb") : NULL;
    const int wide_ok = fwide && fputs("[options]\nhash = quick\n", fwide) >= 0;
    if (fwide) fclose(fwide);
#ifdef _WIN32
    wchar_t wide_value[400];
    const int wide_set = swprintf(wide_value, 400, L"%hs/texrepl_gpu_pack_é漢", SR_SELFTEST_BUILD_ROOT) > 0 &&
                         _wputenv_s(L"SR_TEX_REPLACE", wide_value) == 0;
#else
    const int wide_set = xst_setenv("SR_TEX_REPLACE", wide_dir);
#endif
    tex_repl_init(1);
    const int wide_read = s_repl != NULL;
    tex_repl_shutdown();
    xst_setenv("SR_DISC_ID", disc_copy[0] ? disc_copy : NULL);
    xst_setenv("SR_TEX_REPLACE", pack_copy);
    s_repl = drawn;
    if (!ini_ok || !other_ok || !unknown_ok || !wide_ok || !wide_set || unnamed_read || budget_read ||
        malformed_read || gone_read || unknown_read || other_game_read || no_disc_read || !named_game_read ||
        !wide_read) {
        fprintf(stderr, "gpu texscale selftest: FAIL replace-only: %s\n",
                !ini_ok || !other_ok || !unknown_ok || !wide_ok || !wide_set ? "cannot write the refused packs"
                : unnamed_read ? "texpack=on with no SR_TEX_REPLACE read a pack"
                : budget_read ? "SR_TEX_REPLACE_BUDGET_MB=0 read a pack"
                : malformed_read ? "a textures.ini with no hash type read a pack"
                : gone_read ? "a stored pack whose folder is gone was read"
                : unknown_read ? "a textures.ini with an unknown hash type read a pack"
                : other_game_read ? "a pack whose [games] names another game was read"
                : no_disc_read ? "a pack whose [games] names a game was read by a run with no SR_DISC_ID"
                : !named_game_read ? "a pack was refused for the game its [games] names"
                : "a pack in a folder with a non-ASCII name was refused");
        return 0;
    }
    printf("gpu texscale selftest: PASS %-12s texpack=on with no folder, a bad budget, a gone folder, a "
           "malformed pack or another game's pack reads none; a pack is read for the game it names and from a non-ASCII folder\n",
           "replace-only");
    return 1;
}

int gegpu_texscale_selftest(void) {
    const int f = s_xs_factor;
    if (!f || !s_xs_smart2d) {
        fprintf(stderr, "gpu texscale selftest: FAIL texture scaling or the smart 2D filter is off (factor %d, "
                        "smart2d %d): the Ace line or SR_TEX_SCALE/SR_SMART_2D did not take\n", f, s_xs_smart2d);
        return 0;
    }
    int ok = 1;
    static int (*const cells[])(int) = { xst_cell_scaled_draw, xst_cell_large, xst_cell_upload_cap,
                                         xst_cell_not_ready, xst_cell_rewrite, xst_cell_clut, xst_cell_replace,
                                         xst_cell_replace_palette, xst_cell_dump, xst_cell_dump_xxh,
                                         xst_cell_dump_palette, xst_cell_dump_mip, xst_cell_budget, xst_cell_rtt, xst_cell_color_test, xst_cell_onetoone };
    static const char *const names[] = { "scaled-draw", "large", "upload-cap", "not-ready", "rewrite", "clut",
                                         "replace", "replace-pal", "dump", "dump-xxh",
                                         "dump-pal", "dump-mip", "budget", "rtt", "color-test",
                                         "onetoone" };
    /* The cells before onetoone check copies through 1:1 sprites, so they run with the 1:1 rule
     * off; onetoone turns it back on. */
    s_xs_onetoone_plain = 0;
    const size_t vram = 0x00200000u, fb0 = XST_BASE & 0x001FFFFFu, fb1 = fb0 + XST_STRIDE * FB_H * 4u;
    uint8_t *before = (uint8_t *)malloc(vram), clut[sizeof(s_ge->clutram)];
    if (!before) return 0;
    for (size_t i = 0; ok && i < sizeof(cells) / sizeof(cells[0]); i++) {
        ok = cells[i](f);
        if (!ok) break;
        /* After the cell: one more draw of its last texture while every pending scale lands, and
         * a VRAM read of the texture. Outside the frame that draw writes, guest VRAM and the
         * palette are the bytes the CPU left: no scaling step writes guest memory. */
        memcpy(before, SR_HOST(0x04000000u), vram);
        memcpy(clut, s_ge->clutram, sizeof(clut));
        s_tmpl_ok = 0;
        ok = xst_draw(200.0f, 200.0f);
        nk_tex_scale_wait_idle();
        s_tmpl_ok = 0;
        ok = ok && xst_draw(200.0f, 200.0f);
        (void)hook_vram_read(XST_TEX, 4096u);
        const uint8_t *now = (const uint8_t *)SR_HOST(0x04000000u);
        size_t bad = vram;
        for (size_t b = 0; b < vram; b++)
            if ((b < fb0 || b >= fb1) && now[b] != before[b]) { bad = b; break; }
        if (ok && (bad != vram || memcmp(clut, s_ge->clutram, sizeof(clut)) != 0)) {
            fprintf(stderr, "gpu texscale selftest: FAIL guest-bytes after %s: %s changed (VRAM offset 0x%zx)\n",
                    names[i], bad != vram ? "a guest VRAM byte" : "a CLUT byte", bad);
            ok = 0;
        }
    }
    free(before);
    s_xs_onetoone_plain = 1;
    if (ok) printf("gpu texscale selftest: PASS %-12s VRAM outside the frame and the CLUT unchanged\n", "guest-bytes");
    ok = ok && xst_cell_smart2d();
    xst_reset();
    nk_tex_scale_set_sync(0);
    /* Under --twin, the host twin (#2050) draws every cell a second time. xst_reset compared each
     * twin with its image before freeing them. With `--twin native`, the pixels must be equal, so a
     * scaled copy, its factor and clamp, the one-to-one swap and smart 2D all reach the twin pass.
     * As in gegpu_coherence_selftest, a twin found behind its image and re-seeded also fails, and a
     * requested twin must have run to the end: unavailable or turned off fails, never a quiet OK. */
    const char *twin_env = getenv("SR_GPU_TWIN_COLOR");
    if (twin_mode_parse(twin_env) > 0 && !s_twin) {
        fprintf(stderr, "gpu texscale selftest: FAIL twin-identity: the host twin was requested "
                        "(SR_GPU_TWIN_COLOR=%s) but %s\n", twin_env,
                s_twin_unavailable ? "this device refused it"
                : s_cnt_twin_alloc_fail ? "it turned off after a failed allocation" : "it is off");
        ok = 0;
    } else if (s_twin) {
        if (s_twin_identity_failures || s_cnt_twin_reseed) {
            fprintf(stderr, "gpu texscale selftest: FAIL twin-identity (%u identity failure(s), printed above; "
                            "%lu re-seed(s), last lag: %s)\n", s_twin_identity_failures, s_cnt_twin_reseed,
                    s_twin_lag_cause ? s_twin_lag_cause : "none");
            ok = 0;
        } else {
            printf("gpu texscale selftest: PASS %-12s every twin in step with its image%s\n", "twin-identity",
                   s_twin_color ? "" : ", pixel for pixel");
        }
    }
    return ok;
}
