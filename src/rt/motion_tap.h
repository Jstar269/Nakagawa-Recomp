/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * motion_tap.h - the frame store and the in-between builder of render-state motion (#2081).
 *
 * Pure C: no Vulkan, no runtime calls, no globals. Three parts.
 *
 * The store. A MotionTapFrame is the listener of ge.c's motion tap (GeMotionTap, ge_shared.h)
 * for one frame. It keeps, per PRIM, its identity and projection state; per PRIM position, the
 * vertex reference before the perspective divide; per assembled triangle or sprite, its
 * positions and ge.c's decision. While a raster call runs it names the primitive that call
 * draws (motion_tap_current), so a backend can tie each vertex it captures to its source; a
 * primitive the tap does not name has none, and reads as unknown. Every raster call for one is
 * counted (MotionTapView.unnamed): a point or a line, from a PRIM, a patch or immediate mode;
 * a patch's triangle; an immediate-mode triangle or sprite.
 *
 * Not seen: a block transfer (ge_block_transfer), and any CPU write, into the buffer B draws
 * to. Neither is a raster call, and the plan does not refuse a frame that holds one; an
 * in-between frame built from B's records lacks what they wrote. Whether to refuse such
 * frames is open: counting every transfer would refuse most frames, texture uploads included.
 *
 * The bridge. motion_tap_frame_close ends a frame and builds the PRIM list motion_match takes:
 * one MotionPrim per PRIM that handed something to the rasterizer, with the box of those
 * vertices. A PRIM that drew nothing is not in the list: it has no box and nothing to place.
 *
 * The builder. From frames A and B, the matcher's pairs and t in [0, 1] it gives, for each
 * triangle or sprite B handed to the rasterizer, the vertices of the frame between them:
 *
 *   clip(t) = clip_A (1 - t) + clip_B t          per PRIM position, a value equal in A and B kept as it is
 *   x, y    = snap16(project(clip(t)))           ge.c's project_cvtx with B's viewport and offset,
 *                                                then the backend's 28.4 snap
 *   z, rw   = of that projection                 u, v = B's raw texel coordinate times rw
 *   fog     = clamp01(arg_A (1 - t) + arg_B t)   the unclamped arguments, so fog starts where it should
 *   colour  = the lit colours mixed the same way, clamped to 0..255 and packed
 *
 * Through mode mixes the screen positions with rw = 1. A sprite's two corners are placed that
 * way and then floored to whole pixels as the backend floors them.
 *
 * Every draw of B is carried in B's order. A draw the matcher paired gets values; one it did
 * not pair, or whose pair the gates here refuse (different index bytes; a different type, mode,
 * vertex count or vertex type), is drawn as in B and counted. A triangle whose inputs are the
 * same in A and B is drawn with B's own vertices, bit for bit. Only what cannot be built
 * exactly stops the whole frame, each with a named reason (MOTION_WHY_*).
 *
 * A PRIM ge.c drew in one frame and dropped whole in the other has no box there, so the matcher
 * cannot pair it. It is paired by its key instead (signature, vertex and index addresses) when
 * that key is the only one of its kind in both frames and not inside a list, and its records
 * are judged as any other's (MOTION_WHY_NOT_PLAIN for a moving one). When a PRIM of A with the
 * same signature might be it under another key, the frame stops (MOTION_WHY_UNKNOWN). The
 * reasons:
 *
 *   - the matcher's cut, no previous frame, or a failure;
 *   - a moving triangle or sprite ge.c did not draw whole in both frames (MOTION_WHY_NOT_PLAIN),
 *     or one the near plane cut in either (MOTION_WHY_NEAR_CLIP): whether and how it is drawn
 *     at t needs ge.c's acceptance and clip at clip(t), which this builder does not run;
 *   - a moving one whose viewport, offset or depth-clip switch differs between the frames
 *     (MOTION_WHY_STATE): being inside the screen grid in both frames says nothing about t then;
 *   - a point or line PRIM, any point or line raster call, a patch's or an immediate-mode
 *     triangle or sprite (a raster call the tap named nothing for), or a record that is
 *     missing (MOTION_WHY_UNKNOWN);
 *   - a store that hit its cap (MOTION_WHY_OVERFLOW), or one given a call that breaks the
 *     tap's contract (MOTION_WHY_BAD_CALL);
 *   - callbacks after A's or B's close: the list drew on with nothing recorded (MOTION_WHY_LATE);
 *   - t = 1 not giving B's own values (MOTION_WHY_T1_MISMATCH, motion_build_check).
 *
 * With a reason set no values are given at all.
 */
#ifndef SR_MOTION_TAP_H
#define SR_MOTION_TAP_H

#include <stddef.h>
#include <stdint.h>

#include "ge_shared.h"
#include "motion_match.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the store ------------------------------------------------------------------------------ */

typedef struct MotionTapFrame MotionTapFrame;

/* Bytes of records one frame may hold when motion_tap_frame_create is given 0. */
#define MOTION_TAP_DEFAULT_CAP ((size_t)64u << 20)
/* Distinct (view, projection) pairs, and distinct lists, one frame may hold. */
#define MOTION_TAP_MAX_CAMERAS 256u
#define MOTION_TAP_MAX_LISTS   256u
#define MOTION_TAP_NO_CAMERA   0xFFFFu

/* MotionTapView.flags */
#define MOTION_TAP_OVERFLOW 0x01u   /* a record did not fit (the cap, a table, or memory): the frame is incomplete */
#define MOTION_TAP_CLOSED   0x02u   /* motion_tap_frame_close ran */
#define MOTION_TAP_BAD_CALL 0x04u   /* a call broke the tap's contract (a transform-mode PRIM with no view or
                                     * projection, a record with no PRIM open or a position outside it, a fan
                                     * piece with no near-clipped triangle): the frame is incomplete */

/* MotionTapPrim.index_state */
enum {
    MOTION_INDEX_NONE = 0,          /* not indexed: position i is vertex i */
    MOTION_INDEX_HASHED,            /* index_hash covers its index bytes */
    MOTION_INDEX_UNSEEN             /* indexed from where the tap does not read: it pairs with nothing */
};

typedef struct {
    uint32_t first_ref;             /* its `count` vertex references start here in refs[] */
    uint32_t first_tri;             /* its triangle or sprite records start here in tris[] */
    uint32_t ntris;
    uint32_t submitted;             /* raster calls made for them with finite vertices */
    uint32_t count;                 /* vertex count of the PRIM command */
    uint8_t  type;                  /* GE PRIM type, 0..7 */
    uint8_t  through;
    uint16_t flags;                 /* GE_MOTION_PRIM_* */
    uint32_t vtype, vbase, ibase;
    uint32_t tex_addr, clut_addr, state_class;
    uint32_t list_addr, cmd_addr;
    uint32_t fbp, fbw, zbp, zbw;
    float    viewport[6];           /* x, y, z scale, then x, y, z centre */
    float    offset[2];
    float    bbox[4];               /* x0, y0, x1, y1 of the vertices those raster calls were given;
                                     * +inf, +inf, -inf, -inf when there were none */
    uint64_t index_hash;            /* FNV-1a 64 of the index bytes when MOTION_INDEX_HASHED */
    uint32_t index_size;
    uint16_t camera;                /* index in cameras[], MOTION_TAP_NO_CAMERA in through mode */
    uint8_t  index_state;           /* MOTION_INDEX_* */
    uint8_t  reserved;
    int32_t  match_index;           /* its index in the matcher's list once closed, -1 when not in it */
} MotionTapPrim;

/* One assembled triangle or sprite (the PRIM's type says which). */
typedef struct {
    uint16_t pos[3];                /* PRIM positions in the order ge.c submits them; a sprite uses [0] and
                                     * [1], and [2] is the number of corners ge.c computed (1 or 2) */
    uint8_t  decision;              /* GE_MOTION_DRAW, GE_MOTION_CLIP or GE_MOTION_DROP */
    uint8_t  pieces;                /* GE_MOTION_CLIP: bit k set when fan piece k went to the rasterizer */
} MotionTapTri;

typedef struct {
    float    view[12], proj[16];    /* in the GE's own order, as MotionCamera */
    uint64_t verts;                 /* vertex count of the transform-mode PRIMs that used it */
} MotionTapCamera;

/* A frame's records, read-only. The pointers hold until the frame is reset or freed, and while
 * it is open until the next callback. */
typedef struct {
    uint32_t flags;                 /* MOTION_TAP_* */
    uint32_t nprims, nrefs, ntris, ncameras;
    uint32_t dropped;               /* callbacks whose record was not kept: the one that overflowed or broke
                                     * the contract, and every one after it */
    uint32_t late;                  /* callbacks that arrived after the close: nothing was recorded */
    uint32_t unnamed;               /* raster calls the tap named nothing for: every point and line (of a
                                     * PRIM, a patch or immediate mode), a patch's triangles, and
                                     * immediate-mode triangles and sprites */
    size_t   bytes;                 /* memory reserved for the records: what the arrays hold room for, kept
                                     * across resets, at most the frame's cap */
    const MotionTapPrim *prims;     /* in draw order: a PRIM's serial is its index here */
    const GeMotionVtx *refs;
    const uint8_t *ref_set;         /* 1 where refs[] holds a reference (a triangle or sprite named it) */
    const MotionTapTri *tris;
    const MotionTapCamera *cameras;
} MotionTapView;

/* The primitive the running raster call draws. */
enum { MOTION_CUR_TRI = 1, MOTION_CUR_FAN, MOTION_CUR_SPRITE };
typedef struct {
    uint32_t prim;                  /* serial of its PRIM */
    uint32_t tri;                   /* index of its record in tris[] */
    uint8_t  kind;                  /* MOTION_CUR_* */
    uint8_t  piece;                 /* fan piece, 0 otherwise */
} MotionTapCurrent;

/* A frame holding at most cap_bytes of records (0: MOTION_TAP_DEFAULT_CAP). NULL: no memory. */
MotionTapFrame *motion_tap_frame_create(size_t cap_bytes);
void motion_tap_frame_free(MotionTapFrame *f);
/* Empties the frame and opens it; its memory is kept for the next frame. */
void motion_tap_frame_reset(MotionTapFrame *f);
/* The callbacks that fill f, for ge_motion_tap_set. Valid while f lives. */
const GeMotionTap *motion_tap_frame_listener(MotionTapFrame *f);
/* 1 and *out when a raster call is running for a primitive the tap named, else 0: the vertices
 * being drawn have no known source. */
int motion_tap_current(const MotionTapFrame *f, MotionTapCurrent *out);
void motion_tap_frame_view(const MotionTapFrame *f, MotionTapView *out);

/* ---- the bridge to motion_match ------------------------------------------------------------- */

/* Ends the frame and builds the matcher's list. `prev` (may be NULL) is the frame before it: a
 * PRIM whose vertex or index address lies inside a list either frame ran is marked
 * MOTION_PRIM_INLINE. The camera is the (view, projection) pair most transform-mode vertices
 * used. */
void motion_tap_frame_close(MotionTapFrame *f, const MotionTapFrame *prev);
/* The closed frame as motion_match takes it (an open frame gives an empty list). Entry k of the
 * list is the PRIM with serial motion_tap_match_serial(f, k). */
void motion_tap_match_frame(const MotionTapFrame *f, MotionMatchFrame *out);
uint32_t motion_tap_match_serial(const MotionTapFrame *f, uint32_t match_index);

/* ---- the builder ---------------------------------------------------------------------------- */

/* One vertex of the in-between frame, in plain fields: the backend packs them into its own
 * vertex. x and y are final: on the 28.4 grid for a triangle, on whole pixels for a sprite. */
typedef struct {
    float x, y, z, rw;
    float u, v;                     /* a triangle's texel coordinate times rw; 0 for a sprite, which keeps B's */
    float fog;
    uint32_t rgba;                  /* r | g << 8 | b << 16 | a << 24 */
} MotionBuiltVert;

/* Where a vertex of B came from, in the layout agreed with the FSR 3 lane (one per vertex of a
 * display draw). The builder fills clip_b, clip_a, prim, index, state and MOTION_V_UV_SCROLL;
 * the other values are the journal's. */
typedef struct {
    float    clip_b[4];             /* B's clip position (through mode: screen x, y, z and 1) */
    float    clip_a[4];             /* the same PRIM position in A; clip_b when not matched */
    uint32_t prim;                  /* serial of B's PRIM */
    uint16_t index;                 /* the vertex's PRIM position */
    uint8_t  state;                 /* MOTION_V_UNMATCHED, _MATCHED, _CLIPPED or _THROUGH_SUFFIX */
    uint8_t  flags;                 /* MOTION_V_TEX_CHANGED, _SAMPLES_CHANGED_TARGET, _UV_SCROLL */
} MotionVert;

enum { MOTION_V_UNMATCHED = 0, MOTION_V_MATCHED, MOTION_V_CLIPPED, MOTION_V_THROUGH_SUFFIX };
#define MOTION_V_TEX_CHANGED            0x01u
#define MOTION_V_SAMPLES_CHANGED_TARGET 0x02u
#define MOTION_V_UV_SCROLL              0x04u   /* the raw UV differs between A and B: B's is used */

/* MotionBuildResult.flags, the names agreed for a frame. */
#define MOTION_F_CUT            0x01u   /* held by the matcher's cut: show A, then B */
#define MOTION_F_NOT_REPLAYABLE 0x02u   /* any other reason */

/* MotionBuildResult.why */
#define MOTION_WHY_NO_PREVIOUS 0x0001u  /* no frame A */
#define MOTION_WHY_CUT         0x0002u  /* the matcher's cut flags, its coverage rule included */
#define MOTION_WHY_FAILED      0x0004u  /* the matcher failed, an argument is invalid, or memory ran out */
#define MOTION_WHY_OVERFLOW    0x0008u  /* A's or B's store is incomplete */
#define MOTION_WHY_UNKNOWN     0x0010u  /* a point or line PRIM or an unnamed raster call in B (each point
                                         * or line counts as both), or a record missing in A */
#define MOTION_WHY_NOT_PLAIN   0x0020u  /* a moving triangle or sprite not drawn whole in both frames */
#define MOTION_WHY_NEAR_CLIP   0x0040u  /* a moving triangle the near plane cut in A or B */
#define MOTION_WHY_STATE       0x0080u  /* a moving one whose projection state differs between A and B */
#define MOTION_WHY_T1_MISMATCH 0x0100u  /* motion_build_check: t = 1 did not give B's values */
#define MOTION_WHY_BAD_CALL    0x0200u  /* A's or B's store was given a call that breaks the tap's contract */
#define MOTION_WHY_LATE        0x0400u  /* A's or B's store was called after its close */

typedef struct {
    uint32_t flags;                 /* MOTION_F_* */
    uint32_t why;                   /* MOTION_WHY_*: every reason found; 0 when the frame can be built */
    uint32_t match_flags;           /* the matcher's flags as given */
    uint32_t draws;                 /* B's PRIMs that drew something (the matcher's list) */
    uint32_t paired;                /* of them, paired with a PRIM of A after the gates */
    uint32_t paired_undrawn;        /* PRIMs of B paired by key because one of the two drew nothing */
    uint32_t unpaired;              /* of them, drawn as in B (motion_build_unpaired lists them) */
    uint32_t unpaired_index;        /*   pairs refused because the index bytes differ or were not seen */
    uint32_t unpaired_shape;        /*   pairs refused because type, mode, vertex count or vertex type differ */
    uint32_t built;                 /* triangles and sprites that get interpolated values */
    uint32_t copied;                /* those drawn with B's own vertices: nothing moved, or the PRIM is unpaired */
    uint32_t uv_changed;            /* vertices of built ones whose raw UV differs between A and B */
    uint32_t not_plain, near_clip, state, unknown;   /* primitives or PRIMs behind each reason */
    uint32_t dropped_both;          /* of not_plain: moving triangles and sprites ge.c dropped in both frames */
    uint32_t t1_mismatch;           /* motion_build_check failures */
} MotionBuildResult;

typedef struct MotionBuild MotionBuild;

MotionBuild *motion_build_create(void);
void motion_build_free(MotionBuild *b);

/* Plans the frame between A and B. pair_a and match are motion_match's outputs for the two
 * frames' lists (motion_tap_match_frame); fa may be NULL when there is no frame A. The frames
 * must stay as they are while the plan is used. Returns 1 when the frame can be built (why is
 * 0), else 0; the result holds the counts either way. */
int motion_build_plan(MotionBuild *b, const MotionTapFrame *fa, const MotionTapFrame *fb,
                      const int32_t *pair_a, const MotionMatchResult *match);
const MotionBuildResult *motion_build_result(const MotionBuild *b);
/* Serials of B's PRIMs drawn as in B because they have no pair, in draw order. */
const uint32_t *motion_build_unpaired(const MotionBuild *b, uint32_t *n);

enum {
    MOTION_ITEM_NONE = 0,           /* no values: the frame has a reason, B did not draw it, or a bad argument */
    MOTION_ITEM_COPY_B,             /* draw B's own vertices */
    MOTION_ITEM_BUILT               /* out[] holds the vertices at t */
};

/* The triangle or sprite with record index `tri` in B, at t in [0, 1]. A triangle gives three
 * vertices in submission order; a sprite gives two, its floored rectangle's low and high
 * corner (the backend builds its six vertices from them and keeps B's texture coordinates).
 * mv (may be NULL) receives one MotionVert per source vertex, three or two, whenever the
 * return is not MOTION_ITEM_NONE. t = 1 takes the same path as any t. */
int motion_build_prim(const MotionBuild *b, uint32_t tri, float t, MotionBuiltVert *out, MotionVert *mv);

/* The builder's own guard, to run on every frame: `submitted` holds the values B handed over
 * for record `tri`, in motion_build_prim's layout (a sprite's u and v are not compared). It
 * builds the record at t = 1 and compares bit for bit. Returns 1 when they are equal, or when
 * the record gets no built values; else 0, and the plan gains MOTION_WHY_T1_MISMATCH, so it
 * gives no values from then on. */
int motion_build_check(MotionBuild *b, uint32_t tri, const MotionBuiltVert *submitted);

/* The backend's vertex rule (ge_gpu.c's put_vert) for what ge.c handed one raster call, in
 * motion_build_prim's layout: n = 3, a triangle (flat: every vertex takes the last one's
 * colour); n = 2, a sprite's floored rectangle with the second corner's depth, fog and colour.
 * This is what motion_build_check expects as `submitted`. Other n write nothing. */
void motion_backend_vertices(int flat, const GeVtx *g, int n, MotionBuiltVert *out);

#ifdef __cplusplus
}
#endif

#endif /* SR_MOTION_TAP_H */
