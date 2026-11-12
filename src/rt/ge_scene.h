// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
/*
 * The GE scene record (#1330 phase 1; shared with #318 and #791).
 *
 * One read-only record per GE PRIM command, in list order, taken in ge.c's draw_prim after the
 * vertex format is decoded and before the through-mode or transform-mode path runs. It carries
 * what a renderer that rebuilds the scene needs and the screen-space hooks of ge_shared.h do not:
 * the PRIM's identity, its raw vertex and index spans, and the GE state it draws with (the WORLD,
 * VIEW and PROJ matrices, the bone and texgen matrices, the lights and material, the texture and
 * CLUT, the fragment state and the targets). Consumers are host-only: an RTX Remix backend, per-eye
 * stereo for VR, GPU transform and lighting, and the one-frame dump below.
 *
 * Rules:
 * - Authentic rendering is unchanged. With no consumer the cost in draw_prim is one predicted
 *   branch, and with one the CPU transform and every existing hook run exactly as before. A
 *   consumer must not write guest memory or the GE state (the record is const, and `ge` points at
 *   the live state only for the duration of the call).
 * - Order. Records arrive in list order, one per PRIM command, before that PRIM is drawn.
 *   Bezier and spline patches (draw_patch) and immediate-mode vertices (0xF0-0xF9) are not PRIM
 *   commands and have no record in version 1.
 * - Versioning. `version` and `size` let a consumer refuse a record it does not know. The dump
 *   format below is the stable contract: fields are only ever appended to it.
 *
 * Model-space vertices are decoded on request (ge_scene_vertex), with ge.c's own readers, so a
 * consumer that only needs identities pays nothing for vertices.
 */
#ifndef GE_SCENE_H
#define GE_SCENE_H

#include <stdint.h>

#include "ge_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GE_SCENE_RECORD_VERSION 1u
#define GE_SCENE_MAX_WEIGHTS 8

typedef struct GeSceneRecord {
    uint32_t version;          /* GE_SCENE_RECORD_VERSION */
    uint32_t size;             /* sizeof(GeSceneRecord) in the build that filled it */
    uint32_t frame;            /* the GE frame number (ge_set_frame) */
    uint32_t list_addr;        /* the display list the PRIM belongs to */
    uint32_t cmd_addr;         /* the PRIM command's own address */
    uint64_t prim_index;       /* PRIM commands executed before this one, in list order */
    uint32_t type;             /* 0 points, 1 lines, 2 line strip, 3 triangles, 4 strip, 5 fan, 6 sprites */
    uint32_t count;            /* vertex references */
    uint32_t vtype;            /* VERTEXTYPE as set */
    uint32_t through;          /* 1: through mode (screen coordinates, no transform) */
    uint32_t weights;          /* skinning weights per vertex, 0..8 */
    uint32_t morphs;           /* morph targets, 1..8 */
    uint32_t index_format;     /* 0 none, 1 u8, 2 u16, 3 u32 */
    uint32_t vertex_addr;      /* guest address of vertex 0 (BASE applied) */
    uint32_t vertex_stride;    /* bytes per vertex, morph targets included */
    uint32_t vertex_span;      /* vertices from vertex_addr the PRIM reads: count, or the largest
                                * index + 1 (0xFFFFFFFF for index 0xFFFFFFFF, which saturates) */
    uint32_t index_addr;       /* guest address of the first index; 0 without indices */
    uint32_t index_bytes;      /* bytes of indices the PRIM reads; 0 without indices */
    const GeState *ge;         /* the state the PRIM draws with; valid only during the callback */
} GeSceneRecord;

/* One vertex reference, after morphing and before skinning, read by ge.c's own readers. pos, nrm,
 * uv and rgba are blended across the morph targets; weight comes from the first target only, as
 * the CPU path reads it. nrm is the normal the T&L uses: REVERSENORMAL is already applied, and the
 * record's state (`ge->reverse_normal`, and the dump's reverse_normal field) says whether it was.
 * Through mode keeps the screen coordinates in pos. When the format lacks a field, the value is
 * the one the T&L uses: a missing normal is (0, 0, 1) (negated under REVERSENORMAL), a missing
 * colour is the material colour where one is set and opaque white otherwise, and missing
 * texture coordinates and weights are 0. */
typedef struct GeSceneVertex {
    float pos[3];
    float nrm[3];
    float uv[2];
    uint32_t rgba;             /* r in the low byte */
    float weight[GE_SCENE_MAX_WEIGHTS];
} GeSceneVertex;

typedef void (*GeSceneConsumer)(const GeSceneRecord *rec, void *user);

/* Register the one consumer, or none with NULL. Replacing the SR_GESCENE dump closes its file.
 * Without a call, ge.c reads SR_GESCENE at the first PRIM and registers the dump if it is set. */
void ge_scene_set_consumer(GeSceneConsumer fn, void *user);

/* Decode vertex reference i (0 <= i < rec->count, in PRIM order, the index applied) during the
 * callback. Returns 0, leaving *out zeroed, when i is out of range or the vertex is not wholly
 * inside guest memory. Reads guest memory only. */
int ge_scene_vertex(const GeSceneRecord *rec, uint32_t i, GeSceneVertex *out);

/* Test and diagnostic use: vertex reference i through ge.c's own transform-mode T&L (skinning,
 * transform, lighting, fog) and projection, as the software path computes it for a triangle that
 * needs no clipping. Returns 1 with *out filled, 0 for a through-mode record, an out-of-range i,
 * or a vertex the projection would not accept on its own (non-finite, or clip w at or below the
 * near clipper's limit). Updates no counters that a run's GE output depends on. */
int ge_scene_project_for_test(const GeSceneRecord *rec, uint32_t i, GeVtx *out);

/* End the SR_GESCENE dump as the run ends: an open or waiting dump becomes NOT_CAPTURED. atexit
 * runs it; an exit path that ends the process with _Exit calls it first, beside its other
 * end-of-run reports. Idempotent, and a no-op without a dump. */
void ge_scene_dump_finish(void);

/* Test use: the SR_GESCENE dump's state; the end-of-run step atexit runs (an open dump becomes
 * NOT_CAPTURED); and a re-arm that closes any dump that way and reads SR_GESCENE again at the next
 * PRIM, as at start-up. */
enum {
    GE_SCENE_DUMP_IDLE,         /* SR_GESCENE not read yet, or not set */
    GE_SCENE_DUMP_CONFIG_FAIL,
    GE_SCENE_DUMP_WAITING,      /* set; the frame has drawn nothing yet */
    GE_SCENE_DUMP_WRITING,
    GE_SCENE_DUMP_DONE,
    GE_SCENE_DUMP_WRITE_FAIL,
    GE_SCENE_DUMP_NOT_CAPTURED
};
int ge_scene_dump_state_for_test(void);
void ge_scene_dump_end_for_test(void);
void ge_scene_dump_rearm_for_test(void);

/*
 * The one-frame dump: SR_GESCENE=<GE frame number> with SR_GESCENE_DIR naming an existing
 * directory writes every record of that frame to <dir>/gescene_f<frame>.bin, flushed after each
 * record, and prints `GESCENE: frame <n>: <k> records -> <path>` once a later frame begins. If the
 * run ends, or a consumer replaces the dump, before that, it prints `GESCENE NOT_CAPTURED (...)`
 * instead: a file already started keeps the records written so far but is not the whole frame. A
 * bad setting prints `GESCENE CONFIG_FAIL (...)` and writes nothing; a failed create, write or
 * close prints `GESCENE WRITE_FAIL (...)`, stops the dump and claims nothing more. The file holds
 * decoded game geometry: it is private evidence, like a VRAM dump, and never published.
 *
 * Format, version 1, all little-endian:
 *   file header:  "NKGESCN1" (8 bytes), u32 version (1), u32 frame
 *   each record:  u32 magic 0x4D495250 ("PRIM"), u32 record_bytes (this record, magic included),
 *                 then the fields below in order, then u32 vertex count and that many vertices.
 *   identity:     u32 frame, list_addr, cmd_addr; u64 prim_index; u32 type, count, vtype, through,
 *                 weights, morphs, index_format, vertex_addr, vertex_stride, vertex_span,
 *                 index_addr, index_bytes
 *   transforms:   f32 world[12], view[12], proj[16], bone[96], tgen[12]; u32 viewport_raw[6],
 *                 offsetx, offsety; i32 scissor x1, y1, x2, y2; u32 minz, maxz
 *   targets:      u32 fbp, fbw, fbfmt, zbp, zbw
 *   texture:      u32 tex_enable, tex_addr, tex_bufw, tex_fmt, tex_w, tex_h, tex_swizzle, tex_wrap,
 *                 tex_func, tex_env, tex_filter, tex_map_mode, tex_shade_ls, clut_addr, clut_fmt,
 *                 clut_gen; f32 tex_scale_u, tex_scale_v, tex_off_u, tex_off_v
 *   output:       u32 blend_enable, blend_mode, blend_fixa, blend_fixb, atest_enable, atest,
 *                 ztest_enable, ztest, zwrite_disable, depth_clip, fog_enable; f32 fog_end,
 *                 fog_slope; u32 fog_color, dither_enable, cull_enable, cull, clear, clear_mode,
 *                 shade_gouraud, stencil_enable, color_test_enable, logic_op_enable, mask_rgb,
 *                 mask_alpha
 *   lighting:     u32 lighting_enable, light_mode, reverse_normal, light_on[4], light_type[4];
 *                 f32 light_pos[4][3], light_dir[4][3], light_att[4][3], light_spot_exp[4],
 *                 light_spot_cut[4]; u32 light_acol[4], light_dcol[4], light_scol[4], amb_color,
 *                 amb_alpha, material, mat_emissive, mat_diffuse, mat_specular, mat_update;
 *                 f32 spec_coef, morph_weight[8]
 *   each vertex:  f32 pos[3], nrm[3], uv[2]; u32 rgba; f32 weight[8] (ge_scene_vertex, PRIM order;
 *                 a vertex ge_scene_vertex refuses is written as zeros)
 */

#ifdef __cplusplus
}
#endif

#endif
