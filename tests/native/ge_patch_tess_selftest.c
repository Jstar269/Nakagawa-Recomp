// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// ge-patch-tess-selftest: finer Bezier and spline patches (#2053, ENHANCEMENT_CONTRACT.md 10.14)
// on the production GE object's software renderer. Source-owned display lists run through
// ge_run_list(); the harness supplies guest memory and the runtime's diagnostic globals at their
// disabled values. No game input.
//
//   settings    the tess key's factor: 0 (off, and an authentic run) and 1 keep the PSP's
//               division; 2, 3 and 4 are taken; anything else keeps the PSP's division.
//   same-grid   factor F on a guest division d draws, byte for byte, what this renderer draws
//               for the guest division d*F (Bezier and spline, varied vertex colours), and not
//               what it draws for d: the finer surface is the PSP's own surface, finer.
//   quads       each factor draws F*F times the PSP's quads.
//   off         factor 1 is the PSP's path: the same bytes as a run that never set it.
//   seam        two neighbouring Bezier patches (two draws sharing a curved edge) and one
//               two-patch spline leave no background pixel between their outer edges, at
//               every factor 1 to 4. The control draws the two halves at factors 4 and 1,
//               the case a per-draw cap could make, and requires the check to find the crack.
//   wire        line and point patches keep the PSP's division at every factor.
//   cap         a draw whose finer grid would pass 262144 vertices drops along the divisor
//               chain (4, 2, 1; 3, 1) to the first factor that fits, and is counted.
//   psp-cap     a draw the PSP's own 65535-vertex cap lowers is lowered first and multiplied
//               after: four patches at 127x127 draw 126x126 times F, not 127x127 times F.
//   timing      with the GE timing model on, factor F on a division d costs exactly the GE clocks
//               of the division d*F, which differ from the PSP's d: the dome costs less, the ribbon and
//               the small patch more. On 8888 without depth test, each added triangle adds a fixed
//               row charge on spans under 48 pixels; elsewhere the charge is per pixel.
//   exit        the run's end prints PATCH_TESS: exit with the quads drawn and the capped draws.

#include "recomp.h"
#include "debug.h"
#include "ge_capture.h"
#include "ge_shared.h"

#ifndef SR_SELFTEST_BUILD_ROOT
#define SR_SELFTEST_BUILD_ROOT "build"
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
    fprintf(stderr, "ge_patch_tess_selftest: unexpected runtime call %s addr=0x%08" PRIx32 "\n", what, addr);
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

static uint64_t s_clock_ns = 1000000u;
uint64_t SDL_GetTicksNS(void) {
    s_clock_ns += 7u;
    return s_clock_ns;
}

/* ---- Source-owned display lists ---- */

#define LIST  0x08100000u
#define VERTS 0x08200000u
#define FB    0x04044000u   /* FRAMEBUFPTR 0x044000, 512 pixels a row, 8888 */
#define FB_W  480
#define FB_H  272

/* Through mode, colour 8888, float normal, float position: 28 bytes per vertex. */
#define VT_THROUGH ((7u << 2) | (3u << 5) | (3u << 7) | (1u << 23))
#define VSTRIDE    28u

static int s_fail = 0;
static uint32_t s_pc;
static uint32_t s_vtx;

static void W(uint32_t cmd, uint32_t data) {
    uint32_t word = (cmd << 24) | (data & 0xFFFFFFu);
    memcpy(SR_HOST(s_pc), &word, 4);
    s_pc += 4;
}

static void vtx(float x, float y, uint32_t color) {
    float normal[3] = { 0.0f, 0.0f, 1.0f };
    float pos[3] = { x, y, 0.0f };
    memcpy(SR_HOST(s_vtx), &color, 4);
    memcpy(SR_HOST(s_vtx + 4u), normal, sizeof normal);
    memcpy(SR_HOST(s_vtx + 16u), pos, sizeof pos);
    s_vtx += VSTRIDE;
}

static void emit_setup(void) {
    W(0x10, 0x080000);              /* BASE: upper address bits 0x08000000 */
    W(0x13, 0);                     /* OFFSETADDR */
    W(0x9C, 0x044000);              /* FRAMEBUFPTR -> 0x04044000 */
    W(0x9D, 512);                   /* FRAMEBUFWIDTH */
    W(0xD2, 3);                     /* PIXFORMAT 8888 */
    W(0xD4, 0);                     /* SCISSOR1 */
    W(0xD5, (271u << 10) | 479u);   /* SCISSOR2 */
    W(0x1E, 0);                     /* TEXTUREMAPENABLE */
    W(0x23, 0);                     /* ZTESTENABLE */
    W(0x21, 0);                     /* ALPHABLENDENABLE */
    W(0x17, 0);                     /* LIGHTINGENABLE */
    W(0x50, 1);                     /* SHADEMODE gouraud */
    W(0x4C, 0); W(0x4D, 0);         /* OFFSETX/Y */
    W(0x12, VT_THROUGH);
}

typedef struct {
    uint32_t vaddr;
    uint32_t op;       /* the BEZIER or SPLINE command word's low 24 bits */
    int spline;
} Patch;

/* One list: setup, then each patch with PATCHDIVISION (du, dv) and PATCHPRIMITIVE prim. */
static void run_patches_on(const Patch *p, int n, int du, int dv, int prim, int clear) {
    if (clear) memset(SR_HOST(FB), 0, 512u * FB_H * 4u);
    s_pc = LIST;
    emit_setup();
    W(0x36, ((uint32_t)dv << 8) | (uint32_t)du);
    W(0x37, (uint32_t)prim);
    for (int i = 0; i < n; i++) {
        W(0x01, p[i].vaddr);
        W(p[i].spline ? 0x06 : 0x05, p[i].op);
    }
    W(0x0F, 0);                     /* FINISH */
    W(0x0C, 0);                     /* END */
    ge_run_list(LIST, 0);
}

static void run_patches(const Patch *p, int n, int du, int dv, int prim) {
    run_patches_on(p, n, du, dv, prim, 1);
}

static uint64_t fb_hash(void) {
    uint64_t h = 1469598103934665603ull;
    for (int y = 0; y < FB_H; y++) {
        const uint8_t *row = SR_HOST(FB + (uint32_t)y * 512u * 4u);
        for (int i = 0; i < FB_W * 4; i++) { h ^= row[i]; h *= 1099511628211ull; }
    }
    return h;
}

static uint32_t fb_px(int x, int y) {
    uint32_t w;
    memcpy(&w, SR_HOST(FB + ((uint32_t)y * 512u + (uint32_t)x) * 4u), 4);
    return w;
}

static void check(int ok, const char *cell, const char *what) {
    if (ok) {
        printf("ge patch tess selftest: PASS %-10s %s\n", cell, what);
    } else {
        fprintf(stderr, "ge patch tess selftest: FAIL %-10s %s\n", cell, what);
        s_fail++;
    }
}

/* ---- Fixtures ---- */

/* A curved 4x4 Bezier patch (one patch) with a colour per control point. */
static uint32_t make_dome(void) {
    const uint32_t at = s_vtx;
    static const float bulge[4] = { 0.0f, 60.0f, -40.0f, 10.0f };
    for (int v = 0; v < 4; v++)
        for (int u = 0; u < 4; u++) {
            const float x = 140.0f + 70.0f * (float)u + (u == 1 || u == 2 ? bulge[v] : 0.0f);
            const float y = 40.0f + 60.0f * (float)v + (v == 1 || v == 2 ? bulge[u] * 0.5f : 0.0f);
            const uint32_t c = 0xFF000000u | (uint32_t)(40 + 60 * u) << 16 | (uint32_t)(30 + 70 * v) << 8 |
                               (uint32_t)(200 - 40 * (u + v) / 2);
            vtx(x, y, c);
        }
    return at;
}

/* A 7x4 spline control grid: four patches in U, one in V, with a colour per point. */
static uint32_t make_ribbon(void) {
    const uint32_t at = s_vtx;
    for (int v = 0; v < 4; v++)
        for (int u = 0; u < 7; u++) {
            const float x = 60.0f + 60.0f * (float)u;
            const float y = 50.0f + 55.0f * (float)v + ((u & 1) ? 35.0f : -25.0f);
            vtx(x, y, 0xFF000000u | (uint32_t)(30 * u) << 16 | (uint32_t)(60 * v) << 8 | 0x80u);
        }
    return at;
}

/* Two Bezier patches that share a curved edge column: left over x 100..edge, right over
 * edge..300, rows y 60..210. One solid colour, so a gap shows as background. */
static void make_seam(uint32_t *left, uint32_t *right) {
    static const float edge[4] = { 200.0f, 245.0f, 155.0f, 200.0f };
    static const float lx[3] = { 100.0f, 115.0f, 130.0f }, rx[3] = { 265.0f, 285.0f, 300.0f };
    *left = s_vtx;
    for (int v = 0; v < 4; v++) {
        for (int u = 0; u < 3; u++) vtx(lx[u], 60.0f + 50.0f * (float)v, 0xFF40C080u);
        vtx(edge[v], 60.0f + 50.0f * (float)v, 0xFF40C080u);
    }
    *right = s_vtx;
    for (int v = 0; v < 4; v++) {
        vtx(edge[v], 60.0f + 50.0f * (float)v, 0xFF40C080u);
        for (int u = 0; u < 3; u++) vtx(rx[u], 60.0f + 50.0f * (float)v, 0xFF40C080u);
    }
}

/* Background pixels inside the rows and columns the seam fixture covers. */
static int seam_holes(int x0, int x1, int y0, int y1) {
    int holes = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (fb_px(x, y) == 0u) holes++;
    return holes;
}

int main(void) {
    uint8_t *arena = (uint8_t *)calloc(0x0c000000u, 1);
    if (!arena) {
        fprintf(stderr, "ge_patch_tess_selftest: no guest arena\n");
        return 2;
    }
    g_mem = arena + 0x08000000u;
    s_vtx = VERTS;
    char what[512];

    /* settings: the factor the run's launch line carries (the tess key, ace.c), which the driver
     * passes from the effective Ace config: 0 (off, and an authentic run) and 1 keep the PSP's
     * division, 2 to 4 are taken, and a value the key never carries keeps the PSP's. */
    {
        static const struct { int factor, want; } cells[] = {
            { 0, 1 }, { 1, 1 }, { 2, 2 }, { 3, 3 }, { 4, 4 }, { 5, 1 }, { 8, 1 }, { 22, 1 }, { -2, 1 },
        };
        int ok = 1;
        for (size_t i = 0; i < sizeof cells / sizeof cells[0]; i++) {
            const int got = ge_patch_tess_init(cells[i].factor);
            if (got != cells[i].want) {
                fprintf(stderr, "ge patch tess selftest: tess=%d gave %d, want %d\n", cells[i].factor, got,
                        cells[i].want);
                ok = 0;
            }
        }
        (void)ge_patch_tess_init(0);
        check(ok, "settings", "0 and 1 keep 1; 2, 3 and 4 are taken; 5, 8, 22 and -2 keep 1");
    }

    const uint32_t dome = make_dome(), ribbon = make_ribbon();
    uint32_t seam_l, seam_r;
    make_seam(&seam_l, &seam_r);
    const Patch dome_p = { dome, 0x0404u, 0 };
    const Patch ribbon_p = { ribbon, 0x0407u | (3u << 16) | (3u << 18), 1 };   /* nu 7, nv 4, open knots both ways */
    const Patch seam_p[2] = { { seam_l, 0x0404u, 0 }, { seam_r, 0x0404u, 0 } };

    /* off: factor 1 is a run that never set it. */
    {
        run_patches(&dome_p, 1, 2, 1, 0);
        const uint64_t base = fb_hash();
        unsigned long long q0, q1;
        ge_patch_tess_counts(&q0, NULL);
        ge_patch_tess_set(1);
        run_patches(&dome_p, 1, 2, 1, 0);
        ge_patch_tess_counts(&q1, NULL);
        snprintf(what, sizeof what, "factor 1 draws the default run's bytes (%016" PRIx64 "), %llu quads", base, q1 - q0);
        check(fb_hash() == base && q1 - q0 == 2u, "off", what);
    }

    /* same-grid and quads */
    for (int s = 0; s < 2; s++) {
        const Patch *p = s ? &ribbon_p : &dome_p;
        const int du = 2, dv = 1;
        const int pu = s ? 4 : 1;
        for (int f = 2; f <= 4; f++) {
            ge_patch_tess_set(1);
            run_patches(p, 1, du * f, dv * f, 0);
            const uint64_t psp_fine = fb_hash();
            run_patches(p, 1, du, dv, 0);
            const uint64_t psp_coarse = fb_hash();
            unsigned long long q0, q1;
            ge_patch_tess_counts(&q0, NULL);
            ge_patch_tess_set(f);
            run_patches(p, 1, du, dv, 0);
            ge_patch_tess_counts(&q1, NULL);
            const uint64_t ours = fb_hash();
            const unsigned long long want_q = (unsigned long long)pu * (unsigned long long)(du * f) * (unsigned long long)(dv * f);
            snprintf(what, sizeof what, "%s factor %d on division %dx%d = the PSP's %dx%d (%016" PRIx64 "), not %dx%d",
                     s ? "spline" : "bezier", f, du, dv, du * f, dv * f, ours, du, dv);
            check(ours == psp_fine && ours != psp_coarse, "same-grid", what);
            snprintf(what, sizeof what, "%s factor %d: %llu quads (the PSP's %d x %d)", s ? "spline" : "bezier", f,
                     q1 - q0, pu * du * dv, f * f);
            check(q1 - q0 == want_q, "quads", what);
        }
    }

    /* seam: no background between the outer edges, at every factor. */
    for (int f = 1; f <= 4; f++) {
        ge_patch_tess_set(f);
        run_patches(seam_p, 2, 1, 3, 0);
        const int bez = seam_holes(102, 297, 62, 208);
        ge_patch_tess_set(f);
        run_patches(&ribbon_p, 1, 2, 2, 0);
        /* The ribbon's spline surface is not a rectangle: check the band every patch covers. */
        int sp = 0;
        for (int x = 70; x <= 410; x++)
            for (int y = 130; y <= 150; y++)
                if (fb_px(x, y) == 0u) sp++;
        snprintf(what, sizeof what, "factor %d: %d background pixels between two Bezier patches, %d across a "
                 "four-patch spline", f, bez, sp);
        check(bez == 0 && sp == 0, "seam", what);
    }

    /* seam control: the left half at factor 4 and the right at 1 leave a crack the check sees,
     * so a zero above is a watertight seam, not a check that cannot fail. */
    {
        ge_patch_tess_set(4);
        run_patches_on(&seam_p[0], 1, 1, 3, 0, 1);
        ge_patch_tess_set(1);
        run_patches_on(&seam_p[1], 1, 1, 3, 0, 0);
        const int holes = seam_holes(102, 297, 62, 208);
        snprintf(what, sizeof what, "factors 4 and 1 across the shared edge leave %d background pixels", holes);
        check(holes > 0, "seam", what);
    }

    /* wire: line and point patches keep the PSP's division. */
    for (int prim = 1; prim <= 2; prim++) {
        ge_patch_tess_set(1);
        run_patches(&dome_p, 1, 3, 2, prim);
        const uint64_t psp = fb_hash();
        ge_patch_tess_set(4);
        run_patches(&dome_p, 1, 3, 2, prim);
        snprintf(what, sizeof what, "%s patch at factor 4 draws the PSP's division (%016" PRIx64 ")",
                 prim == 1 ? "line" : "point", psp);
        check(fb_hash() == psp, "wire", what);
    }

    /* cap: two Bezier patches at division 127x127. Factor 4 would be 509 * 509 * 2 vertices;
     * the chain's next, 2, is 255 * 255 * 2 = 130050, which fits. Factor 3 (382 * 382 * 2) drops
     * to 1, the PSP's division. */
    {
        const uint32_t wide = s_vtx;
        for (int v = 0; v < 4; v++)
            for (int u = 0; u < 7; u++) vtx(60.0f + 50.0f * (float)u, 40.0f + 60.0f * (float)v, 0xFFFFFFFFu);
        const Patch wide_p = { wide, 0x0407u, 0 };
        unsigned long long q0, q1, c0, c1;
        ge_patch_tess_set(1);
        run_patches(&wide_p, 1, 127, 127, 0);
        ge_patch_tess_counts(&q0, &c0);
        ge_patch_tess_set(4);
        run_patches(&wide_p, 1, 127, 127, 0);
        ge_patch_tess_counts(&q1, &c1);
        snprintf(what, sizeof what, "factor 4 at division 127x127, two patches: %llu quads (factor 2), %llu capped",
                 q1 - q0, c1 - c0);
        check(q1 - q0 == 2ull * 254ull * 254ull && c1 - c0 == 1u, "cap", what);
        ge_patch_tess_set(2);
        run_patches(&wide_p, 1, 127, 127, 0);
        unsigned long long c2;
        ge_patch_tess_counts(NULL, &c2);
        check(c2 == c1, "cap", "factor 2 at the same draw fits and is not counted");
        unsigned long long q3, q4, c3;
        ge_patch_tess_counts(&q3, NULL);
        ge_patch_tess_set(3);
        run_patches(&wide_p, 1, 127, 127, 0);
        ge_patch_tess_counts(&q4, &c3);
        snprintf(what, sizeof what, "factor 3 at the same draw: %llu quads (the PSP's), %llu capped", q4 - q3, c3 - c2);
        check(q4 - q3 == 2ull * 127ull * 127ull && c3 - c2 == 1u, "cap", what);
    }

    /* psp-cap: four Bezier patches (7x7 control points) at 127x127 are 4 * 128 * 128 = 65536
     * vertices, past the PSP's 65535, so the PSP draws 126x126. Factor 2 multiplies that:
     * 4 * 253 * 253 = 256036 vertices fit the host cap, 4 * 252 * 252 quads. Multiplying before
     * the PSP's cap would draw 4 * 254 * 254. */
    {
        const uint32_t quad4 = s_vtx;
        for (int v = 0; v < 7; v++)
            for (int u = 0; u < 7; u++) vtx(60.0f + 50.0f * (float)u, 20.0f + 38.0f * (float)v, 0xFFFFFFFFu);
        const Patch quad4_p = { quad4, 0x0707u, 0 };
        unsigned long long q0, q1, c0, c1;
        ge_patch_tess_set(1);
        run_patches(&quad4_p, 1, 127, 127, 0);
        ge_patch_tess_counts(&q0, &c0);
        ge_patch_tess_set(2);
        run_patches(&quad4_p, 1, 127, 127, 0);
        ge_patch_tess_counts(&q1, &c1);
        snprintf(what, sizeof what, "four patches at 127x127, factor 2: %llu quads (the PSP's 126x126, times 2), "
                 "%llu capped", q1 - q0, c1 - c0);
        check(q1 - q0 == 4ull * 252ull * 252ull && c1 == c0, "psp-cap", what);
    }

    /* timing: with the GE timing model on, factor F on division d costs exactly the GE clocks
     * of the division d*F, and that is not the PSP's d: the model charges each triangle's rows
     * and spans. The fixture is 8888, untextured, without depth test, the case that charges a
     * fixed amount per row on spans under 48 pixels (ge_time_row_clk); elsewhere a span costs
     * per pixel. A finer grid costs less for the dome, whose wide spans it cuts below 48 pixels,
     * and more for the ribbon (21614 to 21936 clocks at factor 2) and for a 30-pixel patch (one
     * more fixed row charge per triangle). The contract states the change. */
    {
        const uint32_t tiny_at = s_vtx;
        for (int v = 0; v < 4; v++)
            for (int u = 0; u < 4; u++) vtx(200.0f + 10.0f * (float)u, 100.0f + 10.0f * (float)v, 0xFFFFFFFFu);
        const Patch tiny_p = { tiny_at, 0x0404u, 0 };
        const struct { const Patch *p; const char *name; int direction; /* 1: finer costs more, -1: costs less */ } shapes[] = {
            { &dome_p, "bezier", -1 }, { &ribbon_p, "spline", 1 }, { &tiny_p, "small bezier", 1 },
        };
        ge_timing_set_enabled(1);
        int ok = 1;
        char detail[160] = "";
        for (size_t s = 0; s < sizeof shapes / sizeof shapes[0] && ok; s++) {
            ge_patch_tess_set(1);
            run_patches(shapes[s].p, 1, 2, 1, 0);
            const double psp = ge_timing_run_clk();
            for (int f = 2; f <= 4; f++) {
                ge_patch_tess_set(1);
                run_patches(shapes[s].p, 1, 2 * f, f, 0);
                const double fine = ge_timing_run_clk();
                ge_patch_tess_set(f);
                run_patches(shapes[s].p, 1, 2, 1, 0);
                const double got = ge_timing_run_clk();
                const int direction_ok = shapes[s].direction > 0 ? got > psp : got < psp;
                if (got != fine || !direction_ok) {
                    snprintf(detail, sizeof detail, "%s factor %d: %.3f clocks, division %dx%d %.3f, the PSP's %.3f",
                             shapes[s].name, f, got, 2 * f, f, fine, psp);
                    ok = 0;
                    break;
                }
            }
        }
        snprintf(what, sizeof what, "factors 2 to 4 on 2x1 cost the clocks of the division 2F x F, not the PSP's "
                 "2x1 (the dome less, the ribbon and the small patch more)%s%s", ok ? "" : ": ", detail);
        check(ok, "timing", what);
        ge_timing_set_enabled(0);
        ge_patch_tess_set(1);
    }

    /* exit: the end-of-run report. The settings cell turned the feature on, which registered it;
     * sr_perf_shutdown runs the process-end reports, here into a file that is read back. */
    {
        unsigned long long q, c;
        ge_patch_tess_counts(&q, &c);
        char want[96], line[256];
        snprintf(want, sizeof want, "PATCH_TESS: exit (quads=%llu, capped_draws=%llu)\n", q, c);
        const char *path = SR_SELFTEST_BUILD_ROOT "/ge_patch_tess_selftest_exit.txt";
        int found = 0;
        fflush(stderr);
        if (freopen(path, "w", stderr)) {
            sr_perf_shutdown();
            fflush(stderr);
            FILE *in = fopen(path, "r");
            if (in) {
                while (fgets(line, sizeof line, in))
                    if (strcmp(line, want) == 0) found = 1;
                fclose(in);
            }
        }
        /* stderr is the file from here on, so the result goes to stdout. */
        want[strlen(want) - 1] = '\0';
        if (found && c >= 2u) {
            printf("ge patch tess selftest: PASS %-10s the run's end printed \"%s\"\n", "exit", want);
        } else {
            printf("ge patch tess selftest: FAIL %-10s no \"%s\" line at the run's end, or fewer than 2 "
                   "capped draws\n", "exit", want);
            s_fail++;
        }
    }

    ge_patch_tess_set(1);
    free(arena);
    if (s_fail) {
        printf("ge patch tess selftest: %d failure(s)\n", s_fail);
        return 1;
    }
    puts("ge patch tess selftest: OK");
    return 0;
}
