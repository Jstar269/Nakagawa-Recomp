/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * glyph_gpu.inc - sharp game text on the Vulkan GE renderer (glyph_recon.h, #2110). Included by
 * ge_gpu.c after tex_get, whose statics it shares.
 *
 * A texture cache entry that text is drawn from gets a second, host-only image beside its
 * guest-resolution one: the entry's size times the render scale, holding the rebuilt version of
 * every rectangle a sprite draw has sampled from it. The image is made when the first rectangle
 * of the page passes the checks, so a page that is not text costs its table of tiles (about 28 KiB
 * of host memory, one per cache entry the feature looks at) and no GPU memory, descriptor set or
 * synchronous clear. Draw -> tile is exact: a sprite that maps one
 * texel to one pixel over whole texels [x0, x1) x [y0, y1) of a page that looks like anti-aliased
 * text is a tile, rebuilt once on the renderer thread (so a run is the same whatever the host's
 * speed), checked against the guest's bytes (nk_glyph_verify) and uploaded into its own place in
 * the image. The draw then binds the image with the scaled-copy flag (the shader samples it on its
 * own lattice, one rebuilt texel under each device pixel centre); a draw whose tile is not ready,
 * was refused, or is stale keeps the game's own picture, exactly.
 *
 * Tile validity: a tile belongs to the entry content (its generation, e->gen) it was checked against. e->gen is a number
 * the texture cache hands out each time it decodes and uploads the entry's texels (a new entry, or a
 * rewrite found on lookup), never reused by another entry: the cache's content hash samples part of a
 * page, so a rewrite can leave it unchanged, and an address can name another entry after an eviction.
 * When the guest rewrites the page, the entry's generation changes; the next draw decodes the page once, compares
 * every tile's texels with the ones it was built from, keeps the tiles whose texels are unchanged
 * and drops the rest to be rebuilt. A tile is never bound for content it was not built from.
 */

/* The entry content a tile is keyed on: the cache's generation (see above). */
#ifdef NK_GLYPH_GPU_MUTANT_KEY_ON_HASH
#define GL_KEY(e) ((e)->hash)   /* gpu-glyph-mutant: the sampled content hash, which a rewrite can leave unchanged */
#else
#define GL_KEY(e) ((e)->gen)
#endif

#define GL_MAX_TILES 768
#define GL_IDX 2048u   /* a power of two, more than GL_MAX_TILES */
enum { GL_TILE_VALID = 1, GL_TILE_REFUSED, GL_TILE_STALE };

typedef struct GlTile {
    uint16_t x, y, w, h;      /* the rectangle, in texels of the page */
    uint8_t state, verdict;   /* GL_TILE_*, NkGlyphVerdict */
    uint64_t rect_hash;       /* of the page's texels in the rectangle when it was checked */
    uint64_t entry_gen;      /* the entry content (generation) it was checked against */
} GlTile;

typedef struct GlSet {
    VkImage img; VkDeviceMemory mem; VkImageView view; VkSampler smp; VkDescriptorSet set;
    VkImageLayout layout;
    int factor;
    uint64_t bytes;           /* budget the image holds; the image handles stay null until a tile passes */
    uint64_t synced_gen;     /* the entry content every tile was last compared with */
    int n;
    GlTile t[GL_MAX_TILES];
    uint16_t idx[GL_IDX];     /* open-addressed table of tile numbers + 1 */
} GlSet;

static uint64_t s_cnt_gl_bound, s_cnt_gl_built, s_cnt_gl_kept, s_cnt_gl_stale, s_cnt_gl_capped,
                s_cnt_gl_budget, s_cnt_gl_full, s_cnt_gl_decodes, s_cnt_gl_images;

/* The render scale is the renderer's, and the tile buffer is sized for its largest. */
_Static_assert(MAX_SCALE <= NK_GLYPH_MAX_FACTOR, "the glyph rebuild must take every render scale the renderer runs at");
#define GL_TILE_SIDE (NK_GLYPH_MAX_SIDE * MAX_SCALE)
static uint64_t s_cnt_gl_refused[NK_GLYPH_VERDICT_COUNT];

static uint64_t gl_hash_rect(const uint32_t *page, int pw, int x0, int y0, int w, int h) {
    uint64_t hsh = 1469598103934665603ull;
    for (int y = 0; y < h; y++) {
        const uint32_t *row = page + (size_t)(y0 + y) * (size_t)pw + (size_t)x0;
        for (int x = 0; x < w; x++) {
            hsh ^= row[x];
            hsh *= 1099511628211ull;
        }
    }
    return hsh ^ ((uint64_t)w << 48) ^ ((uint64_t)h << 56);
}

static uint32_t gl_slot(int x, int y, int w, int h) {
    uint32_t k = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)w * 83492791u ^ (uint32_t)h * 2654435761u;
    return (k ^ (k >> 15)) & (GL_IDX - 1u);
}

static GlTile *gl_find(GlSet *g, int x, int y, int w, int h) {
    for (uint32_t s = gl_slot(x, y, w, h), n = 0; n < GL_IDX; n++, s = (s + 1u) & (GL_IDX - 1u)) {
        if (!g->idx[s]) return NULL;
        GlTile *t = &g->t[g->idx[s] - 1u];
        if (t->x == x && t->y == y && t->w == w && t->h == h) return t;
    }
    return NULL;
}

static GlTile *gl_add(GlSet *g, int x, int y, int w, int h) {
    if (g->n >= GL_MAX_TILES) return NULL;
    uint32_t s = gl_slot(x, y, w, h);
    while (g->idx[s]) s = (s + 1u) & (GL_IDX - 1u);
    GlTile *t = &g->t[g->n];
    memset(t, 0, sizeof(*t));
    t->x = (uint16_t)x; t->y = (uint16_t)y; t->w = (uint16_t)w; t->h = (uint16_t)h;
    g->idx[s] = (uint16_t)(++g->n);
    return t;
}

/* The page the tiles are built from: the entry's texels, decoded once per content (N tiles of a new page cost one
 * decode, not N). It is the glyph code's own, so the texture upload's scratch is never overwritten. */
static uint32_t s_gl_page[512 * 512];
static const TexEnt *s_gl_page_e;
static uint64_t s_gl_page_gen;

static void gl_decode(TexEnt *e) {
    if (s_gl_page_e == e && s_gl_page_gen == GL_KEY(e)) return;
    ge_decode_tex_rgba(s_gl_page);
    s_gl_page_e = e;
    s_gl_page_gen = GL_KEY(e);
    s_cnt_gl_decodes++;
}

/* Free the glyph image of a set, giving back its budget. */
static void gl_image_free(GlSet *g) {
    if (g->set) vkFreeDescriptorSets(s_dev, s_dpool_tex, 1, &g->set);
    if (g->smp) vkDestroySampler(s_dev, g->smp, NULL);
    if (g->view) vkDestroyImageView(s_dev, g->view, NULL);
    if (g->img) vkDestroyImage(s_dev, g->img, NULL);
    if (g->mem) vkFreeMemory(s_dev, g->mem, NULL);
    if (g->bytes) nk_tex_scale_budget_give(g->bytes);
    g->set = VK_NULL_HANDLE;
    g->smp = VK_NULL_HANDLE;
    g->view = VK_NULL_HANDLE;
    g->img = VK_NULL_HANDLE;
    g->mem = VK_NULL_HANDLE;
    g->bytes = 0;
}

/* Free an entry's glyph image and tiles and give back its budget. The caller has drained every
 * command that could reference them (eviction, shutdown). */
static void tex_gl_free(TexEnt *e) {
    GlSet *g = e->gl;
    if (s_gl_page_e == e) s_gl_page_e = NULL;
    if (!g) return;
    gl_image_free(g);
    free(g);
    e->gl = NULL;
}

/* The entry's table of tiles, made the first time a 1:1 sprite over an eligible page asks. */
static GlSet *gl_tiles_for(TexEnt *e, int factor) {
    if (e->gl) return e->gl->factor == factor ? e->gl : NULL;
    GlSet *g = (GlSet *)calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->factor = factor;
    g->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    e->gl = g;
    return g;
}

/* The set's image, made when its first tile passes the checks: transparent, so no texel is ever read before a
 * tile wrote it (a draw binds the image only over a valid tile, but the sampler's neighbours at an edge are
 * then transparent at worst). */
static int gl_image_for(TexEnt *e, GlSet *g) {
    if (g->img) return 1;
    const uint32_t sw = (uint32_t)(e->w * g->factor), sh = (uint32_t)(e->h * g->factor);
    const uint64_t bytes = (uint64_t)sw * sh * 4u;
    if (!nk_tex_scale_budget_take(bytes)) {
        s_cnt_gl_budget++;
        return 0;
    }
    g->bytes = bytes;
    VkSamplerCreateInfo sci;
    TexMipKey none;
    memset(&none, 0, sizeof(none));
    const unsigned filter = tex_filter_variant(s_ge->tex_filter);
    tex_sampler_info((filter & 2u) != 0, (filter & 1u) != 0, s_ge->tex_wrap & 1, (s_ge->tex_wrap >> 8) & 1, &none, &sci);
    if (e->pending) submit_pending();
    if (!make_image_levels(sw, sh, 1u, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &g->img, &g->mem) ||
        !make_view_levels(g->img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1u, &g->view) ||
        vkCreateSampler(s_dev, &sci, NULL, &g->smp) != VK_SUCCESS ||
        !(g->set = make_descriptor(g->view, g->smp, s_dpool_tex))) {
        /* Nothing recorded references the half-made image yet. */
        gl_image_free(g);
        return 0;
    }
    /* A fresh command buffer of its own (a boundary, as the render-target transition is): nothing is
     * recording here, and the clear must be in the queue before any tile is copied into the image. */
    if (s_nbatch) submit_pending();
    if (!cmd_begin()) {
        gl_image_free(g);
        return 0;
    }
    g->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_layout(s_cmd, g->img, VK_IMAGE_ASPECT_COLOR_BIT, &g->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue zero;
    memset(&zero, 0, sizeof(zero));
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(s_cmd, g->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &rng);
    to_layout(s_cmd, g->img, VK_IMAGE_ASPECT_COLOR_BIT, &g->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!cmd_submit_wait(SR_PERF_GE_TEXTURE_UPLOAD)) {
        gl_image_free(g);
        return 0;
    }
    s_cnt_gl_images++;
    return 1;
}

/* Upload a built tile into its place in the entry's glyph image. */
static int gl_store(GlSet *g, const GlTile *t, const uint32_t *px) {
    const int f = g->factor;
    const uint32_t tw = (uint32_t)(t->w * f), th = (uint32_t)(t->h * f);
    UploadReservation upload;
    if (!upload_reserve((VkDeviceSize)tw * th * 4u, &upload)) return 0;
    memcpy(upload.map, px, (size_t)tw * th * 4u);
    to_layout(s_cmd, g->img, VK_IMAGE_ASPECT_COLOR_BIT, &g->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bic = {0};
    bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bic.imageSubresource.layerCount = 1;
    bic.imageOffset.x = t->x * f;
    bic.imageOffset.y = t->y * f;
    bic.imageExtent.width = tw; bic.imageExtent.height = th; bic.imageExtent.depth = 1;
    bic.bufferOffset = upload.offset;
    vkCmdCopyBufferToImage(s_cmd, upload.buffer, g->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    to_layout(s_cmd, g->img, VK_IMAGE_ASPECT_COLOR_BIT, &g->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return upload_finish(&upload, SR_PERF_GE_TEXTURE_UPLOAD);
}

/* Rectangles checked or rebuilt this present, at most NK_GLYPH_PER_PRESENT (a count, not a time). */
static uint64_t s_gl_present = UINT64_MAX;
static int s_gl_ops;
static int gl_take_op(void) {
    const uint64_t now = xs_now();
    if (s_gl_present != now) {
        s_gl_present = now;
        s_gl_ops = 0;
    }
#ifdef NK_GLYPH_GPU_MUTANT_NO_CAP
    if (0) {   /* gpu-glyph-mutant: no per-present count */
#else
    if (s_gl_ops >= NK_GLYPH_PER_PRESENT) {
#endif
        s_cnt_gl_capped++;
        return 0;
    }
    s_gl_ops++;
    return 1;
}

static NkGlyphFormat gl_format_of(uint32_t tex_fmt) {
    switch (tex_fmt) {
    case 0: return NK_GLYPH_FMT_5650;
    case 1: return NK_GLYPH_FMT_5551;
    case 2: return NK_GLYPH_FMT_4444;
    case 3: return NK_GLYPH_FMT_8888;
    case 4: return NK_GLYPH_FMT_T4;
    case 5: return NK_GLYPH_FMT_T8;
    case 6: return NK_GLYPH_FMT_T16;
    case 7: return NK_GLYPH_FMT_T32;
    default: return NK_GLYPH_FMT_DXT;
    }
}

/* The rectangle a sprite samples, in whole texels, when it maps one texel to one pixel: the span
 * of u and of v, each end within 1/16 texel of a texel boundary. */
static int gl_sprite_rect(float ul, float ur, float vt, float vb, int *x0, int *y0, int *w, int *h) {
    const float u0 = fminf(ul, ur), u1 = fmaxf(ul, ur), v0 = fminf(vt, vb), v1 = fmaxf(vt, vb);
    const float r[4] = { u0, u1, v0, v1 };
    int q[4];
    for (int i = 0; i < 4; i++) {
        q[i] = (int)floorf(r[i] + 0.5f);
        if (fabsf(r[i] - (float)q[i]) > 1.0f / 16.0f) return 0;
    }
    *x0 = q[0]; *y0 = q[2]; *w = q[1] - q[0]; *h = q[3] - q[2];
    return 1;
}

/* Compare every tile with the entry's decoded page (s_gl_page; main RAM only, a page in VRAM was refused before
 * any decode), and keep the ones whose texels are unchanged. */
static void gl_sync(TexEnt *e) {
    GlSet *g = e->gl;
    for (int i = 0; i < g->n; i++) {
        GlTile *t = &g->t[i];
        if (t->entry_gen == GL_KEY(e)) continue;
        if (gl_hash_rect(s_gl_page, e->w, t->x, t->y, t->w, t->h) == t->rect_hash) {
            t->entry_gen = GL_KEY(e);
            s_cnt_gl_kept++;
        } else if (t->state != GL_TILE_STALE) {
            t->state = GL_TILE_STALE;
            s_cnt_gl_stale++;
        }
    }
    g->synced_gen = GL_KEY(e);
}

/* A sprite draw, batch `b`, from the entry `e` its descriptor set belongs to, over the texel
 * rectangle given. Binds the glyph image when the rectangle's tile is valid; builds the tile first
 * when it is new or stale and the present's count allows. Returns 1 when the batch was changed. */
static int glyph_apply(Batch *b, TexEnt *e, float ul, float ur, float vt, float vb) {
    if (!s_glyphs || !e || s_tex_noscale) return 0;
    int x0, y0, w, h;
    if (!gl_sprite_rect(ul, ur, vt, vb, &x0, &y0, &w, &h)) return 0;
    if (w < NK_GLYPH_MIN_SIDE || h < NK_GLYPH_MIN_SIDE || w > NK_GLYPH_MAX_SIDE || h > NK_GLYPH_MAX_SIDE ||
        x0 < 0 || y0 < 0 || x0 + w > e->w || y0 + h > e->h)
        return 0;
    const int f = s_scale;
    if ((e->w > e->h ? e->w : e->h) * f > NK_TEX_SCALE_MAX_DIM) return 0;
    NkGlyphSource src = { gl_format_of(s_ge->tex_fmt), is_vram(e->addr), 1 + (int)e->mip.max_level, f };
    const NkGlyphVerdict sv = nk_glyph_source_check(&src);
    if (sv != NK_GLYPH_OK) {
        s_cnt_gl_refused[sv]++;
        return 0;
    }
    GlSet *g = e->gl;
    GlTile *t = g ? gl_find(g, x0, y0, w, h) : NULL;
#ifdef NK_GLYPH_GPU_MUTANT_NO_HASH_CHECK
    if (!t) {   /* gpu-glyph-mutant: a tile is bound for whatever content the page holds now */
#else
    const int fresh = g && g->synced_gen == GL_KEY(e);
    if (!(t && fresh && t->entry_gen == GL_KEY(e))) {
#endif
        /* New, stale, or the page changed since the tiles were compared: the page is decoded once per content. */
        if (!gl_take_op()) return 0;
        gl_decode(e);
        if (!g && !(g = gl_tiles_for(e, f))) {
            s_cnt_gl_refused[NK_GLYPH_REFUSE_MEMORY]++;
            return 0;
        }
        if (g->factor != f) return 0;
        if (g->synced_gen != GL_KEY(e)) {
            gl_sync(e);
            t = gl_find(g, x0, y0, w, h);
        }
        if (!t || t->entry_gen != GL_KEY(e)) {
            if (!t) t = gl_add(g, x0, y0, w, h);
            if (!t) {
                s_cnt_gl_full++;
                return 0;
            }
            static uint32_t tile[GL_TILE_SIDE * GL_TILE_SIDE];
            const NkGlyphVerdict v = nk_glyph_rebuild(s_gl_page, e->w, e->h, x0, y0, w, h, f, tile);
            t->rect_hash = gl_hash_rect(s_gl_page, e->w, x0, y0, w, h);
            t->entry_gen = GL_KEY(e);
            t->verdict = (uint8_t)v;
            if (v != NK_GLYPH_OK) {
                t->state = GL_TILE_REFUSED;
                s_cnt_gl_refused[v]++;
            } else if (!gl_image_for(e, g)) {
                t->state = GL_TILE_REFUSED;
                t->verdict = (uint8_t)NK_GLYPH_REFUSE_MEMORY;
                s_cnt_gl_refused[NK_GLYPH_REFUSE_MEMORY]++;
            } else {
                if (e->pending) submit_pending();   /* recorded batches may sample the image */
                if (!gl_store(g, t, tile)) {
                    t->state = GL_TILE_REFUSED;
                    t->verdict = (uint8_t)NK_GLYPH_REFUSE_MEMORY;
                    s_cnt_gl_refused[NK_GLYPH_REFUSE_MEMORY]++;
                } else {
                    t->state = GL_TILE_VALID;
                    s_cnt_gl_built++;
                }
            }
            g->synced_gen = GL_KEY(e);
        }
    }
#ifdef NK_GLYPH_GPU_MUTANT_NO_HASH_CHECK
    if (!t || t->state != GL_TILE_VALID || !e->gl || !e->gl->img) return 0;
#else
    if (!t || t->state != GL_TILE_VALID || t->entry_gen != GL_KEY(e) || !e->gl || !e->gl->img) return 0;
#endif
    e->pending = 1;
    b->dset = e->gl->set;
    b->dset_plain = VK_NULL_HANDLE;
    /* Nearest: each device pixel centre is a tile texel centre, so nearest takes that texel exactly at any
     * factor, where linear snaps its weight down to 1/16 and a factor that is not a power of two (1/3 of a texel
     * is not on the shader's 1/4096 coordinate lattice) would leave a pixel up to 1/16 of a neighbour off. */
    b->pc.cfg[0] = (int32_t)((((uint32_t)b->pc.cfg[0] & ~((uint32_t)7u << (F_TSCALE_SHIFT + 8))) |
                              ((uint32_t)(f - 1) << (F_TSCALE_SHIFT + 8))) | ((uint32_t)F_NEAREST << 8));
    s_cnt_gl_bound++;
    return 1;
}

static void glyph_report(void) {
    if (!s_glyphs_asked) return;
    fprintf(stderr, "GLYPHS: draws (bound=%llu, tiles_built=%llu, tiles_kept=%llu, tiles_stale=%llu, "
                    "page_decodes=%llu, images=%llu, capped=%llu, budget_short=%llu, tiles_full=%llu)\n",
            (unsigned long long)s_cnt_gl_bound, (unsigned long long)s_cnt_gl_built,
            (unsigned long long)s_cnt_gl_kept, (unsigned long long)s_cnt_gl_stale,
            (unsigned long long)s_cnt_gl_decodes, (unsigned long long)s_cnt_gl_images,
            (unsigned long long)s_cnt_gl_capped,
            (unsigned long long)s_cnt_gl_budget, (unsigned long long)s_cnt_gl_full);
    char refused[256];
    size_t used = 0;
    refused[0] = '\0';
    for (int v = 1; v < NK_GLYPH_VERDICT_COUNT; v++)
        if (s_cnt_gl_refused[v] && used + 40 < sizeof(refused))
            used += (size_t)snprintf(refused + used, sizeof(refused) - used, "%s%s=%llu", used ? ", " : "",
                                     nk_glyph_verdict_name((NkGlyphVerdict)v), (unsigned long long)s_cnt_gl_refused[v]);
    fprintf(stderr, "GLYPHS: exit (refused: %s)\n", used ? refused : "none");
}
