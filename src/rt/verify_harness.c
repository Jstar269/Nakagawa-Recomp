// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * Verify-build harness (issue #1843): runs calls into one module's generated code with the
 * sampled in-play checker (src/rt/verify.c) armed, the way the player would, and leaves the
 * verdicts to the report (SR_VERIFY_REPORT) and the captures (SR_VERIFY_CAPTURE).
 *
 *   verify_harness [--call ADDR:COUNT[:A0[:A1[:A2]]]]... [--sched-slice N] [--frozen-clock]
 *                  [--hard-exit] [--selftest-corrupt FIELD] <image.bin> <base-hex>
 *
 * Each --call enters the translated function at ADDR (hex) COUNT times (decimal) through
 * production dispatch as a host entry, with the arguments in hex (default: the data block
 * below, 0, 0) and $sp at the top of a stack region. --sched-slice N turns the scheduler
 * flag on with a slice of N yield points; each time the slice expires guest code reaches sr_yield,
 * which here only re-arms the slice, so a call that yields is reported SKIP preempted.
 * --frozen-clock fixes the verifier's clock at zero, to test the rate budget.
 * --hard-exit ends the run with _Exit, which skips atexit, after the report call the
 * runtime's own _Exit paths make (SR_VERIFY_REPORT_NOW).
 * --selftest-corrupt FIELD changes that compared field of the translated side at each
 * syscall or import stop (sr_end_state_selftest_corrupt in src/rt/end_state_compare.h), so
 * the checker must report it; src/rt/snapshot_runner.c takes the same option, and the gate
 * requires the same first difference from both.
 *
 * The harness links the generated module, recomp.c and the dispatch and interpreter units
 * but no scheduler, HLE, GE or host platform. src/rt/harness_stubs.c stands in for those,
 * as it does for src/rt/snapshot_runner.c: none of its definitions can change a compared
 * result silently. The two calls that would leave guest code, sr_syscall and
 * sr_raw_syscall, are defined below: they report the stop to the verifier and return.
 */

#include "recomp.h"
#include "end_state_compare.h"
#include "harness_stubs.h"
#include "nested_frames.h"
#include "verify.h"

#ifdef SR_VERIFY   /* a verify build only; the strict compile of the default build sees nothing */

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sr_register_all(void);   /* generated: executable spans and every native body */

static unsigned s_yields;
static unsigned long s_slice;

/* A scheduler slice expired inside guest code. The real sr_yield may switch guest threads;
 * here it only counts and re-arms the slice. The verifier was told by SR_YIELD. */
void sr_yield(CpuState *s) {
    (void)s;
    s_yields++;
    atomic_store(&sr_timeslice, (int_least32_t)s_slice);
}

#define ARENA_SIZE 0x0c000000u   /* the runtime arena: guest physical [0, 0x0c000000) */

static const char *s_selftest_corrupt;

/* Self-test only: change one compared field of the translated side at this stop. */
static void selftest_corrupt_at_stop(CpuState *s, SrEndStop *stop) {
    if (!s_selftest_corrupt) return;
    if (!sr_end_state_selftest_corrupt(s_selftest_corrupt, s, stop, g_mem - SR_RAM_BASE, ARENA_SIZE)) {
        fprintf(stderr, "verify_harness: --selftest-corrupt does not know %s\n", s_selftest_corrupt);
        exit(2);
    }
}

uint32_t sr_syscall(CpuState *s, uint32_t nid) {
    SrEndStop stop = { SR_END_STOP_IMPORT, 0u, 0u, nid };
    selftest_corrupt_at_stop(s, &stop);
    sr_verify_at_stop(s, SR_VERIFY_STOP_IMPORT, stop.import_nid, 0u);
    return 0u;
}

void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc) {
    SrEndStop stop = { SR_END_STOP_SYSCALL, pc, code, 0u };
    selftest_corrupt_at_stop(s, &stop);
    sr_verify_at_stop(s, SR_VERIFY_STOP_SYSCALL, stop.syscall_code, stop.syscall_pc);
}

static uint64_t frozen_clock(void) { return 0u; }

/* With the scheduler flag on, dispatch measures the host stack. The harness runs on the
 * process's own stack, which is far larger than any call it makes. */
static int main_stack_remaining(size_t *remaining) {
    *remaining = (size_t)1 << 20;
    return 1;
}

#define DATA_ADDR (SR_RAM_BASE + 0x01000000u)   /* a data block 16 MiB into RAM */
#define STACK_TOP (SR_RAM_BASE + 0x01100000u)   /* the stack region above it */
#define MAX_CALLS 64u

typedef struct {
    uint32_t addr, count, arg[3];
} CallSpec;

static int parse_call(const char *text, CallSpec *c) {
    char buf[128];
    snprintf(buf, sizeof buf, "%s", text);
    c->arg[0] = DATA_ADDR;
    c->arg[1] = 0u;
    c->arg[2] = 0u;
    char *field[5] = { buf, NULL, NULL, NULL, NULL };
    int n = 1;
    for (char *p = buf; *p && n < 5; p++) {
        if (*p == ':') { *p = '\0'; field[n++] = p + 1; }
    }
    if (n < 2 || !sr_harness_parse_hex32(field[0], &c->addr)) return 0;
    char *end = NULL;
    unsigned long long count = strtoull(field[1], &end, 10);
    if (!end || end == field[1] || *end != '\0' || count == 0u || count > UINT32_MAX) return 0;
    c->count = (uint32_t)count;
    for (int i = 2; i < n; i++)
        if (!sr_harness_parse_hex32(field[i], &c->arg[i - 2])) return 0;
    return 1;
}

static int usage(void) {
    fprintf(stderr,
            "usage: verify_harness [--call ADDR:COUNT[:A0[:A1[:A2]]]]... [--sched-slice N]\n"
            "                      [--frozen-clock] [--hard-exit] [--selftest-corrupt FIELD]\n"
            "                      <image.bin> <base-hex>\n");
    return 2;
}

int main(int argc, char **argv) {
    CallSpec calls[MAX_CALLS];
    unsigned ncalls = 0;
    unsigned long slice = 0;
    int frozen = 0, hard_exit = 0, positional = 0;
    const char *image_path = NULL;
    uint32_t base = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--call") && i + 1 < argc) {
            if (ncalls >= MAX_CALLS || !parse_call(argv[++i], &calls[ncalls])) return usage();
            ncalls++;
        } else if (!strcmp(a, "--sched-slice") && i + 1 < argc) {
            char *end = NULL;
            slice = strtoul(argv[++i], &end, 10);
            if (!end || *end != '\0' || slice == 0ul) return usage();
        } else if (!strcmp(a, "--frozen-clock")) {
            frozen = 1;
        } else if (!strcmp(a, "--hard-exit")) {
            hard_exit = 1;
        } else if (!strcmp(a, "--selftest-corrupt") && i + 1 < argc) {
            s_selftest_corrupt = argv[++i];
        } else if (a[0] == '-' && a[1] == '-') {
            return usage();
        } else if (positional == 0) {
            image_path = a;
            positional++;
        } else if (positional == 1 && sr_harness_parse_hex32(a, &base)) {
            positional++;
        } else {
            return usage();
        }
    }
    if (positional != 2) return usage();

    uint32_t image_len = 0;
    uint8_t *image = sr_harness_read_image(image_path, &image_len);
    if (!image) {
        fprintf(stderr, "verify_harness: cannot read %s\n", image_path);
        return 2;
    }
    sr_mem_init();
    sr_load_segment(base, image, image_len);
    free(image);
    sr_register_all();
    for (uint32_t i = 0; i < 0x40u; i++)
        g_mem[DATA_ADDR - SR_RAM_BASE + i] = (uint8_t)(i * 37u + 11u);

    SrVerifyConfig cfg;
    sr_verify_config_from_env(&cfg);
    sr_verify_configure(&cfg);
    if (frozen) sr_verify_set_clock(frozen_clock);
    if (slice) {
        s_slice = slice;
        sr_sched_on = 1;
        sr_dispatch_host_stack_remaining = main_stack_remaining;
        atomic_store(&sr_timeslice, (int_least32_t)slice);
    }

    static CpuState cpu;
    for (unsigned i = 0; i < ncalls; i++) {
        for (uint32_t n = 0; n < calls[i].count; n++) {
            memset(&cpu, 0, sizeof cpu);
            cpu.r[29] = STACK_TOP;
            cpu.r[31] = SR_HOST_RETURN_PC;
            cpu.r[4] = calls[i].arg[0];
            cpu.r[5] = calls[i].arg[1];
            cpu.r[6] = calls[i].arg[2];
            dispatch_call(&cpu, calls[i].addr, SR_HOST_RETURN_PC);
        }
    }
    fprintf(stderr, "verify_harness: %u call spec(s) done, %u yield(s)\n", ncalls, s_yields);
    if (hard_exit) {
        fflush(stderr);
        SR_VERIFY_REPORT_NOW();
        _Exit(0);
    }
    return 0;
}

#else
typedef int nk_verify_harness_is_verify_build_only;
#endif /* SR_VERIFY */
