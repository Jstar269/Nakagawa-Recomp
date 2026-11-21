// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// Real-PSP instruction trace for the PSP-3000 oracle, one PRX and one launch:
//   CASE=insn-trace (PSP-TRACE-001, #312): executes each source-owned microprogram of
//     insn_trace_fixtures.h one instruction at a time on the Allegrex and records the
//     complete traced register state (r1..r25, r29..r31, hi, lo, selected f0..f3/fcr31
//     and v0..v3/v32) and any store after every instruction, so tools/psp_oracle/insn_trace.py
//     can write a v2 PSP_HARDWARE trace and compare it with Nakagawa's trace of the same words.
//   CASE=insn-trace-exception (PSP-TRACE-EXCEPTION-001, #312): a source-owned break in a
//     branch delay slot; the runner retains EPC/Cause/BadVAddr/Status from exprint.
//
// How a step runs. The probe keeps the guest state in memory. An ordinary instruction is
// copied into the slot of a trampoline (insn_trace_tramp.S) that loads the guest registers,
// executes it on the CPU and stores them back. A conditional branch is placed in the slot
// with its offset rewritten to the trampoline's taken label and a nop in its delay slot, so
// the CPU decides taken or not; the probe then writes the link register for the linking
// forms (pc + 8), runs the delay-slot instruction as its own step unless a likely branch
// was not taken, and continues at the target or at pc + 8. j/jal take their target from the
// word and jr/jalr from the register, read before the link write and the delay slot. A
// store's address is the base register before the step plus the offset, and its value is
// read back after the step. A word outside these forms stops the fixture with an
// UNSUPPORTED end record; no step is guessed.
//
// Memory: the fixtures' data block is the 4 KiB of user partition 2 at NT_DATA_BASE,
// allocated at that address and zeroed; a fixture never touches anything else. The probe's
// own trampoline buffer is a static array, written back and invalidated in the caches
// before every step.
//
// Record stream and log: the same writer, round trip and module lifecycle as
// probe_hle_issues.c (both include probe_hle_harness.h): NAKAGAWA_PSP_META, a NAKAGAWA_PSP_STEP per
// fixture, per fixture a `<id>-init` record, one `<id>-sNNNN` record per instruction and a
// `<id>-end` record, then an `insn-trace-done` terminal record whose out0 counts the
// records, the host0 round trip and a final NAKAGAWA_PSP_COMPLETE line.

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <pspsysmem.h>
#include <psputils.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("PSP_HLE_MEASURE", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(64);

#ifndef PROBE_BUILD_COMMIT
#error PROBE_BUILD_COMMIT is required
#endif

#define NT_CASE_TRACE 119
#define NT_CASE_EXCEPTION 135

#if PSP_ORACLE_CASE == NT_CASE_TRACE
#define HLE_CAMPAIGN_ID "insn-trace"
#define HLE_TEST_ID "PSP-TRACE-001"
#define HLE_LOG "host0:/insn_trace_log.txt"
#elif PSP_ORACLE_CASE == NT_CASE_EXCEPTION
#define HLE_CAMPAIGN_ID "insn-trace-exception"
#define HLE_TEST_ID "PSP-TRACE-EXCEPTION-001"
#define HLE_LOG "host0:/insn_trace_exception_log.txt"
#else
#error probe_insn_trace.c has no case for this PSP_ORACLE_CASE
#endif
#define HLE_DONE_ID HLE_CAMPAIGN_ID "-done"

#include "probe_hle_harness.h"   /* writer, round trip and module lifecycle */

int module_stop(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    const SceUID main_thread = s_hle_main_thread;
    if (main_thread < 0) {
        return 1;
    }
    s_hle_stop_requested = 1;
    (void)sceKernelWakeupThread(main_thread);
    SceUInt timeout = HLE_MAIN_STOP_TIMEOUT_US;
    if (sceKernelWaitThreadEnd(main_thread, &timeout) < 0) {
        return 1;
    }
    if (sceKernelDeleteThread(main_thread) < 0) {
        return 1;
    }
    s_hle_main_thread = -1;
    return 0;
}

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == NT_CASE_TRACE

#include "insn_trace_fixtures.h"

#define NT_MAX_STEPS 512u
#define NT_TRAMP_WORDS 192u
#define NT_RECORD_WORDS 44u
#define NT_INIT_WORDS 44u
#define NT_END_REACHED 0u
#define NT_END_STEP_CAP 1u
#define NT_END_UNSUPPORTED 2u
#define NT_END_SETUP 3u

/* The trampoline's view of the guest (insn_trace_tramp.S documents the layout). */
typedef struct NtState {
    uint32_t r[32];
    uint32_t hi, lo;
    uint32_t flag;
    uint32_t f[4];
    uint32_t fcr31;
    uint32_t v[5];
    uint32_t host[12];
    uint32_t host_f[4];
    uint32_t host_fcr31;
    uint32_t host_v[5];
} NtState;

extern const uint32_t nt_tramp_begin[];
extern const uint32_t nt_tramp_slot[];
extern const uint32_t nt_tramp_taken[];
extern const uint32_t nt_tramp_end[];

static uint32_t s_nt_tramp[NT_TRAMP_WORDS] __attribute__((aligned(64)));
static uint32_t s_nt_tramp_words;
static uint32_t s_nt_slot;   /* word index of the slot in s_nt_tramp */
static uint32_t s_nt_taken;  /* word index of the taken label */
static NtState s_nt;
static SceUID s_nt_block = -1;

static int nt_prepare_tramp(void)
{
    const uint32_t words = (uint32_t)(nt_tramp_end - nt_tramp_begin);
    if (words > NT_TRAMP_WORDS) {
        return 0;
    }
    memcpy(s_nt_tramp, nt_tramp_begin, words * 4u);
    s_nt_tramp_words = words;
    s_nt_slot = (uint32_t)(nt_tramp_slot - nt_tramp_begin);
    s_nt_taken = (uint32_t)(nt_tramp_taken - nt_tramp_begin);
    return 1;
}

/* Patch the slot pair and run the trampoline once on s_nt. */
static void nt_run(uint32_t first, uint32_t second)
{
    s_nt_tramp[s_nt_slot] = first;
    s_nt_tramp[s_nt_slot + 1u] = second;
    sceKernelDcacheWritebackRange(s_nt_tramp, s_nt_tramp_words * 4u);
    sceKernelIcacheInvalidateRange(s_nt_tramp, s_nt_tramp_words * 4u);
    void (*tramp)(NtState *) = (void (*)(NtState *))(uintptr_t)s_nt_tramp;
    tramp(&s_nt);
}

static uint32_t nt_fnv(const uint32_t *words, uint32_t count)
{
    uint32_t h = 0x811c9dc5u;
    for (uint32_t i = 0; i < count; i++) {
        for (unsigned b = 0; b < 4u; b++) {
            h ^= (words[i] >> (8u * b)) & 0xffu;
            h *= 0x01000193u;
        }
    }
    return h;
}

/* The traced registers in canonical TRACE_FORMAT.md order. */
static uint32_t nt_regs(uint32_t *out)
{
    uint32_t n = 0;
    for (unsigned i = 1; i <= 25u; i++) out[n++] = s_nt.r[i];
    out[n++] = s_nt.r[29];
    out[n++] = s_nt.r[30];
    out[n++] = s_nt.r[31];
    out[n++] = s_nt.hi;
    out[n++] = s_nt.lo;
    for (unsigned i = 0; i < 4u; i++) out[n++] = s_nt.f[i];
    out[n++] = s_nt.fcr31;
    for (unsigned i = 0; i < 5u; i++) out[n++] = s_nt.v[i];
    return n;
}

static void nt_init_state(void)
{
    memset(&s_nt, 0, sizeof(s_nt));
    for (unsigned i = 1; i < 32u; i++) s_nt.r[i] = 0x01010101u * i;
    s_nt.r[4] = NT_DATA_BASE;
    s_nt.r[29] = NT_DATA_BASE + 0x800u;
    s_nt.r[26] = s_nt.r[27] = s_nt.r[28] = 0u;
    s_nt.hi = NT_HI_INIT;
    s_nt.lo = NT_LO_INIT;
    s_nt.f[0] = NT_FPU_F0_INIT;
    s_nt.f[1] = NT_FPU_F1_INIT;
    s_nt.f[2] = NT_FPU_F2_INIT;
    s_nt.f[3] = NT_FPU_F3_INIT;
    s_nt.fcr31 = NT_FCR_INIT;
    s_nt.v[0] = NT_VFPU_S000_INIT;
    s_nt.v[1] = NT_VFPU_S001_INIT;
    s_nt.v[2] = NT_VFPU_S002_INIT;
    s_nt.v[3] = NT_VFPU_S003_INIT;
    s_nt.v[4] = NT_VFPU_S200_INIT;
}

static int nt_is_cond_branch(uint32_t w)
{
    const uint32_t op = w >> 26, rt = (w >> 16) & 31u;
    if (op == 4u || op == 5u || op == 6u || op == 7u || (op >= 20u && op <= 23u)) return 1;
    return op == 1u && (rt <= 3u || (rt >= 0x10u && rt <= 0x13u));
}

static int nt_is_likely(uint32_t w)
{
    const uint32_t op = w >> 26, rt = (w >> 16) & 31u;
    return (op >= 20u && op <= 23u) || (op == 1u && (rt & 2u));
}

static int nt_is_control(uint32_t w)
{
    const uint32_t op = w >> 26, fn = w & 63u;
    return nt_is_cond_branch(w) || op == 2u || op == 3u || (op == 0u && (fn == 8u || fn == 9u)) ||
           op == 0x11u || op == 0x12u;
}

/* Ordinary words the trampoline may run: SPECIAL ALU/shift/mult/div/moves (not jr, jalr,
   syscall, break, sync), the immediate ALU group, lui, the byte/half/word loads and
   stores, and the SPECIAL3/Allegrex extension groups. Anything else is unsupported. */
static int nt_is_plain(uint32_t w)
{
    const uint32_t op = w >> 26, fn = w & 63u;
    if (op == 0u) return fn != 8u && fn != 9u && fn != 0x0cu && fn != 0x0du && fn != 0x0fu;
    if (op == 0x11u) {
        const uint32_t rs = (w >> 21) & 31u;
        return rs == 0u || rs == 2u ||
               (rs == 16u && (fn == 0x00u || fn == 0x32u));
    }
    if (w == 0x48ea0000u || w == 0x486b0000u || w == 0x486c0020u ||
        w == 0x486e0008u) return 1;
    if (op >= 8u && op <= 15u) return 1;
    if (op == 0x20u || op == 0x21u || op == 0x23u || op == 0x24u || op == 0x25u) return 1;
    if (op == 0x28u || op == 0x29u || op == 0x2bu) return 1;
    return op == 0x1cu || op == 0x1fu;
}

/* Run one ordinary word at pc and record it. */
static void nt_step_plain(const char *id, uint32_t step, uint32_t pc, uint32_t w)
{
    const uint32_t op = w >> 26;
    uint32_t mkind = 0, maddr = 0, mval = 0;
    if (op == 0x28u || op == 0x29u || op == 0x2bu) {
        maddr = s_nt.r[(w >> 21) & 31u] + (uint32_t)(int32_t)(int16_t)(w & 0xffffu);
        mkind = op == 0x28u ? 8u : op == 0x29u ? 16u : 32u;
    }
    nt_run(w, 0u);
    if (mkind == 8u) mval = *(volatile uint8_t *)(uintptr_t)maddr;
    else if (mkind == 16u) mval = *(volatile uint16_t *)(uintptr_t)maddr;
    else if (mkind == 32u) mval = *(volatile uint32_t *)(uintptr_t)maddr;
    static char name[48];
    uint32_t out[NT_RECORD_WORDS];
    out[0] = pc;
    out[1] = w;
    uint32_t n = 2u + nt_regs(out + 2);
    out[n++] = maddr;   /* the store's width follows from the op word */
    out[n++] = mval;
    snprintf(name, sizeof(name), "%s-s%04u", id, (unsigned int)step);
    hle_record(name, "PASS", 0, out, n);
}

/* A control word at pc: decide, write the link, record it (no store), then the delay slot. */
static void nt_record_control(const char *id, uint32_t step, uint32_t pc, uint32_t w)
{
    static char name[48];
    uint32_t out[NT_RECORD_WORDS];
    out[0] = pc;
    out[1] = w;
    uint32_t n = 2u + nt_regs(out + 2);
    out[n++] = 0u;
    out[n++] = 0u;
    snprintf(name, sizeof(name), "%s-s%04u", id, (unsigned int)step);
    hle_record(name, "PASS", 0, out, n);
}

static void nt_trace(const NtFixture *fx)
{
    static char name[48];
    hle_step(fx->id);
    nt_init_state();
    memset((void *)(uintptr_t)NT_DATA_BASE, 0, NT_DATA_BYTES);
    sceKernelDcacheWritebackInvalidateRange((void *)(uintptr_t)NT_DATA_BASE, NT_DATA_BYTES);
    uint32_t init[NT_INIT_WORDS];
    init[0] = fx->base;
    init[1] = fx->count;
    init[2] = nt_fnv(fx->words, fx->count);
    init[3] = NT_DATA_BASE;
    const uint32_t ni = 4u + nt_regs(init + 4);
    snprintf(name, sizeof(name), "%s-init", fx->id);
    hle_record(name, "PASS", 0, init, ni);

    const uint32_t end = fx->base + 4u * fx->count;
    uint32_t pc = fx->base;
    uint32_t step = 0;
    uint32_t status = NT_END_STEP_CAP;
    uint32_t last = 0;
    while (step < NT_MAX_STEPS) {
        if (pc == end) {
            status = NT_END_REACHED;
            break;
        }
        if (pc < fx->base || pc > end || (pc & 3u)) {
            status = NT_END_UNSUPPORTED;
            break;
        }
        const uint32_t w = fx->words[(pc - fx->base) / 4u];
        last = w;
        if (nt_is_plain(w)) {
            nt_step_plain(fx->id, step++, pc, w);
            pc += 4u;
            continue;
        }
        if (!nt_is_control(w) || (w >> 26) == 0x11u || (w >> 26) == 0x12u || pc + 4u >= end) {
            status = NT_END_UNSUPPORTED;
            break;
        }
        const uint32_t dw = fx->words[(pc + 4u - fx->base) / 4u];
        if (!nt_is_plain(dw)) {
            last = dw;
            status = NT_END_UNSUPPORTED;
            break;
        }
        const uint32_t op = w >> 26, fn = w & 63u;
        uint32_t target = 0;
        int execute_delay = 1;
        if (nt_is_cond_branch(w)) {
            NtState pre = s_nt;
            const int32_t off = (int32_t)s_nt_taken - (int32_t)(s_nt_slot + 1u);
            nt_run((w & 0xffff0000u) | ((uint32_t)off & 0xffffu), 0u);
            const int taken = s_nt.flag != 0u;
            s_nt = pre;
            if (op == 1u && (((w >> 16) & 31u) & 0x10u)) s_nt.r[31] = pc + 8u;
            target = taken ? pc + 4u + ((uint32_t)(int32_t)(int16_t)(w & 0xffffu) << 2) : pc + 8u;
            execute_delay = taken || !nt_is_likely(w);
        } else if (op == 2u || op == 3u) {
            target = ((pc + 4u) & 0xf0000000u) | ((w & 0x03ffffffu) << 2);
            if (op == 3u) s_nt.r[31] = pc + 8u;
        } else if (op == 0u && (fn == 8u || fn == 9u)) {
            target = s_nt.r[(w >> 21) & 31u];
            const uint32_t rd = (w >> 11) & 31u;
            if (fn == 9u && rd != 0u) s_nt.r[rd] = pc + 8u;
        }
        nt_record_control(fx->id, step++, pc, w);
        if (execute_delay && step < NT_MAX_STEPS) {
            nt_step_plain(fx->id, step++, pc + 4u, dw);
        }
        pc = target;
    }
    const uint32_t tail[4] = {step, pc, status, last};
    snprintf(name, sizeof(name), "%s-end", fx->id);
    hle_record(name, status == NT_END_REACHED ? "PASS" : "FAIL", status, tail, 4);
}

static int hle_run(void)
{
    s_nt_block = sceKernelAllocPartitionMemory(2, "nt-data", PSP_SMEM_Addr, NT_DATA_BYTES,
                                               (void *)(uintptr_t)NT_DATA_BASE);
    const uint32_t head = s_nt_block >= 0
        ? (uint32_t)(uintptr_t)sceKernelGetBlockHeadAddr(s_nt_block) : 0u;
    if (s_nt_block < 0 || head != NT_DATA_BASE || !nt_prepare_tramp()) {
        const uint32_t out[3] = {(uint32_t)s_nt_block, head, s_nt_tramp_words};
        hle_record("insn-trace-setup", "ERROR", NT_END_SETUP, out, 3);
        if (s_nt_block >= 0) sceKernelFreePartitionMemory(s_nt_block);
        return 0;
    }
    for (uint32_t f = 0; f < NT_FIXTURE_COUNT; f++) {
        nt_trace(&nt_fixtures[f]);
    }
    sceKernelFreePartitionMemory(s_nt_block);
    s_nt_block = -1;
    return 1;
}

#endif /* NT_CASE_TRACE */

/* ------------------------------------------------------------------------- */

#if PSP_ORACLE_CASE == NT_CASE_EXCEPTION

extern const unsigned char nt_exception_branch[];
extern const unsigned char nt_exception_break[];

static int hle_run(void)
{
    const uint32_t out[1] = {0u};
    hle_step("exception-break-in-delay-slot");
    hle_record("exception-armed", "SKIP",
               (uint32_t)(uintptr_t)nt_exception_branch, out, 1u);
    __asm__ volatile(
        ".set push\n\t"
        ".set noreorder\n\t"
        ".globl nt_exception_branch\n"
        "nt_exception_branch:\n\t"
        "beq $0, $0, 1f\n\t"
        ".globl nt_exception_break\n"
        "nt_exception_break:\n\t"
        "break\n\t"
        "1:\n\t"
        "nop\n\t"
        ".set pop\n\t"
        : : : "memory");
    return 1;
}

#endif /* NT_CASE_EXCEPTION */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    s_hle_main_thread = sceKernelGetThreadId();
    hle_begin();
    const int ok = hle_run();
    hle_finish(ok);
    hle_park_until_stop();
    return 0;
}
