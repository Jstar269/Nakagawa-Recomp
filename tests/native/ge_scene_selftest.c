// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// ge-scene-selftest: the GE scene record (src/rt/ge_scene.h, #1330 phase 1) against the
// production GE object.
//
// Source-owned display lists run through ge_run_list() with a GPU backend stand-in that records
// every screen-space triangle and sprite ge.c hands it (the real ge_set_gpu_hooks() seam). The
// lists cover a static triangle list, an indexed strip, a 4-weight skinned mesh, a 2-target
// morph, a lit draw with one directional light, a draw into a second target and a through-mode
// sprite. Four checks:
//
// 1. The dump. SR_GESCENE=0 with SR_GESCENE_DIR=<argv[1]> is set before the first PRIM, so ge.c
//    registers its dump; the file is read back and every record's identity and vertices are
//    compared with what the record consumer below saw.
// 2. Record and draw agree. For every transform-mode triangle the stand-in received, the three
//    vertices equal, bit for bit, ge_scene_project_for_test() of the record's vertex references
//    in the order draw_prim forms that triangle. So a consumer that rebuilds the scene from the
//    record (model-space vertices, the matrices and lights in `ge`) gets exactly the geometry the
//    renderer drew. Stored vertices, weights, morph results, spans and identities are checked
//    against the values written.
// 3. No effect. The same lists with no consumer give the stand-in the same submissions, byte for
//    byte, as with the consumer registered.
// 4. The dump's outcomes: frame numbers that are not plain digits and an empty folder are
//    CONFIG_FAIL, a folder that does not exist is WRITE_FAIL, and a frame that never comes, passes
//    with nothing drawn, or is cut short by the end of the run is NOT_CAPTURED.
//
// No game input. Runtime entry points a GE list must not reach abort.

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* setenv/unsetenv under a strict -std */
#endif

#include "recomp.h"
#include "debug.h"
#include "ge_shared.h"
#include "ge_scene.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ge.c: the GE frame number, as the scheduler hands it each vblank. */
extern void ge_set_frame(uint32_t frame);

extern uint32_t ge_run_list(uint32_t addr, int resume);

/* ---- Runtime state at its disabled values, and entry points that must stay cold ---- */

uint8_t *g_mem;
CpuState *s_cpu;
uint32_t g_sr_debug;
int g_sr_heap_watch;
int g_sr_metadata_watch;
int g_sr_last_writer_enabled;
int g_sr_mem_watch_count;
SrMemWatch g_sr_mem_watches[SR_MAX_MEM_WATCHES];
uint32_t g_sr_store_context_pc;
unsigned g_sr_store_context_count;
unsigned g_sr_store_context_limit;
uint32_t g_sr_mem_watch_context_pc;
unsigned g_sr_mem_watch_context_limit;
unsigned g_sr_mem_watch_context_count;
int g_sr_mem_watch_context_fpr = -1;
uint32_t g_sr_mem_watch_context_fpr_value;
int g_sr_store_context_mem_gpr = -1;
uint32_t g_sr_store_context_mem_offset;
unsigned g_sr_store_context_mem_words;

static void unexpected(const char *what, uint32_t addr) {
    fprintf(stderr, "ge_scene_selftest: unexpected runtime call %s addr=0x%08" PRIx32 "\n", what, addr);
    fflush(stderr);
    abort();
}

void sr_oor(uint32_t a, uint32_t v, int store) { (void)v; (void)store; unexpected("sr_oor", a); }
void sr_heap_note_write(uint32_t addr, uint32_t width, uint32_t value, uint32_t pc) {
    (void)width; (void)value; (void)pc;
    unexpected("sr_heap_note_write", addr);
}
void sr_heap_note_bulk_write(uint32_t addr, uint32_t width, uint32_t pc) {
    (void)width; (void)pc;
    unexpected("sr_heap_note_bulk_write", addr);
}
void sr_note_mem_write(uint32_t addr, uint32_t width, uint32_t value, uint32_t pc) {
    (void)width; (void)value; (void)pc;
    unexpected("sr_note_mem_write", addr);
}
int sr_find_last_writer(uint32_t addr, uint32_t width, uint32_t *write_addr,
                        uint32_t *write_width, uint32_t *value, uint32_t *pc) {
    (void)width; (void)write_addr; (void)write_width; (void)value; (void)pc;
    unexpected("sr_find_last_writer", addr);
    return 0;
}
void sr_add_mem_watch(uint32_t start, uint32_t end, const char *label) {
    (void)end; (void)label;
    unexpected("sr_add_mem_watch", start);
}
uint32_t sr_get_ge_status(void) {
    unexpected("sr_get_ge_status", 0u);
    return 0u;
}

static uint64_t s_clock_ns = 1000000u;
uint64_t SDL_GetTicksNS(void) {
    s_clock_ns += 7u;
    return s_clock_ns;
}

static void env_set(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

static int s_failures;
static void expect(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "ge_scene_selftest: FAIL %s\n", what);
        s_failures++;
    }
}

/* ---- Source-owned display lists ---- */

#define LIST_MAIN 0x08100000u
#define VERTS     0x08200000u
#define INDICES   0x08280000u

/* col 8888, float normal, float position: 28 bytes. */
#define VT_PLAIN   ((7u << 2) | (3u << 5) | (3u << 7))
#define VT_THROUGH (VT_PLAIN | (1u << 23))
#define VT_INDEX16 (2u << 11)
/* 4 float weights first: 16 + 28 = 44 bytes. */
#define VT_SKIN4   (VT_PLAIN | (3u << 9) | (3u << 14))
/* 2 morph targets of the plain vertex: 56 bytes. */
#define VT_MORPH2  (VT_PLAIN | (1u << 18))

static uint32_t s_pc;
static uint32_t s_vtx = VERTS;

static void W(uint32_t cmd, uint32_t data) {
    uint32_t word = (cmd << 24) | (data & 0xFFFFFFu);
    memcpy(SR_HOST(s_pc), &word, 4);
    s_pc += 4;
}

static uint32_t f24(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u >> 8;
}

static void put_plain(uint32_t addr, float x, float y, float z, uint32_t color) {
    const float normal[3] = { 0.0f, 0.0f, 1.0f };
    const float pos[3] = { x, y, z };
    memcpy(SR_HOST(addr), &color, 4);
    memcpy(SR_HOST(addr + 4u), normal, sizeof normal);
    memcpy(SR_HOST(addr + 16u), pos, sizeof pos);
}

static uint32_t vtx(float x, float y, float z) {
    uint32_t addr = s_vtx;
    put_plain(addr, x, y, z, 0xFF80C040u);
    s_vtx += 28u;
    return addr;
}

static uint32_t skin_vtx(float x, float y, float z, const float w[4]) {
    uint32_t addr = s_vtx;
    memcpy(SR_HOST(addr), w, 16);
    put_plain(addr + 16u, x, y, z, 0xFF2060A0u);
    s_vtx += 44u;
    return addr;
}

static uint32_t morph_vtx(float x0, float y0, float x1, float y1, float z) {
    uint32_t addr = s_vtx;
    put_plain(addr, x0, y0, z, 0xFF0000FFu);
    put_plain(addr + 28u, x1, y1, z, 0xFF00FF00u);
    s_vtx += 56u;
    return addr;
}

/* The expected identity of each PRIM, recorded as the list is written. */
typedef struct {
    uint32_t cmd_addr, type, count, vtype, through, weights, morphs, index_format;
    uint32_t vertex_addr, vertex_stride, vertex_span, index_addr, index_bytes;
} ExpectPrim;
#define MAX_PRIMS 16
static ExpectPrim s_expect[MAX_PRIMS];
static unsigned s_nexpect;

static void prim(uint32_t vtype, uint32_t type, uint32_t count, uint32_t vaddr, uint32_t stride,
                 uint32_t weights, uint32_t morphs, uint32_t iaddr, uint32_t span) {
    W(0x12, vtype);
    if (iaddr) W(0x02, iaddr);
    W(0x01, vaddr);
    ExpectPrim *e = &s_expect[s_nexpect++];
    memset(e, 0, sizeof(*e));
    e->cmd_addr = s_pc;
    e->type = type;
    e->count = count;
    e->vtype = vtype;
    e->through = (vtype >> 23) & 1u;
    e->weights = weights;
    e->morphs = morphs;
    e->index_format = (vtype >> 11) & 3u;
    e->vertex_addr = vaddr;
    e->vertex_stride = stride;
    e->vertex_span = span;
    e->index_addr = iaddr;
    e->index_bytes = iaddr ? count << (e->index_format - 1u) : 0u;
    W(0x04, (type << 16) | count);
}

static void matrix(uint32_t select, uint32_t data, const float *m, int n) {
    W(select, 0);
    for (int i = 0; i < n; i++) W(data, f24(m[i]));
}

#define FRONT (-5.0f)

static const float s_skin_w[4][4] = {
    { 1.0f, 0.0f, 0.0f, 0.0f },
    { 0.5f, 0.5f, 0.0f, 0.0f },
    { 0.25f, 0.25f, 0.25f, 0.25f },
};

static void build_list(void) {
    static const float identity43[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
    static const float world43[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0.125f, -0.0625f, 0 };
    static const float perspective[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, -101.0f / 99.0f, -1,
        0, 0, -200.0f / 99.0f, 0,
    };
    /* Bones: four translations. */
    static const float bones[4][12] = {
        { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0.0f, 0.0f, 0.0f },
        { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0.25f, 0.0f, 0.0f },
        { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0.0f, 0.25f, 0.0f },
        { 1, 0, 0, 0, 1, 0, 0, 0, 1, -0.25f, -0.25f, 0.0f },
    };

    uint32_t tris = vtx(-0.4f, -0.4f, FRONT);
    vtx(0.4f, -0.4f, FRONT); vtx(0.0f, 0.4f, FRONT);
    vtx(-0.3f, 0.1f, -6.0f); vtx(0.2f, 0.3f, -7.0f); vtx(0.1f, -0.2f, -4.0f);
    static const uint16_t idx16[6] = { 0, 1, 2, 4, 3, 5 };
    memcpy(SR_HOST(INDICES), idx16, sizeof idx16);
    uint32_t skin = skin_vtx(-0.3f, -0.3f, FRONT, s_skin_w[0]);
    skin_vtx(0.3f, -0.3f, FRONT, s_skin_w[1]);
    skin_vtx(0.0f, 0.3f, FRONT, s_skin_w[2]);
    uint32_t morph = morph_vtx(-0.25f, -0.25f, -0.75f, -0.75f, FRONT);
    morph_vtx(0.25f, -0.25f, 0.75f, -0.75f, FRONT);
    morph_vtx(0.25f, 0.25f, 0.75f, 0.75f, FRONT);
    uint32_t sprite = vtx(10.0f, 12.0f, 0.0f);
    vtx(40.0f, 30.0f, 0.0f);

    s_pc = LIST_MAIN;
    W(0x10, 0x080000);              /* BASE */
    W(0x13, 0);                     /* OFFSETADDR */
    W(0x9C, 0x044000);              /* FRAMEBUFPTR -> 0x04044000 */
    W(0x9D, 512);
    W(0xD2, 3);                     /* PIXFORMAT 8888 */
    W(0xD4, 0);
    W(0xD5, (271u << 10) | 479u);
    W(0x1E, 0);                     /* no texture */
    W(0x23, 0);                     /* no depth test */
    W(0x1D, 0);                     /* no culling: triangles reach the backend in list order */
    W(0x1C, 1);                     /* CLIPENABLE */
    W(0x17, 0);                     /* no lighting */
    W(0x42, f24(240.0f)); W(0x43, f24(-136.0f)); W(0x44, f24(1000.0f));
    W(0x45, f24(240.0f)); W(0x46, f24(136.0f)); W(0x47, f24(30000.0f));
    W(0x4C, 0); W(0x4D, 0);
    matrix(0x3A, 0x3B, world43, 12);
    matrix(0x3C, 0x3D, identity43, 12);
    matrix(0x3E, 0x3F, perspective, 16);
    W(0x2A, 0);                     /* BONEMATRIXNUMBER 0 */
    for (int b = 0; b < 4; b++)
        for (int k = 0; k < 12; k++) W(0x2B, f24(bones[b][k]));
    W(0x2C, f24(0.5f)); W(0x2D, f24(0.5f));    /* MORPHWEIGHT0/1 */

    prim(VT_PLAIN, 3, 6, tris, 28u, 0u, 1u, 0u, 6u);                  /* static list */
    prim(VT_PLAIN | VT_INDEX16, 4, 6, tris, 28u, 0u, 1u, INDICES, 6u); /* indexed strip */
    prim(VT_SKIN4, 3, 3, skin, 44u, 4u, 1u, 0u, 3u);                  /* skinned */
    prim(VT_MORPH2, 5, 3, morph, 56u, 0u, 2u, 0u, 3u);                /* morphed fan */
    /* Lit: one directional light, ambient and diffuse colour. */
    W(0x17, 1);
    W(0x5C, 0x202020); W(0x5D, 0xFF);           /* AMBIENTCOLOR, AMBIENTALPHA */
    W(0x53, 0);                                  /* MATERIALUPDATE: material colours */
    W(0x55, 0x808080); W(0x56, 0xC0C0C0);        /* MATERIALAMBIENT, MATERIALDIFFUSE */
    W(0x18, 1);                                  /* LIGHTENABLE0 */
    W(0x5F, 0);                                  /* LIGHTTYPE0: directional, diffuse */
    W(0x63, f24(0.0f)); W(0x64, f24(0.0f)); W(0x65, f24(1.0f));
    W(0x8F, 0x101010); W(0x90, 0xFFE0C0);        /* LAC0, LDC0 */
    prim(VT_PLAIN, 3, 3, tris + 3u * 28u, 28u, 0u, 1u, 0u, 3u);
    W(0x17, 0);
    /* A second target. */
    W(0x9C, 0x088000);
    prim(VT_PLAIN, 3, 3, tris, 28u, 0u, 1u, 0u, 3u);
    W(0x9C, 0x044000);
    prim(VT_THROUGH, 6, 2, sprite, 28u, 0u, 1u, 0u, 2u);             /* through-mode sprite */
    W(0x0F, 0);                     /* FINISH */
    W(0x0C, 0);                     /* END */
}

/* ---- The GPU backend stand-in: every submission, in order ---- */

typedef struct { int kind; GeVtx v[3]; int persp; } Sub;
#define MAX_SUBS 64
static Sub s_subs[MAX_SUBS];
static unsigned s_nsubs;

static void sub_put(int kind, const GeVtx *a, const GeVtx *b, const GeVtx *c, int persp) {
    if (s_nsubs >= MAX_SUBS) { expect(0, "submission log overflow"); return; }
    Sub *s = &s_subs[s_nsubs++];
    memset(s, 0, sizeof(*s));
    s->kind = kind;
    s->persp = persp;
    if (a) s->v[0] = *a;
    if (b) s->v[1] = *b;
    if (c) s->v[2] = *c;
}
static int hook_tri(const GeVtx *a, const GeVtx *b, const GeVtx *c, int persp) {
    sub_put(3, a, b, c, persp);
    return 1;
}
static int hook_sprite(const GeVtx *a, const GeVtx *b, int persp) {
    sub_put(6, a, b, NULL, persp);
    return 1;
}
static int hook_line(const GeVtx *a, const GeVtx *b, int persp) {
    sub_put(1, a, b, NULL, persp);
    return 1;
}
static int hook_point(const GeVtx *a, int persp) {
    sub_put(0, a, NULL, NULL, persp);
    return 1;
}
static void hook_flush(const char *reason) { (void)reason; }
static void hook_vram_dirty(uint32_t addr, uint32_t bytes) { (void)addr; (void)bytes; }

static void hooks_on(void) {
    static GeGpuHooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.tri = hook_tri;
    hooks.sprite = hook_sprite;
    hooks.line = hook_line;
    hooks.point = hook_point;
    hooks.flush = hook_flush;
    hooks.vram_dirty = hook_vram_dirty;
    ge_set_gpu_hooks(&hooks);
}

/* ---- The consumer: a copy of each record, its vertices and their projections ---- */

#define MAX_VERTS 8
typedef struct {
    GeSceneRecord rec;
    uint32_t fbp;
    int lit;
    GeSceneVertex v[MAX_VERTS];
    int vok[MAX_VERTS];
    GeVtx p[MAX_VERTS];
    int pok[MAX_VERTS];
} Seen;
static Seen s_seen[MAX_PRIMS];
static unsigned s_nseen;

static void consumer(const GeSceneRecord *rec, void *user) {
    expect(user == (void *)&s_seen, "the consumer gets its user pointer");
    if (s_nseen >= MAX_PRIMS) { expect(0, "record log overflow"); return; }
    Seen *s = &s_seen[s_nseen++];
    memset(s, 0, sizeof(*s));
    s->rec = *rec;
    s->rec.ge = NULL;
    s->fbp = rec->ge->fbp;
    s->lit = rec->ge->lighting_enable;
    for (uint32_t i = 0; i < rec->count && i < MAX_VERTS; i++) {
        s->vok[i] = ge_scene_vertex(rec, i, &s->v[i]);
        s->pok[i] = ge_scene_project_for_test(rec, i, &s->p[i]);
    }
    GeSceneVertex junk;
    expect(!ge_scene_vertex(rec, rec->count, &junk), "a reference past count is refused");
    if (rec->index_format) {
        /* The same indexed PRIM with its vertices, then its indices, ending past guest memory: the
         * reference is refused and left zeroed (the dump writes such a vertex as zeros). */
        static const GeSceneVertex zero;
        GeSceneRecord past = *rec;
        past.vertex_addr = 0x0BFFFFFFu;
        expect(!ge_scene_vertex(&past, 0, &junk) && memcmp(&junk, &zero, sizeof zero) == 0,
               "a vertex past guest memory is refused and zeroed");
        past = *rec;
        past.index_addr = 0x0BFFFFFFu;
        expect(!ge_scene_vertex(&past, 0, &junk) && memcmp(&junk, &zero, sizeof zero) == 0,
               "indices past guest memory are refused and zeroed");
    }
}

/* ---- Checks ---- */

static void check_identities(void) {
    expect(s_nseen == s_nexpect, "one record per PRIM command");
    for (unsigned k = 0; k < s_nseen && k < s_nexpect; k++) {
        const GeSceneRecord *r = &s_seen[k].rec;
        const ExpectPrim *e = &s_expect[k];
        char what[96];
        snprintf(what, sizeof what, "record %u identity", k);
        expect(r->version == GE_SCENE_RECORD_VERSION && r->size == sizeof(GeSceneRecord), what);
        expect(r->list_addr == LIST_MAIN && r->cmd_addr == e->cmd_addr, what);
        expect(k == 0 || r->prim_index == s_seen[k - 1].rec.prim_index + 1u, what);
        expect(r->type == e->type && r->count == e->count && r->vtype == e->vtype, what);
        expect(r->through == e->through && r->weights == e->weights && r->morphs == e->morphs, what);
        expect(r->index_format == e->index_format && r->vertex_addr == e->vertex_addr, what);
        expect(r->vertex_stride == e->vertex_stride && r->vertex_span == e->vertex_span, what);
        expect(r->index_addr == e->index_addr && r->index_bytes == e->index_bytes, what);
        for (uint32_t i = 0; i < r->count && i < MAX_VERTS; i++) expect(s_seen[k].vok[i], what);
    }
    if (s_nseen < 7) return;
    /* Stored values: the static list's first vertex, the index applied, skin weights before
     * skinning, the morph blend, a second target, the through-mode sprite's screen corner. */
    expect(s_seen[0].v[0].pos[0] == -0.4f && s_seen[0].v[0].pos[2] == FRONT &&
           s_seen[0].v[0].rgba == 0xFF80C040u && s_seen[0].v[0].nrm[2] == 1.0f, "stored vertex");
    expect(s_seen[1].v[3].pos[0] == 0.2f && s_seen[1].v[4].pos[0] == -0.3f, "indices applied");
    expect(s_seen[2].v[0].pos[0] == -0.3f, "skinned vertex is stored before skinning");
    for (int i = 0; i < 3; i++)
        expect(memcmp(s_seen[2].v[i].weight, s_skin_w[i], sizeof s_skin_w[i]) == 0, "skin weights");
    expect(s_seen[3].v[0].pos[0] == -0.5f && s_seen[3].v[2].pos[1] == 0.5f, "morph targets blended");
    expect(s_seen[4].lit && !s_seen[0].lit, "the lit draw's state");
    expect(s_seen[5].fbp != s_seen[0].fbp, "the second target's state");
    expect(s_seen[6].rec.through &&s_seen[6].v[1].pos[0] == 40.0f && !s_seen[6].pok[0],
           "through-mode sprite: screen coordinates, no projection");
}

/* draw_prim's triangle order for one record: list, strip or fan. */
static int tri_refs(const GeSceneRecord *r, int t, int ref[3]) {
    int n = r->type == 3 ? (int)r->count / 3 : (int)r->count - 2;
    if (t >= n) return 0;
    if (r->type == 3) { ref[0] = 3 * t; ref[1] = 3 * t + 1; ref[2] = 3 * t + 2; }
    else if (r->type == 4) { ref[0] = t; ref[1] = t + 1; ref[2] = t + 2; }
    else { ref[0] = 0; ref[1] = t + 1; ref[2] = t + 2; }
    return 1;
}

static void check_draws_match(void) {
    unsigned sub = 0, matched = 0;
    for (unsigned k = 0; k < s_nseen; k++) {
        const Seen *s = &s_seen[k];
        if (s->rec.through) {
            expect(sub < s_nsubs && s_subs[sub].kind == 6, "the sprite reached the backend");
            sub++;
            continue;
        }
        int ref[3];
        for (int t = 0; tri_refs(&s->rec, t, ref); t++, sub++) {
            char what[96];
            snprintf(what, sizeof what, "record %u triangle %d matches the drawn triangle", k, t);
            if (sub >= s_nsubs || s_subs[sub].kind != 3) { expect(0, what); continue; }
            int ok = 1;
            for (int j = 0; j < 3; j++)
                ok &= s->pok[ref[j]] && memcmp(&s->p[ref[j]], &s_subs[sub].v[j], sizeof(GeVtx)) == 0;
            expect(ok, what);
            matched += (unsigned)ok;
        }
    }
    expect(sub == s_nsubs, "every submission is accounted for by a record");
    expect(matched == 2u + 4u + 1u + 1u + 1u + 1u, "every transform-mode triangle matched");
}

/* ---- The dump, read back ---- */

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* 314 words: magic and size 2, identity 17, transforms 162, targets 5, texture 20, output 25,
 * lighting 83. */
#define DUMP_FIXED 1256u

static int file_exists(const char *dir, const char *name) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* Re-arm the dump, set the two variables, run the list once at the current GE frame, and return
 * the dump's state. */
static int probe_run(const char *frame, const char *dir) {
    ge_scene_dump_rearm_for_test();
    env_set("SR_GESCENE", frame);
    env_set("SR_GESCENE_DIR", dir);
    s_nsubs = 0;
    expect(ge_run_list(LIST_MAIN, 0) == 0, "probe run reaches END");
    return ge_scene_dump_state_for_test();
}

/* 4. Settings the dump refuses, and the ways it ends without the whole frame. Runs at GE frame 1. */
static void check_dump_outcomes(const char *dir) {
    static const char *const bad[] = { "-1", "+1", " 1", "1x", "0x1", "4294967296",
                                       "99999999999999999999999" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char what[96];
        snprintf(what, sizeof what, "SR_GESCENE=\"%s\" is CONFIG_FAIL", bad[i]);
        expect(probe_run(bad[i], dir) == GE_SCENE_DUMP_CONFIG_FAIL, what);
    }
    expect(!file_exists(dir, "gescene_f4294967295.bin"), "a refused frame number writes nothing");
    expect(probe_run("1", "") == GE_SCENE_DUMP_CONFIG_FAIL, "an empty SR_GESCENE_DIR is CONFIG_FAIL");

    char missing[1024];
    snprintf(missing, sizeof missing, "%s/no-such-folder", dir);
    expect(probe_run("1", missing) == GE_SCENE_DUMP_WRITE_FAIL, "a folder that does not exist is WRITE_FAIL");

    /* The frame never comes: NOT_CAPTURED when the run ends, and no file. */
    expect(probe_run("99", dir) == GE_SCENE_DUMP_WAITING, "a later frame waits");
    ge_scene_dump_end_for_test();
    expect(ge_scene_dump_state_for_test() == GE_SCENE_DUMP_NOT_CAPTURED, "a frame that never came is NOT_CAPTURED");
    expect(!file_exists(dir, "gescene_f99.bin"), "a frame that never came writes no file");

    /* The frame passes with no PRIM drawn while the dump waits. */
    expect(probe_run("2", dir) == GE_SCENE_DUMP_WAITING, "frame 2 waits");
    ge_set_frame(3);
    expect(ge_scene_dump_state_for_test() == GE_SCENE_DUMP_NOT_CAPTURED, "a frame that drew nothing is NOT_CAPTURED");

    /* The run ends during the frame: NOT_CAPTURED, the records so far kept. Runs at frame 3. */
    expect(probe_run("3", dir) == GE_SCENE_DUMP_WRITING, "the current frame is written");
    ge_scene_dump_end_for_test();
    expect(ge_scene_dump_state_for_test() == GE_SCENE_DUMP_NOT_CAPTURED, "a frame the run cut short is NOT_CAPTURED");
    expect(file_exists(dir, "gescene_f3.bin"), "a cut-short frame keeps its file");

    ge_scene_dump_rearm_for_test();
    env_set("SR_GESCENE", NULL);
    env_set("SR_GESCENE_DIR", NULL);
}

static void check_dump(const char *dir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/gescene_f0.bin", dir);
    FILE *f = fopen(path, "rb");
    expect(f != NULL, "the dump file exists");
    if (!f) return;
    static uint8_t buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    expect(n >= 16 && memcmp(buf, "NKGESCN1", 8) == 0 && rd32(buf + 8) == 1u && rd32(buf + 12) == 0u,
           "dump header");
    size_t at = 16;
    unsigned k = 0;
    /* Offsets in a record: magic 0, bytes 4, frame 8, list 12, cmd 16, prim_index 20, type 28,
     * count 32, vtype 36. The fixed part (header, identity and state, ge_scene.h) is DUMP_FIXED
     * bytes; the targets block starts at byte 724 (fbp), lighting at 924 (lighting_enable). The
     * vertex block is the last 4 + count * 68 bytes (17 words a vertex). */
    while (at + 8 <= n && k < MAX_PRIMS) {
        const uint8_t *r = buf + at;
        uint32_t bytes = rd32(r + 4);
        char what[96];
        snprintf(what, sizeof what, "dump record %u", k);
        if (rd32(r) != 0x4D495250u || bytes < 40u || at + bytes > n) { expect(0, what); return; }
        const Seen *s = &s_seen[k];
        uint64_t prim_index;
        memcpy(&prim_index, r + 20, 8);
        expect(rd32(r + 8) == 0u && prim_index == s->rec.prim_index, what);
        expect(bytes == DUMP_FIXED + 4u + s->rec.count * 68u, what);
        expect(rd32(r + 724) == s->fbp && rd32(r + 924) == (uint32_t)s->lit, what);
        expect(rd32(r + 12) == s->rec.list_addr && rd32(r + 16) == s->rec.cmd_addr, what);
        expect(rd32(r + 28) == s->rec.type && rd32(r + 32) == s->rec.count &&
               rd32(r + 36) == s->rec.vtype, what);
        uint32_t nv = s->rec.count;
        size_t vblock = 4u + (size_t)nv * 68u;
        expect(bytes > vblock && rd32(r + bytes - vblock) == nv, what);
        for (uint32_t i = 0; i < nv && i < MAX_VERTS; i++) {
            const uint8_t *v = r + bytes - vblock + 4u + (size_t)i * 68u;
            expect(memcmp(v, s->v[i].pos, 12) == 0 && rd32(v + 32) == s->v[i].rgba &&
                   memcmp(v + 36, s->v[i].weight, 32) == 0, what);
        }
        at += bytes;
        k++;
    }
    expect(at == n && k == s_nexpect, "the dump holds every record of the frame and nothing else");
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <existing output directory for the dump>\n", argv[0]);
        return 2;
    }
    uint8_t *arena = (uint8_t *)calloc(0x0c000000u, 1);
    if (!arena) {
        fprintf(stderr, "ge_scene_selftest: no guest arena\n");
        return 2;
    }
    g_mem = arena + 0x08000000u;
    build_list();
    hooks_on();

    /* 1. The dump, registered by ge.c from the environment at the first PRIM. */
    env_set("SR_GESCENE", "0");
    env_set("SR_GESCENE_DIR", argv[1]);
    expect(ge_run_list(LIST_MAIN, 0) == 0, "dump run reaches END");
    expect(ge_scene_dump_state_for_test() == GE_SCENE_DUMP_WRITING, "the dump is writing frame 0");
    ge_set_frame(1);                            /* a later frame: the dump is complete */
    expect(ge_scene_dump_state_for_test() == GE_SCENE_DUMP_DONE, "the dump ends DONE when frame 1 begins");
    ge_scene_set_consumer(NULL, NULL);

    /* 2. The consumer, and the draws it must agree with. */
    s_nsubs = 0;
    ge_scene_set_consumer(consumer, &s_seen);
    expect(ge_run_list(LIST_MAIN, 0) == 0, "consumer run reaches END");
    ge_scene_set_consumer(NULL, NULL);
    unsigned with_n = s_nsubs;
    static Sub with[MAX_SUBS];
    memcpy(with, s_subs, sizeof with);
    check_identities();
    check_draws_match();
    check_dump(argv[1]);

    /* 3. No consumer: the same submissions, byte for byte. */
    s_nsubs = 0;
    expect(ge_run_list(LIST_MAIN, 0) == 0, "plain run reaches END");
    expect(s_nsubs == with_n && memcmp(s_subs, with, sizeof(Sub) * with_n) == 0,
           "a consumer changes nothing ge.c submits");

    check_dump_outcomes(argv[1]);

    ge_set_gpu_hooks(NULL);
    if (s_failures) {
        fprintf(stderr, "ge_scene_selftest: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("ge-scene-selftest: PASS (%u records, %u submissions)\n", s_nseen, with_n);
    return 0;
}
