// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Restates src/rt/ge.c's project_cvtx and draw_prim's cull order, and ge_gpu.c's put_vert, as
// reference functions (ge_vertex, pack_rgba, snap_ref, backend_tri, backend_sprite, cull_swap);
// the rest of this file was written for it.
// Modified by Nakagawa Recomp contributors, 2026-11-09.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/*
 * motion_tap_selftest.c - the frame store, the bridge to the matcher and the in-between builder
 * of render-state motion (motion_tap.h, #2081), on synthetic tap records with the real matcher
 * between the two frames:
 *
 *   - built positions against a double-precision reference for a translating quad, a strip
 *     skinned by two bones (the reference blends the bone matrix, not the clip positions) and a
 *     morph (the reference blends the weights); vertices exactly on and one float below a 28.4
 *     tie; a sprite's floor rule in both corner orders; fog with one end saturated; colour;
 *   - t = 1 giving B's own values bit for bit through the same path as any t, and the guard
 *     that reports it when it does not;
 *   - the index-hash gate, a draw the matcher cannot pair drawn as in B and counted with the
 *     rest of the frame untouched, and a frame under the matcher's coverage threshold held;
 *   - every reason that stops a frame, one case each, and the store's overflow;
 *   - the store itself: what is current during a raster call, the matcher's list, its boxes,
 *     inline marks and camera.
 *
 * This file stands in for ge.c: emit() makes the tap's calls in ge.c's order, and ge_vertex()
 * and backend_tri() repeat what ge.c hands over and what the backend stores. The same checks
 * run against the production ge.o in ge_cpu_profile_validate_selftest.c.
 *
 * Built from motion_tap.c, motion_match.c and this file alone, with nothing else of the runtime.
 */
#include "motion_tap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail;

#define CHECK(cond, ...) do { \
        if (!(cond)) { s_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                       fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
    } while (0)

#define MAXV 16u
#define MAXP 512u
#define LIST 0x08100000u

/* View: identity. Projection: perspective, w = -z, near 1, far 100. Both in the GE's order. */
static const float k_view[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
static const float k_proj[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -101.0f / 99.0f, -1, 0, 0, -200.0f / 99.0f, 0 };
/* Clip x -1..1 is screen x 0..480, clip y 1..-1 is screen y 0..272, clip z -1..1 is depth 0..65535. */
static const float k_viewport[6] = { 240.0f, -136.0f, 32767.5f, 2048.0f, 2048.0f, 32767.5f };
static const float k_offset[2] = { 1808.0f, 1912.0f };

/* One PRIM as this file's stand-in for ge.c draws it. */
typedef struct {
    GeMotionPrim p;
    GeMotionVtx v[MAXV];            /* the vertex reference at each PRIM position */
    uint8_t dec[MAXV];              /* decision per assembled triangle or sprite: GE_MOTION_DRAW unless set */
    uint8_t corners[MAXV];          /* per sprite: corners computed, 2 unless set to 1 */
    uint8_t index[MAXV * 2u];       /* its index bytes when the vertex type is indexed */
    int index_seen;                 /* 0: the bytes lie where the tap does not read */
} Draw;

static Draw draw_any(int type, uint32_t count, uint32_t serial, int through) {
    Draw d;
    memset(&d, 0, sizeof d);
    d.p.type = (uint8_t)type;
    d.p.through = (uint8_t)through;
    d.p.count = count;
    d.p.vtype = through ? 0x0080011Cu : 0x0000011Cu;
    d.p.vbase = 0x08800000u + serial * 0x1000u;
    d.p.list_addr = LIST;
    d.p.cmd_addr = LIST + 0x100u + serial * 8u;
    d.p.fbp = 0x04000000u;
    d.p.fbw = 512u;
    d.p.zbp = 0x04110000u;
    d.p.zbw = 512u;
    d.p.state_class = GE_MOTION_CLASS_ZTEST | GE_MOTION_CLASS_ZWRITE;
    memcpy(d.p.viewport, k_viewport, sizeof d.p.viewport);
    memcpy(d.p.offset, k_offset, sizeof d.p.offset);
    if (!through) {
        d.p.view = k_view;
        d.p.proj = k_proj;
    }
    for (uint32_t i = 0; i < MAXV; i++) {
        d.v[i].clip[3] = 1.0f;
        d.v[i].fog_arg = 1.0f;
        d.v[i].r = d.v[i].g = d.v[i].b = d.v[i].a = 255;
    }
    return d;
}

static void set_clip(GeMotionVtx *v, float x, float y, float z, float w) {
    v->clip[0] = x;
    v->clip[1] = y;
    v->clip[2] = z;
    v->clip[3] = w;
}

static void set_rgba(GeMotionVtx *v, int r, int g, int b, int a) {
    v->r = r;
    v->g = g;
    v->b = b;
    v->a = a;
}

/* What ge.c hands to the rasterizer for a vertex reference: project_cvtx, or the through-mode
 * vertex as load_vtx leaves it. */
static void ge_vertex(const MotionTapPrim *p, const GeMotionVtx *r, GeVtx *o) {
    memset(o, 0, sizeof *o);
    if (p->through) {
        o->x = r->clip[0];
        o->y = r->clip[1];
        o->z = r->clip[2];
        o->rw = 1.0f;
        o->u = r->u;
        o->v = r->v;
        o->fog = 1.0f;
    } else {
        float inv_w = 1.0f / r->clip[3];
        float ndcx = r->clip[0] * inv_w, ndcy = r->clip[1] * inv_w, ndcz = r->clip[2] * inv_w;
        o->x = ndcx * p->viewport[0] + p->viewport[3] - p->offset[0];
        o->y = ndcy * p->viewport[1] + p->viewport[4] - p->offset[1];
        float sz = ndcz * p->viewport[2] + p->viewport[5];
        o->z = sz < 0.0f ? 0.0f : sz > 65535.0f ? 65535.0f : sz;
        o->rw = inv_w;
        o->u = r->u * inv_w;
        o->v = r->v * inv_w;
        o->fog = r->fog_arg < 0.0f ? 0.0f : r->fog_arg > 1.0f ? 1.0f : r->fog_arg;
    }
    o->r = r->r;
    o->g = r->g;
    o->b = r->b;
    o->a = r->a;
}

static uint32_t pack_rgba(const GeVtx *g) {
    const uint32_t r = (uint32_t)(g->r < 0 ? 0 : g->r > 255 ? 255 : g->r);
    const uint32_t gg = (uint32_t)(g->g < 0 ? 0 : g->g > 255 ? 255 : g->g);
    const uint32_t b = (uint32_t)(g->b < 0 ? 0 : g->b > 255 ? 255 : g->b);
    const uint32_t a = (uint32_t)(g->a < 0 ? 0 : g->a > 255 ? 255 : g->a);
    return r | (gg << 8) | (b << 16) | (a << 24);
}

static float snap_ref(float v) {
    return floorf(v * 16.0f + 0.375f) * (1.0f / 16.0f);
}

/* What the backend stores for a triangle (its put_vert): the snap, and under flat shading the
 * last vertex's colour. */
static void backend_tri(int flat, const GeVtx *g, MotionBuiltVert *o) {
    for (int c = 0; c < 3; c++) {
        o[c].x = snap_ref(g[c].x);
        o[c].y = snap_ref(g[c].y);
        o[c].z = g[c].z;
        o[c].rw = g[c].rw;
        o[c].u = g[c].u;
        o[c].v = g[c].v;
        o[c].fog = g[c].fog;
        o[c].rgba = pack_rgba(&g[flat ? 2 : c]);
    }
}

/* What it stores for a sprite: the floored rectangle, and the second corner's depth, fog and
 * colour. The texture coordinates are not the builder's. */
static void backend_sprite(const GeVtx *g, MotionBuiltVert *o) {
    const float xa = floorf(fminf(g[0].x, g[1].x)), xb = floorf(fmaxf(g[0].x, g[1].x));
    const float ya = floorf(fminf(g[0].y, g[1].y)), yb = floorf(fmaxf(g[0].y, g[1].y));
    for (int c = 0; c < 2; c++) {
        o[c].x = snap_ref(c ? xb : xa);
        o[c].y = snap_ref(c ? yb : ya);
        o[c].z = g[1].z;
        o[c].rw = 1.0f;
        o[c].u = o[c].v = 0.0f;
        o[c].fog = g[1].fog;
        o[c].rgba = pack_rgba(&g[1]);
    }
}

static int cull_swap(const Draw *d, int i) {
    return (d->p.flags & GE_MOTION_PRIM_CULL) && (1 ^ (d->p.type == 4 ? (i & 1) : 0));
}

/* The tap's calls for one PRIM, in ge.c's order. */
static void emit(MotionTapFrame *f, const Draw *d) {
    const GeMotionTap *tap = motion_tap_frame_listener(f);
    GeMotionPrim p = d->p;
    p.index_bytes = (p.flags & GE_MOTION_PRIM_INDEXED) && d->index_seen ? d->index : NULL;
    tap->prim(tap->user, &p);
    MotionTapPrim state;
    memset(&state, 0, sizeof state);
    state.through = p.through;
    memcpy(state.viewport, p.viewport, sizeof state.viewport);
    memcpy(state.offset, p.offset, sizeof state.offset);
    const int type = p.type, count = (int)p.count;
    if (type >= 3 && type <= 5) {
        int k = 0;
        for (int i = 2; i < count; i++) {
            int i0, i1, i2;
            if (type == 3) { if (i % 3 != 2) continue; i0 = i - 2; i1 = i - 1; i2 = i; }
            else if (type == 4) { i0 = i - 2; i1 = i - 1; i2 = i; }
            else { i0 = 0; i1 = i - 1; i2 = i; }
            if (cull_swap(d, i)) { int t = i0; i0 = i1; i1 = t; }
            const uint16_t pos[3] = { (uint16_t)i0, (uint16_t)i1, (uint16_t)i2 };
            GeMotionVtx v[3];
            GeVtx g[3];
            for (int c = 0; c < 3; c++) {
                v[c] = d->v[pos[c]];
                ge_vertex(&state, &v[c], &g[c]);
            }
            const int dec = d->dec[k++];
            tap->tri(tap->user, pos, v, dec);
            if (dec == GE_MOTION_CLIP) tap->fan(tap->user, 0);
            if (dec != GE_MOTION_DROP) tap->rastered(tap->user, &g[0], &g[1], &g[2]);
        }
    } else if (type == 6) {
        for (int i = 1, k = 0; i < count; i += 2, k++) {
            const uint16_t pos[2] = { (uint16_t)(i - 1), (uint16_t)i };
            GeMotionVtx v[2];
            GeVtx g[2];
            for (int c = 0; c < 2; c++) {
                v[c] = d->v[pos[c]];
                ge_vertex(&state, &v[c], &g[c]);
            }
            const int dec = d->dec[k];
            tap->sprite(tap->user, pos, v, d->corners[k] ? d->corners[k] : 2, dec);
            if (dec == GE_MOTION_DRAW) tap->rastered(tap->user, &g[0], &g[1], NULL);
        }
    } else if (type <= 2) {
        /* Points and lines: each raster call is reported, and nothing is published for it. */
        const int calls = type == 0 ? count : type == 1 ? count / 2 : count - 1;
        for (int k = 0; k < calls; k++) tap->line_point(tap->user, type == 0 ? 1 : 2);
    }
}

static MotionTapFrame *frame_with_cap(const Draw *d, uint32_t n, const MotionTapFrame *prev, size_t cap) {
    MotionTapFrame *f = motion_tap_frame_create(cap);
    if (!f) {
        fprintf(stderr, "motion_tap_selftest: no memory\n");
        exit(2);
    }
    for (uint32_t i = 0; i < n; i++) emit(f, &d[i]);
    motion_tap_frame_close(f, prev);
    return f;
}

static MotionTapFrame *frame(const Draw *d, uint32_t n) {
    return frame_with_cap(d, n, NULL, 0);
}

/* A frame whose list, after its PRIMs, also drew what the tap names nothing for: `tris` raster
 * calls for triangles (a Bezier or spline patch's) and `sprites` for rectangles (an
 * immediate-mode one), then `lines` and `points` (a patch drawn as lines or points, or
 * immediate-mode ones). */
static MotionTapFrame *frame_unnamed(const Draw *d, uint32_t n, int tris, int sprites, int lines, int points) {
    MotionTapFrame *f = motion_tap_frame_create(0);
    if (!f) {
        fprintf(stderr, "motion_tap_selftest: no memory\n");
        exit(2);
    }
    const GeMotionTap *tap = motion_tap_frame_listener(f);
    GeVtx g[3];
    memset(g, 0, sizeof g);
    for (int c = 0; c < 3; c++) {
        g[c].x = 300.0f + 8.0f * (float)c;
        g[c].y = 200.0f + 4.0f * (float)(c & 1);
        g[c].rw = 1.0f;
    }
    for (uint32_t i = 0; i < n; i++) emit(f, &d[i]);
    for (int k = 0; k < tris; k++) tap->rastered(tap->user, &g[0], &g[1], &g[2]);
    for (int k = 0; k < sprites; k++) tap->rastered(tap->user, &g[0], &g[1], NULL);
    for (int k = 0; k < lines; k++) tap->line_point(tap->user, 2);
    for (int k = 0; k < points; k++) tap->line_point(tap->user, 1);
    motion_tap_frame_close(f, NULL);
    return f;
}

static float float_bits(uint32_t u) {
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

static int32_t g_pair[MAXP];
static MotionMatchResult g_match;

/* The real matcher on the two frames' lists, then the builder's plan. */
static int plan(MotionBuild *b, const MotionTapFrame *fa, const MotionTapFrame *fb) {
    MotionMatchFrame ma, mb;
    MotionMatchParams p;
    motion_tap_match_frame(fa, &ma);
    motion_tap_match_frame(fb, &mb);
    motion_match_default_params(&p);
    if (mb.nprims > MAXP) {
        fprintf(stderr, "motion_tap_selftest: scene too large\n");
        exit(2);
    }
    motion_match(&ma, &mb, &p, NULL, g_pair, NULL, &g_match);
    return motion_build_plan(b, fa, fb, g_pair, &g_match);
}

static MotionBuild *builder(void) {
    MotionBuild *b = motion_build_create();
    if (!b) {
        fprintf(stderr, "motion_tap_selftest: no memory\n");
        exit(2);
    }
    return b;
}

static const MotionTapPrim *prim_of(const MotionTapView *v, uint32_t tri) {
    for (uint32_t s = 0; s < v->nprims; s++)
        if (tri >= v->prims[s].first_tri && tri - v->prims[s].first_tri < v->prims[s].ntris) return &v->prims[s];
    return NULL;
}

static int same_bits(float a, float b) {
    return memcmp(&a, &b, sizeof a) == 0;
}

static int same_vert(const MotionBuiltVert *a, const MotionBuiltVert *b, int with_uv) {
    return same_bits(a->x, b->x) && same_bits(a->y, b->y) && same_bits(a->z, b->z) && same_bits(a->rw, b->rw) &&
           same_bits(a->fog, b->fog) && a->rgba == b->rgba &&
           (!with_uv || (same_bits(a->u, b->u) && same_bits(a->v, b->v)));
}

/* What B handed over for record `tri`, by this file's stand-ins for ge.c and the backend. */
static int submitted_of(const MotionTapView *v, uint32_t tri, MotionBuiltVert *out) {
    const MotionTapPrim *p = prim_of(v, tri);
    const MotionTapTri *t = &v->tris[tri];
    const int n = p->type == 6 ? 2 : 3;
    GeVtx g[3];
    for (int c = 0; c < n; c++) ge_vertex(p, &v->refs[p->first_ref + t->pos[c]], &g[c]);
    const int flat = (p->flags & GE_MOTION_PRIM_FLAT) != 0;
    if (n == 2) backend_sprite(g, out);
    else backend_tri(flat, g, out);
    /* motion_tap.c's own statement of the rule, which the ge.o test takes as B's values. */
    MotionBuiltVert exported[3];
    memset(exported, 0, sizeof exported);
    motion_backend_vertices(flat, g, n, exported);
    for (int c = 0; c < n; c++)
        CHECK(same_bits(exported[c].x, out[c].x) && same_bits(exported[c].y, out[c].y) &&
              same_bits(exported[c].z, out[c].z) && same_bits(exported[c].rw, out[c].rw) &&
              same_bits(exported[c].u, out[c].u) && same_bits(exported[c].v, out[c].v) &&
              same_bits(exported[c].fog, out[c].fog) && exported[c].rgba == out[c].rgba,
              "motion_backend_vertices: record %u vertex %d is not put_vert's", tri, c);
    return n;
}

/* Every record that gets built values: t = 1 through motion_build_prim equals what B handed
 * over, bit for bit, and the guard agrees. Returns how many records that was. */
static uint32_t check_t1(const char *cell, MotionBuild *b, const MotionTapFrame *fb) {
    MotionTapView v;
    uint32_t built = 0;
    motion_tap_frame_view(fb, &v);
    for (uint32_t tri = 0; tri < v.ntris; tri++) {
        MotionBuiltVert out[3], sub[3];
        if (motion_build_prim(b, tri, 1.0f, out, NULL) != MOTION_ITEM_BUILT) continue;
        const int n = submitted_of(&v, tri, sub);
        for (int c = 0; c < n; c++)
            CHECK(same_vert(&out[c], &sub[c], n == 3), "%s: record %u vertex %d at t = 1 is not B's "
                  "(x %.9g/%.9g y %.9g/%.9g z %.9g/%.9g rw %.9g/%.9g fog %.9g/%.9g rgba %08x/%08x)", cell, tri, c,
                  (double)out[c].x, (double)sub[c].x, (double)out[c].y, (double)sub[c].y, (double)out[c].z,
                  (double)sub[c].z, (double)out[c].rw, (double)sub[c].rw, (double)out[c].fog, (double)sub[c].fog,
                  (unsigned)out[c].rgba, (unsigned)sub[c].rgba);
        CHECK(motion_build_check(b, tri, sub) == 1, "%s: the guard refuses record %u", cell, tri);
        built++;
    }
    CHECK(motion_build_result(b)->why == 0 && motion_build_result(b)->t1_mismatch == 0,
          "%s: the guard left a reason (%#x)", cell, (unsigned)motion_build_result(b)->why);
    return built;
}

/* ---- positions against a double-precision reference ------------------------------------------ */

static uint32_t s_exact;            /* positions compared exactly (the reference is not near a tie) */

/* `got` is the 28.4 snap of `ref`: exactly, unless the reference lies so close to a tie that
 * float rounding may fall on either side of it. */
static void expect_snapped(const char *cell, const char *axis, double ref, float got) {
    const double g = ref * 16.0 + 0.375, lo = floor(g), frac = g - lo;
    CHECK((double)got * 16.0 == floor((double)got * 16.0), "%s: %s %.9g is off the 28.4 grid", cell, axis,
          (double)got);
    if (frac > 0.01 && frac < 0.99) {
        s_exact++;
        CHECK((double)got == lo / 16.0, "%s: %s is %.9g, the reference %.9g snaps to %.9g", cell, axis,
              (double)got, ref, lo / 16.0);
    } else {
        CHECK(fabs((double)got - lo / 16.0) <= 1.0 / 16.0, "%s: %s is %.9g, the reference %.9g snaps near %.9g",
              cell, axis, (double)got, ref, lo / 16.0);
    }
}

static void expect_screen(const char *cell, const MotionTapPrim *p, const double c[4], const MotionBuiltVert *o) {
    double x = c[0], y = c[1], z = c[2], rw = 1.0;
    if (!p->through) {
        rw = 1.0 / c[3];
        x = c[0] * rw * (double)p->viewport[0] + (double)p->viewport[3] - (double)p->offset[0];
        y = c[1] * rw * (double)p->viewport[1] + (double)p->viewport[4] - (double)p->offset[1];
        z = c[2] * rw * (double)p->viewport[2] + (double)p->viewport[5];
        z = z < 0.0 ? 0.0 : z > 65535.0 ? 65535.0 : z;
    }
    expect_snapped(cell, "x", x, o->x);
    expect_snapped(cell, "y", y, o->y);
    CHECK(fabs((double)o->z - z) <= 1e-5 * (1.0 + fabs(z)), "%s: z is %.9g, reference %.9g", cell, (double)o->z, z);
    CHECK(fabs((double)o->rw - rw) <= 1e-6 * fabs(rw), "%s: rw is %.9g, reference %.9g", cell, (double)o->rw, rw);
}

typedef void (*RefClip)(uint32_t pos, double t, double out[4]);

/* Every record of B is built at t and sits where the reference puts its PRIM positions. */
static void check_positions(const char *cell, const MotionBuild *b, const MotionTapFrame *fb, float t, RefClip ref) {
    MotionTapView v;
    motion_tap_frame_view(fb, &v);
    for (uint32_t tri = 0; tri < v.ntris; tri++) {
        MotionBuiltVert out[3];
        const MotionTapPrim *p = prim_of(&v, tri);
        const int kind = motion_build_prim(b, tri, t, out, NULL);
        CHECK(kind == MOTION_ITEM_BUILT, "%s: record %u at t = %g is %d, not built", cell, tri, (double)t, kind);
        if (kind != MOTION_ITEM_BUILT) continue;
        for (int c = 0; c < 3; c++) {
            double clip[4];
            ref(v.tris[tri].pos[c], (double)t, clip);
            expect_screen(cell, p, clip, &out[c]);
        }
    }
}

static Draw s_quad[2];              /* the quad in A and in B */

static void quad_ref(uint32_t pos, double t, double out[4]) {
    for (int k = 0; k < 4; k++)
        out[k] = (double)s_quad[0].v[pos].clip[k] * (1.0 - t) + (double)s_quad[1].v[pos].clip[k] * t;
}

static void quad_at(Draw *d, float cx, float cy) {
    for (uint32_t i = 0; i < 4; i++) {
        set_clip(&d->v[i], cx + ((i >> 1) ? 0.2f : -0.2f), cy + ((i & 1u) ? 0.2f : -0.2f), 1.0f, 2.0f);
        d->v[i].u = (i >> 1) ? 32.0f : 0.0f;
        d->v[i].v = (i & 1u) ? 32.0f : 0.0f;
    }
}

/* A quad (a strip of four) moving 24 pixels, one vertex changing colour. */
static void cell_quad(void) {
    s_quad[0] = draw_any(4, 4, 0, 0);
    s_quad[1] = s_quad[0];
    quad_at(&s_quad[0], -0.3f, 0.1f);
    quad_at(&s_quad[1], -0.1f, 0.15f);
    set_rgba(&s_quad[0].v[3], 255, 0, 0, 255);
    set_rgba(&s_quad[1].v[3], 0, 255, 0, 255);
    s_quad[1].v[0].u = 8.0f;        /* a raw UV that scrolled: B's is used, and counted */
    MotionTapFrame *fa = frame(&s_quad[0], 1), *fb = frame(&s_quad[1], 1);
    MotionBuild *b = builder();
    CHECK(plan(b, fa, fb) == 1, "quad: the frame can be built (why %#x)", (unsigned)motion_build_result(b)->why);
    const MotionBuildResult *r = motion_build_result(b);
    CHECK(r->draws == 1 && r->paired == 1 && r->unpaired == 0 && r->built == 2 && r->copied == 0 && r->flags == 0,
          "quad: one paired draw, two built triangles (%u %u %u %u %u)", (unsigned)r->draws, (unsigned)r->paired,
          (unsigned)r->unpaired, (unsigned)r->built, (unsigned)r->copied);
    CHECK(r->uv_changed == 1, "quad: the one vertex whose raw UV changed is counted (%u)", (unsigned)r->uv_changed);
    static const float ts[] = { 0.25f, 0.5f, 0.75f, 0.0f, 1.0f };
    for (size_t i = 0; i < sizeof ts / sizeof ts[0]; i++) check_positions("quad", b, fb, ts[i], quad_ref);

    MotionTapView vb;
    motion_tap_frame_view(fb, &vb);
    MotionBuiltVert out[3];
    MotionVert mv[3];
    /* Record 1 is positions 1, 2, 3. At t = 0.5: rw of the middle clip position, B's raw UV
     * times it, and the colour halfway, rounded. */
    CHECK(motion_build_prim(b, 1, 0.5f, out, mv) == MOTION_ITEM_BUILT, "quad: record 1 is built");
    CHECK(out[2].rgba == 0xFF008080u, "quad: the colour halfway is %08x", (unsigned)out[2].rgba);
    CHECK(out[0].rgba == 0xFFFFFFFFu, "quad: an unchanged colour stays (%08x)", (unsigned)out[0].rgba);
    CHECK(out[2].rw == 0.5f && out[2].u == 16.0f && out[2].v == 16.0f, "quad: u and v are B's raw UV times rw "
          "(%g %g %g)", (double)out[2].rw, (double)out[2].u, (double)out[2].v);
    CHECK(out[2].fog == 1.0f, "quad: no fog is factor 1 (%g)", (double)out[2].fog);
    for (int c = 0; c < 3; c++) {
        const uint32_t pos = vb.tris[1].pos[c];
        CHECK(mv[c].state == MOTION_V_MATCHED && mv[c].prim == 0 && mv[c].index == pos &&
              memcmp(mv[c].clip_a, s_quad[0].v[pos].clip, sizeof mv[c].clip_a) == 0 &&
              memcmp(mv[c].clip_b, s_quad[1].v[pos].clip, sizeof mv[c].clip_b) == 0,
              "quad: the source record of vertex %d", c);
    }
    CHECK(motion_build_prim(b, 0, 0.5f, out, mv) == MOTION_ITEM_BUILT && (mv[0].flags & MOTION_V_UV_SCROLL) &&
          !(mv[1].flags & MOTION_V_UV_SCROLL), "quad: the scrolled UV is marked on its vertex only");
    CHECK(out[0].u == 8.0f * out[0].rw, "quad: the scrolled vertex uses B's UV");
    /* t = 0 is A's position (through B's state, which is A's here). */
    MotionTapView va;
    MotionBuiltVert sub[3];
    motion_tap_frame_view(fa, &va);
    submitted_of(&va, 0, sub);
    CHECK(motion_build_prim(b, 0, 0.0f, out, NULL) == MOTION_ITEM_BUILT, "quad: t = 0 is built");
    for (int c = 0; c < 3; c++)
        CHECK(same_bits(out[c].x, sub[c].x) && same_bits(out[c].y, sub[c].y) && same_bits(out[c].z, sub[c].z) &&
              same_bits(out[c].rw, sub[c].rw), "quad: t = 0 is where A drew vertex %d", c);
    CHECK(motion_build_prim(b, 0, -0.01f, out, NULL) == MOTION_ITEM_NONE &&
          motion_build_prim(b, 0, 1.01f, out, NULL) == MOTION_ITEM_NONE &&
          motion_build_prim(b, 0, (float)NAN, out, NULL) == MOTION_ITEM_NONE &&
          motion_build_prim(b, 2, 0.5f, out, NULL) == MOTION_ITEM_NONE &&
          motion_build_prim(b, 0, 0.5f, NULL, NULL) == MOTION_ITEM_NONE, "quad: a bad t, record or buffer gives nothing");
    CHECK(check_t1("quad", b, fb) == 2, "quad: both triangles are checked at t = 1");
    motion_build_free(b);
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);
}

/* The strip: eight positions, the weight of bone 1 rising along it. Bone 0 is the identity;
 * bone 1 turns 20 degrees about z and moves between A and B. */
static float s_strip_p[8][3], s_strip_w1[8], s_bone1[2][12];

static void mat43d(const double m[12], const double p[3], double o[3]) {
    for (int a = 0; a < 3; a++) o[a] = m[a] * p[0] + m[3 + a] * p[1] + m[6 + a] * p[2] + m[9 + a];
}

static void proj_d(const double p[3], double o[4]) {
    for (int a = 0; a < 4; a++)
        o[a] = (double)k_proj[a] * p[0] + (double)k_proj[4 + a] * p[1] + (double)k_proj[8 + a] * p[2] +
               (double)k_proj[12 + a];
}

static void proj_f(const float p[3], float o[4]) {
    for (int a = 0; a < 4; a++) o[a] = k_proj[a] * p[0] + k_proj[4 + a] * p[1] + k_proj[8 + a] * p[2] + k_proj[12 + a];
}

/* The clip position under the bone matrix blended at t, in double precision. */
static void strip_ref(uint32_t pos, double t, double out[4]) {
    double m[12], p[3], q[3], s[3];
    for (int k = 0; k < 12; k++) m[k] = (double)s_bone1[0][k] * (1.0 - t) + (double)s_bone1[1][k] * t;
    for (int a = 0; a < 3; a++) p[a] = (double)s_strip_p[pos][a];
    mat43d(m, p, q);
    for (int a = 0; a < 3; a++) s[a] = p[a] * (1.0 - (double)s_strip_w1[pos]) + q[a] * (double)s_strip_w1[pos];
    proj_d(s, out);
}

/* The same in single precision with one frame's bone, as a GE computes it. */
static void strip_clip(const float bone[12], uint32_t pos, float out[4]) {
    const float *p = s_strip_p[pos];
    float q[3], s[3];
    for (int a = 0; a < 3; a++) q[a] = bone[a] * p[0] + bone[3 + a] * p[1] + bone[6 + a] * p[2] + bone[9 + a];
    for (int a = 0; a < 3; a++) s[a] = p[a] * (1.0f - s_strip_w1[pos]) + q[a] * s_strip_w1[pos];
    proj_f(s, out);
}

static void cell_strip(void) {
    static const float ident[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
    const float cs = cosf(20.0f * 3.14159265f / 180.0f), sn = sinf(20.0f * 3.14159265f / 180.0f);
    const float turned[12] = { cs, sn, 0, -sn, cs, 0, 0, 0, 1, 0.3f, 0.2f, 0 };
    memcpy(s_bone1[0], ident, sizeof ident);
    memcpy(s_bone1[1], turned, sizeof turned);
    Draw d[2];
    d[0] = draw_any(4, 8, 1, 0);
    d[0].p.flags |= GE_MOTION_PRIM_CULL;    /* the strip's triangles alternate their first two positions */
    d[1] = d[0];
    for (uint32_t i = 0; i < 8; i++) {
        s_strip_p[i][0] = -0.9f + 0.25f * (float)i;
        s_strip_p[i][1] = (i & 1u) ? 0.3f : -0.3f;
        s_strip_p[i][2] = -4.0f;
        s_strip_w1[i] = (float)i / 7.0f;
        strip_clip(s_bone1[0], i, d[0].v[i].clip);
        strip_clip(s_bone1[1], i, d[1].v[i].clip);
    }
    MotionTapFrame *fa = frame(&d[0], 1), *fb = frame(&d[1], 1);
    MotionBuild *b = builder();
    CHECK(plan(b, fa, fb) == 1, "strip: the frame can be built (why %#x)", (unsigned)motion_build_result(b)->why);
    /* Position 0 has weight 0 for bone 1: its triangle (0, 1, 2) still moves with the other two. */
    CHECK(motion_build_result(b)->built == 6 && motion_build_result(b)->copied == 0, "strip: six built triangles "
          "(%u)", (unsigned)motion_build_result(b)->built);
    MotionTapView v;
    motion_tap_frame_view(fb, &v);
    CHECK(v.ntris == 6 && v.tris[0].pos[0] == 1 && v.tris[0].pos[1] == 0 && v.tris[0].pos[2] == 2 &&
          v.tris[1].pos[0] == 1 && v.tris[1].pos[1] == 2 && v.tris[1].pos[2] == 3,
          "strip: the positions are kept in submission order");
    static const float ts[] = { 0.25f, 0.5f, 0.75f };
    for (size_t i = 0; i < sizeof ts / sizeof ts[0]; i++) check_positions("strip", b, fb, ts[i], strip_ref);
    CHECK(check_t1("strip", b, fb) == 6, "strip: six triangles are checked at t = 1");
    motion_build_free(b);
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);
}

/* The morph: two targets per position, the weights (1, 0) in A and (0.25, 0.75) in B. */
static float s_morph_p[2][6][3];
static const float k_morph_w[2][2] = { { 1.0f, 0.0f }, { 0.25f, 0.75f } };

static void morph_ref(uint32_t pos, double t, double out[4]) {
    double p[3];
    const double w0 = (double)k_morph_w[0][0] * (1.0 - t) + (double)k_morph_w[1][0] * t;
    const double w1 = (double)k_morph_w[0][1] * (1.0 - t) + (double)k_morph_w[1][1] * t;
    for (int a = 0; a < 3; a++) p[a] = (double)s_morph_p[0][pos][a] * w0 + (double)s_morph_p[1][pos][a] * w1;
    proj_d(p, out);
}

static void cell_morph(void) {
    Draw d[2];
    d[0] = draw_any(3, 6, 2, 0);
    d[1] = d[0];
    for (uint32_t i = 0; i < 6; i++) {
        s_morph_p[0][i][0] = -0.5f + 0.37f * (float)(i % 3u);
        s_morph_p[0][i][1] = 0.1f + 0.29f * (float)(i / 3u) - 0.21f * (float)(i % 2u);
        s_morph_p[0][i][2] = -3.0f - 0.1f * (float)i;
        s_morph_p[1][i][0] = s_morph_p[0][i][0] * 1.4f + 0.2f;
        s_morph_p[1][i][1] = s_morph_p[0][i][1] * 0.6f - 0.3f;
        s_morph_p[1][i][2] = s_morph_p[0][i][2] - 0.8f;
        for (int f = 0; f < 2; f++) {
            float p[3];
            for (int a = 0; a < 3; a++)
                p[a] = s_morph_p[0][i][a] * k_morph_w[f][0] + s_morph_p[1][i][a] * k_morph_w[f][1];
            proj_f(p, d[f].v[i].clip);
        }
    }
    MotionTapFrame *fa = frame(&d[0], 1), *fb = frame(&d[1], 1);
    MotionBuild *b = builder();
    CHECK(plan(b, fa, fb) == 1, "morph: the frame can be built (why %#x)", (unsigned)motion_build_result(b)->why);
    static const float ts[] = { 0.25f, 0.5f, 0.75f };
    for (size_t i = 0; i < sizeof ts / sizeof ts[0]; i++) check_positions("morph", b, fb, ts[i], morph_ref);
    CHECK(check_t1("morph", b, fb) == 2, "morph: both triangles are checked at t = 1");
    motion_build_free(b);
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);
}

/* The snap is floor(v * 16 + 0.375) / 16: a position whose v * 16 + 0.375 is a whole number is
 * on a tie and takes the step above; one float below it takes the step below. Through mode, so
 * every value here is exact. */
static void cell_tie(void) {
    Draw d[2];
    d[0] = draw_any(3, 3, 3, 1);
    d[1] = d[0];
    const float tie = 9.9765625f;                    /* 9.9765625 * 16 + 0.375 = 160 */
    set_clip(&d[0].v[0], 9.0f, 20.0f, 100.0f, 1.0f);
    set_clip(&d[1].v[0], 10.953125f, 21.953125f, 300.0f, 1.0f);              /* halfway: the tie in x and in y */
    set_clip(&d[0].v[1], 9.0f, 60.0f, 100.0f, 1.0f);
    set_clip(&d[1].v[1], 10.953125f - ldexpf(1.0f, -19), 60.0f, 100.0f, 1.0f);   /* halfway: one float below it */
    set_clip(&d[0].v[2], 40.0f, 40.0f, 100.0f, 1.0f);
    set_clip(&d[1].v[2], 40.21875f, 40.0f, 100.0f, 1.0f);                    /* halfway 40.109375: between steps */
    MotionTapFrame *fa = frame(&d[0], 1), *fb = frame(&d[1], 1);
    MotionBuild *b = builder();
    MotionBuiltVert out[3];
    CHECK(plan(b, fa, fb) == 1 && motion_build_prim(b, 0, 0.5f, out, NULL) == MOTION_ITEM_BUILT, "tie: built");
    CHECK(tie * 16.0f + 0.375f == 160.0f, "tie: the constant is a tie");
    CHECK(out[0].x == 10.0f && out[0].y == 21.0f, "tie: on the tie the step above (%.9g, %.9g)", (double)out[0].x,
          (double)out[0].y);
    CHECK(out[1].x == 9.9375f, "tie: one float below it the step below (%.9g)", (double)out[1].x);
    CHECK(out[2].x == 40.125f, "tie: 40.109375 takes the step above its 0.625 point (%.9g)", (double)out[2].x);
    CHECK(out[0].z == 200.0f && out[0].rw == 1.0f && out[0].fog == 1.0f, "tie: through mode mixes z, rw is 1 "
          "(%g %g %g)", (double)out[0].z, (double)out[0].rw, (double)out[0].fog);
    CHECK(check_t1("tie", b, fb) == 1, "tie: checked at t = 1");
    motion_build_free(b);
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);
}

/* ---- gates ------------------------------------------------------------------------------------ */

static Draw indexed_tri(uint32_t serial, float x) {
    Draw d = draw_any(3, 3, serial, 0);
    d.p.vtype |= 2u << 11;          /* 16-bit indices */
    d.p.flags |= GE_MOTION_PRIM_INDEXED;
    d.p.ibase = 0x08900000u + serial * 0x100u;
    d.p.index_size = 6;
    d.index_seen = 1;
    for (uint32_t i = 0; i < 3; i++) {
        d.index[i * 2u] = (uint8_t)(i + 4u);
        set_clip(&d.v[i], x + 0.1f * (float)i, 0.2f * (float)(i & 1u), 1.0f, 2.0f);
    }
    return d;
}

/* Position i names the same vertex only when the index bytes are the same. */
static void cell_index_gate(void) {
    Draw a = indexed_tri(4, -0.4f), same = indexed_tri(4, -0.3f), other = same, unseen = same;
    other.index[2] = 9;             /* position 1 names another vertex */
    unseen.index_seen = 0;
    MotionTapFrame *fa = frame(&a, 1), *fs = frame(&same, 1), *fo = frame(&other, 1), *fu = frame(&unseen, 1);
    MotionTapFrame *fau = frame(&unseen, 1);
    MotionTapView va, vs, vo, vu;
    motion_tap_frame_view(fa, &va);
    motion_tap_frame_view(fs, &vs);
    motion_tap_frame_view(fo, &vo);
    motion_tap_frame_view(fu, &vu);
    CHECK(va.prims[0].index_state == MOTION_INDEX_HASHED && va.prims[0].index_size == 6 &&
          va.prims[0].index_hash == vs.prims[0].index_hash && va.prims[0].index_hash != vo.prims[0].index_hash &&
          vu.prims[0].index_state == MOTION_INDEX_UNSEEN, "index gate: the hash follows the index bytes");
    MotionBuild *b = builder();
    MotionBuiltVert out[3];
    MotionVert mv[3];
    CHECK(plan(b, fa, fs) == 1 && motion_build_result(b)->paired == 1 && motion_build_result(b)->unpaired == 0 &&
          motion_build_prim(b, 0, 0.5f, out, mv) == MOTION_ITEM_BUILT && mv[0].state == MOTION_V_MATCHED,
          "index gate: equal index bytes pair");
    CHECK(check_t1("index gate", b, fs) == 1, "index gate: checked at t = 1");
    /* The matcher pairs them (same signature and addresses); the gate does not. */
    CHECK(plan(b, fa, fo) == 1 && g_pair[0] == 0, "index gate: the matcher pairs the changed index buffer");
    const MotionBuildResult *r = motion_build_result(b);
    uint32_t n = 0;
    const uint32_t *list = motion_build_unpaired(b, &n);
    CHECK(r->why == 0 && r->paired == 0 && r->unpaired == 1 && r->unpaired_index == 1 && r->built == 0 &&
          r->copied == 1 && n == 1 && list[0] == 0, "index gate: other index bytes do not pair (%u %u %u)",
          (unsigned)r->paired, (unsigned)r->unpaired, (unsigned)r->unpaired_index);
    CHECK(motion_build_prim(b, 0, 0.5f, out, mv) == MOTION_ITEM_COPY_B && mv[1].state == MOTION_V_UNMATCHED &&
          memcmp(mv[1].clip_a, mv[1].clip_b, sizeof mv[1].clip_a) == 0, "index gate: it is drawn as in B");
    CHECK(plan(b, fau, fu) == 1 && motion_build_result(b)->unpaired_index == 1 &&
          motion_build_prim(b, 0, 0.5f, out, NULL) == MOTION_ITEM_COPY_B,
          "index gate: index bytes the tap did not see pair with nothing");
    /* A different vertex count under one pair is refused too (the matcher never gives one). */
    Draw longer = draw_any(3, 6, 4, 0);
    for (uint32_t i = 0; i < 6; i++) set_clip(&longer.v[i], -0.3f + 0.1f * (float)i, 0.2f * (float)(i & 1u), 1.0f, 2.0f);
    MotionTapFrame *fl = frame(&longer, 1);
    MotionMatchResult ok;
    const int32_t forced[1] = { 0 };
    memset(&ok, 0, sizeof ok);
    CHECK(motion_build_plan(b, fa, fl, forced, &ok) == 1 && motion_build_result(b)->unpaired_shape == 1 &&
          motion_build_result(b)->unpaired == 1, "index gate: a pair of different shapes is refused");
    motion_build_free(b);
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fs);
    motion_tap_frame_free(fo);
    motion_tap_frame_free(fu);
    motion_tap_frame_free(fau);
    motion_tap_frame_free(fl);
}

static void sprite_at(Draw *d, uint32_t k, float x0, float y0, float x1, float y1, float z1) {
    set_clip(&d->v[k * 2u], x0, y0, 7.0f, 1.0f);
    set_clip(&d->v[k * 2u + 1u], x1, y1, z1, 1.0f);
}

/* A sprite is the rectangle of its two floored corners, with the second corner's depth, fog
 * and colour, whichever corner is the upper left. */
static void cell_sprite(void) {
    Draw d[2];
    d[0] = draw_any(6, 6, 5, 1);
    d[1] = d[0];
    sprite_at(&d[0], 0, 10.25f, 20.75f, 30.5f, 40.25f, 100.0f);
    sprite_at(&d[1], 0, 11.25f, 21.25f, 31.5f, 40.75f, 300.0f);
    sprite_at(&d[0], 1, 100.5f, 50.5f, 80.25f, 30.25f, 9.0f);       /* the corners the other way round */
    sprite_at(&d[1], 1, 98.5f, 48.5f, 78.25f, 28.25f, 9.0f);
    sprite_at(&d[0], 2, 200.0f, 100.0f, 216.0f, 116.0f, 5.0f);      /* does not move */
    sprite_at(&d[1], 2, 200.0f, 100.0f, 216.0f, 116.0f, 5.0f);
    set_rgba(&d[0].v[0], 1, 2, 3, 4);
    set_rgba(&d[1].v[0], 1, 2, 3, 4);
    set_rgba(&d[0].v[1], 100, 50, 0, 255);
    set_rgba(&d[1].v[1], 200, 150, 0, 255);
    MotionTapFrame *fa = frame(&d[0], 1), *fb = frame(&d[1], 1);
    MotionBuild *b = builder();
    MotionBuiltVert out[2];
    MotionVert mv[2];
    CHECK(plan(b, fa, fb) == 1, "sprite: the frame can be built (why %#x)", (unsigned)motion_build_result(b)->why);
    CHECK(motion_build_result(b)->built == 2 && motion_build_result(b)->copied == 1, "sprite: two built, one copied");
    /* Halfway the corners are (10.75, 21.0) and (31.0, 40.5). */
    CHECK(motion_build_prim(b, 0, 0.5f, out, mv) == MOTION_ITEM_BUILT, "sprite: record 0 is built");
    CHECK(out[0].x == 10.0f && out[0].y == 21.0f && out[1].x == 31.0f && out[1].y == 40.0f,
          "sprite: floored corners (%g, %g) (%g, %g)", (double)out[0].x, (double)out[0].y, (double)out[1].x,
          (double)out[1].y);
    CHECK(out[0].z == 200.0f && out[1].z == 200.0f && out[0].rw == 1.0f && out[0].fog == 1.0f &&
          out[0].rgba == 0xFF006496u && out[1].rgba == 0xFF006496u && out[0].u == 0.0f && out[0].v == 0.0f,
          "sprite: depth and colour of the second corner (%g %08x)", (double)out[0].z, (unsigned)out[0].rgba);
    CHECK(mv[0].index == 0 && mv[1].index == 1 && mv[1].state == MOTION_V_MATCHED && mv[1].clip_a[2] == 100.0f &&
          mv[1].clip_b[2] == 300.0f, "sprite: the source records are the two corners");
    /* Halfway the corners are (99.5, 49.5) and (79.25, 29.25): low corner first all the same. */
    CHECK(motion_build_prim(b, 1, 0.5f, out, NULL) == MOTION_ITEM_BUILT && out[0].x == 79.0f && out[0].y == 29.0f &&
          out[1].x == 99.0f && out[1].y == 49.0f, "sprite: the other corner order (%g, %g) (%g, %g)",
          (double)out[0].x, (double)out[0].y, (double)out[1].x, (double)out[1].y);
    CHECK(motion_build_prim(b, 2, 0.5f, out, NULL) == MOTION_ITEM_COPY_B, "sprite: one that does not move is B's");
    CHECK(check_t1("sprite", b, fb) == 2, "sprite: two sprites are checked at t = 1");

    /* Corners that change sides between the frames: the backend's texture orientation would
     * turn round inside the in-between. */
    Draw flip = d[1];
    sprite_at(&flip, 0, 31.5f, 21.25f, 11.25f, 40.75f, 300.0f);
    MotionTapFrame *ff = frame(&flip, 1);
    CHECK(plan(b, fa, ff) == 0 && motion_build_result(b)->why == MOTION_WHY_NOT_PLAIN &&
          motion_build_result(b)->not_plain == 1, "sprite: corners that change sides stop the frame (%#x)",
          (unsigned)motion_build_result(b)->why);
    /* A moving sprite B's backend never drew, having no pixel: nothing of B to move. */
    Draw thin = d[1];
    sprite_at(&thin, 0, 11.25f, 21.25f, 11.75f, 40.75f, 300.0f);
    MotionTapFrame *ft = frame(&thin, 1);
    CHECK(plan(b, fa, ft) == 0 && motion_build_result(b)->why == MOTION_WHY_NOT_PLAIN,
          "sprite: one with no pixel in B stops the frame (%#x)", (unsigned)motion_build_result(b)->why);
    motion_tap_frame_free(ff);
    motion_tap_frame_free(ft);
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);

    /* Transform mode: both corners projected at t, then the same rule. The corners share one
     * w; corners at different depths can cross twice between the frames. */
    Draw x[2];
    x[0] = draw_any(6, 2, 6, 0);
    x[1] = x[0];
    set_clip(&x[0].v[0], -0.52f, 0.51f, 1.0f, 2.0f);
    set_clip(&x[0].v[1], -0.25f, 0.24f, 1.0f, 2.0f);
    set_clip(&x[1].v[0], -0.31f, 0.56f, 1.0f, 2.0f);
    set_clip(&x[1].v[1], -0.04f, 0.31f, 1.0f, 2.0f);
    fa = frame(&x[0], 1);
    fb = frame(&x[1], 1);
    CHECK(plan(b, fa, fb) == 1 && motion_build_prim(b, 0, 0.5f, out, NULL) == MOTION_ITEM_BUILT,
          "sprite: a transform-mode sprite is built (why %#x)", (unsigned)motion_build_result(b)->why);
    /* Halfway: clip (-0.415, 0.535) and (-0.145, 0.275) over w = 2: x 190.2 and 222.6, y 99.6 and 117.3. */
    CHECK(out[0].x == 190.0f && out[1].x == 222.0f && out[0].y == 99.0f && out[1].y == 117.0f && out[0].rw == 1.0f,
          "sprite: transform-mode corners (%g, %g) (%g, %g)", (double)out[0].x, (double)out[0].y, (double)out[1].x,
          (double)out[1].y);
    CHECK(check_t1("sprite", b, fb) == 1, "sprite: the transform-mode sprite is checked at t = 1");
    Draw deep = x[1];
    deep.v[1].clip[3] = 2.5f;
    MotionTapFrame *fd = frame(&deep, 1);
    CHECK(plan(b, fa, fd) == 0 && motion_build_result(b)->why == MOTION_WHY_NOT_PLAIN,
          "sprite: corners at different depths stop the frame (%#x)", (unsigned)motion_build_result(b)->why);
    motion_tap_frame_free(fd);
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);
    motion_build_free(b);
}

/* Fog is clamped after the mix: where one frame's factor is saturated, the factor between the
 * frames is not the mix of the two clamped ones. */
static void cell_fog(void) {
    Draw d[2];
    d[0] = draw_any(3, 3, 7, 0);
    d[0].p.flags |= GE_MOTION_PRIM_FOG;
    for (uint32_t i = 0; i < 3; i++) set_clip(&d[0].v[i], -0.2f + 0.2f * (float)i, 0.3f * (float)(i & 1u), 1.0f, 2.0f);
    d[1] = d[0];
    d[0].v[0].fog_arg = 1.5f;       /* saturated at 1 in A */
    d[0].v[1].fog_arg = -0.5f;      /* saturated at 0 in A */
    d[0].v[2].fog_arg = 2.0f;
    d[1].v[0].fog_arg = d[1].v[1].fog_arg = d[1].v[2].fog_arg = 0.5f;
    MotionTapFrame *fa = frame(&d[0], 1), *fb = frame(&d[1], 1);
    MotionBuild *b = builder();
    MotionBuiltVert out[3];
    CHECK(plan(b, fa, fb) == 1 && motion_build_result(b)->built == 1, "fog: a change of fog alone is built");
    CHECK(motion_build_prim(b, 0, 0.5f, out, NULL) == MOTION_ITEM_BUILT && out[0].fog == 1.0f && out[1].fog == 0.0f &&
          out[2].fog == 1.0f, "fog: halfway (%g %g %g), not the mix of the clamped factors (0.75 0.25 0.75)",
          (double)out[0].fog, (double)out[1].fog, (double)out[2].fog);
    CHECK(motion_build_prim(b, 0, 0.75f, out, NULL) == MOTION_ITEM_BUILT && out[0].fog == 0.75f &&
          out[1].fog == 0.25f && out[2].fog == 0.875f, "fog: at 0.75 (%g %g %g)", (double)out[0].fog,
          (double)out[1].fog, (double)out[2].fog);
    CHECK(check_t1("fog", b, fb) == 1, "fog: checked at t = 1");
    motion_tap_frame_free(fb);
    /* At t = 1 a value is B's own bits, -0 included: 1.5 (1 - 1) + -0 would be +0. */
    d[1].v[0].fog_arg = -0.0f;
    fb = frame(&d[1], 1);
    CHECK(plan(b, fa, fb) == 1 && motion_build_prim(b, 0, 1.0f, out, NULL) == MOTION_ITEM_BUILT &&
          same_bits(out[0].fog, -0.0f), "fog: -0 in B is -0 at t = 1 (%g)", (double)out[0].fog);
    CHECK(check_t1("fog -0", b, fb) == 1, "fog: -0 checked at t = 1");
    motion_build_free(b);
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);
}

/* Flat shading: every vertex takes the last vertex's colour, in the in-between as in B. */
static void cell_flat(void) {
    Draw d[2];
    d[0] = draw_any(3, 3, 8, 0);
    d[0].p.flags |= GE_MOTION_PRIM_FLAT;
    for (uint32_t i = 0; i < 3; i++) {
        set_clip(&d[0].v[i], -0.2f + 0.2f * (float)i, 0.3f * (float)(i & 1u), 1.0f, 2.0f);
        set_rgba(&d[0].v[i], 10 * (int)i, 0, 0, 255);
    }
    d[1] = d[0];
    d[1].v[2].clip[0] += 0.1f;
    set_rgba(&d[0].v[2], 0, 0, 300, 255);   /* a lit colour above 255 is clamped after the mix */
    set_rgba(&d[1].v[2], 0, 0, 100, -20);
    MotionTapFrame *fa = frame(&d[0], 1), *fb = frame(&d[1], 1);
    MotionBuild *b = builder();
    MotionBuiltVert out[3];
    CHECK(plan(b, fa, fb) == 1 && motion_build_prim(b, 0, 0.5f, out, NULL) == MOTION_ITEM_BUILT, "flat: built");
    CHECK(out[0].rgba == 0x76C80000u && out[1].rgba == out[0].rgba && out[2].rgba == out[0].rgba,
          "flat: the last vertex's colour on all three (%08x %08x %08x)", (unsigned)out[0].rgba,
          (unsigned)out[1].rgba, (unsigned)out[2].rgba);
    CHECK(motion_build_prim(b, 0, 0.125f, out, NULL) == MOTION_ITEM_BUILT && out[0].rgba == 0xDDFF0000u,
          "flat: 300 to 100 an eighth of the way is 275, clamped to 255, and 255 to -20 is 221 (%08x)",
          (unsigned)out[0].rgba);
    CHECK(check_t1("flat", b, fb) == 1, "flat: checked at t = 1");
    motion_tap_frame_free(fb);
    /* A channel of B above 255: B's backend stores 255, and so does t = 1. */
    set_rgba(&d[1].v[2], 0, 400, 100, -20);
    fb = frame(&d[1], 1);
    CHECK(plan(b, fa, fb) == 1 && motion_build_prim(b, 0, 1.0f, out, NULL) == MOTION_ITEM_BUILT &&
          out[0].rgba == 0x0064FF00u && out[2].rgba == 0x0064FF00u, "flat: green 400 in B is 255 at t = 1 (%08x)",
          (unsigned)out[0].rgba);
    CHECK(check_t1("flat above 255", b, fb) == 1, "flat: a channel above 255 checked at t = 1");
    motion_build_free(b);
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);
}

/* Two triangles of one draw: the first moves, the second does not. `dec_a` and `dec_b` are
 * ge.c's decisions for the second in each frame, and `shift` moves it in B. */
static void two_tris(Draw d[2], uint32_t serial, int dec_a, int dec_b, float shift) {
    d[0] = draw_any(3, 6, serial, 0);
    for (uint32_t i = 0; i < 6; i++)
        set_clip(&d[0].v[i], -0.6f + 0.2f * (float)i, 0.3f * (float)(i & 1u), 1.0f, 2.0f);
    d[1] = d[0];
    for (uint32_t i = 0; i < 3; i++) d[1].v[i].clip[0] += 0.05f;
    for (uint32_t i = 3; i < 6; i++) d[1].v[i].clip[0] += shift;
    d[0].dec[1] = (uint8_t)dec_a;
    d[1].dec[1] = (uint8_t)dec_b;
}

/* A triangle whose inputs are the same in both frames is B's own, whatever ge.c decided. */
static void cell_static(void) {
    Draw d[2];
    MotionBuild *b = builder();
    MotionBuiltVert out[3];
    MotionVert mv[3];
    two_tris(d, 9, GE_MOTION_DRAW, GE_MOTION_DRAW, 0.0f);
    MotionTapFrame *fa = frame(&d[0], 1), *fb = frame(&d[1], 1);
    CHECK(plan(b, fa, fb) == 1 && motion_build_result(b)->built == 1 && motion_build_result(b)->copied == 1,
          "static: one built, one copied");
    CHECK(motion_build_prim(b, 0, 0.5f, out, NULL) == MOTION_ITEM_BUILT &&
          motion_build_prim(b, 1, 0.5f, out, mv) == MOTION_ITEM_COPY_B && mv[0].state == MOTION_V_MATCHED &&
          memcmp(mv[0].clip_a, mv[0].clip_b, sizeof mv[0].clip_a) == 0, "static: the still triangle is B's");
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);
    /* Cut by the near plane in both frames, and still: its fan pieces are B's. */
    two_tris(d, 9, GE_MOTION_CLIP, GE_MOTION_CLIP, 0.0f);
    fa = frame(&d[0], 1);
    fb = frame(&d[1], 1);
    CHECK(plan(b, fa, fb) == 1 && motion_build_prim(b, 1, 0.5f, out, mv) == MOTION_ITEM_COPY_B &&
          mv[0].state == MOTION_V_CLIPPED, "static: a still near-clipped triangle is B's (why %#x)",
          (unsigned)motion_build_result(b)->why);
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);
    /* Dropped in both frames, and still: nothing to draw, no reason. */
    two_tris(d, 9, GE_MOTION_DROP, GE_MOTION_DROP, 0.0f);
    fa = frame(&d[0], 1);
    fb = frame(&d[1], 1);
    CHECK(plan(b, fa, fb) == 1 && motion_build_prim(b, 1, 0.5f, out, NULL) == MOTION_ITEM_NONE &&
          motion_build_prim(b, 0, 0.5f, out, NULL) == MOTION_ITEM_BUILT, "static: a still dropped triangle is nothing");
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);
    /* Still, with another viewport in B: B's own vertices are right at every t. */
    two_tris(d, 9, GE_MOTION_DRAW, GE_MOTION_DRAW, 0.0f);
    for (uint32_t i = 0; i < 3; i++) d[1].v[i] = d[0].v[i];
    d[1].p.viewport[3] += 8.0f;
    fa = frame(&d[0], 1);
    fb = frame(&d[1], 1);
    CHECK(plan(b, fa, fb) == 1 && motion_build_result(b)->copied == 2 && motion_build_result(b)->built == 0,
          "static: still triangles under a moved viewport are B's (why %#x)", (unsigned)motion_build_result(b)->why);
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);
    /* Still, with a vertex that is not a number, the same bits in both frames: ge.c dropped it
     * in both, and it did not move. A NaN with other bits in B moved, as far as can be told. */
    two_tris(d, 9, GE_MOTION_DROP, GE_MOTION_DROP, 0.0f);
    d[0].v[4].clip[1] = d[1].v[4].clip[1] = float_bits(0x7FC00001u);
    fa = frame(&d[0], 1);
    fb = frame(&d[1], 1);
    CHECK(plan(b, fa, fb) == 1 && motion_build_result(b)->not_plain == 0 && motion_build_result(b)->built == 1 &&
          motion_build_prim(b, 1, 0.5f, out, NULL) == MOTION_ITEM_NONE,
          "static: a still triangle with a NaN vertex is still (why %#x)", (unsigned)motion_build_result(b)->why);
    motion_tap_frame_free(fb);
    d[1].v[4].clip[1] = float_bits(0x7FC00002u);
    fb = frame(&d[1], 1);
    CHECK(plan(b, fa, fb) == 0 && motion_build_result(b)->why == MOTION_WHY_NOT_PLAIN &&
          motion_build_result(b)->dropped_both == 1, "static: another NaN in B is a move (why %#x)",
          (unsigned)motion_build_result(b)->why);
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);
    motion_build_free(b);
}

/* ---- the reasons that stop a frame ------------------------------------------------------------ */

/* No record of B gets values, at any t. */
static void expect_no_values(const char *cell, const MotionBuild *b, const MotionTapFrame *fb) {
    MotionTapView v;
    MotionBuiltVert out[3];
    MotionVert mv[3];
    motion_tap_frame_view(fb, &v);
    for (uint32_t tri = 0; tri < v.ntris; tri++)
        CHECK(motion_build_prim(b, tri, 0.5f, out, mv) == MOTION_ITEM_NONE &&
              motion_build_prim(b, tri, 1.0f, out, NULL) == MOTION_ITEM_NONE, "%s: record %u still gets values",
              cell, tri);
}

/* The frame stops with exactly `why` and gives no values. */
static void expect_stopped(const char *cell, MotionBuild *b, const MotionTapFrame *fa, const MotionTapFrame *fb,
                           uint32_t why) {
    const int ok = plan(b, fa, fb);
    const MotionBuildResult *r = motion_build_result(b);
    CHECK(ok == 0 && r->why == why, "%s: the frame stops with %#x (got %d, %#x)", cell, (unsigned)why, ok,
          (unsigned)r->why);
    CHECK(r->flags == (why == MOTION_WHY_CUT ? MOTION_F_CUT : MOTION_F_NOT_REPLAYABLE), "%s: flags %#x", cell,
          (unsigned)r->flags);
    expect_no_values(cell, b, fb);
}

static void cell_reasons(void) {
    Draw d[2];
    MotionBuild *b = builder();
    MotionTapFrame *fa, *fb;

    /* Drawn in B only, and moving: at t it may or may not be on screen. */
    two_tris(d, 10, GE_MOTION_DROP, GE_MOTION_DRAW, 0.05f);
    fa = frame(&d[0], 1);
    fb = frame(&d[1], 1);
    expect_stopped("drawn in B only", b, fa, fb, MOTION_WHY_NOT_PLAIN);
    CHECK(motion_build_result(b)->not_plain == 1 && motion_build_result(b)->dropped_both == 0 &&
          motion_build_result(b)->built == 1, "drawn in B only: counted once");
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);

    /* Drawn in A only. */
    two_tris(d, 10, GE_MOTION_DRAW, GE_MOTION_DROP, 0.05f);
    fa = frame(&d[0], 1);
    fb = frame(&d[1], 1);
    expect_stopped("drawn in A only", b, fa, fb, MOTION_WHY_NOT_PLAIN);
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);

    /* Dropped in both, and moving: between two drops it can still cross the screen. */
    two_tris(d, 10, GE_MOTION_DROP, GE_MOTION_DROP, 0.05f);
    fa = frame(&d[0], 1);
    fb = frame(&d[1], 1);
    expect_stopped("dropped in both", b, fa, fb, MOTION_WHY_NOT_PLAIN);
    CHECK(motion_build_result(b)->dropped_both == 1, "dropped in both: counted as such");
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);

    /* Cut by the near plane in B (then in A), and moving: its fan is not B's fan at t. */
    two_tris(d, 10, GE_MOTION_DRAW, GE_MOTION_CLIP, 0.05f);
    fa = frame(&d[0], 1);
    fb = frame(&d[1], 1);
    expect_stopped("near clip in B", b, fa, fb, MOTION_WHY_NEAR_CLIP);
    CHECK(motion_build_result(b)->near_clip == 1, "near clip in B: counted once");
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);
    two_tris(d, 10, GE_MOTION_CLIP, GE_MOTION_DRAW, 0.05f);
    fa = frame(&d[0], 1);
    fb = frame(&d[1], 1);
    expect_stopped("near clip in A", b, fa, fb, MOTION_WHY_NEAR_CLIP);
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);

    /* Moving under another viewport, offset or depth-clip switch in B. */
    for (int which = 0; which < 3; which++) {
        two_tris(d, 10, GE_MOTION_DRAW, GE_MOTION_DRAW, 0.0f);
        if (which == 0) d[1].p.viewport[0] = 200.0f;
        else if (which == 1) d[1].p.offset[1] += 1.0f;
        else d[1].p.flags |= GE_MOTION_PRIM_DEPTH_CLIP;
        fa = frame(&d[0], 1);
        fb = frame(&d[1], 1);
        expect_stopped("projection state", b, fa, fb, MOTION_WHY_STATE);
        CHECK(motion_build_result(b)->state == 1 && motion_build_result(b)->copied == 1,
              "projection state: the moving triangle only");
        motion_tap_frame_free(fa);
        motion_tap_frame_free(fb);
    }

    /* A vertex that is not a number in A: nothing to mix from. */
    two_tris(d, 10, GE_MOTION_DRAW, GE_MOTION_DRAW, 0.05f);
    d[0].v[4].clip[1] = (float)NAN;
    fa = frame(&d[0], 1);
    fb = frame(&d[1], 1);
    expect_stopped("non-finite", b, fa, fb, MOTION_WHY_NOT_PLAIN);
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);

    /* A line PRIM in B: the tap names nothing of what it drew. One with a single vertex draws
     * nothing and stops nothing; a line in A alone does not matter. */
    Draw with_line[2][2];
    two_tris(d, 10, GE_MOTION_DRAW, GE_MOTION_DRAW, 0.05f);
    for (int f = 0; f < 2; f++) {
        with_line[f][0] = d[f];
        with_line[f][1] = draw_any(1, 2, 11, 0);
    }
    fa = frame(with_line[0], 2);
    fb = frame(with_line[1], 2);
    expect_stopped("line", b, fa, fb, MOTION_WHY_UNKNOWN);
    CHECK(motion_build_result(b)->unknown == 2 && motion_build_result(b)->built == 2,
          "line: the PRIM and its one line counted (%u)", (unsigned)motion_build_result(b)->unknown);
    motion_tap_frame_free(fb);
    with_line[1][1].p.count = 1;
    fb = frame(with_line[1], 2);
    CHECK(plan(b, fa, fb) == 1, "line: a line PRIM of one vertex stops nothing (why %#x)",
          (unsigned)motion_build_result(b)->why);
    motion_tap_frame_free(fb);
    with_line[1][1] = draw_any(0, 1, 11, 0);
    fb = frame(with_line[1], 2);
    expect_stopped("point", b, fa, fb, MOTION_WHY_UNKNOWN);
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);

    /* A Bezier patch's triangles, then an immediate-mode rectangle, in B: raster calls the tap
     * names nothing for, so what they drew cannot be placed either. In A alone they do not
     * matter. */
    two_tris(d, 10, GE_MOTION_DRAW, GE_MOTION_DRAW, 0.05f);
    fa = frame(&d[0], 1);
    fb = frame_unnamed(&d[1], 1, 2, 0, 0, 0);
    MotionTapView vu;
    motion_tap_frame_view(fb, &vu);
    CHECK(vu.unnamed == 2 && vu.ntris == 2 && vu.prims[0].submitted == 2 && vu.flags == MOTION_TAP_CLOSED,
          "patch: the store counts its two raster calls apart from the PRIM's (%u)", (unsigned)vu.unnamed);
    expect_stopped("patch", b, fa, fb, MOTION_WHY_UNKNOWN);
    CHECK(motion_build_result(b)->unknown == 2 && motion_build_result(b)->built == 2,
          "patch: each of its triangles is counted (%u)", (unsigned)motion_build_result(b)->unknown);
    motion_tap_frame_free(fb);
    fb = frame_unnamed(&d[1], 1, 0, 1, 0, 0);
    expect_stopped("immediate mode", b, fa, fb, MOTION_WHY_UNKNOWN);
    CHECK(motion_build_result(b)->unknown == 1, "immediate mode: counted once (%u)",
          (unsigned)motion_build_result(b)->unknown);
    motion_tap_frame_free(fb);
    /* A patch drawn as lines (three per quad) or as points, an immediate-mode line or point:
     * no triangle or sprite raster call at all, each still counted. */
    static const struct { const char *what; int lines, points; } lp[4] = {
        { "patch as lines", 3, 0 }, { "patch as points", 0, 4 }, { "immediate-mode line", 1, 0 },
        { "immediate-mode point", 0, 1 },
    };
    for (int k = 0; k < 4; k++) {
        fb = frame_unnamed(&d[1], 1, 0, 0, lp[k].lines, lp[k].points);
        motion_tap_frame_view(fb, &vu);
        const uint32_t want = (uint32_t)(lp[k].lines + lp[k].points);
        CHECK(vu.unnamed == want && vu.prims[0].submitted == 2, "%s: the store counts %u (%u)", lp[k].what,
              (unsigned)want, (unsigned)vu.unnamed);
        expect_stopped(lp[k].what, b, fa, fb, MOTION_WHY_UNKNOWN);
        CHECK(motion_build_result(b)->unknown == want && motion_build_result(b)->built == 2, "%s: counted (%u)",
              lp[k].what, (unsigned)motion_build_result(b)->unknown);
        motion_tap_frame_free(fb);
    }
    motion_tap_frame_free(fa);
    fa = frame_unnamed(&d[0], 1, 2, 1, 1, 1);
    fb = frame(&d[1], 1);
    CHECK(plan(b, fa, fb) == 1, "patch: one in A alone stops nothing (why %#x)", (unsigned)motion_build_result(b)->why);
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);

    /* No frame A: an empty one, and none at all. */
    two_tris(d, 10, GE_MOTION_DRAW, GE_MOTION_DRAW, 0.05f);
    fa = frame(d, 0);
    fb = frame(&d[1], 1);
    expect_stopped("empty A", b, fa, fb, MOTION_WHY_NO_PREVIOUS);
    expect_stopped("no A", b, NULL, fb, MOTION_WHY_NO_PREVIOUS);
    motion_tap_frame_free(fa);

    /* The matcher failed, a frame is still open, or an argument is missing. */
    fa = frame(&d[0], 1);
    MotionMatchResult failed;
    memset(&failed, 0, sizeof failed);
    failed.flags = MOTION_MATCH_FAILED;
    g_pair[0] = -1;
    CHECK(motion_build_plan(b, fa, fb, g_pair, &failed) == 0 && motion_build_result(b)->why == MOTION_WHY_FAILED,
          "failed: the matcher's failure stops the frame (%#x)", (unsigned)motion_build_result(b)->why);
    MotionTapFrame *open = motion_tap_frame_create(0);
    emit(open, &d[1]);
    memset(&failed, 0, sizeof failed);
    CHECK(motion_build_plan(b, fa, open, g_pair, &failed) == 0 && motion_build_result(b)->why == MOTION_WHY_FAILED &&
          motion_build_plan(b, fa, fb, NULL, &failed) == 0 && motion_build_result(b)->why == MOTION_WHY_FAILED &&
          motion_build_plan(b, fa, fb, g_pair, NULL) == 0 && motion_build_result(b)->why == MOTION_WHY_FAILED &&
          motion_build_plan(b, fa, NULL, g_pair, &failed) == 0 && motion_build_result(b)->why == MOTION_WHY_FAILED,
          "failed: an open frame or a missing argument stops the frame");
    MotionBuiltVert out[3];
    CHECK(motion_build_prim(b, 0, 0.5f, out, NULL) == MOTION_ITEM_NONE && motion_build_check(b, 0, out) == 0,
          "failed: a failed plan gives nothing");
    g_pair[0] = 7;                  /* a pair that names no PRIM of A */
    CHECK(motion_build_plan(b, fa, fb, g_pair, &failed) == 0 && (motion_build_result(b)->why & MOTION_WHY_FAILED),
          "failed: a pair outside A stops the frame");
    motion_tap_frame_free(open);
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);
    motion_build_free(b);
}

/* The guard: values that are not what the builder gives at t = 1 stop the frame from then on. */
static void cell_guard(void) {
    Draw d[2];
    two_tris(d, 12, GE_MOTION_DRAW, GE_MOTION_DRAW, 0.05f);
    MotionTapFrame *fa = frame(&d[0], 1), *fb = frame(&d[1], 1);
    MotionBuild *b = builder();
    MotionTapView v;
    MotionBuiltVert sub[3], out[3];
    motion_tap_frame_view(fb, &v);
    static const char *const field[] = { "x", "y", "z", "rw", "u", "v", "fog", "rgba" };
    for (int which = 0; which < 8; which++) {
        CHECK(plan(b, fa, fb) == 1, "guard: the frame can be built");
        submitted_of(&v, 1, sub);
        CHECK(motion_build_check(b, 1, sub) == 1, "guard: B's values pass");
        float *f[] = { &sub[2].x, &sub[2].y, &sub[2].z, &sub[2].rw, &sub[2].u, &sub[2].v, &sub[2].fog };
        if (which < 7) *f[which] = nextafterf(*f[which], 1e30f);
        else sub[2].rgba ^= 0x00010000u;
        CHECK(motion_build_check(b, 1, sub) == 0, "guard: a changed %s passes", field[which]);
        const MotionBuildResult *r = motion_build_result(b);
        CHECK(r->why == MOTION_WHY_T1_MISMATCH && r->t1_mismatch == 1 && r->flags == MOTION_F_NOT_REPLAYABLE,
              "guard: the reason is set (%#x)", (unsigned)r->why);
        CHECK(motion_build_prim(b, 0, 0.5f, out, NULL) == MOTION_ITEM_NONE &&
              motion_build_prim(b, 1, 1.0f, out, NULL) == MOTION_ITEM_NONE, "guard: no values after a mismatch");
    }
    /* -0 is not +0: the comparison is of bits. */
    CHECK(plan(b, fa, fb) == 1, "guard: the frame can be built");
    submitted_of(&v, 0, sub);
    CHECK(sub[0].u == 0.0f, "guard: the scene has a zero to flip");
    sub[0].u = -0.0f;
    CHECK(motion_build_check(b, 0, sub) == 0, "guard: -0 for +0 passes");
    motion_build_free(b);
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);
}

/* ---- the coordinator's two cases: a draw nobody pairs, and a held frame ------------------------ */

static void three_draws(Draw d[2][3]) {
    for (uint32_t s = 0; s < 3; s++) {
        Draw pair[2];
        two_tris(pair, 20 + s, GE_MOTION_DRAW, GE_MOTION_DRAW, 0.03f);
        for (int f = 0; f < 2; f++) {
            for (uint32_t i = 0; i < 6; i++) pair[f].v[i].clip[1] += 0.5f * (float)s - 0.6f;
            d[f][s] = pair[f];
        }
    }
}

/* A draw that is in A and in B and that the matcher cannot pair is drawn as in B and counted;
 * the draws around it get the values they get without it. */
static void cell_unpaired(void) {
    Draw d[2][3], changed[3];
    three_draws(d);
    MotionTapFrame *fa = frame(d[0], 3), *fb = frame(d[1], 3);
    MotionBuild *b = builder();
    MotionBuiltVert ref[6][3], out[3];
    MotionVert mv[3];
    CHECK(plan(b, fa, fb) == 1 && motion_build_result(b)->paired == 3 && motion_build_result(b)->built == 6,
          "unpaired: the control frame pairs all three draws");
    for (uint32_t tri = 0; tri < 6; tri++)
        CHECK(motion_build_prim(b, tri, 0.5f, ref[tri], NULL) == MOTION_ITEM_BUILT, "unpaired: control record %u", tri);

    /* In B the middle draw's vertices lie inside the list (an inline key) and its texture
     * address differs: no PRIM of A has its signature. */
    memcpy(changed, d[1], sizeof changed);
    changed[1].p.vbase = LIST + 0x40u;
    changed[1].p.tex_addr = 0x04154000u;
    MotionTapFrame *fc = frame(changed, 3);
    MotionMatchFrame mc;
    motion_tap_match_frame(fc, &mc);
    CHECK(mc.nprims == 3 && (mc.prims[1].flags & MOTION_PRIM_INLINE) && !(mc.prims[0].flags & MOTION_PRIM_INLINE),
          "unpaired: the changed draw is an inline key");
    CHECK(plan(b, fa, fc) == 1, "unpaired: the frame is still built (why %#x)", (unsigned)motion_build_result(b)->why);
    CHECK(g_pair[0] == 0 && g_pair[1] == -1 && g_pair[2] == 2 && !(g_match.flags & MOTION_MATCH_CUT),
          "unpaired: the matcher pairs the two others and nothing with the changed one (%d %d %d, flags %#x)",
          (int)g_pair[0], (int)g_pair[1], (int)g_pair[2], (unsigned)g_match.flags);
    const MotionBuildResult *r = motion_build_result(b);
    uint32_t n = 0;
    const uint32_t *list = motion_build_unpaired(b, &n);
    CHECK(r->why == 0 && r->flags == 0 && r->draws == 3 && r->paired == 2 && r->unpaired == 1 &&
          r->unpaired_index == 0 && r->unpaired_shape == 0 && r->built == 4 && r->copied == 2,
          "unpaired: counted once (%u paired, %u unpaired, %u built, %u copied)", (unsigned)r->paired,
          (unsigned)r->unpaired, (unsigned)r->built, (unsigned)r->copied);
    CHECK(n == 1 && list && list[0] == 1, "unpaired: its serial is listed (%u)", (unsigned)n);
    for (uint32_t tri = 0; tri < 6; tri++) {
        const int kind = motion_build_prim(b, tri, 0.5f, out, mv);
        if (tri == 2 || tri == 3) {
            CHECK(kind == MOTION_ITEM_COPY_B && mv[0].state == MOTION_V_UNMATCHED && mv[0].prim == 1,
                  "unpaired: record %u of the changed draw is B's (%d)", tri, kind);
        } else {
            CHECK(kind == MOTION_ITEM_BUILT && same_vert(&out[0], &ref[tri][0], 1) &&
                  same_vert(&out[1], &ref[tri][1], 1) && same_vert(&out[2], &ref[tri][2], 1),
                  "unpaired: record %u of another draw changed", tri);
        }
    }
    CHECK(check_t1("unpaired", b, fc) == 4, "unpaired: the paired draws are checked at t = 1");
    motion_build_free(b);
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);
    motion_tap_frame_free(fc);
}

/* Under half of B's transform-mode vertices matched: the matcher cuts, the frame is held, and
 * no record gets values, the paired draw's included. */
static void cell_held(void) {
    Draw d[2][3];
    three_draws(d);
    d[1][0].p.tex_addr = 0x04154000u;
    d[1][2].p.tex_addr = 0x04158000u;
    MotionTapFrame *fa = frame(d[0], 3), *fb = frame(d[1], 3);
    MotionBuild *b = builder();
    expect_stopped("held", b, fa, fb, MOTION_WHY_CUT);
    const MotionBuildResult *r = motion_build_result(b);
    CHECK((g_match.flags & MOTION_MATCH_CUT) == MOTION_MATCH_CUT_COVERAGE && r->match_flags == g_match.flags &&
          g_match.xform_verts == 18 && g_match.xform_verts_matched == 6,
          "held: the matcher's coverage rule cut it (flags %#x, %u of %u vertices)", (unsigned)g_match.flags,
          (unsigned)g_match.xform_verts_matched, (unsigned)g_match.xform_verts);
    CHECK(r->paired == 1 && r->unpaired == 2, "held: the counts are still reported (%u, %u)", (unsigned)r->paired,
          (unsigned)r->unpaired);
    /* One more matched draw is at the threshold, and the frame is built again. */
    d[1][2].p.tex_addr = 0;
    MotionTapFrame *fc = frame(d[1], 3);
    CHECK(plan(b, fa, fc) == 1 && motion_build_result(b)->unpaired == 1, "held: two of three matched is built");
    motion_build_free(b);
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);
    motion_tap_frame_free(fc);
}

/* ---- the store -------------------------------------------------------------------------------- */

/* What is current: a published triangle, fan piece or sprite during its raster call, and
 * nothing for whatever else reaches the rasterizer. */
static void cell_current(void) {
    MotionTapFrame *f = motion_tap_frame_create(0);
    const GeMotionTap *tap = motion_tap_frame_listener(f);
    MotionTapCurrent cur;
    MotionTapView v;
    Draw d = draw_any(3, 6, 30, 0);
    GeMotionVtx vx[3];
    GeVtx g[3];
    memset(g, 0, sizeof g);
    for (int c = 0; c < 3; c++) {
        vx[c] = d.v[c];
        g[c].x = 10.0f + 20.0f * (float)c;
        g[c].y = 30.0f - 5.0f * (float)c;
        g[c].rw = 1.0f;
    }
    const uint16_t p012[3] = { 0, 1, 2 }, p345[3] = { 3, 4, 5 }, p01[2] = { 0, 1 };
    CHECK(!motion_tap_current(f, &cur), "current: nothing before the first PRIM");
    tap->rastered(tap->user, &g[0], &g[1], &g[2]);          /* a patch or an immediate-mode triangle */
    tap->prim(tap->user, &d.p);
    CHECK(!motion_tap_current(f, &cur), "current: nothing at a PRIM's start");
    tap->tri(tap->user, p012, vx, GE_MOTION_DRAW);
    CHECK(motion_tap_current(f, &cur) && cur.prim == 0 && cur.tri == 0 && cur.kind == MOTION_CUR_TRI &&
          cur.piece == 0, "current: the triangle about to be drawn");
    tap->rastered(tap->user, &g[0], &g[1], &g[2]);
    CHECK(!motion_tap_current(f, &cur), "current: nothing once its raster call returned");
    tap->rastered(tap->user, &g[0], &g[1], &g[2]);          /* a wing copy, a patch: not this PRIM's */
    tap->tri(tap->user, p345, vx, GE_MOTION_CLIP);
    CHECK(!motion_tap_current(f, &cur), "current: a near-clipped triangle is not drawn whole");
    for (int piece = 0; piece < 2; piece++) {
        tap->fan(tap->user, piece);
        CHECK(motion_tap_current(f, &cur) && cur.tri == 1 && cur.kind == MOTION_CUR_FAN && cur.piece == piece,
              "current: fan piece %d", piece);
        tap->rastered(tap->user, &g[0], &g[1], &g[2]);
        CHECK(!motion_tap_current(f, &cur), "current: nothing after fan piece %d", piece);
    }
    tap->tri(tap->user, p012, vx, GE_MOTION_DROP);
    CHECK(!motion_tap_current(f, &cur), "current: a dropped triangle is never current");
    g[1].x = (float)NAN;
    tap->tri(tap->user, p012, vx, GE_MOTION_DRAW);
    tap->rastered(tap->user, &g[0], &g[1], &g[2]);          /* the rasterizer drops a non-finite vertex */
    g[1].x = 30.0f;
    motion_tap_frame_view(f, &v);
    CHECK(v.nprims == 1 && v.ntris == 4 && v.prims[0].ntris == 4 && v.prims[0].submitted == 3,
          "current: one whole triangle and two fan pieces were handed over (%u)", (unsigned)v.prims[0].submitted);
    CHECK(v.unnamed == 2 && v.flags == 0 && v.dropped == 0,
          "current: the two raster calls nothing was published for are counted (%u)", (unsigned)v.unnamed);
    CHECK(v.tris[1].decision == GE_MOTION_CLIP && v.tris[1].pieces == 3 && v.tris[0].pieces == 0,
          "current: the fan pieces are noted on their triangle (%u)", (unsigned)v.tris[1].pieces);
    CHECK(v.prims[0].bbox[0] == 10.0f && v.prims[0].bbox[1] == 20.0f && v.prims[0].bbox[2] == 50.0f &&
          v.prims[0].bbox[3] == 30.0f, "current: the box is of the vertices handed over");
    /* A line PRIM: its start ends whatever was published, and nothing of it is published. */
    Draw line = draw_any(2, 4, 31, 0), spr = draw_any(6, 2, 32, 1);
    tap->tri(tap->user, p012, vx, GE_MOTION_DRAW);
    tap->prim(tap->user, &line.p);
    CHECK(!motion_tap_current(f, &cur), "current: a line PRIM has no current primitive");
    tap->prim(tap->user, &spr.p);
    tap->sprite(tap->user, p01, vx, 2, GE_MOTION_DRAW);
    CHECK(motion_tap_current(f, &cur) && cur.prim == 2 && cur.kind == MOTION_CUR_SPRITE && cur.tri == 5,
          "current: the sprite about to be drawn");
    tap->rastered(tap->user, &g[0], &g[1], NULL);
    CHECK(!motion_tap_current(f, &cur), "current: nothing after the sprite");
    tap->sprite(tap->user, p01, vx, 1, GE_MOTION_DROP);
    CHECK(!motion_tap_current(f, &cur), "current: a dropped sprite is never current");
    motion_tap_frame_view(f, &v);
    CHECK(v.nprims == 3 && v.prims[1].ntris == 0 && v.prims[1].submitted == 0 && v.prims[2].ntris == 2 &&
          v.tris[6].pos[2] == 1 && v.tris[5].pos[2] == 2, "current: the line PRIM holds no record");
    /* References are written once, by the first record that names the position. */
    CHECK(v.ref_set[0] && v.ref_set[5] && v.nrefs == 12 && !v.ref_set[6] && !v.ref_set[9] && v.ref_set[10] &&
          v.ref_set[11], "current: references are set where a record named them");
    motion_tap_frame_close(f, NULL);
    tap->prim(tap->user, &d.p);
    tap->tri(tap->user, p012, vx, GE_MOTION_DRAW);
    tap->fan(tap->user, 0);
    tap->sprite(tap->user, p01, vx, 2, GE_MOTION_DRAW);
    tap->rastered(tap->user, &g[0], &g[1], &g[2]);
    motion_tap_frame_view(f, &v);
    CHECK(v.nprims == 3 && v.late == 5 && v.unnamed == 2 && !motion_tap_current(f, &cur) &&
          v.flags == MOTION_TAP_CLOSED, "current: a closed frame records nothing and counts the calls (%u late)",
          (unsigned)v.late);
    motion_tap_frame_reset(f);
    motion_tap_frame_view(f, &v);
    CHECK(v.nprims == 0 && v.flags == 0 && v.late == 0 && v.unnamed == 0, "current: a reset frame is empty and open");
    /* A fan piece with no near-clipped triangle before it names nothing and breaks the contract. */
    tap->prim(tap->user, &d.p);
    tap->tri(tap->user, p012, vx, GE_MOTION_DROP);
    tap->fan(tap->user, 0);
    CHECK(!motion_tap_current(f, &cur), "current: a fan piece with no clipped triangle before it is nothing");
    motion_tap_frame_view(f, &v);
    CHECK(v.flags == MOTION_TAP_BAD_CALL && v.dropped == 1 && v.ntris == 1,
          "current: a fan piece with no clipped triangle is a bad call (%#x)", (unsigned)v.flags);
    motion_tap_frame_free(f);
}

/* The matcher's list: the PRIMs that handed something over, their boxes, inline marks and the
 * frame's camera. */
static void cell_bridge(void) {
    Draw d[5];
    two_tris(d, 40, GE_MOTION_DROP, GE_MOTION_DROP, 0.0f);      /* d[0]: its second triangle is dropped */
    d[1] = draw_any(3, 3, 41, 0);                               /* every triangle dropped: not in the list */
    for (uint32_t i = 0; i < 3; i++) set_clip(&d[1].v[i], 0.1f * (float)i, 0.2f, 1.0f, 2.0f);
    d[1].dec[0] = GE_MOTION_DROP;
    d[2] = draw_any(6, 2, 42, 1);                               /* a through-mode sprite */
    sprite_at(&d[2], 0, 100.0f, 50.0f, 164.0f, 82.0f, 0.0f);
    d[2].p.tex_addr = 0x04100000u;
    d[2].p.clut_addr = 0x08a00000u;
    d[2].p.state_class = GE_MOTION_CLASS_BLEND;
    d[3] = draw_any(3, 3, 43, 0);                               /* vertices inside the list: inline */
    for (uint32_t i = 0; i < 3; i++) set_clip(&d[3].v[i], -0.5f + 0.1f * (float)i, -0.4f * (float)(i & 1u), 1.0f, 2.0f);
    d[3].p.vbase = LIST + 0x20u;
    d[4] = draw_any(1, 2, 44, 0);                               /* a line: not in the list */
    static float other_proj[16];
    memcpy(other_proj, k_proj, sizeof other_proj);
    other_proj[0] = 2.0f;
    d[3].p.proj = other_proj;                                   /* three vertices use it, six the first */
    MotionTapFrame *f = frame(d, 5);
    MotionMatchFrame m;
    MotionTapView v;
    motion_tap_match_frame(f, &m);
    motion_tap_frame_view(f, &v);
    CHECK(m.nprims == 3 && motion_tap_match_serial(f, 0) == 0 && motion_tap_match_serial(f, 1) == 2 &&
          motion_tap_match_serial(f, 2) == 3 && motion_tap_match_serial(f, 3) == UINT32_MAX,
          "bridge: the list holds the PRIMs that drew (%u)", (unsigned)m.nprims);
    CHECK(v.prims[0].match_index == 0 && v.prims[1].match_index == -1 && v.prims[2].match_index == 1 &&
          v.prims[3].match_index == 2 && v.prims[4].match_index == -1, "bridge: each PRIM knows its place in it");
    const MotionPrim *q = &m.prims[1];
    CHECK(q->through == 1 && q->prim_type == 6 && q->count == 2 && q->vtype == d[2].p.vtype &&
          q->tex_addr == 0x04100000u && q->clut_addr == 0x08a00000u && q->state_class == GE_MOTION_CLASS_BLEND &&
          q->vaddr == d[2].p.vbase && q->iaddr == 0 && q->flags == 0, "bridge: the signature and key fields");
    CHECK(q->bbox[0] == 100.0f && q->bbox[1] == 50.0f && q->bbox[2] == 164.0f && q->bbox[3] == 82.0f,
          "bridge: a sprite's box (%g %g %g %g)", (double)q->bbox[0], (double)q->bbox[1], (double)q->bbox[2],
          (double)q->bbox[3]);
    /* d[0] draws positions 0..2 only: clip x -0.6, -0.4, -0.2 over w = 2 are x 168, 192, 216. */
    CHECK(fabsf(m.prims[0].bbox[0] - 168.0f) < 0.01f && fabsf(m.prims[0].bbox[2] - 216.0f) < 0.01f &&
          m.prims[0].bbox[1] < m.prims[0].bbox[3],
          "bridge: a box is of the triangle drawn, not the dropped one (%g %g)", (double)m.prims[0].bbox[0],
          (double)m.prims[0].bbox[2]);
    CHECK((m.prims[2].flags & MOTION_PRIM_INLINE) && !(m.prims[0].flags & MOTION_PRIM_INLINE),
          "bridge: vertices inside the list are inline");
    CHECK(m.camera.valid == 1 && memcmp(m.camera.proj, k_proj, sizeof k_proj) == 0 &&
          memcmp(m.camera.view, k_view, sizeof k_view) == 0 && v.ncameras == 2 && v.cameras[0].verts == 11 &&
          v.cameras[1].verts == 3 && v.prims[3].camera == 1 && v.prims[2].camera == MOTION_TAP_NO_CAMERA,
          "bridge: the camera most vertices used (%u cameras)", (unsigned)v.ncameras);
    /* The frame after it: an address inside a list the frame before ran is inline too. */
    Draw next = d[0];
    next.p.list_addr = 0x08180000u;
    next.p.cmd_addr = 0x08180040u;
    next.p.vbase = LIST + 0x30u;
    MotionTapFrame *g = frame_with_cap(&next, 1, f, 0), *alone = frame(&next, 1);
    MotionMatchFrame mg, ma;
    motion_tap_match_frame(g, &mg);
    motion_tap_match_frame(alone, &ma);
    CHECK(mg.nprims == 1 && (mg.prims[0].flags & MOTION_PRIM_INLINE) && !(ma.prims[0].flags & MOTION_PRIM_INLINE),
          "bridge: the previous frame's lists count");
    /* Through mode only: no camera. An open frame: no list. */
    MotionTapFrame *t = frame(&d[2], 1), *open = motion_tap_frame_create(0);
    MotionMatchFrame mt, mo;
    emit(open, &d[0]);
    motion_tap_match_frame(t, &mt);
    motion_tap_match_frame(open, &mo);
    CHECK(mt.nprims == 1 && mt.camera.valid == 0 && mo.nprims == 0 && mo.prims == NULL,
          "bridge: no camera without a transform-mode PRIM, no list while open");
    motion_tap_frame_free(f);
    motion_tap_frame_free(g);
    motion_tap_frame_free(alone);
    motion_tap_frame_free(t);
    motion_tap_frame_free(open);
}

/* A record that does not fit makes the frame incomplete: it is counted, nothing is current,
 * what was kept stays readable, and no in-between is built from it. */
static void cell_overflow(void) {
    Draw d[2];
    two_tris(d, 50, GE_MOTION_DRAW, GE_MOTION_DRAW, 0.05f);
    MotionTapFrame *fa = frame(&d[0], 1), *fb = frame(&d[1], 1);
    MotionBuild *b = builder();
    CHECK(plan(b, fa, fb) == 1, "overflow: the frames build under the default cap");

    /* The same PRIM again and again under a small cap. */
    MotionTapFrame *small = motion_tap_frame_create(16384);
    MotionTapCurrent cur;
    MotionTapView v;
    for (uint32_t i = 0; i < 400; i++) emit(small, &d[1]);
    motion_tap_frame_view(small, &v);
    CHECK((v.flags & MOTION_TAP_OVERFLOW) && v.nprims > 0 && v.nprims < 400 && v.dropped > 0 && v.bytes <= 16384,
          "overflow: the cap stops the store (%u PRIMs, %u dropped, %u bytes)", (unsigned)v.nprims,
          (unsigned)v.dropped, (unsigned)v.bytes);
    CHECK(!motion_tap_current(small, &cur), "overflow: nothing is current in an incomplete frame");
    const uint32_t kept = v.nprims, dropped = v.dropped;
    emit(small, &d[1]);
    motion_tap_frame_view(small, &v);
    CHECK(v.nprims == kept && v.dropped > dropped && v.prims[0].count == 6 && v.prims[0].ntris == 2,
          "overflow: later calls are counted, earlier records stay");
    motion_tap_frame_close(small, NULL);
    CHECK(plan(b, fa, small) == 0 && (motion_build_result(b)->why & MOTION_WHY_OVERFLOW) &&
          (motion_build_result(b)->flags & MOTION_F_NOT_REPLAYABLE) && motion_build_result(b)->built == 0,
          "overflow: an incomplete B stops the frame (%#x)", (unsigned)motion_build_result(b)->why);
    expect_no_values("overflow in B", b, small);
    MotionMatchFrame ms, mb;
    MotionMatchParams p;
    motion_tap_match_frame(small, &ms);
    motion_tap_match_frame(fb, &mb);
    motion_match_default_params(&p);
    motion_match(&ms, &mb, &p, NULL, g_pair, NULL, &g_match);
    CHECK(motion_build_plan(b, small, fb, g_pair, &g_match) == 0 &&
          (motion_build_result(b)->why & MOTION_WHY_OVERFLOW), "overflow: an incomplete A stops the frame too");
    motion_tap_frame_reset(small);
    emit(small, &d[1]);
    motion_tap_frame_close(small, NULL);
    CHECK(plan(b, fa, small) == 1, "overflow: a reset frame is whole again (why %#x)",
          (unsigned)motion_build_result(b)->why);
    motion_tap_frame_free(small);

    /* More distinct cameras than the table holds. */
    static float proj[MOTION_TAP_MAX_CAMERAS + 1u][16];
    MotionTapFrame *cams = motion_tap_frame_create(0);
    for (uint32_t i = 0; i <= MOTION_TAP_MAX_CAMERAS; i++) {
        Draw c = d[1];
        memcpy(proj[i], k_proj, sizeof proj[i]);
        proj[i][0] = 1.0f + (float)i;
        c.p.proj = proj[i];
        motion_tap_frame_view(cams, &v);
        CHECK(!(v.flags & MOTION_TAP_OVERFLOW), "overflow: %u cameras fit", (unsigned)i);
        emit(cams, &c);
    }
    motion_tap_frame_view(cams, &v);
    CHECK((v.flags & MOTION_TAP_OVERFLOW) && v.ncameras == MOTION_TAP_MAX_CAMERAS && v.nprims == MOTION_TAP_MAX_CAMERAS,
          "overflow: one camera too many (%u)", (unsigned)v.ncameras);
    motion_tap_frame_free(cams);

    /* More lists than the table holds. */
    MotionTapFrame *lists = motion_tap_frame_create(0);
    for (uint32_t i = 0; i <= MOTION_TAP_MAX_LISTS; i++) {
        Draw c = d[1];
        c.p.list_addr = 0x08400000u + i * 0x100u;
        c.p.cmd_addr = c.p.list_addr + 8u;
        emit(lists, &c);
    }
    motion_tap_frame_view(lists, &v);
    CHECK((v.flags & MOTION_TAP_OVERFLOW) && v.nprims == MOTION_TAP_MAX_LISTS, "overflow: one list too many (%u)",
          (unsigned)v.nprims);
    motion_tap_frame_free(lists);

    /* Calls that break the tap's contract are not records, and not an overflow either: a
     * position outside the PRIM, a record with no PRIM open, a transform-mode PRIM with no
     * camera, a fan piece out of range. Each frame is incomplete and refused as such. */
    for (int which = 0; which < 4; which++) {
        static const char *const what[] = { "a position outside the PRIM", "a record with no PRIM open",
                                            "a transform-mode PRIM with no camera", "fan piece 2" };
        MotionTapFrame *bad = motion_tap_frame_create(0);
        const GeMotionTap *tap = motion_tap_frame_listener(bad);
        const uint16_t outside[3] = { 0, 1, 6 }, inside[3] = { 0, 1, 2 };
        GeMotionPrim p = d[1].p;
        if (which == 2) p.proj = NULL;
        if (which != 1) tap->prim(tap->user, &p);
        if (which < 2) tap->tri(tap->user, which ? inside : outside, d[1].v, GE_MOTION_DRAW);
        if (which == 3) {
            tap->tri(tap->user, inside, d[1].v, GE_MOTION_CLIP);
            tap->fan(tap->user, 2);
        }
        tap->tri(tap->user, inside, d[1].v, GE_MOTION_DRAW);        /* recorded no more */
        motion_tap_frame_view(bad, &v);
        CHECK(v.flags == MOTION_TAP_BAD_CALL && v.ntris == (which == 3 ? 1u : 0u) && v.dropped == 2 &&
                  !motion_tap_current(bad, &cur),
              "bad call: %s makes the frame incomplete (flags %#x, %u records, %u dropped)", what[which],
              (unsigned)v.flags, (unsigned)v.ntris, (unsigned)v.dropped);
        motion_tap_frame_close(bad, NULL);
        const int ok = plan(b, fa, bad);
        const MotionBuildResult *r = motion_build_result(b);
        CHECK(ok == 0 && (r->why & MOTION_WHY_BAD_CALL) && !(r->why & MOTION_WHY_OVERFLOW) && r->built == 0 &&
                  (r->flags & MOTION_F_NOT_REPLAYABLE),
              "bad call: %s stops the frame as a bad call (why %#x)", what[which], (unsigned)r->why);
        expect_no_values("bad call", b, bad);
        motion_tap_frame_free(bad);
    }
    motion_build_free(b);
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);
}

int main(void) {
    cell_quad();
    cell_strip();
    cell_morph();
    cell_tie();
    cell_index_gate();
    cell_sprite();
    cell_fog();
    cell_flat();
    cell_static();
    cell_reasons();
    cell_guard();
    cell_unpaired();
    cell_held();
    cell_current();
    cell_bridge();
    cell_overflow();
    CHECK(s_exact >= 180, "positions: only %u were compared exactly with the reference", (unsigned)s_exact);
    if (s_fail) {
        fprintf(stderr, "motion_tap_selftest: %d failure(s)\n", s_fail);
        return 1;
    }
    printf("motion_tap_selftest: PASS (%u positions exact against the double-precision reference)\n",
           (unsigned)s_exact);
    return 0;
}
