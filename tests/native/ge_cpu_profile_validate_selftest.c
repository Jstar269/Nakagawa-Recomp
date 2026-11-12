// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// ge-cpu-profile-validate-selftest: ge_cpu_profile_validate() against counters produced
// by the production GE object.
//
// Every block expected to validate is written by ge.o itself: source-owned display lists
// run through ge_run_list() (the command walker, draw_prim, patch tessellation, block
// transfer and CLUT load) with the profiles switched through their environment variables
// and ge_cpu_profile_reset(), the way the replay and the runtime configure them.  This file
// changes counters only in copies made for fault injection.  It supplies guest memory, a
// deterministic monotonic clock (each read advances it, so every timed interval is
// non-empty and runs are reproducible), the runtime's diagnostic globals at their disabled
// values, and, for one scenario, a GPU backend stand-in registered through the real
// ge_set_gpu_hooks() seam.  Runtime entry points a GE list must not reach abort.
//
// Fault injection copies a production snapshot, applies one counter fault, and requires
// the failure mask to equal exactly the check(s) that fault breaks, so a fault cannot pass
// because an unrelated check fired.  Each relation, and each clause of a multi-clause
// relation, has a fault that breaks it alone.  The *_wraps faults are built so that a
// wrapping uint64_t sum would land back on the expected value; they fail only because
// those relations are compared exactly.
//
// The Makefile runs this twice, once with --strip-cache-disabled, because ge.c latches
// the strip-cache switch once per process.
//
// The file also holds tests of other GE seams on the same object.  The last of them is the
// render-state motion tap (#2081, test_ge_motion_tap): a list drawn with no listener, with one
// and with none again leaves byte-identical VRAM, depth, GE state, counters and GE clock, and
// the listener's store rebuilds the vertices ge.c handed over, bit for bit.
//
// `--bench ROUNDS` is not a test.  It times the same production GE object on two source-owned
// workloads with the flight recorder compiled in but disarmed, or compiled out when the
// build passes -USR_FLIGHT_RECORDER_LINKED, and prints one line per workload plus a
// framebuffer hash so two builds can be compared (the ge-flight-overhead target, #320).

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* setenv/unsetenv under a strict -std */
#endif

#include "recomp.h"
#include "debug.h"
#include "ge_capture.h"
#include "ge_shared.h"
#include "motion_tap.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

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
    fprintf(stderr, "ge_cpu_profile_validate_selftest: unexpected runtime call %s addr=0x%08" PRIx32
            " (the lists stay in range with every watch disabled)\n", what, addr);
    fflush(stderr);
    abort();
}

void sr_oor(uint32_t a, uint32_t v, int store) {
    (void)v; (void)store;
    unexpected("sr_oor", a);
}
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

/* Deterministic monotonic clock: each read advances 7 ns. */
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

/* ---- Source-owned display lists ---- */

#define LIST_MAIN     0x08100000u
#define LIST_SUB      0x08108000u
#define LIST_EMPTY    0x0810C000u
#define LIST_BAD_CLUT 0x0810E000u
#define LIST_VRAM_DEPTH 0x08110000u
#define LIST_VRAM_CPU_WRITE 0x08112000u
#define LIST_VRAM_AMBIGUOUS 0x08114000u
#define LIST_IMMEDIATE 0x0811E000u
#define LIST_IMMEDIATE_STREAM 0x0811F000u
#define VERTS         0x08200000u
#define INDICES       0x08280000u
#define CLUT_SRC      0x08290000u
#define VRAM_TEST_VERTS 0x08300000u

/* col 8888, float normal, float position: 4 + 12 + 12 = 28 bytes per vertex. */
#define VT_TRANSFORM ((7u << 2) | (3u << 5) | (3u << 7))
#define VT_THROUGH   (VT_TRANSFORM | (1u << 23))
#define VT_INDEX8    (1u << 11)
#define VT_INDEX16   (2u << 11)
#define VSTRIDE      28u

/* View-space depths under the perspective projection below (near 1, far 100). */
#define FRONT  (-5.0f) /* w = 5: in front of the near plane */
#define BEHIND (1.0f)  /* w = -1: behind the camera */

static uint32_t s_pc;
static uint32_t s_vtx = VERTS;
static void expect(int ok, const char *scenario, const char *what);

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

static uint32_t vtx(float x, float y, float z) {
    uint32_t addr = s_vtx;
    uint32_t color = 0xFF80C040u;
    float normal[3] = { 0.0f, 0.0f, 1.0f };
    float pos[3] = { x, y, z };
    memcpy(SR_HOST(addr), &color, 4);
    memcpy(SR_HOST(addr + 4u), normal, sizeof normal);
    memcpy(SR_HOST(addr + 16u), pos, sizeof pos);
    s_vtx += VSTRIDE;
    return addr;
}

static void prim(uint32_t type, uint32_t count, uint32_t vaddr) {
    W(0x01, vaddr);                 /* VADDR (BASE supplies 0x08000000) */
    W(0x04, (type << 16) | count);  /* PRIM */
}

static void matrix(uint32_t select, uint32_t data, const float *m, int n) {
    W(select, 0);
    for (int i = 0; i < n; i++) W(data, f24(m[i]));
}

static void emit_setup(void) {
    static const float identity43[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
    /* Column-major perspective: w = -z, near 1, far 100. */
    static const float perspective[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, -101.0f / 99.0f, -1,
        0, 0, -200.0f / 99.0f, 0,
    };
    W(0x10, 0x080000);              /* BASE: upper address bits 0x08000000 */
    W(0x13, 0);                     /* OFFSETADDR */
    W(0x9C, 0x044000);              /* FRAMEBUFPTR -> 0x04044000 */
    W(0x9D, 512);                   /* FRAMEBUFWIDTH */
    W(0xD2, 3);                     /* PIXFORMAT 8888 */
    W(0xD4, 0);                     /* SCISSOR1 */
    W(0xD5, (271u << 10) | 479u);   /* SCISSOR2 */
    W(0x1E, 0);                     /* TEXTUREMAPENABLE */
    W(0x23, 0);                     /* ZTESTENABLE */
    W(0x1C, 1);                     /* CLIPENABLE: near-plane clipping instead of z drops */
    W(0x17, 0);                     /* LIGHTINGENABLE */
    W(0x42, f24(240.0f)); W(0x43, f24(-136.0f)); W(0x44, f24(1000.0f));
    W(0x45, f24(240.0f)); W(0x46, f24(136.0f)); W(0x47, f24(30000.0f));
    W(0x4C, 0); W(0x4D, 0);         /* OFFSETX/Y */
    matrix(0x3A, 0x3B, identity43, 12);   /* WORLD */
    matrix(0x3C, 0x3D, identity43, 12);   /* VIEW */
    matrix(0x3E, 0x3F, perspective, 16);  /* PROJ */
}

static void build_lists(void) {
    /* Transform-mode geometry. */
    uint32_t tris = vtx(-0.4f, -0.4f, FRONT);           /* triangle drawn */
    vtx(0.4f, -0.4f, FRONT); vtx(0.0f, 0.4f, FRONT);
    vtx(-0.4f, -0.4f, FRONT); vtx(0.4f, -0.4f, FRONT);   /* crosses the near plane */
    vtx(0.0f, 0.4f, BEHIND);
    vtx(-0.4f, -0.4f, BEHIND); vtx(0.4f, -0.4f, BEHIND); /* wholly behind the camera */
    vtx(0.0f, 0.4f, BEHIND);
    vtx(-15.0f, -0.4f, FRONT); vtx(0.4f, -0.4f, FRONT);  /* leaves the screen grid */
    vtx(0.0f, 0.4f, FRONT);
    vtx(0.2f, 0.2f, FRONT);                              /* leftover: no triangle */
    uint32_t strip = vtx(-0.4f, -0.4f, FRONT);
    vtx(-0.4f, 0.4f, FRONT); vtx(0.0f, -0.4f, FRONT); vtx(0.0f, 0.4f, BEHIND);
    vtx(0.4f, -0.4f, FRONT); vtx(0.4f, 0.4f, FRONT);
    uint32_t fan = vtx(0.0f, 0.0f, FRONT);
    vtx(-0.4f, -0.4f, FRONT); vtx(0.4f, -0.4f, FRONT); vtx(0.4f, 0.4f, BEHIND);
    vtx(-0.4f, 0.4f, FRONT);
    uint32_t points = vtx(-0.2f, -0.2f, FRONT);
    vtx(0.2f, 0.2f, BEHIND); vtx(-15.0f, 0.0f, FRONT); vtx(0.2f, -0.2f, FRONT);
    uint32_t sprites = vtx(-0.3f, -0.3f, BEHIND);        /* first corner dropped */
    vtx(0.3f, 0.3f, FRONT);
    vtx(-0.3f, -0.3f, FRONT); vtx(0.3f, 0.3f, BEHIND);   /* second corner dropped */
    vtx(-0.3f, -0.3f, FRONT); vtx(0.3f, 0.3f, FRONT);
    uint32_t lines = vtx(-0.4f, 0.0f, FRONT);
    vtx(0.4f, 0.0f, FRONT); vtx(0.0f, -0.4f, FRONT); vtx(0.0f, 0.4f, FRONT);
    vtx(0.1f, 0.1f, FRONT);
    uint32_t line_strip = vtx(-0.4f, 0.0f, FRONT);
    vtx(0.0f, 0.4f, FRONT); vtx(0.0f, 0.0f, BEHIND); vtx(0.4f, 0.0f, FRONT);
    uint32_t patch = s_vtx;                              /* 4x4 control grid */
    for (int v = 0; v < 4; v++)
        for (int u = 0; u < 4; u++)
            vtx(-0.3f + 0.2f * (float)u, -0.3f + 0.2f * (float)v, FRONT);

    /* Through-mode geometry: screen pixels. */
    uint32_t t_points = vtx(10.0f, 10.0f, 0.0f);
    vtx(20.0f, 10.0f, 0.0f); vtx(30.0f, 10.0f, 0.0f);
    uint32_t t_lines = vtx(10.0f, 20.0f, 0.0f);
    vtx(40.0f, 20.0f, 0.0f); vtx(10.0f, 25.0f, 0.0f); vtx(40.0f, 25.0f, 0.0f);
    vtx(10.0f, 30.0f, 0.0f);
    uint32_t t_line_strip = vtx(50.0f, 20.0f, 0.0f);
    vtx(60.0f, 30.0f, 0.0f); vtx(70.0f, 20.0f, 0.0f); vtx(80.0f, 30.0f, 0.0f);
    uint32_t t_tris = vtx(10.0f, 40.0f, 0.0f);
    vtx(30.0f, 40.0f, 0.0f); vtx(20.0f, 60.0f, 0.0f);
    vtx(40.0f, 40.0f, 0.0f); vtx(60.0f, 40.0f, 0.0f); vtx(50.0f, 60.0f, 0.0f);
    vtx(70.0f, 40.0f, 0.0f);
    uint32_t t_strip = vtx(10.0f, 70.0f, 0.0f);
    vtx(10.0f, 90.0f, 0.0f); vtx(30.0f, 70.0f, 0.0f); vtx(30.0f, 90.0f, 0.0f);
    vtx(50.0f, 70.0f, 0.0f); vtx(50.0f, 90.0f, 0.0f);
    uint32_t t_fan = vtx(80.0f, 80.0f, 0.0f);
    vtx(70.0f, 70.0f, 0.0f); vtx(90.0f, 70.0f, 0.0f); vtx(90.0f, 90.0f, 0.0f);
    vtx(70.0f, 90.0f, 0.0f);
    uint32_t t_sprites = vtx(100.0f, 10.0f, 0.0f);
    vtx(120.0f, 30.0f, 0.0f); vtx(130.0f, 10.0f, 0.0f); vtx(150.0f, 30.0f, 0.0f);
    vtx(160.0f, 10.0f, 0.0f);

    /* Index buffers: an 8-bit transform strip into the triangle vertices and a 16-bit
     * through strip into the through strip vertices. */
    static const uint8_t idx8[5] = { 0, 1, 2, 12, 0 };
    static const uint16_t idx16[4] = { 0, 1, 2, 3 };
    memcpy(SR_HOST(INDICES), idx8, sizeof idx8);
    memcpy(SR_HOST(INDICES + 16u), idx16, sizeof idx16);

    for (uint32_t i = 0; i < 16u; i++) {               /* CLUT source words */
        uint32_t word = 0xFF000000u | i;
        memcpy(SR_HOST(CLUT_SRC + i * 4u), &word, 4);
    }

    s_pc = LIST_SUB;                                     /* CALL target */
    W(0x12, VT_THROUGH);
    prim(3, 3, t_tris);
    W(0x0B, 0);                                          /* RET */

    s_pc = LIST_MAIN;
    emit_setup();
    W(0x12, VT_TRANSFORM);
    prim(3, 13, tris);
    prim(4, 6, strip);
    prim(5, 5, fan);
    prim(0, 4, points);
    prim(6, 6, sprites);
    prim(1, 5, lines);
    prim(2, 4, line_strip);
    W(0x17, 1);                                          /* LIGHTINGENABLE for one draw */
    W(0x12, VT_TRANSFORM | VT_INDEX8);
    W(0x02, INDICES);                                    /* IADDR */
    prim(4, 5, tris);
    W(0x17, 0);
    W(0x12, VT_TRANSFORM);
    prim(7, 3, tris);                                    /* reserved type: no primitives */
    prim(3, 0, tris);
    prim(4, 2, strip);                                   /* strip too short for a triangle */
    W(0x01, patch);
    W(0x36, (2u << 8) | 2u);                             /* PATCHDIVISION 2x2 */
    W(0x37, 0);                                          /* PATCHPRIMITIVE triangles */
    W(0x05, 0x0404);                                     /* BEZIER 4x4 */
    W(0x01, patch);
    W(0x36, (1u << 8) | 1u);
    W(0x37, 2);                                          /* PATCHPRIMITIVE points */
    W(0x06, 0x0404);                                     /* SPLINE 4x4 */
    W(0x12, VT_THROUGH);
    prim(0, 3, t_points);
    prim(1, 5, t_lines);
    prim(2, 4, t_line_strip);
    prim(3, 7, t_tris);
    prim(4, 6, t_strip);
    prim(5, 5, t_fan);
    prim(6, 5, t_sprites);
    prim(7, 2, t_points);
    W(0x12, VT_THROUGH | VT_INDEX16);
    W(0x02, INDICES + 16u);
    prim(4, 4, t_strip);
    W(0xB0, CLUT_SRC & 0xFFFFFFu);                       /* CLUTADDR */
    W(0xB1, (CLUT_SRC >> 8) & 0xFF0000u);                /* CLUTADDRUPPER */
    W(0xC4, 2);                                          /* LOADCLUT: 2 blocks of 32 bytes */
    W(0xB2, 0x100000); W(0xB3, (0x04u << 16) | 64u);     /* TRANSFERSRC 0x04100000/64 */
    W(0xB4, 0x140000); W(0xB5, (0x04u << 16) | 64u);     /* TRANSFERDST 0x04140000/64 */
    W(0xEB, 0); W(0xEC, 0);                              /* TRANSFERSRCPOS/DSTPOS */
    W(0xEE, (7u << 10) | 7u);                            /* TRANSFERSIZE 8x8 */
    W(0xEA, 0);                                          /* TRANSFERSTART 16-bit */
    W(0x0A, LIST_SUB & 0xFFFFFFu);                       /* CALL */
    W(0x0F, 0);                                          /* FINISH */
    W(0x0C, 0);                                          /* END */

    s_pc = LIST_EMPTY;
    W(0x0C, 0);

    /* A CLUT load whose source span leaves the guest arena: production ends the clut_load
     * phase without counting a load (and reports the span on stderr). */
    s_pc = LIST_BAD_CLUT;
    W(0x10, 0x080000);
    W(0xB0, 0xFFFFE0);
    W(0xB1, 0x0B0000);                                   /* 0x0BFFFFE0 */
    W(0xC4, 0x3F);
    W(0x0C, 0);
}

static uint32_t depth_test_vertex(uint32_t address, uint16_t x, uint16_t y,
                                  uint16_t z, uint32_t color) {
    MEM_W32(address, color);
    MEM_W16(address + 4u, x);
    MEM_W16(address + 6u, y);
    MEM_W16(address + 8u, z);
    return address + 12u;                 /* 4-byte color alignment pads the vertex stride */
}

static void build_vram_depth_list_at(uint32_t zbp) {
    static const uint32_t through16 = (7u << 2) | (2u << 7) | (1u << 23);
    uint32_t vertex = VRAM_TEST_VERTS;
    uint32_t clear_sprite = vertex;
    vertex = depth_test_vertex(vertex, 0u, 0u, 0u, 0xFF646464u);
    vertex = depth_test_vertex(vertex, 480u, 272u, 0u, 0xFF646464u);
    uint32_t sprite_a = vertex;
    vertex = depth_test_vertex(vertex, 0u, 0u, 0x1111u, 0xFF101010u);
    vertex = depth_test_vertex(vertex, 32u, 16u, 0x1111u, 0xFF101010u);
    uint32_t sprite_b = vertex;
    vertex = depth_test_vertex(vertex, 64u, 8u, 0x2222u, 0xFF202020u);
    vertex = depth_test_vertex(vertex, 96u, 12u, 0x2222u, 0xFF202020u);
    uint32_t sprite_c = vertex;
    vertex = depth_test_vertex(vertex, 200u, 100u, 0x3333u, 0xFF303030u);
    (void)depth_test_vertex(vertex, 208u, 101u, 0x3333u, 0xFF303030u);

    s_pc = LIST_VRAM_DEPTH;
    W(0x10, 0x080000);
    W(0x9C, 0x00100000u);                /* colour base: 0x04100000 */
    W(0x9D, 512u);
    W(0xD2, 3u);                         /* 8888 */
    W(0x9E, zbp);                        /* depth base */
    W(0x9F, 512u);
    W(0xD4, 0u);
    W(0xD5, (271u << 10) | 479u);
    W(0x23, 1u);                         /* depth test enabled */
    W(0xDE, 1u);                         /* always pass */
    W(0x12, through16);
    W(0xD3, 1u | 0x100u | 0x200u | 0x400u); /* clear colour, alpha, depth */
    prim(6u, 2u, clear_sprite);
    W(0xD3, 0u);
    prim(6u, 2u, sprite_a);
    prim(6u, 2u, sprite_b);
    prim(6u, 2u, sprite_c);
    W(0x0C, 0u);
}

static void build_vram_cpu_write_list(void) {
    static const uint32_t through16 = (7u << 2) | (2u << 7) | (1u << 23);
    uint32_t vertex = VRAM_TEST_VERTS + 0x100u;
    uint32_t depth_sprite = vertex;
    vertex = depth_test_vertex(vertex, 0u, 0u, 0x4444u, 0xFF010203u);
    vertex = depth_test_vertex(vertex, 1u, 1u, 0x4444u, 0xFF010203u);
    uint32_t color_sprite = vertex;
    (void)depth_test_vertex(vertex, 2u, 2u, 0u, 0xFF010203u);
    (void)depth_test_vertex(vertex + 12u, 3u, 3u, 0u, 0xFF010203u);

    s_pc = LIST_VRAM_CPU_WRITE;
    W(0x10, 0x080000u);
    W(0x9C, 0x00100000u);
    W(0x9D, 512u);
    W(0xD2, 3u);
    W(0x9E, 0x0CC000u);
    W(0x9F, 512u);
    W(0xD4, 0u);
    W(0xD5, (271u << 10) | 479u);
    W(0x12, through16);
    W(0x23, 1u);
    W(0xDE, 4u);                         /* less */
    prim(6u, 2u, depth_sprite);
    W(0x23, 0u);
    W(0x28, 1u);                         /* logic op enabled */
    W(0xE6, 6u);                         /* XOR */
    prim(6u, 2u, color_sprite);
    W(0x0C, 0u);
}

static void build_vram_ambiguous_depth_list(uint32_t zbp, uint32_t vertex) {
    static const uint32_t through16 = (7u << 2) | (2u << 7) | (1u << 23);
    (void)depth_test_vertex(vertex, 20u, 20u, 0x1111u, 0xFF010203u);
    (void)depth_test_vertex(vertex + 12u, 21u, 21u, 0x1111u, 0xFF010203u);
    s_pc = LIST_VRAM_AMBIGUOUS;
    W(0x10, 0x080000u);
    W(0x9C, 0x00100000u);
    W(0x9D, 512u);
    W(0xD2, 3u);
    W(0x9E, zbp);
    W(0x9F, 512u);
    W(0xD4, 0u);
    W(0xD5, (271u << 10) | 479u);
    W(0x23, 1u);
    W(0xDE, 1u);
    W(0x21, 0u);
    W(0x28, 0u);
    W(0xD3, 0u);
    W(0x12, through16);
    prim(6u, 2u, vertex);
    W(0x0C, 0u);
}

/* zbp 0x0ce000 (bit 13 set). The PSP XORs bits 6 and 13 of the absolute address (measured 2026-10-21):
 * pixel (x, y) depth is at (zbp + linear) ^ 0x2040, not zbp + (linear ^ 0x2040). The mirror at
 * 0x04200000 then reads it back linearly. */
static void test_ge_vram_depth_bit13_absolute_xor(void) {
    const uint32_t zbp = 0x0CE000u;
    const uint32_t zbase = 0x04000000u + zbp;
    build_vram_depth_list_at(zbp);
    expect(ge_run_list(LIST_VRAM_DEPTH, 0) == 0, "vram_depth_bit13",
           "bit-13 depth list did not reach END");
    static const struct {
        unsigned x, y;
        uint16_t z;
    } points[] = {
        { 0u, 0u, 0x1111u },
        { 64u, 8u, 0x2222u },
        { 200u, 100u, 0x3333u },
    };
    for (size_t i = 0; i < sizeof points / sizeof points[0]; i++) {
        uint32_t linear = (points[i].y * 512u + points[i].x) * 2u;
        uint32_t stored = (zbase + linear) ^ 0x2040u;
        uint32_t alias_linear = 0x04200000u + zbp + linear;
        char detail[128];
        snprintf(detail, sizeof detail, "absolute-XOR depth at x=%u y=%u", points[i].x, points[i].y);
        expect(MEM_R16(stored) == points[i].z, "vram_depth_bit13", detail);
        snprintf(detail, sizeof detail, "linear mirror depth at x=%u y=%u", points[i].x, points[i].y);
        expect(MEM_R16(alias_linear) == points[i].z, "vram_depth_bit13", detail);
    }
}

/* ---- Texture mip levels (#1373) ----
 * A through-mode sprite samples an 8x8 8888 texture whose levels 0-3 are one colour each,
 * so the colour drawn names the level the sampler chose. Level 1 stores its 4 texels per
 * row in a buffer 8 wide with magenta beyond them, which only a sampler ignoring that
 * level's own buffer width reaches. Levels 2 and 3 use buffer width 4: a linear texture's
 * rows are whole 16-byte lines on the PSP (ge_tex_row_bytes, PSP-GE-RASTER-001). The level
 * rules are the PSP's (PSP-GE-MIP-001, ge.c tex_detail16; hle_thread_selftest.c
 * test_ge_mip_cells_match_psp checks them against the PSP); these cases pin the PSPSDK modes
 * and keep today's level-0 output wherever mip levels are off. */
#define MIP_LIST  0x08116000u
#define MIP_VERTS 0x08118000u
#define MIP_TEX   0x08320000u
#define MIP_FB    0x04150000u
#define MIP_X     16u
#define MIP_Y     16u
#define MIP_LEVEL(mode, bias16) ((uint32_t)(mode) | (((uint32_t)(bias16) & 0xFFu) << 16))

static const uint32_t k_mip_color[4] = { 0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFFFFu };

static uint32_t mip_level_addr(unsigned level) { return MIP_TEX + level * 0x1000u; }
static unsigned mip_level_bufw(unsigned level) { return level <= 1u ? 8u : 4u; }

static void mip_fill_texture(void) {
    for (unsigned level = 0; level < 4u; level++) {
        unsigned size = 8u >> level, bufw = mip_level_bufw(level);
        for (unsigned v = 0; v < size; v++)
            for (unsigned u = 0; u < bufw; u++)
                MEM_W32(mip_level_addr(level) + (v * bufw + u) * 4u,
                        u < size ? k_mip_color[level] : 0xFFFF00FFu);
    }
}

/* Draw the texture (UV 0-8) onto a size x size sprite; return the one RGB colour the sprite
 * holds (framebuffer alpha is the stencil, which these draws leave alone), or 0 when it is
 * not uniform or the list did not end. */
/* The route that submits the textured draw (#1373): PRIM, a BEZIER patch or immediate vertices.
 * Each reaches the sampler through its own entry in ge.c, and each must reach the mip-use count. */
enum { MIP_ROUTE_PRIM, MIP_ROUTE_PATCH, MIP_ROUTE_IMMEDIATE };

static uint32_t mip_draw_route(int route, uint32_t max_level, uint32_t level_word,
                               uint32_t min_filter, unsigned size) {
    const float verts[10] = { 0.0f, 0.0f, (float)MIP_X, (float)MIP_Y, 0.0f,
                              8.0f, 8.0f, (float)(MIP_X + size), (float)(MIP_Y + size), 0.0f };
    memcpy(SR_HOST(MIP_VERTS), verts, sizeof verts);
    for (unsigned y = 0; y < 16u; y++)
        for (unsigned x = 0; x < 16u; x++)
            MEM_W32(MIP_FB + ((MIP_Y + y) * 512u + MIP_X + x) * 4u, 0u);
    s_pc = MIP_LIST;
    W(0x10, 0x080000u);                     /* BASE */
    W(0x9C, MIP_FB & 0x00FFFFFFu);          /* FRAMEBUFPTR */
    W(0x9D, 512u);
    W(0xD2, 3u);                            /* 8888 */
    W(0xD4, 0u);
    W(0xD5, (271u << 10) | 479u);
    W(0xD3, 0u);                            /* not clear mode */
    W(0x21, 0u); W(0x22, 0u); W(0x23, 0u); W(0x24, 0u); W(0x27, 0u); W(0x28, 0u);
    for (unsigned level = 0; level < 4u; level++) {
        uint32_t addr = mip_level_addr(level), size_log2 = 3u - level;
        W(0xA0 + level, addr & 0x00FFFFFFu);                         /* TEXADDRn */
        W(0xA8 + level, ((addr >> 8) & 0x00FF0000u) | mip_level_bufw(level));
        W(0xB8 + level, (size_log2 << 8) | size_log2);              /* TEXSIZEn */
    }
    W(0xC2, max_level << 16);               /* TEXMODE: highest level, unswizzled */
    W(0xC3, 3u);                            /* TEXFORMAT 8888 */
    W(0xC6, min_filter);                    /* TEXFILTER, magnification nearest */
    W(0xC7, 0u);                            /* TEXWRAP repeat */
    W(0xC8, level_word);                    /* TEXLEVEL */
    W(0xC9, 3u | 0x100u);                   /* TEXFUNC replace, RGBA */
    W(0x1E, 1u);                            /* TEXTUREMAPENABLE */
    if (route == MIP_ROUTE_PATCH) {
        /* A 4x4 BEZIER patch of zeroed transform-mode control points: it draws nothing visible,
         * but it takes the patch route into the sampler state, which is all the count reads. */
        static const unsigned char zeros[16 * 20];
        memcpy(SR_HOST(MIP_VERTS), zeros, sizeof zeros);
        W(0x12, 3u | (3u << 7));            /* float UV and position, transform mode */
        W(0x36, (1u << 8) | 1u);            /* PATCHDIVISION 1x1 */
        W(0x37, 0u);                        /* PATCHPRIMITIVE triangles */
        W(0x01, MIP_VERTS);
        W(0x05, 0x0404u);                   /* BEZIER 4x4 */
    } else if (route == MIP_ROUTE_IMMEDIATE) {
        /* An immediate sprite: texture flag in VAP, VTCS/VTCT normalized (1.0 is the level-0 size). */
        const uint32_t flags = 0x00200000u;                  /* GE_IMM_TEXTURE */
        W(0x12, 3u | (3u << 7) | (1u << 23));
        W(0xF6, 0x00FFFFFFu);                                /* VCV: white */
        W(0xF8, 255u);                                       /* VFC */
        W(0xF3, 0u); W(0xF4, 0u);                            /* VTCS, VTCT */
        W(0xF0, 0x8000u + MIP_X * 16u); W(0xF1, 0x8000u + MIP_Y * 16u); W(0xF2, 0u);
        W(0xF7, (6u << 8) | 0xFFu | flags);                  /* sprite, first corner */
        W(0xF3, 0x3F8000u); W(0xF4, 0x3F8000u);              /* 1.0 */
        W(0xF0, 0x8000u + (MIP_X + size) * 16u); W(0xF1, 0x8000u + (MIP_Y + size) * 16u);
        W(0xF7, (7u << 8) | 0xFFu | flags);                  /* second corner flushes the sprite */
    } else {
        W(0x12, 3u | (3u << 7) | (1u << 23));   /* float UV and position, through mode */
        prim(6u, 2u, MIP_VERTS);
    }
    W(0x0C, 0u);
    if (ge_run_list(MIP_LIST, 0) != 0) return 0u;
    uint32_t first = MEM_R32(MIP_FB + (MIP_Y * 512u + MIP_X) * 4u) & 0x00FFFFFFu;
    for (unsigned y = 0; y < size; y++)
        for (unsigned x = 0; x < size; x++)
            if ((MEM_R32(MIP_FB + ((MIP_Y + y) * 512u + MIP_X + x) * 4u) & 0x00FFFFFFu) != first)
                return 0u;
    return first;
}

static uint32_t mip_draw(uint32_t max_level, uint32_t level_word, uint32_t min_filter,
                         unsigned size) {
    return mip_draw_route(MIP_ROUTE_PRIM, max_level, level_word, min_filter, size);
}

/* The mip-use count is the evidence that a title enables mipmapping (#1373), so a route that
 * textures without reaching it would let a title mip unseen. Each route counts once with a mip
 * filter and a highest level above 0, and not at all with either off; the "GE mip:" line has
 * printed exactly once by the end, however many draws were counted. */
static void test_ge_mip_use_trace_routes(void) {
    static const struct { int route; const char *name; } routes[] = {
        { MIP_ROUTE_PRIM, "prim" }, { MIP_ROUTE_PATCH, "patch" },
        { MIP_ROUTE_IMMEDIATE, "immediate" },
    };
    static const struct { uint32_t max_level, min_filter; unsigned long want; } states[] = {
        { 3u, 4u, 1ul },   /* mip filter and a highest level: counted */
        { 0u, 4u, 0ul },   /* highest level 0 */
        { 3u, 1u, 0ul },   /* no mip filter bit */
    };
    mip_fill_texture();
    for (size_t r = 0; r < sizeof routes / sizeof routes[0]; r++) {
        for (size_t k = 0; k < sizeof states / sizeof states[0]; k++) {
            unsigned long before = g_ge_mip_draws;
            (void)mip_draw_route(routes[r].route, states[k].max_level, MIP_LEVEL(1, 16),
                                 states[k].min_filter, 8u);
            char detail[128];
            snprintf(detail, sizeof detail, "%s route, max level %u, filter %u: count rose by %lu, expected %lu",
                     routes[r].name, (unsigned)states[k].max_level, (unsigned)states[k].min_filter,
                     g_ge_mip_draws - before, states[k].want);
            expect(g_ge_mip_draws - before == states[k].want, "mip_use_trace_routes", detail);
        }
    }
    expect(g_ge_mip_lines_printed == 1ul, "mip_use_trace_once",
           "the GE mip: line was not printed exactly once");
}

static void test_ge_texture_mip_levels(void) {
    enum { RED = 0, GREEN = 1, BLUE = 2 };
    static const struct {
        const char *name;
        uint32_t max_level, level_word, min_filter;
        unsigned size;
        int level;            /* expected level, or -1 for the blend below */
    } cases[] = {
        /* Mip levels off: level 0 exactly as before. */
        { "highest_level_0",  0u, MIP_LEVEL(1, 16),  4u, 8u,  RED },
        { "non_mip_filter",   3u, MIP_LEVEL(1, 32),  1u, 8u,  RED },
        { "constant_level_0", 3u, MIP_LEVEL(1, 0),   4u, 8u,  RED },
        /* Constant: the bias names the level, clamped to the highest level. */
        { "constant_level_1", 3u, MIP_LEVEL(1, 16),  4u, 8u,  GREEN },
        { "constant_level_2", 3u, MIP_LEVEL(1, 32),  4u, 8u,  BLUE },
        { "constant_clamped", 1u, MIP_LEVEL(1, 48),  4u, 8u,  GREEN },
        /* Automatic: log2 of the texel footprint plus the bias. */
        { "auto_magnified",   3u, MIP_LEVEL(0, 0),   4u, 16u, RED },
        { "auto_one_to_one",  3u, MIP_LEVEL(0, 0),   4u, 8u,  RED },
        { "auto_minified",    3u, MIP_LEVEL(0, 0),   4u, 2u,  BLUE },
        { "auto_biased",      3u, MIP_LEVEL(0, -16), 4u, 2u,  GREEN },
        { "auto_clamped",     1u, MIP_LEVEL(0, 0),   4u, 1u,  GREEN },
        /* Slope with TEXLODSLOPE 0: its logarithm is -2032, so level 0. */
        { "slope_level_0",    3u, MIP_LEVEL(2, 16),  4u, 2u,  RED },
        /* GU_NEAREST_MIPMAP_LINEAR at lod 0.5: half of level 0 and half of level 1. */
        { "linear_between",   3u, MIP_LEVEL(1, 8),   6u, 8u,  -1 },
    };
    mip_fill_texture();
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint32_t want = (cases[i].level >= 0 ? k_mip_color[cases[i].level] : 0x007F7Fu) & 0x00FFFFFFu;
        unsigned long mip_before = g_ge_mip_draws;
        uint32_t got = mip_draw(cases[i].max_level, cases[i].level_word, cases[i].min_filter,
                                cases[i].size);
        char detail[128];
        snprintf(detail, sizeof detail, "sprite colour 0x%08" PRIx32 ", expected 0x%08" PRIx32,
                 got, want);
        expect(got == want, cases[i].name, detail);
        /* The run's mip-use trace counts exactly the draws that sample levels: a mip filter and a
         * highest level above 0. It is the evidence that a title enables mipmapping (#1373). */
        unsigned long mip_expected = ((cases[i].min_filter & 4u) && cases[i].max_level > 0u) ? 1ul : 0ul;
        snprintf(detail, sizeof detail, "g_ge_mip_draws rose by %lu, expected %lu",
                 g_ge_mip_draws - mip_before, mip_expected);
        expect(g_ge_mip_draws - mip_before == mip_expected, "mip_use_trace", detail);
    }
    printf("MIP cases=%u (software rasterizer)\n", (unsigned)(sizeof cases / sizeof cases[0]));
}

/* The depth-surface span is cached per (base, stride) because every CPU VRAM access asks for it
 * (#1296). Six surfaces cycled through a smaller cache must keep returning their own bounds, each
 * the exact XOR image of the surface's even addresses (widened by at most one byte per end), and an
 * unsupported stride must stay refused after a cached success for the same base. */
static void test_ge_zbuf_span_bounds_cache(void) {
    static const struct { uint32_t base, stride; } surfaces[] = {
        { 0x040CE000u, 512u }, { 0x04088000u, 480u }, { 0x04000000u, 512u },
        { 0x04044000u, 256u }, { 0x04110000u, 512u }, { 0x040CC000u, 64u },
    };
    enum { N = sizeof surfaces / sizeof surfaces[0] };
    uint32_t want_first[N], want_end[N];
    for (unsigned i = 0; i < N; i++) {
        uint32_t lo = UINT32_MAX, hi = 0u;
        for (uint32_t p = 0; p < surfaces[i].stride * 272u; p++) {
            uint32_t a = (surfaces[i].base + 2u * p) ^ 0x2040u;
            if (a < lo) lo = a;
            if (a > hi) hi = a;
        }
        uint32_t first = 0u, end = 0u;
        expect(ge_zbuf_guest_span_bounds(surfaces[i].base, surfaces[i].stride, &first, &end),
               "zbuf_span_cache", "a supported depth surface had no span");
        expect(first <= lo && lo - first <= 1u && end >= hi + 2u && end - (hi + 2u) <= 1u,
               "zbuf_span_cache", "the span is not the XOR image of the surface");
        want_first[i] = first;
        want_end[i] = end;
    }
    for (unsigned round = 0; round < 5u; round++) {
        for (unsigned k = 0; k < N; k++) {
            unsigned i = (k * 5u + round) % N;
            uint32_t first = 0u, end = 0u;
            int ok = ge_zbuf_guest_span_bounds(surfaces[i].base, surfaces[i].stride, &first, &end);
            expect(ok && first == want_first[i] && end == want_end[i], "zbuf_span_cache",
                   "a repeated query returned another surface's span");
            uint32_t ignored_first, ignored_end;
            expect(!ge_zbuf_guest_span_bounds(surfaces[i].base, 513u, &ignored_first, &ignored_end),
                   "zbuf_span_cache", "a stride above 512 was accepted after a cached query");
        }
    }
}

static void test_ge_vram_cpu_visibility(void) {
    build_vram_depth_list_at(0x0CC000u);
    expect(ge_run_list(LIST_VRAM_DEPTH, 0) == 0, "vram_depth", "depth list did not reach END");

    static const struct {
        unsigned x, y;
        uint16_t z;
    } points[] = {
        { 0u, 0u, 0x1111u },
        { 64u, 8u, 0x2222u },
        { 200u, 100u, 0x3333u },
    };
    const uint32_t zbase = 0x040CC000u;
    for (size_t i = 0; i < sizeof points / sizeof points[0]; i++) {
        uint32_t linear = (points[i].y * 512u + points[i].x) * 2u;
        uint32_t stored = zbase + (linear ^ 0x2040u);
        uint32_t alias_linear = 0x04200000u + (zbase - 0x04000000u) + linear;
        char detail[112];
        snprintf(detail, sizeof detail, "swizzled depth at x=%u y=%u",
                 points[i].x, points[i].y);
        expect(MEM_R16(stored) == points[i].z, "vram_depth", detail);
        snprintf(detail, sizeof detail, "linear mirror depth at x=%u y=%u",
                 points[i].x, points[i].y);
        expect(MEM_R16(alias_linear) == points[i].z, "vram_depth", detail);
    }
    uint32_t clear_linear = (200u * 512u + 300u) * 2u;
    expect(MEM_R16(zbase + (clear_linear ^ 0x2040u)) == 0u &&
           MEM_R16(0x04200000u + (zbase - 0x04000000u) + clear_linear) == 0u,
           "vram_depth", "GE depth clear is visible in the base and linear mirror views");
    expect(MEM_R32(0x04100000u + (200u * 512u + 300u) * 4u) == 0xFF646464u,
           "vram_color", "CPU load after the software GE clear sees the linear colour row");

    MEM_W16(zbase + 0x2040u, 0xFFFFu);
    uint32_t color_addr = 0x04100000u + (2u * 512u + 2u) * 4u;
    MEM_W32(color_addr, 0xFF112233u);
    build_vram_cpu_write_list();
    expect(ge_run_list(LIST_VRAM_CPU_WRITE, 0) == 0,
           "vram_cpu_write", "post-store GE list did not reach END");
    expect(MEM_R16(zbase + 0x2040u) == 0x4444u,
           "vram_cpu_write", "next GE depth test observes the CPU-written depth value");
    expect(MEM_R32(color_addr) == 0xFF102030u,
           "vram_cpu_write", "next GE logic-op draw observes the CPU-written color value");

    /* zbp 0x0cc040 (bit 6 set) is still refused: the PSP wrote no depth there (2026-10-21). zbp 0x0ce000
     * (bit 13 set) is measured and accepted, so the depth-tested draw runs. */
    static const uint32_t ambiguous_zbp[] = { 0x0CC040u, 0x0CE000u };
    for (size_t i = 0; i < sizeof ambiguous_zbp / sizeof ambiguous_zbp[0]; i++) {
        uint32_t color_pixel = 0x04100000u + (20u * 512u + 20u) * 4u;
        MEM_W32(color_pixel, 0xFFABCDEFu);
        build_vram_ambiguous_depth_list(ambiguous_zbp[i],
                                        VRAM_TEST_VERTS + 0x300u + (uint32_t)i * 32u);
        expect(ge_run_list(LIST_VRAM_AMBIGUOUS, 0) == 0,
               "vram_depth_boundary", "ambiguous depth list did not reach END");
        if (ambiguous_zbp[i] == 0x0CC040u)
            expect(MEM_R32(color_pixel) == 0xFFABCDEFu,
                   "vram_depth_boundary", "bit-6 depth placement rejects the whole depth-tested primitive");
        else
            expect(MEM_R32(color_pixel) == 0xFF010203u,
                   "vram_depth_boundary", "bit-13 depth placement runs the depth-tested primitive (PSP-measured)");
    }
}

/* The colour target's pixels are read and written in place when that is exactly what the
 * guest accessors would do, and through the accessors otherwise. A depth-tested full-screen
 * sprite into a target clear of the depth surface takes no accessor call at all (each one
 * cost about 3000 host cycles); a target that overlaps the depth surface keeps every access
 * on the accessor path, so the software depth is still written back before it is read. */
#define LIST_FB_DIRECT 0x08116000u
static void test_ge_fb_direct_access(void) {
    static const uint32_t through16 = (7u << 2) | (2u << 7) | (1u << 23);
    uint32_t vertex = VRAM_TEST_VERTS + 0x400u;
    uint32_t sprite = vertex;
    vertex = depth_test_vertex(vertex, 0u, 0u, 0x5555u, 0xFF405060u);
    (void)depth_test_vertex(vertex, 480u, 272u, 0x5555u, 0xFF405060u);
    s_pc = LIST_FB_DIRECT;
    W(0x10, 0x080000u);
    W(0x9C, 0x00000000u);                /* colour base: 0x04000000 */
    W(0x9D, 512u);
    W(0xD2, 3u);                         /* 8888 */
    W(0x9E, 0x110000u);                  /* depth base: 0x04110000, clear of the colour */
    W(0x9F, 512u);
    W(0xD4, 0u);
    W(0xD5, (271u << 10) | 479u);
    W(0x23, 1u);                         /* depth test enabled */
    W(0xDE, 1u);                         /* always pass */
    W(0x21, 1u);                         /* blending enabled */
    W(0xDF, 0x000032u);                  /* src alpha, one minus src alpha, add */
    W(0x28, 0u);
    W(0xD3, 0u);
    W(0x12, through16);
    prim(6u, 2u, sprite);
    W(0x0C, 0u);

    unsigned long long direct0, accessor0, direct1, accessor1;
    ge_fb_access_counts(&direct0, &accessor0);
    expect(ge_run_list(LIST_FB_DIRECT, 0) == 0, "fb_direct", "list did not reach END");
    ge_fb_access_counts(&direct1, &accessor1);
    expect(direct1 - direct0 >= 480ull * 272ull * 2ull, "fb_direct",
           "every pixel's read and write of a target clear of depth is made in place");
    expect(accessor1 == accessor0, "fb_direct",
           "a target clear of depth takes no guest-accessor call");
    expect((MEM_R32(0x04000000u + (100u * 512u + 200u) * 4u) & 0x00FFFFFFu) == 0x00405060u,
           "fb_direct", "the in-place store is what the CPU then reads");
    uint32_t linear = (100u * 512u + 200u) * 2u;
    expect(MEM_R16(0x04110000u + (linear ^ 0x2040u)) == 0x5555u,
           "fb_direct", "the depth written beside the in-place colour reaches guest VRAM");

    /* build_vram_depth_list_at(0x0CC000): its depth surface runs past 0x04100000, the start
     * of its colour target, so those colour accesses must stay on the accessor path. */
    build_vram_depth_list_at(0x0CC000u);
    ge_fb_access_counts(&direct0, &accessor0);
    expect(ge_run_list(LIST_VRAM_DEPTH, 0) == 0, "fb_direct_overlap", "list did not reach END");
    ge_fb_access_counts(&direct1, &accessor1);
    expect(accessor1 > accessor0, "fb_direct_overlap",
           "colour accesses that overlap the depth surface keep the accessor path");
}

/* Immediate-mode vertices arrive as VSCX/VSCY/VSCZ/VTCS/VTCT/VTCQ/VCV/VFC/VSCV
 * register writes followed by a VAP submit. VAP bit 10:8 names PRIM type 0-6,
 * or 7 to continue the previous stream; bits 23:11 carry the primitive flags
 * replicated from the through-mode front end. This list exercises every type,
 * keep-previous submission, the flush caused by starting another primitive, and
 * the flush caused by a non-vertex command. */
#define IMMEDIATE_FB 0x04160000u

static unsigned s_hook_prims;
static GeVtx s_hook_last_tri_c;   /* third vertex of the last triangle a hook took */
static int hook_tri(const GeVtx *a, const GeVtx *b, const GeVtx *c, int persp);
static int hook_sprite(const GeVtx *a, const GeVtx *b, int persp);
static int hook_line(const GeVtx *a, const GeVtx *b, int persp);
static int hook_point(const GeVtx *a, int persp);
static void hook_flush(const char *reason);
static void hook_vram_dirty(uint32_t addr, uint32_t bytes);

static uint32_t immediate_coord(int value) {
    return 0x8000u + (uint32_t)value * 16u;
}

static void immediate_vertex(uint32_t type, int x, int y) {
    W(0xF0, immediate_coord(x));   /* VSCX */
    W(0xF1, immediate_coord(y));   /* VSCY */
    W(0xF2, 0u);                  /* VSCZ */
    W(0xF7, (type << 8) | 0xFFu); /* VAP: primitive type, opaque alpha */
}

static uint32_t immediate_pixel(unsigned x, unsigned y) {
    return IMMEDIATE_FB + (y * 512u + x) * 4u;
}

static void test_ge_immediate_mode(void) {
    static const struct {
        unsigned x, y;
        const char *name;
    } probes[] = {
        { 20u, 20u, "point" },
        { 50u, 30u, "line" },
        { 90u, 30u, "line_strip_first" },
        { 110u, 30u, "line_strip_second" },
        { 30u, 70u, "triangle" },
        { 170u, 70u, "triangle_strip_first" },
        { 180u, 70u, "triangle_strip_second" },
        { 270u, 70u, "triangle_fan_first" },
        { 280u, 70u, "triangle_fan_second" },
        { 350u, 70u, "sprite" },
    };
    const uint32_t sentinel = 0xA5A5A5A5u;
    for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++)
        MEM_W32(immediate_pixel(probes[i].x, probes[i].y), sentinel);

    s_pc = LIST_IMMEDIATE;
    W(0x10, 0x080000u);                    /* BASE */
    W(0x13, 0u);                           /* OFFSETADDR */
    W(0x9C, IMMEDIATE_FB & 0x00FFFFFFu);   /* FRAMEBUFPTR */
    W(0x9D, 512u);                         /* FRAMEBUFWIDTH */
    W(0xD2, 3u);                           /* PIXFORMAT 8888 */
    W(0xD4, 0u);                           /* SCISSOR1 */
    W(0xD5, (271u << 10) | 479u);          /* SCISSOR2 */
    W(0x1E, 0u);                           /* texture off */
    W(0x23, 0u);                           /* depth test off */
    W(0xDB, 0u);                           /* alpha test off */
    W(0xDC, 0u);                           /* stencil off */
    W(0xDF, 0u);                           /* blend off */
    W(0xE6, 0u);                           /* logic op off */
    W(0xE8, 0u);                           /* colour mask open */
    W(0xE9, 0u);                           /* alpha mask open */
    W(0xD3, 0u);                           /* not clear mode */
    W(0x12, 1u << 23);                     /* through-position type */
    W(0xF2, 0u);                           /* VSCZ */
    W(0xF6, 0x00FFFFFFu);                  /* VCV: white */
    W(0xF8, 255u);                         /* VFC */

    immediate_vertex(0u, 20, 20);          /* point */
    immediate_vertex(1u, 40, 30);          /* independent line */
    immediate_vertex(7u, 60, 30);
    immediate_vertex(2u, 80, 30);          /* line strip, flushed by type change */
    immediate_vertex(7u, 100, 30);
    immediate_vertex(7u, 120, 30);
    immediate_vertex(3u, 20, 60);          /* triangle, auto-flushed at 3 vertices */
    immediate_vertex(7u, 60, 60);
    immediate_vertex(7u, 20, 100);
    immediate_vertex(4u, 160, 60);         /* triangle strip, flushed by NOP */
    immediate_vertex(7u, 200, 60);
    immediate_vertex(7u, 160, 100);
    immediate_vertex(7u, 200, 100);
    W(0x00, 0u);                           /* NOP between strips */
    immediate_vertex(5u, 260, 60);         /* triangle fan, flushed by type change */
    immediate_vertex(7u, 300, 60);
    immediate_vertex(7u, 260, 100);
    immediate_vertex(7u, 300, 100);
    immediate_vertex(6u, 340, 60);         /* sprite, auto-flushed at 2 vertices */
    immediate_vertex(7u, 380, 90);
    W(0x0C, 0u);                           /* END */

    expect(ge_run_list(LIST_IMMEDIATE, 0) == 0, "immediate",
           "immediate-mode list did not reach END");
    for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++) {
        char detail[96];
        snprintf(detail, sizeof detail, "%s probe pixel was not drawn in white",
                 probes[i].name);
        expect((MEM_R32(immediate_pixel(probes[i].x, probes[i].y)) & 0x00FFFFFFu) ==
                   0x00FFFFFFu,
               "immediate", detail);
    }

    GeGpuHooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.tri = hook_tri;
    hooks.sprite = hook_sprite;
    hooks.line = hook_line;
    hooks.point = hook_point;
    hooks.flush = hook_flush;
    hooks.vram_dirty = hook_vram_dirty;
    unsigned hook_before = s_hook_prims;
    ge_set_gpu_hooks(&hooks);
    expect(ge_run_list(LIST_IMMEDIATE, 0) == 0, "immediate_gpu_hooks",
           "GPU-hook list did not reach END");
    ge_set_gpu_hooks(NULL);
    expect(s_hook_prims - hook_before == 10u, "immediate_gpu_hooks",
           "immediate primitives did not reach the GPU hook in command-list order");
}

/* An immediate triangle strip keeps its previous two vertices across a command that is
 * not a vertex register: the vertex after it still completes a triangle (PPSSPP's software
 * renderer assembles each VAP as it arrives). VTCS/VTCT are normalized and reach the
 * through-mode raster path as texels of the level-0 size. */
static void test_ge_immediate_stream(void) {
    s_pc = LIST_IMMEDIATE_STREAM;
    W(0x12, 1u << 23);                     /* through-position type */
    W(0xB8, (5u << 8) | 6u);               /* TEXSIZE0: 64 x 32 */
    W(0xF3, 0x3F0000u);                    /* VTCS 0.5 */
    W(0xF4, 0x3E8000u);                    /* VTCT 0.25 */
    immediate_vertex(4u, 160, 160);        /* triangle strip */
    immediate_vertex(7u, 200, 160);
    immediate_vertex(7u, 160, 200);        /* first triangle */
    W(0xE8, 0u);                           /* a state command inside the strip */
    immediate_vertex(7u, 200, 200);        /* second triangle, from the kept pair */
    W(0x0C, 0u);                           /* END */

    GeGpuHooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.tri = hook_tri;
    hooks.sprite = hook_sprite;
    hooks.line = hook_line;
    hooks.point = hook_point;
    hooks.flush = hook_flush;
    hooks.vram_dirty = hook_vram_dirty;
    unsigned hook_before = s_hook_prims;
    ge_set_gpu_hooks(&hooks);
    expect(ge_run_list(LIST_IMMEDIATE_STREAM, 0) == 0, "immediate_stream",
           "immediate strip list did not reach END");
    ge_set_gpu_hooks(NULL);
    expect(s_hook_prims - hook_before == 2u, "immediate_stream",
           "a command inside an immediate strip dropped the strip's kept vertices");
    expect(s_hook_last_tri_c.u == 32.0f && s_hook_last_tri_c.v == 8.0f, "immediate_stream",
           "immediate texture coordinates were not scaled to level-0 texels");
}

/* ---- GPU backend stand-in (the hook contract src/rt/gpu_sdl3vk/ge_gpu.c fills) ---- */

static unsigned s_hook_flushes;
static int hook_tri(const GeVtx *a, const GeVtx *b, const GeVtx *c, int persp) {
    (void)a; (void)b; (void)persp;
    s_hook_last_tri_c = *c;
    s_hook_prims++;
    return 1;
}
static int hook_sprite(const GeVtx *a, const GeVtx *b, int persp) {
    (void)a; (void)b; (void)persp;
    s_hook_prims++;
    return 1;
}
static int hook_line(const GeVtx *a, const GeVtx *b, int persp) {
    (void)a; (void)b; (void)persp;
    s_hook_prims++;
    return 1;
}
static int hook_point(const GeVtx *a, int persp) {
    (void)a; (void)persp;
    s_hook_prims++;
    return 1;
}
static void hook_flush(const char *reason) {
    (void)reason;
    s_hook_flushes++;
}
static void hook_vram_dirty(uint32_t addr, uint32_t bytes) {
    (void)addr; (void)bytes;
}

/* ---- Checks ---- */

static int s_failures;
static unsigned s_scenarios, s_faults;

static void expect(int ok, const char *scenario, const char *what) {
    if (!ok) {
        s_failures++;
        fprintf(stderr, "FAIL %s: %s\n", scenario, what);
    }
}

/* Widescreen Fill (#1881), the software rasterizer's wing canvases. One list on a display
 * framebuffer: sceGuClear-style 64-pixel clear strips, a transform-mode quad from guest x -240
 * to 720 (rows 68..203), the band shapes below, and a through-mode bar over the screen's width
 * (rows 100..139); the fade variant ends with an untextured whole-screen sprite.
 *
 * The band shapes pin where the wings put geometry. Rows 4..63 hold shapes in or across the
 * left edge, rows 208..267 shapes in or across the right edge: flat triangle pairs, a gouraud
 * pair across the edge, and transform-mode sprites, one with a corner in (-1, 0). Every corner
 * lies at a non-integer multiple of 1/16 that the identity transform keeps exact. Each shape
 * is drawn a second time WIDE_K pixels toward the centre, wholly on screen, so the authentic
 * frame shows what the rasterizer gives that shape whole: left-wing canvas column c must hold
 * frame column c - D + WIDE_K, and right-wing column c frame column c - D + 480 - WIDE_K.
 * Moving a wing by a pixel, dropping one, or rounding a sprite corner differently fails that.
 *
 * At each window aspect, with Fill on, guest VRAM must be byte-identical to Fill off, the
 * wings must hold the quad, the shapes as mapped above and the clear colour elsewhere, the bar
 * must reach no wing pixel, the fade must cover them, a scissor narrower than the screen must
 * keep every draw out of them, a shown canvas must be cleared by its next wing draw, the GE
 * clock must not change, and the vertices a backend's wing hook receives must be the guest's,
 * snapped to the 28.4 grid and shifted by whole pixels. gpu-coherence-selftest runs the same
 * list on the Vulkan backend and compares the canvases. */
#define LIST_WIDE      0x08130000u
#define WIDE_VERTS     0x08134000u
#define WIDE_FB        0x04000000u
#define WIDE_VRAM      0x04000000u
#define WIDE_VRAM_SIZE 0x00200000u
#define WIDE_D         76
#define WIDE_K         270
#define WIDE_BG        0xff302010u
#define WIDE_QUAD      0xff20c040u
#define WIDE_BAR       0xff0000ffu
#define WIDE_FADE      0xff808080u
enum { WF_FADE = 1, WF_NARROW = 2, WF_NOCLEAR = 4, WF_CAPTURE = 8 };

typedef struct { float x0, x1, y0, y1; uint32_t c0, c1; int sprite, right; } WideShape;
static const WideShape k_wide_shapes[] = {
    { -50.625f,  -30.0f,     4.25f,  17.0f,   0xff4060a0u, 0xff4060a0u, 0, 0 },
    { -20.625f,   19.6875f, 21.25f,  34.0f,   0xff1080f0u, 0xfff08010u, 0, 0 },
    { -11.25f,    -0.9375f, 38.25f,  51.0f,   0xffa0a020u, 0xffa0a020u, 1, 0 },
    {  -0.9375f,  18.75f,   55.25f,  63.75f,  0xff20a0a0u, 0xff20a0a0u, 1, 0 },
    { 510.0f,    530.625f,  208.25f, 221.0f,  0xff6040a0u, 0xff6040a0u, 0, 1 },
    { 460.3125f, 500.625f,  225.25f, 238.0f,  0xfff08010u, 0xff1080f0u, 0, 1 },
    { 491.25f,   501.5625f, 242.25f, 255.0f,  0xff20a0a0u, 0xff20a0a0u, 1, 1 },
    { 460.3125f, 481.875f,  259.25f, 267.75f, 0xffa0a020u, 0xffa0a020u, 1, 1 },
};
#define WIDE_NSHAPES (sizeof k_wide_shapes / sizeof k_wide_shapes[0])

static uint32_t s_wide_vc;

static void wide_vtx(uint32_t color, float x, float y) {
    uint8_t *d = (uint8_t *)SR_HOST(s_wide_vc);
    const float pos[3] = { x, y, 0.0f };
    memcpy(d, &color, 4);
    memcpy(d + 4, pos, 12);
    s_wide_vc += 16u;
}

static void wide_matrix(uint32_t cmd, const float *m, int n) {
    W(cmd, 0u);
    for (int i = 0; i < n; i++) W(cmd + 1u, f24(m[i]));
}

/* One shape, moved `shift` guest pixels, in clip space under the identity transform. */
static void wide_shape(const WideShape *sh, float shift) {
    const float x0 = (sh->x0 + shift - 240.0f) / 240.0f, x1 = (sh->x1 + shift - 240.0f) / 240.0f;
    const float y0 = (136.0f - sh->y0) / 136.0f, y1 = (136.0f - sh->y1) / 136.0f;
    const uint32_t at = s_wide_vc;
    W(0x50, sh->c0 != sh->c1 ? 1u : 0u);   /* gouraud for the gradient pairs */
    if (sh->sprite) {
        wide_vtx(sh->c1, x0, y0); wide_vtx(sh->c1, x1, y1);
    } else {
        wide_vtx(sh->c0, x0, y0); wide_vtx(sh->c1, x1, y0); wide_vtx(sh->c0, x0, y1);
        wide_vtx(sh->c1, x1, y0); wide_vtx(sh->c1, x1, y1); wide_vtx(sh->c0, x0, y1);
    }
    W(0x01, at & 0x00FFFFFFu);
    W(0x04, sh->sprite ? (6u << 16) | 2u : (3u << 16) | 6u);
}

static void build_wide_list(int flags) {
    const uint32_t through = (7u << 2) | (3u << 7) | (1u << 23), transform = (7u << 2) | (3u << 7);
    s_pc = LIST_WIDE;
    s_wide_vc = WIDE_VERTS;
    W(0x10, 0x080000u);                    /* BASE: vertices at 0x08xxxxxx */
    W(0x13, 0u);
    W(0x9C, WIDE_FB & 0x00FFFFFFu);
    W(0x9D, 512u);
    W(0xD2, 3u);
    W(0x15, 0u);
    W(0x16, (271u << 10) | 479u);
    W(0xD4, 0u);
    W(0xD5, (271u << 10) | ((flags & WF_NARROW) ? 400u : 479u));
    W(0xD6, 0u);
    W(0xD7, 0xFFFFu);
    for (uint32_t cmd = 0x17; cmd <= 0x28; cmd++) W(cmd, 0u);   /* every unit off */
    W(0x50, 0u);                           /* flat shading */
    W(0xE8, 0u);
    W(0xE9, 0u);

    uint32_t at = s_wide_vc;
    if (!(flags & (WF_NOCLEAR | WF_CAPTURE))) {
        for (int i = 0; i < 8; i++) {
            wide_vtx(WIDE_BG, (float)(i * 64), 0.0f);
            wide_vtx(WIDE_BG, (float)(i * 64 + 64), 272.0f);
        }
        W(0xD3, 0x301u);                   /* clear colour and alpha */
        W(0x12, through);
        W(0x01, at & 0x00FFFFFFu);
        W(0x04, (6u << 16) | 16u);
        W(0xD3, 0u);
    }

    static const float ident43[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
    static const float ident44[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    wide_matrix(0x3A, ident43, 12);
    wide_matrix(0x3C, ident43, 12);
    wide_matrix(0x3E, ident44, 16);
    W(0x42, f24(240.0f));
    W(0x43, f24(-136.0f));
    W(0x44, f24(-32000.0f));
    W(0x45, f24(2048.0f));
    W(0x46, f24(2048.0f));
    W(0x47, f24(32767.5f));
    W(0x4C, (2048u - 240u) << 4);
    W(0x4D, (2048u - 136u) << 4);
    W(0x12, transform);
    if (flags & WF_CAPTURE) {
        /* Corners off the 1/16 grid (guest x about -50.3, 39.6, 520.4 and -10.3), so the
         * 28.4 snap before the wing shift is visible in what the wing hook receives. */
        at = s_wide_vc;
        wide_vtx(WIDE_QUAD, -1.2095833f, 0.5f); wide_vtx(WIDE_QUAD, -0.835f, 0.5f);
        wide_vtx(WIDE_QUAD, -1.2095833f, -0.5f);
        wide_vtx(WIDE_QUAD, 1.2016f, 0.5f); wide_vtx(WIDE_QUAD, 0.835f, 0.5f);
        wide_vtx(WIDE_QUAD, 1.2016f, -0.5f);
        W(0x01, at & 0x00FFFFFFu);
        W(0x04, (3u << 16) | 6u);
        at = s_wide_vc;
        wide_vtx(WIDE_BAR, -1.0429f, 0.9f); wide_vtx(WIDE_BAR, -0.5f, 0.8f);
        W(0x01, at & 0x00FFFFFFu);
        W(0x04, (6u << 16) | 2u);
        W(0x0F, 0u);
        W(0x0C, 0u);
        return;
    }
    if (!(flags & WF_NOCLEAR)) {
        at = s_wide_vc;                    /* clip x -2..2 is guest x -240..720 */
        wide_vtx(WIDE_QUAD, -2.0f, 0.5f); wide_vtx(WIDE_QUAD, 2.0f, 0.5f); wide_vtx(WIDE_QUAD, -2.0f, -0.5f);
        wide_vtx(WIDE_QUAD, 2.0f, 0.5f); wide_vtx(WIDE_QUAD, 2.0f, -0.5f); wide_vtx(WIDE_QUAD, -2.0f, -0.5f);
        W(0x01, at & 0x00FFFFFFu);
        W(0x04, (3u << 16) | 6u);
    }
    for (size_t i = 0; i < WIDE_NSHAPES; i++) {
        wide_shape(&k_wide_shapes[i], 0.0f);
        wide_shape(&k_wide_shapes[i], k_wide_shapes[i].right ? -(float)WIDE_K : (float)WIDE_K);
    }
    W(0x50, 0u);

    at = s_wide_vc;
    wide_vtx(WIDE_BAR, 0.0f, 100.0f); wide_vtx(WIDE_BAR, 480.0f, 140.0f);
    if (flags & WF_FADE) { wide_vtx(WIDE_FADE, 0.0f, 0.0f); wide_vtx(WIDE_FADE, 480.0f, 272.0f); }
    W(0x12, through);
    W(0x01, at & 0x00FFFFFFu);
    W(0x04, (6u << 16) | ((flags & WF_FADE) ? 4u : 2u));
    W(0x0F, 0u);
    W(0x0C, 0u);
}

/* Run the list with wing width `wing` from zeroed VRAM and copy all of guest VRAM to `vram`. */
static void run_wide_list(int wing, int flags, uint8_t *vram) {
    memset(SR_HOST(WIDE_VRAM), 0, WIDE_VRAM_SIZE);
    expect(ge_wide_set_wing(wing) == wing, "wide_fill", "the wing width is taken as given");
    ge_wide_note_display(WIDE_FB);
    build_wide_list(flags);
    expect(ge_run_list(LIST_WIDE, 0) == 0, "wide_fill", "list did not reach END");
    memcpy(vram, SR_HOST(WIDE_VRAM), WIDE_VRAM_SIZE);
}

/* The colour canvas pixel (x, y) must hold, from the authentic frame `frame` of the same list. */
static uint32_t wide_want(const uint32_t *frame, int wing, int flags, uint32_t x, uint32_t y) {
    const uint32_t d = (uint32_t)wing;
    if (x >= 2u * d) return 0u;
    if (flags & WF_FADE) return WIDE_FADE;
    if (y >= 68u && y <= 203u) return (flags & WF_NOCLEAR) ? 0u : WIDE_QUAD;
    if (y < 68u && x < d) return frame[y * 512u + x - d + WIDE_K];
    if (y > 203u && x >= d) return frame[y * 512u + x - d + 480u - WIDE_K];
    return (flags & WF_NOCLEAR) ? 0u : WIDE_BG;
}

static size_t wide_check_canvas(const char *name, int wing, int flags, const uint32_t *frame) {
    int fmt = -1;
    const uint32_t *c = ge_wide_canvas(WIDE_FB, &fmt);
    if (!c || fmt != 3) { expect(0, "wide_fill", "Fill on draws the canvas"); return 1; }
    size_t bad = 0;
    for (uint32_t y = 0; y < 272u; y++)
        for (uint32_t x = 0; x < GE_WIDE_CANVAS_W; x++) {
            const uint32_t want = wide_want(frame, wing, flags, x, y);
            if (c[y * GE_WIDE_CANVAS_W + x] != want) {
                if (!bad)
                    fprintf(stderr, "wide_fill: %s canvas (%u,%u) = %08x, want %08x (flags=%d)\n",
                            name, x, y, (unsigned)c[y * GE_WIDE_CANVAS_W + x], (unsigned)want, flags);
                bad++;
            }
        }
    /* Spot checks the mapping cannot satisfy vacuously: each shape is really there. */
    const uint32_t d = (uint32_t)wing;
    if (!(flags & WF_FADE)) {
        static const struct { int gx, y, shape; } spots[] = {
            { -40, 10, 0 }, { -12, 45, 2 }, { -1, 60, 3 }, { 520, 215, 4 }, { 491, 250, 6 }, { 480, 263, 7 },
        };
        for (size_t i = 0; i < sizeof spots / sizeof spots[0]; i++) {
            const uint32_t x = (uint32_t)(spots[i].gx < 0 ? spots[i].gx + wing : spots[i].gx - 480 + wing);
            if (x >= 2u * d) continue;
            /* RGB only: without a clear the frame's alpha (stencil) stays 0 under the shapes. */
            if ((c[(uint32_t)spots[i].y * GE_WIDE_CANVAS_W + x] ^ k_wide_shapes[spots[i].shape].c1) & 0x00FFFFFFu) {
                fprintf(stderr, "wide_fill: %s guest (%d,%d) is %08x, not shape %d\n", name, spots[i].gx,
                        spots[i].y, (unsigned)c[(uint32_t)spots[i].y * GE_WIDE_CANVAS_W + x], spots[i].shape);
                bad++;
            }
        }
        /* The sprite from -11.25 covers guest column -12, not -13 (floor, not truncation). */
        if (d >= 13u && !((c[45u * GE_WIDE_CANVAS_W + d - 13u] ^ k_wide_shapes[2].c1) & 0x00FFFFFFu)) {
            fprintf(stderr, "wide_fill: %s sprite reaches guest column -13\n", name);
            bad++;
        }
    }
    return bad;
}

/* Wing hooks of a fake backend that records what it is handed; the guest's own draws are
 * declined, so the software rasterizer draws the authentic frame. */
static GeVtx s_cap_auth[3][3], s_cap_wing[3][3];
static int s_cap_cols[3][2], s_cap_nauth, s_cap_nwing;
static int cap_tri(const GeVtx *a, const GeVtx *b, const GeVtx *c, int persp) {
    (void)persp;
    if (s_cap_nauth < 3) { s_cap_auth[s_cap_nauth][0] = *a; s_cap_auth[s_cap_nauth][1] = *b; s_cap_auth[s_cap_nauth][2] = *c; }
    s_cap_nauth++;
    return 0;
}
static int cap_sprite(const GeVtx *a, const GeVtx *b, int persp) {
    (void)persp;
    if (s_cap_nauth < 3) { s_cap_auth[s_cap_nauth][0] = *a; s_cap_auth[s_cap_nauth][1] = *b; }
    s_cap_nauth++;
    return 0;
}
static int cap_wing_tri(uint32_t fb, int x0, int x1, const GeVtx *a, const GeVtx *b, const GeVtx *c, int persp) {
    (void)fb; (void)persp;
    if (s_cap_nwing < 3) {
        s_cap_wing[s_cap_nwing][0] = *a; s_cap_wing[s_cap_nwing][1] = *b; s_cap_wing[s_cap_nwing][2] = *c;
        s_cap_cols[s_cap_nwing][0] = x0; s_cap_cols[s_cap_nwing][1] = x1;
    }
    s_cap_nwing++;
    return 1;
}
static int cap_wing_sprite(uint32_t fb, int x0, int x1, const GeVtx *a, const GeVtx *b, int persp) {
    (void)fb; (void)persp;
    if (s_cap_nwing < 3) {
        s_cap_wing[s_cap_nwing][0] = *a; s_cap_wing[s_cap_nwing][1] = *b;
        s_cap_cols[s_cap_nwing][0] = x0; s_cap_cols[s_cap_nwing][1] = x1;
    }
    s_cap_nwing++;
    return 1;
}

static void test_ge_wide_capture(uint8_t *vram) {
    GeGpuHooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.tri = cap_tri;
    hooks.sprite = cap_sprite;
    hooks.flush = hook_flush;
    hooks.vram_dirty = hook_vram_dirty;
    hooks.wing_tri = cap_wing_tri;
    hooks.wing_sprite = cap_wing_sprite;
    s_cap_nauth = s_cap_nwing = 0;
    ge_set_gpu_hooks(&hooks);
    run_wide_list(WIDE_D, WF_CAPTURE, vram);
    ge_set_gpu_hooks(NULL);
    const int d = WIDE_D;
    expect(s_cap_nauth == 3 && s_cap_nwing == 3, "wide_fill",
           "each of the two triangles and the sprite reaches one wing hook");
    expect(ge_wide_drawn(WIDE_FB), "wide_fill", "a wing hook that draws marks the canvas drawn");
    if (s_cap_nauth != 3 || s_cap_nwing != 3) { ge_wide_set_wing(0); return; }
    int off_grid = 0;
    for (int t = 0; t < 2; t++) {
        const int shift = t == 0 ? d : d - 480;
        expect(s_cap_cols[t][0] == (t == 0 ? 0 : d) && s_cap_cols[t][1] == (t == 0 ? d - 1 : 2 * d - 1),
               "wide_fill", "a wing hook gets its wing's canvas columns");
        for (int i = 0; i < 3; i++) {
            const float x = s_cap_auth[t][i].x;
            const float want = floorf(x * 16.0f + 0.375f) / 16.0f + (float)shift;
            if (x * 16.0f != floorf(x * 16.0f)) off_grid = 1;
            if (s_cap_wing[t][i].x != want || s_cap_wing[t][i].y != s_cap_auth[t][i].y) {
                fprintf(stderr, "wide_fill: wing vertex x %.6f, want %.6f (guest %.6f)\n",
                        (double)s_cap_wing[t][i].x, (double)want, (double)x);
                expect(0, "wide_fill", "a wing triangle is the guest's, snapped to 28.4 and shifted");
            }
        }
    }
    expect(off_grid, "wide_fill", "the capture list has corners off the 28.4 grid");
    expect(s_cap_cols[2][0] == 0 && s_cap_cols[2][1] == d - 1, "wide_fill", "the sprite reaches the left wing");
    for (int i = 0; i < 2; i++)
        expect(s_cap_wing[2][i].x == floorf(s_cap_auth[2][i].x) + (float)d, "wide_fill",
               "a transform sprite's corners are floored, then shifted");
    ge_wide_set_wing(0);
}

static uint32_t wide_word_identity(uint32_t word, int fmt) {
    (void)fmt;
    return word;
}

/* ge_wide_picture for WIDE_FB, given the frame in `vram` (stride 512, 8888): the left wing, the
 * frame's 480 columns and the right wing, the wings the canvas's (want_wings) or 0. */
static int wide_check_picture(int wing, const uint32_t *vram, int want_wings) {
    const int cols = 480 + 2 * wing;
    uint32_t *frame = (uint32_t *)malloc(480u * 272u * 4u);
    uint32_t *pic = (uint32_t *)malloc((size_t)cols * 272u * 4u);
    const uint32_t *cv = ge_wide_canvas(WIDE_FB, NULL);
    int ok = frame && pic && (cv || !want_wings);
    if (ok) {
        for (uint32_t y = 0; y < 272u; y++)
            for (uint32_t x = 0; x < 480u; x++) frame[y * 480u + x] = vram[y * 512u + x];
        ok = ge_wide_picture(WIDE_FB, frame, pic, wide_word_identity) == cols;
    }
    for (int y = 0; ok && y < 272; y++)
        for (int x = 0; x < cols; x++) {
            uint32_t want;
            if (x < wing) want = want_wings ? cv[y * 512 + x] : 0u;
            else if (x < wing + 480) want = frame[y * 480 + x - wing];
            else want = want_wings ? cv[y * 512 + x - 480] : 0u;
            if (pic[(size_t)y * (size_t)cols + (size_t)x] != want) {
                fprintf(stderr, "wide_fill: picture (%d,%d) = %08x, want %08x\n", x, y,
                        (unsigned)pic[(size_t)y * (size_t)cols + (size_t)x], (unsigned)want);
                ok = 0;
                break;
            }
        }
    free(frame);
    free(pic);
    return ok;
}

static void test_ge_wide_fill(void) {
    /* Window sizes at 16:9, 21:9 and 32:9 and the wing each gives. The PSP's 480x272 is 30:17,
     * which a 16:9 window widens by under 4 pixels, so 16:9 has no wings. */
    static const struct { int w, h, wing; const char *name; } aspects[] = {
        { 1920, 1080, 0, "16:9" }, { 2520, 1080, WIDE_D, "21:9" }, { 3840, 1080, 240, "32:9" },
    };
    for (size_t i = 0; i < sizeof aspects / sizeof aspects[0]; i++)
        expect(ge_wide_wing_for(aspects[i].w, aspects[i].h) == aspects[i].wing, "wide_fill",
               "the wing width of a 16:9, 21:9 or 32:9 window");
    expect(ge_wide_wing_for(1280, 1024) == 0, "wide_fill", "a window narrower than 30:17 has no wings");
    expect(ge_wide_wing_for(7680, 1080) == GE_WIDE_MAX_WING, "wide_fill", "the wing is capped");
    expect(ge_wide_set_wing(77) == 76 && ge_wide_set_wing(0) == 0, "wide_fill",
           "a wing width rounds down to a multiple of 4");

    uint8_t *off = (uint8_t *)malloc(WIDE_VRAM_SIZE), *on = (uint8_t *)malloc(WIDE_VRAM_SIZE);
    if (!off || !on) { expect(0, "wide_fill", "no memory"); free(off); free(on); return; }
    for (size_t a = 0; a < sizeof aspects / sizeof aspects[0]; a++) {
        const int wing = ge_wide_wing_for(aspects[a].w, aspects[a].h);
        for (int fade = 0; fade < 2; fade++) {
            const int flags = fade ? WF_FADE : 0;
            run_wide_list(0, flags, off);
            expect(!ge_wide_canvas(WIDE_FB, NULL) && !ge_wide_drawn(WIDE_FB), "wide_fill",
                   "Fill off makes no canvas");
            run_wide_list(wing, flags, on);
            if (memcmp(off, on, WIDE_VRAM_SIZE)) {
                fprintf(stderr, "wide_fill: %s (wing %d) changed guest VRAM (fade=%d)\n",
                        aspects[a].name, wing, fade);
                expect(0, "wide_fill", "Fill leaves guest VRAM and the authentic frame unchanged");
            }
            uint32_t bar;
            memcpy(&bar, on + (120u * 512u + 240u) * 4u, 4u);
            expect(fade || bar == WIDE_BAR, "wide_fill", "the bar is in the frame");
            if (!wing) {
                expect(!ge_wide_canvas(WIDE_FB, NULL) && !ge_wide_drawn(WIDE_FB), "wide_fill",
                       "a 16:9 window makes no canvas");
                continue;
            }
            expect(ge_wide_drawn(WIDE_FB), "wide_fill", "Fill on draws the canvas");
            expect(!wide_check_canvas(aspects[a].name, wing, flags, (const uint32_t *)off), "wide_fill",
                   "the wings hold the quad, the shapes where a wider screen puts them, and the clear");
            ge_wide_shown(WIDE_FB);
            ge_wide_note_display(WIDE_FB);
            expect(ge_wide_drawn(WIDE_FB) &&
                   !wide_check_canvas(aspects[a].name, wing, flags, (const uint32_t *)off), "wide_fill",
                   "a frame presented again, with no draw in between, keeps its wings");
            expect(wide_check_picture(wing, (const uint32_t *)on, 1), "wide_fill",
                   "the software picture is the left wing, the frame and the right wing");
            if (!fade) {
                /* The next frame draws its shapes with no clear: its first draw into the shown
                 * buffer clears the canvas, so it holds this frame's shapes on black, not the
                 * last frame's. */
                memset(SR_HOST(WIDE_VRAM), 0, WIDE_VRAM_SIZE);
                ge_wide_note_display(WIDE_FB);
                build_wide_list(WF_NOCLEAR);
                expect(ge_run_list(LIST_WIDE, 0) == 0, "wide_fill", "list did not reach END");
                expect(ge_wide_drawn(WIDE_FB), "wide_fill", "the next frame's wing draws mark it drawn");
                expect(!wide_check_canvas(aspects[a].name, wing, WF_NOCLEAR, (const uint32_t *)SR_HOST(WIDE_VRAM)),
                       "wide_fill", "a shown canvas is cleared by the next frame's first draw");
                ge_wide_shown(WIDE_FB);
                memset(SR_HOST(WIDE_VRAM), 0, WIDE_VRAM_SIZE);
                build_wide_list(WF_NARROW);
                expect(ge_run_list(LIST_WIDE, 0) == 0, "wide_fill", "list did not reach END");
                expect(!ge_wide_drawn(WIDE_FB), "wide_fill",
                       "a frame that draws no wing primitive leaves the canvas undrawn (shown black)");
                const uint32_t *cv = ge_wide_canvas(WIDE_FB, NULL);
                int black = cv != NULL;
                for (size_t i = 0; black && i < (size_t)GE_WIDE_CANVAS_W * 272u; i++) black = cv[i] == 0u;
                expect(black, "wide_fill",
                       "a new frame's first draw, even one that reaches no wing, clears the shown canvas");
                expect(wide_check_picture(wing, (const uint32_t *)SR_HOST(WIDE_VRAM), 0), "wide_fill",
                       "a new frame that draws no wing shows black wings in the software picture");
            }
            ge_wide_set_wing(0);
        }
    }
    run_wide_list(WIDE_D, WF_NARROW, on);
    expect(!ge_wide_canvas(WIDE_FB, NULL) && !ge_wide_drawn(WIDE_FB), "wide_fill",
           "a scissor narrower than the screen keeps every draw out of the wings");
    ge_wide_set_wing(0);

    /* Host-only: the wing passes charge no GE time. */
    ge_timing_set_enabled(1);   /* off in this test otherwise */
    run_wide_list(0, 0, off);
    const double clk_off = ge_timing_run_clk();
    run_wide_list(240, 0, on);
    const double clk_on = ge_timing_run_clk();
    ge_wide_set_wing(0);
    ge_timing_set_enabled(0);
    if (!(clk_off > 0.0 && clk_on == clk_off)) {
        fprintf(stderr, "wide_fill: GE clock %.3f with Fill off, %.3f with Fill on\n", clk_off, clk_on);
        expect(0, "wide_fill", "Fill charges no GE time");
    }

    test_ge_wide_capture(on);
    free(off);
    free(on);
}

/* The GE clock charges a sprite for the columns ge.c rasterizes (#2193): on a 512 stride a
 * scissor to x 511 times the 32 columns past 480 too, on a 500 stride the 20 up to its last
 * column, and on a 480 stride 480 columns, as before. One through-mode sprite, x 0-512, rows
 * 0-15, no texture, wing off. */
static double wide_raster_sprite_clk(uint32_t fbw, uint32_t x2) {
    const uint32_t through = (7u << 2) | (3u << 7) | (1u << 23);
    memset(SR_HOST(WIDE_VRAM), 0, WIDE_VRAM_SIZE);
    s_pc = LIST_WIDE;
    s_wide_vc = WIDE_VERTS;
    W(0x10, 0x080000u);
    W(0x13, 0u);
    W(0x9C, WIDE_FB & 0x00FFFFFFu);
    W(0x9D, fbw);
    W(0xD2, 3u);
    W(0x15, 0u);
    W(0x16, (271u << 10) | 511u);
    W(0xD4, 0u);
    W(0xD5, (271u << 10) | x2);
    for (uint32_t cmd = 0x17; cmd <= 0x28; cmd++) W(cmd, 0u);   /* every unit off */
    W(0x50, 0u);
    const uint32_t at = s_wide_vc;
    wide_vtx(WIDE_BAR, 0.0f, 0.0f);
    wide_vtx(WIDE_BAR, 512.0f, 16.0f);
    W(0x12, through);
    W(0x01, at & 0x00FFFFFFu);
    W(0x04, (6u << 16) | 2u);
    W(0x0F, 0u);
    W(0x0C, 0u);
    expect(ge_run_list(LIST_WIDE, 0) == 0, "wide_raster_clk", "list did not reach END");
    return ge_timing_run_clk();
}

static void test_ge_wide_raster_timing(void) {
    ge_wide_set_wing(0);
    ge_timing_set_enabled(1);
    const double wide = wide_raster_sprite_clk(512u, 511u);
    const double screen = wide_raster_sprite_clk(512u, 479u);
    const double narrow = wide_raster_sprite_clk(480u, 511u);
    const double mid = wide_raster_sprite_clk(500u, 511u);
    const double small = wide_raster_sprite_clk(256u, 479u);
    ge_timing_set_enabled(0);
    if (!(screen > 0.0 && wide > screen && narrow == screen && mid > screen && mid < wide &&
          small > 0.0 && small < screen)) {
        fprintf(stderr, "wide_raster_clk: GE clock %.3f (512 stride, x2 511), %.3f (512 stride, x2 479), "
                        "%.3f (480 stride, x2 511), %.3f (500 stride, x2 511), %.3f (256 stride, x2 479)\n",
                wide, screen, narrow, mid, small);
        expect(0, "wide_raster_clk",
               "a 512 scissor times 512 columns on a 512 stride, 500 on a 500 stride, 480 on a 480 stride, "
               "and 256 on a 256 stride");
    }
}

/* Every raster write stays inside the buffer it writes (#2193): colour inside the target's
 * stride, depth inside the depth buffer's zbw columns and 272 rows. Column x of row y is guest
 * address (y * stride + x), so a write past the stride lands in the next row, or past the last
 * row. Three cells, software rasterizer, wing off:
 *   colour: a 256-stride 8888 target, scissor x2 479, a sprite x 0-512 on rows 100-109. Rows
 *     100-109 must hold it in columns 0-255, and nothing else may change: not row 110, not the
 *     row past the buffer.
 *   depth: a 512-stride target with a 256-wide depth buffer, cleared to depth A, then a
 *     depth-writing sprite (ALWAYS, depth B) on rows 0-9 at x 256-511, outside the depth
 *     buffer's columns. Every depth pixel must still hold A, and the row past the buffer its
 *     sentinel.
 *   clear: a depth clear (depth C) under a scissor of x 0-99 must write columns 0-99 only. */
#define RSB_FB         0x04000000u
#define RSB_ZB         0x04100000u
#define RSB_BG         0x5a5a5a5au
#define RSB_SPRITE     0xff3080c0u
#define RSB_DEPTH_SENT 0x1234u
#define RSB_DEPTH_A    0x4000u
#define RSB_DEPTH_B    0x0100u
#define RSB_DEPTH_C    0x7777u

static void rsb_vtx(uint32_t color, float x, float y, float z) {
    uint8_t *d = (uint8_t *)SR_HOST(s_wide_vc);
    const float pos[3] = { x, y, z };
    memcpy(d, &color, 4);
    memcpy(d + 4, pos, 12);
    s_wide_vc += 16u;
}

static void rsb_sprite(uint32_t color, float x0, float y0, float x1, float y1, float z) {
    const uint32_t at = s_wide_vc;
    rsb_vtx(color, x0, y0, z);
    rsb_vtx(color, x1, y1, z);
    W(0x01, at & 0x00FFFFFFu);
    W(0x04, (6u << 16) | 2u);
}

/* The list's state: an 8888 target at RSB_FB with stride fbw, depth at RSB_ZB with stride
 * zbw, every unit off, depth writes on with the test ALWAYS (enabled per cell), the region the
 * whole 512x272 and the scissor x 0 to scis_x2 on every row. */
static void rsb_begin(uint32_t fbw, uint32_t zbw, uint32_t scis_x2) {
    s_pc = LIST_WIDE;
    s_wide_vc = WIDE_VERTS;
    W(0x10, 0x080000u);
    W(0x13, 0u);
    W(0x9C, RSB_FB & 0x00FFFFFFu);
    W(0x9D, fbw | ((RSB_FB & 0xFF000000u) >> 8));
    W(0x9E, RSB_ZB & 0x00FFFFFFu);
    W(0x9F, zbw | ((RSB_ZB & 0xFF000000u) >> 8));
    W(0xD2, 3u);
    W(0x15, 0u);
    W(0x16, (271u << 10) | 511u);
    W(0xD4, 0u);
    W(0xD5, (271u << 10) | scis_x2);
    for (uint32_t cmd = 0x17; cmd <= 0x28; cmd++) W(cmd, 0u);   /* every unit off */
    W(0xE8, 0u);                                                  /* colour and alpha masks open */
    W(0xE9, 0u);
    W(0xD3, 0u);
    W(0xE7, 0u);                                                  /* depth writes on */
    W(0xDE, 1u);                                                  /* depth test ALWAYS */
    W(0x50, 0u);
    W(0x12, (7u << 2) | (3u << 7) | (1u << 23));                  /* 8888, float, through */
}

static void rsb_end(const char *scenario) {
    W(0x0F, 0u);
    W(0x0C, 0u);
    expect(ge_run_list(LIST_WIDE, 0) == 0, scenario, "list did not reach END");
}

static uint32_t rsb_fb(uint32_t linear) {
    uint32_t v;
    memcpy(&v, SR_HOST(RSB_FB + linear * 4u), 4);
    return v;
}

/* Guest depth pixel `linear` (y * zbw + x), at the address ge.c's depth surface uses for it. */
static void *rsb_depth(uint32_t linear) {
    return SR_HOST((RSB_ZB + linear * 2u) ^ 0x2040u);
}

/* A CPU write of guest depth: ge.c's cached surface over it is stale, as after a guest store. */
static void rsb_fill_depth(uint32_t pixels, uint16_t z) {
    for (uint32_t i = 0; i < pixels; i++) memcpy(rsb_depth(i), &z, 2);
    ge_zbuf_invalidate_guest_span(RSB_ZB, pixels * 2u + 0x2040u);
}

/* Columns below `cols` hold `inside`, the rest `outside`, and the row past the buffer the
 * sentinel, once ge.c has written its depth surface back to guest memory. */
static void rsb_check_depth(const char *scenario, uint32_t zbw, uint32_t cols, uint16_t inside,
                            uint16_t outside) {
    if (!ge_zbuf_materialize_current()) {
        expect(0, scenario, "the depth surface was not written back to guest memory");
        return;
    }
    for (uint32_t i = 0; i < zbw * 273u; i++) {
        const uint32_t x = i % zbw, y = i / zbw;
        const uint16_t want = y >= 272u ? RSB_DEPTH_SENT : (x < cols ? inside : outside);
        uint16_t got;
        memcpy(&got, rsb_depth(i), 2);
        if (got != want) {
            fprintf(stderr, "%s: depth pixel (%u,%u) of a %u-wide depth buffer is 0x%04x, want 0x%04x\n",
                    scenario, (unsigned)x, (unsigned)y, (unsigned)zbw, got, want);
            expect(0, scenario, y >= 272u ? "a depth write landed past the depth buffer's last row"
                                          : "a depth write landed outside the pixel it was given");
            return;
        }
    }
}

static void test_ge_raster_stride_bounds(void) {
    ge_wide_set_wing(0);

    /* colour: a 256 stride, scissor x2 479 */
    for (uint32_t i = 0; i < 256u * 273u; i++) {
        const uint32_t bg = RSB_BG;
        memcpy(SR_HOST(RSB_FB + i * 4u), &bg, 4);
    }
    rsb_begin(256u, 256u, 479u);
    rsb_sprite(RSB_SPRITE, 0.0f, 100.0f, 512.0f, 110.0f, 0.0f);
    rsb_end("raster_stride_colour");
    for (uint32_t i = 0; i < 256u * 273u; i++) {
        const uint32_t y = i / 256u;
        const uint32_t want = y >= 100u && y < 110u ? RSB_SPRITE : RSB_BG;
        if ((rsb_fb(i) & 0x00ffffffu) != (want & 0x00ffffffu)) {
            fprintf(stderr, "raster_stride_colour: pixel (%u,%u) of a 256-stride target is %08x, want %08x\n",
                    (unsigned)(i % 256u), (unsigned)y, rsb_fb(i), want);
            expect(0, "raster_stride_colour",
                   y >= 272u ? "a colour write landed past the target's last row"
                             : "a colour write past the stride landed in another row");
            break;
        }
    }

    /* depth: a 512-stride target, a 256-wide depth buffer */
    rsb_fill_depth(256u * 273u, RSB_DEPTH_SENT);
    rsb_begin(512u, 256u, 511u);
    W(0xD3, 1u | (4u << 8));                                      /* clear mode: depth only */
    rsb_sprite(RSB_SPRITE, 0.0f, 0.0f, 512.0f, 272.0f, (float)RSB_DEPTH_A);
    W(0xD3, 0u);
    W(0x23, 1u);                                                  /* depth test on, ALWAYS */
    rsb_sprite(RSB_SPRITE, 256.0f, 0.0f, 512.0f, 10.0f, (float)RSB_DEPTH_B);
    rsb_end("raster_stride_depth");
    rsb_check_depth("raster_stride_depth", 256u, 256u, RSB_DEPTH_A, RSB_DEPTH_A);

    /* clear: a depth clear under a 100-column scissor */
    rsb_fill_depth(256u * 273u, RSB_DEPTH_SENT);
    rsb_begin(512u, 256u, 99u);
    W(0xD3, 1u | (4u << 8));
    rsb_sprite(RSB_SPRITE, 0.0f, 0.0f, 512.0f, 272.0f, (float)RSB_DEPTH_C);
    W(0xD3, 0u);
    rsb_end("raster_stride_depth_clear");
    rsb_check_depth("raster_stride_depth_clear", 256u, 100u, RSB_DEPTH_C, RSB_DEPTH_SENT);
}

static void print_mask(FILE *out, uint32_t mask) {
    if (!mask) { fputs("none", out); return; }
    const char *sep = "";
    for (unsigned i = 0; i < GE_PROFILE_CHECK_COUNT; i++)
        if (mask & GE_PROFILE_CHECK_MASK(i)) {
            fprintf(out, "%s%s", sep, ge_profile_check_name((GeProfileCheck)i));
            sep = ",";
        }
}

static GeCpuProfileStats snapshot(void) {
    GeCpuProfileStats s;
    memset(&s, 0, sizeof s);
    ge_cpu_profile_get(&s);
    return s;
}

static void configure(const char *cpu, const char *prim_profile, const char *stride,
                      const char *calibration) {
    env_set("SR_GPU_CPU_PROFILE", NULL);
    env_set("SR_GE_CPU_PROFILE", cpu);
    env_set("SR_GE_PRIM_PROFILE", prim_profile);
    env_set("SR_GE_PRIM_PROFILE_STRIDE", stride);
    env_set("SR_GE_PRIM_CALIBRATION", calibration);
    ge_cpu_profile_reset();
}

static void run_list(uint32_t list, const char *scenario) {
    expect(ge_run_list(list, 0) == 0, scenario, "list did not reach END");
}

static void run_scene(const char *scenario) {
    run_list(LIST_MAIN, scenario);
}

static void expect_consistent(const char *scenario, const GeCpuProfileStats *s) {
    char detail[256];
    uint32_t mask = ge_cpu_profile_validate(s, detail, sizeof detail);
    s_scenarios++;
    if (mask) {
        s_failures++;
        fprintf(stderr, "FAIL %s: production profile rejected (", scenario);
        print_mask(stderr, mask);
        fprintf(stderr, "): %s\n", detail);
    }
    expect(detail[0] == '\0', scenario, "a consistent block left a detail message");
    printf("SCENARIO %s mask=0x%" PRIx32 " commands=%" PRIu64 " primitive_commands=%" PRIu64
           " triangle_candidates=%" PRIu64 " drawn/clipped/rejected=%" PRIu64 "/%" PRIu64 "/%" PRIu64
           " decoded=%" PRIu64 " rejects=%" PRIu64 " list_ns=%" PRIu64 " gpu_hook_ns=%" PRIu64 "\n",
           scenario, mask, s->commands, s->primitive_commands,
           s->primitive_profile_triangle_candidates,
           s->primitive_profile_transform_triangles_drawn,
           s->primitive_profile_transform_triangles_clipped,
           s->primitive_profile_transform_triangles_rejected,
           s->primitive_profile_actual_decoded_vertices, s->primitive_profile_vertex_rejects,
           s->phase[GE_CPU_LIST_TOTAL].ns, s->phase[GE_CPU_GPU_HOOK].ns);
}

static int coarse_counters_zero(const GeCpuProfileStats *s) {
    static const GeCpuProfileStats zero;
    size_t from = offsetof(GeCpuProfileStats, phase);
    size_t to = offsetof(GeCpuProfileStats, primitive_profile_stride);
    return memcmp((const char *)s + from, (const char *)&zero + from, to - from) == 0;
}

static int sparse_counters_zero(const GeCpuProfileStats *s) {
    static const GeCpuProfileStats zero;
    size_t from = offsetof(GeCpuProfileStats, primitive_profile_draw_setup);
    return memcmp((const char *)s + from, (const char *)&zero + from, sizeof *s - from) == 0;
}

/* Every field, compared by the padding-free uint64_t runs so padding bytes never matter. */
static int same_profile(const GeCpuProfileStats *a, const GeCpuProfileStats *b) {
    size_t coarse = offsetof(GeCpuProfileStats, phase);
    size_t coarse_end = offsetof(GeCpuProfileStats, primitive_profile_stride);
    size_t sparse = offsetof(GeCpuProfileStats, primitive_profile_timer_pair_ns);
    return a->enabled == b->enabled &&
           a->primitive_profile_enabled == b->primitive_profile_enabled &&
           a->primitive_profile_calibration_enabled == b->primitive_profile_calibration_enabled &&
           a->primitive_profile_stride == b->primitive_profile_stride &&
           memcmp((const char *)a + coarse, (const char *)b + coarse, coarse_end - coarse) == 0 &&
           memcmp((const char *)a + sparse, (const char *)b + sparse, sizeof *a - sparse) == 0;
}

/* The populations each relation constrains must actually be produced by the scene, so a
 * relation cannot hold only because both of its sides are zero. */
static void expect_full_coverage(const char *scenario, const GeCpuProfileStats *s,
                                 int strip_cache, int hooked) {
    const GeCpuPhaseStats *p = s->phase;
    expect(s->enabled && s->primitive_profile_enabled && s->primitive_profile_calibration_enabled &&
           s->primitive_profile_stride == 1u, scenario, "profile configuration");
    expect(p[GE_CPU_LIST_TOTAL].ns > 0 && p[GE_CPU_COMMAND_DISPATCH].ns > 0 &&
           p[GE_CPU_PRIMITIVE].ns > 0 && p[GE_CPU_BLOCK_TRANSFER].ns > 0 &&
           p[GE_CPU_CLUT_LOAD].ns > 0, scenario, "coarse phases timed");
    expect(hooked ? (p[GE_CPU_GPU_HOOK].ns > 0 && s->flushes > 0)
                  : (p[GE_CPU_GPU_HOOK].calls == 0 && s->flushes == 0),
           scenario, "gpu hook and flush populations match the backend");
    expect(s->block_transfers > 0 && s->clut_loads > 0 && s->primitive_type[7] > 0 &&
           s->primitive_profile_patch_commands == 2u, scenario, "command kinds");
    expect(s->primitive_profile_through_triangle_candidates > 0 &&
           s->primitive_profile_transform_triangle_candidates > 0 &&
           s->primitive_profile_transform_triangles_drawn > 0 &&
           s->primitive_profile_transform_triangles_clipped > 0 &&
           s->primitive_profile_transform_triangles_rejected > 0,
           scenario, "triangle candidates and every acceptance verdict");
    expect(s->primitive_profile_non_triangle_primitives > 0 &&
           s->primitive_profile_vertex_rejects > 0 &&
           s->primitive_profile_actual_through_vertices > 0 &&
           s->primitive_profile_actual_transformed_vertices <
               s->primitive_profile_transform_vertex_uses,
           scenario, "rejects and a skipped sprite-corner decode");
    expect(strip_cache ? (s->primitive_profile_strip_cache_commands > 0 &&
                          s->strip_vertices_reused > 0 &&
                          s->primitive_profile_vertex_references > s->primitive_profile_vertices)
                       : (s->primitive_profile_strip_cache_commands == 0 &&
                          s->strip_vertices_reused == 0 &&
                          s->primitive_profile_vertex_references == s->primitive_profile_vertices),
           scenario, "strip-cache population matches the cache switch");
    for (unsigned i = 0; i < GE_PRIM_PROFILE_PHASE_COUNT; i++)
        expect(s->primitive_profile_phase[i].calls > 0 && s->primitive_profile_eligible[i] > 0,
               scenario, "every sparse phase sampled at stride 1");
    expect(s->primitive_profile_sampled_total.calls > 0 &&
           s->primitive_profile_empty_control.calls > 0, scenario, "calibration samples");
}

/* ---- Fault injection ---- */

typedef void (*Mutation)(GeCpuProfileStats *s);

static uint64_t nested_ns(const GeCpuProfileStats *s) {
    return s->phase[GE_CPU_COMMAND_DISPATCH].ns + s->phase[GE_CPU_PRIMITIVE].ns +
           s->phase[GE_CPU_BLOCK_TRANSFER].ns + s->phase[GE_CPU_CLUT_LOAD].ns +
           s->phase[GE_CPU_FLUSH].ns;
}

static void m_hook_outside_primitive(GeCpuProfileStats *s) {
    s->phase[GE_CPU_GPU_HOOK].ns = s->phase[GE_CPU_PRIMITIVE].ns + 1u;
}
static void m_dispatch_overrun(GeCpuProfileStats *s) {
    s->phase[GE_CPU_COMMAND_DISPATCH].ns += s->phase[GE_CPU_LIST_TOTAL].ns - nested_ns(s) + 1u;
}
static void m_nested_sum_wraps(GeCpuProfileStats *s) {
    /* A wrapping sum would come to exactly 0 <= list_total. */
    s->phase[GE_CPU_FLUSH].ns = UINT64_MAX - (nested_ns(s) - s->phase[GE_CPU_FLUSH].ns) + 1u;
}
static void m_primitive_phase_unpaired(GeCpuProfileStats *s) {
    s->phase[GE_CPU_PRIMITIVE].calls++;
}
static void m_list_calls_unpaired(GeCpuProfileStats *s) {
    s->phase[GE_CPU_LIST_TOTAL].calls++;
}
static void m_block_phase_unpaired(GeCpuProfileStats *s) {
    s->phase[GE_CPU_BLOCK_TRANSFER].calls++;
}
static void m_flush_phase_unpaired(GeCpuProfileStats *s) {
    s->phase[GE_CPU_FLUSH].calls++;
}
static void m_clut_load_unended(GeCpuProfileStats *s) {
    s->clut_loads = s->phase[GE_CPU_CLUT_LOAD].calls + 1u;
}
static void m_command_undercount(GeCpuProfileStats *s) {
    s->commands = s->primitive_commands + s->block_transfers + s->clut_loads + s->flushes - 1u;
}
static void m_command_bound_wraps(GeCpuProfileStats *s) {
    /* Block transfers stay paired with their phase calls; a wrapping sum of the command
     * kinds would fall back below commands. */
    s->block_transfers = UINT64_MAX;
    s->phase[GE_CPU_BLOCK_TRANSFER].calls = UINT64_MAX;
}
static void m_command_bound_wraps_last(GeCpuProfileStats *s) {
    /* The same with the overflow on the last term of the sum. */
    s->flushes = UINT64_MAX;
    s->phase[GE_CPU_FLUSH].calls = UINT64_MAX;
}
static void m_patch_without_command(GeCpuProfileStats *s) {
    s->primitive_profile_patch_commands++;
}
static void m_command_without_draw(GeCpuProfileStats *s) {
    /* One primitive command (with its phase call) that is neither a draw nor a patch: the
     * exact relation fails from below. */
    s->primitive_commands++;
    s->phase[GE_CPU_PRIMITIVE].calls++;
}
static void m_type_sum_wraps(GeCpuProfileStats *s) {
    /* Both profiles agree per type; a wrapping type sum would come back to
     * primitive_commands. */
    uint64_t type7 = s->primitive_type[7];
    s->primitive_type[7] = s->primitive_profile_commands[7] = UINT64_MAX;
    s->primitive_type[0] += type7 + 1u;
    s->primitive_profile_commands[0] += type7 + 1u;
}
static void m_type_slot_overcount(GeCpuProfileStats *s) {
    uint64_t types = 0;
    for (unsigned i = 0; i < 8; i++) types += s->primitive_type[i];
    s->primitive_type[3] += s->primitive_commands - types + 1u;
}
static void m_decode_use_drift(GeCpuProfileStats *s) {
    s->vertex_decode_uses++;
}
static void m_strip_reuse_drift(GeCpuProfileStats *s) {
    s->strip_vertices_reused++;
}
static void m_submitted_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_submitted[3]++;
}
static void m_submitted_sum_wraps(GeCpuProfileStats *s) {
    /* type 7 submits nothing; a wrapping sum would come back to the true total. */
    s->primitive_profile_submitted[7] = UINT64_MAX;
    s->primitive_profile_submitted[0]++;
}
static void m_submitted_sum_exceeds(GeCpuProfileStats *s) {
    /* The parts before the last already equal the expected total; the last pushes the sum
     * past UINT64_MAX, which must not compare equal. */
    s->primitive_profile_submitted[7] = UINT64_MAX;
}
static void m_through_candidate_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_through_triangle_candidates++;
}
static void m_mode_split_wraps(GeCpuProfileStats *s) {
    /* Transform candidates at UINT64_MAX with verdicts to match; a wrapping through +
     * transform sum would come back to triangle_candidates. */
    uint64_t transform = s->primitive_profile_transform_triangle_candidates;
    s->primitive_profile_transform_triangle_candidates = UINT64_MAX;
    s->primitive_profile_through_triangle_candidates += transform + 1u;
    s->primitive_profile_transform_triangles_drawn =
        UINT64_MAX - s->primitive_profile_transform_triangles_clipped -
        s->primitive_profile_transform_triangles_rejected;
}
static void m_triangle_reference_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_triangle_vertex_references++;
}
static void m_reference_split_wraps(GeCpuProfileStats *s) {
    uint64_t triangle = s->primitive_profile_triangle_vertex_references;
    s->primitive_profile_triangle_vertex_references = UINT64_MAX;
    s->primitive_profile_non_triangle_vertex_references += triangle + 1u;
}
static void m_triangle_use_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_triangle_vertex_uses++;
}
static void m_transform_vertices_drift(GeCpuProfileStats *s) {
    s->primitive_profile_transform_vertices++;
}
static void m_transform_use_overcount(GeCpuProfileStats *s) {
    /* Through + transform uses no longer sum to the uses; transform_vertices follows. */
    s->primitive_profile_transform_vertex_uses++;
    s->primitive_profile_transform_vertices++;
}
static void m_use_split_wraps(GeCpuProfileStats *s) {
    uint64_t triangle = s->primitive_profile_triangle_vertex_uses;
    s->primitive_profile_triangle_vertex_uses = UINT64_MAX;
    s->primitive_profile_non_triangle_vertex_uses += triangle + 1u;
}
static void m_references_undercount(GeCpuProfileStats *s) {
    /* Lower the total and its triangle split together, just below the uses. */
    uint64_t d = s->primitive_profile_vertex_references - s->primitive_profile_vertices + 1u;
    s->primitive_profile_triangle_vertex_references -= d;
    s->primitive_profile_vertex_references -= d;
}
static void m_reject_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_vertex_rejects = s->primitive_profile_non_triangle_vertex_references + 1u;
}
static void m_decoded_total_drift(GeCpuProfileStats *s) {
    s->primitive_profile_actual_decoded_vertices++;
}
static void m_through_decode_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_actual_through_vertices++;
    s->primitive_profile_actual_decoded_vertices++;
}
static void m_transform_decode_overcount(GeCpuProfileStats *s) {
    uint64_t extra = s->primitive_profile_transform_vertex_uses + 1u -
                     s->primitive_profile_actual_transformed_vertices;
    s->primitive_profile_actual_transformed_vertices += extra;
    s->primitive_profile_actual_decoded_vertices += extra;
}
static void m_skipped_decode_without_reject(GeCpuProfileStats *s) {
    /* One reject fewer than skipped transform decodes; the through decodes still match. */
    s->primitive_profile_vertex_rejects = s->primitive_profile_transform_vertex_uses -
                                          s->primitive_profile_actual_transformed_vertices - 1u;
}
static void m_calibration_bound_exceeds(GeCpuProfileStats *s) {
    /* Decodes just large enough that decoded + candidates exceeds UINT64_MAX, with more
     * control samples than decodes: a bound past UINT64_MAX holds for any count, so only
     * the through decodes (moved to keep the mode split) disagree with their uses. */
    uint64_t decoded = UINT64_MAX - s->primitive_profile_triangle_candidates + 1u;
    s->primitive_profile_actual_decoded_vertices = decoded;
    s->primitive_profile_actual_through_vertices =
        decoded - s->primitive_profile_actual_transformed_vertices;
    s->primitive_profile_empty_control.calls = decoded + 1u;
}
static void m_decode_split_wraps(GeCpuProfileStats *s) {
    /* A wrapping through + transformed sum would come back to decoded; both modes now also
     * disagree with their uses. */
    uint64_t through = s->primitive_profile_actual_through_vertices;
    s->primitive_profile_actual_through_vertices = UINT64_MAX;
    s->primitive_profile_actual_transformed_vertices += through + 1u;
}
static void m_decision_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_transform_triangles_drawn++;
}
static void m_decision_sum_wraps(GeCpuProfileStats *s) {
    uint64_t rejected = s->primitive_profile_transform_triangles_rejected;
    s->primitive_profile_transform_triangles_rejected = UINT64_MAX;
    s->primitive_profile_transform_triangles_clipped += rejected + 1u;
}
static void m_fetch_sample_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_phase[GE_PRIM_PROFILE_VERTEX_FETCH_DECODE].calls =
        s->primitive_profile_eligible[GE_PRIM_PROFILE_VERTEX_FETCH_DECODE] + 1u;
}
static void m_lighting_sample_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_phase[GE_PRIM_PROFILE_LIGHTING].calls =
        s->primitive_profile_eligible[GE_PRIM_PROFILE_LIGHTING] + 1u;
}
static void m_draw_setup_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_draw_setup.calls = s->primitive_profile_draw_setup_eligible + 1u;
}
static void m_sampled_total_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_sampled_total.calls = s->primitive_profile_triangle_candidates + 1u;
}
static void m_control_overcount(GeCpuProfileStats *s) {
    s->primitive_profile_empty_control.calls = s->primitive_profile_actual_decoded_vertices +
                                               s->primitive_profile_triangle_candidates + 1u;
}
static void m_two_faults(GeCpuProfileStats *s) {
    m_hook_outside_primitive(s);
    m_decision_overcount(s);
}

#define BIT(check) GE_PROFILE_CHECK_MASK(GE_PROFILE_CHECK_##check)

typedef struct Fault {
    const char *name;
    Mutation mutate;
    uint32_t want;
} Fault;

/* Faults for a block with both profiles and calibration enabled. */
static const Fault s_full_faults[] = {
    { "hook_outside_primitive", m_hook_outside_primitive, BIT(PHASE_NESTING) },
    { "dispatch_overrun", m_dispatch_overrun, BIT(PHASE_NESTING) },
    { "nested_sum_wraps", m_nested_sum_wraps, BIT(PHASE_NESTING) },
    { "primitive_phase_unpaired", m_primitive_phase_unpaired, BIT(PHASE_CALLS) },
    { "list_calls_unpaired", m_list_calls_unpaired, BIT(PHASE_CALLS) },
    { "block_phase_unpaired", m_block_phase_unpaired, BIT(PHASE_CALLS) },
    { "flush_phase_unpaired", m_flush_phase_unpaired, BIT(PHASE_CALLS) },
    { "clut_load_unended", m_clut_load_unended, BIT(PHASE_CALLS) },
    { "command_undercount", m_command_undercount, BIT(COMMAND_BOUND) },
    { "command_bound_wraps", m_command_bound_wraps, BIT(COMMAND_BOUND) },
    { "command_bound_wraps_last", m_command_bound_wraps_last, BIT(COMMAND_BOUND) },
    { "patch_without_command", m_patch_without_command, BIT(PRIMITIVE_TYPES) },
    { "command_without_draw", m_command_without_draw, BIT(PRIMITIVE_TYPES) },
    { "type_sum_wraps", m_type_sum_wraps, BIT(PRIMITIVE_TYPES) },
    { "type_slot_overcount", m_type_slot_overcount, BIT(PRIMITIVE_TYPES) | BIT(PROFILE_AGREEMENT) },
    { "decode_use_drift", m_decode_use_drift, BIT(PROFILE_AGREEMENT) },
    { "strip_reuse_drift", m_strip_reuse_drift, BIT(PROFILE_AGREEMENT) },
    { "submitted_overcount", m_submitted_overcount, BIT(SUBMITTED_SPLIT) },
    { "submitted_sum_wraps", m_submitted_sum_wraps, BIT(SUBMITTED_SPLIT) },
    { "submitted_sum_exceeds", m_submitted_sum_exceeds, BIT(SUBMITTED_SPLIT) },
    { "through_candidate_overcount", m_through_candidate_overcount, BIT(TRIANGLE_MODE_SPLIT) },
    { "mode_split_wraps", m_mode_split_wraps, BIT(TRIANGLE_MODE_SPLIT) },
    { "triangle_reference_overcount", m_triangle_reference_overcount, BIT(REFERENCE_SPLIT) },
    { "reference_split_wraps", m_reference_split_wraps, BIT(REFERENCE_SPLIT) },
    { "triangle_use_overcount", m_triangle_use_overcount, BIT(USE_SPLIT) },
    { "transform_use_overcount", m_transform_use_overcount, BIT(USE_SPLIT) },
    { "transform_vertices_drift", m_transform_vertices_drift, BIT(USE_SPLIT) },
    { "use_split_wraps", m_use_split_wraps, BIT(USE_SPLIT) },
    { "references_undercount", m_references_undercount, BIT(REFERENCES_COVER_USES) },
    { "reject_overcount", m_reject_overcount, BIT(REJECT_BOUND) },
    { "decoded_total_drift", m_decoded_total_drift, BIT(DECODE_SPLIT) },
    { "decode_split_wraps", m_decode_split_wraps, BIT(DECODE_SPLIT) | BIT(DECODE_COVERAGE) },
    { "through_decode_overcount", m_through_decode_overcount, BIT(DECODE_COVERAGE) },
    { "transform_decode_overcount", m_transform_decode_overcount, BIT(DECODE_COVERAGE) },
    { "skipped_decode_without_reject", m_skipped_decode_without_reject, BIT(DECODE_COVERAGE) },
    { "calibration_bound_exceeds", m_calibration_bound_exceeds, BIT(DECODE_COVERAGE) },
    { "decision_overcount", m_decision_overcount, BIT(TRIANGLE_DECISIONS) },
    { "decision_sum_wraps", m_decision_sum_wraps, BIT(TRIANGLE_DECISIONS) },
    { "fetch_sample_overcount", m_fetch_sample_overcount, BIT(SAMPLE_BOUND) },
    { "lighting_sample_overcount", m_lighting_sample_overcount, BIT(SAMPLE_BOUND) },
    { "draw_setup_overcount", m_draw_setup_overcount, BIT(SAMPLE_BOUND) },
    { "sampled_total_overcount", m_sampled_total_overcount, BIT(CALIBRATION_BOUND) },
    { "control_overcount", m_control_overcount, BIT(CALIBRATION_BOUND) },
    { "two_faults", m_two_faults, BIT(PHASE_NESTING) | BIT(TRIANGLE_DECISIONS) },
};

static void expect_fault(const char *base, const GeCpuProfileStats *production,
                         const Fault *fault) {
    GeCpuProfileStats s;
    char detail[256];
    char label[128];
    memcpy(&s, production, sizeof s);
    fault->mutate(&s);
    uint32_t mask = ge_cpu_profile_validate(&s, detail, sizeof detail);
    snprintf(label, sizeof label, "%s/%s", base, fault->name);
    s_faults++;
    if (mask != fault->want) {
        s_failures++;
        fprintf(stderr, "FAIL %s: want ", label);
        print_mask(stderr, fault->want);
        fprintf(stderr, ", got ");
        print_mask(stderr, mask);
        fprintf(stderr, " (%s)\n", detail);
        return;
    }
    /* The detail names the lowest-numbered failed check. */
    unsigned lowest = 0;
    while (!(mask & GE_PROFILE_CHECK_MASK(lowest))) lowest++;
    const char *name = ge_profile_check_name((GeProfileCheck)lowest);
    expect(strncmp(detail, name, strlen(name)) == 0 && detail[strlen(name)] == ':',
           label, "detail does not name the lowest failed check");
}

static void expect_faults(const char *base, const GeCpuProfileStats *production) {
    for (size_t i = 0; i < sizeof s_full_faults / sizeof s_full_faults[0]; i++)
        expect_fault(base, production, &s_full_faults[i]);
}

static void expect_api_contract(void) {
    char detail[64];
    uint32_t all = GE_PROFILE_CHECK_MASK(GE_PROFILE_CHECK_COUNT) - 1u;
    expect(ge_cpu_profile_validate(NULL, detail, sizeof detail) == all &&
           strcmp(detail, "no profile snapshot") == 0, "api", "NULL snapshot fails closed");
    GeCpuProfileStats zero;
    memset(&zero, 0, sizeof zero);
    expect(ge_cpu_profile_validate(&zero, NULL, 0) == 0, "api", "all-zero block without detail");
    zero.enabled = zero.primitive_profile_enabled = zero.primitive_profile_calibration_enabled = 1;
    expect(ge_cpu_profile_validate(&zero, detail, sizeof detail) == 0, "api",
           "enabled block with zero samples");
    expect(strcmp(ge_profile_check_name(GE_PROFILE_CHECK_COUNT), "unknown") == 0, "api",
           "out-of-range check name");
    for (unsigned i = 0; i < GE_PROFILE_CHECK_COUNT; i++)
        for (unsigned j = i + 1; j < GE_PROFILE_CHECK_COUNT; j++)
            expect(strcmp(ge_profile_check_name((GeProfileCheck)i),
                          ge_profile_check_name((GeProfileCheck)j)) != 0, "api", "duplicate name");
}

static uint32_t vram_expected_cpu_address(uint32_t address) {
    uint32_t phys = SR_PHYS(address);
    uint32_t region = (phys >> 21) & 3u;
    uint32_t offset = phys & 0x001FFFFFu;
    if (region == 1u) {
        offset ^= 0x2040u;
    } else if (region == 3u) {
        uint32_t bank = (offset >> 5) & 0x1Fu;
        bank = ((bank << 1) | (bank >> 4)) & 0x1Fu;
        offset = (offset & ~0x3E0u) | (bank << 5);
        offset ^= 0x2040u;
    }
    return 0x04000000u + offset;
}

static void test_vram_mirror_cpu_stores(void) {
    static const uint32_t mirrors[] = {
        0x04000000u, 0x04200000u, 0x04400000u, 0x04600000u,
        0x44000000u, 0x44200000u, 0x44400000u, 0x44600000u,
    };
    const uint32_t offset = 0x00010100u;
    const uint32_t value = 0x78563412u;
    for (size_t mirror = 0; mirror < sizeof mirrors / sizeof mirrors[0]; mirror++) {
        uint32_t address = mirrors[mirror] + offset;
        MEM_W32(address, value);
        for (unsigned byte = 0; byte < 4u; byte++) {
            uint32_t source = address + byte;
            uint32_t physical = vram_expected_cpu_address(source);
            uint8_t actual = MEM_R8(physical);
            uint8_t expected = (uint8_t)(value >> (byte * 8u));
            char detail[128];
            snprintf(detail, sizeof detail,
                     "mirror %08" PRIx32 " byte %u maps to %08" PRIx32,
                     mirrors[mirror], byte, physical);
            expect(actual == expected, "vram_mirror_store", detail);
        }
        MEM_W32(0x04000000u + offset, value);
        expect(MEM_R32(address) == value, "vram_mirror_read",
               "CPU load through each cached and uncached mirror returns the physical bytes");
    }
    static const uint32_t boundaries[] = { 0x1Fu, 0x3Fu, 0x1FFFu };
    static const size_t swizzled_mirrors[] = { 1u, 3u, 5u, 7u };
    for (size_t m = 0; m < sizeof swizzled_mirrors / sizeof swizzled_mirrors[0]; m++) {
        for (size_t b = 0; b < sizeof boundaries / sizeof boundaries[0]; b++) {
            uint32_t address = mirrors[swizzled_mirrors[m]] + boundaries[b];
            MEM_W32(address, value);
            for (unsigned byte = 0; byte < 4u; byte++) {
                uint32_t physical = vram_expected_cpu_address(address + byte);
                expect(MEM_R8(physical) == (uint8_t)(value >> (byte * 8u)),
                       "vram_mirror_boundary",
                       "scalar word stores translate every byte across 32-byte, 64-byte, and 8 KiB boundaries");
            }
        }
    }
    expect(!sr_guest_span_readable(0x0420003Fu, 2u) &&
           !sr_guest_span_writable(0x0460001Fu, 2u),
           "vram_mirror_bulk", "swizzled mirrors reject non-contiguous bulk spans");
    expect(sr_guest_span_readable(0x04200100u, 4u) &&
           sr_guest_span_readable(0x04400100u, 4u),
           "vram_mirror_bulk", "contiguous swizzled blocks and the linear mirror stay accessible");
}

/* ---- BOUNDINGBOX / BJUMP conditional rendering (issue #1372) ----
 *
 * The sceGuBeginObject/sceGuEndObject shape inside a CALLed sublist: OFFSETADDR (VADDR and the
 * BJUMP target are offset-relative, like JUMP's), BASE and VADDR, BOUNDINGBOX, BASE and a BJUMP
 * past the object, then the object itself (a red sprite at (10,10) and a FRAMEBUFPTR switch)
 * and RET.  After the RET the main list JUMPs to a tail that draws a green sprite at (30,10);
 * the JUMP target and the sprite's VADDR need the caller's offset, so the list reaches END and
 * draws green only if the PC, the call stack and the offset came back intact.  A skipped object leaves no red pixel and no framebuffer switch, so green
 * lands in framebuffer A; an object that runs draws red and moves green to framebuffer B.
 * Each case runs on the software rasterizer and again through the GPU hook seam the Vulkan
 * backend fills, and both must execute the same PRIM commands. */

extern unsigned long g_ge_prim_count;

#define LIST_BBOX       0x08118000u
#define LIST_BBOX_SUB   0x0811A000u
#define LIST_BBOX_TAIL  0x0811C000u
#define BBOX_VERTS      0x08320000u
#define BBOX_INDICES    0x08328000u
#define BBOX_SPRITES    0x08329000u
#define BBOX_FB_A       0x041A0000u
#define BBOX_FB_B       0x041C0000u
#define BBOX_SUB_OFFSET 0x00010000u          /* OFFSETADDR 0x000100 in the sublist */
#define BBOX_SENTINEL   0xFF5A5A5Au
#define BBOX_RED        0xFF0000FFu
#define BBOX_GREEN      0xFF00FF00u
#define BBOX_THROUGH16  ((7u << 2) | (2u << 7) | (1u << 23))

typedef struct {
    const char *name;
    uint32_t vtype;     /* VERTEXTYPE in force at BOUNDINGBOX */
    uint32_t vaddr;     /* box vertices */
    int count;          /* BOUNDINGBOX count; -1 emits no BOUNDINGBOX at all */
    int small_scissor;  /* scissor (0,0)-(99,271): the box is beyond its right edge only */
    int stall;          /* stall the list on the BJUMP word, then resume it */
    int skip;           /* expected: BJUMP takes its branch */
} BboxCase;

/* BASE supplies address bits 24-27 and OFFSETADDR is added, as sceGuBeginObject relies on. */
static uint32_t bbox_sub_base(uint32_t addr) {
    return (addr >> 8) & 0x0F0000u;
}
static uint32_t bbox_sub_rel(uint32_t addr) {
    return (addr - BBOX_SUB_OFFSET) & 0x00FFFFFFu;
}

static uint32_t bbox_px(uint32_t fb, unsigned x, unsigned y) {
    return fb + (y * 512u + x) * 4u;
}

/* Returns the address of the BJUMP word. */
static uint32_t build_bbox_list(const BboxCase *c, uint32_t sprite_a, uint32_t sprite_b) {
    s_pc = LIST_BBOX_SUB;
    W(0x13, BBOX_SUB_OFFSET >> 8);                     /* OFFSETADDR */
    W(0x10, bbox_sub_base(c->vaddr));                  /* BASE */
    W(0x01, bbox_sub_rel(c->vaddr));                   /* VADDR */
    if (c->count >= 0) W(0x07, (uint32_t)c->count);    /* BOUNDINGBOX */
    W(0x10, bbox_sub_base(LIST_BBOX_SUB));             /* BASE for the BJUMP target */
    uint32_t bjump = s_pc;
    W(0x09, 0);                                        /* BJUMP, patched below */
    W(0x12, BBOX_THROUGH16);                           /* the object: state and a draw */
    W(0x01, bbox_sub_rel(sprite_a));
    W(0x04, (6u << 16) | 2u);                          /* PRIM: sprite A */
    W(0x9C, BBOX_FB_B & 0xFFFFFFu);                    /* FRAMEBUFPTR -> B */
    uint32_t after_object = s_pc;
    W(0x0B, 0);                                        /* RET */
    uint32_t end_pc = s_pc;
    s_pc = bjump;                                      /* as sceGuEndObject patches it */
    W(0x09, bbox_sub_rel(after_object));
    s_pc = end_pc;

    s_pc = LIST_BBOX_TAIL;
    W(0x12, BBOX_THROUGH16);
    prim(6u, 2u, sprite_b);                            /* sprite B */
    W(0x0F, 0);                                        /* FINISH */
    W(0x0C, 0);                                        /* END */

    s_pc = LIST_BBOX;
    emit_setup();
    W(0x9C, BBOX_FB_A & 0xFFFFFFu); W(0x9D, 512u); W(0xD2, 3u);
    W(0x1D, 0); W(0x1F, 0); W(0x20, 0); W(0x21, 0); W(0x22, 0);   /* cull, fog, dither, */
    W(0x24, 0); W(0x27, 0); W(0x28, 0); W(0xD3, 0);               /* blend, tests, logic op, */
    W(0xE8, 0); W(0xE9, 0);                                       /* clear mode and masks off */
    if (c->small_scissor) W(0xD5, (271u << 10) | 99u);
    W(0x12, c->vtype);
    W(0x02, BBOX_INDICES & 0xFFFFFFu);                 /* IADDR */
    W(0x0A, LIST_BBOX_SUB & 0xFFFFFFu);                /* CALL */
    W(0x08, LIST_BBOX_TAIL & 0xFFFFFFu);               /* JUMP */
    return bjump;
}

/* The developer VRAM viewer's decoder (#314): each colour PSM expands as the texture decoder
 * expands it, DEPTH16 is the VRAMDUMP grey, rows follow the stride, unsupported or unreadable
 * requests read nothing, and guest memory is left as it was. */
static void test_ge_vram_view_decode(void) {
    static const uint32_t base = 0x041C0000u;
    static const uint16_t p16[2][4] = {
        {0x001Fu, 0x07E0u, 0xDEADu, 0xBEEFu},
        {0xF800u, 0x0000u, 0xDEADu, 0xBEEFu},
    };
    uint8_t before[64];
    uint32_t out[8];
    for (unsigned y = 0; y < 2u; y++)
        for (unsigned x = 0; x < 4u; x++)
            memcpy(SR_HOST(base + (y * 4u + x) * 2u), &p16[y][x], 2u);
    memcpy(before, SR_HOST(base), sizeof before);

    /* 5650, a 2x2 window of a stride-4 surface: the second row starts one stride on. */
    memset(out, 0xA5, sizeof out);
    expect(ge_vram_decode_surface(base, 2u, 2u, 4u, 0u, out) == 1, "vram_view", "5650 decodes");
    expect(out[0] == 0xFF0000FFu && out[1] == 0xFF00FF00u && out[2] == 0xFFFF0000u &&
           out[3] == 0xFF000000u, "vram_view", "5650 expands each channel and is opaque");
    expect(out[4] == 0xA5A5A5A5u, "vram_view", "nothing is written past width * height");

    uint16_t v5551[2] = {0x801Fu, 0x7C00u};
    memcpy(SR_HOST(base + 0x100u), v5551, sizeof v5551);
    expect(ge_vram_decode_surface(base + 0x100u, 2u, 1u, 2u, 1u, out) == 1 &&
           out[0] == 0xFF0000FFu && out[1] == 0x00FF0000u, "vram_view",
           "5551 takes alpha from bit 15");
    uint16_t v4444 = 0x1234u;
    memcpy(SR_HOST(base + 0x110u), &v4444, 2u);
    expect(ge_vram_decode_surface(base + 0x110u, 1u, 1u, 1u, 2u, out) == 1 &&
           out[0] == 0x11223344u, "vram_view", "4444 scales each nibble by 17");
    uint32_t v8888 = 0x80402010u;
    memcpy(SR_HOST(base + 0x120u), &v8888, 4u);
    expect(ge_vram_decode_surface(base + 0x120u, 1u, 1u, 1u, 3u, out) == 1 &&
           out[0] == 0x80402010u, "vram_view", "8888 is the stored word");
    uint16_t depth = 0xABCDu;
    memcpy(SR_HOST(base + 0x130u), &depth, 2u);
    expect(ge_vram_decode_surface(base + 0x130u, 1u, 1u, 1u, 11u, out) == 1 &&
           out[0] == 0xFFABABABu, "vram_view", "DEPTH16 is its high byte as opaque grey");

    memset(out, 0xA5, sizeof out);
    expect(!ge_vram_decode_surface(base, 0u, 1u, 4u, 0u, out) &&
           !ge_vram_decode_surface(base, 4u, 1u, 2u, 0u, out) &&
           !ge_vram_decode_surface(base, 1u, 1u, 1u, 4u, out) &&
           !ge_vram_decode_surface(base, GE_VRAM_VIEW_MAX_WIDTH + 1u, 1u,
                                   GE_VRAM_VIEW_MAX_WIDTH + 1u, 0u, out) &&
           !ge_vram_decode_surface(base, 1u, 1u, 1u, 0u, NULL) &&
           !ge_vram_decode_surface(0xFFFFF000u, 4u, 4u, 4u, 3u, out),
           "vram_view", "a zero size, short stride, unknown format, oversize, NULL output or "
           "unreadable rectangle is refused");
    expect(out[0] == 0xA5A5A5A5u, "vram_view", "a refused request writes nothing");
    expect(memcmp(before, SR_HOST(base), sizeof before) == 0, "vram_view",
           "decoding leaves guest memory as it was");

    /* The bound texture goes through the texture decoder with the GE state as it is; the
     * state is only read. */
    GeState *state = ge_state_ptr();
    GeState saved = *state;
    uint32_t texels[2] = {0x11223344u, 0x55667788u};
    memcpy(SR_HOST(base + 0x200u), texels, sizeof texels);
    state->tex_addr = base + 0x200u;
    state->tex_w = 2;
    state->tex_h = 1;
    state->tex_bufw = 2u;
    state->tex_fmt = 3u;
    state->tex_swizzle = 0;
    GeState staged = *state;
    memset(out, 0, sizeof out);
    expect(ge_vram_decode_bound_texture(out, 8u) == 1 && out[0] == 0x11223344u &&
           out[1] == 0x55667788u, "vram_view", "the bound texture decodes as the sampler reads it");
    expect(memcmp(&staged, state, sizeof staged) == 0, "vram_view",
           "decoding the bound texture leaves the GE state as it was");
    expect(ge_vram_decode_bound_texture(out, 1u) == 0, "vram_view",
           "a texture larger than the buffer is refused");
    g_ge_capture_active = 1;
    expect(ge_vram_decode_bound_texture(out, 8u) == 0, "vram_view",
           "no texture is decoded while a GE capture runs");
    g_ge_capture_active = 0;
    state->tex_addr = 0xFFFFF000u;
    expect(ge_vram_decode_bound_texture(out, 8u) == 0, "vram_view",
           "an unreadable texture is refused");
    *state = saved;
    printf("VRAM_VIEW decode cases=11\n");
}

/* The viewer's sources and decode (#314), the functions gui.c's pause page calls: which
 * buffer, stride, size and format each source names (the depth buffer's GE-local ZBP folded
 * onto VRAM, its stride falling back to FBW and then 512), sources the state does not name are
 * absent, and a decode is opaque, sized as asked, and refused without writing. */
static void test_ge_vram_view_sources(void) {
    static const uint32_t base = 0x041C0000u;
    GeState *state = ge_state_ptr();
    GeState saved = *state;
    GeVramSources src;
    uint32_t out[8], w = 99u, h = 99u;

    memset(state, 0, sizeof(*state));
    ge_vram_view_sources(0x44088000u, 512u, 7u, 0, &src);
    expect(!src.present[GE_VRAM_VIEW_DISPLAY] && !src.present[GE_VRAM_VIEW_DRAW] &&
           !src.present[GE_VRAM_VIEW_DEPTH] && !src.present[GE_VRAM_VIEW_TEXTURE],
           "vram_view_sources", "nothing named, nothing offered");

    ge_vram_view_sources(0x44088000u, 512u, 7u, 1, &src);
    expect(src.present[GE_VRAM_VIEW_DISPLAY] && src.window[GE_VRAM_VIEW_DISPLAY].addr == 0x44088000u &&
           src.window[GE_VRAM_VIEW_DISPLAY].stride == 512u &&
           src.window[GE_VRAM_VIEW_DISPLAY].width == 480u &&
           src.window[GE_VRAM_VIEW_DISPLAY].height == 272u &&
           src.window[GE_VRAM_VIEW_DISPLAY].format == 3u, "vram_view_sources",
           "the display is the shown buffer at its stride, 480 x 272, in its colour format");

    state->fbp = 0x00044000u;
    state->fbw = 512u;
    state->fbfmt = 1u;
    state->zbp = 0x00110000u;
    ge_vram_view_sources(0, 0, 0, 0, &src);
    expect(src.present[GE_VRAM_VIEW_DRAW] && src.window[GE_VRAM_VIEW_DRAW].addr == 0x04044000u &&
           src.window[GE_VRAM_VIEW_DRAW].stride == 512u && src.window[GE_VRAM_VIEW_DRAW].format == 1u &&
           src.window[GE_VRAM_VIEW_DRAW].width == 480u && src.window[GE_VRAM_VIEW_DRAW].height == 272u &&
           src.window[GE_VRAM_VIEW_DEPTH].width == 480u && src.window[GE_VRAM_VIEW_DEPTH].height == 272u,
           "vram_view_sources", "the draw target is the GE-local FBP folded onto VRAM, 480 x 272 at FBW "
           "in FBPSM, and the depth buffer is 480 x 272");
    expect(src.present[GE_VRAM_VIEW_DEPTH] && src.window[GE_VRAM_VIEW_DEPTH].addr == 0x04110000u &&
           src.window[GE_VRAM_VIEW_DEPTH].stride == 512u &&
           src.window[GE_VRAM_VIEW_DEPTH].format == GE_VRAM_VIEW_DEPTH16, "vram_view_sources",
           "a GE-local ZBP folds onto VRAM; without ZBW the depth stride is FBW; DEPTH16");
    state->zbp = 0x44110000u;
    state->zbw = 256u;
    ge_vram_view_sources(0, 0, 0, 0, &src);
    expect(src.window[GE_VRAM_VIEW_DEPTH].addr == 0x04110000u &&
           src.window[GE_VRAM_VIEW_DEPTH].stride == 256u, "vram_view_sources",
           "an uncached ZBP folds onto VRAM and ZBW is the depth stride");
    state->fbw = 0u;
    state->zbw = 0u;
    ge_vram_view_sources(0, 0, 0, 0, &src);
    expect(!src.present[GE_VRAM_VIEW_DRAW] && src.window[GE_VRAM_VIEW_DEPTH].stride == 512u,
           "vram_view_sources", "no FBW: no draw target, and the depth stride is 512");

    state->tex_addr = base + 0x200u;
    state->tex_w = 2;
    state->tex_h = 1;
    state->tex_bufw = 0u;
    state->tex_fmt = 5u;
    state->tex_swizzle = 1;
    ge_vram_view_sources(0, 0, 0, 0, &src);
    expect(src.present[GE_VRAM_VIEW_TEXTURE] && src.window[GE_VRAM_VIEW_TEXTURE].addr == base + 0x200u &&
           src.window[GE_VRAM_VIEW_TEXTURE].stride == 2u &&
           src.window[GE_VRAM_VIEW_TEXTURE].width == 2u &&
           src.window[GE_VRAM_VIEW_TEXTURE].height == 1u &&
           src.window[GE_VRAM_VIEW_TEXTURE].format == 5u && src.texture_swizzled,
           "vram_view_sources", "the texture is its address, buffer width (else width), size, PSM "
           "and swizzle");
    state->tex_bufw = 8u;
    ge_vram_view_sources(0, 0, 0, 0, &src);
    expect(src.window[GE_VRAM_VIEW_TEXTURE].stride == 8u &&
           src.window[GE_VRAM_VIEW_TEXTURE].width == 2u, "vram_view_sources",
           "a texture's buffer width, when set, is its stride and its width stays its own");

    /* The decode: a 5551 pixel with alpha clear shows opaque, sized as the window. */
    {
        const uint16_t clear_alpha = 0x7C00u;
        const GeVramWindow window = { base + 0x100u, 1u, 1u, 1u, 1u };
        memcpy(SR_HOST(base + 0x100u), &clear_alpha, 2u);
        expect(ge_vram_view_decode(0, &window, out, 8u, &w, &h) == GE_VRAM_VIEW_SHOWN &&
               out[0] == 0xFFFF0000u && w == 1u && h == 1u, "vram_view_sources",
               "a decoded window is opaque and sized as asked");
        const GeVramWindow wide = { base, 4u, 3u, 3u, 0u };
        memset(out, 0xA5, sizeof out);
        expect(ge_vram_view_decode(0, &wide, out, 8u, &w, &h) == GE_VRAM_VIEW_NO_ROOM &&
               out[0] == 0xA5A5A5A5u && w == 0u && h == 0u, "vram_view_sources",
               "a window larger than the buffer is refused and writes nothing");
        const GeVramWindow outside = { 0xFFFFF000u, 4u, 1u, 1u, 3u };
        expect(ge_vram_view_decode(0, &outside, out, 8u, &w, &h) == GE_VRAM_VIEW_UNREADABLE &&
               out[0] == 0xA5A5A5A5u, "vram_view_sources", "an unreadable window is refused");
    }
    {
        uint32_t texels[2] = {0x00223344u, 0x55667788u};
        memcpy(SR_HOST(base + 0x200u), texels, sizeof texels);
        state->tex_fmt = 3u;
        state->tex_swizzle = 0;
        state->tex_bufw = 2u;
        expect(ge_vram_view_decode(1, NULL, out, 8u, &w, &h) == GE_VRAM_VIEW_SHOWN &&
               out[0] == 0xFF223344u && out[1] == 0xFF667788u && w == 2u && h == 1u,
               "vram_view_sources", "the bound texture decodes opaque at its own size");
        g_ge_capture_active = 1;
        expect(ge_vram_view_decode(1, NULL, out, 8u, &w, &h) == GE_VRAM_VIEW_TEXTURE_REFUSED,
               "vram_view_sources", "no texture is decoded while a GE capture runs");
        g_ge_capture_active = 0;
    }
    *state = saved;
    printf("VRAM_VIEW sources cases=13\n");
}

static void test_ge_bounding_box_branch(void) {
    /* Transform-mode boxes under emit_setup's perspective (w = -z) and 480x272 viewport. */
    s_vtx = BBOX_VERTS;
    uint32_t box_on = s_vtx;                           /* centre of the screen */
    for (int i = 0; i < 8; i++)
        vtx((i & 1) ? 0.4f : -0.4f, (i & 2) ? 0.4f : -0.4f, (i & 4) ? -6.0f : FRONT);
    uint32_t box_right = s_vtx;                        /* right after box_on: x >= 520 */
    for (int i = 0; i < 8; i++)
        vtx((i & 1) ? 9.0f : 7.0f, (i & 2) ? 0.4f : -0.4f, (i & 4) ? -6.0f : FRONT);
    uint32_t box_above = s_vtx;                        /* y < -20 */
    for (int i = 0; i < 8; i++)
        vtx((i & 1) ? 0.4f : -0.4f, (i & 2) ? 9.0f : 7.0f, (i & 4) ? -6.0f : FRONT);
    uint32_t box_spans = s_vtx;                        /* beyond the left AND right edges */
    for (int i = 0; i < 8; i++)
        vtx((i & 1) ? 9.0f : -9.0f, (i & 2) ? 0.4f : -0.4f, (i & 4) ? -6.0f : FRONT);
    /* box_right with one corner behind the eye (w = -1) whose projection also lands right of
     * the screen (x -9 / w -1): only the w test keeps this box visible. */
    uint32_t box_behind = s_vtx;
    for (int i = 0; i < 8; i++)
        vtx(i == 7 ? -9.0f : ((i & 1) ? 9.0f : 7.0f), (i & 2) ? 0.4f : -0.4f,
            i == 7 ? BEHIND : FRONT);
    for (uint8_t i = 0; i < 8u; i++) {                 /* 8-bit indices of box_right's corners */
        uint8_t index = (uint8_t)(8u + i);
        memcpy(SR_HOST(BBOX_INDICES + i), &index, 1);
    }
    uint32_t sprite_a = BBOX_SPRITES;
    uint32_t v = depth_test_vertex(sprite_a, 10u, 10u, 0u, BBOX_RED);
    v = depth_test_vertex(v, 14u, 14u, 0u, BBOX_RED);
    uint32_t sprite_b = v;
    v = depth_test_vertex(v, 30u, 10u, 0u, BBOX_GREEN);
    v = depth_test_vertex(v, 34u, 14u, 0u, BBOX_GREEN);
    uint32_t box_through = v;                          /* through-mode corners at x = 600 */
    v = depth_test_vertex(v, 600u, 10u, 0u, BBOX_RED);
    (void)depth_test_vertex(v, 610u, 20u, 0u, BBOX_RED);

    const BboxCase cases[] = {
        /* Documented: a box with a vertex in view runs the object; a box wholly outside the
         * scissor region skips it. */
        { "onscreen",  VT_TRANSFORM, box_on,     8, 0, 0, 0 },
        { "right",     VT_TRANSFORM, box_right,  8, 0, 0, 1 },
        /* A fresh list without BOUNDINGBOX does not inherit the previous list's result. */
        { "no_box",    VT_TRANSFORM, box_on,    -1, 0, 0, 0 },
        { "above",     VT_TRANSFORM, box_above,  8, 0, 0, 1 },
        { "scissor",   VT_TRANSFORM, box_on,     8, 1, 0, 1 },
        { "partial",   VT_TRANSFORM, box_on,    16, 0, 0, 0 },   /* box_on then box_right */
        { "indexed",   VT_TRANSFORM | VT_INDEX8, box_on, 8, 0, 0, 1 },
        { "stalled",   VT_TRANSFORM, box_right,  8, 0, 1, 1 },
        /* UNMEASURED on the PSP, so the object runs (the behaviour before BJUMP was
         * emulated): no single edge, a corner behind the eye, through mode, zero count. */
        { "spans",     VT_TRANSFORM, box_spans,  8, 0, 0, 0 },
        { "behind",    VT_TRANSFORM, box_behind, 8, 0, 0, 0 },
        { "through",   BBOX_THROUGH16, box_through, 2, 0, 0, 0 },
        { "zero",      VT_TRANSFORM, box_right,  0, 0, 0, 0 },
        /* Vertices past the end of guest memory: no test is possible, so the object runs. */
        { "outside",   VT_TRANSFORM, 0x0BFFFFF0u, 8, 0, 0, 0 },
    };

    GeGpuHooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.tri = hook_tri; hooks.sprite = hook_sprite; hooks.line = hook_line;
    hooks.point = hook_point; hooks.flush = hook_flush; hooks.vram_dirty = hook_vram_dirty;
    for (int hooked = 0; hooked < 2; hooked++) {
        ge_set_gpu_hooks(hooked ? &hooks : NULL);
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            const BboxCase *c = &cases[i];
            char scenario[64];
            snprintf(scenario, sizeof scenario, "bbox_%s_%s", c->name,
                     hooked ? "gpu_hooks" : "software");
            uint32_t bjump = build_bbox_list(c, sprite_a, sprite_b);
            static const unsigned xs[2] = { 11u, 31u };
            for (unsigned k = 0; k < 2u; k++) {
                MEM_W32(bbox_px(BBOX_FB_A, xs[k], 11u), BBOX_SENTINEL);
                MEM_W32(bbox_px(BBOX_FB_B, xs[k], 11u), BBOX_SENTINEL);
            }
            unsigned hook_prims = s_hook_prims;
            uint32_t next = LIST_BBOX;
            int resume = 0;
            if (c->stall) {
                g_ge_stall_addr = bjump;
                next = ge_run_list(next, 0);
                g_ge_stall_addr = 0;
                expect(next == bjump, scenario, "list did not stall on the BJUMP word");
                resume = 1;
            }
            expect(ge_run_list(next, resume) == 0, scenario,
                   "list did not reach END (PC, call stack or offset lost)");
            unsigned long prims = c->skip ? 1ul : 2ul;
            expect(g_ge_prim_count == prims, scenario,
                   c->skip ? "BJUMP did not skip the object's PRIM"
                           : "the object's PRIM did not run");
            if (hooked) {
                expect(s_hook_prims - hook_prims == (unsigned)prims, scenario,
                       "GPU hooks did not receive the same primitives");
            } else if (c->skip) {
                expect(MEM_R32(bbox_px(BBOX_FB_A, 11u, 11u)) == BBOX_SENTINEL, scenario,
                       "skipped sprite A was drawn");
                expect(MEM_R32(bbox_px(BBOX_FB_A, 31u, 11u)) == BBOX_GREEN &&
                       MEM_R32(bbox_px(BBOX_FB_B, 31u, 11u)) == BBOX_SENTINEL, scenario,
                       "skipped FRAMEBUFPTR changed the target of sprite B");
            } else {
                expect(MEM_R32(bbox_px(BBOX_FB_A, 11u, 11u)) == BBOX_RED, scenario,
                       "sprite A was not drawn");
                expect(MEM_R32(bbox_px(BBOX_FB_B, 31u, 11u)) == BBOX_GREEN &&
                       MEM_R32(bbox_px(BBOX_FB_A, 31u, 11u)) == BBOX_SENTINEL, scenario,
                       "FRAMEBUFPTR in the object did not move sprite B");
            }
        }
    }
    ge_set_gpu_hooks(NULL);
    printf("BBOX cases=%u (software and GPU hooks)\n", (unsigned)(sizeof cases / sizeof cases[0]));
}

/* ---- Render-state motion tap (#2081) on the production GE object ----
 *
 * One list on a display framebuffer with depth: a clear sprite, a triangle strip skinned by two
 * bones under fog and culling, an indexed flat-shaded quad, a through-mode sprite and a triangle
 * across the near plane; with MS_UNNAMED also the kinds the tap publishes no primitive for: a
 * line, a point, a Bezier patch and an immediate-mode triangle.
 *
 * The bar for the tap is that with no listener each of its sites in ge.c is a pointer test and
 * nothing else. So the list drawn with no listener, with one, and with none again must leave
 * byte-identical guest VRAM (colour), depth, GE state, profile counters (every counter on, on
 * this file's stepping clock, so one more clock read or count shows), frame counters and GE
 * clock, with the timing model off and on; and a listener that was removed is not called.
 *
 * With the listener (a MotionTapFrame, motion_tap.h) the store must hold the PRIMs, vertex
 * references, triangles and decisions ge.c produced. A backend stand-in that declines every
 * primitive, so the software rasterizer still draws it, must find the published primitive
 * during each call for a PRIM's triangle or sprite, and none during a call for the line, the
 * point, the patch's triangles or the immediate-mode triangle. Then two frames of
 * the list, with the bone, the quad's world matrix and the sprite moved, are matched by
 * motion_match, and the builder at t = 1 must give, bit for bit, what ge.c handed the
 * stand-in for frame B after the backend's vertex rule (the 28.4 snap, the flat-shading colour,
 * the sprite's floored corners). A frame B that also holds the patch (MS_PATCH, and drawn as
 * lines or points), or an immediate-mode triangle, line or point, cannot be built: the plan
 * refuses it as unknown. */
#define LIST_MOTION      0x08160000u
#define MOTION_VERTS     0x08168000u
#define MOTION_INDICES   0x0816C000u
#define MOTION_VRAM      0x04000000u
#define MOTION_VRAM_SIZE 0x00200000u
#define MOTION_ZBP       0x04110000u
#define MOTION_BG        0xff302010u
#define MOTION_CULL      0u          /* the face that keeps the strip's triangles */
enum {
    MS_UNNAMED = 1, MS_PATCH = 2, MS_IMMEDIATE = 4, MS_PATCH_LINES = 8, MS_PATCH_POINTS = 16, MS_IMM_LINE = 32,
    MS_IMM_POINT = 64, MS_SPRITE3D = 128
};

static uint32_t s_motion_vc;

/* col 8888 and a float position: 16 bytes. */
static void motion_vtx(uint32_t color, float x, float y, float z) {
    uint8_t *d = (uint8_t *)SR_HOST(s_motion_vc);
    const float pos[3] = { x, y, z };
    memcpy(d, &color, 4);
    memcpy(d + 4, pos, 12);
    s_motion_vc += 16u;
}

/* Two float weights before them: 24 bytes. */
static void motion_skin_vtx(float w1, uint32_t color, float x, float y, float z) {
    uint8_t *d = (uint8_t *)SR_HOST(s_motion_vc);
    const float w[2] = { 1.0f - w1, w1 }, pos[3] = { x, y, z };
    memcpy(d, w, 8);
    memcpy(d + 8, &color, 4);
    memcpy(d + 12, pos, 12);
    s_motion_vc += 24u;
}

/* What differs between frame 0 and frame 1: bone 1's translation, the quad's world x and the
 * sprite's x. Every value is a short binary fraction, so ge.c's sums are exact and the
 * expectations below can be too. */
static void motion_bone1(int frame, float t[3]) {
    t[0] = frame ? 0.25f : 0.125f;
    t[1] = frame ? 0.125f : 0.0f;
    t[2] = frame ? -0.125f : 0.0f;
}
static float motion_quad_tx(int frame) { return frame ? 0.25f : 0.125f; }
static float motion_sprite_dx(int frame) { return frame ? 2.5f : 0.0f; }
static float motion_sprite3d_dx(int frame) { return frame ? 0.05f : 0.0f; }

static void motion_strip_vertex(int i, float p[3], float *w1, uint32_t *color) {
    p[0] = -0.75f + 0.125f * (float)i;
    p[1] = (i & 1) ? 0.25f : 0.5f;
    p[2] = -0.5f + 0.125f * (float)i;
    *w1 = 0.125f * (float)i;
    *color = 0xff800000u | ((uint32_t)(255 - 40 * i) << 8) | (uint32_t)(40 * i);
}

static const float k_motion_quad[4][3] = {
    { 0.25f, -0.25f, 0.25f }, { 0.5f, -0.25f, 0.25f }, { 0.25f, -0.5f, 0.25f }, { 0.5f, -0.5f, 0.25f },
};
static const uint8_t k_motion_quad_index[6] = { 0, 1, 2, 2, 1, 3 };
static const uint32_t k_motion_quad_color[4] = { 0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xff00ffffu };

static void build_motion_list(int flags, int frame) {
    static const float ident43[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
    static const float ident44[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    const uint32_t plain = (7u << 2) | (3u << 7), through = plain | (1u << 23);
    const uint32_t skin = plain | (3u << 9) | (1u << 14);   /* two float weights */
    s_pc = LIST_MOTION;
    s_motion_vc = MOTION_VERTS;
    W(0x10, 0x080000u);                    /* BASE: vertices at 0x08xxxxxx */
    W(0x13, 0u);
    W(0x9C, MOTION_VRAM & 0x00FFFFFFu);
    W(0x9D, 512u);
    W(0xD2, 3u);
    W(0x9E, MOTION_ZBP & 0x00FFFFFFu);
    W(0x9F, 512u);
    W(0x15, 0u);
    W(0x16, (271u << 10) | 479u);
    W(0xD4, 0u);
    W(0xD5, (271u << 10) | 479u);
    W(0xD6, 0u);
    W(0xD7, 0xFFFFu);
    for (uint32_t cmd = 0x17; cmd <= 0x28; cmd++) W(cmd, 0u);   /* every unit off */
    W(0x1C, 1u);                           /* CLIPENABLE: the near plane clips */
    W(0x50, 1u);                           /* gouraud */
    W(0xDE, 7u);                           /* ZTEST: greater or equal */
    W(0xE7, 0u);                           /* depth writes on */
    W(0xE8, 0u);
    W(0xE9, 0u);

    /* PRIM 0: the clear, colour, alpha and depth. */
    uint32_t at = s_motion_vc;
    motion_vtx(MOTION_BG, 0.0f, 0.0f, 0.0f);
    motion_vtx(MOTION_BG, 480.0f, 272.0f, 0.0f);
    W(0xD3, 0x701u);
    W(0x12, through);
    W(0x01, at & 0x00FFFFFFu);
    W(0x04, (6u << 16) | 2u);
    W(0xD3, 0u);

    /* Clip is the model position: identity matrices, w = 1. Clip x -1..1 is screen x 0..480,
     * clip y 1..-1 is screen y 0..272. */
    wide_matrix(0x3A, ident43, 12);
    wide_matrix(0x3C, ident43, 12);
    wide_matrix(0x3E, ident44, 16);
    W(0x42, f24(240.0f));
    W(0x43, f24(-136.0f));
    W(0x44, f24(-16000.0f));
    W(0x45, f24(2048.0f));
    W(0x46, f24(2048.0f));
    W(0x47, f24(32768.0f));
    W(0x4C, (2048u - 240u) << 4);
    W(0x4D, (2048u - 136u) << 4);
    W(0x23, 1u);                           /* ZTESTENABLE */

    /* PRIM 1: the skinned strip. Bone 0 is the identity, bone 1 a translation; fog ends inside
     * the strip, so some of its factors are saturated. */
    float bone[24], t[3];
    motion_bone1(frame, t);
    memcpy(bone, ident43, sizeof ident43);
    memcpy(bone + 12, ident43, sizeof ident43);
    memcpy(bone + 21, t, sizeof t);
    wide_matrix(0x2A, bone, 24);
    W(0x1F, 1u);
    W(0xCD, f24(0.25f));
    W(0xCE, f24(1.0f));
    W(0xCF, 0x00404040u);
    W(0x1D, 1u);
    W(0x9B, MOTION_CULL);
    at = s_motion_vc;
    for (int i = 0; i < 6; i++) {
        float p[3], w1;
        uint32_t color;
        motion_strip_vertex(i, p, &w1, &color);
        motion_skin_vtx(w1, color, p[0], p[1], p[2]);
    }
    W(0x12, skin);
    W(0x01, at & 0x00FFFFFFu);
    W(0x04, (4u << 16) | 6u);
    W(0x1D, 0u);
    W(0x1F, 0u);

    /* PRIM 2: the quad, indexed and flat-shaded, moved by the world matrix. */
    float world[12];
    memcpy(world, ident43, sizeof world);
    world[9] = motion_quad_tx(frame);
    wide_matrix(0x3A, world, 12);
    W(0x50, 0u);
    at = s_motion_vc;
    for (int i = 0; i < 4; i++)
        motion_vtx(k_motion_quad_color[i], k_motion_quad[i][0], k_motion_quad[i][1], k_motion_quad[i][2]);
    memcpy(SR_HOST(MOTION_INDICES), k_motion_quad_index, sizeof k_motion_quad_index);
    W(0x12, plain | VT_INDEX8);
    W(0x01, at & 0x00FFFFFFu);
    W(0x02, MOTION_INDICES & 0x00FFFFFFu);
    W(0x04, (3u << 16) | 6u);
    W(0x50, 1u);
    wide_matrix(0x3A, ident43, 12);

    /* PRIM 3: the through-mode sprite, moved in its vertex data. */
    at = s_motion_vc;
    motion_vtx(0xff00a0f0u, 300.25f + motion_sprite_dx(frame), 20.5f, 9000.0f);
    motion_vtx(0xff10b0e0u, 340.75f + motion_sprite_dx(frame), 60.25f, 1000.0f);
    W(0x12, through);
    W(0x01, at & 0x00FFFFFFu);
    W(0x04, (6u << 16) | 2u);

    /* PRIM 4: a triangle whose third vertex is behind the near plane (z + w < 0). */
    at = s_motion_vc;
    motion_vtx(0xffc0c0c0u, -0.5f, -0.5f, 0.0f);
    motion_vtx(0xffc0c0c0u, -0.25f, -0.5f, 0.0f);
    motion_vtx(0xffc0c0c0u, -0.375f, -0.25f, -3.0f);
    W(0x12, plain);
    W(0x01, at & 0x00FFFFFFu);
    W(0x04, (3u << 16) | 3u);

    if (flags & MS_SPRITE3D) {
        /* PRIM 5: two transform-mode sprites. The first is dropped on its first corner, which
         * lies off the GE's screen grid (clip x 10), and does not move. The second moves in its
         * vertex data; its corners differ in depth and colour, and its left one lies past the
         * screen's left edge, so with Fill on it is drawn again in the left wing. */
        at = s_motion_vc;
        motion_vtx(0xff2040ffu, 10.0f, 0.5f, 0.0f);
        motion_vtx(0xff2040ffu, 0.25f, 0.6f, 0.0f);
        motion_vtx(0xff80ff20u, -1.05f + motion_sprite3d_dx(frame), 0.75f, 0.25f);
        motion_vtx(0xff4010c0u, -0.75f + motion_sprite3d_dx(frame), 0.55f, -0.25f);
        W(0x12, plain);
        W(0x01, at & 0x00FFFFFFu);
        W(0x04, (6u << 16) | 4u);
    }

    if (flags & (MS_UNNAMED | MS_PATCH | MS_PATCH_LINES | MS_PATCH_POINTS)) {
        const uint32_t patch_prim = (flags & MS_PATCH_LINES) ? 1u : (flags & MS_PATCH_POINTS) ? 2u : 0u;
        /* A 4x4 Bezier patch over clip x 0.625..0.8125, y 0.6875..0.875: triangles, no PRIM.
         * It and the immediate-mode triangle follow PRIM 4's last fan piece with no PRIM
         * command between: only the end of that piece's raster call keeps them from being
         * taken for it. */
        at = s_motion_vc;
        for (int row = 0; row < 4; row++)
            for (int col = 0; col < 4; col++)
                motion_vtx(0xff40ff40u, 0.625f + 0.0625f * (float)col, 0.875f - 0.0625f * (float)row, 0.0f);
        W(0x01, at & 0x00FFFFFFu);
        W(0x36, (1u << 8) | 1u);           /* PATCHDIVISION 1x1 */
        W(0x37, patch_prim);               /* PATCHPRIMITIVE triangles, lines or points */
        W(0x05, 0x0404u);                  /* BEZIER 4x4 */
        if (patch_prim) W(0x37, 0u);       /* back to triangles for whatever runs next */
    }
    if (flags & (MS_UNNAMED | MS_IMMEDIATE)) {
        /* An immediate-mode triangle: vertices latched by commands, no PRIM. */
        W(0x12, through);
        W(0xF6, 0x00FFFFFFu);
        W(0xF8, 255u);
        immediate_vertex(3u, 20, 150);
        immediate_vertex(7u, 60, 150);
        immediate_vertex(7u, 20, 180);
    }
    if (flags & (MS_IMM_LINE | MS_IMM_POINT)) {
        /* An immediate-mode line or point, flushed by a NOP. */
        W(0x12, through);
        W(0xF6, 0x00FFFFFFu);
        W(0xF8, 255u);
        if (flags & MS_IMM_LINE) {
            immediate_vertex(1u, 20, 150);
            immediate_vertex(7u, 60, 170);
        } else {
            immediate_vertex(0u, 30, 160);
        }
        W(0x00, 0u);
    }
    if (flags & MS_UNNAMED) {
        /* PRIM 5, a line, and PRIM 6, a point. */
        at = s_motion_vc;
        motion_vtx(0xffffffffu, 0.0f, 0.875f, 0.0f);
        motion_vtx(0xffffffffu, 0.5f, 0.875f, 0.0f);
        motion_vtx(0xffffffffu, -0.875f, 0.875f, 0.0f);
        W(0x12, plain);
        W(0x01, at & 0x00FFFFFFu);
        W(0x04, (1u << 16) | 2u);
        W(0x04, (0u << 16) | 1u);
    }
    W(0x0F, 0u);
    W(0x0C, 0u);
}

/* Everything one run of the list left behind. */
typedef struct {
    uint8_t vram[MOTION_VRAM_SIZE];
    uint16_t depth[272 * 512];
    GeState state;
    GeCpuProfileStats profile;
    uint32_t prims;
    unsigned long pixels, list[6];
    double clk;
} MotionRun;

static void motion_run(int flags, int frame, MotionRun *r) {
    memset(r, 0, sizeof *r);
    memset(SR_HOST(MOTION_VRAM), 0, MOTION_VRAM_SIZE);
    build_motion_list(flags, frame);
    configure("1", "1", "1", "1");         /* both profiles, every sample: all counters, from zero */
    const uint32_t prims = g_frame_prims;
    const unsigned long pixels = g_ge_pixels;
    expect(ge_run_list(LIST_MOTION, 0) == 0, "motion_tap", "list did not reach END");
    r->prims = g_frame_prims - prims;
    r->pixels = g_ge_pixels - pixels;
    r->list[0] = g_ge_list_through_cmds;
    r->list[1] = g_ge_list_transform_cmds;
    r->list[2] = g_ge_list_through_vertices;
    r->list[3] = g_ge_list_transform_vertices;
    r->list[4] = g_ge_list_through_sprites;
    r->list[5] = g_ge_list_transform_sprites;
    r->clk = ge_timing_run_clk();
    r->profile = snapshot();
    memcpy(&r->state, ge_state_ptr(), sizeof r->state);
    memcpy(r->vram, SR_HOST(MOTION_VRAM), MOTION_VRAM_SIZE);
    const uint16_t *depth = ge_zbuf_ptr();
    expect(depth != NULL, "motion_tap", "no depth surface");
    if (depth) memcpy(r->depth, depth, sizeof r->depth);
}

/* 1 when the two runs left the same of everything; says what differs otherwise. */
static int motion_same(const MotionRun *a, const MotionRun *b, const char *legs) {
    const struct { int same; const char *what; } part[] = {
        { memcmp(a->vram, b->vram, sizeof a->vram) == 0, "guest VRAM" },
        { memcmp(a->depth, b->depth, sizeof a->depth) == 0, "depth" },
        { memcmp(&a->state, &b->state, sizeof a->state) == 0, "GE state" },
        { same_profile(&a->profile, &b->profile), "profile counters" },
        { a->prims == b->prims && a->pixels == b->pixels && memcmp(a->list, b->list, sizeof a->list) == 0,
          "frame and list counters" },
        { a->clk == b->clk, "GE clock" },
    };
    int same = 1;
    for (size_t i = 0; i < sizeof part / sizeof part[0]; i++)
        if (!part[i].same) {
            char what[128];
            snprintf(what, sizeof what, "%s: %s differs", legs, part[i].what);
            expect(0, "motion_tap", what);
            same = 0;
        }
    return same;
}

static uint32_t motion_px(const MotionRun *r, unsigned x, unsigned y) {
    uint32_t px;
    memcpy(&px, r->vram + ((size_t)y * 512u + x) * 4u, 4u);
    return px;
}

/* The backend stand-in: it notes what it was handed and what the store called current, and
 * declines, so the software rasterizer draws the primitive. */
typedef struct {
    int n;                          /* vertices: 3 a triangle, 2 a sprite or a line, 1 a point */
    int line;
    int current;                    /* the store named a primitive for this call */
    MotionTapCurrent cur;
    GeVtx v[3];
} MotionSeen;

static MotionSeen s_motion_seen[64];
static unsigned s_motion_nseen;
static MotionTapFrame *s_motion_store;

static int motion_see(const GeVtx *a, const GeVtx *b, const GeVtx *c, int line) {
    if (s_motion_nseen < sizeof s_motion_seen / sizeof s_motion_seen[0]) {
        MotionSeen *s = &s_motion_seen[s_motion_nseen];
        memset(s, 0, sizeof *s);
        s->v[0] = *a;
        s->n = 1;
        if (b) s->v[s->n++] = *b;
        if (c) s->v[s->n++] = *c;
        s->line = line;
        s->current = motion_tap_current(s_motion_store, &s->cur);
    }
    s_motion_nseen++;
    return 0;
}
static int motion_hook_tri(const GeVtx *a, const GeVtx *b, const GeVtx *c, int persp) {
    (void)persp;
    return motion_see(a, b, c, 0);
}
static int motion_hook_sprite(const GeVtx *a, const GeVtx *b, int persp) {
    (void)persp;
    return motion_see(a, b, NULL, 0);
}
static int motion_hook_line(const GeVtx *a, const GeVtx *b, int persp) {
    (void)persp;
    return motion_see(a, b, NULL, 1);
}
static int motion_hook_point(const GeVtx *a, int persp) {
    (void)persp;
    return motion_see(a, NULL, NULL, 1);
}
/* Fill's wing copies: counted, with whether the store called a primitive current then. */
static unsigned s_motion_wing_calls, s_motion_wing_current;
static int motion_hook_wing_tri(uint32_t fb, int x0, int x1, const GeVtx *a, const GeVtx *b, const GeVtx *c,
                                int persp) {
    (void)fb; (void)x0; (void)x1; (void)a; (void)b; (void)c; (void)persp;
    s_motion_wing_calls++;
    if (motion_tap_current(s_motion_store, NULL)) s_motion_wing_current++;
    return 0;
}
static int motion_hook_wing_sprite(uint32_t fb, int x0, int x1, const GeVtx *p0, const GeVtx *p1, int persp) {
    (void)fb; (void)x0; (void)x1; (void)p0; (void)p1; (void)persp;
    s_motion_wing_calls++;
    if (motion_tap_current(s_motion_store, NULL)) s_motion_wing_current++;
    return 0;
}
static void motion_hook_flush(const char *reason) { (void)reason; }
static void motion_hook_vram_dirty(uint32_t addr, uint32_t bytes) { (void)addr; (void)bytes; }

static const float k_motion_viewport[6] = { 240.0f, -136.0f, -16000.0f, 2048.0f, 2048.0f, 32768.0f };

/* The store after frame `frame` of the list: the PRIMs, references, triangles and decisions. */
static void motion_expect_store(const MotionTapFrame *f, int flags, int frame) {
    static const struct { uint8_t type, through; uint32_t count, ntris, submitted; } want[7] = {
        { 6, 1, 2, 1, 1 }, { 4, 0, 6, 4, 4 }, { 3, 0, 6, 2, 2 }, { 6, 1, 2, 1, 1 }, { 3, 0, 3, 1, 2 },
        { 1, 0, 2, 0, 0 }, { 0, 0, 1, 0, 0 },
    };
    static const struct { uint8_t type, through; uint32_t count, ntris, submitted; } want_sprite3d = { 6, 0, 4, 2, 1 };
    const int sprite3d = (flags & MS_SPRITE3D) != 0;
    const uint32_t nprims = (flags & MS_UNNAMED) ? 7u : sprite3d ? 6u : 5u, ntris = sprite3d ? 11u : 9u;
    const GeState *ge = ge_state_ptr();
    MotionTapView v;
    motion_tap_frame_view(f, &v);
    expect(v.nprims == nprims && v.ntris == ntris &&
           v.nrefs == ((flags & MS_UNNAMED) ? 22u : sprite3d ? 23u : 19u) && (v.flags & ~MOTION_TAP_CLOSED) == 0u &&
           v.dropped == 0u && v.late == 0u, "motion_tap",
           "the store holds every PRIM, triangle and sprite of the list");
    if (v.nprims != nprims || v.ntris != ntris) return;
    for (uint32_t s = 0; s < nprims; s++) {
        const MotionTapPrim *p = &v.prims[s];
        const uint8_t wtype = sprite3d && s == 5u ? want_sprite3d.type : want[s].type;
        const uint8_t wthrough = sprite3d && s == 5u ? want_sprite3d.through : want[s].through;
        const uint32_t wcount = sprite3d && s == 5u ? want_sprite3d.count : want[s].count;
        const uint32_t wntris = sprite3d && s == 5u ? want_sprite3d.ntris : want[s].ntris;
        const uint32_t wsub = sprite3d && s == 5u ? want_sprite3d.submitted : want[s].submitted;
        expect(p->type == wtype && p->through == wthrough && p->count == wcount && p->ntris == wntris &&
               p->submitted == wsub, "motion_tap", "a PRIM's type, mode, vertex count, records and raster calls");
        expect(p->list_addr == LIST_MOTION && (s == 0 || p->cmd_addr > v.prims[s - 1].cmd_addr) &&
               p->cmd_addr > LIST_MOTION && p->fbp == ge->fbp && p->fbw == 512u && p->zbp == ge->zbp &&
               p->zbw == 512u && p->tex_addr == 0u && p->clut_addr == 0u, "motion_tap",
               "a PRIM's list, command address and target");
        /* The clear comes before the list sets the viewport and offset. */
        expect((p->flags & GE_MOTION_PRIM_DEPTH_CLIP) &&
               (s == 0 || (p->offset[0] == 1808.0f && p->offset[1] == 1912.0f &&
                           memcmp(p->viewport, k_motion_viewport, sizeof p->viewport) == 0)),
               "motion_tap", "a PRIM's projection state");
    }
    /* PRIM 0, the clear: a through-mode sprite in clear mode, its corners as stored. */
    const MotionTapPrim *p = &v.prims[0];
    const GeMotionVtx *r = &v.refs[p->first_ref];
    expect(p->state_class == GE_MOTION_CLASS_CLEAR && (p->flags & GE_MOTION_PRIM_FLAT) &&
           !(p->flags & (GE_MOTION_PRIM_CULL | GE_MOTION_PRIM_FOG | GE_MOTION_PRIM_INDEXED)) &&
           p->camera == MOTION_TAP_NO_CAMERA && p->index_state == MOTION_INDEX_NONE && p->ibase == 0u, "motion_tap",
           "the clear's state");
    expect(v.tris[p->first_tri].decision == GE_MOTION_DRAW && v.tris[p->first_tri].pos[0] == 0 &&
           v.tris[p->first_tri].pos[1] == 1 && v.tris[p->first_tri].pos[2] == 2 && r[1].clip[0] == 480.0f &&
           r[1].clip[1] == 272.0f && r[1].clip[3] == 1.0f && r[0].fog_arg == 1.0f && r[1].r == 0x10 &&
           r[1].g == 0x20 && r[1].b == 0x30 && r[1].a == 0xff, "motion_tap", "the clear's sprite record and corners");

    /* PRIM 1, the strip: every triangle drawn, positions in submission order (the cull swap on
     * every other triangle), and each reference the skinned position, fog argument and colour. */
    p = &v.prims[1];
    r = &v.refs[p->first_ref];
    expect(p->state_class == (GE_MOTION_CLASS_ZTEST | GE_MOTION_CLASS_ZWRITE) &&
           (p->flags & GE_MOTION_PRIM_CULL) && (p->flags & GE_MOTION_PRIM_FOG) && !(p->flags & GE_MOTION_PRIM_FLAT) &&
           p->camera == 0u && p->vtype == ((7u << 2) | (3u << 7) | (3u << 9) | (1u << 14)) &&
           p->vbase == MOTION_VERTS + 32u, "motion_tap", "the strip's state");
    for (int i = 2; i < 6; i++) {
        const MotionTapTri *t = &v.tris[p->first_tri + (uint32_t)(i - 2)];
        const int swap = ((MOTION_CULL & 1u) == 0u) ^ (i & 1);
        expect(t->decision == GE_MOTION_DRAW && t->pieces == 0u && t->pos[0] == (swap ? i - 1 : i - 2) &&
               t->pos[1] == (swap ? i - 2 : i - 1) && t->pos[2] == i, "motion_tap",
               "a strip triangle's decision and submission order");
    }
    float bone1[3];
    motion_bone1(frame, bone1);
    for (int i = 0; i < 6; i++) {
        float pos[3], w1, clip[3];
        uint32_t color;
        motion_strip_vertex(i, pos, &w1, &color);
        for (int a = 0; a < 3; a++) clip[a] = pos[a] * (1.0f - w1) + (pos[a] + bone1[a]) * w1;
        expect(v.ref_set[p->first_ref + (uint32_t)i] && r[i].clip[0] == clip[0] && r[i].clip[1] == clip[1] &&
               r[i].clip[2] == clip[2] && r[i].clip[3] == 1.0f, "motion_tap", "a strip reference's clip position");
        expect(r[i].fog_arg == clip[2] + 0.25f && r[i].r == (int)(color & 0xffu) &&
               r[i].g == (int)((color >> 8) & 0xffu) && r[i].b == 0x80 && r[i].a == 0xff, "motion_tap",
               "a strip reference's unclamped fog argument and colour");
    }
    expect(r[0].fog_arg < 0.0f && r[5].fog_arg > 0.0f && r[5].fog_arg < 1.0f, "motion_tap",
           "the strip has a fog argument below the clamp and one inside it");

    /* PRIM 2, the quad: indexed, so position i is the vertex its index names. */
    p = &v.prims[2];
    r = &v.refs[p->first_ref];
    expect((p->flags & GE_MOTION_PRIM_INDEXED) && (p->flags & GE_MOTION_PRIM_FLAT) && !(p->flags & GE_MOTION_PRIM_CULL) &&
           p->index_state == MOTION_INDEX_HASHED && p->index_size == 6u && p->ibase == MOTION_INDICES &&
           p->vtype == ((7u << 2) | (3u << 7) | VT_INDEX8), "motion_tap", "the quad's state and index bytes");
    for (int i = 0; i < 6; i++) {
        const float *q = k_motion_quad[k_motion_quad_index[i]];
        expect(r[i].clip[0] == q[0] + motion_quad_tx(frame) && r[i].clip[1] == q[1] && r[i].clip[2] == q[2] &&
               r[i].clip[3] == 1.0f && r[i].fog_arg == 1.0f &&
               r[i].r == (int)(k_motion_quad_color[k_motion_quad_index[i]] & 0xffu), "motion_tap",
               "a quad reference is the vertex its index names, moved by the world matrix");
    }
    expect(v.tris[p->first_tri].decision == GE_MOTION_DRAW && v.tris[p->first_tri + 1u].decision == GE_MOTION_DRAW &&
           v.tris[p->first_tri + 1u].pos[0] == 3 && v.tris[p->first_tri + 1u].pos[2] == 5, "motion_tap",
           "the quad's two triangles");

    /* PRIM 3, the sprite. */
    p = &v.prims[3];
    r = &v.refs[p->first_ref];
    expect(v.tris[p->first_tri].decision == GE_MOTION_DRAW && r[0].clip[0] == 300.25f + motion_sprite_dx(frame) &&
           r[0].clip[1] == 20.5f && r[0].clip[2] == 9000.0f && r[1].clip[0] == 340.75f + motion_sprite_dx(frame) &&
           r[1].clip[1] == 60.25f && r[1].clip[2] == 1000.0f && r[1].clip[3] == 1.0f && r[1].r == 0xe0, "motion_tap",
           "the sprite's corners");

    /* PRIM 4, across the near plane: one record, clipped, both fan pieces handed over. */
    p = &v.prims[4];
    r = &v.refs[p->first_ref];
    expect(v.tris[p->first_tri].decision == GE_MOTION_CLIP && v.tris[p->first_tri].pieces == 3u &&
           r[2].clip[2] == -3.0f && r[2].clip[3] == 1.0f && r[0].clip[0] == -0.5f, "motion_tap",
           "the near-clipped triangle's decision, fan pieces and source vertices");

    /* PRIMs 5 and 6, the line and the point: PRIMs with no record and nothing handed over under
     * their name. The patch and the immediate-mode triangle are not PRIMs and leave nothing. */
    if (flags & MS_UNNAMED)
        expect(v.prims[5].ntris == 0u && !v.ref_set[v.prims[5].first_ref] && !v.ref_set[v.prims[5].first_ref + 1u] &&
               v.prims[6].ntris == 0u && !v.ref_set[v.prims[6].first_ref], "motion_tap",
               "the line and point PRIMs hold no record");
    /* PRIM 5, the transform-mode sprites: one record each, the first dropped on its first corner
     * (one corner computed), the second drawn with both corners in order. */
    if (sprite3d) {
        const float dx = motion_sprite3d_dx(frame);
        p = &v.prims[5];
        r = &v.refs[p->first_ref];
        const MotionTapTri *t0 = &v.tris[p->first_tri], *t1 = &v.tris[p->first_tri + 1u];
        expect(p->camera == 0u && t0->decision == GE_MOTION_DROP && t0->pos[0] == 0 && t0->pos[1] == 1 &&
               t0->pos[2] == 1 && t1->decision == GE_MOTION_DRAW && t1->pos[0] == 2 && t1->pos[1] == 3 &&
               t1->pos[2] == 2 && v.ref_set[p->first_ref] && !v.ref_set[p->first_ref + 1u] && r[0].clip[0] == 10.0f &&
               r[2].clip[0] == -1.05f + dx && r[2].clip[2] == 0.25f && r[2].r == 0x20 && r[3].clip[0] == -0.75f + dx &&
               r[3].clip[2] == -0.25f && r[3].r == 0xc0 && r[3].clip[3] == 1.0f, "motion_tap",
               "the transform-mode sprites: the first dropped on its first corner, the second's corners as computed");
    }
    expect(v.ncameras == 1u && v.cameras[0].verts == ((flags & MS_UNNAMED) ? 18u : sprite3d ? 19u : 15u) &&
           v.cameras[0].view[0] == 1.0f && v.cameras[0].proj[15] == 1.0f && v.cameras[0].proj[11] == 0.0f,
           "motion_tap", "one camera, used by every transform-mode vertex");
    expect(!motion_tap_current(f, NULL), "motion_tap", "nothing is current once the list has ended");
}

static void test_ge_motion_tap(void) {
    MotionRun *off = (MotionRun *)malloc(sizeof *off), *on = (MotionRun *)malloc(sizeof *on);
    MotionRun *again = (MotionRun *)malloc(sizeof *again);
    MotionTapFrame *f = motion_tap_frame_create(0), *fa = motion_tap_frame_create(0);
    MotionTapFrame *fb = motion_tap_frame_create(0);
    MotionBuild *build = motion_build_create();
    if (!off || !on || !again || !f || !fa || !fb || !build) {
        expect(0, "motion_tap", "no memory");
        goto done;
    }

    /* No listener, a listener, no listener again: nothing ge.c produces may differ. */
    for (int timing = 0; timing < 2; timing++) {
        MotionTapView before, after;
        ge_timing_set_enabled(timing);
        motion_tap_frame_reset(f);
        /* One run first, so every leg starts from what a run of this list leaves: GE state the
         * list does not set, the depth mirror, the immediate-mode stream. */
        motion_run(MS_UNNAMED, 0, off);
        motion_run(MS_UNNAMED, 0, off);
        ge_motion_tap_set(motion_tap_frame_listener(f));
        motion_run(MS_UNNAMED, 0, on);
        ge_motion_tap_set(NULL);
        motion_tap_frame_view(f, &before);
        motion_run(MS_UNNAMED, 0, again);
        motion_tap_frame_view(f, &after);
        const int same_on = motion_same(off, on, timing ? "no listener against a listener, timing model on"
                                                        : "no listener against a listener");
        const int same_again = motion_same(off, again, timing ? "no listener against a removed one, timing model on"
                                                              : "no listener against a removed one");
        expect(same_on && same_again, "motion_tap",
               "a listener changes none of ge.c's results: VRAM, depth, GE state, counters and clock");
        expect(before.nprims == 7u && after.nprims == before.nprims && after.ntris == before.ntris &&
               after.nrefs == before.nrefs && after.late == 0u && after.dropped == 0u, "motion_tap",
               "a removed listener is not called");
        expect(off->prims >= 7u && off->pixels > 0ul && (!timing || off->clk > 0.0) && off->list[0] >= 2ul &&
               off->list[1] >= 5ul, "motion_tap", "the list drew its seven PRIMs");
        /* The scene is really there: each shape left pixels, so a draw the tap suppressed or
         * a clip that drew nothing could not pass as identical. */
        expect(motion_px(off, 10u, 260u) == MOTION_BG && motion_px(off, 120u, 80u) != MOTION_BG &&
               motion_px(off, 350u, 190u) != MOTION_BG && motion_px(off, 320u, 40u) == 0xff10b0e0u &&
               motion_px(off, 145u, 195u) != MOTION_BG &&
               (motion_px(off, 300u, 17u) == 0xffffffffu || motion_px(off, 300u, 16u) == 0xffffffffu) &&
               motion_px(off, 410u, 30u) != MOTION_BG, "motion_tap",
               "the strip, quad, sprite, clipped triangle, line and patch are in the frame");
        if (!timing) motion_expect_store(f, MS_UNNAMED, 0);
    }
    ge_timing_set_enabled(0);

    /* The stand-in backend: what is current during each hook call. */
    GeGpuHooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.tri = motion_hook_tri;
    hooks.sprite = motion_hook_sprite;
    hooks.line = motion_hook_line;
    hooks.point = motion_hook_point;
    hooks.flush = motion_hook_flush;
    hooks.vram_dirty = motion_hook_vram_dirty;
    ge_set_gpu_hooks(&hooks);
    motion_tap_frame_reset(f);
    s_motion_store = f;
    s_motion_nseen = 0;
    ge_motion_tap_set(motion_tap_frame_listener(f));
    motion_run(MS_UNNAMED, 0, on);
    ge_motion_tap_set(NULL);
    expect(memcmp(on->vram, off->vram, sizeof on->vram) == 0 && memcmp(on->depth, off->depth, sizeof on->depth) == 0,
           "motion_tap", "a declining backend and a listener leave the software frame as it was");
    /* The PRIMs' own: 1 clear, 4 strip triangles, 2 quad triangles, 1 sprite, 2 fan pieces. Then
     * the unnamed ones: the patch's triangles, the immediate-mode triangle, the line, the point. */
    static const struct { int n, kind; uint32_t prim, tri; int piece; } named[10] = {
        { 2, MOTION_CUR_SPRITE, 0, 0, 0 }, { 3, MOTION_CUR_TRI, 1, 1, 0 }, { 3, MOTION_CUR_TRI, 1, 2, 0 },
        { 3, MOTION_CUR_TRI, 1, 3, 0 },    { 3, MOTION_CUR_TRI, 1, 4, 0 }, { 3, MOTION_CUR_TRI, 2, 5, 0 },
        { 3, MOTION_CUR_TRI, 2, 6, 0 },    { 2, MOTION_CUR_SPRITE, 3, 7, 0 }, { 3, MOTION_CUR_FAN, 4, 8, 0 },
        { 3, MOTION_CUR_FAN, 4, 8, 1 },
    };
    unsigned unnamed_lines = 0, unnamed_points = 0, unnamed_tris = 0;
    expect(s_motion_nseen >= 14u && s_motion_nseen <= 64u, "motion_tap", "the stand-in was offered every primitive");
    for (unsigned i = 0; i < s_motion_nseen && i < 64u; i++) {
        const MotionSeen *s = &s_motion_seen[i];
        if (i < 10u) {
            expect(s->n == named[i].n && !s->line && s->current && s->cur.kind == named[i].kind &&
                   s->cur.prim == named[i].prim && s->cur.tri == named[i].tri && s->cur.piece == named[i].piece,
                   "motion_tap", "during a hook call the store names the PRIM's primitive being drawn");
            continue;
        }
        expect(!s->current, "motion_tap",
               "a line, a point, a patch triangle or an immediate-mode triangle has no current primitive");
        if (s->line && s->n == 2) unnamed_lines++;
        else if (s->line) unnamed_points++;
        else if (s->n == 3) unnamed_tris++;
    }
    /* A 1x1 patch is two triangles; the immediate-mode triangle is the third. */
    expect(unnamed_lines == 1u && unnamed_points == 1u && unnamed_tris >= 3u, "motion_tap",
           "the line, the point, the patch and the immediate-mode triangle each reached the stand-in");
    MotionTapView vu;
    motion_tap_frame_view(f, &vu);
    expect(vu.unnamed == unnamed_tris + unnamed_lines + unnamed_points && vu.flags == 0u && vu.dropped == 0u,
           "motion_tap", "the store counts each raster call the tap named nothing for: the patch's and the "
           "immediate-mode triangles, the line and the point");
    motion_expect_store(f, MS_UNNAMED, 0);

    /* Two frames, matched, and frame B built at t = 1 against what ge.c handed over for it: the
     * list with the transform-mode sprites (MS_SPRITE3D), that list again with Fill on, and the
     * list as it is, which the checks after the loop use. With Fill on the stand-in also takes
     * the wing hooks, and no primitive may be current during a wing copy. */
    MotionMatchFrame ma, mb;
    MotionMatchParams params;
    MotionMatchResult match;
    MotionTapView vb;
    int32_t pair[8];
    const MotionBuildResult *res = motion_build_result(build);
    unsigned checked = 0, fans = 0;
    static const struct { int flags, wing; } runs[3] = { { MS_SPRITE3D, 0 }, { MS_SPRITE3D, 64 }, { 0, 0 } };
    for (int run = 0; run < 3; run++) {
        const int rflags = runs[run].flags, sprite3d = (rflags & MS_SPRITE3D) != 0;
        const uint32_t nlisted = sprite3d ? 6u : 5u;
        hooks.wing_tri = runs[run].wing ? motion_hook_wing_tri : NULL;
        hooks.wing_sprite = runs[run].wing ? motion_hook_wing_sprite : NULL;
        s_motion_wing_calls = s_motion_wing_current = 0;
        if (runs[run].wing) {
            expect(ge_wide_set_wing(runs[run].wing) == runs[run].wing, "motion_tap", "Fill takes the wing width");
            ge_wide_note_display(MOTION_VRAM);
        }
        ge_set_gpu_hooks(&hooks);
        motion_tap_frame_reset(fa);
        motion_tap_frame_reset(fb);
        s_motion_store = fa;
        ge_motion_tap_set(motion_tap_frame_listener(fa));
        motion_run(rflags, 0, on);
        motion_tap_frame_close(fa, NULL);
        s_motion_store = fb;
        s_motion_nseen = 0;
        ge_motion_tap_set(motion_tap_frame_listener(fb));
        motion_run(rflags, 1, on);
        ge_motion_tap_set(NULL);
        ge_set_gpu_hooks(NULL);
        if (runs[run].wing) {
            expect(s_motion_wing_calls > 0u && s_motion_wing_current == 0u, "motion_tap",
                   "Fill copied primitives into a wing, and no wing copy was taken for the primitive it copies");
            ge_wide_set_wing(0);
        }
        motion_tap_frame_close(fb, fa);
        motion_expect_store(fa, rflags, 0);
        motion_expect_store(fb, rflags, 1);

        motion_tap_match_frame(fa, &ma);
        motion_tap_match_frame(fb, &mb);
        motion_tap_frame_view(fb, &vb);
        motion_match_default_params(&params);
        expect(ma.nprims == nlisted && mb.nprims == nlisted, "motion_tap",
               "the matcher's list holds the PRIMs that drew");
        if (ma.nprims != nlisted || mb.nprims != nlisted) goto done;
        /* The quad at clip x 0.5..0.75, y -0.25..-0.5 in frame 1: screen x 360..420, y 170..204. */
        expect(mb.prims[2].bbox[0] == 360.0f && mb.prims[2].bbox[1] == 170.0f && mb.prims[2].bbox[2] == 420.0f &&
               mb.prims[2].bbox[3] == 204.0f && mb.prims[2].iaddr == MOTION_INDICES && mb.prims[2].flags == 0u &&
               mb.camera.valid, "motion_tap", "a PRIM's box is of the vertices ge.c handed to the rasterizer");
        expect(motion_match(&ma, &mb, &params, NULL, pair, NULL, &match) == 0 && match.flags == 0u &&
               pair[0] == 0 && pair[1] == 1 && pair[2] == 2 && pair[3] == 3 && pair[4] == 4 &&
               (!sprite3d || pair[5] == 5), "motion_tap", "the matcher pairs every PRIM that drew");
        const int planned = motion_build_plan(build, fa, fb, pair, &match);
        if (planned != 1) fprintf(stderr, "motion_tap: run %d: why %#x\n", run, (unsigned)res->why);
        expect(planned == 1 && res->why == 0u && res->paired == nlisted && res->unpaired == 0u, "motion_tap",
               "the frame between the two can be built");
        /* Built: 4 strip triangles, 2 quad triangles, the sprite, and the moving transform-mode
         * sprite. B's own: the clear and the triangle across the near plane, neither of which
         * moved. The dropped transform-mode sprite did not move either, so it is nothing. */
        expect(res->built == (sprite3d ? 8u : 7u) && res->copied == 2u, "motion_tap",
               "the moving primitives are built, two are B's own");
        checked = fans = 0;
        for (unsigned i = 0; i < s_motion_nseen && i < 64u; i++) {
            const MotionSeen *s = &s_motion_seen[i];
            MotionBuiltVert sub[3], out[3];
            expect(s->current, "motion_tap", "every primitive of frame B has a source");
            if (!s->current) continue;
            const int kind = motion_build_prim(build, s->cur.tri, 1.0f, out, NULL);
            if (s->cur.kind == MOTION_CUR_FAN) {
                expect(kind == MOTION_ITEM_COPY_B, "motion_tap", "a still near-clipped triangle's pieces are B's own");
                fans++;
                continue;
            }
            if (s->cur.prim == 0u) {
                expect(kind == MOTION_ITEM_COPY_B, "motion_tap", "the clear is B's own");
                continue;
            }
            /* The backend's vertex rule (put_vert) on what ge.c handed over: motion_tap.c's own
             * statement of it, which motion_tap_selftest checks against its restatement of put_vert. */
            memset(sub, 0, sizeof sub);
            motion_backend_vertices((vb.prims[s->cur.prim].flags & GE_MOTION_PRIM_FLAT) != 0, s->v, s->n, sub);
            expect(kind == MOTION_ITEM_BUILT && motion_build_check(build, s->cur.tri, sub) == 1, "motion_tap",
                   "t = 1 gives the vertices ge.c handed over for frame B, bit for bit");
            for (int c = 0; kind == MOTION_ITEM_BUILT && c < s->n; c++)
                expect(memcmp(&out[c].x, &sub[c].x, 4) == 0 && memcmp(&out[c].y, &sub[c].y, 4) == 0 &&
                       memcmp(&out[c].z, &sub[c].z, 4) == 0 && memcmp(&out[c].rw, &sub[c].rw, 4) == 0 &&
                       memcmp(&out[c].fog, &sub[c].fog, 4) == 0 && out[c].rgba == sub[c].rgba &&
                       (s->n == 2 || (memcmp(&out[c].u, &sub[c].u, 4) == 0 && memcmp(&out[c].v, &sub[c].v, 4) == 0)),
                       "motion_tap", "a vertex built at t = 1 is not the one ge.c handed over");
            checked++;
        }
        expect(checked == (sprite3d ? 8u : 7u) && fans == 2u && res->t1_mismatch == 0u && res->why == 0u, "motion_tap",
               "every moving primitive was compared at t = 1");
    }
    /* Halfway: the quad's first vertex at clip x 0.25 + 0.1875, screen x 345; the sprite's
     * corners at 301.5 and 342, floored. */
    MotionBuiltVert out[3];
    expect(motion_build_prim(build, vb.prims[2].first_tri, 0.5f, out, NULL) == MOTION_ITEM_BUILT &&
           out[0].x == 345.0f && out[0].y == 170.0f && out[0].rw == 1.0f, "motion_tap",
           "the quad halfway between the frames");
    expect(motion_build_prim(build, vb.prims[3].first_tri, 0.5f, out, NULL) == MOTION_ITEM_BUILT &&
           out[0].x == 301.0f && out[0].y == 20.0f && out[1].x == 342.0f && out[1].y == 60.0f &&
           out[0].z == 1000.0f && out[0].rgba == 0xff10b0e0u, "motion_tap", "the sprite halfway between the frames");
    /* Frame B again, with the Bezier patch drawn as triangles, lines and points, then with an
     * immediate-mode triangle, line and point: ge.c draws them with no PRIM, so nothing places
     * them in the frame between, and the plan refuses it. The patch is one quad: two
     * triangles, three lines or one point. */
    static const struct { int flags; const char *what; uint32_t unnamed; } legs[6] = {
        { MS_PATCH, "a Bezier patch", 2u },
        { MS_IMMEDIATE, "an immediate-mode triangle", 1u },
        { MS_PATCH_LINES, "a Bezier patch drawn as lines", 3u },
        { MS_PATCH_POINTS, "a Bezier patch drawn as points", 1u },
        { MS_IMM_LINE, "an immediate-mode line", 1u },
        { MS_IMM_POINT, "an immediate-mode point", 1u },
    };
    for (int leg = 0; leg < 6; leg++) {
        const char *what = legs[leg].what;
        const uint32_t want = legs[leg].unnamed;
        MotionMatchFrame mu;
        MotionMatchResult mres;
        int32_t upair[8];
        motion_tap_frame_reset(f);
        ge_motion_tap_set(motion_tap_frame_listener(f));
        motion_run(legs[leg].flags, 1, on);
        ge_motion_tap_set(NULL);
        motion_tap_frame_close(f, fa);
        motion_tap_match_frame(f, &mu);
        MotionTapView vl;
        motion_tap_frame_view(f, &vl);
        if (vl.unnamed != want) fprintf(stderr, "motion_tap: %s: unnamed %u\n", what, (unsigned)vl.unnamed);
        expect(mu.nprims == 5u && vl.unnamed == want, "motion_tap",
               "the unnamed draw adds nothing to the matcher's list, and the store counts its raster calls");
        if (mu.nprims != 5u) continue;
        expect(motion_match(&ma, &mu, &params, NULL, upair, NULL, &mres) == 0 && mres.flags == 0u &&
               upair[0] == 0 && upair[4] == 4, "motion_tap", "the matcher pairs the five PRIMs");
        const int ok = motion_build_plan(build, fa, f, upair, &mres);
        if (ok != 0 || res->why != MOTION_WHY_UNKNOWN || res->unknown != want || res->built != 7u)
            fprintf(stderr, "motion_tap: %s: plan %d why %#x unknown %u built %u\n", what, ok, (unsigned)res->why,
                    (unsigned)res->unknown, (unsigned)res->built);
        expect(ok == 0 && res->why == MOTION_WHY_UNKNOWN && res->unknown == want && res->built == 7u,
               "motion_tap", "a frame B holding a patch or an immediate-mode primitive is refused as unknown");
    }
    printf("MOTION_TAP identical=3 legs x 2 timing models, built_at_t1=%u prims=%u refs=%u tris=%u bytes=%zu\n",
           checked, (unsigned)vb.nprims, (unsigned)vb.nrefs, (unsigned)vb.ntris, vb.bytes);

done:
    ge_motion_tap_set(NULL);
    ge_set_gpu_hooks(NULL);
    ge_timing_set_enabled(0);
    configure(NULL, NULL, NULL, NULL);
    motion_build_free(build);
    motion_tap_frame_free(f);
    motion_tap_frame_free(fa);
    motion_tap_frame_free(fb);
    free(off);
    free(on);
    free(again);
}

/* ---- --bench: disarmed flight recorder cost on the GE paths (#320) ---- */

#define LIST_BENCH       0x08120000u
#define BENCH_PRIMS      1024u
#define BENCH_RUNS_PER_ROUND 40u
#define BENCH_CALLS      20000000u
#define BENCH_FB_BYTES   (512u * 272u * 4u)

static uint64_t bench_now_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
#endif
}

static int bench_cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t bench_fb_hash(void) {
    uint64_t h = 1469598103934665603u;
    const uint8_t *p = SR_HOST(0x04044000u);
    for (uint32_t i = 0; i < BENCH_FB_BYTES; i++) h = (h ^ p[i]) * 1099511628211u;
    return h;
}

/* One list of BENCH_PRIMS through-mode triangles after the common setup. */
static void bench_build_list(void) {
    s_pc = LIST_BENCH;
    emit_setup();
    uint32_t tri = vtx(10.0f, 40.0f, 0.0f);          /* zero area: no pixels, so the */
    vtx(10.0f, 40.0f, 0.0f);                         /* per-primitive bookkeeping dominates */
    vtx(10.0f, 40.0f, 0.0f);
    W(0x12, VT_THROUGH);
    for (uint32_t i = 0; i < BENCH_PRIMS; i++) prim(3, 3, tri);
    W(0x0F, 0);                                          /* FINISH */
    W(0x0C, 0);                                          /* END */
}

static void bench_workload(const char *name, uint32_t list, uint32_t prims, uint32_t rounds) {
    uint64_t *samples = (uint64_t *)malloc(rounds * sizeof *samples);
    if (!samples) exit(2);
    configure(NULL, NULL, NULL, NULL);                   /* every profile off */
    memset(SR_HOST(0x04044000u), 0, BENCH_FB_BYTES);
    for (uint32_t i = 0; i < 4u; i++) run_list(list, name);  /* warm up */
    for (uint32_t r = 0; r < rounds; r++) {
        uint64_t t0 = bench_now_ns();
        for (uint32_t i = 0; i < BENCH_RUNS_PER_ROUND; i++) run_list(list, name);
        samples[r] = (bench_now_ns() - t0) / BENCH_RUNS_PER_ROUND;
    }
    uint64_t hash = bench_fb_hash();
    qsort(samples, rounds, sizeof *samples, bench_cmp);
    printf("GE_FLIGHT_BENCH flavour=%s workload=%s prims=%" PRIu32 " rounds=%" PRIu32
           " ns_per_list_median=%" PRIu64 " ns_per_list_min=%" PRIu64 " fb_hash=%016" PRIx64 "\n",
#ifdef SR_FLIGHT_RECORDER_LINKED
           "linked",
#else
           "absent",
#endif
           name, prims, rounds, samples[rounds / 2u], samples[0], hash);
    free(samples);
}

static int bench_main(uint32_t rounds) {
    uint8_t *arena = (uint8_t *)calloc(0x0c000000u, 1);
    if (!arena) return 2;
    g_mem = arena + 0x08000000u;
    build_lists();
    bench_build_list();
#ifdef SR_FLIGHT_RECORDER_LINKED
    /* The recorder is disarmed (SR_FLIGHT unset): the GE class reads as off, and the
     * cross-object class test, the call each list and present site makes, is timed alone. */
    if (sr_flight_class_enabled(SR_FLIGHT_CLASS_GE)) {
        fprintf(stderr, "ge_cpu_profile_validate_selftest: --bench needs SR_FLIGHT unset\n");
        return 2;
    }
    volatile uint32_t sink = 0;
    uint64_t t0 = bench_now_ns();
    for (uint32_t i = 0; i < BENCH_CALLS; i++) sink += (uint32_t)sr_flight_class_enabled(SR_FLIGHT_CLASS_GE);
    uint64_t dt = bench_now_ns() - t0;
    printf("GE_FLIGHT_BENCH flavour=linked class_check calls=%u ns_per_call_x1000=%" PRIu64
           " sink=%u\n", BENCH_CALLS, dt * 1000u / BENCH_CALLS, (unsigned)sink);
#endif
    bench_workload("scene", LIST_MAIN, 0, rounds);
    bench_workload("prims", LIST_BENCH, BENCH_PRIMS, rounds);
    free(arena);
    return s_failures ? 1 : 0;
}

int main(int argc, char **argv) {
    int strip_cache = 1;
    if (argc == 3 && strcmp(argv[1], "--bench") == 0) {
        long rounds = strtol(argv[2], NULL, 10);
        if (rounds < 1 || rounds > 100000) {
            fprintf(stderr, "usage: %s --bench ROUNDS (1 to 100000)\n", argv[0]);
            return 2;
        }
        return bench_main((uint32_t)rounds);
    }
    if (argc == 2 && strcmp(argv[1], "--strip-cache-disabled") == 0) strip_cache = 0;
    else if (argc != 1) {
        fprintf(stderr, "usage: %s [--strip-cache-disabled]\n", argv[0]);
        return 2;
    }
    env_set("SR_GE_STRIP_CACHE_DISABLE", strip_cache ? NULL : "1");

    uint8_t *arena = (uint8_t *)calloc(0x0c000000u, 1);
    if (!arena) {
        fprintf(stderr, "ge_cpu_profile_validate_selftest: no guest arena\n");
        return 2;
    }
    g_mem = arena + 0x08000000u;
    test_vram_mirror_cpu_stores();
    test_ge_zbuf_span_bounds_cache();
    test_ge_texture_mip_levels();
    test_ge_mip_use_trace_routes();
    test_ge_immediate_mode();
    test_ge_immediate_stream();
    test_ge_wide_fill();
    test_ge_wide_raster_timing();
    test_ge_raster_stride_bounds();
    build_lists();
    expect_api_contract();

    /* Both profiles, every sample, calibration: the fault-injection base. */
    configure("1", "1", "1", "1");
    run_scene("full");
    run_list(LIST_BAD_CLUT, "full");
    GeCpuProfileStats full = snapshot();
    expect_consistent("full", &full);
    expect_full_coverage("full", &full, strip_cache, 0);
    expect(full.phase[GE_CPU_CLUT_LOAD].calls > full.clut_loads, "full",
           "an unloaded CLUT span still ends its phase");

    /* Profiles latch until a reset: a changed stride does not apply mid-session, and a
     * second pass accumulates onto the first. */
    env_set("SR_GE_PRIM_PROFILE_STRIDE", "5");
    run_scene("accumulated");
    GeCpuProfileStats accumulated = snapshot();
    expect_consistent("accumulated", &accumulated);
    expect(accumulated.primitive_profile_stride == 1u, "accumulated", "stride latched until reset");
    expect(accumulated.primitive_profile_triangle_candidates ==
               2u * full.primitive_profile_triangle_candidates &&
           accumulated.primitive_commands == 2u * full.primitive_commands,
           "accumulated", "second pass accumulates");

    /* A reset clears every counter and sampling ordinal: a fresh identical run reproduces
     * the first snapshot byte for byte. */
    configure("1", "1", "1", "1");
    GeCpuProfileStats cleared = snapshot();
    expect_consistent("reset", &cleared);
    expect(coarse_counters_zero(&cleared) && sparse_counters_zero(&cleared), "reset",
           "reset clears every counter");
    run_scene("reset");
    run_list(LIST_BAD_CLUT, "reset");
    GeCpuProfileStats rerun = snapshot();
    expect(same_profile(&rerun, &full), "reset", "rerun after reset differs");

    /* The GPU backend seam: hooks take every primitive and flush at list end. */
    GeGpuHooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.tri = hook_tri; hooks.sprite = hook_sprite; hooks.line = hook_line;
    hooks.point = hook_point; hooks.flush = hook_flush; hooks.vram_dirty = hook_vram_dirty;
    ge_set_gpu_hooks(&hooks);
    configure("1", "1", "1", "1");
    run_scene("hooked");
    ge_set_gpu_hooks(NULL);
    GeCpuProfileStats hooked = snapshot();
    expect_consistent("hooked", &hooked);
    expect_full_coverage("hooked", &hooked, strip_cache, 1);
    expect(s_hook_prims > 0 && s_hook_flushes > 0 &&
           hooked.phase[GE_CPU_FLUSH].calls == hooked.flushes, "hooked", "hook traffic");

    /* Sparse defaults: stride 512, no calibration. */
    configure("1", "1", NULL, NULL);
    run_scene("sparse_default");
    GeCpuProfileStats sparse_default = snapshot();
    expect_consistent("sparse_default", &sparse_default);
    expect(sparse_default.primitive_profile_stride == 512u &&
           !sparse_default.primitive_profile_calibration_enabled &&
           sparse_default.primitive_profile_sampled_total.calls == 0 &&
           sparse_default.primitive_profile_empty_control.calls == 0 &&
           sparse_default.primitive_profile_phase[GE_PRIM_PROFILE_VERTEX_FETCH_DECODE].calls <
               sparse_default.primitive_profile_eligible[GE_PRIM_PROFILE_VERTEX_FETCH_DECODE],
           "sparse_default", "sparse sampling below eligibility");

    /* One profile at a time: the other's counters stay zero. */
    configure("1", NULL, NULL, NULL);
    run_scene("coarse_only");
    GeCpuProfileStats coarse_only = snapshot();
    expect_consistent("coarse_only", &coarse_only);
    expect(coarse_only.enabled && !coarse_only.primitive_profile_enabled &&
           coarse_only.primitive_commands > 0 && sparse_counters_zero(&coarse_only),
           "coarse_only", "only the coarse profile counts");
    configure(NULL, "1", "1", NULL);
    run_scene("sparse_only");
    GeCpuProfileStats sparse_only = snapshot();
    expect_consistent("sparse_only", &sparse_only);
    expect(!sparse_only.enabled && sparse_only.primitive_profile_enabled &&
           sparse_only.primitive_profile_triangle_candidates > 0 &&
           coarse_counters_zero(&sparse_only), "sparse_only", "only the sparse profile counts");
    configure(NULL, NULL, NULL, NULL);
    env_set("SR_GPU_CPU_PROFILE", "1");
    ge_cpu_profile_reset();
    run_scene("gpu_cpu_switch");
    GeCpuProfileStats gpu_switch = snapshot();
    expect_consistent("gpu_cpu_switch", &gpu_switch);
    expect(gpu_switch.enabled && gpu_switch.primitive_commands == coarse_only.primitive_commands,
           "gpu_cpu_switch", "SR_GPU_CPU_PROFILE enables the coarse profile");

    /* Profiling off: nothing is counted at all. */
    configure(NULL, NULL, NULL, NULL);
    run_scene("disabled");
    GeCpuProfileStats disabled = snapshot();
    expect_consistent("disabled", &disabled);
    expect(!disabled.enabled && !disabled.primitive_profile_enabled &&
           coarse_counters_zero(&disabled) && sparse_counters_zero(&disabled),
           "disabled", "a disabled profile counts nothing");

    /* A list with no draws: zero-sample populations. */
    configure("1", "1", "1", "1");
    run_list(LIST_EMPTY, "empty_list");
    GeCpuProfileStats empty = snapshot();
    expect_consistent("empty_list", &empty);
    expect(empty.phase[GE_CPU_LIST_TOTAL].calls == 1u && empty.commands == 1u &&
           empty.primitive_commands == 0u && sparse_counters_zero(&empty),
           "empty_list", "one END command and no primitives");

    expect_faults("full", &full);
    expect_faults("hooked", &hooked);
    static const Fault coarse_fault = {
        "type_slot_overcount", m_type_slot_overcount, BIT(PRIMITIVE_TYPES) };
    expect_fault("coarse_only", &coarse_only, &coarse_fault);
    static const Fault sparse_fault = {
        "through_candidate_overcount", m_through_candidate_overcount, BIT(TRIANGLE_MODE_SPLIT) };
    expect_fault("sparse_only", &sparse_only, &sparse_fault);

    test_ge_vram_cpu_visibility();
    test_ge_fb_direct_access();
    test_ge_vram_depth_bit13_absolute_xor();
    test_ge_bounding_box_branch();
    test_ge_vram_view_decode();
    test_ge_vram_view_sources();
    test_ge_motion_tap();

    printf("ge_cpu_profile_validate_selftest: strip_cache=%s scenarios=%u faults=%u failures=%d\n",
           strip_cache ? "on" : "off", s_scenarios, s_faults, s_failures);
    free(arena);
    return s_failures ? 1 : 0;
}
