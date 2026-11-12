// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * AOT <-> interpreter cosimulation gate.
 *
 * Both execution lanes run THE SAME source-owned guest bytes, produced by the
 * ordinary production pipeline (fixtures/cosim/generate.py -> prxload -> codegen):
 *
 *   lane AOT     the generated f_<addr> body, entered through production dispatch()
 *   lane INTERP  the production interpreter floor (src/rt/guest_interp.c) over the
 *                loaded image, entered through the same dispatch() with the cell's
 *                native body simply absent from the dispatch table
 *
 * Nothing about lane INTERP is a test mode: it is precisely the seam the AOT-gap
 * smoke exercises at build time (--omit-aot), selected here at run time so one
 * build can compare both lanes cell by cell.
 *
 * EVIDENCE TIER 2 (production helper / white-box). The production dispatch core,
 * the production interpreter and real codegen output all execute; the CpuState
 * seeding, the dispatch-table reset and the cell entry are test-specific.
 *
 * WHAT IS AND IS NOT PROVEN
 *   * Integer, memory and control-flow semantics are independently implemented in
 *     the two lanes, so a disagreement is real evidence.
 *   * The fpu cell still compares AOT and interpreter operand selection and FCR31
 *     threading. Its shared arithmetic helper is additionally checked by the
 *     AOT-only fpu_aot cell against fpu_reference.c, a project-authored binary32
 *     evaluator that uses integer bit manipulation rather than production helpers
 *     or the host rounding environment.
 */

#include "recomp.c"   /* white-box: the real dispatch core and its dispatch table */

#include "cosim_recomp_funcs.h"  /* generated: f_<addr> declarations */
#include "cosim_cells.h"         /* generated: cell addresses and the guest layout */
#include "cosim_fpu_corpus.h"    /* generated: deterministic scalar-FPU boundary corpus */
#include "fpu_reference.h"       /* verification-only integer binary32 evaluator */
#include "perf.h"

#include <stdlib.h>
#include <string.h>

/* The C newline escape, spelled once so the sources that generate this file
 * never have to round-trip a backslash through a shell. */
#define NEWLINE "\n"

void sr_register_all(void);   /* generated: executable spans + every native body */

/* ---- stubs for runtime symbols recomp.c references ------------------------------- */

/* No GE in this harness: the VRAM coherence hooks the guest accessors call are inert. */
volatile int g_sr_vram_read_pending;
void sr_ge_vram_read_prepare(uint32_t addr, uint32_t width) { (void)addr; (void)width; }
void sr_ge_vram_write_prepare(uint32_t addr, uint32_t width) { (void)addr; (void)width; }
void sr_ge_vram_write_commit(uint32_t addr, uint32_t width) { (void)addr; (void)width; }

uint32_t g_sr_debug = 0;
SrMemWatch g_sr_mem_watches[SR_MAX_MEM_WATCHES];
int g_sr_mem_watch_count = 0;
int g_sr_metadata_watch = 0;
uint32_t g_sr_mem_watch_context_pc = 0;
unsigned g_sr_mem_watch_context_limit = 0;
unsigned g_sr_mem_watch_context_count = 0;
int g_sr_mem_watch_context_fpr = -1;
uint32_t g_sr_mem_watch_context_fpr_value = 0;
uint32_t g_sr_store_context_pc = 0;
unsigned g_sr_store_context_count = 0;
unsigned g_sr_store_context_limit = 0;
int g_sr_store_context_mem_gpr = -1;
uint32_t g_sr_store_context_mem_offset = 0;
unsigned g_sr_store_context_mem_words = 0;
int g_sr_last_writer_enabled = 0;
CpuState *s_cpu = NULL;
int sr_sched_on = 0;
atomic_int_least32_t sr_timeslice;

uint32_t sched_current_uid(void) { return 0u; }
void sched_exit_current(int32_t status) { (void)status; }
void sched_exit_current_delete(int32_t status) { (void)status; }
uint32_t sched_start_thread(uint32_t uid, uint32_t arglen, uint32_t argp) { (void)uid; (void)arglen; (void)argp; return 0; }
uint32_t sched_terminate_thread(uint32_t uid) { (void)uid; return 0; }
uint32_t sched_delete_thread(uint32_t uid) { (void)uid; return 0; }
uint32_t sched_thread_wakeup(uint32_t uid) { (void)uid; return 0; }
void sched_set_current_join_target(uint32_t uid) { (void)uid; }
void sched_clear_current_join_target(void) {}
int sched_take_current_join_result(uint32_t uid, uint32_t *result_out) { (void)uid; (void)result_out; return 0; }
uint32_t sr_get_ge_status(void) { return 0u; }
uint32_t sr_hle_resolve_late_import(uint32_t nid) { (void)nid; return 0u; }
/* The generated sr_register_all() hands the main module's record to the HLE module manager,
 * which the cosimulation harness does not have. */
void sr_main_module_register(const SrMainModuleRecord *record) { (void)record; }
uint32_t sr_syscall(CpuState *s, uint32_t nid) { (void)s; (void)nid; return 0u; }
/* The harness has no scheduler. The resume cases (#2172) turn the slice counter on and take
 * a yield as the park; every other case leaves sr_sched_on at 0 and never gets here. */
static void (*g_cosim_yield_hook)(CpuState *s);
void sr_yield(CpuState *s) {
    if (g_cosim_yield_hook) g_cosim_yield_hook(s);
}
int sr_vfpu_interp(CpuState *s, uint32_t op) { (void)s; (void)op; return 0; }
uint64_t SDL_GetTicksNS(void) { return 0u; }

/* ---- ordered guest-write log --------------------------------------------------- */
/*
 * sr_note_mem_write() is the production last-writer hook: every guest store in
 * BOTH lanes reaches it through the same MEM_W*_PC accessors, so this list is an
 * ordered record of the lane's memory effects rather than a lane-specific probe.
 * The pre-store value is read here, before the accessor commits, giving each
 * record a real before/after pair.
 */

#define COSIM_MAX_WRITES 64

typedef struct {
    uint32_t pc;
    uint32_t address;
    uint32_t width;
    uint32_t before;
    uint32_t after;
} CosimWrite;

typedef struct {
    CosimWrite records[COSIM_MAX_WRITES];
    unsigned count;
    unsigned overflow;
} CosimWriteLog;

static CosimWriteLog *g_active_write_log = NULL;

void sr_note_mem_write(uint32_t addr, uint32_t width, uint32_t val, uint32_t pc) {
    if (!g_active_write_log) {
        return;
    }
    if (g_active_write_log->count >= COSIM_MAX_WRITES) {
        g_active_write_log->overflow++;
        return;
    }
    uint32_t before = 0u;
    if (sr_guest_span_readable(addr, width)) {
        before = width == 1u ? MEM_R8(addr) : width == 2u ? MEM_R16(addr) : MEM_R32(addr);
    }
    CosimWrite *record = &g_active_write_log->records[g_active_write_log->count++];
    record->pc = pc;
    record->address = addr;
    record->width = width;
    record->before = before;
    record->after = val;
}

/* ---- canonical instruction trace ------------------------------------------------ */
/*
 * sr_trace_open()/sr_begin()/sr_end() is the production per-instruction trace the
 * generated code already emits (TRACE_FORMAT.md). src/rt/guest_interp.c emits the
 * same records in the same order, so the two lanes produce directly comparable
 * traces and the FIRST differing line names the exact guest PC and opcode.
 */

#define COSIM_MAX_TRACE_LINES 512
#define COSIM_TRACE_LINE 512

typedef struct {
    char lines[COSIM_MAX_TRACE_LINES][COSIM_TRACE_LINE];
    unsigned count;
    unsigned truncated;
} CosimTrace;

static void trace_load(CosimTrace *trace, const char *path) {
    trace->count = 0;
    trace->truncated = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return;
    }
    char line[COSIM_TRACE_LINE * 2];
    while (fgets(line, (int)sizeof line, fp)) {
        if (line[0] == '#') {
            continue;   /* header: identical by construction in both lanes */
        }
        size_t length = strlen(line);
        while (length > 0u && (line[length - 1u] == '\n' || line[length - 1u] == '\r')) {
            line[--length] = '\0';
        }
        if (length == 0u) {
            continue;
        }
        if (trace->count >= COSIM_MAX_TRACE_LINES) {
            trace->truncated = 1;
            break;
        }
        if (length >= COSIM_TRACE_LINE) {
            trace->truncated = 1;
            length = COSIM_TRACE_LINE - 1u;
        }
        memcpy(trace->lines[trace->count], line, length);
        trace->lines[trace->count][length] = '\0';
        trace->count++;
    }
    fclose(fp);
}

/* ---- architectural state vector ------------------------------------------------- */
/*
 * One flat, ordered list of every architecturally visible 32-bit field, so a
 * comparison reports the FIRST differing field by name instead of "states differ".
 *
 * CpuState.pc is deliberately absent: it is not a shared architectural field
 * between the lanes (see the precise-PC contract below) and is asserted
 * separately, per lane, against its documented value.
 */

#define COSIM_FIELD_COUNT (32 + 2 + 2 + 32 + 128 + 16 + 32 + 5)

typedef struct {
    char name[12];
    uint32_t value;
} CosimField;

typedef struct {
    CosimField fields[COSIM_FIELD_COUNT];
    unsigned count;
} CosimVector;

/* COSIM_FIELD_COUNT is hand-derived from the CpuState layout, so a field added
 * to the vector without updating it would run off the end silently. Fail loudly
 * instead: this is a comparator, and a comparator that corrupts its own storage
 * reports whatever happens to be adjacent. */
static void vector_push(CosimVector *vector, const char *name, uint32_t value) {
    if (vector->count >= COSIM_FIELD_COUNT) {
        fprintf(stderr,
                "cosim: architectural vector overflow at field '%s' (capacity %u). "
                "COSIM_FIELD_COUNT no longer matches vector_build()." NEWLINE,
                name, (unsigned)COSIM_FIELD_COUNT);
        exit(2);
    }
    CosimField *field = &vector->fields[vector->count++];
    snprintf(field->name, sizeof field->name, "%s", name);
    field->value = value;
}

static void vector_pushf(CosimVector *vector, const char *format, unsigned index, uint32_t value) {
    if (vector->count >= COSIM_FIELD_COUNT) {
        fprintf(stderr, "cosim: architectural vector overflow (capacity %u)" NEWLINE,
                (unsigned)COSIM_FIELD_COUNT);
        exit(2);
    }
    CosimField *field = &vector->fields[vector->count++];
    snprintf(field->name, sizeof field->name, format, index);
    field->value = value;
}

static void vector_build(CosimVector *vector, const CpuState *s) {
    vector->count = 0;
    for (unsigned i = 0; i < 32u; i++) vector_pushf(vector, "r%u", i, s->r[i]);
    vector_push(vector, "hi", s->hi);
    vector_push(vector, "lo", s->lo);
    vector_push(vector, "fcr31", s->fcr31);
    vector_push(vector, "fpcond", s->fpcond);
    for (unsigned i = 0; i < 32u; i++) vector_pushf(vector, "f%u", i, s->fi[i]);
    for (unsigned i = 0; i < 128u; i++) vector_pushf(vector, "v%u", i, s->vi[i]);
    for (unsigned i = 0; i < 16u; i++) vector_pushf(vector, "vc%u", i, s->vfpuCtrl[i]);
    for (unsigned i = 0; i < 32u; i++) vector_pushf(vector, "cop0[%u]", i, s->cop0[i]);
    vector_push(vector, "next_pc", s->next_pc);
    vector_push(vector, "delayslot", s->in_delay_slot);
    vector_push(vector, "flow_kind", s->flow_kind);
    vector_push(vector, "flow_target", s->flow_target);
    vector_push(vector, "llbit", s->llbit);
}

/* ---- lane execution ------------------------------------------------------------- */

typedef enum {
    COSIM_TERM_AOT_HOST_RETURN = 0,   /* generated body returned to its host caller */
    COSIM_TERM_INTERP_HANDOFF  = 1,   /* interpreter reached a registered AOT target */
    COSIM_TERM_CALL_RETURN     = 2,   /* interpreter reached an explicit CALL boundary */
    COSIM_TERM_REJECT          = 3,   /* the dispatch core refused to execute */
} CosimTermination;

typedef struct {
    const char *lane;
    CpuState state;
    CosimVector vector;
    CosimWriteLog writes;
    CosimTrace trace;
    unsigned char window[COSIM_WINDOW_HI - COSIM_WINDOW_LO];
    CosimTermination termination;
    int dispatch_result;
    uint32_t handoff_target;
    int trampoline_entered;
} CosimLane;

static int g_trampoline_hits = 0;

/* One resume case (#2172) needs a native body that leaves exception flow behind, which no
 * generated body does without the LLE CPU. With g_trampoline_raise set, the trampoline does so
 * once, and the dispatcher dispatches that target again under the same call. */
static uint32_t g_trampoline_raise = 0u;

/* The interpreter's handoff destination. It is deliberately inert: it exists so
 * lane INTERP has a REGISTERED AOT target to stop at, and it must not perturb any
 * guest-visible state, or the lanes would differ because of the harness. The one
 * exception is the resume case above, which no lane comparison runs. */

static void cosim_return_trampoline(CpuState *s) {
    g_trampoline_hits++;
    if (g_trampoline_raise != 0u) {
        s->flow_kind = SR_FLOW_EXCEPTION;
        s->flow_target = g_trampoline_raise;
        g_trampoline_raise = 0u;
    }
}

static uint32_t g_image_base = 0u;

/* Install the dispatch table for one lane.
 *
 * sr_register_all() is the real generated registration: it resets and installs
 * the analyzer-owned executable spans and every native body. Executable spans
 * live outside the dispatch table, so dropping the table afterwards leaves lane
 * INTERP with exactly the ownership lane AOT has and none of the bodies -- the
 * run-time equivalent of codegen's --omit-aot, applied to every cell at once.
 */
/* Lane MIXED: every native body EXCEPT one.
 *
 * This is the tier boundary the production AOT-gap floor actually crosses --
 * native caller, dispatch miss, production interpreter, and back. The omitted
 * body is removed by rebuilding the table from a snapshot rather than by
 * punching a hole in it, because the main table is open-addressed and clearing
 * an interior slot would truncate the probe chain of any key that collided
 * through it. */
typedef struct {
    uint32_t addr;
    RecompFn fn;
} CosimRegistration;

#define COSIM_MAX_REGISTRATIONS 256

static CosimRegistration g_registrations[COSIM_MAX_REGISTRATIONS];
static unsigned g_registration_count;

static void snapshot_registrations(void) {
    g_registration_count = 0;
    for (uint32_t i = 0; i < SR_DTAB_SIZE; i++) {
        if (atomic_load_explicit(&g_dtab.main[i].state, memory_order_relaxed) != 1u) {
            continue;
        }
        if (g_registration_count >= COSIM_MAX_REGISTRATIONS) {
            fprintf(stderr, "cosim: registration snapshot overflow" NEWLINE);
            exit(2);
        }
        g_registrations[g_registration_count].addr =
            atomic_load_explicit(&g_dtab.main[i].addr, memory_order_relaxed);
        g_registrations[g_registration_count].fn =
            (RecompFn)atomic_load_explicit(&g_dtab.main[i].fn, memory_order_relaxed);
        g_registration_count++;
    }
}

static void install_lane_mixed(uint32_t omit_address) {
    memset(&g_dtab, 0, sizeof g_dtab);
    s_register_count = 0;
    sr_register_all();
    snapshot_registrations();

    int found = 0;
    for (unsigned i = 0; i < g_registration_count; i++) {
        if (g_registrations[i].addr == omit_address) {
            found = 1;
        }
    }
    if (!found) {
        /* The cell would silently become an ordinary all-native run and every
         * cross-tier assertion below would pass without crossing anything. */
        fprintf(stderr,
                "cosim: cross-tier omission 0x%08x is not a registered body" NEWLINE,
                omit_address);
        exit(2);
    }

    memset(&g_dtab, 0, sizeof g_dtab);
    s_register_count = 0;
    for (unsigned i = 0; i < g_registration_count; i++) {
        if (g_registrations[i].addr == omit_address) {
            continue;
        }
        sr_register(g_registrations[i].addr, g_registrations[i].fn);
    }
    sr_register(COSIM_RETURN, cosim_return_trampoline);
}

static void install_lane(int with_native_bodies) {
    memset(&g_dtab, 0, sizeof g_dtab);
    s_register_count = 0;
    sr_register_all();
    if (!with_native_bodies) {
        memset(&g_dtab, 0, sizeof g_dtab);
        s_register_count = 0;
    }
    sr_register(COSIM_RETURN, cosim_return_trampoline);
}

/* A declared, self-retiring architectural asymmetry.
 *
 * `fields` is the COMPLETE set of architectural fields allowed to differ, and it
 * is enforced in BOTH directions: a field that stops differing fails the gate
 * just as loudly as an extra one that starts. That is what makes a known-defect
 * declaration retire itself -- when the production repair lands, this gate goes
 * red until the declaration is deleted. */
typedef struct {
    const char *const *fields;        /* NULL-terminated */
    CosimTermination termination;     /* the lane-B termination this defect forces */
    const char *reference;            /* what has to change for this to retire */
} CosimKnownDefect;

typedef struct {
    const char *name;
    uint32_t address;
    uint32_t words;
    const char *description;
    uint32_t cross_tier_omit;         /* 0, or the body lane MIXED must drop */
    uint32_t fcr31;
    const CosimKnownDefect *known_defect;
} CosimCase;

/* Deterministic, poisoned seed.
 *
 * Every general register, FPU register and VFPU lane starts at a distinct
 * non-zero value, so a lane that fails to materialize a register shows the
 * poison rather than a plausible zero. */
static void seed_state(CpuState *s, const CosimCase *test) {
    memset(s, 0, sizeof *s);
    for (unsigned i = 1; i < 32u; i++) s->r[i] = 0xa5a50000u | i;
    for (unsigned i = 0; i < 32u; i++) s->fi[i] = 0xf0f00000u | i;
    for (unsigned i = 0; i < 128u; i++) s->vi[i] = 0xc0c00000u | i;
    for (unsigned i = 0; i < 16u; i++) s->vfpuCtrl[i] = 0xd0d00000u | i;
    s->r[0] = 0u;
    s->r[4] = COSIM_SCRATCH;    /* $a0: the scratch pointer every memory cell uses */
    s->r[29] = COSIM_STACK;     /* $sp */
    s->r[31] = COSIM_RETURN;    /* $ra: the cosim synchronization point */
    s->hi = 0x11223344u;
    s->lo = 0x55667788u;
    s->fcr31 = test->fcr31;
    /* FCC0 lives in FCR31 bit 23 and fpcond caches it; seed them coherently so a
     * lane that desynchronizes them is visible rather than pre-broken. */
    s->fpcond = (test->fcr31 >> 23) & 1u;
    /* The synthetic caller PC. COSIM_ENTRY is guest text and is outside the
     * 0x1000..0x10ff PLT window (which the PLT miss policy claims), so that policy
     * cannot shadow the behavior under test. */
    s->pc = COSIM_ENTRY;
    for (unsigned i = 0; i < 32u; i++) s->cop0[i] = 0xc0c00000u | i;
    s->cop0[SR_CP0_STATUS] = 0u;
    s->next_pc = 0u;
    s->in_delay_slot = 0u;
    s->flow_kind = 0u;
    s->flow_target = 0u;
    s->llbit = 0u;   /* no ll..sc window is open at a cell's entry */
}

static void seed_window(void) {
    for (uint32_t address = COSIM_WINDOW_LO; address < COSIM_WINDOW_HI; address += 4u) {
        MEM_W32(address, 0xdead0000u | (address - COSIM_WINDOW_LO));
    }
}

typedef enum {
    COSIM_LANE_INTERP = 0,   /* no native bodies at all */
    COSIM_LANE_AOT    = 1,   /* every native body */
    COSIM_LANE_MIXED  = 2,   /* every native body except the cell's omission */
} CosimLaneMode;

static void run_lane(CosimLane *lane, const CosimCase *test, int mode,
                     const char *trace_path) {
    lane->lane = mode == COSIM_LANE_AOT ? "AOT"
               : mode == COSIM_LANE_INTERP ? "INTERP"
               : "MIXED";
    if (mode == COSIM_LANE_MIXED) {
        install_lane_mixed(test->cross_tier_omit);
    } else {
        install_lane(mode);
    }

    /* Seeding order matters: the window is rewritten through the ordinary store
     * accessors, so it must happen before the write log is armed. */
    seed_window();
    memset(&lane->writes, 0, sizeof lane->writes);
    g_trampoline_hits = 0;
    seed_state(&lane->state, test);
    const int dispatcher_boundary = strcmp(test->name, "rarewrite-dispatch") == 0;
    if (dispatcher_boundary) {
        lane->state.pc = COSIM_RA_REWRITE_RESUME - 8u;
        lane->state.r[31] = COSIM_RA_REWRITE_RESUME;
        lane->state.r[5] = COSIM_RA_REWRITE_RESUME;
    }
    s_cpu = &lane->state;

    if (sr_trace_open(trace_path, "cosim", test->address) != 0) {
        fprintf(stderr, "cosim: cannot open trace file %s\n", trace_path);
        exit(2);
    }
    g_active_write_log = &lane->writes;
    g_sr_last_writer_enabled = 1;

    lane->dispatch_result = dispatcher_boundary
        ? dispatch_call_try(&lane->state, COSIM_RA_REWRITE_CALLEE,
                            COSIM_RA_REWRITE_RESUME)
        : dispatch_try(&lane->state, test->address);

    g_sr_last_writer_enabled = 0;
    g_active_write_log = NULL;
    sr_trace_close();
    s_cpu = NULL;

    lane->trampoline_entered = g_trampoline_hits > 0;
    if (lane->dispatch_result < 0) {
        lane->termination = COSIM_TERM_REJECT;
        lane->handoff_target = 0u;
    } else if (dispatcher_boundary &&
               lane->dispatch_result == SR_GUEST_INTERP_CALL_RETURN) {
        lane->termination = COSIM_TERM_CALL_RETURN;
        lane->handoff_target = lane->state.pc;
    } else if (lane->trampoline_entered) {
        /* The interpreter owns and advances the architectural PC, and leaves the
         * handoff destination there. */
        lane->termination = COSIM_TERM_INTERP_HANDOFF;
        lane->handoff_target = lane->state.pc;
    } else {
        /* Generated code does not materialize an architectural PC; a `jr $ra`
         * exit IS the host return, so the destination is the $ra it jumped
         * through. */
        lane->termination = COSIM_TERM_AOT_HOST_RETURN;
        lane->handoff_target = lane->state.r[31];
    }

    for (uint32_t address = COSIM_WINDOW_LO; address < COSIM_WINDOW_HI; address++) {
        lane->window[address - COSIM_WINDOW_LO] = MEM_R8(address);
    }
    vector_build(&lane->vector, &lane->state);
    trace_load(&lane->trace, trace_path);
}

/* ---- reporting ------------------------------------------------------------------ */

static int g_failures = 0;

static void report_fail(const CosimCase *test, const char *what) {
    g_failures++;
    fprintf(stderr, "\nCOSIM DIVERGENCE cell=%s (%s)\n  %s\n",
            test->name, test->description, what);
}

static void expect_lane_word(const CosimCase *test, const CosimLane *lane,
                             const char *field, uint32_t got, uint32_t want,
                             int *failed) {
    if (got == want) return;
    char message[192];
    snprintf(message, sizeof message,
             "lane %s expected %s=0x%08x, got 0x%08x",
             lane->lane, field, want, got);
    report_fail(test, message);
    *failed = 1;
}

static void dump_cell_program(const CosimCase *test) {
    fprintf(stderr, "  guest program at 0x%08x (%u words):\n", test->address, test->words);
    for (uint32_t i = 0; i < test->words; i++) {
        const uint32_t address = test->address + i * 4u;
        fprintf(stderr, "    0x%08x  0x%08x\n", address, MEM_R32(address));
    }
}

static const char *termination_name(CosimTermination termination) {
    switch (termination) {
    case COSIM_TERM_AOT_HOST_RETURN: return "aot-host-return";
    case COSIM_TERM_INTERP_HANDOFF: return "interpreter-aot-handoff";
    case COSIM_TERM_CALL_RETURN: return "interpreter-call-return";
    default: return "dispatch-reject";
    }
}

static void dump_termination(const CosimLane *lane) {
    fprintf(stderr,
            "    lane %-6s termination=%s dispatch_result=%d handoff_target=0x%08x "
            "raw_pc=0x%08x sp=0x%08x ra=0x%08x\n",
            lane->lane, termination_name(lane->termination), lane->dispatch_result,
            lane->handoff_target, lane->state.pc, lane->state.r[29], lane->state.r[31]);
}

static void dump_write_log(const CosimLane *lane) {
    fprintf(stderr, "    lane %-6s ordered writes (%u%s):\n", lane->lane,
            lane->writes.count, lane->writes.overflow ? ", TRUNCATED" : "");
    for (unsigned i = 0; i < lane->writes.count; i++) {
        const CosimWrite *record = &lane->writes.records[i];
        fprintf(stderr, "      [%u] pc=0x%08x m%u[0x%08x] 0x%08x -> 0x%08x\n",
                i, record->pc, record->width * 8u, record->address,
                record->before, record->after);
    }
}

/* The first differing ordered memory effect, reported with the exact instruction
 * PC that produced it. */
static int compare_write_logs(const CosimCase *test, const CosimLane *a, const CosimLane *b) {
    const unsigned limit = a->writes.count < b->writes.count
        ? a->writes.count : b->writes.count;
    for (unsigned i = 0; i < limit; i++) {
        const CosimWrite *x = &a->writes.records[i];
        const CosimWrite *y = &b->writes.records[i];
        if (x->pc == y->pc && x->address == y->address && x->width == y->width &&
            x->before == y->before && x->after == y->after) {
            continue;
        }
        char detail[512];
        snprintf(detail, sizeof detail,
                 "ordered memory effect #%u differs\n"
                 "    AOT     pc=0x%08x m%u[0x%08x] 0x%08x -> 0x%08x\n"
                 "    INTERP  pc=0x%08x m%u[0x%08x] 0x%08x -> 0x%08x",
                 i, x->pc, x->width * 8u, x->address, x->before, x->after,
                 y->pc, y->width * 8u, y->address, y->before, y->after);
        report_fail(test, detail);
        return 1;
    }
    if (a->writes.overflow || b->writes.overflow) {
        /* The trace channel already fails closed on truncation; the ordered-write
         * channel must too, or a divergence past the cap would be invisible to
         * it while the log still printed a plausible-looking prefix. */
        char detail[256];
        snprintf(detail, sizeof detail,
                 "the ordered write log overflowed (%s dropped %u, %s dropped %u); "
                 "the memory-effect comparison is incomplete",
                 a->lane, a->writes.overflow, b->lane, b->writes.overflow);
        report_fail(test, detail);
        return 1;
    }
    if (a->writes.count != b->writes.count) {
        const CosimLane *longer = a->writes.count > b->writes.count ? a : b;
        const CosimWrite *extra = &longer->writes.records[limit];
        char detail[512];
        snprintf(detail, sizeof detail,
                 "lane %s performed %u ordered writes, lane %s performed %u; the first\n"
                 "    unmatched one is pc=0x%08x m%u[0x%08x] 0x%08x -> 0x%08x",
                 a->lane, a->writes.count, b->lane, b->writes.count,
                 extra->pc, extra->width * 8u, extra->address, extra->before, extra->after);
        report_fail(test, detail);
        return 1;
    }
    return 0;
}

/* The first differing traced instruction. Both lanes emit the canonical
 * production trace, so this localizes a divergence to one guest instruction. */
static int compare_traces(const CosimCase *test, const CosimLane *a, const CosimLane *b) {
    const unsigned limit = a->trace.count < b->trace.count ? a->trace.count : b->trace.count;
    for (unsigned i = 0; i < limit; i++) {
        if (strcmp(a->trace.lines[i], b->trace.lines[i]) == 0) {
            continue;
        }
        if (test->known_defect) {
            continue;
        }
        char detail[2048];
        snprintf(detail, sizeof detail,
                 "instruction trace diverges at step %u\n"
                 "    AOT     %s\n"
                 "    INTERP  %s\n"
                 "    (each line is `step pc=... op=... <changed registers> <memory>`;\n"
                 "     the pc/op prefix names the exact guest instruction)",
                 i, a->trace.lines[i], b->trace.lines[i]);
        report_fail(test, detail);
        return 1;
    }
    if (a->trace.count != b->trace.count) {
        if (test->known_defect) {
            /* A declared defect changes how many instructions run; the
             * architectural vector is what pins its exact shape. Report it so it
             * stays visible, but do not double-count it as a second failure. */
            fprintf(stderr,
                    "  cosim: cell %s executed %u instructions in lane %s vs %u in "
                    "lane %s (declared defect)" NEWLINE,
                    test->name, a->trace.count, a->lane, b->trace.count, b->lane);
            return 0;
        }
        const CosimLane *longer = a->trace.count > b->trace.count ? a : b;
        char detail[2048];
        snprintf(detail, sizeof detail,
                 "lane %s executed %u traced instructions, lane %s executed %u;\n"
                 "    the first unmatched instruction is: %s",
                 a->lane, a->trace.count, b->lane, b->trace.count,
                 longer->trace.lines[limit]);
        report_fail(test, detail);
        return 1;
    }
    if (a->trace.truncated || b->trace.truncated) {
        report_fail(test, "instruction trace was truncated; the comparison is incomplete");
        return 1;
    }
    if (a->trace.count == 0u) {
        report_fail(test, "no instructions were traced; the comparison would be vacuous");
        return 1;
    }
    return 0;
}

/* The first differing architectural field, honouring exactly one declared
 * expected divergence per cell -- no more and no fewer. */
static int declared_field(const CosimCase *test, const char *name) {
    if (!test->known_defect) {
        return -1;
    }
    for (unsigned i = 0; test->known_defect->fields[i]; i++) {
        if (strcmp(test->known_defect->fields[i], name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int compare_vectors(const CosimCase *test, const CosimLane *a, const CosimLane *b) {
    int failed = 0;
    unsigned char seen[16];
    memset(seen, 0, sizeof seen);

    for (unsigned i = 0; i < a->vector.count; i++) {
        if (a->vector.fields[i].value == b->vector.fields[i].value) {
            continue;
        }
        const char *name = a->vector.fields[i].name;
        const int declared = declared_field(test, name);
        if (declared >= 0 && (unsigned)declared < sizeof seen) {
            seen[declared] = 1;
            continue;
        }
        if (!failed) {
            char detail[512];
            snprintf(detail, sizeof detail,
                     "architectural field %s differs: %s=0x%08x %s=0x%08x",
                     name, a->lane, a->vector.fields[i].value,
                     b->lane, b->vector.fields[i].value);
            report_fail(test, detail);
            failed = 1;
        }
    }

    if (test->known_defect) {
        for (unsigned i = 0; test->known_defect->fields[i]; i++) {
            if (i < sizeof seen && seen[i]) {
                continue;
            }
            char detail[512];
            snprintf(detail, sizeof detail,
                     "field %s was DECLARED to diverge and did NOT.\n"
                     "    Either the comparator stopped observing this asymmetry, or the\n"
                     "    defect it stands for was repaired -- in which case delete the\n"
                     "    declaration. Retirement criterion: %s",
                     test->known_defect->fields[i], test->known_defect->reference);
            report_fail(test, detail);
            failed = 1;
        }
    }
    return failed;
}

static int compare_windows(const CosimCase *test, const CosimLane *a, const CosimLane *b) {
    for (unsigned i = 0; i < sizeof a->window; i++) {
        if (a->window[i] == b->window[i]) {
            continue;
        }
        char detail[256];
        snprintf(detail, sizeof detail,
                 "guest memory differs at 0x%08x: AOT=0x%02x INTERP=0x%02x",
                 COSIM_WINDOW_LO + i, a->window[i], b->window[i]);
        report_fail(test, detail);
        return 1;
    }
    return 0;
}

static uint32_t lane_window_word(const CosimLane *lane, uint32_t address) {
    const unsigned offset = (unsigned)(address - (uint32_t)COSIM_WINDOW_LO);
    return (uint32_t)lane->window[offset] |
           ((uint32_t)lane->window[offset + 1u] << 8) |
           ((uint32_t)lane->window[offset + 2u] << 16) |
           ((uint32_t)lane->window[offset + 3u] << 24);
}

static int unaligned_matches_expected(const CosimLane *lane,
                                      const uint32_t expected_gprs[8]) {
    static const unsigned registers[8] = { 10u, 11u, 12u, 13u, 14u, 15u, 24u, 25u };
    static const uint32_t memory_offsets[10] = {
        0u, 4u, 8u, 12u, 16u, 20u, 24u, 28u, 32u, 36u
    };
    static const uint32_t expected_memory[10] = {
        0x11223344u, 0x55667788u,
        0x112233A1u, 0x1122A1B2u, 0x11A1B2C3u, 0xA1B2C3D4u,
        0xA1B2C3D4u, 0xB2C3D444u, 0xC3D43344u, 0xD4223344u
    };
    for (unsigned i = 0; i < 8u; i++) {
        if (lane->state.r[registers[i]] != expected_gprs[i]) return 0;
    }
    for (unsigned i = 0; i < 10u; i++) {
        if (lane_window_word(lane, (uint32_t)COSIM_SCRATCH + memory_offsets[i]) !=
            expected_memory[i]) return 0;
    }
    return 1;
}

/* ---- precise PC contract -------------------------------------------------------- */
/*
 * CpuState.pc is NOT a shared architectural field between the lanes, and the
 * cosim would be dishonest if it pretended otherwise. The contract each lane
 * actually satisfies is pinned here, so a change to either lane's PC behavior
 * fails this gate instead of silently redefining what pc means:
 *
 *   CURRENT INSTRUCTION   lane INTERP: pc, advanced per instruction.
 *                         lane AOT:    not in CpuState. The generated code carries
 *                                      each instruction's address as a literal into
 *                                      sr_begin(), which is why the instruction
 *                                      trace -- not CpuState -- localizes an AOT
 *                                      divergence.
 *   NEXT INSTRUCTION      neither lane maintains next_pc; both leave it at its
 *                         seeded value (parity field for src/ref).
 *   BRANCH OWNER          the branch instruction owns its delay slot in both lanes:
 *                         condition and transfer target are read at the branch,
 *                         the link register is written before the slot, and the AOT
 *                         tier is not reconsulted at pc+4.
 *   DELAY SLOT            in_delay_slot is likewise unmaintained by both lanes.
 *   AOT HANDOFF           on `jr $ra`, lane AOT returns to its host caller without
 *                         writing pc; the architectural destination is $ra.
 *   INTERPRETER HANDOFF   lane INTERP writes pc = destination before dispatching
 *                         into the registered native body.
 *   EXCEPTION FUTURE      next_pc and in_delay_slot are unclaimed by both lanes and
 *                         are asserted to stay at their seeded values, so a future
 *                         COP0 BD/EPC model can define them without colliding with
 *                         an existing consumer.
 */
static int check_pc_contract(const CosimCase *test, const CosimLane *aot,
                             const CosimLane *interp) {
    int failed = 0;
    char detail[768];

    if (aot->termination != COSIM_TERM_AOT_HOST_RETURN) {
        snprintf(detail, sizeof detail,
                 "lane AOT terminated as %s; the cell must leave native code through "
                 "its host return", termination_name(aot->termination));
        report_fail(test, detail);
        failed = 1;
    }
    /* Lane B's expected termination depends on what it IS.
     *
     *   INTERP  interprets everything and stops at the registered trampoline.
     *   MIXED   re-enters native code, so a correct cross-tier run ends exactly
     *           as lane AOT does -- through the caller's own host return. A
     *           declared defect may force the interpreter-handoff shape instead,
     *           and that expectation is enforced in both directions below.
     */
    const CosimTermination expected_b =
        test->cross_tier_omit == 0u
            ? COSIM_TERM_INTERP_HANDOFF
            : (test->known_defect ? test->known_defect->termination
                                  : COSIM_TERM_AOT_HOST_RETURN);
    if (interp->termination != expected_b) {
        snprintf(detail, sizeof detail,
                 "lane %s terminated as %s, expected %s", interp->lane,
                 termination_name(interp->termination), termination_name(expected_b));
        report_fail(test, detail);
        failed = 1;
    }
    if (aot->state.pc != COSIM_ENTRY) {
        snprintf(detail, sizeof detail,
                 "lane AOT wrote CpuState.pc (0x%08x, seeded 0x%08x). Generated code is "
                 "documented not to maintain an architectural PC; if that changed, the "
                 "precise-PC contract and every scheduler/debug consumer of pc must be "
                 "revisited", aot->state.pc, (uint32_t)COSIM_ENTRY);
        report_fail(test, detail);
        failed = 1;
    }
    /* Lane INTERP owns and advances the architectural pc and leaves the handoff
     * destination in it. Lane MIXED only reaches the interpreter for part of the
     * run, so pc is whatever the last interpreted step left; it is not a shared
     * observable and is not asserted here -- the handoff target below is. */
    if (test->cross_tier_omit == 0u && interp->state.pc != COSIM_RETURN) {
        snprintf(detail, sizeof detail,
                 "lane INTERP left CpuState.pc at 0x%08x, expected the handoff target "
                 "0x%08x", interp->state.pc, (uint32_t)COSIM_RETURN);
        report_fail(test, detail);
        failed = 1;
    }
    if (test->known_defect == NULL && aot->handoff_target != interp->handoff_target) {
        snprintf(detail, sizeof detail,
                 "normalized handoff target differs: AOT=0x%08x INTERP=0x%08x",
                 aot->handoff_target, interp->handoff_target);
        report_fail(test, detail);
        failed = 1;
    }
    /* PRECONDITION for normalizing lane AOT's handoff target to $ra.
     *
     * Generated code has no architectural PC, so the harness reads $ra at
     * TERMINATION and calls it the destination. That is only the destination the
     * transfer actually latched while nothing rewrote $ra afterwards -- a return
     * delay slot that writes $ra would make the normalization report a target
     * control never went to, and manufacture a divergence out of the harness
     * rather than the code under test.
     *
     * The canonical trace already carries the answer: the final delay slot's line
     * lists the registers it changed. If $ra is among them, this normalization is
     * not valid for the cell and the gate says so instead of quietly comparing
     * the wrong thing. */
    if (aot->trace.count >= 1u) {
        const char *last = aot->trace.lines[aot->trace.count - 1u];
        if (strstr(last, " r31=") != NULL) {
            snprintf(detail, sizeof detail,
                     "the cell's final delay slot writes $ra (%s). Lane AOT's handoff "
                     "target is normalized FROM $ra at termination, so this cell would "
                     "compare a destination control never reached; latch the transfer "
                     "target instead of rewriting $ra in a return slot", last);
            report_fail(test, detail);
            failed = 1;
        }
    }
    if (aot->handoff_target != COSIM_RETURN) {
        snprintf(detail, sizeof detail,
                 "cell did not return to the cosim synchronization point "
                 "(target=0x%08x, expected 0x%08x)",
                 aot->handoff_target, (uint32_t)COSIM_RETURN);
        report_fail(test, detail);
        failed = 1;
    }
    if (aot->state.next_pc != 0u || interp->state.next_pc != 0u ||
        aot->state.in_delay_slot != 0u || interp->state.in_delay_slot != 0u) {
        report_fail(test,
                    "next_pc/in_delay_slot are no longer unclaimed by both lanes; the "
                    "future COP0 BD/EPC model now has an existing consumer to reconcile");
        failed = 1;
    }
    return failed;
}

static int check_ra_rewrite_contract(const CosimCase *test, const CosimLane *aot,
                                     const CosimLane *interp, uint32_t resume_pc,
                                     int dispatcher_boundary) {
    int failed = 0;
    if (aot->termination != COSIM_TERM_REJECT) {
        report_fail(test,
                    "AOT accepted a linked callee whose live $ra differs from the call resume PC");
        failed = 1;
    }
    if (aot->dispatch_result != SR_GUEST_INTERP_FLOW_FATAL) {
        char detail[192];
        snprintf(detail, sizeof detail,
                 "AOT dispatch result=%d, expected fail-closed flow result=%d",
                 aot->dispatch_result, SR_GUEST_INTERP_FLOW_FATAL);
        report_fail(test, detail);
        failed = 1;
    }
    expect_lane_word(test, aot, "fatal flow kind", aot->state.flow_kind,
                     SR_FLOW_FATAL, &failed);
    expect_lane_word(test, aot, "rejected guest target", aot->state.flow_target,
                     COSIM_RA_REWRITE_TARGET, &failed);
    expect_lane_word(test, aot, "caller continuation not executed", aot->state.r[3],
                     0xa5a50003u, &failed);

    const CosimTermination expected_termination = dispatcher_boundary
        ? COSIM_TERM_CALL_RETURN : COSIM_TERM_INTERP_HANDOFF;
    if (interp->termination != expected_termination) {
        report_fail(test,
                    "interpreter did not follow the rewritten $ra to its call boundary");
        failed = 1;
    }
    const uint32_t interpreter_resume = dispatcher_boundary ? resume_pc : COSIM_RETURN;
    expect_lane_word(test, interp, "interpreter return target", interp->handoff_target,
                     interpreter_resume, &failed);
    if (dispatcher_boundary) {
        expect_lane_word(test, interp, "interpreter call-boundary PC", interp->state.pc,
                         resume_pc, &failed);
    }
    expect_lane_word(test, interp, "rewritten target marker", interp->state.r[2],
                     0xa034u, &failed);
    expect_lane_word(test, interp, "caller continuation marker", interp->state.r[3],
                     dispatcher_boundary ? 0xa5a50003u : 0xa5a5a037u, &failed);
    if (!failed) {
        fprintf(stderr,
                "  cosim: AOT rejected rewritten $ra at resume 0x%08x; interpreter followed 0x%08x\n",
                resume_pc,
                (uint32_t)COSIM_RA_REWRITE_TARGET);
    }
    return failed;
}

/* ---- cases ---------------------------------------------------------------------- */

#define COSIM_CASE_ROW(name_, address_, words_, description_, omit_) \
    { #name_, address_, words_, description_, omit_, 0u, NULL },

static CosimCase g_cases[] = {
    COSIM_CELL_LIST(COSIM_CASE_ROW)
};

static const unsigned g_case_count = sizeof g_cases / sizeof g_cases[0];

/* Per-cell qualification applied to the generated table above. Everything a cell
 * needs beyond its address lives here, keyed by name, so the generated manifest
 * stays the single source of guest addresses. */
/* The o32 callee-saved-SP asymmetry: generated code closes a callable entry with
 * `s->r[29] = _sp_entry`; the interpreter executes only the instructions present. */
static const char *const SPLEAK_FIELDS[] = { "r29", NULL };
static const CosimKnownDefect SPLEAK_DEFECT = {
    SPLEAK_FIELDS,
    COSIM_TERM_INTERP_HANDOFF,
    "the generated entry epilogue stops assuming an o32 callee-saved $sp",
};

/* The cross-tier RETURNING call carried a declared, self-retiring CosimKnownDefect
 * here until `rt: preserve AOT continuations across interpreter calls` (#127).
 *
 * Generated code emits `dispatch(s, _t);` for a jal/jalr with no host return after
 * it, so the caller's native frame is still live; the interpreter used to stop only
 * at a REGISTERED entry, so the callee's `jr $ra` landed in that caller's interior
 * and the caller's tail ran twice -- double-counting its work and reloading $ra
 * from an already-popped frame. The repair gives the interpreter an explicit call
 * boundary, and lane MIXED now terminates exactly as lane AOT does.
 *
 * The declaration is deleted rather than relaxed, which is the whole point of
 * enforcing one in both directions: this gate went red the moment the fields
 * stopped diverging, and stayed red until the claim was removed. `xcall` is now an
 * ordinary PASS and any regression is an ordinary failure. */

/* The cases the stack census declares, as it declares `spleak` (#1935). `xsibmid` hands
 * its live frame to `xsibpiece`, which returns at the lowered $sp it was entered at, so in
 * guest terms the chain from `xsibmid` comes back 0x20 low. Outside the census the tail
 * record of the body that entered `xsibmid` gives that $sp back; under the census records
 * carry no restore and each wrapper restores after it has recorded the guest's value. So
 * the census counts, in lane AOT, the native `xsibmid` of both cases and, in lane MIXED,
 * the native `xsibtail` that sibcalls the interpreted `xsibmid`. A linked call into the
 * interpreter owes its entry $sp in both builds, so `xsibcall` balances in lane MIXED. */
static const struct {
    const char *name;
    uint64_t mismatches;
} CENSUS_DECLARED_CASES[] = { { "xsibtail", 2u }, { "xsibcall", 1u }, { NULL, 0u } };

static int census_declared_case(const char *name, uint64_t *mismatches) {
    for (unsigned i = 0; CENSUS_DECLARED_CASES[i].name != NULL; i++) {
        if (strcmp(name, CENSUS_DECLARED_CASES[i].name) == 0) {
            *mismatches = CENSUS_DECLARED_CASES[i].mismatches;
            return 1;
        }
    }
    return 0;
}

static void qualify_cases(void) {
    for (unsigned i = 0; i < g_case_count; i++) {
        CosimCase *test = &g_cases[i];
        if (strcmp(test->name, "spleak") == 0) {
            test->known_defect = &SPLEAK_DEFECT;
        }
        if (strcmp(test->name, "fpu_moves") == 0) {
            /* Raw register moves must ignore the guest flush-subnormal mode: run
             * the cell with FCR31.FS set so a lane that flushed would show it. */
            test->fcr31 = 0x01000000u;
        }
    }
}

/* The FPU cell is additionally run once per FCR31 rounding mode, because the
 * whole point of the #120 helper path is that the guest's mode selects the
 * result: a cosim that only ever ran the default mode would not exercise the
 * FCR31 threading it claims to compare. */
static const uint32_t g_fcr31_modes[] = {
    0x00000000u,  /* RN, FS off */
    0x00000001u,  /* RZ */
    0x00000002u,  /* RP */
    0x00000003u,  /* RM */
    0x01000000u,  /* RN with FS (flush) set */
};

/* ---- driver --------------------------------------------------------------------- */

static void load_guest_image(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "cosim: cannot open guest image %s\n", path);
        exit(2);
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fprintf(stderr, "cosim: cannot size guest image %s\n", path);
        exit(2);
    }
    long size = ftell(fp);
    if (size <= 0) {
        fprintf(stderr, "cosim: guest image %s is empty\n", path);
        exit(2);
    }
    rewind(fp);
    unsigned char *bytes = malloc((size_t)size);
    if (!bytes || fread(bytes, 1, (size_t)size, fp) != (size_t)size) {
        fprintf(stderr, "cosim: cannot read guest image %s\n", path);
        exit(2);
    }
    fclose(fp);
    sr_mem_init();
    sr_load_segment(g_image_base, bytes, (uint32_t)size);
    free(bytes);
}

static int run_case(const CosimCase *test, const char *trace_dir) {
    char path_a[512];
    char path_b[512];
    snprintf(path_a, sizeof path_a, "%s/%s_fcr%08x_aot.trace",
             trace_dir, test->name, test->fcr31);
    snprintf(path_b, sizeof path_b, "%s/%s_fcr%08x_%s.trace",
             trace_dir, test->name, test->fcr31,
             test->cross_tier_omit ? "mixed" : "interp");

    static CosimLane aot;
    static CosimLane interp;
    run_lane(&aot, test, COSIM_LANE_AOT, path_a);
    run_lane(&interp, test,
             test->cross_tier_omit ? COSIM_LANE_MIXED : COSIM_LANE_INTERP,
             path_b);

    const int before = g_failures;
    if (strcmp(test->name, "rarewrite") == 0 ||
        strcmp(test->name, "rarewrite-dispatch") == 0) {
        const int dispatcher_boundary =
            strcmp(test->name, "rarewrite-dispatch") == 0;
        const int failed = check_ra_rewrite_contract(
            test, &aot, &interp, (uint32_t)COSIM_RA_REWRITE_RESUME,
            dispatcher_boundary);
        if (g_failures != before) {
            dump_termination(&aot);
            dump_termination(&interp);
            dump_write_log(&aot);
            dump_write_log(&interp);
            dump_cell_program(test);
            fprintf(stderr, "  traces: %s\n          %s\n", path_a, path_b);
        }
        return failed;
    }
    /* Ordered from most localizing to least: a traced instruction names the exact
     * guest PC, a write names the storing PC, a field names what ended up wrong. */
    int failed = 0;
    failed |= check_pc_contract(test, &aot, &interp);
    failed |= compare_traces(test, &aot, &interp);
    failed |= compare_write_logs(test, &aot, &interp);
    failed |= compare_windows(test, &aot, &interp);
    failed |= compare_vectors(test, &aot, &interp);
    if (strcmp(test->name, "jthunk") == 0) {
        const CosimLane *lanes[2] = {&aot, &interp};
        for (unsigned i = 0u; i < 2u; i++) {
            expect_lane_word(test, lanes[i], "callee a0", lanes[i]->state.r[4],
                             COSIM_SCRATCH - 0x20u, &failed);
            expect_lane_word(test, lanes[i], "callee v0", lanes[i]->state.r[2],
                             COSIM_SCRATCH - 0x20u, &failed);
        }
    } else if (strcmp(test->name, "xing_taken") == 0 || strcmp(test->name, "xing_likely") == 0
               || strcmp(test->name, "xing_fall_a") == 0) {
        /* Entry crossings (generate.py): the same result whether the body copies the
         * other entry's code or transfers to its own body. */
        const uint32_t want_v0 = strcmp(test->name, "xing_taken") == 0 ? 0x1103u
            : strcmp(test->name, "xing_likely") == 0 ? 0x1110u : 0x1364u;
        const uint32_t want_v1 = 0xFFFFFFFFu;  /* xing_dst's counter, run down to -1 */
        const CosimLane *lanes[2] = {&aot, &interp};
        for (unsigned i = 0u; i < 2u; i++) {
            expect_lane_word(test, lanes[i], "v0", lanes[i]->state.r[2], want_v0, &failed);
            expect_lane_word(test, lanes[i], "v1", lanes[i]->state.r[3], want_v1, &failed);
        }
    } else if (strcmp(test->name, "jtarget") == 0) {
        expect_lane_word(test, &aot, "v0", aot.state.r[2], COSIM_SCRATCH, &failed);
        expect_lane_word(test, &interp, "v0", interp.state.r[2], COSIM_SCRATCH, &failed);
    } else if (strcmp(test->name, "branch_likely") == 0) {
        static const unsigned annulled_gprs[] = {10u, 11u, 12u, 13u, 14u, 15u, 24u, 25u, 16u};
        const CosimLane *lanes[2] = {&aot, &interp};
        for (unsigned li = 0u; li < 2u; li++) {
            const CosimLane *lane = lanes[li];
            expect_lane_word(test, lane, "v0 not-taken count", lane->state.r[2], 9u, &failed);
            expect_lane_word(test, lane, "v1 branch-target count", lane->state.r[3], 18u, &failed);
            expect_lane_word(test, lane, "taken bc1tl slot f2", lane->state.fi[2],
                             0x40000000u, &failed);
            for (unsigned i = 0u; i < sizeof annulled_gprs / sizeof annulled_gprs[0]; i++) {
                char field[48];
                snprintf(field, sizeof field, "annulled register r%u", annulled_gprs[i]);
                expect_lane_word(test, lane, field, lane->state.r[annulled_gprs[i]], 0u, &failed);
            }
            for (unsigned i = 0u; i < 9u; i++) {
                const uint32_t address = COSIM_SCRATCH + 4u * i;
                const uint32_t want = i == 7u
                    ? (0xdead0000u | (address - COSIM_WINDOW_LO))
                    : 0xC100u + i;
                char field[48];
                snprintf(field, sizeof field, "taken slot memory[%u]", i);
                expect_lane_word(test, lane, field, lane_window_word(lane, address), want, &failed);
            }
        }
    } else if (strcmp(test->name, "branch_likely_int") == 0) {
        static const unsigned annulled_gprs[] = {10u, 11u, 12u, 13u, 14u, 15u, 24u};
        const CosimLane *lanes[2] = {&aot, &interp};
        for (unsigned li = 0u; li < 2u; li++) {
            const CosimLane *lane = lanes[li];
            expect_lane_word(test, lane, "v0 not-taken count", lane->state.r[2], 7u, &failed);
            expect_lane_word(test, lane, "v1 branch-target count", lane->state.r[3], 14u, &failed);
            for (unsigned i = 0u; i < sizeof annulled_gprs / sizeof annulled_gprs[0]; i++) {
                char field[48];
                snprintf(field, sizeof field, "annulled register r%u", annulled_gprs[i]);
                expect_lane_word(test, lane, field, lane->state.r[annulled_gprs[i]], 0u, &failed);
            }
            for (unsigned i = 0u; i < 7u; i++) {
                char field[48];
                snprintf(field, sizeof field, "taken slot memory[%u]", i);
                expect_lane_word(test, lane, field,
                                 lane_window_word(lane, COSIM_SCRATCH + 4u * i), 0xC100u + i,
                                 &failed);
            }
        }
    } else if (strcmp(test->name, "bitops") == 0) {
        static const struct { unsigned reg; uint32_t value; const char *name; } expected[] = {
            {22u, 0x89abcdefu, "ext full word"}, {23u, 1u, "ext bit 31"},
            {24u, 0x44d5e6f7u, "ext 31 bits"}, {25u, 0x00123456u, "ext high field"},
            {12u, 0x02468acfu, "ins edge field"}, {13u, 0xfffff00fu, "ins zero clears field"},
            {16u, 0x0000007fu, "seb +127"}, {17u, 0xffffff80u, "seb -128"},
            {18u, 0xffffffffu, "seb -1"}, {19u, 0x00007fffu, "seh +32767"},
            {20u, 0xffff8000u, "seh -32768"}, {21u, 0xffffffffu, "seh -1"},
            {2u, 0x00001111u, "movn rd==rs"}, {3u, 0x00002222u, "movz rd==rs"},
            {26u, 0x00003333u, "movn rd==rt"}, {27u, 0x00004444u, "movz rd==rt"},
            {0u, 0u, "movn/movz rd==$zero"},
        };
        const CosimLane *lanes[2] = {&aot, &interp};
        for (unsigned li = 0u; li < 2u; li++) {
            const CosimLane *lane = lanes[li];
            for (unsigned i = 0u; i < sizeof expected / sizeof expected[0]; i++) {
                expect_lane_word(test, lane, expected[i].name,
                                 lane->state.r[expected[i].reg], expected[i].value, &failed);
            }
            expect_lane_word(test, lane, "ins full word memory",
                             lane_window_word(lane, COSIM_SCRATCH), 0x01234567u, &failed);
            expect_lane_word(test, lane, "ins bit 31 memory",
                             lane_window_word(lane, COSIM_SCRATCH + 4u), 0xffffffffu, &failed);
            expect_lane_word(test, lane, "unsigned 0x7fffffff to float",
                             lane_window_word(lane, COSIM_SCRATCH + 0x30u), 0x4f000000u, &failed);
            expect_lane_word(test, lane, "unsigned 0x80000000 to float",
                             lane_window_word(lane, COSIM_SCRATCH + 0x34u), 0x4f000000u, &failed);
            expect_lane_word(test, lane, "unsigned 0xffffffff to float",
                             lane_window_word(lane, COSIM_SCRATCH + 0x38u), 0x4f800000u, &failed);
        }
    } else if (strcmp(test->name, "fpu_moves") == 0) {
        static const uint32_t expected_bits[] = {
            0x7f800001u, 0xff812345u, 0x7fc12345u, 0xffc54321u,
            0x00000001u, 0x807fffffu, 0x80000000u, 0xffffffffu,
        };
        static const unsigned moved_regs[] = {9u, 10u, 11u, 12u, 13u, 14u, 15u, 24u};
        static const unsigned loaded_regs[] = {16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u};
        const CosimLane *lanes[2] = {&aot, &interp};
        for (unsigned li = 0u; li < 2u; li++) {
            const CosimLane *lane = lanes[li];
            expect_lane_word(test, lane, "guest FCR31 FS", lane->state.fcr31,
                             0x01000000u, &failed);
            for (unsigned i = 0u; i < sizeof expected_bits / sizeof expected_bits[0]; i++) {
                char field[48];
                snprintf(field, sizeof field, "mfc1 after mov.s r%u", moved_regs[i]);
                expect_lane_word(test, lane, field, lane->state.r[moved_regs[i]],
                                 expected_bits[i], &failed);
                snprintf(field, sizeof field, "mfc1 after lwc1 r%u", loaded_regs[i]);
                expect_lane_word(test, lane, field, lane->state.r[loaded_regs[i]],
                                 expected_bits[i], &failed);
                snprintf(field, sizeof field, "swc1 raw word %u", i);
                expect_lane_word(test, lane, field,
                                 lane_window_word(lane, COSIM_SCRATCH + 4u * i),
                                 expected_bits[i], &failed);
            }
        }
    } else if (strcmp(test->name, "fpu_compare") == 0) {
        const CosimLane *lanes[2] = {&aot, &interp};
        for (unsigned li = 0u; li < 2u; li++) {
            const CosimLane *lane = lanes[li];
            for (unsigned i = 0u; i < 32u; i++) {
                char field[48];
                snprintf(field, sizeof field, "C.cond predicate result %u", i);
                expect_lane_word(test, lane, field,
                                 lane_window_word(lane, COSIM_SCRATCH + 4u * i),
                                 i < 16u ? 1u : 0u, &failed);
            }
            expect_lane_word(test, lane, "FCC0 after final ordered signaling compare",
                             lane->state.fpcond, 0u, &failed);
            expect_lane_word(test, lane, "FCR31 FCC0 after final compare",
                             lane->state.fcr31 & 0x00800000u, 0u, &failed);
        }
    } else if (strcmp(test->name, "xcall") == 0) {
        expect_lane_word(test, &aot, "float result from f12 in f0", aot.state.r[16],
                         aot.state.fi[12], &failed);
        expect_lane_word(test, &interp, "float result from f12 in f0", interp.state.r[16],
                         interp.state.fi[12], &failed);
    }
    if (strcmp(test->name, "unaligned") == 0) {
        static const uint32_t expected_gprs[8] = {
            0x44B2C3D4u, 0x3344C3D4u, 0x223344D4u, 0x11223344u,
            0x55667788u, 0xA1556677u, 0xA1B25566u, 0xA1B2C355u
        };
        uint32_t deliberately_wrong_gprs[8];
        memcpy(deliberately_wrong_gprs, expected_gprs, sizeof deliberately_wrong_gprs);
        deliberately_wrong_gprs[0] ^= 1u;
        if (!unaligned_matches_expected(&aot, expected_gprs)) {
            report_fail(test, "AOT merge outputs do not match the independent byte oracle");
            failed = 1;
        }
        if (!unaligned_matches_expected(&interp, expected_gprs)) {
            report_fail(test, "interpreter merge outputs do not match the independent byte oracle");
            failed = 1;
        }
        if (unaligned_matches_expected(&aot, deliberately_wrong_gprs)) {
            report_fail(test, "positive control accepted a deliberately wrong merge expectation");
            failed = 1;
        } else {
            fprintf(stderr,
                    "  cosim: unaligned merge expectation positive control rejected the wrong value\n");
        }
    }

    if (g_failures != before) {
        dump_termination(&aot);
        dump_termination(&interp);
        dump_write_log(&aot);
        dump_write_log(&interp);
        dump_cell_program(test);
        fprintf(stderr, "  traces: %s\n          %s\n", path_a, path_b);
    }
    return failed;
}

/* ---- independent scalar-FPU reference oracle ------------------------------------ */

typedef struct {
    uint32_t bits;
    uint32_t cvt_source;
} CosimFpuValue;

#define COSIM_FPU_VALUE_ROW(bits_, source_) { bits_, source_ },

static const CosimFpuValue g_fpu_values[] = {
    COSIM_FPU_VALUE_LIST(COSIM_FPU_VALUE_ROW)
};

static const unsigned g_fpu_compare_registers[] = {
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 4, 5, 6, 7, 12, 13
};

static const char *const g_fpu_compare_names[] = {
    "c.f.s", "c.un.s", "c.eq.s", "c.ueq.s", "c.olt.s", "c.ult.s",
    "c.ole.s", "c.ule.s", "c.sf.s", "c.nle.s", "c.seq.s", "c.ngl.s",
    "c.lt.s", "c.nge.s", "c.le.s", "c.ngt.s"
};

static unsigned g_fpu_oracle_checks;
static unsigned g_fpu_oracle_failures;
static unsigned g_fpu_oracle_unknown;
static const char *g_fpu_reported[32];
static unsigned g_fpu_reported_count;

static void fpu_oracle_mismatch(const char *op, uint32_t a, uint32_t b,
                                uint32_t fcr31, uint32_t got, uint32_t want) {
    g_fpu_oracle_failures++;
    for (unsigned i = 0u; i < g_fpu_reported_count; i++) {
        if (strcmp(g_fpu_reported[i], op) == 0) {
            return;
        }
    }
    if (g_fpu_reported_count < sizeof g_fpu_reported / sizeof g_fpu_reported[0]) {
        g_fpu_reported[g_fpu_reported_count++] = op;
    }
    fprintf(stderr,
            "COSIM FPU REFERENCE FAIL op=%s a=0x%08x b=0x%08x "
            "fcr31=0x%08x production=0x%08x reference=0x%08x" NEWLINE,
            op, a, b, fcr31, got, want);
}

static void fpu_oracle_expect(const char *op, uint32_t a, uint32_t b,
                              uint32_t fcr31, uint32_t got, uint32_t want,
                              enum FpuReferenceStatus status) {
    g_fpu_oracle_checks++;
    if (status == COSIM_FPU_REFERENCE_EXPECTED_UNKNOWN) {
        g_fpu_oracle_unknown++;
        return;
    }
    if (status == COSIM_FPU_REFERENCE_INVALID || got != want) {
        fpu_oracle_mismatch(op, a, b, fcr31, got,
                            status == COSIM_FPU_REFERENCE_INVALID ? 0u : want);
    }
}

static void fpu_oracle_expect_compare(const char *op, uint32_t a, uint32_t b,
                                      uint32_t fcr31, uint32_t got, uint32_t want,
                                      enum FpuReferenceStatus status) {
    g_fpu_oracle_checks++;
    if (status == COSIM_FPU_REFERENCE_EXPECTED_UNKNOWN) {
        g_fpu_oracle_unknown++;
    }
    if (status == COSIM_FPU_REFERENCE_INVALID || got != want) {
        fpu_oracle_mismatch(op, a, b, fcr31, got,
                            status == COSIM_FPU_REFERENCE_INVALID ? 0u : want);
    }
}

static int32_t fpu_oracle_s32(uint32_t bits) {
    int32_t value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static float fpu_oracle_float(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static uint32_t fpu_oracle_direct_binary(unsigned op, float a, float b,
                                          uint32_t fcr31) {
    float result;
    switch (op) {
    case COSIM_FPU_REFERENCE_ADD: result = sr_fpu_add_s(a, b, fcr31); break;
    case COSIM_FPU_REFERENCE_SUB: result = sr_fpu_sub_s(a, b, fcr31); break;
    case COSIM_FPU_REFERENCE_MUL: result = sr_fpu_mul_s(a, b, fcr31); break;
    default: result = sr_fpu_div_s(a, b, fcr31); break;
    }
    return sr_float_bits(result);
}

static uint32_t fpu_oracle_direct_unary(unsigned op, float a, uint32_t fcr31) {
    float result;
    switch (op) {
    case COSIM_FPU_REFERENCE_SQRT: result = sqrtf(a); break;
    case COSIM_FPU_REFERENCE_ABS: result = fabsf(a); break;
    case COSIM_FPU_REFERENCE_MOV: result = a; break;
    default: result = -a; break;
    }
    (void)fcr31;
    return sr_float_bits(result);
}

static int run_fpu_reference_oracle(void) {
    static const struct {
        unsigned op;
        unsigned fd;
        const char *name;
    } binary[] = {
        { COSIM_FPU_REFERENCE_ADD, 2u, "add.s" },
        { COSIM_FPU_REFERENCE_SUB, 3u, "sub.s" },
        { COSIM_FPU_REFERENCE_MUL, 4u, "mul.s" },
        { COSIM_FPU_REFERENCE_DIV, 5u, "div.s" },
    };
    static const struct {
        unsigned op;
        unsigned fd;
        const char *name;
    } unary[] = {
        { COSIM_FPU_REFERENCE_SQRT, 6u, "sqrt.s" },
        { COSIM_FPU_REFERENCE_ABS, 7u, "abs.s" },
        { COSIM_FPU_REFERENCE_MOV, 8u, "mov.s" },
        { COSIM_FPU_REFERENCE_NEG, 9u, "neg.s" },
    };
    static const struct {
        unsigned fd;
        unsigned funct;
        const char *name;
    } to_word[] = {
        { 10u, 0x0cu, "round.w.s" },
        { 11u, 0x0du, "trunc.w.s" },
        { 12u, 0x0eu, "ceil.w.s" },
        { 13u, 0x0fu, "floor.w.s" },
        { 14u, 0x24u, "cvt.w.s" },
    };
    const unsigned value_count = sizeof g_fpu_values / sizeof g_fpu_values[0];

    for (unsigned ai = 0u; ai < value_count; ai++) {
        for (unsigned bi = 0u; bi < value_count; bi++) {
            const uint32_t a = g_fpu_values[ai].bits;
            const uint32_t b = g_fpu_values[bi].bits;
            for (unsigned mode = 0u; mode < 4u; mode++) {
                for (unsigned fs = 0u; fs < 2u; fs++) {
                    const uint32_t fcr31 = mode | (fs ? 0x01000000u : 0u);
                    CpuState state;
                    memset(&state, 0, sizeof state);
                    state.r[31] = SR_HOST_RETURN_PC;  /* a host entry's return address (#1375) */
                    state.fi[0] = a;
                    state.fi[1] = b;
                    state.r[8] = g_fpu_values[ai].cvt_source;
                    /* The two ctc1 seeds also carry FCR31 bits 25..31, which the
                     * PSP FPU does not implement (PSP-FPU-CC-001): the guest
                     * writes them and the cell's last ctc1 must drop them. */
                    state.r[14] = fcr31 | 0xfe800000u;
                    state.r[15] = fcr31 | 0xfe000000u;
                    state.fcr31 = fcr31;
                    s_cpu = &state;
                    COSIM_FPU_ORACLE_FN(&state);
                    s_cpu = NULL;

                    for (unsigned op = 0u; op < sizeof binary / sizeof binary[0]; op++) {
                        enum FpuReferenceStatus status;
                        const uint32_t want = fpu_reference_binary(
                            binary[op].op, a, b, fcr31, &status);
                        fpu_oracle_expect(binary[op].name, a, b, fcr31,
                                          state.fi[binary[op].fd], want, status);
                        fpu_oracle_expect("direct", a, b, fcr31,
                                          fpu_oracle_direct_binary(
                                              binary[op].op,
                                              fpu_oracle_float(a),
                                              fpu_oracle_float(b), fcr31),
                                          want, status);
                    }
                    for (unsigned op = 0u; op < sizeof unary / sizeof unary[0]; op++) {
                        enum FpuReferenceStatus status;
                        const uint32_t want = fpu_reference_unary(
                            unary[op].op, a, fcr31, &status);
                        fpu_oracle_expect(unary[op].name, a, 0u, fcr31,
                                          state.fi[unary[op].fd], want, status);
                        fpu_oracle_expect("direct", a, 0u, fcr31,
                                          fpu_oracle_direct_unary(
                                              unary[op].op,
                                              fpu_oracle_float(a), fcr31),
                                          want, status);
                    }
                    for (unsigned op = 0u; op < sizeof to_word / sizeof to_word[0]; op++) {
                        enum FpuReferenceStatus status;
                        const uint32_t want = fpu_reference_to_word(
                            a, to_word[op].funct, fcr31, &status);
                        fpu_oracle_expect(to_word[op].name, a, 0u, fcr31,
                                          state.fi[to_word[op].fd], want, status);
                        fpu_oracle_expect("direct", a, 0u, fcr31,
                                          sr_fpu_to_word(
                                              fpu_oracle_float(a),
                                              to_word[op].funct, fcr31),
                                          want, status);
                    }
                    {
                        enum FpuReferenceStatus status;
                        const uint32_t want = fpu_reference_cvt_s_w(
                            fpu_oracle_s32(g_fpu_values[ai].cvt_source),
                            fcr31, &status);
                        fpu_oracle_expect("cvt.s.w", a, 0u, fcr31,
                                          state.fi[15], want, status);
                        fpu_oracle_expect("direct", a, 0u, fcr31,
                                          sr_float_bits(sr_fpu_cvt_s_w(
                                              fpu_oracle_s32(
                                                  g_fpu_values[ai].cvt_source),
                                              fcr31)), want, status);
                    }
                    for (unsigned condition = 0u; condition < 16u; condition++) {
                        enum FpuReferenceStatus status;
                        const unsigned want = fpu_reference_compare(
                            condition, a, b, &status);
                        const uint32_t captured = state.r[
                            g_fpu_compare_registers[condition]];
                        fpu_oracle_expect_compare(
                            g_fpu_compare_names[condition], a, b, fcr31,
                            (captured >> 23) & 1u, want, status);
                        fpu_oracle_expect_compare(
                            "direct", a, b, fcr31,
                            sr_fpu_condition_s(condition, a, b), want, status);
                        fpu_oracle_expect_compare(
                            "c.cond.s/FCR31", a, b, fcr31,
                            captured, fcr31 | (want << 23), status);
                    }
                    fpu_oracle_expect(
                        "bc1t", a, b, fcr31, state.r[10],
                        fpu_reference_bc1(1u, (state.r[14] >> 23) & 1u)
                            ? 0x11u : 0x22u,
                        COSIM_FPU_REFERENCE_OK);
                    fpu_oracle_expect(
                        "bc1f", a, b, fcr31, state.r[11],
                        fpu_reference_bc1(0u, (state.r[15] >> 23) & 1u)
                            ? 0x33u : 0x44u,
                        COSIM_FPU_REFERENCE_OK);
                    fpu_oracle_expect(
                        "ctc1/FCR31", state.r[15], 0u, fcr31, state.fcr31,
                        fpu_reference_ctc1(state.r[15]), COSIM_FPU_REFERENCE_OK);
                }
            }
        }
    }

    g_failures += (int)g_fpu_oracle_failures;
    fprintf(stderr,
            NEWLINE "cosim: scalar FPU reference oracle: %u checks, "
            "%u EXPECTED_UNKNOWN (#312), %u failures" NEWLINE,
            g_fpu_oracle_checks, g_fpu_oracle_unknown,
            g_fpu_oracle_failures);
    return (int)g_fpu_oracle_failures;
}

/* ---- fail-closed negative corpus ------------------------------------------------ */
/*
 * The comparison above only ever exercises SUCCESSFUL execution. The production
 * interpreter's central claim is the opposite one -- that it refuses rather than
 * fabricates -- and a mutation campaign against the two-lane comparator cannot
 * see that claim at all: every guard below can be deleted while every cell still
 * agrees, because no cell ever reaches one.
 *
 * These are therefore INTERPRETER-TIER assertions, not lane comparisons. Lane AOT
 * is deliberately more permissive (MEM_* absorbs an out-of-range access through
 * sr_oor() instead of stopping), and teaching it to manufacture matching faults
 * purely so the corpus could be expressed as a cosim cell would be fabricating
 * agreement. Each case asserts four things:
 *
 *   1. the exact SrGuestInterpResult classification;
 *   2. the fault record (pc, opcode where valid, address);
 *   3. no illicit guest write -- the whole observed window is byte-compared;
 *   4. no silent continuation -- architectural state changed in exactly the
 *      declared ways and no others.
 */

static uint32_t enc_r(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sh, uint32_t fn) {
    return ((rs & 31u) << 21) | ((rt & 31u) << 16) | ((rd & 31u) << 11) |
           ((sh & 31u) << 6) | (fn & 63u);
}

static uint32_t enc_i(uint32_t op, uint32_t rs, uint32_t rt, uint32_t imm) {
    return ((op & 63u) << 26) | ((rs & 31u) << 21) | ((rt & 31u) << 16) | (imm & 0xffffu);
}

static uint32_t enc_j(uint32_t op, uint32_t target) {
    return ((op & 63u) << 26) | ((target >> 2) & 0x03ffffffu);
}

typedef struct {
    const char *name;
    const char *guard;             /* the fail-closed guard this case stands for */
    uint32_t words[4];
    unsigned word_count;
    uint32_t entry_offset;         /* bytes into negpad, or COSIM_NEGPAD_OFFSET_UNOWNED */
    int expect_result;
    uint32_t expect_fault_pc;      /* relative to negpad unless unowned */
    int expect_opcode_valid;
    unsigned expect_opcode_index;  /* which patched word the fault must name */
    uint32_t expect_address;       /* absolute, or 0 to mean "the faulting pc" */
    const char *allowed_field;     /* the one field this case may legitimately change */
    uint32_t allowed_value;
} CosimNegative;

#define NEG_UNOWNED 0xffffffffu

static int g_negative_failures = 0;

static void negative_fail(const CosimNegative *test, const char *what) {
    g_negative_failures++;
    fprintf(stderr, NEWLINE "COSIM NEGATIVE FAIL case=%s (%s)" NEWLINE "  %s" NEWLINE,
            test->name, test->guard, what);
}

static int run_negative(const CosimNegative *test) {
    char detail[512];

    /* Lane INTERP's installation: executable spans registered, no native bodies,
     * so the interpreter owns these bytes exactly as it owns an AOT gap. */
    install_lane(0);
    seed_window();

    for (unsigned i = 0; i < test->word_count; i++) {
        MEM_W32(COSIM_NEGPAD + test->entry_offset + i * 4u, test->words[i]);
    }

    CpuState state;
    CosimCase seed = { test->name, 0u, 0u, test->guard, 0u, 0u, NULL };
    seed_state(&state, &seed);
    /* $a0 addresses memory that is neither readable nor writable, so the bounded
     * access cases fault on authority rather than on alignment. */
    state.r[4] = COSIM_UNOWNED;

    CosimVector before;
    CosimVector after;
    vector_build(&before, &state);

    unsigned char window_before[COSIM_WINDOW_HI - COSIM_WINDOW_LO];
    for (uint32_t a = COSIM_WINDOW_LO; a < COSIM_WINDOW_HI; a++) {
        window_before[a - COSIM_WINDOW_LO] = MEM_R8(a);
    }

    const uint32_t entry = test->entry_offset == NEG_UNOWNED
        ? (uint32_t)COSIM_UNOWNED
        : (uint32_t)COSIM_NEGPAD + test->entry_offset;

    SrGuestInterpFault fault;
    memset(&fault, 0, sizeof fault);
    s_cpu = &state;
    const SrGuestInterpResult result = sr_guest_interp_run(&state, entry, &fault);
    s_cpu = NULL;

    int failed = 0;

    if ((int)result != test->expect_result) {
        snprintf(detail, sizeof detail,
                 "result was %s (%d), expected %d. A guard that stops rejecting is "
                 "exactly the regression this case exists to catch",
                 sr_guest_interp_result_name(result), (int)result, test->expect_result);
        negative_fail(test, detail);
        failed = 1;
    }

    const uint32_t expect_pc = test->entry_offset == NEG_UNOWNED
        ? (uint32_t)COSIM_UNOWNED
        : (uint32_t)COSIM_NEGPAD + test->expect_fault_pc;
    if (fault.pc != expect_pc) {
        snprintf(detail, sizeof detail, "fault.pc=0x%08x, expected 0x%08x",
                 fault.pc, expect_pc);
        negative_fail(test, detail);
        failed = 1;
    }

    if (fault.opcode_valid != test->expect_opcode_valid) {
        snprintf(detail, sizeof detail, "fault.opcode_valid=%d, expected %d",
                 fault.opcode_valid, test->expect_opcode_valid);
        negative_fail(test, detail);
        failed = 1;
    } else if (test->expect_opcode_valid) {
        const uint32_t want = test->words[test->expect_opcode_index];
        if (fault.opcode != want) {
            snprintf(detail, sizeof detail,
                     "fault.opcode=0x%08x, expected the rejected word 0x%08x",
                     fault.opcode, want);
            negative_fail(test, detail);
            failed = 1;
        }
    }

    const uint32_t expect_address = test->expect_address ? test->expect_address : expect_pc;
    if (fault.address != expect_address) {
        snprintf(detail, sizeof detail, "fault.address=0x%08x, expected 0x%08x",
                 fault.address, expect_address);
        negative_fail(test, detail);
        failed = 1;
    }

    for (uint32_t a = COSIM_WINDOW_LO; a < COSIM_WINDOW_HI; a++) {
        const unsigned char now = MEM_R8(a);
        if (now == window_before[a - COSIM_WINDOW_LO]) {
            continue;
        }
        snprintf(detail, sizeof detail,
                 "a REJECTED sequence wrote guest memory at 0x%08x (0x%02x -> 0x%02x); "
                 "rejection must leave guest state exactly as it was",
                 a, window_before[a - COSIM_WINDOW_LO], now);
        negative_fail(test, detail);
        failed = 1;
        break;
    }

    vector_build(&after, &state);
    for (unsigned i = 0; i < after.count; i++) {
        if (before.fields[i].value == after.fields[i].value) {
            continue;
        }
        const char *name = after.fields[i].name;
        if (test->allowed_field && strcmp(name, test->allowed_field) == 0 &&
            after.fields[i].value == test->allowed_value) {
            continue;
        }
        snprintf(detail, sizeof detail,
                 "architectural field %s changed 0x%08x -> 0x%08x across a REJECTED "
                 "sequence", name, before.fields[i].value, after.fields[i].value);
        negative_fail(test, detail);
        failed = 1;
        break;
    }
    if (test->allowed_field) {
        int seen = 0;
        for (unsigned i = 0; i < after.count; i++) {
            if (strcmp(after.fields[i].name, test->allowed_field) == 0) {
                seen = after.fields[i].value == test->allowed_value;
            }
        }
        if (!seen) {
            snprintf(detail, sizeof detail,
                     "field %s was declared to hold 0x%08x after the rejection and does "
                     "not; the ordering this case pins no longer holds",
                     test->allowed_field, test->allowed_value);
            negative_fail(test, detail);
            failed = 1;
        }
    }

    if (!failed) {
        fprintf(stderr, "  cosim negative OK  %-22s %s" NEWLINE, test->name, test->guard);
    }
    return failed;
}

/* Every control encoding, in a delay slot, one at a time.
 *
 * `control-in-delay-slot` above pins one representative. This sweep pins the
 * whole class, because the property that matters is not "a `j` is rejected" but
 * "no control encoding is ever decoded as arithmetic" -- and the bit fields of a
 * branch overlap real arithmetic forms, so a single representative would leave
 * the interesting encodings untested.
 *
 * NOTE ON LAYERING: deleting the is_control_opcode() guard inside
 * execute_noncontrol() does NOT make this sweep fail, and that is correct rather
 * than a hole. Every control encoding also lacks an arithmetic handler, so both
 * layers independently reject it today. The guard becomes the load-bearing one
 * the moment a primary opcode is implemented that a control encoding shares --
 * which is exactly when this sweep starts distinguishing them. It asserts the
 * PROPERTY, not one implementation of it.
 */
static int run_control_encoding_sweep(void) {
    const struct { const char *name; uint32_t word; } control[] = {
        { "j",      enc_j(0x02u, (uint32_t)COSIM_NEGPAD) },
        { "jal",    enc_j(0x03u, (uint32_t)COSIM_NEGPAD) },
        { "beq",    enc_i(0x04u, 8u, 9u, 1u) },
        { "bne",    enc_i(0x05u, 8u, 9u, 1u) },
        { "blez",   enc_i(0x06u, 8u, 0u, 1u) },
        { "bgtz",   enc_i(0x07u, 8u, 0u, 1u) },
        { "bltz",   enc_i(0x01u, 8u, 0x00u, 1u) },   /* REGIMM */
        { "bgezal", enc_i(0x01u, 8u, 0x11u, 1u) },   /* REGIMM, links */
        { "beql",   enc_i(0x14u, 8u, 9u, 1u) },      /* the likely forms */
        { "bnel",   enc_i(0x15u, 8u, 9u, 1u) },
        { "bc1f",   enc_i(0x11u, 8u, 0u, 1u) },      /* COP1 branch group */
        { "bc2f",   enc_i(0x12u, 8u, 0u, 1u) },      /* COP2 branch group */
        { "jr",     enc_r(8u, 0u, 0u, 0u, 0x08u) },
        { "jalr",   enc_r(8u, 0u, 31u, 0u, 0x09u) },
    };
    const unsigned count = sizeof control / sizeof control[0];
    int failures = 0;

    fprintf(stderr, NEWLINE "cosim: control-in-delay-slot sweep (%u encodings)" NEWLINE,
            count);
    for (unsigned i = 0; i < count; i++) {
        install_lane(0);
        seed_window();
        /* An unconditional `beq $zero, $zero` owns the slot under test, and its
         * taken target is a `break` sentinel. If the slot ever stops being
         * rejected, the run stops AT THE SENTINEL rather than walking the pad --
         * a bounded, attributable failure instead of a hang. */
        MEM_W32(COSIM_NEGPAD + 0u, enc_i(0x04u, 0u, 0u, 1u));
        MEM_W32(COSIM_NEGPAD + 4u, control[i].word);
        MEM_W32(COSIM_NEGPAD + 8u, enc_r(0u, 0u, 0u, 0u, 0x0Du));

        CpuState state;
        CosimCase seed = { "sweep", 0u, 0u, "control sweep", 0u, 0u, NULL };
        seed_state(&state, &seed);

        SrGuestInterpFault fault;
        memset(&fault, 0, sizeof fault);
        s_cpu = &state;
        const SrGuestInterpResult result =
            sr_guest_interp_run(&state, (uint32_t)COSIM_NEGPAD, &fault);
        s_cpu = NULL;

        if (result != SR_GUEST_INTERP_UNSUPPORTED ||
            fault.pc != (uint32_t)COSIM_NEGPAD + 4u ||
            !fault.opcode_valid || fault.opcode != control[i].word) {
            fprintf(stderr,
                    "COSIM SWEEP FAIL %s (0x%08x): result=%s fault.pc=0x%08x "
                    "opcode=0x%08x valid=%d" NEWLINE,
                    control[i].name, control[i].word,
                    sr_guest_interp_result_name(result), fault.pc, fault.opcode,
                    fault.opcode_valid);
            failures++;
        }
    }
    if (failures == 0) {
        fprintf(stderr, "cosim: control sweep OK (%u encodings rejected in a slot)"
                NEWLINE, count);
    }
    return failures;
}

/* `jalr rd, rs` must link into rd, and rd is NOT always $ra.
 *
 * The two-lane comparison cannot reach this: generated code models jal/jalr as a
 * host CALL, so a callee entered through a link register other than $ra cannot
 * return coherently, and no cosim cell can exercise it without faking the frame
 * model. It is still a real decode property of the interpreter, so it is asserted
 * here at the interpreter tier -- which is what closes the gap the two-lane
 * comparison structurally cannot.
 */
static int run_link_register_shape(void) {
    install_lane(0);
    seed_window();
    /* jalr $t1, $t0 with $t0 = the registered trampoline. A linked transfer into a
     * registered body is a CALL, as in generated code: the trampoline runs once and
     * the interpreter resumes at the link address, where the break sentinel stops
     * the run fail-closed. */
    MEM_W32(COSIM_NEGPAD + 0u, enc_r(8u, 0u, 9u, 0u, 0x09u));
    MEM_W32(COSIM_NEGPAD + 4u, 0u);
    MEM_W32(COSIM_NEGPAD + 8u, enc_r(0u, 0u, 0u, 0u, 0x0Du));   /* break sentinel */

    CpuState state;
    CosimCase seed = { "linkshape", 0u, 0u, "jalr link register", 0u, 0u, NULL };
    seed_state(&state, &seed);
    const uint32_t seeded_ra = state.r[31];
    state.r[8] = (uint32_t)COSIM_RETURN;

    SrGuestInterpFault fault;
    memset(&fault, 0, sizeof fault);
    g_trampoline_hits = 0;
    s_cpu = &state;
    const SrGuestInterpResult result =
        sr_guest_interp_run(&state, (uint32_t)COSIM_NEGPAD, &fault);
    s_cpu = NULL;

    int failures = 0;
    if (g_trampoline_hits != 1) {
        fprintf(stderr, "COSIM LINKSHAPE FAIL: the registered callee ran %d time(s), "
                "expected exactly once" NEWLINE, g_trampoline_hits);
        failures++;
    }
    if (result != SR_GUEST_INTERP_UNSUPPORTED ||
        fault.pc != (uint32_t)COSIM_NEGPAD + 8u) {
        fprintf(stderr, "COSIM LINKSHAPE FAIL: result=%s at 0x%08x, expected the break "
                "sentinel at the link address 0x%08x -- the call did not resume its "
                "interpreted caller" NEWLINE,
                sr_guest_interp_result_name(result), fault.pc,
                (uint32_t)COSIM_NEGPAD + 8u);
        failures++;
    }
    if (state.r[9] != (uint32_t)COSIM_NEGPAD + 8u) {
        fprintf(stderr,
                "COSIM LINKSHAPE FAIL: $t1 = 0x%08x, expected the link 0x%08x -- the "
                "rd field of jalr is not being honoured" NEWLINE,
                state.r[9], (uint32_t)COSIM_NEGPAD + 8u);
        failures++;
    }
    if (state.r[31] != seeded_ra) {
        fprintf(stderr,
                "COSIM LINKSHAPE FAIL: $ra = 0x%08x, expected the seed 0x%08x -- the "
                "link was written to $ra rather than to rd" NEWLINE,
                state.r[31], seeded_ra);
        failures++;
    }
    if (failures == 0) {
        fprintf(stderr, "cosim: jalr link-register shape OK ($t1 linked, $ra untouched, "
                "caller resumed at the link)" NEWLINE);
    }
    return failures;
}

/* ---- behavioral form census ----------------------------------------------------- */
/*
 * "Every form the interpreter implements exists because a cell executes it" was
 * previously enforced from ONE side: a hand-maintained list in
 * tools/test_cosim_fixture.py describing the fixture. Nothing checked the
 * interpreter, so an opcode added with no cell behind it would have gone
 * unnoticed -- which is exactly the speculative coverage the claim rules out.
 *
 * This asks the production interpreter directly instead of parsing it. Each
 * candidate encoding is executed as a real instruction; a form the interpreter
 * does not decode fails closed as SR_GUEST_INTERP_UNSUPPORTED *at that pc*, and
 * anything else means it was decoded (a decoded form may still reject its
 * operands -- a bad address is a memory fault, not an unknown opcode).
 */
typedef enum { FORM_PRIMARY, FORM_SPECIAL, FORM_COP1, FORM_COP1_BRANCH } CosimFormKind;

static uint32_t form_probe_word(CosimFormKind kind, uint32_t a, uint32_t b) {
    switch (kind) {
    case FORM_SPECIAL:
        /* Shape 0: the three-operand ALU shape ($t0, $t1 -> $t2).
         * Shape 1: the transfer shape, rt and rd zero.
         *
         * Both are needed. `jr`/`jalr` require the unused fields to be zero and
         * correctly REFUSE shape 0, so probing only that shape would report the
         * fixture's own jr/jalr as undecoded. The question this census asks is
         * "is this form decoded at all", so a form counts as decoded if any
         * well-formed shape of it is accepted.
         * Shape 2: the shift shape, rs zero and a shift amount. A shift's rs field
         * is not an operand: in srl, rs=1 selects the Allegrex rotate and any
         * other nonzero rs is reserved, so shapes 0 and 1 are malformed for it. */
        if (b == 2u) return enc_r(0u, 9u, 10u, 3u, a);
        return b == 0u ? enc_r(8u, 9u, 10u, 0u, a) : enc_r(8u, 0u, 0u, 0u, a);
    case FORM_COP1:
        /* fmt in rs, funct in the low bits, ft/fs/fd all f0. */
        return (0x11u << 26) | ((a & 31u) << 21) | (b & 63u);
    case FORM_COP1_BRANCH:
        /* The low 16 bits are a branch displacement, not a function field. */
        return enc_i(0x11u, 8u, 0u, 1u); /* bc1f +4 */
    default:
        if (a == 0x01u) return enc_i(a, 8u, 2u, 1u); /* bltzl */
        if (a == 0x06u || a == 0x07u || a == 0x16u || a == 0x17u)
            return enc_i(a, 8u, 0u, 1u);          /* blez/bgtz, including likely */
        /* rs = $a0 (the seeded scratch pointer) so a load/store form addresses
         * memory that exists and cannot be refused for its operand. */
        return enc_i(a, 4u, 8u, 0u);
    }
}

static int interpreter_decodes(CosimFormKind kind, uint32_t a, uint32_t b) {
    install_lane(0);
    seed_window();
    MEM_W32(COSIM_NEGPAD + 0u, form_probe_word(kind, a, b));
    MEM_W32(COSIM_NEGPAD + 4u, 0u);                       /* benign delay slot */
    MEM_W32(COSIM_NEGPAD + 8u, enc_r(0u, 0u, 0u, 0u, 0x0Du));  /* break: bound the run */

    CpuState state;
    CosimCase seed = { "census", 0u, 0u, "form census", 0u, 0u, NULL };
    seed_state(&state, &seed);
    state.r[4] = COSIM_SCRATCH;

    SrGuestInterpFault fault;
    memset(&fault, 0, sizeof fault);
    s_cpu = &state;
    const SrGuestInterpResult result =
        sr_guest_interp_run(&state, (uint32_t)COSIM_NEGPAD, &fault);
    s_cpu = NULL;

    /* Refused AT the probe word => not decoded. Anything else -- including a
     * fault raised further along, or on this word's operands -- means the form
     * itself was recognised. */
    return !(result == SR_GUEST_INTERP_UNSUPPORTED &&
             fault.pc == (uint32_t)COSIM_NEGPAD);
}

/* A SPECIAL form counts as decoded if any of its encoding shapes is accepted. */
static int form_is_decoded(CosimFormKind kind, uint32_t a, uint32_t b) {
    if (interpreter_decodes(kind, a, b)) {
        return 1;
    }
    return kind == FORM_SPECIAL &&
           (interpreter_decodes(kind, a, 1u) || interpreter_decodes(kind, a, 2u));
}

typedef struct {
    CosimFormKind kind;
    uint32_t a;
    uint32_t b;
    const char *label;
} CosimForm;

#define COSIM_FORM_ROW(kind_, a_, b_, label_) { FORM_##kind_, a_, b_, label_ },

static int run_form_census(void) {
    static const CosimForm required[] = { COSIM_FORM_LIST(COSIM_FORM_ROW) };
    const unsigned required_count = sizeof required / sizeof required[0];
    int failures = 0;

    fprintf(stderr, NEWLINE "cosim: interpreter form census (%u fixture forms)" NEWLINE,
            required_count);

    /* Direction 1: every form a cell executes must be decoded. */
    for (unsigned i = 0; i < required_count; i++) {
        if (form_is_decoded(required[i].kind, required[i].a, required[i].b)) {
            continue;
        }
        fprintf(stderr,
                "COSIM CENSUS FAIL: the fixture executes %s but the interpreter does "
                "not decode it" NEWLINE, required[i].label);
        failures++;
    }

    /* Direction 2: nothing else is decoded. This is the half that was previously
     * unchecked, and it is what keeps "no speculative opcodes" true. */
    unsigned extra = 0;
    for (uint32_t primary = 0u; primary < 64u; primary++) {
        if (primary == 0x00u || primary == 0x11u) {
            continue;   /* enumerated by their own sub-spaces below */
        }
        if (!interpreter_decodes(FORM_PRIMARY, primary, 0u)) {
            continue;
        }
        int declared = 0;
        for (unsigned i = 0; i < required_count; i++) {
            if (required[i].kind == FORM_PRIMARY && required[i].a == primary) {
                declared = 1;
            }
        }
        if (!declared) {
            fprintf(stderr,
                    "COSIM CENSUS FAIL: the interpreter decodes primary 0x%02x, which "
                    "NO fixture cell executes" NEWLINE, primary);
            extra++;
        }
    }
    for (uint32_t funct = 0u; funct < 64u; funct++) {
        if (!form_is_decoded(FORM_SPECIAL, funct, 0u)) {
            continue;
        }
        int declared = 0;
        for (unsigned i = 0; i < required_count; i++) {
            if (required[i].kind == FORM_SPECIAL && required[i].a == funct) {
                declared = 1;
            }
        }
        if (!declared) {
            fprintf(stderr,
                    "COSIM CENSUS FAIL: the interpreter decodes SPECIAL funct 0x%02x, "
                    "which NO fixture cell executes" NEWLINE, funct);
            extra++;
        }
    }
    if (interpreter_decodes(FORM_COP1_BRANCH, 0u, 0u)) {
        int declared = 0;
        for (unsigned i = 0; i < required_count; i++) {
            if (required[i].kind == FORM_COP1_BRANCH) declared = 1;
        }
        if (!declared) {
            fprintf(stderr,
                    "COSIM CENSUS FAIL: the interpreter decodes COP1 branch instructions, "
                    "which NO fixture cell executes" NEWLINE);
            extra++;
        }
    }
    for (uint32_t fmt = 0u; fmt < 32u; fmt++) {
        if (fmt == 8u) continue; /* COP1 branch displacement bits are not funct values. */
        for (uint32_t funct = 0u; funct < 64u; funct++) {
            if (!interpreter_decodes(FORM_COP1, fmt, funct)) {
                continue;
            }
            int declared = 0;
            for (unsigned i = 0; i < required_count; i++) {
                if (required[i].kind == FORM_COP1 && required[i].a == fmt &&
                    required[i].b == funct) {
                    declared = 1;
                }
            }
            if (!declared) {
                fprintf(stderr,
                        "COSIM CENSUS FAIL: the interpreter decodes COP1 fmt=0x%02x "
                        "funct=0x%02x, which NO fixture cell executes" NEWLINE,
                        fmt, funct);
                extra++;
            }
        }
    }

    failures += (int)extra;
    if (failures == 0) {
        fprintf(stderr,
                "cosim: form census OK (%u forms, decoded set == executed set)" NEWLINE,
                required_count);
    }
    return failures;
}

static int run_negative_corpus(void) {
    /* $t0 = 8, $a0 = COSIM_UNOWNED (seeded above). */
    const CosimNegative cases[] = {
        {
            "store-authority", "sr_guest_span_writable() before any store commits",
            /* `break` trails every bounded-access case: if the guard under test
             * ever stops firing, execution stops on the very next word instead of
             * wandering through the pad and reporting some unrelated symptom. */
            { enc_i(0x2Bu, 4u, 8u, 0u), enc_r(0u, 0u, 0u, 0u, 0x0Du) }, 2u, 0u,
            SR_GUEST_INTERP_MEMORY_FAULT, 0u, 1, 0u, (uint32_t)COSIM_UNOWNED, NULL, 0u,
        },
        {
            "load-authority", "sr_guest_span_readable() before any load commits",
            { enc_i(0x23u, 4u, 8u, 0u), enc_r(0u, 0u, 0u, 0u, 0x0Du) }, 2u, 0u,
            SR_GUEST_INTERP_MEMORY_FAULT, 0u, 1, 0u, (uint32_t)COSIM_UNOWNED, NULL, 0u,
        },
        {
            "misaligned-data", "alignment is checked before bounds and before effect",
            /* Inside the window, so ONLY the alignment rule can reject it. */
            { enc_i(0x0Fu, 0u, 4u, (uint32_t)COSIM_SCRATCH >> 16),
              enc_i(0x0Du, 4u, 4u, (uint32_t)COSIM_SCRATCH & 0xffffu),
              enc_i(0x23u, 4u, 8u, 2u) }, 3u, 0u,
            SR_GUEST_INTERP_MISALIGNED_DATA, 8u, 1, 2u,
            (uint32_t)COSIM_SCRATCH + 2u, "r4", (uint32_t)COSIM_SCRATCH,
        },
        {
            "control-in-delay-slot", "a transfer is never decoded as arithmetic",
            /* jal, with a `j` in its delay slot. The link write happens at the
             * transfer and is architecturally correct; the slot is then refused.
             *
             * BOTH transfers name the registered trampoline, never this pad. A
             * negative program has to terminate even when the guard it tests is
             * DELETED -- the interpreter deliberately has no instruction cap, so a
             * transfer back into the pad would turn a removed guard into a hang
             * instead of a failure. (Measured: it did, under `skip-delay-slot`.) */
            { enc_j(0x03u, (uint32_t)COSIM_RETURN), enc_j(0x02u, (uint32_t)COSIM_RETURN) },
            2u, 0u,
            SR_GUEST_INTERP_UNSUPPORTED, 4u, 1, 1u, 0u,
            "r31", (uint32_t)COSIM_NEGPAD + 8u,
        },
        {
            "exec-span-authority", "membership in a registered executable span",
            { 0u }, 0u, NEG_UNOWNED,
            SR_GUEST_INTERP_NOT_EXECUTABLE, 0u, 0, 0u, 0u, NULL, 0u,
        },
        {
            "strict-jr-encoding", "jr with a non-zero hint/rd field is refused",
            { enc_r(8u, 0u, 1u, 0u, 0x08u) }, 1u, 0u,
            SR_GUEST_INTERP_UNSUPPORTED, 0u, 1, 0u, 0u, NULL, 0u,
        },
        {
            "strict-jalr-encoding", "jalr with a non-zero rt field is refused",
            { enc_r(8u, 1u, 31u, 0u, 0x09u) }, 1u, 0u,
            SR_GUEST_INTERP_UNSUPPORTED, 0u, 1, 0u, 0u, NULL, 0u,
        },
        {
            "misaligned-pc", "an unaligned entry is refused before any fetch",
            { 0u }, 0u, 2u,
            SR_GUEST_INTERP_MISALIGNED_PC, 2u, 0, 0u, 0u, NULL, 0u,
        },
    };

    const unsigned count = sizeof cases / sizeof cases[0];
    fprintf(stderr, NEWLINE "cosim: fail-closed negative corpus (%u cases)" NEWLINE, count);
    for (unsigned i = 0; i < count; i++) {
        run_negative(&cases[i]);
    }
    if (g_negative_failures == 0) {
        fprintf(stderr, "cosim: negative corpus OK (%u cases)" NEWLINE, count);
    }
    g_negative_failures += run_control_encoding_sweep();
    g_negative_failures += run_link_register_shape();
    g_negative_failures += run_form_census();
    return g_negative_failures;
}

/* ---- resume at a PC (#2172) ----------------------------------------------------- */
/*
 * "Resume at a PC" in docs/ARCHITECTURE.md. A thread parks at a yield point, its frames are
 * described from the return-boundary records (sr_resume_describe), and a run that starts from
 * nothing but the parked registers, the parked memory and that frame list
 * (dispatch_resume_from_host) must end exactly where the uninterrupted run ended.
 *
 * The harness's sr_yield is the park. A thread is parked once sr_yield's own switch has
 * switched it out, so the hook does what that switch does to the call records: it swaps them
 * out of the running side (sr_aot_return_boundary_stack_swap, as sched_run does around every
 * fiber switch), describes the swapped-out descriptor and swaps it back. It copies the rest of
 * the state the way a save state would and returns, so one run is both the uninterrupted
 * reference and the source of the snapshot.
 * Every yield point the run passes is tried in turn, by starting the slice counter at its
 * ordinal. The resumed run begins with an empty return-boundary stack and a rebuilt dispatch
 * table: no host frame of the first run exists in it.
 *
 * What is compared: the whole CpuState, pc included (both runs are generated code, so
 * unlike the two-lane comparison above no field is lane-specific); the memory window; the
 * guest writes after the park; the instruction trace after the park, which the reference run
 * writes to a file of its own from the park on, so the two traces must be equal line for
 * line and in length; and the slice counter, so a resume that counted a yield point the
 * uninterrupted run did not, or skipped one, fails.
 *
 * A resumed thread must also be a thread like any other. Each resumed run is repeated with the
 * slice counter at 1, so it parks again at the very next yield point, and that second park --
 * registers, memory, description and frame list -- must equal the second park of an
 * uninterrupted run that parks at the same two points. That is the check on the rebuilt call
 * records themselves. (The reference has to park twice as well: a park writes s->pc, so a run
 * that parked only at the second point is a different run.)
 */

#define COSIM_RESUME_FRAMES 8u
#define COSIM_RESUME_SLICE 0x100000

typedef struct {
    int armed;
    int fired;
    CpuState state;
    unsigned char window[COSIM_WINDOW_HI - COSIM_WINDOW_LO];
    unsigned writes_before;
    SrResumeStatus status;
    char detail[200];
    SrResumeFrame frames[COSIM_RESUME_FRAMES];
    uint32_t frame_count;
    /* Refusals staged at the live park (g_resume_stage_refusals). */
    SrResumeStatus foreign_state, short_capacity, scope_live, inside_yield, after_interrupt;
} CosimPark;

static CosimPark g_park;
/* With g_resume_park_twice, the yield point after the park parks too, into g_park_next. */
static CosimPark g_park_next;
static int g_resume_park_twice;
/* Set for the `rs` sweep: each park also asks for five refusals only a live thread can show. */
static int g_resume_stage_refusals;
/* Set for an uninterrupted run: from its park on, the trace goes to this file. */
static const char *g_resume_after_park_trace;
static uint32_t g_resume_trace_entry;

/* Describe the fiber as the scheduler would after sr_yield's switch: with its call records
 * swapped out of the running side. */
static SrResumeStatus resume_describe_switched_out(const CpuState *s, SrResumeFrame *frames,
                                                   uint32_t capacity, uint32_t *count,
                                                   char *detail, size_t detail_cap) {
    SrAotReturnBoundaryStack parked;
    memset(&parked, 0, sizeof parked);
    sr_aot_return_boundary_stack_swap(&parked);
    const SrResumeStatus status =
        sr_resume_describe(&parked, s, frames, capacity, count, detail, detail_cap);
    sr_aot_return_boundary_stack_swap(&parked);
    return status;
}
static CosimLane g_resume_ref, g_resume_res;
static int g_resume_failures = 0;

static void resume_fail(const char *name, unsigned ordinal, const char *what) {
    g_resume_failures++;
    fprintf(stderr, NEWLINE "COSIM DIVERGENCE cell=%s (resume at a PC, yield point %u)" NEWLINE
            "  %s" NEWLINE, name, ordinal, what);
}

static void resume_park_capture(CosimPark *park, CpuState *s) {
    park->armed = 0;
    park->fired = 1;
    park->state = *s;
    for (uint32_t address = COSIM_WINDOW_LO; address < COSIM_WINDOW_HI; address++) {
        park->window[address - COSIM_WINDOW_LO] = MEM_R8(address);
    }
    park->writes_before = g_active_write_log ? g_active_write_log->count : 0u;
    park->status = resume_describe_switched_out(s, park->frames, COSIM_RESUME_FRAMES,
                                                &park->frame_count, park->detail,
                                                sizeof park->detail);
}

static void resume_park_hook(CpuState *s) {
    /* A scheduler hands out a fresh slice; nothing else may fire in this run. */
    atomic_store_explicit(&sr_timeslice, COSIM_RESUME_SLICE, memory_order_relaxed);
    if (!g_park.armed) {
        if (g_park_next.armed) resume_park_capture(&g_park_next, s);
        return;
    }
    resume_park_capture(&g_park, s);
    if (g_resume_stage_refusals) {
        SrResumeFrame scratch[COSIM_RESUME_FRAMES];
        uint32_t scratch_count = 0u;
        CpuState other = *s;
        g_park.foreign_state = resume_describe_switched_out(&other, scratch, COSIM_RESUME_FRAMES,
                                                            &scratch_count, NULL, 0u);
        g_park.short_capacity =
            resume_describe_switched_out(s, scratch, 1u, &scratch_count, NULL, 0u);
        SrAotReturnBoundaryScope scope;
        sr_aot_return_boundary_scope_begin(&scope);
        g_park.scope_live = resume_describe_switched_out(s, scratch, COSIM_RESUME_FRAMES,
                                                         &scratch_count, NULL, 0u);
        (void)sr_aot_return_boundary_scope_end(s, &scope, "cosim resume test");
        /* Inside the yield, before any switch: the body has named its point, and the thread
         * is still the running one. A capture here would skip the rest of the yield. */
        g_park.inside_yield = sr_resume_describe(NULL, s, scratch, COSIM_RESUME_FRAMES,
                                                 &scratch_count, NULL, 0u);
        /* The yield services an interrupt, as sr_yield does before it decides to switch: a
         * handler runs in a scope on this fiber and returns. The thread is still running. */
        {
            const CpuState saved = *s;
            sr_aot_return_boundary_scope_begin(&scope);
            s->r[31] = SR_HOST_RETURN_PC;   /* a host entry's own link */
            dispatch_from_host(s, COSIM_LEAF_A);
            (void)sr_aot_return_boundary_scope_end(s, &scope, "cosim resume test");
            *s = saved;
        }
        g_park.after_interrupt = sr_resume_describe(NULL, s, scratch, COSIM_RESUME_FRAMES,
                                                    &scratch_count, NULL, 0u);
        atomic_store_explicit(&sr_timeslice, COSIM_RESUME_SLICE, memory_order_relaxed);
    }
    if (g_resume_park_twice) {
        g_park_next.armed = 1;
        atomic_store_explicit(&sr_timeslice, 1, memory_order_relaxed);
    }
    if (g_resume_after_park_trace != NULL) {
        sr_trace_close();
        if (sr_trace_open(g_resume_after_park_trace, "cosim", g_resume_trace_entry) != 0) {
            fprintf(stderr, "cosim: cannot open trace file %s\n", g_resume_after_park_trace);
            exit(2);
        }
    }
}

/* With the scheduler on, the dispatcher and every body that makes a direct call bound the
 * host stack through the scheduler's hook. The harness runs on the process stack, not a
 * fiber, so it answers with room these few frames cannot exhaust. */
static int resume_host_stack_remaining(size_t *remaining_out) {
    *remaining_out = (size_t)1u << 20;
    return 1;
}

static void resume_lane_begin(CosimLane *lane, const char *trace_path, uint32_t entry) {
    memset(&lane->writes, 0, sizeof lane->writes);
    s_cpu = &lane->state;
    sr_aot_return_boundaries_reset();
    sr_dispatch_host_stack_remaining = resume_host_stack_remaining;
    sr_sched_on = 1;
    g_cosim_yield_hook = resume_park_hook;
    if (sr_trace_open(trace_path, "cosim", entry) != 0) {
        fprintf(stderr, "cosim: cannot open trace file %s\n", trace_path);
        exit(2);
    }
    g_active_write_log = &lane->writes;
    g_sr_last_writer_enabled = 1;
}

static int32_t resume_lane_end(CosimLane *lane, const char *trace_path) {
    g_sr_last_writer_enabled = 0;
    g_active_write_log = NULL;
    sr_trace_close();
    g_cosim_yield_hook = NULL;
    sr_sched_on = 0;
    sr_dispatch_host_stack_remaining = NULL;
    sr_host_stack_window_clear();
    s_cpu = NULL;
    for (uint32_t address = COSIM_WINDOW_LO; address < COSIM_WINDOW_HI; address++) {
        lane->window[address - COSIM_WINDOW_LO] = MEM_R8(address);
    }
    trace_load(&lane->trace, trace_path);
    return (int32_t)atomic_load_explicit(&sr_timeslice, memory_order_relaxed);
}

/* The uninterrupted run, entered as a thread's entry is. `park_at` is the ordinal of the
 * yield point that parks (0: none); `omit` is a body left to the interpreter (0: none).
 * Returns the slice counter the run ended with. */
static int32_t resume_run_uninterrupted(uint32_t entry, uint32_t omit, uint32_t raise_to,
                                        int park_at, const char *trace_path) {
    static const CosimCase plain = { "resume", 0u, 0u, "", 0u, 0u, NULL };
    CosimLane *lane = &g_resume_ref;
    if (omit != 0u) install_lane_mixed(omit);
    else install_lane(COSIM_LANE_AOT);
    g_trampoline_raise = raise_to;
    seed_window();
    seed_state(&lane->state, &plain);
    lane->state.r[31] = SR_HOST_RETURN_PC;
    memset(&g_park, 0, sizeof g_park);
    memset(&g_park_next, 0, sizeof g_park_next);
    g_park.armed = park_at > 0;
    atomic_store_explicit(&sr_timeslice, park_at > 0 ? park_at : COSIM_RESUME_SLICE,
                          memory_order_relaxed);
    static char after_park[560];
    snprintf(after_park, sizeof after_park, "%s.after-park", trace_path);
    g_resume_after_park_trace = after_park;
    g_resume_trace_entry = entry;
    resume_lane_begin(lane, trace_path, entry);
    dispatch_from_host(&lane->state, entry);
    g_resume_after_park_trace = NULL;
    /* The reference trace is what the run wrote after its park. */
    return resume_lane_end(lane, g_park.fired ? after_park : trace_path);
}

/* A run that starts from the snapshot alone: a rebuilt dispatch table, an empty
 * return-boundary stack, the parked registers and memory, and the frame list. With `repark`
 * its first yield point parks again, into g_park. */
static SrResumeStatus resume_run_resumed(const CosimPark *from, const SrResumeFrame *frames,
                                         uint32_t count, int repark, const char *trace_path,
                                         int32_t *slice_left, char *detail, size_t detail_cap) {
    CosimLane *lane = &g_resume_res;
    install_lane(COSIM_LANE_AOT);
    for (uint32_t address = COSIM_WINDOW_LO; address < COSIM_WINDOW_HI; address++) {
        MEM_W8(address, from->window[address - COSIM_WINDOW_LO]);
    }
    lane->state = from->state;
    if (repark) {
        memset(&g_park, 0, sizeof g_park);
        g_park.armed = 1;
    }
    atomic_store_explicit(&sr_timeslice, repark ? 1 : COSIM_RESUME_SLICE, memory_order_relaxed);
    resume_lane_begin(lane, trace_path, count ? frames[0].body : 0u);
    const SrResumeStatus status =
        dispatch_resume_from_host(&lane->state, frames, count, detail, detail_cap);
    *slice_left = resume_lane_end(lane, trace_path);
    return status;
}

static int resume_stack_is_empty(const CpuState *s) {
    SrResumeFrame frames[1];
    uint32_t count = 0u;
    return sr_resume_describe(NULL, s, frames, 1u, &count, NULL, 0u) == SR_RESUME_NO_FRAMES;
}

/* The second park of an uninterrupted run that parked at two consecutive yield points, against
 * the park a run resumed from the first reached at its own first yield point. */
static void resume_compare_repark(const char *name, unsigned ordinal, const CosimPark *from,
                                  const CosimPark *reference, const CosimPark *again) {
    char detail[560];
    if (again->fired != reference->fired) {
        resume_fail(name, ordinal, again->fired
                    ? "the resumed run met a yield point the uninterrupted run did not"
                    : "the resumed run met no yield point; the uninterrupted run met one");
        return;
    }
    if (!again->fired) return;
    if (again->status != reference->status || again->frame_count != reference->frame_count ||
        memcmp(again->frames, reference->frames,
               reference->frame_count * sizeof reference->frames[0]) != 0) {
        snprintf(detail, sizeof detail,
                 "the resumed thread parked again and is described differently: uninterrupted "
                 "%s with %u frame(s), resumed %s with %u frame(s) (%s)",
                 sr_resume_status_name(reference->status), reference->frame_count,
                 sr_resume_status_name(again->status), again->frame_count, again->detail);
        resume_fail(name, ordinal, detail);
    }
    if (memcmp(&again->state, &reference->state, sizeof reference->state) != 0) {
        const uint32_t *a = (const uint32_t *)(const void *)&reference->state;
        const uint32_t *b = (const uint32_t *)(const void *)&again->state;
        size_t word = 0;
        while (word < sizeof reference->state / 4u && a[word] == b[word]) word++;
        snprintf(detail, sizeof detail,
                 "the resumed thread parked again with other registers: CpuState byte offset "
                 "%zu is 0x%08x uninterrupted, 0x%08x resumed", word * 4u, a[word], b[word]);
        resume_fail(name, ordinal, detail);
    }
    if (memcmp(again->window, reference->window, sizeof reference->window) != 0) {
        resume_fail(name, ordinal, "the resumed thread parked again with other memory");
    }
    if (reference->writes_before < from->writes_before ||
        reference->writes_before - from->writes_before != again->writes_before) {
        snprintf(detail, sizeof detail,
                 "guest writes between the two parks: uninterrupted %u, resumed %u",
                 reference->writes_before - from->writes_before, again->writes_before);
        resume_fail(name, ordinal, detail);
    }
}

static void resume_compare(const CosimPark *from, const char *name, unsigned ordinal,
                           int32_t ref_slice, int32_t res_slice) {
    const CosimLane *ref = &g_resume_ref;
    const CosimLane *res = &g_resume_res;
    char detail[320];
    if (memcmp(&ref->state, &res->state, sizeof ref->state) != 0) {
        const uint32_t *a = (const uint32_t *)(const void *)&ref->state;
        const uint32_t *b = (const uint32_t *)(const void *)&res->state;
        size_t word = 0;
        while (word < sizeof ref->state / 4u && a[word] == b[word]) word++;
        snprintf(detail, sizeof detail,
                 "CpuState differs at byte offset %zu: uninterrupted 0x%08x, resumed 0x%08x "
                 "(r2 0x%08x/0x%08x, r29 0x%08x/0x%08x, pc 0x%08x/0x%08x)",
                 word * 4u, a[word], b[word], ref->state.r[2], res->state.r[2],
                 ref->state.r[29], res->state.r[29], ref->state.pc, res->state.pc);
        resume_fail(name, ordinal, detail);
    }
    for (uint32_t i = 0; i < COSIM_WINDOW_HI - COSIM_WINDOW_LO; i++) {
        if (ref->window[i] != res->window[i]) {
            snprintf(detail, sizeof detail,
                     "memory differs at 0x%08x: uninterrupted 0x%02x, resumed 0x%02x",
                     COSIM_WINDOW_LO + i, ref->window[i], res->window[i]);
            resume_fail(name, ordinal, detail);
            break;
        }
    }
    if (ref->writes.overflow || res->writes.overflow ||
        ref->writes.count < from->writes_before ||
        ref->writes.count - from->writes_before != res->writes.count) {
        snprintf(detail, sizeof detail,
                 "guest writes after the park: uninterrupted %u (of %u), resumed %u",
                 ref->writes.count - from->writes_before, ref->writes.count, res->writes.count);
        resume_fail(name, ordinal, detail);
    } else {
        for (unsigned i = 0; i < res->writes.count; i++) {
            const CosimWrite *a = &ref->writes.records[from->writes_before + i];
            const CosimWrite *b = &res->writes.records[i];
            if (memcmp(a, b, sizeof *a) != 0) {
                snprintf(detail, sizeof detail,
                         "guest write %u after the park differs: uninterrupted pc=0x%08x "
                         "[0x%08x]=0x%08x, resumed pc=0x%08x [0x%08x]=0x%08x",
                         i, a->pc, a->address, a->after, b->pc, b->address, b->after);
                resume_fail(name, ordinal, detail);
                break;
            }
        }
    }
    /* Both traces start at the park, so they are equal in length and line for line, each
     * line's ordinal included. */
    if (ref->trace.truncated || res->trace.truncated || res->trace.count == 0u ||
        res->trace.count != ref->trace.count) {
        snprintf(detail, sizeof detail,
                 "instruction trace after the park: uninterrupted %u lines, resumed %u lines",
                 ref->trace.count, res->trace.count);
        resume_fail(name, ordinal, detail);
    } else {
        for (unsigned i = 0; i < res->trace.count; i++) {
            if (strcmp(ref->trace.lines[i], res->trace.lines[i]) != 0) {
                snprintf(detail, sizeof detail,
                         "trace line %u after the park differs:" NEWLINE
                         "    uninterrupted: %.110s" NEWLINE "    resumed:       %.110s",
                         i, ref->trace.lines[i], res->trace.lines[i]);
                resume_fail(name, ordinal, detail);
                break;
            }
        }
    }
    if (ref_slice != res_slice) {
        snprintf(detail, sizeof detail,
                 "yield points counted after the park: uninterrupted %d, resumed %d",
                 COSIM_RESUME_SLICE - ref_slice, COSIM_RESUME_SLICE - res_slice);
        resume_fail(name, ordinal, detail);
    }
    if (!resume_stack_is_empty(&res->state) || sr_resume_request != NULL) {
        resume_fail(name, ordinal, "the resumed run left a call record or a resume request");
    }
}

typedef struct {
    const char *name;
    uint32_t entry;
    /* In a state-resumable build: the first yield point in a body that must be refused, and
     * the refusal (0: every body point resumes). The points before `first_body_point` are
     * the dispatcher's own, met before any call record exists. */
    unsigned refused_from;
    SrResumeStatus refusal;
    unsigned first_body_point;
    /* A body left to the interpreter: the entry itself, so no host entry dispatches a body. */
    uint32_t omit;
    /* The entry is the harness's trampoline, which raises exception flow to this address. */
    uint32_t raise_to;
} CosimResumeCase;

/* Shapes the `rs` sweep must have met, so the cell cannot stop exercising one unnoticed. */
static struct {
    unsigned entry_park_depth4, loop_park_depth3, loop_park_at_return_pc, call_at_loop_head;
} g_resume_seen;

/* One snapshot four frames deep, kept for the refusal corpus. */
static CosimPark g_resume_deep;
static unsigned g_resume_deep_ordinal;

static unsigned run_resume_case(const CosimResumeCase *test, const char *ref_path,
                                const char *res_path, int resumable_build) {
    const int32_t whole =
        resume_run_uninterrupted(test->entry, test->omit, test->raise_to, 0, ref_path);
    const unsigned points = (unsigned)(COSIM_RESUME_SLICE - whole);
    unsigned resumed = 0u;
    static CosimPark parked, reference_next;
    const int failures_before = g_resume_failures;
    if (points < 2u || g_park.fired) {
        resume_fail(test->name, 0u, "the run passed no yield point in a body");
        return 0u;
    }
    /* The first divergence of a case ends its sweep: it is the located one, and a resumed
     * run that has gone wrong once can end the process at a later point. */
    for (unsigned ordinal = 1u; ordinal <= points && g_resume_failures == failures_before;
         ordinal++) {
        const int32_t ref_slice = resume_run_uninterrupted(test->entry, test->omit,
                                                           test->raise_to, (int)ordinal, ref_path);
        char detail[320];
        if (!g_park.fired) {
            resume_fail(test->name, ordinal, "the yield point did not park");
            continue;
        }
        SrResumeStatus expected = SR_RESUME_OK;
        if (ordinal < test->first_body_point) {
            expected = SR_RESUME_NO_FRAMES;
        } else if (test->omit != 0u || test->raise_to != 0u) {
            /* The outermost call is refused before anything else is looked at, in any build. */
            expected = test->refusal;
        } else if (!resumable_build) {
            /* No body has a resume point: the outermost is not parked, and a deeper call
             * was begun by an ordinary call site. */
            expected = g_park.status == SR_RESUME_NOT_PARKED ||
                       g_park.status == SR_RESUME_TAIL_CHAIN_LIVE
                ? g_park.status : SR_RESUME_FRAME_NOT_DESCRIBED;
        } else if (test->refused_from != 0u && ordinal >= test->refused_from) {
            expected = test->refusal;
        }
        if (g_park.status != expected) {
            snprintf(detail, sizeof detail, "describe: expected %s, got %s (%s)",
                     sr_resume_status_name(expected), sr_resume_status_name(g_park.status),
                     g_park.detail);
            resume_fail(test->name, ordinal, detail);
            continue;
        }
        if (g_park.status != SR_RESUME_OK) continue;

        parked = g_park;
        const SrResumeFrame *frames = parked.frames;
        const uint32_t count = parked.frame_count;
        const SrResumeFrame *inner = &frames[count - 1u];
        if (test->entry == COSIM_RS) {
            if (count == 4u && inner->kind == SR_RESUME_POINT_ENTRY && inner->body == COSIM_LEAF_A) {
                g_resume_seen.entry_park_depth4++;
                if (g_resume_deep_ordinal == 0u) {
                    g_resume_deep = parked;
                    g_resume_deep_ordinal = ordinal;
                }
            }
            if (count == 3u && inner->kind == SR_RESUME_POINT_YIELD &&
                inner->pc == COSIM_RS_INNER_LOOP)
                g_resume_seen.loop_park_depth3++;
            if (count == 1u && inner->kind == SR_RESUME_POINT_YIELD && inner->pc == COSIM_RS_LOOP)
                g_resume_seen.loop_park_at_return_pc++;
            if (count > 1u && frames[0].kind == SR_RESUME_POINT_CALL && frames[0].pc == COSIM_RS_LOOP)
                g_resume_seen.call_at_loop_head++;
        }
        int32_t res_slice = 0;
        const SrResumeStatus status = resume_run_resumed(&parked, frames, count, 0, res_path,
                                                         &res_slice, detail, sizeof detail);
        if (status != SR_RESUME_OK) {
            char refused[420];
            snprintf(refused, sizeof refused, "resume refused its own description: %s (%s)",
                     sr_resume_status_name(status), detail);
            resume_fail(test->name, ordinal, refused);
            continue;
        }
        resume_compare(&parked, test->name, ordinal, ref_slice, res_slice);
        resumed++;
        /* The uninterrupted run again, parking at this point and at the next, and the same
         * resume again, parking at its first yield point. */
        g_resume_park_twice = 1;
        (void)resume_run_uninterrupted(test->entry, test->omit, test->raise_to, (int)ordinal,
                                       ref_path);
        g_resume_park_twice = 0;
        reference_next = g_park_next;
        if (reference_next.fired != (ordinal < points)) {
            resume_fail(test->name, ordinal, "the two-park reference did not park as expected");
            continue;
        }
        if (resume_run_resumed(&parked, frames, count, 1, res_path, &res_slice, detail,
                               sizeof detail) != SR_RESUME_OK) {
            resume_fail(test->name, ordinal, "the second resume of one snapshot was refused");
            continue;
        }
        resume_compare_repark(test->name, ordinal, &parked, &reference_next, &g_park);
    }
    if (g_resume_failures == failures_before) {
        fprintf(stderr,
                "  cosim resume OK   %-10s %u yield points, %u resumed and compared" NEWLINE,
                test->name, points, resumed);
    }
    return resumed;
}

/* ---- fail-closed: a frame list this build cannot resume ---- */

static void resume_expect_refusal(const char *what, const SrResumeFrame *frames, uint32_t count,
                                  SrResumeStatus expected, const char *res_path) {
    char detail[200];
    int32_t slice = 0;
    const SrResumeStatus status = resume_run_resumed(&g_resume_deep, frames, count, 0, res_path,
                                                     &slice, detail, sizeof detail);
    char report[420];
    if (status != expected) {
        snprintf(report, sizeof report, "%s: expected %s, got %s (%s)", what,
                 sr_resume_status_name(expected), sr_resume_status_name(status), detail);
        resume_fail("refusal", g_resume_deep_ordinal, report);
        return;
    }
    /* A refusal comes before any change: registers, memory, the trace, the write log, the
     * slice counter and the call records are as the snapshot left them. */
    if (memcmp(&g_resume_res.state, &g_resume_deep.state, sizeof g_resume_deep.state) != 0 ||
        memcmp(g_resume_res.window, g_resume_deep.window, sizeof g_resume_deep.window) != 0 ||
        g_resume_res.writes.count != 0u || g_resume_res.trace.count != 0u ||
        slice != COSIM_RESUME_SLICE || sr_resume_request != NULL) {
        snprintf(report, sizeof report, "%s: refused as %s but changed state", what,
                 sr_resume_status_name(status));
        resume_fail("refusal", g_resume_deep_ordinal, report);
        return;
    }
    fprintf(stderr, "  cosim resume OK   refusal    %-26s %s" NEWLINE,
            sr_resume_status_name(status), what);
}

static void run_resume_refusals(const char *res_path) {
    SrResumeFrame frames[COSIM_RESUME_FRAMES];
    const uint32_t count = g_resume_deep.frame_count;
#define RESUME_FRAMES_RESET() memcpy(frames, g_resume_deep.frames, sizeof frames)

    RESUME_FRAMES_RESET();
    resume_expect_refusal("no frames", frames, 0u, SR_RESUME_BAD_FRAMES, res_path);

    RESUME_FRAMES_RESET();
    frames[1].kind = 9u;
    resume_expect_refusal("a kind no build knows", frames, count, SR_RESUME_BAD_FRAMES, res_path);

    RESUME_FRAMES_RESET();
    frames[count - 1u].kind = SR_RESUME_POINT_CALL;
    resume_expect_refusal("a call point innermost", frames, count, SR_RESUME_POINT_KIND, res_path);

    RESUME_FRAMES_RESET();
    frames[1].kind = SR_RESUME_POINT_YIELD;
    resume_expect_refusal("a parked point below a frame", frames, count, SR_RESUME_POINT_KIND,
                          res_path);

    /* An address outside the guest arena: no build registers a function for it. */
    RESUME_FRAMES_RESET();
    frames[0].body = COSIM_UNOWNED;
    resume_expect_refusal("a body no function is registered for", frames, count,
                          SR_RESUME_BODY_NOT_REGISTERED, res_path);

    /* The trampoline is registered, by the harness, and has no resume points. Validation must
     * refuse it without calling it: g_trampoline_hits would count the call. */
    RESUME_FRAMES_RESET();
    frames[0].body = COSIM_RETURN;
    g_trampoline_hits = 0;
    resume_expect_refusal("a registered body with no resume points", frames, count,
                          SR_RESUME_BODY_NOT_RESUMABLE, res_path);
    if (g_trampoline_hits != 0) {
        resume_fail("refusal", g_resume_deep_ordinal,
                    "validation ran a body that is not state-resumable");
    }

    RESUME_FRAMES_RESET();
    frames[1].pc += 4u;
    resume_expect_refusal("a return PC that is no call's", frames, count,
                          SR_RESUME_NO_RESUME_POINT, res_path);

    /* COSIM_RS_INNER_RETURN is a call point of rs_inner and not a loop target. */
    RESUME_FRAMES_RESET();
    frames[2].kind = SR_RESUME_POINT_YIELD;
    frames[2].pc = COSIM_RS_INNER_RETURN;
    resume_expect_refusal("a call's return PC as a parked point", frames, 3u,
                          SR_RESUME_NO_RESUME_POINT, res_path);

    /* Every point exists, but rs's call entered rs_mid, not rs_inner. */
    RESUME_FRAMES_RESET();
    frames[1] = g_resume_deep.frames[2];
    frames[2] = g_resume_deep.frames[3];
    resume_expect_refusal("a frame the call below did not enter", frames, 3u,
                          SR_RESUME_FRAME_CHAIN, res_path);

    /* A second resume while one is in progress. */
    RESUME_FRAMES_RESET();
    {
        CpuState state = g_resume_deep.state;
        install_lane(COSIM_LANE_AOT);
        sr_resume_request = &frames[0];
        const SrResumeStatus status = sr_resume_validate(&state, frames, count, NULL, 0u);
        const int kept = sr_resume_request == &frames[0];
        sr_resume_request = NULL;
        if (status != SR_RESUME_BUSY || !kept ||
            memcmp(&state, &g_resume_deep.state, sizeof state) != 0) {
            resume_fail("refusal", g_resume_deep_ordinal,
                        "a resume in progress was not refused as resume-busy, or was disturbed");
        } else {
            fprintf(stderr, "  cosim resume OK   refusal    %-26s %s" NEWLINE,
                    sr_resume_status_name(status), "a resume while one is in progress");
        }
    }

    /* A fiber that already holds a call record is not a fresh one. */
    RESUME_FRAMES_RESET();
    {
        char detail[200];
        CpuState state = g_resume_deep.state;
        const CpuState before = state;
        install_lane(COSIM_LANE_AOT);
        sr_aot_return_boundaries_reset();
        (void)sr_aot_return_boundary_begin(&state, 0u, 0u, 0u);
        const SrResumeStatus status =
            dispatch_resume_from_host(&state, frames, count, detail, sizeof detail);
        sr_aot_return_boundaries_reset();
        if (status != SR_RESUME_BUSY || memcmp(&state, &before, sizeof state) != 0) {
            resume_fail("refusal", g_resume_deep_ordinal,
                        "a fiber holding a call record was not refused as resume-busy");
        } else {
            fprintf(stderr, "  cosim resume OK   refusal    %-26s %s" NEWLINE,
                    sr_resume_status_name(status), "a fiber that holds a call record");
        }
    }
#undef RESUME_FRAMES_RESET
}

/* ---- fail-closed: a thread the frame list cannot describe ---- */

/* The records a frame list is rebuilt into: describing them must give the list back, and
 * every single departure from them must be refused by name. */
static void resume_records_from_frames(SrAotReturnBoundary *records, CpuState *s,
                                       const SrResumeFrame *frames, uint32_t count) {
    memset(records, 0, count * sizeof *records);
    for (uint32_t k = 0; k < count; k++) {
        records[k].state = s;
        records[k].resume_pc = k == 0u ? SR_HOST_RETURN_PC : frames[k - 1u].pc;
        records[k].has_resume_pc = 1u;
        records[k].call_sp = frames[k].sp;
        records[k].dispatch_owned = k == 0u;
        records[k].callee = frames[k].body;
        records[k].flags = SR_BOUNDARY_CALLEE;
        if (k == 0u) records[k].flags |= SR_BOUNDARY_HOST_ENTRY;
        if (k != 0u) {
            records[k].caller_body = frames[k - 1u].body;
            records[k].caller_sp = frames[k - 1u].sp;
            records[k].flags |= SR_BOUNDARY_SITE;
        }
    }
    records[count - 1u].park_body = frames[count - 1u].body;
    records[count - 1u].park_pc = frames[count - 1u].pc;
    records[count - 1u].park_kind = frames[count - 1u].kind;
    records[count - 1u].park_sp = frames[count - 1u].sp;
    records[count - 1u].flags |= SR_BOUNDARY_PARKED;
}

static void resume_expect_undescribed(const char *what, const SrAotReturnBoundaryStack *stack,
                                      const CpuState *s, SrResumeStatus expected) {
    SrResumeFrame frames[COSIM_RESUME_FRAMES];
    uint32_t count = 77u;
    char detail[200];
    const SrResumeStatus status =
        sr_resume_describe(stack, s, frames, COSIM_RESUME_FRAMES, &count, detail, sizeof detail);
    if (status != expected || count != 0u) {
        char report[420];
        snprintf(report, sizeof report, "%s: expected %s, got %s with %u frame(s) (%s)", what,
                 sr_resume_status_name(expected), sr_resume_status_name(status), count, detail);
        resume_fail("describe", g_resume_deep_ordinal, report);
        return;
    }
    fprintf(stderr, "  cosim resume OK   describe   %-26s %s" NEWLINE,
            sr_resume_status_name(status), what);
}

static void run_resume_describe_refusals(void) {
    const uint32_t count = g_resume_deep.frame_count;
    CpuState state = g_resume_deep.state;
    SrAotReturnBoundary records[COSIM_RESUME_FRAMES];
    SrAotReturnBoundaryStack stack;
#define RESUME_STACK_RESET() do { \
    resume_records_from_frames(records, &state, g_resume_deep.frames, count); \
    memset(&stack, 0, sizeof stack); \
    stack.boundaries = records; \
    stack.depth = count; \
    stack.capacity = COSIM_RESUME_FRAMES; \
    stack.top = &records[count - 1u]; \
} while (0)

    RESUME_STACK_RESET();
    {
        SrResumeFrame frames[COSIM_RESUME_FRAMES];
        uint32_t described = 0u;
        const SrResumeStatus status = sr_resume_describe(&stack, &state, frames,
                                                         COSIM_RESUME_FRAMES, &described, NULL, 0u);
        if (status != SR_RESUME_OK || described != count ||
            memcmp(frames, g_resume_deep.frames, count * sizeof frames[0]) != 0) {
            resume_fail("describe", g_resume_deep_ordinal,
                        "the records a frame list is rebuilt into do not describe as that list");
            return;
        }
    }

    RESUME_STACK_RESET();
    stack.depth = 0u;
    resume_expect_undescribed("no live call", &stack, &state, SR_RESUME_NO_FRAMES);

    RESUME_STACK_RESET();
    stack.floor = 2u;
    resume_expect_undescribed("a callback entered above frame 2", &stack, &state,
                              SR_RESUME_SCOPE_LIVE);

    RESUME_STACK_RESET();
    stack.scopes_live = 1u;
    resume_expect_undescribed("a callback entered before the first call record", &stack, &state,
                              SR_RESUME_SCOPE_LIVE);

    RESUME_STACK_RESET();
    stack.drains_live = 1u;
    resume_expect_undescribed("a tail drain running a body", &stack, &state,
                              SR_RESUME_TAIL_CHAIN_LIVE);

    RESUME_STACK_RESET();
    {
        CpuState other = state;
        records[2].state = &other;
        resume_expect_undescribed("a call record of another CpuState", &stack, &state,
                                  SR_RESUME_FOREIGN_STATE);
    }

    RESUME_STACK_RESET();
    records[0].resume_pc = COSIM_RETURN;
    resume_expect_undescribed("an outermost call that returns into guest code", &stack, &state,
                              SR_RESUME_ROOT_NOT_HOST_ENTRY);

    RESUME_STACK_RESET();
    records[0].dispatch_owned = 0u;
    resume_expect_undescribed("an outermost call no dispatcher began", &stack, &state,
                              SR_RESUME_ROOT_NOT_HOST_ENTRY);

    RESUME_STACK_RESET();
    records[0].flags &= ~SR_BOUNDARY_HOST_ENTRY;
    resume_expect_undescribed("an outermost call the interpreter handed on", &stack, &state,
                              SR_RESUME_ROOT_NOT_HOST_ENTRY);

    RESUME_STACK_RESET();
    records[1].observed_target = UINT64_C(0x100000000) | COSIM_RETURN;
    resume_expect_undescribed("a captured return target", &stack, &state,
                              SR_RESUME_RETURN_IN_FLIGHT);

    RESUME_STACK_RESET();
    records[2].unwinding = 1u;
    resume_expect_undescribed("a call a guest return is unwinding", &stack, &state,
                              SR_RESUME_RETURN_IN_FLIGHT);

    RESUME_STACK_RESET();
    records[2].flags &= ~SR_BOUNDARY_SITE;
    resume_expect_undescribed("a call an ordinary call site began", &stack, &state,
                              SR_RESUME_FRAME_NOT_DESCRIBED);

    RESUME_STACK_RESET();
    records[2].dispatch_owned = 1u;
    resume_expect_undescribed("a call the dispatcher began", &stack, &state,
                              SR_RESUME_FRAME_NOT_DESCRIBED);

    RESUME_STACK_RESET();
    records[2].has_resume_pc = 0u;
    resume_expect_undescribed("a call from host code", &stack, &state,
                              SR_RESUME_FRAME_NOT_DESCRIBED);

    RESUME_STACK_RESET();
    records[2].caller_body = COSIM_RS;
    resume_expect_undescribed("a call from a body the call below did not enter", &stack, &state,
                              SR_RESUME_FRAME_CHAIN);

    RESUME_STACK_RESET();
    records[2].caller_sp += 16u;
    resume_expect_undescribed("a body whose entry $sp is not its call's", &stack, &state,
                              SR_RESUME_FRAME_SP);

    RESUME_STACK_RESET();
    records[count - 1u].flags &= ~SR_BOUNDARY_PARKED;
    resume_expect_undescribed("an innermost body that is running", &stack, &state,
                              SR_RESUME_NOT_PARKED);

    RESUME_STACK_RESET();
    records[count - 1u].park_body = COSIM_RS_INNER;
    resume_expect_undescribed("a parked body the innermost call did not enter", &stack, &state,
                              SR_RESUME_NOT_PARKED);

    RESUME_STACK_RESET();
    records[count - 1u].park_sp -= 16u;
    resume_expect_undescribed("a parked body whose entry $sp is not its call's", &stack, &state,
                              SR_RESUME_FRAME_SP);
#undef RESUME_STACK_RESET
}

/* ---- fail-closed: a callback entered before the thread has a call record ---- */

/* A thread whose own code has begun no call record (it is in the dispatcher, or in the
 * interpreter) takes a callback: a second host entry on the same fiber, in a scope, as
 * sr_callback_dispatch_one makes it. The callback's body parks. Its call record is the
 * fiber's first and is a host entry's own, and the scope's floor is 0, so only the count of
 * open scopes says the thread's own host entry is still underneath. */
static struct {
    int stage;
    SrResumeStatus outer;
    SrResumeStatus status;
} g_resume_nested;

static void resume_nested_hook(CpuState *s) {
    atomic_store_explicit(&sr_timeslice, COSIM_RESUME_SLICE, memory_order_relaxed);
    if (g_resume_nested.stage == 0) {
        g_resume_nested.stage = 1;
        const CpuState saved = *s;
        SrAotReturnBoundaryScope scope;
        /* The premise: the thread itself has no call record here, so the floor will be 0. */
        SrResumeFrame none[1];
        uint32_t none_count = 0u;
        g_resume_nested.outer = sr_resume_describe(NULL, s, none, 1u, &none_count, NULL, 0u);
        sr_aot_return_boundary_scope_begin(&scope);
        /* The callback's own dispatch counts one yield point; its body's entry is the next. */
        atomic_store_explicit(&sr_timeslice, 2, memory_order_relaxed);
        dispatch_from_host(s, COSIM_LEAF_A);
        (void)sr_aot_return_boundary_scope_end(s, &scope, "cosim resume test");
        *s = saved;
        atomic_store_explicit(&sr_timeslice, COSIM_RESUME_SLICE, memory_order_relaxed);
    } else if (g_resume_nested.stage == 1) {
        g_resume_nested.stage = 2;
        SrResumeFrame frames[COSIM_RESUME_FRAMES];
        uint32_t count = 0u;
        g_resume_nested.status =
            resume_describe_switched_out(s, frames, COSIM_RESUME_FRAMES, &count, NULL, 0u);
    }
}

static void run_resume_nested_entry(const char *trace_path) {
    static const CosimCase plain = { "resume", 0u, 0u, "", 0u, 0u, NULL };
    CosimLane *lane = &g_resume_ref;
    install_lane(COSIM_LANE_AOT);
    seed_window();
    seed_state(&lane->state, &plain);
    lane->state.r[31] = SR_HOST_RETURN_PC;
    g_trampoline_raise = 0u;
    memset(&g_resume_nested, 0, sizeof g_resume_nested);
    /* The thread's own dispatch is the first yield point: no call record exists yet. */
    atomic_store_explicit(&sr_timeslice, 1, memory_order_relaxed);
    resume_lane_begin(lane, trace_path, COSIM_LEAF_A);
    g_cosim_yield_hook = resume_nested_hook;
    dispatch_from_host(&lane->state, COSIM_LEAF_A);
    (void)resume_lane_end(lane, trace_path);
    if (g_resume_nested.stage != 2 || g_resume_nested.outer != SR_RESUME_NO_FRAMES ||
        g_resume_nested.status != SR_RESUME_SCOPE_LIVE) {
        char detail[240];
        snprintf(detail, sizeof detail,
                 "a callback's body at a yield point before the thread's first call record: "
                 "expected %s, got %s (stage %d; the thread itself described as %s)",
                 sr_resume_status_name(SR_RESUME_SCOPE_LIVE),
                 sr_resume_status_name(g_resume_nested.status), g_resume_nested.stage,
                 sr_resume_status_name(g_resume_nested.outer));
        resume_fail("callback", 0u, detail);
    } else {
        fprintf(stderr, "  cosim resume OK   describe   %-26s %s" NEWLINE,
                sr_resume_status_name(g_resume_nested.status),
                "a callback's body, entered before the thread's first call record");
    }
}

static int run_resume_cases(const char *trace_dir) {
#if defined(COSIM_STATE_RESUME)
    const int resumable_build = 1;
#else
    const int resumable_build = 0;
#endif
    char ref_path[512], res_path[512];
    snprintf(ref_path, sizeof ref_path, "%s/resume_uninterrupted.trace", trace_dir);
    snprintf(res_path, sizeof res_path, "%s/resume_resumed.trace", trace_dir);
    uint32_t link = 0u, zlink = 0u, linkr = 0u;
    for (unsigned i = 0; i < g_case_count; i++) {
        if (strcmp(g_cases[i].name, "link") == 0) link = g_cases[i].address;
        if (strcmp(g_cases[i].name, "zlink") == 0) zlink = g_cases[i].address;
        if (strcmp(g_cases[i].name, "linkr") == 0) linkr = g_cases[i].address;
    }
    if (link == 0u || zlink == 0u || linkr == 0u) {
        resume_fail("setup", 0u, "the link, zlink and linkr cells are not in the manifest");
        return g_resume_failures;
    }
    const CosimResumeCase cases[] = {
        /* Three frames and a leaf: every yield point in a body resumes. */
        { "rs", COSIM_RS, 0u, SR_RESUME_OK, 2u, 0u, 0u },
        /* A direct call in a conventional frame, and one behind a taken linked branch. */
        { "link", link, 0u, SR_RESUME_OK, 2u, 0u, 0u },
        { "zlink", zlink, 0u, SR_RESUME_OK, 2u, 0u, 0u },
        /* rs_tail's own entry resumes (point 2); rs_inner then runs under a tail drain. */
        { "rs_tail", COSIM_RS_TAIL, 3u, SR_RESUME_TAIL_CHAIN_LIVE, 2u, 0u, 0u },
        /* linkr's own entry resumes (point 2); its `jalr` goes through the dispatcher, whose
         * call record no state-resumable call site began. */
        { "linkr", linkr, 3u, SR_RESUME_FRAME_NOT_DESCRIBED, 2u, 0u, 0u },
        /* The host entry lands on a body with no translation. The interpreter runs it and
         * hands the frame to xread by `j` (points 1 and 2 are the two dispatches); xread's
         * call record returns to the host, but the interpreter's run under it still holds
         * what the call owes, so it is not a host entry's own. */
        { "xtailmid", COSIM_XTAILMID, 3u, SR_RESUME_ROOT_NOT_HOST_ENTRY, 3u, COSIM_XTAILMID, 0u },
        /* The host entry's body leaves exception flow to rs, and the dispatcher dispatches rs
         * under the same call (points 1 and 2 are the two dispatches). The frames below rs
         * keep the call's own entry $sp, so every park in rs and under it is refused. */
        { "flow-again", COSIM_RETURN, 3u, SR_RESUME_ROOT_NOT_HOST_ENTRY, 3u, 0u, COSIM_RS },
    };
    fprintf(stderr, NEWLINE "cosim: resume at a PC (build: %s)" NEWLINE,
            resumable_build ? "state-resumable" : "ordinary; every thread must be refused");
    unsigned resumed = 0u;
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        g_resume_stage_refusals = i == 0u;
        resumed += run_resume_case(&cases[i], ref_path, res_path, resumable_build);
    }
    g_resume_stage_refusals = 0;
    run_resume_nested_entry(ref_path);

    SrResumeFrame entry_only = { COSIM_RS, COSIM_RS, SR_RESUME_POINT_ENTRY, COSIM_STACK };
    CpuState probe_state;
    memset(&probe_state, 0, sizeof probe_state);
    install_lane(COSIM_LANE_AOT);
    const SrResumeStatus entry_status = sr_resume_validate(&probe_state, &entry_only, 1u, NULL, 0u);
    if (!resumable_build) {
        if (resumed != 0u) {
            resume_fail("ordinary-build", 0u, "a build with no resume points resumed a thread");
        }
        if (entry_status != SR_RESUME_BODY_NOT_RESUMABLE) {
            resume_fail("ordinary-build", 0u,
                        "a body of an ordinary build was not refused as body-not-resumable");
        }
        if (g_resume_failures == 0) {
            fprintf(stderr, "cosim: resume OK (ordinary build: every yield point refused, "
                    "bodies refused as %s)" NEWLINE, sr_resume_status_name(entry_status));
        }
        return g_resume_failures;
    }
    if (entry_status != SR_RESUME_OK) {
        resume_fail("rs", 0u, "the build is not state-resumable: rs has no entry point");
        return g_resume_failures;
    }
    if (!g_resume_seen.entry_park_depth4 || !g_resume_seen.loop_park_depth3 ||
        !g_resume_seen.loop_park_at_return_pc || !g_resume_seen.call_at_loop_head ||
        g_resume_deep_ordinal == 0u) {
        char detail[240];
        snprintf(detail, sizeof detail,
                 "the rs sweep missed a shape: entry park four deep %u, loop park three deep %u, "
                 "loop park at a return PC %u, call point at a loop head %u",
                 g_resume_seen.entry_park_depth4, g_resume_seen.loop_park_depth3,
                 g_resume_seen.loop_park_at_return_pc, g_resume_seen.call_at_loop_head);
        resume_fail("rs", 0u, detail);
        return g_resume_failures;
    }
    /* Each park of the rs sweep staged five refusals; these are the four-deep park's. */
    if (g_resume_deep.foreign_state != SR_RESUME_FOREIGN_STATE ||
        g_resume_deep.short_capacity != SR_RESUME_TOO_MANY_FRAMES ||
        g_resume_deep.scope_live != SR_RESUME_SCOPE_LIVE ||
        g_resume_deep.inside_yield != SR_RESUME_NOT_PARKED ||
        g_resume_deep.after_interrupt != SR_RESUME_NOT_PARKED) {
        char detail[320];
        snprintf(detail, sizeof detail,
                 "live refusals at the four-deep park: another CpuState %s, room for one frame "
                 "%s, a callback scope %s, the running fiber inside its yield %s, the same after "
                 "the yield serviced an interrupt %s",
                 sr_resume_status_name(g_resume_deep.foreign_state),
                 sr_resume_status_name(g_resume_deep.short_capacity),
                 sr_resume_status_name(g_resume_deep.scope_live),
                 sr_resume_status_name(g_resume_deep.inside_yield),
                 sr_resume_status_name(g_resume_deep.after_interrupt));
        resume_fail("describe", g_resume_deep_ordinal, detail);
    } else {
        fprintf(stderr, "  cosim resume OK   describe   %-26s %s" NEWLINE,
                sr_resume_status_name(g_resume_deep.after_interrupt),
                "the running fiber inside its yield, before and after it serviced an interrupt");
    }
    run_resume_refusals(res_path);
    run_resume_describe_refusals();
    if (g_resume_failures == 0) {
        fprintf(stderr, "cosim: resume OK (%u parked threads resumed and compared)" NEWLINE, resumed);
    }
    return g_resume_failures;
}

int main(int argc, char **argv) {
    sr_perf_init();
    if (argc < 4) {
        fprintf(stderr,
                "usage: cosim_selftest <guest_image.bin> <image_base_hex> <trace_dir>\n");
        return 2;
    }
    g_image_base = (uint32_t)strtoul(argv[2], NULL, 16);
    if (g_image_base != COSIM_BASE) {
        fprintf(stderr,
                "cosim: image base 0x%08x does not match the generated manifest "
                "0x%08x\n", g_image_base, (uint32_t)COSIM_BASE);
        return 2;
    }
    load_guest_image(argv[1]);
    qualify_cases();

    const int skip_ra_return_cases =
        getenv("SR_COSIM_SKIP_AOT_RETURN_CASES") != NULL;
    if (skip_ra_return_cases) {
        fprintf(stderr,
                "cosim: mutation isolation skips the dedicated AOT return-boundary cells\n");
    }
    unsigned executed = 0;
    uint64_t declared_census_mismatches = 0u;
    for (unsigned i = 0; i < g_case_count; i++) {
        if (skip_ra_return_cases && strcmp(g_cases[i].name, "rarewrite") == 0) {
            continue;
        }
#if defined(SR_STACK_CENSUS_ENABLED)
        SrStackCensusSummary census_before;
        sr_stack_census_snapshot(&census_before);
#endif
        run_case(&g_cases[i], argv[3]);
        executed++;
#if defined(SR_STACK_CENSUS_ENABLED)
        uint64_t declared = 0u;
        if (census_declared_case(g_cases[i].name, &declared)) {
            SrStackCensusSummary census_after;
            sr_stack_census_snapshot(&census_after);
            const uint64_t added = census_after.mismatches - census_before.mismatches;
            declared_census_mismatches += added;
            if (added != declared) {
                char detail[160];
                snprintf(detail, sizeof detail,
                         "stack census: declared case added %" PRIu64
                         " mismatches, expected %" PRIu64, added, declared);
                report_fail(&g_cases[i], detail);
            }
        }
#endif
    }
#if !defined(SR_STACK_CENSUS_ENABLED)
    (void)declared_census_mismatches;
#endif
    for (unsigned m = 0; m < sizeof g_fcr31_modes / sizeof g_fcr31_modes[0]; m++) {
        for (unsigned i = 0; i < g_case_count; i++) {
            if (strcmp(g_cases[i].name, "fpu") != 0) {
                continue;
            }
            CosimCase mode_case = g_cases[i];
            mode_case.fcr31 = g_fcr31_modes[m];
            run_case(&mode_case, argv[3]);
            executed++;
        }
    }

    if (!skip_ra_return_cases) {
        CosimCase dispatch_boundary_case = {
            "rarewrite-dispatch", (uint32_t)COSIM_RA_REWRITE_CALLEE, 4u,
            "rewritten $ra through dispatch_call_try", 0u, 0u, NULL
        };
        run_case(&dispatch_boundary_case, argv[3]);
        executed++;
    }

    if (executed == 0u) {
        fprintf(stderr, "cosim: no comparison cases ran" NEWLINE);
        return 1;
    }

    g_failures += run_fpu_reference_oracle();
    g_failures += run_negative_corpus();
#if defined(SR_STACK_CENSUS_ENABLED)
    SrStackCensusSummary stack_census;
    /* Returns with no ordinary return contract: the rewritten-$ra callee in both
     * rewritten-$ra cases and the fatal exit of the caller it stops, plus the nonlocal
     * case's exit block and the middle frame its return passes through, plus the two
     * frames each longjmp abandons (lj_g, lj_f) in the lj case's two native lanes. */
    const uint64_t expected_excluded = (skip_ra_return_cases ? 0u : 3u) + 2u + 4u;
    sr_stack_census_snapshot(&stack_census);
    const CosimCase *stack_leak_control = NULL;
    for (unsigned i = 0; i < g_case_count; i++) {
        if (strcmp(g_cases[i].name, "spleak") == 0) {
            stack_leak_control = &g_cases[i];
            break;
        }
    }
    if (stack_leak_control == NULL
        || stack_census.status != SR_STACK_CENSUS_FAILED
        || stack_census.mismatches != 1u + declared_census_mismatches
        || stack_census.entries == 0u
        || stack_census.entries != stack_census.returns
        || !stack_census.unobserved_known
        || stack_census.excluded != expected_excluded
        || stack_census.unexpected != 0u
        || !stack_census.has_mismatch
        || stack_census.first_mismatch_entry != stack_leak_control->address
        || stack_census.first_mismatch_expected_sp
            < stack_census.first_mismatch_actual_sp
        || stack_census.first_mismatch_expected_sp
            - stack_census.first_mismatch_actual_sp != 32u) {
        sr_stack_census_report();
        char detail[384];
        snprintf(detail, sizeof detail,
                 "stack census did not match its controls: status=%u entries=%" PRIu64
                 " returns=%" PRIu64 " excluded=%" PRIu64 " expected_excluded=%" PRIu64
                 " unexpected=%" PRIu64 " mismatches=%" PRIu64,
                 (unsigned)stack_census.status, stack_census.entries,
                 stack_census.returns, stack_census.excluded, expected_excluded,
                 stack_census.unexpected, stack_census.mismatches);
        CosimCase census_failure = {
            "stack-census", stack_leak_control ? stack_leak_control->address : 0u,
            0u, "generated host return accounting", 0u, 0u, NULL
        };
        report_fail(&census_failure, detail);
    } else {
        sr_stack_census_report();
        fprintf(stderr,
                "cosim: stack census caught the declared spleak control at 0x%08x; "
                "unobserved=%u, excluded=%" PRIu64 " (expected %" PRIu64 "), "
                "%" PRIu64 " declared in the cross-tier sibcall cases, "
                "all other observed entries balanced\n",
                stack_leak_control->address, stack_census.unobserved,
                stack_census.excluded, expected_excluded, declared_census_mismatches);
    }
#endif
    /* The resume cases compare a resumed run with the uninterrupted one, so they mean
     * something only when the lanes agree. A run that has already diverged has its verdict,
     * and its defect may never end the resume cells: an interpreter that drops delay slots
     * never counts the rs loop down (its counter steps in a slot), and the trace's step
     * abort would then replace the divergence report above with a crash, which the mutation
     * gate cannot count as a kill. */
    if (g_failures == 0) {
        g_failures += run_resume_cases(argv[3]);
    } else {
        fprintf(stderr, "\ncosim: resume cases not run: the comparison above already diverged\n");
    }
    fprintf(stderr, "\ncosim: %u comparison cases, %d divergence report(s)\n",
            executed, g_failures);
    if (g_failures != 0) {
        fprintf(stderr, "cosim: FAIL\n");
        return 1;
    }
    fprintf(stderr, "cosim: OK\n");
    return 0;
}
