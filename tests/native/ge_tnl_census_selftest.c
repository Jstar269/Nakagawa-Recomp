// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// ge-tnl-census-selftest: why ge.c keeps its own transform and lighting for a PRIM it could
// offer to the GPU T&L route (#318), counted per reason (ge_tnl_reason_counts in ge_shared.h).
//
// The production GE object runs one source-owned display list per reason through ge_run_list(),
// with the GPU T&L switch on and a backend stand-in registered through the real ge_set_gpu_hooks()
// seam: its tnl_prim takes every PRIM unless told to decline, and its screen-space hooks take the
// CPU route's triangles. Each list draws one transform-mode triangle in exactly one state the
// route cannot repeat, and the census must count that PRIM under that reason and nowhere else;
// the reasons must sum to ge_tnl_counts' `cpu`, and an eligible PRIM must be taken. Not covered
// here: `diagnostic` (the GE timing model, SR_RTRACE or the primitive profile, each latched once
// per process) and `alloc` (an allocation failure).
//
// No game input, no GPU. Runtime entry points a GE list must not reach abort.

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "recomp.h"
#include "debug.h"
#include "ge_shared.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    fprintf(stderr, "ge_tnl_census_selftest: unexpected runtime call %s addr=0x%08" PRIx32 "\n", what, addr);
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

static int s_failures;
static void expect(int ok, const char *scenario, const char *what) {
    if (!ok) {
        fprintf(stderr, "ge_tnl_census_selftest: FAIL %s: %s\n", scenario, what);
        s_failures++;
    }
}

/* ---- The backend stand-in ---- */

static int s_decline;
static unsigned s_tnl_calls, s_cpu_tris;

static int hook_tnl_prim(const GeTnlPrim *p) {
    (void)p;
    s_tnl_calls++;
    return !s_decline;
}
static int hook_tri(const GeVtx *a, const GeVtx *b, const GeVtx *c, int persp) {
    (void)a; (void)b; (void)c; (void)persp;
    s_cpu_tris++;
    return 1;
}
static int hook_sprite(const GeVtx *a, const GeVtx *b, int persp) { (void)a; (void)b; (void)persp; return 1; }
static int hook_line(const GeVtx *a, const GeVtx *b, int persp) { (void)a; (void)b; (void)persp; return 1; }
static int hook_point(const GeVtx *a, int persp) { (void)a; (void)persp; return 1; }
static void hook_flush(const char *reason) { (void)reason; }
static void hook_vram_dirty(uint32_t addr, uint32_t bytes) { (void)addr; (void)bytes; }

/* ---- Source-owned display lists ---- */

#define LIST  0x08100000u
#define VERTS 0x08200000u

/* col 8888, float normal, float position: 28 bytes. */
#define VT_PLAIN ((7u << 2) | (3u << 5) | (3u << 7))

static uint32_t s_pc;

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

static void put_vtx(uint32_t addr, float x, float y, float z) {
    const uint32_t color = 0xFF80C040u;
    const float normal[3] = { 0.0f, 0.0f, 1.0f };
    const float pos[3] = { x, y, z };
    memcpy(SR_HOST(addr), &color, 4);
    memcpy(SR_HOST(addr + 4u), normal, sizeof normal);
    memcpy(SR_HOST(addr + 16u), pos, sizeof pos);
}

#define BIG 0x1p50f   /* outside the route's 2^-40..2^40 range */

/* Every state a scenario changes, at the value that keeps the route eligible. */
static void base_state(void) {
    static const float identity43[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
    static const float perspective[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, -101.0f / 99.0f, -1,
        0, 0, -200.0f / 99.0f, 0,
    };
    W(0x10, 0x080000);              /* BASE */
    W(0x13, 0);
    W(0x9C, 0x044000); W(0x9D, 512); W(0xD2, 3);
    W(0xD4, 0); W(0xD5, (271u << 10) | 479u);
    W(0x23, 0);                     /* no depth test */
    W(0x1D, 0);                     /* no culling */
    W(0x1C, 1);                     /* CLIPENABLE */
    W(0xD3, 0);                     /* CLEARMODE off */
    W(0x50, 1);                     /* SHADEMODE gouraud */
    W(0x1E, 0); W(0xC0, 0);         /* no texture, UV map mode */
    W(0x48, f24(1.0f)); W(0x49, f24(1.0f)); W(0x4A, 0); W(0x4B, 0);
    W(0x1F, 0);                     /* no fog */
    W(0xCD, f24(1.0f)); W(0xCE, f24(1.0f));
    W(0x17, 0); W(0x18, 0);         /* no lighting, light 0 off */
    W(0x5F, 0);                     /* light 0 directional, diffuse */
    W(0x63, 0); W(0x64, 0); W(0x65, f24(1.0f));
    W(0x42, f24(240.0f)); W(0x43, f24(-136.0f)); W(0x44, f24(1000.0f));
    W(0x45, f24(240.0f)); W(0x46, f24(136.0f)); W(0x47, f24(30000.0f));
    W(0x4C, 0); W(0x4D, 0);
    W(0x3A, 0); for (int i = 0; i < 12; i++) W(0x3B, f24(identity43[i]));
    W(0x3C, 0); for (int i = 0; i < 12; i++) W(0x3D, f24(identity43[i]));
    W(0x3E, 0); for (int i = 0; i < 16; i++) W(0x3F, f24(perspective[i]));
    W(0x12, VT_PLAIN);
}

#define SC_TAKEN (-1)

typedef struct {
    const char *name;
    int why;          /* the reason expected, or SC_TAKEN */
    void (*state)(void);
    float far_x;      /* first vertex x (BIG for the vertex-range case) */
    float z0;         /* first vertex z (+1 is behind the camera) */
    int decline;      /* the stand-in's tnl_prim declines */
} Scenario;

static void st_none(void) {}
static void st_clear(void) { W(0xD3, 1); }
static void st_flat(void) { W(0x50, 0); }
static void st_texgen(void) { W(0x1E, 1); W(0xC0, 1); }
static void st_matrix(void) {
    static const float world43[12] = { BIG, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
    W(0x3A, 0);
    for (int i = 0; i < 12; i++) W(0x3B, f24(world43[i]));
}
static void st_viewport(void) { W(0x42, f24(BIG)); }
static void st_texcoord(void) { W(0x48, f24(BIG)); }
static void st_fog(void) { W(0x1F, 1); W(0xCD, f24(BIG)); }
static void st_specular(void) { W(0x17, 1); W(0x18, 1); W(0x5F, 1); }
static void st_light(void) { W(0x17, 1); W(0x18, 1); W(0x63, f24(BIG)); }

static const Scenario k_scenarios[] = {
    { "eligible", SC_TAKEN, st_none, -0.4f, -5.0f, 0 },
    { "clear", GE_TNL_WHY_CLEAR, st_clear, -0.4f, -5.0f, 0 },
    { "flat", GE_TNL_WHY_FLAT, st_flat, -0.4f, -5.0f, 0 },
    { "texgen", GE_TNL_WHY_TEXGEN, st_texgen, -0.4f, -5.0f, 0 },
    { "matrix", GE_TNL_WHY_MATRIX_RANGE, st_matrix, -0.4f, -5.0f, 0 },
    { "viewport", GE_TNL_WHY_VIEWPORT_RANGE, st_viewport, -0.4f, -5.0f, 0 },
    { "texcoord", GE_TNL_WHY_TEXCOORD_RANGE, st_texcoord, -0.4f, -5.0f, 0 },
    { "fog", GE_TNL_WHY_FOG_RANGE, st_fog, -0.4f, -5.0f, 0 },
    { "specular", GE_TNL_WHY_SPECULAR_POW, st_specular, -0.4f, -5.0f, 0 },
    { "light", GE_TNL_WHY_LIGHT_RANGE, st_light, -0.4f, -5.0f, 0 },
    { "vertex", GE_TNL_WHY_VERTEX_RANGE, st_none, BIG, -5.0f, 0 },
    { "clip", GE_TNL_WHY_CLIP, st_none, -0.4f, 1.0f, 0 },
    { "backend", GE_TNL_WHY_BACKEND, st_none, -0.4f, -5.0f, 1 },
};

static void run_scenario(const Scenario *sc) {
    put_vtx(VERTS, sc->far_x, -0.4f, sc->z0);
    put_vtx(VERTS + 28u, 0.4f, -0.4f, -5.0f);
    put_vtx(VERTS + 56u, 0.0f, 0.4f, -5.0f);
    s_pc = LIST;
    base_state();
    sc->state();
    W(0x01, VERTS);
    W(0x04, (3u << 16) | 3u);        /* PRIM: one triangle */
    W(0x0F, 0);
    W(0x0C, 0);

    unsigned long long why[GE_TNL_WHY_COUNT], taken = 0, cpu = 0;
    ge_tnl_reason_counts(NULL, 1);
    s_decline = sc->decline;
    s_tnl_calls = 0;
    expect(ge_run_list(LIST, 0) == 0, sc->name, "list did not reach END");
    ge_tnl_counts(&taken, &cpu, 0);
    ge_tnl_reason_counts(why, 0);

    unsigned long long sum = 0;
    for (int i = 0; i < GE_TNL_WHY_COUNT; i++) {
        sum += why[i];
        if (i == sc->why) expect(why[i] == 1u, sc->name, "the PRIM is not counted under its reason");
        else if (why[i]) {
            char what[96];
            snprintf(what, sizeof what, "counted under %s as well", ge_tnl_reason_name(i));
            expect(0, sc->name, what);
        }
    }
    expect(sum == cpu, sc->name, "the reasons do not sum to cpu");
    if (sc->why == SC_TAKEN) {
        expect(taken == 1u && cpu == 0u && s_tnl_calls == 1u, sc->name, "the eligible PRIM was not taken");
    } else {
        expect(taken == 0u && cpu == 1u, sc->name, "the PRIM was not counted as kept on the CPU");
        expect(s_tnl_calls == (sc->decline ? 1u : 0u), sc->name,
               "the backend was offered a PRIM ge.c had ruled out");
    }
    /* A reset through either function clears both. */
    ge_tnl_counts(NULL, NULL, 1);
    ge_tnl_reason_counts(why, 0);
    ge_tnl_counts(&taken, &cpu, 0);
    sum = 0;
    for (int i = 0; i < GE_TNL_WHY_COUNT; i++) sum += why[i];
    expect(sum == 0u && taken == 0u && cpu == 0u, sc->name, "a reset left counts behind");
}

int main(void) {
    uint8_t *arena = (uint8_t *)calloc(0x0c000000u, 1);
    if (!arena) {
        fprintf(stderr, "ge_tnl_census_selftest: no guest arena\n");
        return 2;
    }
    g_mem = arena + 0x08000000u;

    for (int i = 0; i < GE_TNL_WHY_COUNT; i++)
        expect(ge_tnl_reason_name(i) != NULL, "names", "a reason has no name");
    expect(ge_tnl_reason_name(GE_TNL_WHY_COUNT) == NULL && ge_tnl_reason_name(-1) == NULL, "names",
           "an out-of-range reason has a name");

    static GeGpuHooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.tri = hook_tri;
    hooks.sprite = hook_sprite;
    hooks.line = hook_line;
    hooks.point = hook_point;
    hooks.flush = hook_flush;
    hooks.vram_dirty = hook_vram_dirty;
    hooks.tnl_prim = hook_tnl_prim;
    ge_set_gpu_hooks(&hooks);
    ge_tnl_set_enabled(1);
    for (size_t i = 0; i < sizeof k_scenarios / sizeof k_scenarios[0]; i++) run_scenario(&k_scenarios[i]);

    /* The switch off: nothing is offered and nothing is counted, a texgen PRIM included. */
    ge_tnl_set_enabled(0);
    ge_tnl_counts(NULL, NULL, 1);
    s_tnl_calls = 0;
    s_pc = LIST;
    base_state();
    st_texgen();
    W(0x01, VERTS);
    W(0x04, (3u << 16) | 3u);
    W(0x0F, 0);
    W(0x0C, 0);
    expect(ge_run_list(LIST, 0) == 0, "off", "list did not reach END");
    unsigned long long taken = 1, cpu = 1, why[GE_TNL_WHY_COUNT], sum = 0;
    ge_tnl_counts(&taken, &cpu, 0);
    ge_tnl_reason_counts(why, 0);
    for (int i = 0; i < GE_TNL_WHY_COUNT; i++) sum += why[i];
    expect(taken == 0u && cpu == 0u && sum == 0u && s_tnl_calls == 0u, "off",
           "the route counted or offered a PRIM while off");
    ge_set_gpu_hooks(NULL);

    if (s_failures) {
        fprintf(stderr, "ge_tnl_census_selftest: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("ge-tnl-census-selftest: PASS (%zu scenarios)\n", sizeof k_scenarios / sizeof k_scenarios[0]);
    return 0;
}
