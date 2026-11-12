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
// A reset through either ge_tnl_counts or ge_tnl_reason_counts clears both. SR_GESTAT's
// GESTAT-TNL line is read back from ge_set_frame over two stat windows: its exact text, and the
// second window's counts only.
//
// Last, a render-state motion tap (#2081, GeMotionTap in ge_shared.h): while one is set the route
// is not offered the eligible PRIM and counts nothing, and ge.c's own T&L draws it.
//
// The test removes every SR_ variable from its environment first (ge.c latches SR_NOLIGHT,
// SR_NOFOG, SR_RTRACE, SR_GE_PRIM_PROFILE and others once per process), then sets SR_GESTAT.
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

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define census_environ _environ
#define cap_pipe(fds) _pipe((fds), 65536u, _O_BINARY)
#define cap_dup _dup
#define cap_dup2 _dup2
#define cap_close _close
#define cap_read(fd, buf, n) _read((fd), (buf), (unsigned)(n))
static int env_unset(const char *name) { return _putenv_s(name, ""); }
static int env_set(const char *name, const char *value) { return _putenv_s(name, value); }
#else
#include <unistd.h>
extern char **environ;
#define census_environ environ
#define cap_pipe(fds) pipe(fds)
#define cap_dup dup
#define cap_dup2 dup2
#define cap_close close
#define cap_read(fd, buf, n) read((fd), (buf), (n))
static int env_unset(const char *name) { return unsetenv(name); }
static int env_set(const char *name, const char *value) { return setenv(name, value, 1); }
#endif

extern uint32_t ge_run_list(uint32_t addr, int resume);
extern void ge_set_frame(uint32_t frame);

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

/* A motion tap that only counts what it is told. */
static unsigned s_tap_prims, s_tap_drawn, s_tap_rastered, s_tap_line_points;

static void tap_prim(void *user, const GeMotionPrim *p) { (void)user; (void)p; s_tap_prims++; }
static void tap_tri(void *user, const uint16_t pos[3], const GeMotionVtx v[3], int decision) {
    (void)user; (void)pos; (void)v;
    if (decision == GE_MOTION_DRAW) s_tap_drawn++;
}
static void tap_fan(void *user, int piece) { (void)user; (void)piece; }
static void tap_sprite(void *user, const uint16_t pos[2], const GeMotionVtx *v, int nv, int decision) {
    (void)user; (void)pos; (void)v; (void)nv; (void)decision;
}
static void tap_rastered(void *user, const GeVtx *v0, const GeVtx *v1, const GeVtx *v2) {
    (void)user; (void)v0; (void)v1; (void)v2;
    s_tap_rastered++;
}
static void tap_line_point(void *user, int nv) { (void)user; (void)nv; s_tap_line_points++; }

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

static void run_scenario(const Scenario *sc, int reset_by_reason) {
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
    /* A reset through either function clears both: the scenarios alternate between them. */
    if (reset_by_reason) ge_tnl_reason_counts(NULL, 1);
    else ge_tnl_counts(NULL, NULL, 1);
    ge_tnl_reason_counts(why, 0);
    ge_tnl_counts(&taken, &cpu, 0);
    sum = 0;
    for (int i = 0; i < GE_TNL_WHY_COUNT; i++) sum += why[i];
    expect(sum == 0u && taken == 0u && cpu == 0u, sc->name,
           reset_by_reason ? "a reset through ge_tnl_reason_counts left counts behind"
                           : "a reset through ge_tnl_counts left counts behind");
}

/* ---- SR_GESTAT's window line ---- */

/* ge_set_frame(frame) with stderr read back into out. Returns 0, or -1 if stderr could not be
 * redirected. The window's lines are a few hundred bytes, well inside a pipe's buffer. */
static int set_frame_captured(uint32_t frame, char *out, size_t cap) {
    int fds[2];
    out[0] = 0;
    if (cap_pipe(fds) != 0) return -1;
    fflush(stderr);
    int saved = cap_dup(2);
    if (saved < 0 || cap_dup2(fds[1], 2) < 0) {
        if (saved >= 0) cap_close(saved);
        cap_close(fds[0]);
        cap_close(fds[1]);
        return -1;
    }
    cap_close(fds[1]);
    ge_set_frame(frame);
    fflush(stderr);
    cap_dup2(saved, 2);
    cap_close(saved);
    size_t len = 0;
    while (len + 1u < cap) {
        long got = (long)cap_read(fds[0], out + len, cap - 1u - len);
        if (got <= 0) break;
        len += (size_t)got;
    }
    cap_close(fds[0]);
    out[len] = 0;
    return 0;
}

/* The window closing at `frame` prints exactly one GESTAT-TNL line, equal to `want`. */
static void expect_window_line(uint32_t frame, const char *scenario, const char *want) {
    static char out[16384];
    if (set_frame_captured(frame, out, sizeof out) != 0) {
        expect(0, scenario, "stderr could not be redirected");
        return;
    }
    const char *line = strstr(out, "GESTAT-TNL ");
    if (!line) {
        expect(0, scenario, "no GESTAT-TNL line");
        return;
    }
    const char *end = strchr(line, '\n');
    size_t len = end ? (size_t)(end - line) : strlen(line);
    if (len != strlen(want) || memcmp(line, want, len) != 0) {
        fprintf(stderr, "ge_tnl_census_selftest: %s: got  \"%.*s\"\n", scenario, (int)len, line);
        fprintf(stderr, "ge_tnl_census_selftest: %s: want \"%s\"\n", scenario, want);
        expect(0, scenario, "the GESTAT-TNL line differs");
    }
    expect(strstr(line + len, "GESTAT-TNL ") == NULL, scenario, "more than one GESTAT-TNL line");
}

/* Removes every SR_ variable, one at a time so the environment is never walked while it
 * changes. Returns the number left behind (0 unless removal fails). */
static int env_scrub(void) {
    for (int guard = 0; guard < 4096; guard++) {
        const char *hit = NULL;
        for (char **e = census_environ; e && *e; e++)
            if (strncmp(*e, "SR_", 3) == 0) { hit = *e; break; }
        if (!hit) return 0;
        char name[256];
        const char *eq = strchr(hit, '=');
        size_t n = eq ? (size_t)(eq - hit) : strlen(hit);
        if (n >= sizeof name) return 1;
        memcpy(name, hit, n);
        name[n] = 0;
        if (env_unset(name) != 0) return 1;
    }
    return 1;
}

int main(void) {
    if (env_scrub() != 0 || env_set("SR_GESTAT", "1") != 0) {
        fprintf(stderr, "ge_tnl_census_selftest: cannot clear the SR_ environment\n");
        return 2;
    }
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
    for (size_t i = 0; i < sizeof k_scenarios / sizeof k_scenarios[0]; i++)
        run_scenario(&k_scenarios[i], (int)(i & 1u));

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

    /* The first window holds every scenario above, whatever the resets between them did, and
     * nothing from the PRIM drawn with the switch off. The second holds only its own two PRIMs. */
    ge_tnl_set_enabled(1);
    expect_window_line(60u, "window-1",
        "GESTAT-TNL f=60 taken=1 cpu=12: clear=1 flat=1 diagnostic=0 texgen=1 matrix=1 viewport=1"
        " texcoord=1 fog=1 specular=1 light=1 alloc=0 vertex=1 clip=1 backend=1");
    run_scenario(&k_scenarios[0], 0);   /* eligible */
    run_scenario(&k_scenarios[3], 1);   /* texgen */
    expect_window_line(120u, "window-2",
        "GESTAT-TNL f=120 taken=1 cpu=1: clear=0 flat=0 diagnostic=0 texgen=1 matrix=0 viewport=0"
        " texcoord=0 fog=0 specular=0 light=0 alloc=0 vertex=0 clip=0 backend=0");

    /* A motion tap (#2081) needs ge.c's clip positions, which this route never forms: while one
     * is set the eligible PRIM is not offered and is in neither count, ge.c's own T&L draws it,
     * and the tap is told its triangle. With the tap removed the PRIM is taken as before. */
    static const GeMotionTap tap = { NULL, tap_prim, tap_tri, tap_fan, tap_sprite, tap_rastered, tap_line_point };
    put_vtx(VERTS, -0.4f, -0.4f, -5.0f);
    put_vtx(VERTS + 28u, 0.4f, -0.4f, -5.0f);
    put_vtx(VERTS + 56u, 0.0f, 0.4f, -5.0f);
    s_pc = LIST;
    base_state();
    W(0x01, VERTS);
    W(0x04, (3u << 16) | 3u);
    W(0x0F, 0);
    W(0x0C, 0);
    ge_tnl_counts(NULL, NULL, 1);
    s_tnl_calls = s_cpu_tris = 0;
    ge_motion_tap_set(&tap);
    expect(ge_run_list(LIST, 0) == 0, "motion-tap", "list did not reach END");
    ge_motion_tap_set(NULL);
    ge_tnl_counts(&taken, &cpu, 0);
    ge_tnl_reason_counts(why, 0);
    sum = 0;
    for (int i = 0; i < GE_TNL_WHY_COUNT; i++) sum += why[i];
    expect(s_tnl_calls == 0u && taken == 0u && cpu == 0u && sum == 0u, "motion-tap",
           "the route offered or counted a PRIM while a motion tap was set");
    expect(s_cpu_tris == 1u && s_tap_prims == 1u && s_tap_drawn == 1u && s_tap_rastered == 1u &&
               s_tap_line_points == 0u, "motion-tap",
           "ge.c's own T&L did not draw the PRIM and tell the tap");
    expect(ge_run_list(LIST, 0) == 0, "motion-tap", "list did not reach END");
    ge_tnl_counts(&taken, &cpu, 0);
    expect(s_tnl_calls == 1u && taken == 1u && cpu == 0u && s_tap_prims == 1u, "motion-tap",
           "with the tap removed the eligible PRIM was not taken, or the tap was still called");
    ge_set_gpu_hooks(NULL);

    if (s_failures) {
        fprintf(stderr, "ge_tnl_census_selftest: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("ge-tnl-census-selftest: PASS (%zu scenarios)\n", sizeof k_scenarios / sizeof k_scenarios[0]);
    return 0;
}
