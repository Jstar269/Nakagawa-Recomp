// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Restates src/rt/ge.c's project_cvtx and ge_gpu.c's put_vert (the 28.4 snap, the fog clamp, the
// colour pack and the sprite rectangle) in snap16, clamp01, channel, mix_rgba, place, build and
// motion_backend_vertices; the rest of this file was written for it.
// Modified by Nakagawa Recomp contributors, 2026-11-09.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/*
 * motion_tap.c - the frame store and the in-between builder of render-state motion (#2081).
 * The rules are in motion_tap.h. Pure C: the C library only.
 *
 * The projection, the snap, the fog clamp and the colour pack below repeat ge.c's project_cvtx
 * and the backend's put_vert expression for expression. Nothing ties the copies together at
 * build time; motion_build_check does at run time, on every frame, and the tests do: the ge.c
 * half against the production ge.o, motion_backend_vertices against motion_tap_selftest's own
 * restatement of put_vert.
 */
#include "motion_tap.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The layouts the plan and the FSR 3 interface fix. */
typedef char motion_vert_is_40_bytes[sizeof(MotionVert) == 40 ? 1 : -1];
typedef char motion_tap_tri_is_8_bytes[sizeof(MotionTapTri) == 8 ? 1 : -1];
typedef char motion_ref_is_44_bytes[sizeof(GeMotionVtx) == 44 ? 1 : -1];

#define NO_TRI 0xFFFFFFFFu

/* The command addresses one list reached: [addr, hi]. */
typedef struct {
    uint32_t addr, hi;
} ListExtent;

struct MotionTapFrame {
    GeMotionTap tap;                    /* the listener; its user is this frame */
    size_t cap_bytes, bytes;
    uint32_t flags, dropped, late, unnamed;

    MotionTapPrim *prims;
    uint32_t nprims, prims_cap;
    GeMotionVtx *refs;
    uint32_t nrefs, refs_cap;
    uint8_t *ref_set;
    uint32_t set_cap;
    MotionTapTri *tris;
    uint32_t ntris, tris_cap;
    MotionTapCamera *cams;
    uint32_t ncams, cams_cap, cam_last;
    ListExtent *lists;
    uint32_t nlists, lists_cap, list_last;

    /* The matcher's list, built at close; room for every PRIM is kept as PRIMs arrive, so the
     * close cannot fail. */
    MotionPrim *mprims;
    uint32_t mprims_cap, nmprims;
    uint32_t *mserial;
    uint32_t mserial_cap;
    MotionCamera camera;

    int32_t open_prim;                  /* the PRIM being filled, -1 when none */
    uint32_t clip_tri;                  /* the near-clipped triangle whose pieces follow, or NO_TRI */
    int have_cur;
    MotionTapCurrent cur;
};

/* ---- the store ------------------------------------------------------------------------------ */

/* The array `p` with room for `need` elements of `elem` bytes, inside the frame's cap. When it
 * cannot grow it is returned as it was and *ok is cleared. */
static void *room_for(MotionTapFrame *f, void *p, uint32_t *cap, uint64_t need, size_t elem, int *ok) {
    if (need <= *cap) return p;
    uint64_t ncap = *cap ? (uint64_t)*cap * 2u : 64u;
    if (ncap < need) ncap = need;
    if (ncap > UINT32_MAX) ncap = UINT32_MAX;
    const uint64_t room = (uint64_t)(f->cap_bytes - f->bytes) / elem;
    if (ncap - *cap > room) ncap = *cap + room;
    void *q = ncap < need ? NULL : realloc(p, (size_t)ncap * elem);
    if (!q) {
        *ok = 0;
        return p;
    }
    f->bytes += (size_t)(ncap - *cap) * elem;
    *cap = (uint32_t)ncap;
    return q;
}

/* A record was not kept: `why` is MOTION_TAP_OVERFLOW when it did not fit, MOTION_TAP_BAD_CALL
 * when the call broke the tap's contract. What the frame holds stays readable, but it is not
 * the whole frame, and nothing more is recorded into it. */
static void stop(MotionTapFrame *f, uint32_t why) {
    f->flags |= why;
    f->dropped++;
    f->open_prim = -1;
    f->clip_tri = NO_TRI;
    f->have_cur = 0;
}

static void overflow(MotionTapFrame *f) {
    stop(f, MOTION_TAP_OVERFLOW);
}

static void bad_call(MotionTapFrame *f) {
    stop(f, MOTION_TAP_BAD_CALL);
}

/* 1 when the callback may record; counts it otherwise. */
static int recording(MotionTapFrame *f) {
    if (f->flags & MOTION_TAP_CLOSED) {
        f->late++;
        return 0;
    }
    if (f->flags & (MOTION_TAP_OVERFLOW | MOTION_TAP_BAD_CALL)) {
        f->dropped++;
        return 0;
    }
    return 1;
}

static uint64_t fnv1a64(const uint8_t *p, uint32_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

static int camera_is(const MotionTapCamera *c, const float *view, const float *proj) {
    return memcmp(c->view, view, sizeof c->view) == 0 && memcmp(c->proj, proj, sizeof c->proj) == 0;
}

/* The index of (view, proj) in the frame's table, adding it when it is not there. */
static int camera_index(MotionTapFrame *f, const float *view, const float *proj, uint32_t *out) {
    if (f->ncams && camera_is(&f->cams[f->cam_last], view, proj)) {
        *out = f->cam_last;
        return 1;
    }
    for (uint32_t i = 0; i < f->ncams; i++)
        if (camera_is(&f->cams[i], view, proj)) {
            f->cam_last = *out = i;
            return 1;
        }
    int ok = f->ncams < MOTION_TAP_MAX_CAMERAS;
    if (ok) f->cams = room_for(f, f->cams, &f->cams_cap, (uint64_t)f->ncams + 1u, sizeof *f->cams, &ok);
    if (!ok) return 0;
    MotionTapCamera *c = &f->cams[f->ncams];
    memcpy(c->view, view, sizeof c->view);
    memcpy(c->proj, proj, sizeof c->proj);
    c->verts = 0;
    f->cam_last = *out = f->ncams++;
    return 1;
}

static int list_note(MotionTapFrame *f, uint32_t addr, uint32_t cmd) {
    ListExtent *l = NULL;
    if (f->nlists && f->lists[f->list_last].addr == addr) l = &f->lists[f->list_last];
    for (uint32_t i = 0; !l && i < f->nlists; i++)
        if (f->lists[i].addr == addr) {
            f->list_last = i;
            l = &f->lists[i];
        }
    if (!l) {
        int ok = f->nlists < MOTION_TAP_MAX_LISTS;
        if (ok) f->lists = room_for(f, f->lists, &f->lists_cap, (uint64_t)f->nlists + 1u, sizeof *f->lists, &ok);
        if (!ok) return 0;
        f->list_last = f->nlists++;
        l = &f->lists[f->list_last];
        l->addr = addr;
        l->hi = cmd;
    }
    if (cmd > l->hi) l->hi = cmd;
    return 1;
}

static void on_prim(void *user, const GeMotionPrim *p) {
    MotionTapFrame *f = (MotionTapFrame *)user;
    f->open_prim = -1;
    f->clip_tri = NO_TRI;
    f->have_cur = 0;
    if (!recording(f)) return;
    if (!p || (!p->through && (!p->view || !p->proj))) {
        bad_call(f);
        return;
    }
    const uint64_t n = (uint64_t)f->nprims + 1u, nrefs = (uint64_t)f->nrefs + p->count;
    uint32_t cam = MOTION_TAP_NO_CAMERA;
    int ok = 1;
    f->prims = room_for(f, f->prims, &f->prims_cap, n, sizeof *f->prims, &ok);
    f->mprims = room_for(f, f->mprims, &f->mprims_cap, n, sizeof *f->mprims, &ok);
    f->mserial = room_for(f, f->mserial, &f->mserial_cap, n, sizeof *f->mserial, &ok);
    f->refs = room_for(f, f->refs, &f->refs_cap, nrefs, sizeof *f->refs, &ok);
    f->ref_set = room_for(f, f->ref_set, &f->set_cap, nrefs, sizeof *f->ref_set, &ok);
    if (!ok || !list_note(f, p->list_addr, p->cmd_addr) ||
        (!p->through && !camera_index(f, p->view, p->proj, &cam))) {
        overflow(f);
        return;
    }
    MotionTapPrim *q = &f->prims[f->nprims];
    memset(q, 0, sizeof *q);
    q->first_ref = f->nrefs;
    q->first_tri = f->ntris;
    q->count = p->count;
    q->type = p->type;
    q->through = p->through;
    q->flags = p->flags;
    q->vtype = p->vtype;
    q->vbase = p->vbase;
    q->ibase = p->ibase;
    q->tex_addr = p->tex_addr;
    q->clut_addr = p->clut_addr;
    q->state_class = p->state_class;
    q->list_addr = p->list_addr;
    q->cmd_addr = p->cmd_addr;
    q->fbp = p->fbp;
    q->fbw = p->fbw;
    q->zbp = p->zbp;
    q->zbw = p->zbw;
    memcpy(q->viewport, p->viewport, sizeof q->viewport);
    memcpy(q->offset, p->offset, sizeof q->offset);
    q->bbox[0] = q->bbox[1] = INFINITY;
    q->bbox[2] = q->bbox[3] = -INFINITY;
    if (!(p->flags & GE_MOTION_PRIM_INDEXED)) {
        q->index_state = MOTION_INDEX_NONE;
    } else if (p->index_bytes) {
        q->index_state = MOTION_INDEX_HASHED;
        q->index_hash = fnv1a64(p->index_bytes, p->index_size);
        q->index_size = p->index_size;
    } else {
        q->index_state = MOTION_INDEX_UNSEEN;
    }
    q->camera = (uint16_t)cam;
    if (cam != MOTION_TAP_NO_CAMERA) f->cams[cam].verts += p->count;
    q->match_index = -1;
    if (p->count) memset(f->ref_set + f->nrefs, 0, p->count);
    f->nrefs += p->count;
    f->open_prim = (int32_t)f->nprims++;
}

/* A reference is written by the first triangle or sprite that names its position; the ones
 * after it name the same vertex and were given the same values. */
static void put_ref(MotionTapFrame *f, const MotionTapPrim *q, uint32_t pos, const GeMotionVtx *v) {
    const uint32_t i = q->first_ref + pos;
    if (f->ref_set[i]) return;
    f->refs[i] = *v;
    f->ref_set[i] = 1;
}

static void set_current(MotionTapFrame *f, uint32_t tri, int kind, int piece) {
    f->have_cur = 1;
    f->cur.prim = (uint32_t)f->open_prim;
    f->cur.tri = tri;
    f->cur.kind = (uint8_t)kind;
    f->cur.piece = (uint8_t)piece;
}

/* A record for the open PRIM, or NULL when it cannot be kept. A record with no PRIM open, or
 * with a position outside it, breaks the tap's contract. */
static MotionTapTri *new_record(MotionTapFrame *f, const uint16_t *pos, int npos) {
    f->clip_tri = NO_TRI;
    f->have_cur = 0;
    if (!recording(f)) return NULL;
    if (f->open_prim < 0) {
        bad_call(f);
        return NULL;
    }
    const MotionTapPrim *q = &f->prims[f->open_prim];
    for (int k = 0; k < npos; k++)
        if (pos[k] >= q->count) {
            bad_call(f);
            return NULL;
        }
    int ok = 1;
    f->tris = room_for(f, f->tris, &f->tris_cap, (uint64_t)f->ntris + 1u, sizeof *f->tris, &ok);
    if (!ok) {
        overflow(f);
        return NULL;
    }
    MotionTapTri *t = &f->tris[f->ntris];
    memset(t, 0, sizeof *t);
    return t;
}

static void on_tri(void *user, const uint16_t pos[3], const GeMotionVtx v[3], int decision) {
    MotionTapFrame *f = (MotionTapFrame *)user;
    MotionTapTri *t = new_record(f, pos, 3);
    if (!t) return;
    MotionTapPrim *q = &f->prims[f->open_prim];
    for (int k = 0; k < 3; k++) {
        t->pos[k] = pos[k];
        put_ref(f, q, pos[k], &v[k]);
    }
    t->decision = (uint8_t)decision;
    const uint32_t tri = f->ntris++;
    q->ntris++;
    if (decision == GE_MOTION_DRAW) set_current(f, tri, MOTION_CUR_TRI, 0);
    else if (decision == GE_MOTION_CLIP) f->clip_tri = tri;
}

static void on_fan(void *user, int piece) {
    MotionTapFrame *f = (MotionTapFrame *)user;
    f->have_cur = 0;
    if (!recording(f)) return;
    if (f->clip_tri == NO_TRI || piece < 0 || piece > 1) {
        bad_call(f);
        return;
    }
    f->tris[f->clip_tri].pieces |= (uint8_t)(1u << piece);
    set_current(f, f->clip_tri, MOTION_CUR_FAN, piece);
}

static void on_sprite(void *user, const uint16_t pos[2], const GeMotionVtx *v, int nv, int decision) {
    MotionTapFrame *f = (MotionTapFrame *)user;
    nv = nv < 0 ? 0 : nv > 2 ? 2 : nv;
    MotionTapTri *t = new_record(f, pos, 2);
    if (!t) return;
    MotionTapPrim *q = &f->prims[f->open_prim];
    t->pos[0] = pos[0];
    t->pos[1] = pos[1];
    t->pos[2] = (uint16_t)nv;
    for (int k = 0; k < nv; k++) put_ref(f, q, pos[k], &v[k]);
    t->decision = (uint8_t)decision;
    const uint32_t tri = f->ntris++;
    q->ntris++;
    if (decision == GE_MOTION_DRAW) set_current(f, tri, MOTION_CUR_SPRITE, 0);
}

/* A point or line: nothing is ever published for one, so what it draws cannot be placed. */
static void on_line_point(void *user, int nv) {
    MotionTapFrame *f = (MotionTapFrame *)user;
    (void)nv;
    if (recording(f)) f->unnamed++;
}

static void box_add(float *box, const GeVtx *v) {
    if (v->x < box[0]) box[0] = v->x;
    if (v->y < box[1]) box[1] = v->y;
    if (v->x > box[2]) box[2] = v->x;
    if (v->y > box[3]) box[3] = v->y;
}

static void on_rastered(void *user, const GeVtx *v0, const GeVtx *v1, const GeVtx *v2) {
    MotionTapFrame *f = (MotionTapFrame *)user;
    if (!f->have_cur) {
        /* Nothing was published for it: a Bezier or spline patch's primitive, or an
         * immediate-mode one. What it drew cannot be placed. */
        if (recording(f)) f->unnamed++;
        return;
    }
    f->have_cur = 0;
    /* The rasterizer drops a primitive with a non-finite vertex before it draws anything. */
    if (!ge_vtx_finite(v0) || !ge_vtx_finite(v1) || (v2 && !ge_vtx_finite(v2))) return;
    MotionTapPrim *q = &f->prims[f->cur.prim];
    q->submitted++;
    box_add(q->bbox, v0);
    box_add(q->bbox, v1);
    if (v2) box_add(q->bbox, v2);
}

MotionTapFrame *motion_tap_frame_create(size_t cap_bytes) {
    MotionTapFrame *f = (MotionTapFrame *)calloc(1, sizeof *f);
    if (!f) return NULL;
    f->cap_bytes = cap_bytes ? cap_bytes : MOTION_TAP_DEFAULT_CAP;
    f->tap.user = f;
    f->tap.prim = on_prim;
    f->tap.tri = on_tri;
    f->tap.fan = on_fan;
    f->tap.sprite = on_sprite;
    f->tap.rastered = on_rastered;
    f->tap.line_point = on_line_point;
    motion_tap_frame_reset(f);
    return f;
}

void motion_tap_frame_free(MotionTapFrame *f) {
    if (!f) return;
    free(f->prims);
    free(f->refs);
    free(f->ref_set);
    free(f->tris);
    free(f->cams);
    free(f->lists);
    free(f->mprims);
    free(f->mserial);
    free(f);
}

void motion_tap_frame_reset(MotionTapFrame *f) {
    if (!f) return;
    f->flags = f->dropped = f->late = f->unnamed = 0;
    f->nprims = f->nrefs = f->ntris = f->ncams = f->nlists = f->nmprims = 0;
    f->cam_last = f->list_last = 0;
    memset(&f->camera, 0, sizeof f->camera);
    f->open_prim = -1;
    f->clip_tri = NO_TRI;
    f->have_cur = 0;
}

const GeMotionTap *motion_tap_frame_listener(MotionTapFrame *f) {
    return f ? &f->tap : NULL;
}

int motion_tap_current(const MotionTapFrame *f, MotionTapCurrent *out) {
    if (!f || !f->have_cur) return 0;
    if (out) *out = f->cur;
    return 1;
}

void motion_tap_frame_view(const MotionTapFrame *f, MotionTapView *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!f) return;
    out->flags = f->flags;
    out->nprims = f->nprims;
    out->nrefs = f->nrefs;
    out->ntris = f->ntris;
    out->ncameras = f->ncams;
    out->dropped = f->dropped;
    out->late = f->late;
    out->unnamed = f->unnamed;
    out->bytes = f->bytes;
    out->prims = f->prims;
    out->refs = f->refs;
    out->ref_set = f->ref_set;
    out->tris = f->tris;
    out->cameras = f->cams;
}

/* ---- the bridge to motion_match ------------------------------------------------------------- */

static int in_lists(const MotionTapFrame *f, uint32_t addr) {
    for (uint32_t i = 0; i < f->nlists; i++)
        if (addr >= f->lists[i].addr && addr <= f->lists[i].hi) return 1;
    return 0;
}

void motion_tap_frame_close(MotionTapFrame *f, const MotionTapFrame *prev) {
    if (!f || (f->flags & MOTION_TAP_CLOSED)) return;
    f->flags |= MOTION_TAP_CLOSED;
    f->open_prim = -1;
    f->clip_tri = NO_TRI;
    f->have_cur = 0;
    f->nmprims = 0;
    for (uint32_t s = 0; s < f->nprims; s++) {
        MotionTapPrim *q = &f->prims[s];
        q->match_index = -1;
        if (!q->submitted) continue;
        MotionPrim *m = &f->mprims[f->nmprims];
        memset(m, 0, sizeof *m);
        m->through = q->through;
        m->prim_type = q->type;
        m->vtype = q->vtype;
        m->count = q->count;
        m->tex_addr = q->tex_addr;
        m->clut_addr = q->clut_addr;
        m->state_class = q->state_class;
        m->vaddr = q->vbase;
        m->iaddr = q->ibase;
        memcpy(m->bbox, q->bbox, sizeof m->bbox);
        const int indexed = (q->flags & GE_MOTION_PRIM_INDEXED) != 0;
        if (in_lists(f, q->vbase) || (indexed && in_lists(f, q->ibase)) ||
            (prev && (in_lists(prev, q->vbase) || (indexed && in_lists(prev, q->ibase)))))
            m->flags |= MOTION_PRIM_INLINE;
        f->mserial[f->nmprims] = s;
        q->match_index = (int32_t)f->nmprims++;
    }
    memset(&f->camera, 0, sizeof f->camera);
    const MotionTapCamera *best = NULL;
    for (uint32_t i = 0; i < f->ncams; i++)
        if (!best || f->cams[i].verts > best->verts) best = &f->cams[i];
    if (best) {
        f->camera.valid = 1;
        memcpy(f->camera.view, best->view, sizeof f->camera.view);
        memcpy(f->camera.proj, best->proj, sizeof f->camera.proj);
    }
}

void motion_tap_match_frame(const MotionTapFrame *f, MotionMatchFrame *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!f || !(f->flags & MOTION_TAP_CLOSED)) return;
    out->prims = f->mprims;
    out->nprims = f->nmprims;
    out->camera = f->camera;
}

uint32_t motion_tap_match_serial(const MotionTapFrame *f, uint32_t match_index) {
    if (!f || !(f->flags & MOTION_TAP_CLOSED) || match_index >= f->nmprims) return UINT32_MAX;
    return f->mserial[match_index];
}

/* ---- the builder ---------------------------------------------------------------------------- */

enum { CLS_NONE = 0, CLS_COPY, CLS_LERP };

struct MotionBuild {
    const MotionTapFrame *fa, *fb;
    MotionBuildResult r;
    int32_t *a_of;                      /* per PRIM of B: the serial of its PRIM in A, or -1 */
    uint32_t a_cap;
    uint32_t *prim_of;                  /* per record of B: the serial of its PRIM */
    uint32_t prim_cap;
    uint8_t *cls;                       /* per record of B: CLS_* */
    uint32_t cls_cap;
    uint32_t *unpaired;
    uint32_t unpaired_cap, nunpaired;
    uint8_t *a_taken;                   /* per PRIM of A: 1 when the matcher paired it with a PRIM of B */
    uint32_t taken_cap;
};

/* One value of the frame at t. A value that does not change stays itself at every t, which
 * the weighted sum alone does not promise, and at t = 1 the value is B's own, bits included:
 * a (1 - 1) + b would turn a -0 of B into +0. */
static float mix(float a, float b, float t) {
    if (a == b || t == 1.0f) return b;
    return a * (1.0f - t) + b * t;
}

/* The backend's 28.4 snap (put_vert), which is also the software rasterizer's rounding. */
static float snap16(float v) {
    return floorf(v * 16.0f + 0.375f) * (1.0f / 16.0f);
}

static float clamp01(float v) {
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

static uint32_t channel(int a, int b, float t) {
    const float m = mix((float)a, (float)b, t);
    return m < 0.0f ? 0u : m > 255.0f ? 255u : (uint32_t)(m + 0.5f);
}

static uint32_t mix_rgba(const GeMotionVtx *a, const GeMotionVtx *b, float t) {
    return channel(a->r, b->r, t) | (channel(a->g, b->g, t) << 8) | (channel(a->b, b->b, t) << 16) |
           (channel(a->a, b->a, t) << 24);
}

/* A position before the snap: ge.c's project_cvtx with the PRIM's viewport and offset, or, in
 * through mode, the position itself with rw = 1 as load_vtx leaves it. */
static void place(const MotionTapPrim *p, const float c[4], float u, float v, MotionBuiltVert *o) {
    if (p->through) {
        o->x = c[0];
        o->y = c[1];
        o->z = c[2];
        o->rw = 1.0f;
        o->u = u;
        o->v = v;
        return;
    }
    float inv_w = 1.0f / c[3];
    float ndcx = c[0] * inv_w, ndcy = c[1] * inv_w, ndcz = c[2] * inv_w;
    o->x = ndcx * p->viewport[0] + p->viewport[3] - p->offset[0];
    o->y = ndcy * p->viewport[1] + p->viewport[4] - p->offset[1];
    float sz = ndcz * p->viewport[2] + p->viewport[5];
    o->z = sz < 0.0f ? 0.0f : sz > 65535.0f ? 65535.0f : sz;
    o->rw = inv_w;
    o->u = u * inv_w;
    o->v = v * inv_w;
}

static const GeMotionVtx *ref_at(const MotionTapFrame *f, const MotionTapPrim *p, uint32_t pos) {
    if (pos >= p->count || !f->ref_set[p->first_ref + pos]) return NULL;
    return &f->refs[p->first_ref + pos];
}

static int same_bits(float a, float b) {
    return memcmp(&a, &b, sizeof a) == 0;
}

/* -0 equals +0; a NaN equals a NaN with the same bits, so a vertex that is not a number and
 * did not change is still. */
static int same_value(float a, float b) {
    return a == b || same_bits(a, b);
}

/* Whether every value the builder mixes is the same in both. */
static int ref_same(const GeMotionVtx *a, const GeMotionVtx *b) {
    return same_value(a->clip[0], b->clip[0]) && same_value(a->clip[1], b->clip[1]) &&
           same_value(a->clip[2], b->clip[2]) && same_value(a->clip[3], b->clip[3]) &&
           same_value(a->fog_arg, b->fog_arg) && a->r == b->r && a->g == b->g && a->b == b->b && a->a == b->a;
}

static int ref_finite(const GeMotionVtx *v) {
    return isfinite(v->clip[0]) && isfinite(v->clip[1]) && isfinite(v->clip[2]) && isfinite(v->clip[3]) &&
           isfinite(v->fog_arg);
}

static int same_shape(const MotionTapPrim *a, const MotionTapPrim *b) {
    return a->type == b->type && a->through == b->through && a->count == b->count && a->vtype == b->vtype;
}

/* Position i names the same model vertex in both only when the index bytes are the same. */
static int same_indices(const MotionTapPrim *a, const MotionTapPrim *b) {
    if (a->index_state != b->index_state) return 0;
    if (a->index_state == MOTION_INDEX_NONE) return 1;
    return a->index_state == MOTION_INDEX_HASHED && a->index_size == b->index_size &&
           a->index_hash == b->index_hash;
}

/* ge.c's acceptance tests are half-spaces in clip space for a fixed viewport, offset and
 * depth-clip switch: only then does "inside in A and in B" hold for every t between. */
static int same_projection(const MotionTapPrim *a, const MotionTapPrim *b) {
    return memcmp(a->viewport, b->viewport, sizeof a->viewport) == 0 &&
           memcmp(a->offset, b->offset, sizeof a->offset) == 0 &&
           ((a->flags ^ b->flags) & GE_MOTION_PRIM_DEPTH_CLIP) == 0;
}

static int why_unknown(MotionBuild *b) {
    b->r.unknown++;
    b->r.why |= MOTION_WHY_UNKNOWN;
    return CLS_NONE;
}

static int why_not_plain(MotionBuild *b, int dropped_both) {
    b->r.not_plain++;
    if (dropped_both) b->r.dropped_both++;
    b->r.why |= MOTION_WHY_NOT_PLAIN;
    return CLS_NONE;
}

static int why_state(MotionBuild *b) {
    b->r.state++;
    b->r.why |= MOTION_WHY_STATE;
    return CLS_NONE;
}

static int classify_tri(MotionBuild *b, const MotionTapPrim *pa, const MotionTapPrim *pb,
                        const MotionTapTri *ta, const MotionTapTri *tb) {
    /* The same three positions, whichever way each frame's cull state ordered the first two. */
    if (ta->pos[2] != tb->pos[2] ||
        !((ta->pos[0] == tb->pos[0] && ta->pos[1] == tb->pos[1]) ||
          (ta->pos[0] == tb->pos[1] && ta->pos[1] == tb->pos[0])))
        return why_unknown(b);
    const GeMotionVtx *ra[3], *rb[3];
    int moving = 0, finite = 1;
    for (int c = 0; c < 3; c++) {
        ra[c] = ref_at(b->fa, pa, tb->pos[c]);
        rb[c] = ref_at(b->fb, pb, tb->pos[c]);
        if (!ra[c] || !rb[c]) return why_unknown(b);
        moving = moving || !ref_same(ra[c], rb[c]);
        finite = finite && ref_finite(ra[c]) && ref_finite(rb[c]);
    }
    if (!moving) {
        if (tb->decision == GE_MOTION_DROP) return CLS_NONE;
        b->r.copied++;
        return CLS_COPY;
    }
    if (ta->decision == GE_MOTION_CLIP || tb->decision == GE_MOTION_CLIP) {
        b->r.near_clip++;
        b->r.why |= MOTION_WHY_NEAR_CLIP;
        return CLS_NONE;
    }
    if (ta->decision != GE_MOTION_DRAW || tb->decision != GE_MOTION_DRAW)
        return why_not_plain(b, ta->decision == GE_MOTION_DROP && tb->decision == GE_MOTION_DROP);
    if (!pb->through && !same_projection(pa, pb)) return why_state(b);
    if (!finite) return why_not_plain(b, 0);
    for (int c = 0; c < 3; c++)
        if (ra[c]->u != rb[c]->u || ra[c]->v != rb[c]->v) b->r.uv_changed++;
    b->r.built++;
    return CLS_LERP;
}

/* 1 when the corner order along one axis cannot turn round between A and B. The backend
 * assigns a sprite's texture coordinates by that order, and the in-between keeps B's. */
static int same_order(float a0, float a1, float b0, float b1) {
    return b0 <= b1 ? a0 <= a1 : a0 >= a1;
}

static int classify_sprite(MotionBuild *b, const MotionTapPrim *pa, const MotionTapPrim *pb,
                           const MotionTapTri *ta, const MotionTapTri *tb) {
    if (ta->pos[0] != tb->pos[0] || ta->pos[1] != tb->pos[1]) return why_unknown(b);
    const uint32_t na = ta->pos[2], nb = tb->pos[2];
    const GeMotionVtx *ra[2] = { NULL, NULL }, *rb[2] = { NULL, NULL };
    int same = na == nb, finite = 1;
    for (uint32_t c = 0; c < 2; c++) {
        if (c < na && !(ra[c] = ref_at(b->fa, pa, ta->pos[c]))) return why_unknown(b);
        if (c < nb && !(rb[c] = ref_at(b->fb, pb, tb->pos[c]))) return why_unknown(b);
        if (ra[c] && rb[c]) {
            same = same && ref_same(ra[c], rb[c]);
            finite = finite && ref_finite(ra[c]) && ref_finite(rb[c]);
        }
    }
    if (same) {
        if (tb->decision != GE_MOTION_DRAW) return CLS_NONE;
        b->r.copied++;
        return CLS_COPY;
    }
    if (ta->decision != GE_MOTION_DRAW || tb->decision != GE_MOTION_DRAW)
        return why_not_plain(b, ta->decision == GE_MOTION_DROP && tb->decision == GE_MOTION_DROP);
    if (na != 2u || nb != 2u) return why_unknown(b);
    if (!pb->through && !same_projection(pa, pb)) return why_state(b);
    if (!finite) return why_not_plain(b, 0);
    /* With one w for both corners in each frame their order on the screen is the order of
     * their clip x and y, which the mix keeps. Corners at different depths can cross twice. */
    if (!pb->through && (ra[0]->clip[3] != ra[1]->clip[3] || rb[0]->clip[3] != rb[1]->clip[3]))
        return why_not_plain(b, 0);
    MotionBuiltVert a0, a1, b0, b1;
    place(pb, ra[0]->clip, 0.0f, 0.0f, &a0);
    place(pb, ra[1]->clip, 0.0f, 0.0f, &a1);
    place(pb, rb[0]->clip, 0.0f, 0.0f, &b0);
    place(pb, rb[1]->clip, 0.0f, 0.0f, &b1);
    if (!same_order(a0.x, a1.x, b0.x, b1.x) || !same_order(a0.y, a1.y, b0.y, b1.y))
        return why_not_plain(b, 0);
    /* A rectangle with no pixel was not handed on by B's backend: there are no vertices of B
     * to move, though the in-between one could cover a pixel. */
    if (!(floorf(fmaxf(b0.x, b1.x)) > floorf(fminf(b0.x, b1.x)) &&
          floorf(fmaxf(b0.y, b1.y)) > floorf(fminf(b0.y, b1.y))))
        return why_not_plain(b, 0);
    b->r.built++;
    return CLS_LERP;
}

static int build_room(MotionBuild *b, uint32_t nprims, uint32_t ntris, uint32_t na) {
    if (na > b->taken_cap) {
        void *p = realloc(b->a_taken, na);
        if (!p) return 0;
        b->a_taken = (uint8_t *)p;
        b->taken_cap = na;
    }
    if (nprims > b->a_cap) {
        void *p = realloc(b->a_of, (size_t)nprims * sizeof *b->a_of);
        if (!p) return 0;
        b->a_of = (int32_t *)p;
        b->a_cap = nprims;
    }
    if (nprims > b->unpaired_cap) {
        void *p = realloc(b->unpaired, (size_t)nprims * sizeof *b->unpaired);
        if (!p) return 0;
        b->unpaired = (uint32_t *)p;
        b->unpaired_cap = nprims;
    }
    if (ntris > b->prim_cap) {
        void *p = realloc(b->prim_of, (size_t)ntris * sizeof *b->prim_of);
        if (!p) return 0;
        b->prim_of = (uint32_t *)p;
        b->prim_cap = ntris;
    }
    if (ntris > b->cls_cap) {
        void *p = realloc(b->cls, (size_t)ntris);
        if (!p) return 0;
        b->cls = (uint8_t *)p;
        b->cls_cap = ntris;
    }
    return 1;
}

/* What the matcher keys a PRIM by: its signature, then its vertex and index addresses. */
static int same_signature(const MotionTapPrim *a, const MotionTapPrim *b) {
    return same_shape(a, b) && a->tex_addr == b->tex_addr && a->clut_addr == b->clut_addr &&
           a->state_class == b->state_class;
}

static int same_key(const MotionTapPrim *a, const MotionTapPrim *b) {
    const int indexed = (a->flags & GE_MOTION_PRIM_INDEXED) != 0;
    return same_signature(a, b) && indexed == ((b->flags & GE_MOTION_PRIM_INDEXED) != 0) && a->vbase == b->vbase &&
           (!indexed || a->ibase == b->ibase);
}

/* A key whose vertices or indices lie inside a list either frame ran can name another object
 * in the other frame (motion_match never anchors one). */
static int inline_key(const MotionBuild *b, const MotionTapPrim *q) {
    const int indexed = (q->flags & GE_MOTION_PRIM_INDEXED) != 0;
    return in_lists(b->fb, q->vbase) || in_lists(b->fa, q->vbase) ||
           (indexed && (in_lists(b->fb, q->ibase) || in_lists(b->fa, q->ibase)));
}

#define TWIN_NONE  (-1)                 /* no PRIM of A can be it */
#define TWIN_UNSURE (-2)                /* one might be, and which cannot be told */

/* The PRIM of A that B's PRIM s stands for when one of the two drew nothing, so the matcher
 * had no box to pair them by. It is the PRIM of A with the same key, when that key is the only
 * one of its kind in both frames and not inline. `b_drew` says whether B's PRIM drew: then
 * only PRIMs of A that drew nothing are candidates, the others being the matcher's to pair.
 * Any other candidate with the same signature (one the matcher left unpaired, or one that
 * drew nothing) might be the same object under another address, so it makes the answer
 * unsure. */
static int32_t twin_in_a(const MotionBuild *b, uint32_t s, int b_drew) {
    const MotionTapFrame *fa = b->fa, *fb = b->fb;
    const MotionTapPrim *pb = &fb->prims[s];
    int32_t twin = TWIN_NONE;
    uint32_t keyed = 0;
    int other = 0;
    for (uint32_t i = 0; i < fa->nprims; i++) {
        const MotionTapPrim *pa = &fa->prims[i];
        const int drew = pa->match_index >= 0;
        if (!pa->ntris || (b_drew && drew) || !same_signature(pa, pb)) continue;
        if (same_key(pa, pb)) {
            keyed++;
            twin = (int32_t)i;
        } else if (!drew || !b->a_taken[i]) {
            other = 1;
        }
    }
    if (!keyed) return other ? TWIN_UNSURE : TWIN_NONE;
    if (keyed > 1 || other || inline_key(b, pb)) return TWIN_UNSURE;
    for (uint32_t k = 0; k < fb->nprims; k++)
        if (k != s && same_key(&fb->prims[k], pb)) return TWIN_UNSURE;
    return twin;
}

/* B's PRIM s against A's PRIM pa, record by record. */
static void classify_records(MotionBuild *b, uint32_t s, const MotionTapPrim *pa) {
    const MotionTapFrame *fa = b->fa, *fb = b->fb;
    const MotionTapPrim *pb = &fb->prims[s];
    b->a_of[s] = (int32_t)(pa - fa->prims);
    for (uint32_t k = 0; k < pb->ntris; k++) {
        const MotionTapTri *tb = &fb->tris[pb->first_tri + k];
        if (k >= pa->ntris) {
            (void)why_unknown(b);
            continue;
        }
        const MotionTapTri *ta = &fa->tris[pa->first_tri + k];
        b->cls[pb->first_tri + k] = (uint8_t)(pb->type == 6u ? classify_sprite(b, pa, pb, ta, tb)
                                                             : classify_tri(b, pa, pb, ta, tb));
    }
}

/* B's PRIM s and a PRIM of A that drew nothing in one of the two frames, paired by key. */
static int classify_twin(MotionBuild *b, uint32_t s, int b_drew) {
    const int32_t ia = twin_in_a(b, s, b_drew);
    if (ia == TWIN_NONE) return 0;
    const MotionTapPrim *pa = ia >= 0 ? &b->fa->prims[ia] : NULL;
    if (!pa || !same_indices(pa, &b->fb->prims[s])) {
        (void)why_unknown(b);
        return 1;
    }
    b->r.paired_undrawn++;
    classify_records(b, s, pa);
    return 1;
}

static void classify(MotionBuild *b, const int32_t *pair_a) {
    const MotionTapFrame *fa = b->fa, *fb = b->fb;
    if (fa) {
        memset(b->a_taken, 0, fa->nprims);
        for (uint32_t k = 0; pair_a && k < fb->nmprims; k++)
            if (pair_a[k] >= 0 && (uint32_t)pair_a[k] < fa->nmprims) b->a_taken[fa->mserial[pair_a[k]]] = 1;
    }
    for (uint32_t s = 0; s < fb->nprims; s++) {
        const MotionTapPrim *pb = &fb->prims[s];
        b->a_of[s] = -1;
        for (uint32_t k = 0; k < pb->ntris; k++) {
            b->prim_of[pb->first_tri + k] = s;
            b->cls[pb->first_tri + k] = CLS_NONE;
        }
        /* Points and lines publish no primitive: what they drew cannot be placed, and it must
         * not be taken for something drawn as in B. */
        if (pb->type <= 2u && pb->count >= (pb->type == 0u ? 1u : 2u)) {
            (void)why_unknown(b);
            continue;
        }
        /* A PRIM B recorded but drew nothing of: what A drew of it, if anything, is judged
         * record by record, so a moving one stops the frame as one dropped triangle does. */
        if (pb->match_index < 0) {
            if (fa && pb->ntris) (void)classify_twin(b, s, 0);
            continue;
        }
        b->r.draws++;
        const MotionTapPrim *pa = NULL;
        if (fa && pair_a && pair_a[pb->match_index] < 0 && classify_twin(b, s, 1)) continue;
        if (fa && pair_a && pair_a[pb->match_index] >= 0) {
            const uint32_t ia = (uint32_t)pair_a[pb->match_index];
            if (ia >= fa->nmprims) b->r.why |= MOTION_WHY_FAILED;
            else pa = &fa->prims[fa->mserial[ia]];
        }
        if (pa && !same_shape(pa, pb)) {
            b->r.unpaired_shape++;
            pa = NULL;
        }
        if (pa && !same_indices(pa, pb)) {
            b->r.unpaired_index++;
            pa = NULL;
        }
        if (!pa) {
            b->r.unpaired++;
            b->unpaired[b->nunpaired++] = s;
            for (uint32_t k = 0; k < pb->ntris; k++)
                if (fb->tris[pb->first_tri + k].decision != GE_MOTION_DROP) {
                    b->cls[pb->first_tri + k] = CLS_COPY;
                    b->r.copied++;
                }
            continue;
        }
        b->r.paired++;
        classify_records(b, s, pa);
    }
}

MotionBuild *motion_build_create(void) {
    return (MotionBuild *)calloc(1, sizeof(MotionBuild));
}

void motion_build_free(MotionBuild *b) {
    if (!b) return;
    free(b->a_of);
    free(b->prim_of);
    free(b->cls);
    free(b->unpaired);
    free(b->a_taken);
    free(b);
}

int motion_build_plan(MotionBuild *b, const MotionTapFrame *fa, const MotionTapFrame *fb,
                      const int32_t *pair_a, const MotionMatchResult *match) {
    if (!b) return 0;
    memset(&b->r, 0, sizeof b->r);
    b->fa = fa;
    b->fb = fb;
    b->nunpaired = 0;
    if (!fb || !match || !(fb->flags & MOTION_TAP_CLOSED) || (fa && !(fa->flags & MOTION_TAP_CLOSED)) ||
        (fa && !pair_a) || !build_room(b, fb->nprims, fb->ntris, fa ? fa->nprims : 0u)) {
        b->fb = NULL;
        b->r.why = MOTION_WHY_FAILED;
        b->r.flags = MOTION_F_NOT_REPLAYABLE;
        return 0;
    }
    b->r.match_flags = match->flags;
    if (match->flags & MOTION_MATCH_FAILED) b->r.why |= MOTION_WHY_FAILED;
    if (!fa || (match->flags & MOTION_MATCH_NO_PREVIOUS)) b->r.why |= MOTION_WHY_NO_PREVIOUS;
    if (match->flags & MOTION_MATCH_CUT) b->r.why |= MOTION_WHY_CUT;
    if ((fb->flags & MOTION_TAP_OVERFLOW) || (fa && (fa->flags & MOTION_TAP_OVERFLOW)))
        b->r.why |= MOTION_WHY_OVERFLOW;
    if ((fb->flags & MOTION_TAP_BAD_CALL) || (fa && (fa->flags & MOTION_TAP_BAD_CALL)))
        b->r.why |= MOTION_WHY_BAD_CALL;
    /* Calls after a close: the list drew on into that frame with nothing recorded. */
    if (fb->late || (fa && fa->late)) b->r.why |= MOTION_WHY_LATE;
    /* What B drew with nothing published for it (points, lines, patches, immediate-mode
     * primitives) cannot be placed; in A alone it does not matter. */
    if (fb->unnamed) {
        b->r.unknown += fb->unnamed;
        b->r.why |= MOTION_WHY_UNKNOWN;
    }
    /* An incomplete store cannot be classified. A held frame still is: its counts say what
     * the pairs would have given. */
    if (b->r.why & (MOTION_WHY_OVERFLOW | MOTION_WHY_BAD_CALL)) {
        if (fb->ntris) memset(b->cls, CLS_NONE, fb->ntris);
    } else {
        classify(b, pair_a);
    }
    if (b->r.why & MOTION_WHY_CUT) b->r.flags |= MOTION_F_CUT;
    if (b->r.why & ~MOTION_WHY_CUT) b->r.flags |= MOTION_F_NOT_REPLAYABLE;
    return b->r.why == 0;
}

const MotionBuildResult *motion_build_result(const MotionBuild *b) {
    return b ? &b->r : NULL;
}

const uint32_t *motion_build_unpaired(const MotionBuild *b, uint32_t *n) {
    if (n) *n = b ? b->nunpaired : 0;
    return b ? b->unpaired : NULL;
}

/* The values of a CLS_LERP record at t: three vertices of a triangle, or a sprite's rectangle. */
static void build(const MotionBuild *b, uint32_t tri, float t, MotionBuiltVert *out) {
    const MotionTapFrame *fa = b->fa, *fb = b->fb;
    const uint32_t s = b->prim_of[tri];
    const MotionTapPrim *pb = &fb->prims[s], *pa = &fa->prims[b->a_of[s]];
    const MotionTapTri *tb = &fb->tris[tri];
    const int n = pb->type == 6u ? 2 : 3;
    const GeMotionVtx *ra[3], *rb[3];
    MotionBuiltVert v[3];
    for (int c = 0; c < n; c++) {
        ra[c] = &fa->refs[pa->first_ref + tb->pos[c]];
        rb[c] = &fb->refs[pb->first_ref + tb->pos[c]];
        float clip[4];
        for (int k = 0; k < 4; k++) clip[k] = mix(ra[c]->clip[k], rb[c]->clip[k], t);
        place(pb, clip, rb[c]->u, rb[c]->v, &v[c]);
        v[c].fog = clamp01(mix(ra[c]->fog_arg, rb[c]->fog_arg, t));
        v[c].rgba = 0;
    }
    if (n == 2) {
        /* The backend's sprite: whole pixels from the floored corners, depth, fog and colour
         * of the second corner, rw = 1. */
        const float xa = floorf(fminf(v[0].x, v[1].x)), xb = floorf(fmaxf(v[0].x, v[1].x));
        const float ya = floorf(fminf(v[0].y, v[1].y)), yb = floorf(fmaxf(v[0].y, v[1].y));
        const uint32_t rgba = mix_rgba(ra[1], rb[1], t);
        for (int c = 0; c < 2; c++) {
            out[c].x = snap16(c ? xb : xa);
            out[c].y = snap16(c ? yb : ya);
            out[c].z = v[1].z;
            out[c].rw = 1.0f;
            out[c].u = out[c].v = 0.0f;
            out[c].fog = v[1].fog;
            out[c].rgba = rgba;
        }
        return;
    }
    const int flat = (pb->flags & GE_MOTION_PRIM_FLAT) != 0;
    for (int c = 0; c < 3; c++) {
        const int from = flat ? 2 : c;   /* the last vertex is the provoking one */
        out[c] = v[c];
        out[c].x = snap16(v[c].x);
        out[c].y = snap16(v[c].y);
        out[c].rgba = mix_rgba(ra[from], rb[from], t);
    }
}

void motion_backend_vertices(int flat, const GeVtx *g, int n, MotionBuiltVert *out) {
    if (!g || !out || (n != 2 && n != 3)) return;
    for (int c = 0; c < n; c++) {
        /* The vertex whose colour it takes: a sprite's second corner, under flat shading the last. */
        const GeVtx *from = n == 2 ? &g[1] : &g[flat ? 2 : c];
        const int ch[4] = { from->r, from->g, from->b, from->a };
        uint32_t rgba = 0;
        for (int k = 0; k < 4; k++) rgba |= (uint32_t)(ch[k] < 0 ? 0 : ch[k] > 255 ? 255 : ch[k]) << (8 * k);
        out[c].rgba = rgba;
        if (n == 2) {
            /* Whole pixels from the floored corners, the second corner's depth and fog, rw = 1. */
            out[c].x = snap16(floorf(c ? fmaxf(g[0].x, g[1].x) : fminf(g[0].x, g[1].x)));
            out[c].y = snap16(floorf(c ? fmaxf(g[0].y, g[1].y) : fminf(g[0].y, g[1].y)));
            out[c].z = g[1].z;
            out[c].rw = 1.0f;
            out[c].u = out[c].v = 0.0f;
            out[c].fog = g[1].fog;
        } else {
            out[c].x = snap16(g[c].x);
            out[c].y = snap16(g[c].y);
            out[c].z = g[c].z;
            out[c].rw = g[c].rw;
            out[c].u = g[c].u;
            out[c].v = g[c].v;
            out[c].fog = g[c].fog;
        }
    }
}

static void fill_mv(const MotionBuild *b, uint32_t tri, MotionVert *mv) {
    const MotionTapFrame *fa = b->fa, *fb = b->fb;
    const uint32_t s = b->prim_of[tri];
    const MotionTapPrim *pb = &fb->prims[s];
    const MotionTapPrim *pa = b->a_of[s] >= 0 ? &fa->prims[b->a_of[s]] : NULL;
    const MotionTapTri *tb = &fb->tris[tri];
    const int n = pb->type == 6u ? 2 : 3;
    for (int c = 0; c < n; c++) {
        const GeMotionVtx *rb = ref_at(fb, pb, tb->pos[c]);
        const GeMotionVtx *ra = pa ? ref_at(fa, pa, tb->pos[c]) : NULL;
        memset(&mv[c], 0, sizeof mv[c]);
        mv[c].prim = s;
        mv[c].index = tb->pos[c];
        if (!rb) continue;
        memcpy(mv[c].clip_b, rb->clip, sizeof mv[c].clip_b);
        memcpy(mv[c].clip_a, ra ? ra->clip : rb->clip, sizeof mv[c].clip_a);
        if (!ra) continue;
        mv[c].state = tb->decision == GE_MOTION_CLIP ? MOTION_V_CLIPPED : MOTION_V_MATCHED;
        if (ra->u != rb->u || ra->v != rb->v) mv[c].flags |= MOTION_V_UV_SCROLL;
    }
}

int motion_build_prim(const MotionBuild *b, uint32_t tri, float t, MotionBuiltVert *out, MotionVert *mv) {
    if (!b || !b->fb || b->r.why || tri >= b->fb->ntris || !(t >= 0.0f && t <= 1.0f)) return MOTION_ITEM_NONE;
    const int cls = b->cls[tri];
    if (cls == CLS_NONE || (cls == CLS_LERP && !out)) return MOTION_ITEM_NONE;
    if (mv) fill_mv(b, tri, mv);
    if (cls == CLS_COPY) return MOTION_ITEM_COPY_B;
    build(b, tri, t, out);
    return MOTION_ITEM_BUILT;
}

int motion_build_check(MotionBuild *b, uint32_t tri, const MotionBuiltVert *submitted) {
    if (!b || !b->fb || !submitted || tri >= b->fb->ntris) return 0;
    if (b->r.why || b->cls[tri] != CLS_LERP) return 1;
    const int sprite = b->fb->prims[b->prim_of[tri]].type == 6u;
    MotionBuiltVert v[3];
    build(b, tri, 1.0f, v);
    int equal = 1;
    for (int c = 0; c < (sprite ? 2 : 3); c++) {
        const MotionBuiltVert *x = &v[c], *y = &submitted[c];
        equal = equal && same_bits(x->x, y->x) && same_bits(x->y, y->y) && same_bits(x->z, y->z) &&
                same_bits(x->rw, y->rw) && same_bits(x->fog, y->fog) && x->rgba == y->rgba &&
                (sprite || (same_bits(x->u, y->u) && same_bits(x->v, y->v)));
    }
    if (equal) return 1;
    b->r.t1_mismatch++;
    b->r.why |= MOTION_WHY_T1_MISMATCH;
    b->r.flags |= MOTION_F_NOT_REPLAYABLE;
    return 0;
}
