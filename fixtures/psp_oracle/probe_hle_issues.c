// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// HLE issue cells for the PSP-3000 oracle, one PRX and one launch per case:
//   CASE=hle-issues-batch (PSP-HLE-ISSUES-001): #356 model getters and #326 random-unit
//     carry and vscmp NaN/infinity cells;
//   CASE=hle-intr-cells (PSP-HLE-INTR-001): #325 VTimer/alarm handler return values and
//     VBLANK sub-interrupt delivery while interrupts are suspended or a handler holds;
//   CASE=vfpu-forms (PSP-VFPU-FORMS-001): #326 raw result words of vh2f, vuc2i, vsocp,
//     vwbn, vrndf1 and vrndf2, register-only;
//   CASE=intr-cause (PSP-INTR-CAUSE-001): #325 COP0 Cause, Status and Count and the
//     Allegrex interrupt-enable word, read in kernel mode through the kernel bridge,
//     across a sceKernelCpuSuspendIntr window that spans one VBLANK;
//   CASE=vfpu-funnel (PSP-VFPU-FUNNEL-001): #326 raw result words of vfad and vavg at
//     .p, .t and .q, register-only;
//   CASE=vfpu-sort (PSP-VFPU-SORT-001): #326 raw result words of vsrt1.q to vsrt4.q,
//     register-only.
//   CASE=vfpu-unmodeled (PSP-VFPU-UNMODELED-001): #326 raw words of the VFPU forms the runtime
//     refuses (vcrs .s/.p/.q, vcmov imm3 7, mfvc/mtvc of control 255, the rejected VFPU9
//     widths), register-only; a refused form may raise an exception that stops the launch.
//   CASE=vfpu-quad-wb (PSP-VFPU-QUADWB-001): #326 lv.q/sv.q with the write-back flag (word bit 1):
//     the base register after each form and the lanes and buffer words moved, in a probe-owned
//     buffer; POWER RISK: nothing catches an exception.
//   CASE=vfpu-convert (PSP-VFPU-CONVERT-001): #326 raw result words of the VFPU float/integer
//     conversions (vf2iz .s/.q over scales 0-31, vf2in/vf2iu/vf2id .s, vi2f .s/.p/.t/.q,
//     vi2s/vi2us/vi2c/vi2uc .q), register-only, owner present.
//   CASE=vfpu-cc-cmov (PSP-VFPU-CCCMOV-001): #326 raw VFPU_CC words of vcmp (16 conditions,
//     .s/.q, VFPU_CC read three ways and through a vcmovt), vcmovt/vcmovf imm3 0-6 under nine VFPU_CC presets and mfvc/mtvc of
//     VFPU_CC, register-only.
//   CASE=vfpu-vus2i (PSP-VFPU-VUS2I-001): #326 raw result words of vus2i/vs2i .s/.p, vsgn .p/.t/.q
//     and a vflush cell (pending S prefix), register-only, owner present.
//   CASE=vfpu-basic-arith (PSP-VFPU-BASICARITH-001): #326 raw result words of vadd, vsub, vmul,
//     vmin, vmax, vscl, vdiv.s, vneg, vabs, vmov, vzero, vone and vidt over 20 operand classes
//     (all ordered pairs for the binary forms), register-only; vidt.s may raise an exception.
//   CASE=vfpu-wide-dot-matrix (PSP-VFPU-WIDEDOT-001): #326 raw result words of vdot, vhdp, vcrsp,
//     vqmul, vtfm, vhtfm, vmmul, vmscl, vmzero, vmidt, vmone, vmmov and vrot over special lanes
//     and order-sensitive four-term sums, register-only.
//   CASE=vfpu-mfv-mtv (PSP-VFPU-MFVMTV-001): #326 raw words read back by mfv.s after mtv.s for 17 chosen words
//     (zeros, denormals, qNaN/sNaN of both signs, infinities, FLT_MAX, 1.0, patterns) through two
//     VFPU registers, register-only.
//   CASE=vfpu-constants (PSP-VFPU-CONSTANTS-001): #326 raw destination words of vcst.s and vcst.q for constant
//     indices 0-31 over a sentinel background (indices 20-31 are reserved and run last),
//     register-only; a reserved index may raise an exception.
//   CASE=vfpu-round-sep (PSP-VFPU-ROUNDSEP-001): #326 raw result words of vdot.q, vmscl.q,
//     vmul.s, vadd.s and vsub.s over operands that separate round-to-nearest-even from
//     truncation, register-only.
//   CASE=vfpu-prefix-state (PSP-VFPU-PREFIX-001): #326 non-identity vpfxs/vpfxt/vpfxd on measured
//     forms, prefix lifetime across vnop/vsync/vflush/branch/FPU/mtv/mfv/lv.q/vpfxt/mfvc, and
//     vfim.s/viim.s under a pending vpfxd, with the prefix control words, register-only.
//   CASE=vfpu-wider-ops (PSP-VFPU-WIDEROPS-001): #326 raw result words of vdiv, vsin, vcos, vrcp,
//     vrsq, vsqrt and vocp at the widths not yet measured, the .p/.t dot and matrix forms and
//     vidt into every column and row register of one matrix, register-only.
//   CASE=vfpu-reserved-widths (PSP-VFPU-RESERVED-001): #326 raw words after the hand-encoded
//     vus2i/vs2i .t and .q words psp-as refuses, register-only; a reserved word may raise an
//     exception that stops the launch.
//   CASE=null-code-registration (PSP-NULLCODE-001): #1375 entry $ra/$sp/$gp and ABI
//     argument registers for thread, callback, alarm, VTimer and VBLANK sub-interrupt
//     entries, followed by NULL registration results; interrupt/GE handlers are not invoked;
//   CASE=null-thread-invocation / null-callback-invocation: #1375 follow-ups only when the
//     measured entry context is user mode; each is isolated in its own launch.
// These cases live apart from probe_hle_measure.c because they do what that file's
// families never may: hle-intr-cells registers a VBLANK sub-interrupt handler, sets
// VTimer and alarm handlers and suspends interrupts, and hle-issues-batch calls a kernel
// getter through the ARK kernel bridge. Each case removes what it installs before it
// returns: every VTimer is stopped and deleted, every alarm UID cancelled, the
// sub-interrupt handler disabled and released, and every suspension resumed.
//
// Record stream and log: the same as probe_hle_measure.c, whose writer, round trip and
// module lifecycle are repeated here byte for byte (tools/test_psp_oracle.py pins the
// two copies). NAKAGAWA_PSP_META, a NAKAGAWA_PSP_STEP before each risky cell, one
// NAKAGAWA_PSP_TEST per measurement, a `<case>-done` terminal record whose out0 counts
// the measurements, the host0 round trip, and a final NAKAGAWA_PSP_COMPLETE line. Every
// line is appended to the case's host0 log durably (open, append, close). Steps are
// never written inside a timed cell, so host0 I/O cannot shift what the cell measures.

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("PSP_HLE_MEASURE", 0, 1, 0);
#if PSP_ORACLE_CASE == 106 || PSP_ORACLE_CASE == 108 || PSP_ORACLE_CASE == 121 || PSP_ORACLE_CASE == 122 || PSP_ORACLE_CASE == 125 || PSP_ORACLE_CASE == 163 || PSP_ORACLE_CASE == 165 || PSP_ORACLE_CASE == 168 || PSP_ORACLE_CASE == 169 || PSP_ORACLE_CASE == 170 || PSP_ORACLE_CASE == 171 || PSP_ORACLE_CASE == 173 || PSP_ORACLE_CASE == 174 || PSP_ORACLE_CASE == 175 || PSP_ORACLE_CASE == 176 || PSP_ORACLE_CASE == 182 || PSP_ORACLE_CASE == 183 || PSP_ORACLE_CASE == 184
/* hle-issues-batch, vfpu-forms, vfpu-funnel, vfpu-sort, vfpu-forms2, vfpu-unmodeled, vfpu-quad-wb, vfpu-basic-arith, vfpu-cc-cmov, vfpu-convert, vfpu-vus2i, vfpu-wide-dot-matrix, vfpu-mfv-mtv, vfpu-constants, vfpu-round-sep, vfpu-prefix-state, vfpu-wider-ops and vfpu-reserved-widths run VFPU words on the main thread;
   without the VFPU attribute the first VFPU instruction traps (probe.c's vfpu-compare case, measured). */
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
#else
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
#endif
PSP_HEAP_SIZE_KB(64);

#ifndef PROBE_BUILD_COMMIT
#error PROBE_BUILD_COMMIT is required
#endif

#define HLE_CASE_ISSUES_BATCH 106
#define HLE_CASE_INTR_CELLS 107
#define HLE_CASE_VFPU_FORMS 108
#define HLE_CASE_VFPU_FUNNEL 122
#define HLE_CASE_VFPU_SORT 125
#define HLE_CASE_VFPU_FORMS2 121
#define HLE_CASE_VFPU_UNMODELED 163
#define HLE_CASE_VFPU_QUAD_WB 165
#define HLE_CASE_VFPU_CONVERT 170
#define HLE_CASE_VFPU_CC_CMOV 169
#define HLE_CASE_VFPU_VUS2I 171
#define HLE_CASE_VFPU_BASIC_ARITH 168
#define HLE_CASE_VFPU_WIDE_DOT 173
#define HLE_CASE_VFPU_MFV_MTV 174
#define HLE_CASE_VFPU_CONSTANTS 175
#define HLE_CASE_VFPU_ROUND_SEP 176
#define HLE_CASE_VFPU_PREFIX_STATE 182
#define HLE_CASE_VFPU_WIDER_OPS 183
#define HLE_CASE_VFPU_RESERVED_WIDTHS 184
#define HLE_CASE_INTR_CAUSE 120
#define HLE_CASE_NULL_CODE_REGISTRATION 128
#define HLE_CASE_NULL_THREAD_INVOCATION 129
#define HLE_CASE_NULL_CALLBACK_INVOCATION 130

#if PSP_ORACLE_CASE == HLE_CASE_ISSUES_BATCH
#define HLE_CAMPAIGN_ID "hle-issues-batch"
#define HLE_TEST_ID "PSP-HLE-ISSUES-001"
#define HLE_LOG "host0:/hle_issues_batch_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_INTR_CELLS
#define HLE_CAMPAIGN_ID "hle-intr-cells"
#define HLE_TEST_ID "PSP-HLE-INTR-001"
#define HLE_LOG "host0:/hle_intr_cells_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_VFPU_FORMS
#define HLE_CAMPAIGN_ID "vfpu-forms"
#define HLE_TEST_ID "PSP-VFPU-FORMS-001"
#define HLE_LOG "host0:/vfpu_forms_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_VFPU_FUNNEL
#define HLE_CAMPAIGN_ID "vfpu-funnel"
#define HLE_TEST_ID "PSP-VFPU-FUNNEL-001"
#define HLE_LOG "host0:/vfpu_funnel_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_VFPU_SORT
#define HLE_CAMPAIGN_ID "vfpu-sort"
#define HLE_TEST_ID "PSP-VFPU-SORT-001"
#define HLE_LOG "host0:/vfpu_sort_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_VFPU_FORMS2
#define HLE_CAMPAIGN_ID "vfpu-forms2"
#define HLE_TEST_ID "PSP-VFPU-FORMS2-001"
#define HLE_LOG "host0:/vfpu_forms2_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_VFPU_UNMODELED
#define HLE_CAMPAIGN_ID "vfpu-unmodeled"
#define HLE_TEST_ID "PSP-VFPU-UNMODELED-001"
#define HLE_LOG "host0:/vfpu_unmodeled_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_VFPU_QUAD_WB
#define HLE_CAMPAIGN_ID "vfpu-quad-wb"
#define HLE_TEST_ID "PSP-VFPU-QUADWB-001"
#define HLE_LOG "host0:/vfpu_quad_wb_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_VFPU_CONVERT
#define HLE_CAMPAIGN_ID "vfpu-convert"
#define HLE_TEST_ID "PSP-VFPU-CONVERT-001"
#define HLE_LOG "host0:/vfpu_convert_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_VFPU_CC_CMOV
#define HLE_CAMPAIGN_ID "vfpu-cc-cmov"
#define HLE_TEST_ID "PSP-VFPU-CCCMOV-001"
#define HLE_LOG "host0:/vfpu_cc_cmov_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_VFPU_VUS2I
#define HLE_CAMPAIGN_ID "vfpu-vus2i"
#define HLE_TEST_ID "PSP-VFPU-VUS2I-001"
#define HLE_LOG "host0:/vfpu_vus2i_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_VFPU_BASIC_ARITH
#define HLE_CAMPAIGN_ID "vfpu-basic-arith"
#define HLE_TEST_ID "PSP-VFPU-BASICARITH-001"
#define HLE_LOG "host0:/vfpu_basic_arith_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_VFPU_WIDE_DOT
#define HLE_CAMPAIGN_ID "vfpu-wide-dot-matrix"
#define HLE_TEST_ID "PSP-VFPU-WIDEDOT-001"
#define HLE_LOG "host0:/vfpu_wide_dot_matrix_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_VFPU_MFV_MTV
#define HLE_CAMPAIGN_ID "vfpu-mfv-mtv"
#define HLE_TEST_ID "PSP-VFPU-MFVMTV-001"
#define HLE_LOG "host0:/vfpu_mfv_mtv_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_VFPU_CONSTANTS
#define HLE_CAMPAIGN_ID "vfpu-constants"
#define HLE_TEST_ID "PSP-VFPU-CONSTANTS-001"
#define HLE_LOG "host0:/vfpu_constants_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_VFPU_ROUND_SEP
#define HLE_CAMPAIGN_ID "vfpu-round-sep"
#define HLE_TEST_ID "PSP-VFPU-ROUNDSEP-001"
#define HLE_LOG "host0:/vfpu_round_sep_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_VFPU_PREFIX_STATE
#define HLE_CAMPAIGN_ID "vfpu-prefix-state"
#define HLE_TEST_ID "PSP-VFPU-PREFIX-001"
#define HLE_LOG "host0:/vfpu_prefix_state_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_VFPU_WIDER_OPS
#define HLE_CAMPAIGN_ID "vfpu-wider-ops"
#define HLE_TEST_ID "PSP-VFPU-WIDEROPS-001"
#define HLE_LOG "host0:/vfpu_wider_ops_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_VFPU_RESERVED_WIDTHS
#define HLE_CAMPAIGN_ID "vfpu-reserved-widths"
#define HLE_TEST_ID "PSP-VFPU-RESERVED-001"
#define HLE_LOG "host0:/vfpu_reserved_widths_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_INTR_CAUSE
#define HLE_CAMPAIGN_ID "intr-cause"
#define HLE_TEST_ID "PSP-INTR-CAUSE-001"
#define HLE_LOG "host0:/intr_cause_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_NULL_CODE_REGISTRATION
#define HLE_CAMPAIGN_ID "null-code-registration"
#define HLE_TEST_ID "PSP-NULLCODE-001"
#define HLE_LOG "host0:/null_code_registration_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_NULL_THREAD_INVOCATION
#define HLE_CAMPAIGN_ID "null-thread-invocation"
#define HLE_TEST_ID "PSP-NULLCODE-001"
#define HLE_LOG "host0:/null_thread_invocation_log.txt"
#elif PSP_ORACLE_CASE == HLE_CASE_NULL_CALLBACK_INVOCATION
#define HLE_CAMPAIGN_ID "null-callback-invocation"
#define HLE_TEST_ID "PSP-NULLCODE-001"
#define HLE_LOG "host0:/null_callback_invocation_log.txt"
#else
#error probe_hle_issues.c has no case for this PSP_ORACLE_CASE
#endif
#define HLE_DONE_ID HLE_CAMPAIGN_ID "-done"

#define HLE_SENTINEL 0xA5A5A5A5u
/* The same value as text, for the assembly wrappers (IC_V0_WRAPPER). */
#define HLE_SENTINEL_HEX "0xA5A5A5A5"

static int s_log_failed;
static uint32_t s_records;

/* Durable line writer: open, write everything, close. A later hang or fault
   keeps every line already written. `truncate` starts a fresh log. */
static void hle_write(const char *line, int truncate)
{
    const int flags = PSP_O_WRONLY | PSP_O_CREAT | (truncate ? PSP_O_TRUNC : PSP_O_APPEND);
    const SceUID fd = sceIoOpen(HLE_LOG, flags, 0777);
    if (fd < 0) {
        s_log_failed = 1;
        return;
    }
    const size_t length = strlen(line);
    size_t offset = 0;
    while (offset < length) {
        const int wrote = sceIoWrite(fd, line + offset, (SceSize)(length - offset));
        if (wrote <= 0) {
            s_log_failed = 1;
            break;
        }
        offset += (size_t)wrote;
    }
    if (sceIoClose(fd) < 0) {
        s_log_failed = 1;
    }
}

static void hle_begin(void)
{
    static const char meta[] =
        "NAKAGAWA_PSP_META schema=1 source=psp model=unknown firmware=unknown "
        "binary_sha256=0000000000000000000000000000000000000000000000000000000000000000 "
        "source_commit=" PROBE_BUILD_COMMIT " fixture=hle-measure\n";
    hle_write(meta, 1);
}

/* Progress marker written durably before a call that could hang or fault. The
   step name is the measurement case about to run, so a hang names its cell. */
static void hle_step(const char *step)
{
    static char line[200];
    snprintf(line, sizeof(line), "NAKAGAWA_PSP_STEP schema=1 case_id=%s step=%s\n",
             HLE_CAMPAIGN_ID, step);
    hle_write(line, 0);
}

static void hle_emit(const char *case_id, const char *status, uint32_t result,
                     const uint32_t *out, uint32_t count)
{
    static char line[1536];
    int n = snprintf(line, sizeof(line),
                     "NAKAGAWA_PSP_TEST schema=1 test_id=%s case_id=%s status=%s result=0x%08x",
                     HLE_TEST_ID, case_id, status, (unsigned int)result);
    for (uint32_t i = 0; i < count && n > 0 && n < (int)sizeof(line) - 32; i++) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, " out%u=0x%08x", (unsigned int)i,
                      (unsigned int)out[i]);
    }
    if (n > 0 && n < (int)sizeof(line) - 2) {
        line[n] = '\n';
        line[n + 1] = '\0';
        hle_write(line, 0);
    }
}

static void hle_record(const char *case_id, const char *status, uint32_t result,
                       const uint32_t *out, uint32_t count)
{
    s_records++;
    hle_emit(case_id, status, result, out, count);
}

/* The host0 round trip probe.c's teardown performs (PROBE_HOST0_ROUNDTRIP_PATH and
   probe_host0_roundtrip there): 64 bytes, byte i = (0x5A ^ (i * 0x25 + (i >> 3))) & 0xFF,
   written with truncate, then read back and compared. The runner's
   _verify_host0_roundtrip reads the file after unload, so every family leaves it.
   probe.c is a separate translation unit, so the contract is repeated here and
   tools/test_psp_oracle.py pins both copies. */
#define HLE_ROUNDTRIP_PATH "host0:/nakagawa_transport_write.bin"
#define HLE_ROUNDTRIP_BYTES 64u

static int hle_host0_roundtrip(void)
{
    uint8_t expected[HLE_ROUNDTRIP_BYTES];
    uint8_t observed[HLE_ROUNDTRIP_BYTES];
    for (size_t i = 0; i < sizeof(expected); i++) {
        expected[i] = (uint8_t)(0x5Au ^ (i * 0x25u + (i >> 3)));
    }
    memset(observed, 0, sizeof(observed));
    SceUID fd = sceIoOpen(HLE_ROUNDTRIP_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        return 0;
    }
    const int written = sceIoWrite(fd, expected, (SceSize)sizeof(expected));
    const int write_close = sceIoClose(fd);
    if (written != (int)sizeof(expected) || write_close < 0) {
        return 0;
    }
    fd = sceIoOpen(HLE_ROUNDTRIP_PATH, PSP_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    const int read = sceIoRead(fd, observed, (SceSize)sizeof(observed));
    const int read_close = sceIoClose(fd);
    return read == (int)sizeof(observed) && read_close >= 0 &&
           memcmp(expected, observed, sizeof(expected)) == 0;
}

/* Shared end of run, after every measurement and its cleanup. The round trip
   is host I/O only, so it cannot change a cell already recorded. As in probe.c's
   teardown, a failed round trip makes the completion status FAIL. */
static void hle_finish(int ok)
{
    const uint32_t count = s_records;
    hle_emit(HLE_DONE_ID, "PASS", 0, &count, 1);
    const int roundtrip = hle_host0_roundtrip();
    static char line[96];
    snprintf(line, sizeof(line), "NAKAGAWA_PSP_COMPLETE schema=1 status=%s\n",
             (ok && !s_log_failed && roundtrip) ? "PASS" : "FAIL");
    hle_write(line, 0);
}

/* Module lifecycle, as in probe.c: main parks until module_stop asks it to end,
   then runs the CRT's de-initialisation and exits; module_stop deletes it. */
static volatile SceUID s_hle_main_thread = -1;
static volatile int s_hle_stop_requested;
extern void _fini(void);
extern void __libcglue_deinit(void);
#define HLE_MAIN_STOP_TIMEOUT_US 1000000u

static void hle_park_until_stop(void)
{
    while (!s_hle_stop_requested) {
        sceKernelSleepThread();
    }
    _fini();
    __libcglue_deinit();
    sceKernelExitThread(0);
}

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
#if PSP_ORACLE_CASE == HLE_CASE_ISSUES_BATCH

/* The 2026-10-24 NEEDS-PSP issue batch (PSP-HLE-ISSUES-001), one launch:
   - #356: kuKernelGetModel and sceKernelGetModel side by side, raw;
   - #326: the random unit's carry, one vrndi.s step from 2 x 448 written RCX states, and
     vscmp over NaN, infinity, signed-zero and denormal lanes at every width.
   Both #326 sections run twice in the same launch (pass 0 and pass 1) for repeatability.
   Nothing here touches device state outside the VFPU registers of this thread. */

#include <kubridge.h>
#include <systemctrl.h>
#include "hle_vfpu_cells.h"

/* ---- #356: model getters ---------------------------------------------------- */

/* sceKernelGetModel's user export: SysMemUserForUser NID 0x6373995d (src/rt/nid_names.h).
   PSPSDK ships the name only as a kernel prototype, so the stub is written here. On this
   firmware an unresolved user import returns the caller's $v0 unchanged (measured
   2026-10-23), so the call goes through a wrapper that presets $v0 to HLE_SENTINEL, and
   each row also records the stub's two words as the loader left them: a linked user
   import is `jr $ra` followed by a `syscall` in the delay slot, an unlinked one keeps its
   NID-table words. */
__asm__(
    ".set push\n\t"
    ".section .rodata.sceResident, \"a\"\n\t"
    ".word 0\n"
    "__stub_modulestr_ic_SysMemUserForUser:\n\t"
    ".asciz \"SysMemUserForUser\"\n\t"
    ".align 2\n\t"
    ".section .lib.stub, \"a\", @progbits\n\t"
    ".global __stub_module_ic_SysMemUserForUser\n"
    "__stub_module_ic_SysMemUserForUser:\n\t"
    ".word __stub_modulestr_ic_SysMemUserForUser\n\t"
    ".word 0x40010000\n\t"
    ".word 0x5\n\t"
    ".word __executable_start\n\t"
    ".word __executable_start\n\t"
    ".set pop\n\t"
    ".set push\n\t"
    ".set noreorder\n\t"
    ".section .sceStub.text, \"ax\", @progbits\n\t"
    ".globl ic_sceKernelGetModel\n\t"
    ".type ic_sceKernelGetModel, @function\n\t"
    ".ent ic_sceKernelGetModel, 0\n"
    "ic_sceKernelGetModel:\n\t"
    ".word __stub_module_ic_SysMemUserForUser\n\t"
    ".word 0x6373995d\n\t"
    ".end ic_sceKernelGetModel\n\t"
    ".size ic_sceKernelGetModel, .-ic_sceKernelGetModel\n\t"
    ".section .rodata.sceNid, \"a\"\n\t"
    ".word 0x6373995d\n\t"
    ".set pop\n\t"
    ".text\n");

int ic_sceKernelGetModel(void);

/* wrapper(out): presets $v0 to HLE_SENTINEL, calls the import with no arguments and
   stores the returned $v0 to out[0]. Same frame discipline as HLE_PFV_WRAPPER. The preset
   is before the jal, never in its delay slot: `li` of a 32-bit value is two instructions,
   and the second would run after the call (measured: the first build OR'd 0xa5a5 into
   every result). */
#define IC_V0_WRAPPER(wrapper, target)                                        \
    __asm__(                                                                  \
        ".pushsection .text\n\t"                                              \
        ".set push\n\t"                                                       \
        ".set noreorder\n\t"                                                  \
        ".globl " #wrapper "\n\t"                                             \
        ".ent " #wrapper "\n"                                                 \
        #wrapper ":\n\t"                                                      \
        "addiu $sp, $sp, -32\n\t"                                             \
        "sw $ra, 28($sp)\n\t"                                                 \
        "sw $a0, 24($sp)\n\t"                                                 \
        "li $v0, " HLE_SENTINEL_HEX "\n\t"                                    \
        "jal " #target "\n\t"                                                 \
        "nop\n\t"                                                             \
        "lw $t0, 24($sp)\n\t"                                                 \
        "sw $v0, 0($t0)\n\t"                                                  \
        "lw $ra, 28($sp)\n\t"                                                 \
        "addiu $sp, $sp, 32\n\t"                                              \
        "jr $ra\n\t"                                                          \
        "nop\n\t"                                                             \
        ".end " #wrapper "\n\t"                                               \
        ".set pop\n\t"                                                        \
        ".popsection\n")

IC_V0_WRAPPER(ic_v0_sce_get_model, ic_sceKernelGetModel);
IC_V0_WRAPPER(ic_v0_ku_get_model, kuKernelGetModel);
void ic_v0_sce_get_model(uint32_t *out);
void ic_v0_ku_get_model(uint32_t *out);

/* result = the returned $v0 ($v0 preset to HLE_SENTINEL); out0/out1 = the import stub's
   two words after loading; out2 = the firmware word (sceKernelDevkitVersion). */
static void ic_model_row(const char *case_id, void (*call)(uint32_t *), const void *stub)
{
    hle_step(case_id);
    uint32_t v0 = 0;
    call(&v0);
    const volatile uint32_t *s = (const volatile uint32_t *)stub;
    const uint32_t w[3] = {s[0], s[1], (uint32_t)sceKernelDevkitVersion()};
    hle_record(case_id, "PASS", v0, w, 3);
}

/* The kernel getter itself: SysMemForKernel's sceKernelGetModel (6.60 NID 0x07c586a1; the
   PSPSDK's older NID 0x6373995d), from libpspsysmem_kernel_660.a / libpspkernel.a
   .rodata.sceNid, found with ARK's sctrlHENFindFunction and called through kuKernelCall.
   It only reads a kernel global. The call is made only when the address lies in kernel
   RAM. Row model-kernel-get-model-<nid>: result = the getter's return (ret1, preset to
   HLE_SENTINEL); out0 = the address found (0 = not found); out1 = kuKernelCall's own
   return (0xffffffff when not called); out2 = the NID. */
static void ic_model_kernel(void)
{
    static const uint32_t nids[2] = {0x07c586a1u, 0x6373995du};
    char name[48];
    for (unsigned n = 0; n < 2u; n++) {
        snprintf(name, sizeof(name), "model-kernel-get-model-%08x", (unsigned int)nids[n]);
        hle_step(name);
        const uint32_t addr =
            (uint32_t)sctrlHENFindFunction("sceSystemMemoryManager", "SysMemForKernel", nids[n]);
        uint32_t rc = 0xffffffffu;
        KernelCallArg arg;
        memset(&arg, 0, sizeof(arg));
        arg.ret1 = HLE_SENTINEL;
        if (addr >= 0x88000000u && addr < 0x8c000000u) {
            rc = (uint32_t)kuKernelCall((void *)addr, &arg);
        }
        const uint32_t w[3] = {addr, rc, nids[n]};
        hle_record(name, "PASS", arg.ret1, w, 3);
    }
}

static void ic_model(void)
{
    ic_model_row("model-ku-get-model", ic_v0_ku_get_model, (const void *)kuKernelGetModel);
    ic_model_row("model-sce-get-model", ic_v0_sce_get_model, (const void *)ic_sceKernelGetModel);
    ic_model_row("model-ku-get-model-again", ic_v0_ku_get_model, (const void *)kuKernelGetModel);
    ic_model_row("model-sce-get-model-again", ic_v0_sce_get_model, (const void *)ic_sceKernelGetModel);
    ic_model_kernel();
}

/* ---- #326: random unit carry ------------------------------------------------ */

/* The 2026-10-23 rows fit this state layout: RCX0-7 hold four 32-bit components C0-C3,
   Ck = (RCX(k+4) & 0xffff) << 16 | (RCXk & 0xffff), and a carry whose nibble k sits in
   bits 16-19 of RCXk. Every rule but the next carry out of the C3 step is fixed. Each
   cell writes one state with mtvc, reads RCX back (did the write take?), executes one
   vrndi.s S000 and reads RCX again. Nothing else runs between. */
static const uint32_t s_ic_c2[8] = {0x00000000u, 0x00000001u, 0x7fffffffu, 0x80000000u,
                                    0xfffffffeu, 0xffffffffu, 0x907f6e50u, 0x5a5a5a5au};
static const uint32_t s_ic_c3[8] = {0x00000000u, 0x00000001u, 0x7fffffffu, 0x80000000u,
                                    0xc0000000u, 0xffffffffu, 0xc4d5e6d8u, 0x89abcdefu};
static const uint32_t s_ic_carry[7] = {0x0u, 0x1u, 0x2u, 0x3u, 0xfu, 0x10u, 0xffffu};
/* Second grid: carries wider than 16 bits (all eight nibbles). */
static const uint32_t s_ic_carry_big[7] = {0x00010000u, 0x40000000u, 0x7fff0000u, 0x80000000u,
                                           0x12345678u, 0xfffffffcu, 0xffffffffu};
#define IC_C0 0x2468ace0u
#define IC_C1 0x13579bdfu

/* The carry is 32 bits: nibble k in bits 16-19 of RCXk, k = 0..7 (a seed writes all eight;
   the 2026-10-23 rows show nibbles in RCX4-7 after seeding 0x80000000 and 0xffffffff). */
static void ic_rcx_image(uint32_t *rcx, const uint32_t c[4], uint32_t carry)
{
    for (unsigned k = 0; k < 8u; k++) {
        const uint32_t half = (c[k & 3u] >> (k < 4u ? 0u : 16u)) & 0xffffu;
        rcx[k] = 0x3f800000u | (((carry >> (4u * k)) & 0xfu) << 16) | half;
    }
}

/* out: C000 lanes 0-3 after (lane 0 = the value; lanes 1-3 keep the sentinel), RCX0-7
   after the write, RCX0-7 after the step (20 words). */
static void ic_rnd_step(const uint32_t *rcx_in, const uint32_t *sentinel, uint32_t *out)
{
    __asm__ volatile(
        "lv.q C000, 0(%[sen])\n\t"
        TN_RCX_WRITE("%[st]")
        "addiu $t0, %[out], 16\n\t"
        TN_RCX_READ("$t0")
        ".word 0xd0210000\n\t" /* vrndi.s S000 */
        "sv.q C000, 0(%[out])\n\t"
        "addiu $t0, %[out], 48\n\t"
        TN_RCX_READ("$t0")
        :
        : [st] "r"(rcx_in), [sen] "r"(sentinel), [out] "r"(out)
        : "t0", "t1", "memory");
}

/* Row <stem>-p<pass>-<i>-<j>-<k> (C2 = s_ic_c2[i], C3 = s_ic_c3[j], carry = carries[k];
   C0/C1 = IC_C0/IC_C1; stem rcar = s_ic_carry, rcbig = s_ic_carry_big): result = the
   vrndi.s value (C000 lane 0); out0-7 = RCX0-7 after the write; out8-15 = RCX0-7 after the
   step; out16 = 1 when C000 lanes 1-3 all kept the sentinel. */
static void ic_rnd_carry(unsigned pass, const char *stem, const uint32_t *carries)
{
    static const uint32_t sentinel[4] __attribute__((aligned(16))) = {
        TN_SENTINEL, TN_SENTINEL, TN_SENTINEL, TN_SENTINEL};
    uint32_t rcx[8] __attribute__((aligned(16)));
    uint32_t out[20] __attribute__((aligned(16)));
    char name[48];
    for (unsigned i = 0; i < 8u; i++) {
        snprintf(name, sizeof(name), "%s-p%u-c2-%u", stem, pass, i);
        hle_step(name);
        for (unsigned j = 0; j < 8u; j++) {
            for (unsigned k = 0; k < 7u; k++) {
                const uint32_t c[4] = {IC_C0, IC_C1, s_ic_c2[i], s_ic_c3[j]};
                ic_rcx_image(rcx, c, carries[k]);
                ic_rnd_step(rcx, sentinel, out);
                uint32_t w[17];
                for (unsigned n = 0; n < 16u; n++) {
                    w[n] = out[4u + n];
                }
                w[16] = (out[1] == TN_SENTINEL && out[2] == TN_SENTINEL && out[3] == TN_SENTINEL) ? 1u : 0u;
                snprintf(name, sizeof(name), "%s-p%u-%u-%u-%u", stem, pass, i, j, k);
                hle_record(name, "PASS", out[0], w, 17);
            }
        }
    }
}

/* ---- #326: vscmp ------------------------------------------------------------ */

/* vscmp vd, vs, vt writes the sign of vs - vt per lane. vs = C000 (image 0-3), vt = C100
   (image 16-19), vd = C200 (image 32-35); the same-register form compares C000 with
   itself. Encodings from psp-as (pspdev binutils). */
TN_CELL_FN(ic_vscmp_s, TN_PFX_NONE, "0x6e840008")      /* vscmp.s S200, S000, S100 */
TN_CELL_FN(ic_vscmp_p, TN_PFX_NONE, "0x6e840088")      /* vscmp.p C200, C000, C100 */
TN_CELL_FN(ic_vscmp_t, TN_PFX_NONE, "0x6e848008")      /* vscmp.t C200, C000, C100 */
TN_CELL_FN(ic_vscmp_q, TN_PFX_NONE, "0x6e848088")      /* vscmp.q C200, C000, C100 */
TN_CELL_FN(ic_vscmp_q_same, TN_PFX_NONE, "0x6e808088") /* vscmp.q C200, C000, C000 */

typedef void (*ic_cell_fn)(const uint32_t *in, uint32_t *out, uint32_t *cc);

static const struct {
    const char *name;
    uint32_t vs[4];
    uint32_t vt[4];
} s_ic_vscmp_sets[6] = {
    {"qnan-inf", {0x7fc00000u, 0x7fc00000u, 0x7f800000u, 0xff800000u},
                 {0x7fc00000u, 0x7f800000u, 0x7fc00000u, 0x7fc00000u}},
    {"nan-kinds", {0x7f800001u, 0xffc00000u, 0x7fffffffu, 0x7f800001u},
                  {0x3f800000u, 0xbf800000u, 0x7fffffffu, 0xff800001u}},
    {"inf-inf", {0x7f800000u, 0xff800000u, 0x7f800000u, 0xff800000u},
                {0x7f800000u, 0xff800000u, 0xff800000u, 0x7f800000u}},
    {"zero-nan", {0x00000000u, 0x80000000u, 0x3f800000u, 0xbf800000u},
                 {0x80000000u, 0x00000000u, 0x7fc00000u, 0xffc00000u}},
    {"denorm-finite", {0x00000001u, 0x80000001u, 0x40400000u, 0x40000000u},
                      {0x00000000u, 0x00000000u, 0x40000000u, 0x40400000u}},
    {"nan-mixed", {0x7f800001u, 0x3f800000u, 0x7fc00000u, 0xff800000u},
                  {0xff800001u, 0x7f800001u, 0xff800000u, 0x7fc00000u}},
};

static const struct {
    const char *name;
    uint32_t word;
    ic_cell_fn fn;
} s_ic_vscmp_forms[5] = {
    {"vscmp.s", 0x6e840008u, ic_vscmp_s}, {"vscmp.p", 0x6e840088u, ic_vscmp_p},
    {"vscmp.t", 0x6e848008u, ic_vscmp_t}, {"vscmp.q", 0x6e848088u, ic_vscmp_q},
    {"vscmp.q-same", 0x6e808088u, ic_vscmp_q_same},
};

static uint32_t s_ic_in[TN_IMAGE_WORDS] __attribute__((aligned(64)));
static uint32_t s_ic_out[TN_IMAGE_WORDS] __attribute__((aligned(64)));

/* Row vscmp-p<pass>-<set>-<form>: result = the instruction word; out0-3 = C200 lanes after
   (lanes past the width keep their background value); out4/out5 = VFPU_CC after the preset
   / after the word; out6-9 = vs lanes; out10-13 = vt lanes; out14 = number of image words
   outside C200 that changed; out15 = the first such index (0xffffffff when none). */
static void ic_vscmp(unsigned pass)
{
    char name[64];
    for (unsigned s = 0; s < 6u; s++) {
        snprintf(name, sizeof(name), "vscmp-p%u-%s", pass, s_ic_vscmp_sets[s].name);
        hle_step(name);
        for (unsigned f = 0; f < 5u; f++) {
            for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
                s_ic_in[i] = 0x3f800000u | (i << 16);
            }
            for (unsigned l = 0; l < 4u; l++) {
                s_ic_in[0u + l] = s_ic_vscmp_sets[s].vs[l];
                s_ic_in[16u + l] = s_ic_vscmp_sets[s].vt[l];
            }
            uint32_t cc[2] = {0u, 0u};
            s_ic_vscmp_forms[f].fn(s_ic_in, s_ic_out, cc);
            uint32_t changed = 0u, first = 0xffffffffu;
            for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
                if ((i < 32u || i > 35u) && s_ic_out[i] != s_ic_in[i]) {
                    if (changed++ == 0u) {
                        first = i;
                    }
                }
            }
            const uint32_t w[16] = {
                s_ic_out[32], s_ic_out[33], s_ic_out[34], s_ic_out[35], cc[0], cc[1],
                s_ic_in[0], s_ic_in[1], s_ic_in[2], s_ic_in[3],
                s_ic_in[16], s_ic_in[17], s_ic_in[18], s_ic_in[19], changed, first,
            };
            snprintf(name, sizeof(name), "vscmp-p%u-%s-%s", pass, s_ic_vscmp_sets[s].name,
                     s_ic_vscmp_forms[f].name);
            hle_record(name, "PASS", s_ic_vscmp_forms[f].word, w, 16);
        }
    }
}

static int hle_run(void)
{
    ic_model();
    /* Traps off before the first VFPU cell, as in the vfpu-compare case. */
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    for (unsigned pass = 0; pass < 2u; pass++) {
        ic_vscmp(pass);
        ic_rnd_carry(pass, "rcar", s_ic_carry);
        ic_rnd_carry(pass, "rcbig", s_ic_carry_big);
    }
    return 1;
}

#endif /* HLE_CASE_ISSUES_BATCH */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_VFPU_FORMS

/* #326 VFPU forms (PSP-VFPU-FORMS-001), one launch, register-only: the raw result words of
   vh2f.s/.p (half pairs to float), vuc2i.s (bytes to words), vsocp.s/.p, vwbn.s at eight
   exponents, and one vrndf1.s/vrndf2.s draw from written RCX states. Each form runs under no
   S prefix and the two TN_PFX forms, except vwbn (no prefix at every exponent, P1 at 0x7f)
   and the draws (no prefix). Two passes for repeatability. Every word is emitted as a
   word; the encodings were checked against psp-as (pspdev binutils). Nothing outside the
   VFPU registers of this thread is touched. */

#include "hle_vfpu_cells.h"

/* Source C000 (image 0-3, S000 for a single), destination C200 (image 32-35, S200). */
TN_CELL_SET(vf_h2f_s, "0xd0330008")   /* vh2f.s C200.p, S000 */
TN_CELL_SET(vf_h2f_p, "0xd0330088")   /* vh2f.p C200.q, C000.p */
TN_CELL_FN(vf_h2f_alias, TN_PFX_NONE, "0xd0330080") /* vh2f.p C000.q, C000.p (in place) */
TN_CELL_SET(vf_uc2i, "0xd0380008")    /* vuc2i.s C200.q, S000 */
TN_CELL_SET(vf_socp_s, "0xd0450008")  /* vsocp.s C200.p, S000 */
TN_CELL_SET(vf_socp_p, "0xd0450088")  /* vsocp.p C200.q, C000.p */
TN_CELL_FN(vf_wbn_00, TN_PFX_NONE, "0xd3000008") /* vwbn.s S200, S000, 0x00 */
TN_CELL_FN(vf_wbn_01, TN_PFX_NONE, "0xd3010008")
TN_CELL_FN(vf_wbn_7e, TN_PFX_NONE, "0xd37e0008")
TN_CELL_FN(vf_wbn_7f, TN_PFX_NONE, "0xd37f0008")
TN_CELL_FN(vf_wbn_80, TN_PFX_NONE, "0xd3800008")
TN_CELL_FN(vf_wbn_96, TN_PFX_NONE, "0xd3960008")
TN_CELL_FN(vf_wbn_fe, TN_PFX_NONE, "0xd3fe0008")
TN_CELL_FN(vf_wbn_ff, TN_PFX_NONE, "0xd3ff0008")
TN_CELL_FN(vf_wbn_7f_p1, TN_PFX_P1, "0xd37f0008")

typedef void (*vf_cell_fn)(const uint32_t *in, uint32_t *out, uint32_t *cc);

/* Half pairs, low half first: signed zeros, denormals, the normal limits, 1 and -2,
   infinities, signalling and quiet NaNs, two ordinary values each way. */
static const uint32_t s_vf_halves[12] = {
    0x80000000u, 0x80010001u, 0x020003ffu, 0x3c000400u, 0xc0007bffu, 0xfc007c00u,
    0x7e007c01u, 0xfe007fffu, 0xb7ff3555u, 0x12345640u, 0xfbff8400u, 0xfdff83ffu,
};
/* Byte patterns (two of them float NaN or 1.0 as words). */
static const uint32_t s_vf_bytes[10] = {
    0x00000000u, 0xffffffffu, 0x7f80ff01u, 0x80402010u, 0x01020304u,
    0xfedcba98u, 0x3f800000u, 0x7fc00000u, 0x00000001u, 0x00ff00ffu,
};
/* vsocp operands: signed zeros, the [0, 1] edges, beyond both edges, infinities, NaNs,
   denormals, the neighbours of 1, +-2^-24 and 0.75. */
static const uint32_t s_vf_socp[22] = {
    0x00000000u, 0x80000000u, 0x3e800000u, 0x3f000000u, 0x3f800000u, 0x3fc00000u,
    0x40000000u, 0xbf000000u, 0xbf800000u, 0xc0000000u, 0x7f800000u, 0xff800000u,
    0x7fc00000u, 0xffc00000u, 0x7f800001u, 0x00000001u, 0x80000001u, 0x3f7fffffu,
    0x3f800001u, 0x33800000u, 0xb3800000u, 0x3f400000u,
};
/* vwbn operands: zeros, small and large normals each way, powers of two around 2^23,
   the normal limits, denormals, infinities, NaNs and two arbitrary words. */
static const uint32_t s_vf_wbn[28] = {
    0x00000000u, 0x80000000u, 0x3f800000u, 0x3fc00000u, 0x40000000u, 0x40400000u,
    0x40b00000u, 0x3f000000u, 0x3f400000u, 0x3e800000u, 0x2edbe6ffu, 0x501502f9u,
    0x4b000000u, 0x4b800001u, 0x4e800000u, 0xc0500000u, 0xbe99999au, 0x7f7fffffu,
    0x00800000u, 0x00400000u, 0x00000001u, 0x7f800000u, 0xff800000u, 0x7fc00000u,
    0x7f800001u, 0x40490fdbu, 0x12345678u, 0x8abcdef0u,
};

static const struct {
    const char *name;
    vf_cell_fn fn;
    uint32_t word;
} s_vf_wbn_forms[9] = {
    {"00", vf_wbn_00, 0xd3000008u}, {"01", vf_wbn_01, 0xd3010008u},
    {"7e", vf_wbn_7e, 0xd37e0008u}, {"7f", vf_wbn_7f, 0xd37f0008u},
    {"80", vf_wbn_80, 0xd3800008u}, {"96", vf_wbn_96, 0xd3960008u},
    {"fe", vf_wbn_fe, 0xd3fe0008u}, {"ff", vf_wbn_ff, 0xd3ff0008u},
    {"7f-p1", vf_wbn_7f_p1, 0xd37f0008u},
};

static const char *const s_vf_pfx_names[3] = {"none", "p1", "p2"};
static const uint32_t s_vf_pfx_words[3] = {TN_PFXW_NONE, TN_PFXW_P1, TN_PFXW_P2};

static uint32_t s_vf_in[TN_IMAGE_WORDS] __attribute__((aligned(64)));
static uint32_t s_vf_out[TN_IMAGE_WORDS] __attribute__((aligned(64)));

/* One cell: background image word i = 0x3f800000 | i << 16, source lanes a/b at image 0/1
   (lanes 2/3 keep the background), then the word. Row <name>: result = the instruction
   word; out0-3 = image dest..dest+3 after (C200 = 32, or C000 = 0 for the in-place form);
   out4/out5 = VFPU_CC after the preset / after the word; out6-9 = image 0-3 before;
   out10 = number of image words outside dest..dest+3 that changed; out11 = the first such
   index (0xffffffff when none); out12 = the vpfxs word issued before it (0 = none). */
static void vf_cell(const char *name, vf_cell_fn fn, uint32_t word, uint32_t pfx, uint32_t a,
                    uint32_t b, unsigned dest)
{
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        s_vf_in[i] = 0x3f800000u | (i << 16);
    }
    s_vf_in[0] = a;
    s_vf_in[1] = b;
    uint32_t cc[2] = {0u, 0u};
    fn(s_vf_in, s_vf_out, cc);
    uint32_t changed = 0u, first = 0xffffffffu;
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        if ((i < dest || i > dest + 3u) && s_vf_out[i] != s_vf_in[i]) {
            if (changed++ == 0u) {
                first = i;
            }
        }
    }
    const uint32_t w[13] = {
        s_vf_out[dest], s_vf_out[dest + 1u], s_vf_out[dest + 2u], s_vf_out[dest + 3u],
        cc[0], cc[1], s_vf_in[0], s_vf_in[1], s_vf_in[2], s_vf_in[3], changed, first, pfx,
    };
    hle_record(name, "PASS", word, w, 13);
}

/* Rows h2f-p<pass>-s-<i>-<pfx> (vh2f.s of s_vf_halves[i]), h2f-p<pass>-p-<i>-<pfx> (vh2f.p
   of halves 2i and 2i+1) and h2f-p<pass>-alias-<i> (the same pair, in place). */
static void vf_h2f(unsigned pass)
{
    static const vf_cell_fn s_fns[3] = {vf_h2f_s_none, vf_h2f_s_p1, vf_h2f_s_p2};
    static const vf_cell_fn p_fns[3] = {vf_h2f_p_none, vf_h2f_p_p1, vf_h2f_p_p2};
    char name[64];
    snprintf(name, sizeof(name), "h2f-p%u", pass);
    hle_step(name);
    for (unsigned i = 0; i < 12u; i++) {
        for (unsigned f = 0; f < 3u; f++) {
            snprintf(name, sizeof(name), "h2f-p%u-s-%u-%s", pass, i, s_vf_pfx_names[f]);
            vf_cell(name, s_fns[f], 0xd0330008u, s_vf_pfx_words[f], s_vf_halves[i],
                    TN_SENTINEL, 32u);
        }
    }
    for (unsigned i = 0; i < 6u; i++) {
        for (unsigned f = 0; f < 3u; f++) {
            snprintf(name, sizeof(name), "h2f-p%u-p-%u-%s", pass, i, s_vf_pfx_names[f]);
            vf_cell(name, p_fns[f], 0xd0330088u, s_vf_pfx_words[f], s_vf_halves[2u * i],
                    s_vf_halves[2u * i + 1u], 32u);
        }
        snprintf(name, sizeof(name), "h2f-p%u-alias-%u", pass, i);
        vf_cell(name, vf_h2f_alias, 0xd0330080u, TN_PFXW_NONE, s_vf_halves[2u * i],
                s_vf_halves[2u * i + 1u], 0u);
    }
}

/* Rows uc2i-p<pass>-<i>-<pfx>: vuc2i.s of s_vf_bytes[i]. */
static void vf_uc2i(unsigned pass)
{
    static const vf_cell_fn fns[3] = {vf_uc2i_none, vf_uc2i_p1, vf_uc2i_p2};
    char name[64];
    snprintf(name, sizeof(name), "uc2i-p%u", pass);
    hle_step(name);
    for (unsigned i = 0; i < 10u; i++) {
        for (unsigned f = 0; f < 3u; f++) {
            snprintf(name, sizeof(name), "uc2i-p%u-%u-%s", pass, i, s_vf_pfx_names[f]);
            vf_cell(name, fns[f], 0xd0380008u, s_vf_pfx_words[f], s_vf_bytes[i], TN_SENTINEL,
                    32u);
        }
    }
}

/* Rows socp-p<pass>-s-<i>-<pfx> (vsocp.s of s_vf_socp[i]) and socp-p<pass>-p-<i>-<pfx>
   (vsocp.p of operands 2i and 2i+1). */
static void vf_socp(unsigned pass)
{
    static const vf_cell_fn s_fns[3] = {vf_socp_s_none, vf_socp_s_p1, vf_socp_s_p2};
    static const vf_cell_fn p_fns[3] = {vf_socp_p_none, vf_socp_p_p1, vf_socp_p_p2};
    char name[64];
    snprintf(name, sizeof(name), "socp-p%u", pass);
    hle_step(name);
    for (unsigned i = 0; i < 22u; i++) {
        for (unsigned f = 0; f < 3u; f++) {
            snprintf(name, sizeof(name), "socp-p%u-s-%u-%s", pass, i, s_vf_pfx_names[f]);
            vf_cell(name, s_fns[f], 0xd0450008u, s_vf_pfx_words[f], s_vf_socp[i], TN_SENTINEL,
                    32u);
        }
    }
    for (unsigned i = 0; i < 11u; i++) {
        for (unsigned f = 0; f < 3u; f++) {
            snprintf(name, sizeof(name), "socp-p%u-p-%u-%s", pass, i, s_vf_pfx_names[f]);
            vf_cell(name, p_fns[f], 0xd0450088u, s_vf_pfx_words[f], s_vf_socp[2u * i],
                    s_vf_socp[2u * i + 1u], 32u);
        }
    }
}

/* Rows wbn-p<pass>-<form>-<i>: vwbn.s of s_vf_wbn[i] at the form's exponent. */
static void vf_wbn(unsigned pass)
{
    char name[64];
    for (unsigned f = 0; f < 9u; f++) {
        snprintf(name, sizeof(name), "wbn-p%u-%s", pass, s_vf_wbn_forms[f].name);
        hle_step(name);
        for (unsigned i = 0; i < 28u; i++) {
            snprintf(name, sizeof(name), "wbn-p%u-%s-%u", pass, s_vf_wbn_forms[f].name, i);
            vf_cell(name, s_vf_wbn_forms[f].fn, s_vf_wbn_forms[f].word,
                    f == 8u ? TN_PFXW_P1 : TN_PFXW_NONE, s_vf_wbn[i], TN_SENTINEL, 32u);
        }
    }
}

/* ---- random-unit float draws ------------------------------------------------ */

/* RCX image of four 32-bit components and a 32-bit carry, as ic_rcx_image in the
   hle-issues-batch section: Ck = (RCX(k+4) & 0xffff) << 16 | (RCXk & 0xffff), carry nibble
   k in bits 16-19 of RCXk. */
static void vf_rcx_image(uint32_t *rcx, const uint32_t c[4], uint32_t carry)
{
    for (unsigned k = 0; k < 8u; k++) {
        const uint32_t half = (c[k & 3u] >> (k < 4u ? 0u : 16u)) & 0xffffu;
        rcx[k] = 0x3f800000u | (((carry >> (4u * k)) & 0xfu) << 16) | half;
    }
}

/* out: C000 lanes 0-3 after (lane 0 = the draw), RCX0-7 after the write, RCX0-7 after the
   draw (20 words). */
static void vf_rndf1_step(const uint32_t *rcx_in, const uint32_t *sentinel, uint32_t *out)
{
    __asm__ volatile(
        "lv.q C000, 0(%[sen])\n\t"
        TN_RCX_WRITE("%[st]")
        "addiu $t0, %[out], 16\n\t"
        TN_RCX_READ("$t0")
        ".word 0xd0220000\n\t" /* vrndf1.s S000 */
        "sv.q C000, 0(%[out])\n\t"
        "addiu $t0, %[out], 48\n\t"
        TN_RCX_READ("$t0")
        :
        : [st] "r"(rcx_in), [sen] "r"(sentinel), [out] "r"(out)
        : "t0", "t1", "memory");
}

static void vf_rndf2_step(const uint32_t *rcx_in, const uint32_t *sentinel, uint32_t *out)
{
    __asm__ volatile(
        "lv.q C000, 0(%[sen])\n\t"
        TN_RCX_WRITE("%[st]")
        "addiu $t0, %[out], 16\n\t"
        TN_RCX_READ("$t0")
        ".word 0xd0230000\n\t" /* vrndf2.s S000 */
        "sv.q C000, 0(%[out])\n\t"
        "addiu $t0, %[out], 48\n\t"
        TN_RCX_READ("$t0")
        :
        : [st] "r"(rcx_in), [sen] "r"(sentinel), [out] "r"(out)
        : "t0", "t1", "memory");
}

static const uint32_t s_vf_c2[8] = {0x00000000u, 0x00000001u, 0x7fffffffu, 0x80000000u,
                                    0xfffffffeu, 0xffffffffu, 0x907f6e50u, 0x5a5a5a5au};
static const uint32_t s_vf_c3[8] = {0x00000000u, 0x00000001u, 0x7fffffffu, 0x80000000u,
                                    0xc0000000u, 0xffffffffu, 0xc4d5e6d8u, 0x89abcdefu};
static const uint32_t s_vf_carry[8] = {0x0u, 0x1u, 0x2u, 0x3u, 0xfu, 0x10u, 0xffffu,
                                       0x12345678u};

/* Rows rndf<n>-p<pass>-<k>: k < 16 draws from C0 = 0x2468ace0 + k * 0x11111111,
   C1 = 0x13579bdf ^ (k << 20), C2 = s_vf_c2[k & 7], C3 = s_vf_c3[(3k) & 7],
   carry = s_vf_carry[k & 7]; k = 16 from the thread-entry RCX (0x3f800001, 0x3f800002,
   0x3f800004, 0x3f800008, then 0x3f800000 four times). result = the draw (C000 lane 0);
   out0-7 = RCX0-7 after the write; out8-15 = RCX0-7 after the draw; out16 = 1 when C000
   lanes 1-3 all kept the sentinel. */
static void vf_rndf(unsigned pass, unsigned which)
{
    static const uint32_t sentinel[4] __attribute__((aligned(16))) = {
        TN_SENTINEL, TN_SENTINEL, TN_SENTINEL, TN_SENTINEL};
    uint32_t rcx[8] __attribute__((aligned(16)));
    uint32_t out[20] __attribute__((aligned(16)));
    char name[48];
    snprintf(name, sizeof(name), "rndf%u-p%u", which, pass);
    hle_step(name);
    for (unsigned k = 0; k < 17u; k++) {
        if (k < 16u) {
            const uint32_t c[4] = {0x2468ace0u + k * 0x11111111u, 0x13579bdfu ^ (k << 20),
                                   s_vf_c2[k & 7u], s_vf_c3[(3u * k) & 7u]};
            vf_rcx_image(rcx, c, s_vf_carry[k & 7u]);
        } else {
            for (unsigned r = 0; r < 8u; r++) {
                rcx[r] = r < 4u ? (0x3f800000u | (1u << r)) : 0x3f800000u;
            }
        }
        if (which == 1u) {
            vf_rndf1_step(rcx, sentinel, out);
        } else {
            vf_rndf2_step(rcx, sentinel, out);
        }
        uint32_t w[17];
        for (unsigned n = 0; n < 16u; n++) {
            w[n] = out[4u + n];
        }
        w[16] = (out[1] == TN_SENTINEL && out[2] == TN_SENTINEL && out[3] == TN_SENTINEL) ? 1u : 0u;
        snprintf(name, sizeof(name), "rndf%u-p%u-%u", which, pass, k);
        hle_record(name, "PASS", out[0], w, 17);
    }
}

static int hle_run(void)
{
    /* Traps off before the first VFPU cell, as in the vfpu-compare case. */
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    for (unsigned pass = 0; pass < 2u; pass++) {
        vf_h2f(pass);
        vf_uc2i(pass);
        vf_socp(pass);
        vf_wbn(pass);
        vf_rndf(pass, 1u);
        vf_rndf(pass, 2u);
    }
    return 1;
}

#endif /* HLE_CASE_VFPU_FORMS */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_VFPU_FUNNEL

/* #326 VFPU funnel add and average (PSP-VFPU-FUNNEL-001), one launch, register-only: the
   raw result word of vfad (sum of the source lanes) and vavg (their average) at .p, .t and
   .q, each over 26 operand sets chosen so that different summation rules (sequential,
   pairwise, exact-then-round) and different divisions (x / n against x * (1 / n)) give
   different bits, and each under no S prefix and the two TN_PFX forms. Two passes for
   repeatability. Every word is emitted as a word. The encodings are VFPU op 0x34, jump
   field 2, op field 6 (vfad) or 7 (vavg), vd S200 (0x08), vs C000, size from bit 7 (.p),
   bit 15 (.t) or both (.q); they sit one op field above the vsocp words of the vfpu-forms
   case (0xd0450008, 0xd0450088), which were checked against psp-as (pspdev binutils).
   Nothing outside the VFPU registers of this thread is touched. */

#include "hle_vfpu_cells.h"

/* Source C000 (image 0-3), destination S200 (image 32). */
TN_CELL_SET(fn_fad_p, "0xd0460088")   /* vfad.p S200, C000.p */
TN_CELL_SET(fn_fad_t, "0xd0468008")   /* vfad.t S200, C000.t */
TN_CELL_SET(fn_fad_q, "0xd0468088")   /* vfad.q S200, C000.q */
TN_CELL_SET(fn_avg_p, "0xd0470088")   /* vavg.p S200, C000.p */
TN_CELL_SET(fn_avg_t, "0xd0478008")   /* vavg.t S200, C000.t */
TN_CELL_SET(fn_avg_q, "0xd0478088")   /* vavg.q S200, C000.q */

typedef void (*fn_cell_fn)(const uint32_t *in, uint32_t *out, uint32_t *cc);

/* Operand sets, four float32 words each (lanes 0-3 of C000); lanes beyond the size are
   ignored by the instruction but still recorded. */
static const uint32_t s_fn_ops[26][4] = {
    /*  0: 2^24, 1, 1, 1: sequential 2^24, pairwise 2^24+2, exact-then-round 2^24+4 */
    {0x4b800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u},
    /*  1: 1, 1, 1, 2^24: the same values in reverse order */
    {0x3f800000u, 0x3f800000u, 0x3f800000u, 0x4b800000u},
    /*  2: 1, 2^-24, 2^-24, 0: sequential 1, exact 1 + 2^-23 */
    {0x3f800000u, 0x33800000u, 0x33800000u, 0x00000000u},
    /*  3: 2^-24, 2^-24, 1, 0 */
    {0x33800000u, 0x33800000u, 0x3f800000u, 0x00000000u},
    /*  4: 1e30, -1e30, 3, 0: the cancellation comes first */
    {0x7149f2cau, 0xf149f2cau, 0x40400000u, 0x00000000u},
    /*  5: 3, 1e30, -1e30, 0: the 3 is absorbed before the cancellation */
    {0x40400000u, 0x7149f2cau, 0xf149f2cau, 0x00000000u},
    /*  6: 5, 0, 0, 0 */
    {0x40a00000u, 0x00000000u, 0x00000000u, 0x00000000u},
    /*  7: 7, 0, 0, 0 */
    {0x40e00000u, 0x00000000u, 0x00000000u, 0x00000000u},
    /*  8: 1, 0, 0, 0 */
    {0x3f800000u, 0x00000000u, 0x00000000u, 0x00000000u},
    /*  9: 100, 0, 0, 0 */
    {0x42c80000u, 0x00000000u, 0x00000000u, 0x00000000u},
    /* 10: 1, 1, 1, 0: vavg.t is 3 / 3 or 3 * (1 / 3) */
    {0x3f800000u, 0x3f800000u, 0x3f800000u, 0x00000000u},
    /* 11: 1, 2, 3, 4 */
    {0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u},
    /* 12: -0 four times */
    {0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u},
    /* 13: 0, -0, 0, -0 */
    {0x00000000u, 0x80000000u, 0x00000000u, 0x80000000u},
    /* 14: +inf, -inf, 1, 1 */
    {0x7f800000u, 0xff800000u, 0x3f800000u, 0x3f800000u},
    /* 15: +inf, 1, 1, 1 */
    {0x7f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u},
    /* 16: quiet NaN, 1, 1, 1 */
    {0x7fc00000u, 0x3f800000u, 0x3f800000u, 0x3f800000u},
    /* 17: 1, 1, signalling NaN, 1 */
    {0x3f800000u, 0x3f800000u, 0x7f800001u, 0x3f800000u},
    /* 18: negative NaN with a payload, 0, 0, 0 */
    {0xffc00001u, 0x00000000u, 0x00000000u, 0x00000000u},
    /* 19: the smallest denormal twice */
    {0x00000001u, 0x00000001u, 0x00000000u, 0x00000000u},
    /* 20: a large denormal (2^-127) twice */
    {0x00400000u, 0x00400000u, 0x00000000u, 0x00000000u},
    /* 21: FLT_MAX twice (overflow) */
    {0x7f7fffffu, 0x7f7fffffu, 0x00000000u, 0x00000000u},
    /* 22: -FLT_MAX twice */
    {0xff7fffffu, 0xff7fffffu, 0x00000000u, 0x00000000u},
    /* 23: 1 + 2^-23 four times */
    {0x3f800001u, 0x3f800001u, 0x3f800001u, 0x3f800001u},
    /* 24: 1.5, 2.25, -0.125, 1e-8 */
    {0x3fc00000u, 0x40100000u, 0xbe000000u, 0x322bcc77u},
    /* 25: 1e30, 3, -1e30, 0: the 3 sits between the cancelling pair */
    {0x7149f2cau, 0x40400000u, 0xf149f2cau, 0x00000000u},
};

static const char *const s_fn_forms[2] = {"fad", "avg"};
static const char *const s_fn_sizes[3] = {"p", "t", "q"};
static const fn_cell_fn s_fn_cells[2][3][3] = {
    {{fn_fad_p_none, fn_fad_p_p1, fn_fad_p_p2}, {fn_fad_t_none, fn_fad_t_p1, fn_fad_t_p2},
     {fn_fad_q_none, fn_fad_q_p1, fn_fad_q_p2}},
    {{fn_avg_p_none, fn_avg_p_p1, fn_avg_p_p2}, {fn_avg_t_none, fn_avg_t_p1, fn_avg_t_p2},
     {fn_avg_q_none, fn_avg_q_p1, fn_avg_q_p2}},
};
static const uint32_t s_fn_words[2][3] = {
    {0xd0460088u, 0xd0468008u, 0xd0468088u},
    {0xd0470088u, 0xd0478008u, 0xd0478088u},
};
static const char *const s_fn_pfx_names[3] = {"none", "p1", "p2"};
static const uint32_t s_fn_pfx_words[3] = {TN_PFXW_NONE, TN_PFXW_P1, TN_PFXW_P2};

static uint32_t s_fn_in[TN_IMAGE_WORDS] __attribute__((aligned(64)));
static uint32_t s_fn_out[TN_IMAGE_WORDS] __attribute__((aligned(64)));

/* One cell: background image word i = 0x3f800000 | i << 16, source lanes at image 0-3, then
   the word. Row <name>: result = the instruction word; out0-3 = image 32-35 after (S200 and
   the three words after it); out4/out5 = VFPU_CC after the preset / after the word;
   out6-9 = image 0-3 before; out10 = number of image words outside 32-35 that changed;
   out11 = the first such index (0xffffffff when none); out12 = the vpfxs word issued
   before it (0 = none). */
static void fn_cell(const char *name, fn_cell_fn fn, uint32_t word, uint32_t pfx,
                    const uint32_t *lanes)
{
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        s_fn_in[i] = 0x3f800000u | (i << 16);
    }
    for (unsigned i = 0; i < 4u; i++) {
        s_fn_in[i] = lanes[i];
    }
    uint32_t cc[2] = {0u, 0u};
    fn(s_fn_in, s_fn_out, cc);
    uint32_t changed = 0u, first = 0xffffffffu;
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        if ((i < 32u || i > 35u) && s_fn_out[i] != s_fn_in[i]) {
            if (changed++ == 0u) {
                first = i;
            }
        }
    }
    const uint32_t w[13] = {
        s_fn_out[32], s_fn_out[33], s_fn_out[34], s_fn_out[35], cc[0], cc[1], s_fn_in[0],
        s_fn_in[1], s_fn_in[2], s_fn_in[3], changed, first, pfx,
    };
    hle_record(name, "PASS", word, w, 13);
}

/* Rows fad-p<pass>-<size>-<i>-<pfx> and avg-p<pass>-<size>-<i>-<pfx>: the form at the size
   over operand set i, one step per (form, pass, size) before its cells. */
static void fn_group(unsigned pass, unsigned form, unsigned size)
{
    char name[64];
    snprintf(name, sizeof(name), "%s-p%u-%s", s_fn_forms[form], pass, s_fn_sizes[size]);
    hle_step(name);
    for (unsigned i = 0; i < 26u; i++) {
        for (unsigned f = 0; f < 3u; f++) {
            snprintf(name, sizeof(name), "%s-p%u-%s-%u-%s", s_fn_forms[form], pass,
                     s_fn_sizes[size], i, s_fn_pfx_names[f]);
            fn_cell(name, s_fn_cells[form][size][f], s_fn_words[form][size],
                    s_fn_pfx_words[f], s_fn_ops[i]);
        }
    }
}

static int hle_run(void)
{
    /* Traps off before the first VFPU cell, as in the vfpu-compare case. */
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    for (unsigned pass = 0; pass < 2u; pass++) {
        for (unsigned form = 0; form < 2u; form++) {
            for (unsigned size = 0; size < 3u; size++) {
                fn_group(pass, form, size);
            }
        }
    }
    return 1;
}

#endif /* HLE_CASE_VFPU_FUNNEL */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_VFPU_SORT

/* #326 VFPU sort steps (PSP-VFPU-SORT-001), one launch, register-only: the raw result words
   of vsrt1.q, vsrt2.q, vsrt3.q and vsrt4.q (the min/max compare-exchange of the pairs
   (x, y) and (z, w), (x, w) and (y, z), and so on) over 24 operand sets that put the
   interesting pair in several lane positions, each under no S prefix and the two TN_PFX
   forms. Two passes for repeatability. Only the .q form exists. Every word is emitted as a
   word. The encodings are VFPU op 0x34, jump field 2, op field 0, 1, 8 and 9 (vsrt1, vsrt2,
   vsrt3, vsrt4), vd C200 (0x08), vs C000, size .q (bits 7 and 15). Nothing outside the VFPU
   registers of this thread is touched. */

#include "hle_vfpu_cells.h"

/* Source C000 (image 0-3), destination C200 (image 32-35, all four lanes are results). */
TN_CELL_SET(st_srt1, "0xd0408088")   /* vsrt1.q C200, C000 */
TN_CELL_SET(st_srt2, "0xd0418088")   /* vsrt2.q C200, C000 */
TN_CELL_SET(st_srt3, "0xd0488088")   /* vsrt3.q C200, C000 */
TN_CELL_SET(st_srt4, "0xd0498088")   /* vsrt4.q C200, C000 */

typedef void (*st_cell_fn)(const uint32_t *in, uint32_t *out, uint32_t *cc);

/* Operand sets, four float32 words each (lanes 0-3 of C000). */
static const uint32_t s_st_ops[24][4] = {
    /*  0: 1, 2, 3, 4 */
    {0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u},
    /*  1: 4, 3, 2, 1 */
    {0x40800000u, 0x40400000u, 0x40000000u, 0x3f800000u},
    /*  2: 2, 1, 4, 3 */
    {0x40000000u, 0x3f800000u, 0x40800000u, 0x40400000u},
    /*  3: -1, 1, -2, 2 */
    {0xbf800000u, 0x3f800000u, 0xc0000000u, 0x40000000u},
    /*  4: 5 four times */
    {0x40a00000u, 0x40a00000u, 0x40a00000u, 0x40a00000u},
    /*  5: +0, -0, -0, +0 */
    {0x00000000u, 0x80000000u, 0x80000000u, 0x00000000u},
    /*  6: -0, +0, +0, -0 */
    {0x80000000u, 0x00000000u, 0x00000000u, 0x80000000u},
    /*  7: qNaN, 1, 1, qNaN */
    {0x7fc00000u, 0x3f800000u, 0x3f800000u, 0x7fc00000u},
    /*  8: 1, qNaN, qNaN, 1 */
    {0x3f800000u, 0x7fc00000u, 0x7fc00000u, 0x3f800000u},
    /*  9: -qNaN, 1, 1, -qNaN */
    {0xffc00000u, 0x3f800000u, 0x3f800000u, 0xffc00000u},
    /* 10: signalling NaN, -1, -1, signalling NaN */
    {0x7f800001u, 0xbf800000u, 0xbf800000u, 0x7f800001u},
    /* 11: qNaN, -qNaN, -qNaN, qNaN */
    {0x7fc00000u, 0xffc00000u, 0xffc00000u, 0x7fc00000u},
    /* 12: NaN payloads 0x7fc12345, 0x7fc00001, 0x7fc00001, 0x7fc12345 */
    {0x7fc12345u, 0x7fc00001u, 0x7fc00001u, 0x7fc12345u},
    /* 13: +inf, -inf, -inf, +inf */
    {0x7f800000u, 0xff800000u, 0xff800000u, 0x7f800000u},
    /* 14: +inf, qNaN, qNaN, +inf */
    {0x7f800000u, 0x7fc00000u, 0x7fc00000u, 0x7f800000u},
    /* 15: -inf, 1, 1, -inf */
    {0xff800000u, 0x3f800000u, 0x3f800000u, 0xff800000u},
    /* 16: smallest denormal, 0, 0, smallest denormal */
    {0x00000001u, 0x00000000u, 0x00000000u, 0x00000001u},
    /* 17: -smallest denormal, 0, 0, -smallest denormal */
    {0x80000001u, 0x00000000u, 0x00000000u, 0x80000001u},
    /* 18: +denormal, -denormal, -denormal, +denormal */
    {0x00000001u, 0x80000001u, 0x80000001u, 0x00000001u},
    /* 19: denormal 1, denormal 2, denormal 2, denormal 1 */
    {0x00000001u, 0x00000002u, 0x00000002u, 0x00000001u},
    /* 20: 1, 1 + 2^-23, 1 + 2^-23, 1 */
    {0x3f800000u, 0x3f800001u, 0x3f800001u, 0x3f800000u},
    /* 21: FLT_MAX, +inf, +inf, FLT_MAX */
    {0x7f7fffffu, 0x7f800000u, 0x7f800000u, 0x7f7fffffu},
    /* 22: 1, 1, 0, 0 (ties across the pairs) */
    {0x3f800000u, 0x3f800000u, 0x00000000u, 0x00000000u},
    /* 23: -2, -1, 1, 2 (ascending across zero) */
    {0xc0000000u, 0xbf800000u, 0x3f800000u, 0x40000000u},
};

static const char *const s_st_forms[4] = {"srt1", "srt2", "srt3", "srt4"};
static const st_cell_fn s_st_cells[4][3] = {
    {st_srt1_none, st_srt1_p1, st_srt1_p2}, {st_srt2_none, st_srt2_p1, st_srt2_p2},
    {st_srt3_none, st_srt3_p1, st_srt3_p2}, {st_srt4_none, st_srt4_p1, st_srt4_p2},
};
static const uint32_t s_st_words[4] = {0xd0408088u, 0xd0418088u, 0xd0488088u, 0xd0498088u};
static const char *const s_st_pfx_names[3] = {"none", "p1", "p2"};
static const uint32_t s_st_pfx_words[3] = {TN_PFXW_NONE, TN_PFXW_P1, TN_PFXW_P2};

static uint32_t s_st_in[TN_IMAGE_WORDS] __attribute__((aligned(64)));
static uint32_t s_st_out[TN_IMAGE_WORDS] __attribute__((aligned(64)));

/* One cell: background image word i = 0x3f800000 | i << 16, source lanes at image 0-3, then
   the word. Row <name>: result = the instruction word; out0-3 = image 32-35 after (C200);
   out4/out5 = VFPU_CC after the preset / after the word; out6-9 = image 0-3 before;
   out10 = number of image words outside 32-35 that changed; out11 = the first such index
   (0xffffffff when none); out12 = the vpfxs word issued before it (0 = none). */
static void st_cell(const char *name, st_cell_fn fn, uint32_t word, uint32_t pfx,
                    const uint32_t *lanes)
{
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        s_st_in[i] = 0x3f800000u | (i << 16);
    }
    for (unsigned i = 0; i < 4u; i++) {
        s_st_in[i] = lanes[i];
    }
    uint32_t cc[2] = {0u, 0u};
    fn(s_st_in, s_st_out, cc);
    uint32_t changed = 0u, first = 0xffffffffu;
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        if ((i < 32u || i > 35u) && s_st_out[i] != s_st_in[i]) {
            if (changed++ == 0u) {
                first = i;
            }
        }
    }
    const uint32_t w[13] = {
        s_st_out[32], s_st_out[33], s_st_out[34], s_st_out[35], cc[0], cc[1], s_st_in[0],
        s_st_in[1], s_st_in[2], s_st_in[3], changed, first, pfx,
    };
    hle_record(name, "PASS", word, w, 13);
}

/* Rows srt<n>-p<pass>-<i>-<pfx>: the form over operand set i, one step per (form, pass)
   before its cells. */
static void st_group(unsigned pass, unsigned form)
{
    char name[64];
    snprintf(name, sizeof(name), "%s-p%u", s_st_forms[form], pass);
    hle_step(name);
    for (unsigned i = 0; i < 24u; i++) {
        for (unsigned f = 0; f < 3u; f++) {
            snprintf(name, sizeof(name), "%s-p%u-%u-%s", s_st_forms[form], pass, i,
                     s_st_pfx_names[f]);
            st_cell(name, s_st_cells[form][f], s_st_words[form], s_st_pfx_words[f],
                    s_st_ops[i]);
        }
    }
}

static int hle_run(void)
{
    /* Traps off before the first VFPU cell, as in the vfpu-compare case. */
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    for (unsigned pass = 0; pass < 2u; pass++) {
        for (unsigned form = 0; form < 4u; form++) {
            st_group(pass, form);
        }
    }
    return 1;
}

#endif /* HLE_CASE_VFPU_SORT */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_VFPU_FORMS2

/* #326 VFPU forms, second batch (PSP-VFPU-FORMS2-001), one launch, register-only: the raw
   result words of the forms the runtime does not model yet. vrnds.s (the random seed, each of
   24 seed words from two written RCX states, followed by one vrndi.s draw), vrndi, vrndf1 and
   vrndf2 at .p, .t and .q from eight written RCX states, vf2h.p and vf2h.q over 28 operand
   quads, and vc2i.s over the byte patterns of the vfpu-forms case. vf2h and vc2i run under no
   S prefix and the two TN_PFX forms. Two passes for repeatability. Every word is emitted as a
   word; the encodings were checked against psp-as (pspdev binutils): VFPU op 0x34, jump field
   1, op field 0 (vrnds), 1 (vrndi), 2 (vrndf1), 3 (vrndf2), 18 (vf2h) and 25 (vc2i), size
   bits 7 (.p) and 15 (.t), both for .q. Nothing outside the VFPU registers of this thread is
   touched. */

#include "hle_vfpu_cells.h"

/* Source C000 (image 0-3, S000 for a single), destination C200 (image 32-35, S200). */
TN_CELL_SET(f2_f2h_p, "0xd0320088")   /* vf2h.p S200, C000.p */
TN_CELL_SET(f2_f2h_q, "0xd0328088")   /* vf2h.q C200.p, C000.q */
TN_CELL_SET(f2_c2i, "0xd0390008")     /* vc2i.s C200.q, S000 */

typedef void (*f2_cell_fn)(const uint32_t *in, uint32_t *out, uint32_t *cc);

/* Operand quads, four float32 words each (lanes 0-3 of C000; the .p form reads lanes 0-1). */
static const uint32_t s_f2_quads[28][4] = {
    /*  0: +0, -0, smallest denormal, -smallest denormal */
    {0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u},
    /*  1: largest float denormal, 2^-14, its float neighbour below and above */
    {0x007fffffu, 0x38800000u, 0x387fffffu, 0x38800001u},
    /*  2: -2^-14, 2^-24, 2^-15, 1.5 * 2^-24 */
    {0xb8800000u, 0x33800000u, 0x38000000u, 0x33c00000u},
    /*  3: 0.5 * 2^-24 (tie) and its neighbours, 1.5 * 2^-24 + 1ulp */
    {0x33000000u, 0x33000001u, 0x32ffffffu, 0x33c00001u},
    /*  4: 1.5 * 2^-24 - 1ulp, largest half denormal, -2^-24, -1.5 * 2^-24 */
    {0x33bfffffu, 0x387fc000u, 0xb3800000u, 0xb3c00000u},
    /*  5: 1 + 2^-11 (tie), its neighbours, 1 + 3 * 2^-11 (tie) */
    {0x3f801000u, 0x3f800fffu, 0x3f801001u, 0x3f803000u},
    /*  6: 1 + 3 * 2^-11 neighbours, -(1 + 2^-11), -(1 + 3 * 2^-11) */
    {0x3f802fffu, 0x3f803001u, 0xbf801000u, 0xbf803000u},
    /*  7: 65504, 65519, 65520, 65535 */
    {0x477fe000u, 0x477fef00u, 0x477ff000u, 0x477fff00u},
    /*  8: 65536, 1e6, -65504, -65520 */
    {0x47800000u, 0x49742400u, 0xc77fe000u, 0xc77ff000u},
    /*  9: +inf, -inf, quiet NaN, negative quiet NaN */
    {0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u},
    /* 10: signalling NaN, negative signalling NaN, quiet NaN payload, negative quiet NaN payload */
    {0x7f800001u, 0xff800001u, 0x7fc12345u, 0xffd54321u},
    /* 11: signalling NaN payload, negative one, all-ones payload NaNs */
    {0x7fa5a5a5u, 0xffbfffffu, 0x7fffffffu, 0xffffffffu},
    /* 12: 1, -2, 0.333, 3.14159 */
    {0x3f800000u, 0xc0000000u, 0x3eaa7efau, 0x40490fd0u},
    /* 13: quiet NaN against finite values and -inf in lane 3 */
    {0x7fc00000u, 0x3f800000u, 0x477ff000u, 0xff800000u},
    /* 14: finite lane 0 against a signalling NaN in lane 1 */
    {0x3f800000u, 0x7f800001u, 0x33800000u, 0x477fe000u},
    /* 15: +inf with +0, then 65519 and a tie neighbour */
    {0x7f800000u, 0x00000000u, 0x477fef00u, 0x33000001u},
    /* 16: -inf with -0, -denormal and 1 + 2^-11 */
    {0xff800000u, 0x80000000u, 0x80000001u, 0x3f801000u},
    /* 17: 65519, 65520 (tie), 65504, 65535 */
    {0x477fef00u, 0x477ff000u, 0x477fe000u, 0x477fff00u},
    /* 18: FLT_MAX, -FLT_MAX, FLT_MIN, -FLT_MIN */
    {0x7f7fffffu, 0xff7fffffu, 0x00800000u, 0x80800000u},
    /* 19: 2^-15 neighbours, 2^-24 neighbours */
    {0x38000001u, 0x37ffffffu, 0x33800001u, 0x337fffffu},
    /* 20: 0.75, 2.5, 3 and 3.5 times 2^-24 */
    {0x33400000u, 0x34200000u, 0x34400000u, 0x34600000u},
    /* 21: 1/3, -1/3, 0.1, 100 */
    {0x3eaaaaabu, 0xbeaaaaabu, 0x3dcccccdu, 0x42c80000u},
    /* 22: 2 + 2^-10 (tie) at exponent 1, neighbours, 2 + 3 * 2^-10 */
    {0x40001000u, 0x40000fffu, 0x40001001u, 0x40003000u},
    /* 23: quiet and signalling NaN payload 1 of each sign */
    {0x7fc00001u, 0xffc00001u, 0x7f800002u, 0xff800002u},
    /* 24: -0, +0, quiet NaN, 1 */
    {0x80000000u, 0x00000000u, 0x7fc00000u, 0x3f800000u},
    /* 25: 65536 neighbours below and above, 65520 neighbours */
    {0x477fffffu, 0x47800001u, 0x477ff001u, 0x477fefffu},
    /* 26: arbitrary words */
    {0xc0600000u, 0x12345678u, 0x8abcdef0u, 0x3727c5acu},
    /* 27: denormal, 2^-14 and a normal, -inf */
    {0x00000002u, 0xb8800000u, 0x3f000000u, 0xff800000u},
};

/* vc2i.s operands: the byte patterns of the vfpu-forms case. */
static const uint32_t s_f2_bytes[10] = {
    0x00000000u, 0xffffffffu, 0x7f80ff01u, 0x80402010u, 0x01020304u,
    0xfedcba98u, 0x3f800000u, 0x7fc00000u, 0x00000001u, 0x00ff00ffu,
};

/* vrnds.s seed words. */
static const uint32_t s_f2_seeds[24] = {
    0x00000000u, /* 0 */
    0x00000001u, /* 1 */
    0x80000000u, /* sign bit */
    0xffffffffu, /* all ones */
    0x3f800000u, /* 1.0f */
    0x7fc00000u, /* quiet NaN */
    0x12345678u, /* arbitrary */
    0xdeadbeefu, /* arbitrary */
    0x00000002u, /* walking bit 1 */
    0x00000100u, /* walking bit 8 */
    0x00010000u, /* walking bit 16 */
    0x01000000u, /* walking bit 24 */
    0x40000000u, /* walking bit 30 */
    0x7fffffffu, /* all but the sign */
    0x55555555u, /* alternating */
    0xaaaaaaaau, /* alternating */
    0x0000ffffu, /* low half */
    0xffff0000u, /* high half */
    0x2468ace0u, /* RCX image word */
    0x13579bdfu, /* RCX image word */
    0x907f6e50u, /* RCX image word */
    0xc4d5e6d8u, /* RCX image word */
    0x3f800001u, /* 1.0f + 1ulp */
    0x00000fffu, /* twelve ones */
};

static const char *const s_f2_pfx_names[3] = {"none", "p1", "p2"};
static const uint32_t s_f2_pfx_words[3] = {TN_PFXW_NONE, TN_PFXW_P1, TN_PFXW_P2};

static uint32_t s_f2_in[TN_IMAGE_WORDS] __attribute__((aligned(64)));
static uint32_t s_f2_out[TN_IMAGE_WORDS] __attribute__((aligned(64)));

/* One cell: background image word i = 0x3f800000 | i << 16, source lanes at image 0-3, then
   the word. Row <name>: result = the instruction word; out0-3 = image 32-35 after (C200);
   out4/out5 = VFPU_CC after the preset / after the word; out6-9 = image 0-3 before;
   out10 = number of image words outside 32-35 that changed; out11 = the first such index
   (0xffffffff when none); out12 = the vpfxs word issued before it (0 = none). */
static void f2_cell(const char *name, f2_cell_fn fn, uint32_t word, uint32_t pfx,
                    const uint32_t *lanes)
{
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        s_f2_in[i] = 0x3f800000u | (i << 16);
    }
    for (unsigned i = 0; i < 4u; i++) {
        s_f2_in[i] = lanes[i];
    }
    uint32_t cc[2] = {0u, 0u};
    fn(s_f2_in, s_f2_out, cc);
    uint32_t changed = 0u, first = 0xffffffffu;
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        if ((i < 32u || i > 35u) && s_f2_out[i] != s_f2_in[i]) {
            if (changed++ == 0u) {
                first = i;
            }
        }
    }
    const uint32_t w[13] = {
        s_f2_out[32], s_f2_out[33], s_f2_out[34], s_f2_out[35], cc[0], cc[1], s_f2_in[0],
        s_f2_in[1], s_f2_in[2], s_f2_in[3], changed, first, pfx,
    };
    hle_record(name, "PASS", word, w, 13);
}

/* Rows f2h-p<pass>-<p|q>-<i>-<pfx>: vf2h.p (lanes 0-1) or vf2h.q (lanes 0-3) of quad i. */
static void f2_f2h(unsigned pass, unsigned wide)
{
    static const f2_cell_fn cells[2][3] = {
        {f2_f2h_p_none, f2_f2h_p_p1, f2_f2h_p_p2}, {f2_f2h_q_none, f2_f2h_q_p1, f2_f2h_q_p2}};
    static const uint32_t words[2] = {0xd0320088u, 0xd0328088u};
    char name[64];
    snprintf(name, sizeof(name), "f2h-p%u-%c", pass, wide ? 'q' : 'p');
    hle_step(name);
    for (unsigned i = 0; i < 28u; i++) {
        for (unsigned f = 0; f < 3u; f++) {
            snprintf(name, sizeof(name), "f2h-p%u-%c-%u-%s", pass, wide ? 'q' : 'p', i,
                     s_f2_pfx_names[f]);
            f2_cell(name, cells[wide][f], words[wide], s_f2_pfx_words[f], s_f2_quads[i]);
        }
    }
}

/* Rows c2i-p<pass>-<i>-<pfx>: vc2i.s of s_f2_bytes[i] (lane 0; lane 1 carries the vfpu-forms
   sentinel). */
static void f2_c2i(unsigned pass)
{
    static const f2_cell_fn fns[3] = {f2_c2i_none, f2_c2i_p1, f2_c2i_p2};
    char name[64];
    snprintf(name, sizeof(name), "c2i-p%u", pass);
    hle_step(name);
    for (unsigned i = 0; i < 10u; i++) {
        const uint32_t lanes[4] = {s_f2_bytes[i], TN_SENTINEL, 0x3f820000u, 0x3f830000u};
        for (unsigned f = 0; f < 3u; f++) {
            snprintf(name, sizeof(name), "c2i-p%u-%u-%s", pass, i, s_f2_pfx_names[f]);
            f2_cell(name, fns[f], 0xd0390008u, s_f2_pfx_words[f], lanes);
        }
    }
}

/* ---- random-unit cells -------------------------------------------------------- */

/* RCX image of four 32-bit components and a 32-bit carry, as vf_rcx_image in the
   vfpu-forms section: Ck = (RCX(k+4) & 0xffff) << 16 | (RCXk & 0xffff), carry nibble k in
   bits 16-19 of RCXk. */
static void f2_rcx_image(uint32_t *rcx, const uint32_t c[4], uint32_t carry)
{
    for (unsigned k = 0; k < 8u; k++) {
        const uint32_t half = (c[k & 3u] >> (k < 4u ? 0u : 16u)) & 0xffffu;
        rcx[k] = 0x3f800000u | (((carry >> (4u * k)) & 0xfu) << 16) | half;
    }
}

/* The thread-entry RCX image vf_rndf uses for its k = 16 row. */
static void f2_rcx_entry(uint32_t *rcx)
{
    for (unsigned r = 0; r < 8u; r++) {
        rcx[r] = r < 4u ? (0x3f800000u | (1u << r)) : 0x3f800000u;
    }
}

/* One seed cell. out: RCX0-7 read back after the write (0-7), RCX0-7 after vrnds.s (8-15),
   the vrndi.s draw (16), RCX0-7 after the draw (17-24). */
static void f2_rnds_step(const uint32_t *rcx_in, const uint32_t *seed, uint32_t *out)
{
    __asm__ volatile(
        "lv.s S000, 0(%[seed])\n\t"
        TN_RCX_WRITE("%[st]")
        "move $t0, %[out]\n\t"
        TN_RCX_READ("$t0")
        ".word 0xd0200000\n\t" /* vrnds.s S000 */
        "addiu $t0, %[out], 32\n\t"
        TN_RCX_READ("$t0")
        ".word 0xd0210000\n\t" /* vrndi.s S000 */
        "sv.s S000, 64(%[out])\n\t"
        "addiu $t0, %[out], 68\n\t"
        TN_RCX_READ("$t0")
        :
        : [st] "r"(rcx_in), [seed] "r"(seed), [out] "r"(out)
        : "t0", "t1", "memory");
}

/* One wide draw cell per instruction word. out: C000 lanes 0-3 after the draw (0-3), RCX0-7
   after it (4-11), RCX0-7 after the write (12-19). C000 holds the sentinel before. */
#define F2_DRAW_FN(fname, word)                                                      \
    static void fname(const uint32_t *rcx_in, const uint32_t *sentinel, uint32_t *out) \
    {                                                                                \
        __asm__ volatile(                                                            \
            "lv.q C000, 0(%[sen])\n\t"                                               \
            TN_RCX_WRITE("%[st]")                                                    \
            "addiu $t0, %[out], 48\n\t"                                              \
            TN_RCX_READ("$t0")                                                       \
            ".word " word "\n\t"                                                     \
            "sv.q C000, 0(%[out])\n\t"                                               \
            "addiu $t0, %[out], 16\n\t"                                              \
            TN_RCX_READ("$t0")                                                       \
            :                                                                        \
            : [st] "r"(rcx_in), [sen] "r"(sentinel), [out] "r"(out)                  \
            : "t0", "t1", "memory");                                                 \
    }

F2_DRAW_FN(f2_rndi_p, "0xd0210080")   /* vrndi.p C000.p */
F2_DRAW_FN(f2_rndi_t, "0xd0218000")   /* vrndi.t C000.t */
F2_DRAW_FN(f2_rndi_q, "0xd0218080")   /* vrndi.q C000.q */
F2_DRAW_FN(f2_rndf1_p, "0xd0220080")  /* vrndf1.p C000.p */
F2_DRAW_FN(f2_rndf1_t, "0xd0228000")  /* vrndf1.t C000.t */
F2_DRAW_FN(f2_rndf1_q, "0xd0228080")  /* vrndf1.q C000.q */
F2_DRAW_FN(f2_rndf2_p, "0xd0230080")  /* vrndf2.p C000.p */
F2_DRAW_FN(f2_rndf2_t, "0xd0238000")  /* vrndf2.t C000.t */
F2_DRAW_FN(f2_rndf2_q, "0xd0238080")  /* vrndf2.q C000.q */

typedef void (*f2_draw_fn)(const uint32_t *rcx_in, const uint32_t *sentinel, uint32_t *out);

static const char *const s_f2_draw_stems[3] = {"wrndi", "wrndf1", "wrndf2"};
static const char s_f2_draw_sizes[3] = {'p', 't', 'q'};
static const f2_draw_fn s_f2_draw_fns[3][3] = {
    {f2_rndi_p, f2_rndi_t, f2_rndi_q}, {f2_rndf1_p, f2_rndf1_t, f2_rndf1_q},
    {f2_rndf2_p, f2_rndf2_t, f2_rndf2_q}};
static const uint32_t s_f2_draw_words[3][3] = {
    {0xd0210080u, 0xd0218000u, 0xd0218080u}, {0xd0220080u, 0xd0228000u, 0xd0228080u},
    {0xd0230080u, 0xd0238000u, 0xd0238080u}};

static const uint32_t s_f2_c2[8] = {0x00000000u, 0x00000001u, 0x7fffffffu, 0x80000000u,
                                    0xfffffffeu, 0xffffffffu, 0x907f6e50u, 0x5a5a5a5au};
static const uint32_t s_f2_c3[8] = {0x00000000u, 0x00000001u, 0x7fffffffu, 0x80000000u,
                                    0xc0000000u, 0xffffffffu, 0xc4d5e6d8u, 0x89abcdefu};
static const uint32_t s_f2_carry[8] = {0x0u, 0x1u, 0x2u, 0x3u, 0xfu, 0x10u, 0xffffu,
                                       0x12345678u};

/* The RCX state k of the vfpu-forms draws (k < 8): C0 = 0x2468ace0 + k * 0x11111111,
   C1 = 0x13579bdf ^ (k << 20), C2 = s_f2_c2[k], C3 = s_f2_c3[(3k) & 7], carry = s_f2_carry[k]. */
static void f2_rcx_state(unsigned k, uint32_t *rcx)
{
    const uint32_t c[4] = {0x2468ace0u + k * 0x11111111u, 0x13579bdfu ^ (k << 20),
                           s_f2_c2[k & 7u], s_f2_c3[(3u * k) & 7u]};
    f2_rcx_image(rcx, c, s_f2_carry[k & 7u]);
}

/* Rows rnds-p<pass>-<i>-<a|b>: seed i from the entry RCX (a) or from the carry state
   (b: C0 = 0x2468ace0, C1 = 0x13579bdf, C2 = 0x907f6e50, C3 = 0xc4d5e6d8, carry
   0x12345678). result = the seed; out0-7 = RCX0-7 after the write, out8-15 = after vrnds.s,
   out16 = the vrndi.s draw, out17-24 = RCX0-7 after the draw. */
static void f2_rnds(unsigned pass)
{
    static const uint32_t c_b[4] = {0x2468ace0u, 0x13579bdfu, 0x907f6e50u, 0xc4d5e6d8u};
    uint32_t rcx[8] __attribute__((aligned(16)));
    uint32_t seedq[4] __attribute__((aligned(16))) = {0u, 0u, 0u, 0u};
    uint32_t out[25] __attribute__((aligned(16)));
    char name[48];
    snprintf(name, sizeof(name), "rnds-p%u", pass);
    hle_step(name);
    for (unsigned i = 0; i < 24u; i++) {
        for (unsigned st = 0; st < 2u; st++) {
            if (st == 0u) {
                f2_rcx_entry(rcx);
            } else {
                f2_rcx_image(rcx, c_b, 0x12345678u);
            }
            seedq[0] = s_f2_seeds[i];
            f2_rnds_step(rcx, seedq, out);
            snprintf(name, sizeof(name), "rnds-p%u-%u-%c", pass, i, st == 0u ? 'a' : 'b');
            hle_record(name, "PASS", s_f2_seeds[i], out, 25);
        }
    }
}

/* Rows <wrndi|wrndf1|wrndf2>-p<pass>-<p|t|q>-<k>: the draw from RCX state k (0-7).
   result = the instruction word; out0-3 = C000 lanes 0-3 after, out4-11 = RCX0-7 after,
   out12-19 = RCX0-7 after the write. */
static void f2_wide(unsigned pass, unsigned op, unsigned size)
{
    static const uint32_t sentinel[4] __attribute__((aligned(16))) = {
        TN_SENTINEL, TN_SENTINEL, TN_SENTINEL, TN_SENTINEL};
    uint32_t rcx[8] __attribute__((aligned(16)));
    uint32_t out[20] __attribute__((aligned(16)));
    char name[48];
    snprintf(name, sizeof(name), "%s-p%u-%c", s_f2_draw_stems[op], pass, s_f2_draw_sizes[size]);
    hle_step(name);
    for (unsigned k = 0; k < 8u; k++) {
        f2_rcx_state(k, rcx);
        s_f2_draw_fns[op][size](rcx, sentinel, out);
        snprintf(name, sizeof(name), "%s-p%u-%c-%u", s_f2_draw_stems[op], pass,
                 s_f2_draw_sizes[size], k);
        hle_record(name, "PASS", s_f2_draw_words[op][size], out, 20);
    }
}

static int hle_run(void)
{
    /* Traps off before the first VFPU cell, as in the vfpu-compare case. */
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    for (unsigned pass = 0; pass < 2u; pass++) {
        f2_rnds(pass);
        for (unsigned op = 0; op < 3u; op++) {
            for (unsigned size = 0; size < 3u; size++) {
                f2_wide(pass, op, size);
            }
        }
        f2_f2h(pass, 0u);
        f2_f2h(pass, 1u);
        f2_c2i(pass);
    }
    return 1;
}

#endif /* HLE_CASE_VFPU_FORMS2 */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_VFPU_UNMODELED

/* #326 VFPU forms the runtime refuses as unmodeled (PSP-VFPU-UNMODELED-001), one launch,
   register-only: no memory or host I/O runs between a cell's register seeding and its readback.
   POWER RISK: a refused form may raise a reserved-instruction or coprocessor exception. Nothing
   here can catch it: under PSPLink an exception stops the launch, and the console must then be
   recovered by a person, never by an unattended reset. Each form group writes a durable step
   marker first, and the forms most likely to fault run last in each pass (the mfvc/mtvc words
   with control 255 run at the very end, the mtvc ones after the mfvc one). Rows:
   - image cells run the exact word over a seeded register image and record VFPU_CC before and
     after, the count of changed image words, the prefix word, the operand set and the first 12
     changed (index, word) pairs;
   - control cells run an mfvc/mtvc word, preset VFPU_CC, and record the GPR $v0 (sentinel
     0xa5a5a5a5 before) and controls 0-15 before and after. Encodings are the census rows of
     the brief (vcrs 0x66800000/0x66810190/0x66808090, vcmov imm3 7 0xd2a70000/0xd2a701ff/
     0xd2a7808f/0xd2bfff00, mfvc 0x486200ff, mtvc 0x48e000ff) plus mtvc 0x48e900ff, the same
     control with $t1 = 0x2b so a changed control shows the value. Everything else is the VFPU
     op 0x34 (0xd0...) vsgn (op 0x0a), vbfy1 (2), vbfy2 (3) and vt4444 (0x19) layout of the
     2026-10-24 cells, vd C200 (0x08), vs C000. */

#include "hle_vfpu_cells.h"

#define UM_SENTINEL 0xa5a5a5a5u
#define UM_PAIRS 12u
#define UM_IMAGE_WORDS_OUT 29u
#define UM_CTL_WORDS_OUT 33u
#define UM_MTVC_VALUE 0x2bu

typedef void (*um_img_fn)(const uint32_t *in, uint32_t *out, uint32_t *cc, uint32_t pre);
typedef void (*um_ctl_fn)(uint32_t *before, uint32_t *after, uint32_t *gpr, uint32_t pre,
                          uint32_t val);

/* The TN_CELL_FN shape with the VFPU_CC preset as an argument. */
#define UM_CELL_FN(fname, pfx, word)                                                   \
    static void fname(const uint32_t *in, uint32_t *out, uint32_t *cc, uint32_t pre)   \
    {                                                                                  \
        __asm__ volatile(                                                              \
            "move $t0, %[pre]\n\t"                                                     \
            ".word 0x48e80083\n\t" /* mtvc $t0, VFPU_CC */                             \
            TN_LOAD_ALL                                                                \
            ".word 0x48690083\n\t" /* mfvc $t1, VFPU_CC */                             \
            "sw $t1, 0(%[cc])\n\t"                                                     \
            pfx                                                                        \
            ".word " word "\n\t"                                                       \
            TN_STORE_ALL                                                               \
            ".word 0x48690083\n\t"                                                     \
            "sw $t1, 4(%[cc])\n\t"                                                     \
            :                                                                          \
            : [in] "r"(in), [out] "r"(out), [cc] "r"(cc), [pre] "r"(pre)              \
            : "t0", "t1", "memory");                                                   \
    }

/* mfvc $t1, control 0-15 (0x48690080 + control) stored to dst[0..15]. */
#define UM_CTL_READ(dst)                                                                       \
    ".word 0x48690080\n\tsw $t1, 0(" dst ")\n\t.word 0x48690081\n\tsw $t1, 4(" dst ")\n\t"   \
    ".word 0x48690082\n\tsw $t1, 8(" dst ")\n\t.word 0x48690083\n\tsw $t1, 12(" dst ")\n\t"  \
    ".word 0x48690084\n\tsw $t1, 16(" dst ")\n\t.word 0x48690085\n\tsw $t1, 20(" dst ")\n\t" \
    ".word 0x48690086\n\tsw $t1, 24(" dst ")\n\t.word 0x48690087\n\tsw $t1, 28(" dst ")\n\t" \
    ".word 0x48690088\n\tsw $t1, 32(" dst ")\n\t.word 0x48690089\n\tsw $t1, 36(" dst ")\n\t" \
    ".word 0x4869008a\n\tsw $t1, 40(" dst ")\n\t.word 0x4869008b\n\tsw $t1, 44(" dst ")\n\t" \
    ".word 0x4869008c\n\tsw $t1, 48(" dst ")\n\t.word 0x4869008d\n\tsw $t1, 52(" dst ")\n\t" \
    ".word 0x4869008e\n\tsw $t1, 56(" dst ")\n\t.word 0x4869008f\n\tsw $t1, 60(" dst ")\n\t"

/* Presets VFPU_CC, reads controls 0-15, sets $v0 to the sentinel and $t1 to val, runs the word,
   stores $v0, reads controls 0-15 again. */
#define UM_CTL_FN(fname, word)                                                         \
    static void fname(uint32_t *before, uint32_t *after, uint32_t *gpr, uint32_t pre,  \
                      uint32_t val)                                                    \
    {                                                                                  \
        __asm__ volatile(                                                              \
            "move $t0, %[pre]\n\t"                                                     \
            ".word 0x48e80083\n\t" /* mtvc $t0, VFPU_CC */                             \
            UM_CTL_READ("%[before]")                                                   \
            "move $t1, %[val]\n\t"                                                     \
            "move $v0, %[sent]\n\t"                                                    \
            ".word " word "\n\t"                                                       \
            "sw $v0, 0(%[gpr])\n\t"                                                    \
            UM_CTL_READ("%[after]")                                                    \
            :                                                                          \
            : [before] "r"(before), [after] "r"(after), [gpr] "r"(gpr), [pre] "r"(pre), \
              [val] "r"(val), [sent] "r"(UM_SENTINEL)                                  \
            : "t0", "t1", "v0", "memory");                                             \
    }

/* Source C000 (image 0-3) and C010 (image 4-7), destinations wherever the word names. */
UM_CELL_FN(um_vsgn_q_none, TN_PFX_NONE, "0xd04a80ff")   /* vsgn.q */
UM_CELL_FN(um_vsgn_q_p1, TN_PFX_P1, "0xd04a80ff")
UM_CELL_FN(um_t4444_p_none, TN_PFX_NONE, "0xd0590088")  /* vt4444.p */
UM_CELL_FN(um_t4444_p_p1, TN_PFX_P1, "0xd0590088")
UM_CELL_FN(um_t4444_t_none, TN_PFX_NONE, "0xd0598008")  /* vt4444.t */
UM_CELL_FN(um_t4444_t_p1, TN_PFX_P1, "0xd0598008")
UM_CELL_FN(um_bfy1_s_none, TN_PFX_NONE, "0xd0420008")   /* vbfy1.s */
UM_CELL_FN(um_bfy1_s_p1, TN_PFX_P1, "0xd0420008")
UM_CELL_FN(um_bfy1_t_none, TN_PFX_NONE, "0xd0428008")   /* vbfy1.t */
UM_CELL_FN(um_bfy1_t_p1, TN_PFX_P1, "0xd0428008")
UM_CELL_FN(um_bfy2_p_none, TN_PFX_NONE, "0xd0430088")   /* vbfy2.p */
UM_CELL_FN(um_bfy2_p_p1, TN_PFX_P1, "0xd0430088")
UM_CELL_FN(um_vcrs_s, TN_PFX_NONE, "0x66800000")        /* vcrs.s */
UM_CELL_FN(um_vcrs_p, TN_PFX_NONE, "0x66810190")        /* vcrs.p */
UM_CELL_FN(um_vcrs_q, TN_PFX_NONE, "0x66808090")        /* vcrs.q */
UM_CELL_FN(um_cmov0, TN_PFX_NONE, "0xd2a70000")         /* vcmov imm3 7 */
UM_CELL_FN(um_cmov1, TN_PFX_NONE, "0xd2a701ff")
UM_CELL_FN(um_cmov2, TN_PFX_NONE, "0xd2a7808f")
UM_CELL_FN(um_cmov3, TN_PFX_NONE, "0xd2bfff00")
UM_CTL_FN(um_mfvc255, "0x486200ff")                     /* mfvc $v0, control 255 */
UM_CTL_FN(um_mtvc255_zero, "0x48e000ff")                /* mtvc $zero, control 255 */
UM_CTL_FN(um_mtvc255_val, "0x48e900ff")                 /* mtvc $t1, control 255 */

/* Two operand sets: lanes 0-3 (C000) then 4-7 (C010). 1.0, 2.0, -3.5, +0, -0, +inf, qNaN,
   smallest denormal. The second set swaps the two quads. */
static const uint32_t s_um_sets[2][8] = {
    {0x3f800000u, 0x40000000u, 0xc0600000u, 0x00000000u,
     0x80000000u, 0x7f800000u, 0x7fc00000u, 0x00000001u},
    {0x80000000u, 0x7f800000u, 0x7fc00000u, 0x00000001u,
     0x3f800000u, 0x40000000u, 0xc0600000u, 0x00000000u},
};

struct um_wide {
    const char *stem;
    uint32_t word;
    um_img_fn none;
    um_img_fn p1;
};
static const struct um_wide s_um_wide[6] = {
    {"vsgn-q", 0xd04a80ffu, um_vsgn_q_none, um_vsgn_q_p1},
    {"vt4444-p", 0xd0590088u, um_t4444_p_none, um_t4444_p_p1},
    {"vt4444-t", 0xd0598008u, um_t4444_t_none, um_t4444_t_p1},
    {"vbfy1-s", 0xd0420008u, um_bfy1_s_none, um_bfy1_s_p1},
    {"vbfy1-t", 0xd0428008u, um_bfy1_t_none, um_bfy1_t_p1},
    {"vbfy2-p", 0xd0430088u, um_bfy2_p_none, um_bfy2_p_p1},
};
static const char *const s_um_pfx_names[2] = {"none", "p1"};
static const uint32_t s_um_pfx_words[2] = {TN_PFXW_NONE, TN_PFXW_P1};

struct um_plain {
    const char *stem;
    uint32_t word;
    um_img_fn fn;
};
static const struct um_plain s_um_vcrs[3] = {
    {"vcrs-s", 0x66800000u, um_vcrs_s},
    {"vcrs-p", 0x66810190u, um_vcrs_p},
    {"vcrs-q", 0x66808090u, um_vcrs_q},
};
static const struct um_plain s_um_cmov[4] = {
    {"cmov0", 0xd2a70000u, um_cmov0},
    {"cmov1", 0xd2a701ffu, um_cmov1},
    {"cmov2", 0xd2a7808fu, um_cmov2},
    {"cmov3", 0xd2bfff00u, um_cmov3},
};
static const uint32_t s_um_cmov_cc[3] = {0x00u, 0x3fu, 0x15u};
static const char *const s_um_cmov_cc_names[3] = {"cc00", "cc3f", "cc15"};

static uint32_t s_um_in[TN_IMAGE_WORDS] __attribute__((aligned(64)));
static uint32_t s_um_out[TN_IMAGE_WORDS] __attribute__((aligned(64)));

/* One image cell: background image word i = 0x3f800000 | i << 16, operand set over image 0-7,
   then the word. Row <name>: result = the instruction word; out0/out1 = VFPU_CC after the preset
   / after the word; out2 = number of image words that changed; out3 = the vpfxs word issued
   before it (0 = none); out4 = the operand set; out5.. = up to 12 changed (image index, word)
   pairs in index order, 0xffffffff where fewer changed. */
static void um_image_cell(const char *name, um_img_fn fn, uint32_t word, uint32_t pfx,
                          uint32_t pre, unsigned set)
{
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        s_um_in[i] = 0x3f800000u | (i << 16);
    }
    for (unsigned i = 0; i < 8u; i++) {
        s_um_in[i] = s_um_sets[set][i];
    }
    uint32_t cc[2] = {0u, 0u};
    fn(s_um_in, s_um_out, cc, pre);
    uint32_t w[UM_IMAGE_WORDS_OUT];
    for (unsigned i = 0; i < UM_IMAGE_WORDS_OUT; i++) {
        w[i] = 0xffffffffu;
    }
    uint32_t changed = 0u;
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        if (s_um_out[i] != s_um_in[i]) {
            if (changed < UM_PAIRS) {
                w[5u + 2u * changed] = i;
                w[6u + 2u * changed] = s_um_out[i];
            }
            changed++;
        }
    }
    w[0] = cc[0];
    w[1] = cc[1];
    w[2] = changed;
    w[3] = pfx;
    w[4] = set;
    hle_record(name, "PASS", word, w, UM_IMAGE_WORDS_OUT);
}

/* One control cell. Row <name>: result = the instruction word; out0 = $v0 after the word;
   out1-16 = controls 0-15 before; out17-32 = controls 0-15 after. */
static void um_ctl_cell(const char *name, um_ctl_fn fn, uint32_t word, uint32_t val)
{
    uint32_t before[16], after[16], gpr = UM_SENTINEL;
    for (unsigned i = 0; i < 16u; i++) {
        before[i] = 0xffffffffu;
        after[i] = 0xffffffffu;
    }
    fn(before, after, &gpr, TN_CC_PRESET, val);
    uint32_t w[UM_CTL_WORDS_OUT];
    w[0] = gpr;
    for (unsigned i = 0; i < 16u; i++) {
        w[1u + i] = before[i];
        w[17u + i] = after[i];
    }
    hle_record(name, "PASS", word, w, UM_CTL_WORDS_OUT);
}

/* Rows <stem>-p<pass>-<set>-<none|p1>, one step <stem>-p<pass> first. */
static void um_wide_group(unsigned pass, unsigned form)
{
    char name[64];
    snprintf(name, sizeof(name), "%s-p%u", s_um_wide[form].stem, pass);
    hle_step(name);
    for (unsigned set = 0; set < 2u; set++) {
        for (unsigned f = 0; f < 2u; f++) {
            snprintf(name, sizeof(name), "%s-p%u-%u-%s", s_um_wide[form].stem, pass, set,
                     s_um_pfx_names[f]);
            um_image_cell(name, f == 0u ? s_um_wide[form].none : s_um_wide[form].p1,
                          s_um_wide[form].word, s_um_pfx_words[f], TN_CC_PRESET, set);
        }
    }
}

/* Rows <stem>-p<pass>-<set>, one step <stem>-p<pass> first. */
static void um_vcrs_group(unsigned pass, unsigned form)
{
    char name[64];
    snprintf(name, sizeof(name), "%s-p%u", s_um_vcrs[form].stem, pass);
    hle_step(name);
    for (unsigned set = 0; set < 2u; set++) {
        snprintf(name, sizeof(name), "%s-p%u-%u", s_um_vcrs[form].stem, pass, set);
        um_image_cell(name, s_um_vcrs[form].fn, s_um_vcrs[form].word, TN_PFXW_NONE, TN_CC_PRESET,
                      set);
    }
}

/* Rows cmov<k>-p<pass>-cc<hh>-<set>, one step cmov<k>-p<pass> first. */
static void um_cmov_group(unsigned pass, unsigned form)
{
    char name[64];
    snprintf(name, sizeof(name), "%s-p%u", s_um_cmov[form].stem, pass);
    hle_step(name);
    for (unsigned c = 0; c < 3u; c++) {
        for (unsigned set = 0; set < 2u; set++) {
            snprintf(name, sizeof(name), "%s-p%u-%s-%u", s_um_cmov[form].stem, pass,
                     s_um_cmov_cc_names[c], set);
            um_image_cell(name, s_um_cmov[form].fn, s_um_cmov[form].word, TN_PFXW_NONE,
                          s_um_cmov_cc[c], set);
        }
    }
}

static int hle_run(void)
{
    /* Traps off before the first VFPU cell, as in the vfpu-compare case. */
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    for (unsigned pass = 0; pass < 2u; pass++) {
        for (unsigned form = 0; form < 6u; form++) {
            um_wide_group(pass, form);
        }
        for (unsigned form = 0; form < 3u; form++) {
            um_vcrs_group(pass, form);
        }
        for (unsigned form = 0; form < 4u; form++) {
            um_cmov_group(pass, form);
        }
        char name[64];
        snprintf(name, sizeof(name), "mfvc255-p%u", pass);
        hle_step(name);
        um_ctl_cell(name, um_mfvc255, 0x486200ffu, 0u);
        snprintf(name, sizeof(name), "mtvc255-zero-p%u", pass);
        hle_step(name);
        um_ctl_cell(name, um_mtvc255_zero, 0x48e000ffu, 0u);
        snprintf(name, sizeof(name), "mtvc255-val-p%u", pass);
        hle_step(name);
        um_ctl_cell(name, um_mtvc255_val, 0x48e900ffu, UM_MTVC_VALUE);
    }
    return 1;
}

#endif /* HLE_CASE_VFPU_UNMODELED */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_VFPU_QUAD_WB

/* #326 lv.q / sv.q with the write-back flag (PSP-VFPU-QUADWB-001), one launch, two passes. The
   word is op 0x36 (lv.q, 0xd8000000) or 0x3e (sv.q, 0xf8000000), rs in bits 21-25, vt in bits
   16-20 (bit 5 of the register in word bit 0), a 14-bit offset in bits 2-15 and the write-back
   flag in word bit 1. rs is always $t2 (10) and the seed/readback moves use $t3 (11).
   Everything stays inside s_qw_buf (1024 bytes, 64-byte aligned); the base is its middle
   (byte 512, non-zero, 16-byte aligned), so the +/-48 byte forms and a drifting base stay
   inside it. Rows:
   - lvq-wb-<off>-p<pass> and svq-wb-<off>-p<pass>, off o0, o16, o32, om16: one wb form, the
     result is the word; out0 = base before, out1 = base after, out2 = buffer start address,
     out3-6 = C000 lanes after the form (lv.q: the lanes loaded, read back with a plain sv.q;
     sv.q: the lanes that were stored), out7 = number of changed buffer words, out8/out9 =
     first/last changed word index relative to the base (0xffffffff if none), out10-13 = the
     four buffer words at base + offset after the form;
   - svq-wb-run-p<pass>: four back-to-back wb sv.q at +0/+16/+32/+48 of one base from C000,
     C010, C020 and C030; out0 = base before, out1-4 = $t2 after each, out5 = buffer start,
     out6 = changed buffer words, out7.. = up to 16 (relative word index, word) pairs.
   POWER RISK: the effect of the write-back flag on the console is unknown; an exception
   (reserved instruction, address error) cannot be caught here and stops the launch, and the
   console must then be recovered by a person, never by an unattended reset. Order: the loads
   first (no memory writes), then the single stores, the run of four last; each form is its own
   step, written durably first. */

#define QW_BUF_WORDS 256u
#define QW_MID_WORD 128u
#define QW_PAIRS 16u
#define QW_FORM_WORDS_OUT 14u
#define QW_RUN_WORDS_OUT 39u
#define QW_MISSING 0xffffffffu

static uint32_t s_qw_buf[QW_BUF_WORDS] __attribute__((aligned(64)));
static uint32_t s_qw_res[4] __attribute__((aligned(16)));
static uint32_t s_qw_after[4];
/* Four quads, C000..C030 seeds: 0xc0de0000 | quad << 8 | lane. */
static const uint32_t s_qw_pat[16] __attribute__((aligned(16))) = {
    0xc0de0000u, 0xc0de0001u, 0xc0de0002u, 0xc0de0003u,
    0xc0de0100u, 0xc0de0101u, 0xc0de0102u, 0xc0de0103u,
    0xc0de0200u, 0xc0de0201u, 0xc0de0202u, 0xc0de0203u,
    0xc0de0300u, 0xc0de0301u, 0xc0de0302u, 0xc0de0303u,
};

typedef uint32_t (*qw_fn)(uint32_t base, const uint32_t *pat, uint32_t *res);

/* Seed C000 from pat, run the wb word on $t2 = base, read $t2 back, read C000 back to res. */
#define QW_FORM_FN(fname, word)                                                        \
    static uint32_t fname(uint32_t base, const uint32_t *pat, uint32_t *res)           \
    {                                                                                  \
        uint32_t after;                                                                \
        __asm__ volatile(                                                              \
            "move $t2, %[base]\n\t"                                                    \
            "move $t3, %[pat]\n\t"                                                     \
            ".word 0xd9600000\n\t" /* lv.q C000, 0($t3) */                             \
            ".word " word "\n\t"                                                       \
            "move %[after], $t2\n\t"                                                   \
            "move $t3, %[res]\n\t"                                                     \
            ".word 0xf9600000\n\t" /* sv.q C000, 0($t3) */                             \
            : [after] "=&r"(after)                                                     \
            : [base] "r"(base), [pat] "r"(pat), [res] "r"(res)                         \
            : "t2", "t3", "memory");                                                   \
        return after;                                                                  \
    }

/* lv.q C000, off($t2), wb: 0xd8000000 | 10 << 21 | off & 0xfffc | 2. */
QW_FORM_FN(qw_lv_o0, "0xd9400002")
QW_FORM_FN(qw_lv_o16, "0xd9400012")
QW_FORM_FN(qw_lv_o32, "0xd9400022")
QW_FORM_FN(qw_lv_om16, "0xd940fff2")
/* sv.q C000, off($t2), wb: 0xf8000000 | 10 << 21 | off & 0xfffc | 2. */
QW_FORM_FN(qw_sv_o0, "0xf9400002")
QW_FORM_FN(qw_sv_o16, "0xf9400012")
QW_FORM_FN(qw_sv_o32, "0xf9400022")
QW_FORM_FN(qw_sv_om16, "0xf940fff2")

struct qw_form {
    const char *name;
    uint32_t word;
    int32_t off;
    qw_fn fn;
};
static const struct qw_form s_qw_lv[4] = {
    {"lvq-wb-o0", 0xd9400002u, 0, qw_lv_o0},
    {"lvq-wb-o16", 0xd9400012u, 16, qw_lv_o16},
    {"lvq-wb-o32", 0xd9400022u, 32, qw_lv_o32},
    {"lvq-wb-om16", 0xd940fff2u, -16, qw_lv_om16},
};
static const struct qw_form s_qw_sv[4] = {
    {"svq-wb-o0", 0xf9400002u, 0, qw_sv_o0},
    {"svq-wb-o16", 0xf9400012u, 16, qw_sv_o16},
    {"svq-wb-o32", 0xf9400022u, 32, qw_sv_o32},
    {"svq-wb-om16", 0xf940fff2u, -16, qw_sv_om16},
};

/* Four wb sv.q from one base, seeded C000, C010, C020, C030, $t2 stored after each. */
static void qw_run4(uint32_t base, const uint32_t *pat, uint32_t *after)
{
    __asm__ volatile(
        "move $t2, %[base]\n\t"
        "move $t3, %[pat]\n\t"
        ".word 0xd9600000\n\t" /* lv.q C000, 0($t3) */
        ".word 0xd9640010\n\t" /* lv.q C010, 16($t3) */
        ".word 0xd9680020\n\t" /* lv.q C020, 32($t3) */
        ".word 0xd96c0030\n\t" /* lv.q C030, 48($t3) */
        ".word 0xf9400002\n\t" /* sv.q C000, 0($t2), wb */
        "sw $t2, 0(%[after])\n\t"
        ".word 0xf9440012\n\t" /* sv.q C010, 16($t2), wb */
        "sw $t2, 4(%[after])\n\t"
        ".word 0xf9480022\n\t" /* sv.q C020, 32($t2), wb */
        "sw $t2, 8(%[after])\n\t"
        ".word 0xf94c0032\n\t" /* sv.q C030, 48($t2), wb */
        "sw $t2, 12(%[after])\n\t"
        :
        : [base] "r"(base), [pat] "r"(pat), [after] "r"(after)
        : "t2", "t3", "memory");
}

static void qw_fill(void)
{
    for (unsigned i = 0; i < QW_BUF_WORDS; i++) {
        s_qw_buf[i] = 0x51000000u | i;
    }
}

/* Changed-word census of the buffer against the fill: count, first and last relative index. */
static uint32_t qw_census(uint32_t *first, uint32_t *last)
{
    uint32_t changed = 0u;
    *first = QW_MISSING;
    *last = QW_MISSING;
    for (unsigned i = 0; i < QW_BUF_WORDS; i++) {
        if (s_qw_buf[i] != (0x51000000u | i)) {
            if (changed == 0u) {
                *first = i - QW_MID_WORD;
            }
            *last = i - QW_MID_WORD;
            changed++;
        }
    }
    return changed;
}

static void qw_form_cell(const char *name, const struct qw_form *form)
{
    qw_fill();
    for (unsigned i = 0; i < 4u; i++) {
        s_qw_res[i] = QW_MISSING;
    }
    const uint32_t base = (uint32_t)(uintptr_t)&s_qw_buf[QW_MID_WORD];
    const uint32_t after = form->fn(base, s_qw_pat, s_qw_res);
    uint32_t w[QW_FORM_WORDS_OUT];
    w[0] = base;
    w[1] = after;
    w[2] = (uint32_t)(uintptr_t)s_qw_buf;
    for (unsigned i = 0; i < 4u; i++) {
        w[3u + i] = s_qw_res[i];
    }
    w[7] = qw_census(&w[8], &w[9]);
    const unsigned at = QW_MID_WORD + (unsigned)(form->off / 4);
    for (unsigned i = 0; i < 4u; i++) {
        w[10u + i] = s_qw_buf[at + i];
    }
    hle_record(name, "PASS", form->word, w, QW_FORM_WORDS_OUT);
}

static void qw_run_cell(const char *name)
{
    qw_fill();
    for (unsigned i = 0; i < 4u; i++) {
        s_qw_after[i] = QW_MISSING;
    }
    const uint32_t base = (uint32_t)(uintptr_t)&s_qw_buf[QW_MID_WORD];
    qw_run4(base, s_qw_pat, s_qw_after);
    uint32_t w[QW_RUN_WORDS_OUT];
    for (unsigned i = 0; i < QW_RUN_WORDS_OUT; i++) {
        w[i] = QW_MISSING;
    }
    w[0] = base;
    for (unsigned i = 0; i < 4u; i++) {
        w[1u + i] = s_qw_after[i];
    }
    w[5] = (uint32_t)(uintptr_t)s_qw_buf;
    uint32_t changed = 0u;
    for (unsigned i = 0; i < QW_BUF_WORDS; i++) {
        if (s_qw_buf[i] != (0x51000000u | i)) {
            if (changed < QW_PAIRS) {
                w[7u + 2u * changed] = i - QW_MID_WORD;
                w[8u + 2u * changed] = s_qw_buf[i];
            }
            changed++;
        }
    }
    w[6] = changed;
    hle_record(name, "PASS", 0xf9400002u, w, QW_RUN_WORDS_OUT);
}

/* Rows <lvq|svq>-wb-<off>-p<pass>, one step of the same name first. */
static void qw_group(unsigned pass, const struct qw_form *forms, unsigned form)
{
    char name[64];
    snprintf(name, sizeof(name), "%s-p%u", forms[form].name, pass);
    hle_step(name);
    qw_form_cell(name, &forms[form]);
}

static int hle_run(void)
{
    /* Traps off before the first VFPU cell, as in the vfpu-compare case. */
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    for (unsigned pass = 0; pass < 2u; pass++) {
        for (unsigned form = 0; form < 4u; form++) {
            qw_group(pass, s_qw_lv, form);
        }
        for (unsigned form = 0; form < 4u; form++) {
            qw_group(pass, s_qw_sv, form);
        }
        char name[64];
        snprintf(name, sizeof(name), "svq-wb-run-p%u", pass);
        hle_step(name);
        qw_run_cell(name);
    }
    return 1;
}

#endif /* HLE_CASE_VFPU_QUAD_WB */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_VFPU_CONVERT

/* #326 VFPU float/integer conversion forms the second title executes (PSP-VFPU-CONVERT-001),
   two passes, register-only: no memory or host I/O runs between a cell's register seeding and
   its readback, every instruction is a legal Allegrex word, and no prefix is issued (identity
   prefixes). POWER RISK is low but unmeasured, so run it with the owner present. A step marker
   is written durably before each form group.
   One cell loads sixteen words (eight operands in C000 and C010, a sentinel background in C200
   and C210), runs the form's words so that all eight operands are converted, and stores C200
   and C210 back. A .q form converts four operands per word (two words), .t the first three of
   each quad, .p two per word (four words), .s one per word (eight words). vi2s and vi2us .q
   write a pair, vi2c and vi2uc .q one word, at the first lane of C200 and C210. Row
   <stem>-p<pass>-<image>: result = the form's first instruction word, out0-3 = C200 and
   out4-7 = C210 after the words.
   Operand images (three per kind, eight words each) hold the float classes (+-0, +-0.5, +-1.5,
   +-2.5, +-1.0, the neighbours of +-2^31, +-inf, quiet NaNs of both signs, a signalling
   pattern, +-smallest denormal, FLT_MAX) or the integer edge words (0, 1, 2, 0x7fffffff,
   0x80000000, 0xffffffff, 16-bit and byte boundary patterns). The words are the Allegrex VFPU
   encodings (vf2in 0xd200, vf2iz 0xd220, vf2iu 0xd240, vf2id 0xd260, vi2f 0xd280 in the top
   half, 0xd03c-0xd03f for vi2uc/vi2c/vi2us/vi2s), scale in bits 20:16, size in bits 15 and 7,
   source in bits 14:8 and destination in bits 6:0. */

#define CV_IMAGES 3u
#define CV_IN_WORDS 16u
#define CV_OUT_WORDS 8u
#define CV_BACKGROUND 0x7a5a5a00u

typedef void (*cv_fn)(const uint32_t *in, uint32_t *out);

#define CV_CELL_FN(fname, body)                                                        \
    static void fname(const uint32_t *in, uint32_t *out)                               \
    {                                                                                  \
        __asm__ volatile(                                                              \
            "lv.q C000, 0(%[in])\n\t"                                                  \
            "lv.q C010, 16(%[in])\n\t"                                                 \
            "lv.q C200, 32(%[in])\n\t"                                                 \
            "lv.q C210, 48(%[in])\n\t"                                                 \
            body                                                                       \
            "sv.q C200, 0(%[out])\n\t"                                                 \
            "sv.q C210, 16(%[out])\n\t"                                                \
            :                                                                          \
            : [in] "r"(in), [out] "r"(out)                                             \
            : "memory");                                                               \
    }

CV_CELL_FN(cv_vf2iz_s_sc0,
    ".word 0xd2200008\n\t"
    ".word 0xd2202028\n\t"
    ".word 0xd2204048\n\t"
    ".word 0xd2206068\n\t"
    ".word 0xd2200109\n\t"
    ".word 0xd2202129\n\t"
    ".word 0xd2204149\n\t"
    ".word 0xd2206169\n\t")
CV_CELL_FN(cv_vf2iz_s_sc1,
    ".word 0xd2210008\n\t"
    ".word 0xd2212028\n\t"
    ".word 0xd2214048\n\t"
    ".word 0xd2216068\n\t"
    ".word 0xd2210109\n\t"
    ".word 0xd2212129\n\t"
    ".word 0xd2214149\n\t"
    ".word 0xd2216169\n\t")
CV_CELL_FN(cv_vf2iz_s_sc8,
    ".word 0xd2280008\n\t"
    ".word 0xd2282028\n\t"
    ".word 0xd2284048\n\t"
    ".word 0xd2286068\n\t"
    ".word 0xd2280109\n\t"
    ".word 0xd2282129\n\t"
    ".word 0xd2284149\n\t"
    ".word 0xd2286169\n\t")
CV_CELL_FN(cv_vf2iz_s_sc15,
    ".word 0xd22f0008\n\t"
    ".word 0xd22f2028\n\t"
    ".word 0xd22f4048\n\t"
    ".word 0xd22f6068\n\t"
    ".word 0xd22f0109\n\t"
    ".word 0xd22f2129\n\t"
    ".word 0xd22f4149\n\t"
    ".word 0xd22f6169\n\t")
CV_CELL_FN(cv_vf2iz_s_sc16,
    ".word 0xd2300008\n\t"
    ".word 0xd2302028\n\t"
    ".word 0xd2304048\n\t"
    ".word 0xd2306068\n\t"
    ".word 0xd2300109\n\t"
    ".word 0xd2302129\n\t"
    ".word 0xd2304149\n\t"
    ".word 0xd2306169\n\t")
CV_CELL_FN(cv_vf2iz_s_sc23,
    ".word 0xd2370008\n\t"
    ".word 0xd2372028\n\t"
    ".word 0xd2374048\n\t"
    ".word 0xd2376068\n\t"
    ".word 0xd2370109\n\t"
    ".word 0xd2372129\n\t"
    ".word 0xd2374149\n\t"
    ".word 0xd2376169\n\t")
CV_CELL_FN(cv_vf2iz_s_sc30,
    ".word 0xd23e0008\n\t"
    ".word 0xd23e2028\n\t"
    ".word 0xd23e4048\n\t"
    ".word 0xd23e6068\n\t"
    ".word 0xd23e0109\n\t"
    ".word 0xd23e2129\n\t"
    ".word 0xd23e4149\n\t"
    ".word 0xd23e6169\n\t")
CV_CELL_FN(cv_vf2iz_s_sc31,
    ".word 0xd23f0008\n\t"
    ".word 0xd23f2028\n\t"
    ".word 0xd23f4048\n\t"
    ".word 0xd23f6068\n\t"
    ".word 0xd23f0109\n\t"
    ".word 0xd23f2129\n\t"
    ".word 0xd23f4149\n\t"
    ".word 0xd23f6169\n\t")
CV_CELL_FN(cv_vf2iz_q_sc0,
    ".word 0xd2208088\n\t"
    ".word 0xd2208189\n\t")
CV_CELL_FN(cv_vf2iz_q_sc1,
    ".word 0xd2218088\n\t"
    ".word 0xd2218189\n\t")
CV_CELL_FN(cv_vf2iz_q_sc8,
    ".word 0xd2288088\n\t"
    ".word 0xd2288189\n\t")
CV_CELL_FN(cv_vf2iz_q_sc15,
    ".word 0xd22f8088\n\t"
    ".word 0xd22f8189\n\t")
CV_CELL_FN(cv_vf2iz_q_sc16,
    ".word 0xd2308088\n\t"
    ".word 0xd2308189\n\t")
CV_CELL_FN(cv_vf2iz_q_sc23,
    ".word 0xd2378088\n\t"
    ".word 0xd2378189\n\t")
CV_CELL_FN(cv_vf2iz_q_sc30,
    ".word 0xd23e8088\n\t"
    ".word 0xd23e8189\n\t")
CV_CELL_FN(cv_vf2iz_q_sc31,
    ".word 0xd23f8088\n\t"
    ".word 0xd23f8189\n\t")
CV_CELL_FN(cv_vf2in_s_sc0,
    ".word 0xd2000008\n\t"
    ".word 0xd2002028\n\t"
    ".word 0xd2004048\n\t"
    ".word 0xd2006068\n\t"
    ".word 0xd2000109\n\t"
    ".word 0xd2002129\n\t"
    ".word 0xd2004149\n\t"
    ".word 0xd2006169\n\t")
CV_CELL_FN(cv_vf2in_s_sc8,
    ".word 0xd2080008\n\t"
    ".word 0xd2082028\n\t"
    ".word 0xd2084048\n\t"
    ".word 0xd2086068\n\t"
    ".word 0xd2080109\n\t"
    ".word 0xd2082129\n\t"
    ".word 0xd2084149\n\t"
    ".word 0xd2086169\n\t")
CV_CELL_FN(cv_vf2iu_s_sc0,
    ".word 0xd2400008\n\t"
    ".word 0xd2402028\n\t"
    ".word 0xd2404048\n\t"
    ".word 0xd2406068\n\t"
    ".word 0xd2400109\n\t"
    ".word 0xd2402129\n\t"
    ".word 0xd2404149\n\t"
    ".word 0xd2406169\n\t")
CV_CELL_FN(cv_vf2iu_s_sc8,
    ".word 0xd2480008\n\t"
    ".word 0xd2482028\n\t"
    ".word 0xd2484048\n\t"
    ".word 0xd2486068\n\t"
    ".word 0xd2480109\n\t"
    ".word 0xd2482129\n\t"
    ".word 0xd2484149\n\t"
    ".word 0xd2486169\n\t")
CV_CELL_FN(cv_vf2id_s_sc0,
    ".word 0xd2600008\n\t"
    ".word 0xd2602028\n\t"
    ".word 0xd2604048\n\t"
    ".word 0xd2606068\n\t"
    ".word 0xd2600109\n\t"
    ".word 0xd2602129\n\t"
    ".word 0xd2604149\n\t"
    ".word 0xd2606169\n\t")
CV_CELL_FN(cv_vf2id_s_sc8,
    ".word 0xd2680008\n\t"
    ".word 0xd2682028\n\t"
    ".word 0xd2684048\n\t"
    ".word 0xd2686068\n\t"
    ".word 0xd2680109\n\t"
    ".word 0xd2682129\n\t"
    ".word 0xd2684149\n\t"
    ".word 0xd2686169\n\t")
CV_CELL_FN(cv_vi2f_s_sc0,
    ".word 0xd2800008\n\t"
    ".word 0xd2802028\n\t"
    ".word 0xd2804048\n\t"
    ".word 0xd2806068\n\t"
    ".word 0xd2800109\n\t"
    ".word 0xd2802129\n\t"
    ".word 0xd2804149\n\t"
    ".word 0xd2806169\n\t")
CV_CELL_FN(cv_vi2f_s_sc8,
    ".word 0xd2880008\n\t"
    ".word 0xd2882028\n\t"
    ".word 0xd2884048\n\t"
    ".word 0xd2886068\n\t"
    ".word 0xd2880109\n\t"
    ".word 0xd2882129\n\t"
    ".word 0xd2884149\n\t"
    ".word 0xd2886169\n\t")
CV_CELL_FN(cv_vi2f_s_sc31,
    ".word 0xd29f0008\n\t"
    ".word 0xd29f2028\n\t"
    ".word 0xd29f4048\n\t"
    ".word 0xd29f6068\n\t"
    ".word 0xd29f0109\n\t"
    ".word 0xd29f2129\n\t"
    ".word 0xd29f4149\n\t"
    ".word 0xd29f6169\n\t")
CV_CELL_FN(cv_vi2f_p_sc0,
    ".word 0xd2800088\n\t"
    ".word 0xd28040c8\n\t"
    ".word 0xd2800189\n\t"
    ".word 0xd28041c9\n\t")
CV_CELL_FN(cv_vi2f_p_sc8,
    ".word 0xd2880088\n\t"
    ".word 0xd28840c8\n\t"
    ".word 0xd2880189\n\t"
    ".word 0xd28841c9\n\t")
CV_CELL_FN(cv_vi2f_p_sc31,
    ".word 0xd29f0088\n\t"
    ".word 0xd29f40c8\n\t"
    ".word 0xd29f0189\n\t"
    ".word 0xd29f41c9\n\t")
CV_CELL_FN(cv_vi2f_t_sc0,
    ".word 0xd2808008\n\t"
    ".word 0xd2808109\n\t")
CV_CELL_FN(cv_vi2f_t_sc8,
    ".word 0xd2888008\n\t"
    ".word 0xd2888109\n\t")
CV_CELL_FN(cv_vi2f_t_sc31,
    ".word 0xd29f8008\n\t"
    ".word 0xd29f8109\n\t")
CV_CELL_FN(cv_vi2f_q_sc0,
    ".word 0xd2808088\n\t"
    ".word 0xd2808189\n\t")
CV_CELL_FN(cv_vi2f_q_sc8,
    ".word 0xd2888088\n\t"
    ".word 0xd2888189\n\t")
CV_CELL_FN(cv_vi2f_q_sc31,
    ".word 0xd29f8088\n\t"
    ".word 0xd29f8189\n\t")
CV_CELL_FN(cv_vi2s_q,
    ".word 0xd03f8088\n\t"
    ".word 0xd03f8189\n\t")
CV_CELL_FN(cv_vi2us_q,
    ".word 0xd03e8088\n\t"
    ".word 0xd03e8189\n\t")
CV_CELL_FN(cv_vi2c_q,
    ".word 0xd03d8088\n\t"
    ".word 0xd03d8189\n\t")
CV_CELL_FN(cv_vi2uc_q,
    ".word 0xd03c8088\n\t"
    ".word 0xd03c8189\n\t")

/* Float operand images: zeros and halves, ties and the 2^31 neighbours, infinities, NaNs,
   denormals and FLT_MAX. */
static const uint32_t s_cv_float[CV_IMAGES][8] = {
    {0x00000000u, 0x80000000u, 0x3f000000u, 0x3fc00000u,
     0x40200000u, 0xbf000000u, 0xbfc00000u, 0xc0200000u},
    {0x3f800000u, 0x4effffffu, 0x4f000000u, 0x4f000001u,
     0xcf000000u, 0xcf000001u, 0xceffffffu, 0xbf800000u},
    {0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u,
     0x7f800001u, 0x00000001u, 0x80000001u, 0x7f7fffffu},
};

/* Integer edge-word images: the 32-bit edges, the 16-bit boundaries, byte patterns. */
static const uint32_t s_cv_int[CV_IMAGES][8] = {
    {0x00000000u, 0x00000001u, 0x00000002u, 0x7fffffffu,
     0x80000000u, 0xffffffffu, 0xfffffffeu, 0x00007fffu},
    {0x00008000u, 0x0000ffffu, 0x00010000u, 0xffff8000u,
     0xffff0000u, 0x12345678u, 0x80008000u, 0x7fff7fffu},
    {0x01020304u, 0x7f7f7f7fu, 0x80808080u, 0xff00ff00u,
     0x00ff00ffu, 0x80000001u, 0x0f1e2d3cu, 0xfedcba98u},
};

struct cv_form {
    const char *stem;
    uint32_t word;
    uint32_t kind; /* 0 = float images, 1 = integer images */
    cv_fn fn;
};
static const struct cv_form s_cv_forms[38] = {
    {"vf2iz-s-sc0", 0xd2200008u, 0u, cv_vf2iz_s_sc0},
    {"vf2iz-s-sc1", 0xd2210008u, 0u, cv_vf2iz_s_sc1},
    {"vf2iz-s-sc8", 0xd2280008u, 0u, cv_vf2iz_s_sc8},
    {"vf2iz-s-sc15", 0xd22f0008u, 0u, cv_vf2iz_s_sc15},
    {"vf2iz-s-sc16", 0xd2300008u, 0u, cv_vf2iz_s_sc16},
    {"vf2iz-s-sc23", 0xd2370008u, 0u, cv_vf2iz_s_sc23},
    {"vf2iz-s-sc30", 0xd23e0008u, 0u, cv_vf2iz_s_sc30},
    {"vf2iz-s-sc31", 0xd23f0008u, 0u, cv_vf2iz_s_sc31},
    {"vf2iz-q-sc0", 0xd2208088u, 0u, cv_vf2iz_q_sc0},
    {"vf2iz-q-sc1", 0xd2218088u, 0u, cv_vf2iz_q_sc1},
    {"vf2iz-q-sc8", 0xd2288088u, 0u, cv_vf2iz_q_sc8},
    {"vf2iz-q-sc15", 0xd22f8088u, 0u, cv_vf2iz_q_sc15},
    {"vf2iz-q-sc16", 0xd2308088u, 0u, cv_vf2iz_q_sc16},
    {"vf2iz-q-sc23", 0xd2378088u, 0u, cv_vf2iz_q_sc23},
    {"vf2iz-q-sc30", 0xd23e8088u, 0u, cv_vf2iz_q_sc30},
    {"vf2iz-q-sc31", 0xd23f8088u, 0u, cv_vf2iz_q_sc31},
    {"vf2in-s-sc0", 0xd2000008u, 0u, cv_vf2in_s_sc0},
    {"vf2in-s-sc8", 0xd2080008u, 0u, cv_vf2in_s_sc8},
    {"vf2iu-s-sc0", 0xd2400008u, 0u, cv_vf2iu_s_sc0},
    {"vf2iu-s-sc8", 0xd2480008u, 0u, cv_vf2iu_s_sc8},
    {"vf2id-s-sc0", 0xd2600008u, 0u, cv_vf2id_s_sc0},
    {"vf2id-s-sc8", 0xd2680008u, 0u, cv_vf2id_s_sc8},
    {"vi2f-s-sc0", 0xd2800008u, 1u, cv_vi2f_s_sc0},
    {"vi2f-s-sc8", 0xd2880008u, 1u, cv_vi2f_s_sc8},
    {"vi2f-s-sc31", 0xd29f0008u, 1u, cv_vi2f_s_sc31},
    {"vi2f-p-sc0", 0xd2800088u, 1u, cv_vi2f_p_sc0},
    {"vi2f-p-sc8", 0xd2880088u, 1u, cv_vi2f_p_sc8},
    {"vi2f-p-sc31", 0xd29f0088u, 1u, cv_vi2f_p_sc31},
    {"vi2f-t-sc0", 0xd2808008u, 1u, cv_vi2f_t_sc0},
    {"vi2f-t-sc8", 0xd2888008u, 1u, cv_vi2f_t_sc8},
    {"vi2f-t-sc31", 0xd29f8008u, 1u, cv_vi2f_t_sc31},
    {"vi2f-q-sc0", 0xd2808088u, 1u, cv_vi2f_q_sc0},
    {"vi2f-q-sc8", 0xd2888088u, 1u, cv_vi2f_q_sc8},
    {"vi2f-q-sc31", 0xd29f8088u, 1u, cv_vi2f_q_sc31},
    {"vi2s-q", 0xd03f8088u, 1u, cv_vi2s_q},
    {"vi2us-q", 0xd03e8088u, 1u, cv_vi2us_q},
    {"vi2c-q", 0xd03d8088u, 1u, cv_vi2c_q},
    {"vi2uc-q", 0xd03c8088u, 1u, cv_vi2uc_q},
};

static uint32_t s_cv_in[CV_IN_WORDS] __attribute__((aligned(64)));
static uint32_t s_cv_out[CV_OUT_WORDS] __attribute__((aligned(64)));

static void cv_cell(const char *name, const struct cv_form *form, unsigned image)
{
    const uint32_t *ops = form->kind == 0u ? s_cv_float[image] : s_cv_int[image];
    for (unsigned i = 0; i < 8u; i++) {
        s_cv_in[i] = ops[i];
        s_cv_in[8u + i] = CV_BACKGROUND | i;
    }
    for (unsigned i = 0; i < CV_OUT_WORDS; i++) {
        s_cv_out[i] = 0xffffffffu;
    }
    form->fn(s_cv_in, s_cv_out);
    uint32_t w[CV_OUT_WORDS];
    for (unsigned i = 0; i < CV_OUT_WORDS; i++) {
        w[i] = s_cv_out[i];
    }
    hle_record(name, "PASS", form->word, w, CV_OUT_WORDS);
}

/* Rows <stem>-p<pass>-<image>, one step <stem>-p<pass> first. */
static void cv_group(unsigned pass, unsigned form)
{
    char name[64];
    snprintf(name, sizeof(name), "%s-p%u", s_cv_forms[form].stem, pass);
    hle_step(name);
    for (unsigned image = 0; image < CV_IMAGES; image++) {
        snprintf(name, sizeof(name), "%s-p%u-%u", s_cv_forms[form].stem, pass, image);
        cv_cell(name, &s_cv_forms[form], image);
    }
}

static int hle_run(void)
{
    /* Traps off before the first VFPU cell, as in the vfpu-compare case. */
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    for (unsigned pass = 0; pass < 2u; pass++) {
        for (unsigned form = 0; form < 38u; form++) {
            cv_group(pass, form);
        }
    }
    return 1;
}

#endif /* HLE_CASE_VFPU_CONVERT */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_VFPU_VUS2I

/* #326 vus2i/vs2i (double-width unpack), vsgn and vflush (PSP-VFPU-VUS2I-001), two passes,
   register-only: no memory or host I/O runs between a cell's register seeding and its
   readback, and every instruction is a legal Allegrex word. No prefix word is issued before a
   vus2i, vs2i or vsgn cell (identity prefixes); only the vflush cells issue one. POWER RISK
   is low but unmeasured, so run it with the owner present. A step marker is written durably
   before each form group.
   One cell loads twelve words (C000 = in0-3, C010 = in4-7, C200 = in8-11, a sentinel
   background), runs the form's words, then stores C200 to out0-3 and C000 to out4-7. Row
   <stem>-p<pass>-<index>: result = the form's instruction word, out0-3 = C200 after the words
   and out4-7 = C000 after the words (the source operand lanes, which also records the inputs).
   vus2i.s/vs2i.s: source S000 (lane 0 of C000), dest the pair C200 lanes 0-1 (the output is
   double width: one source lane makes two words, the low half of the lane going to the even
   output lane). vus2i.p/vs2i.p: source the pair C000 lanes 0-1, dest the quad C200. Records
   <form>-p<pass>-<k>, k = 0..11, the source lanes of cell k being the twelve source words
   rotated by k: C000 = w[k], w[k+1], w[k+2], w[k+3] (indices mod 12), so every source word
   appears as lane 0 (.s) and as a pair (word, next word) (.p).
   vsgn.p/.t/.q: C000 into C200, one record per image (<stem>-p<pass>-<image>, three images of
   four float lanes: 1.0, -1.0, +0, -0 / 2.5, -3.5, qNaN, +inf / smallest denormal, its
   negative, a negative qNaN and a signalling NaN); the lanes past the width keep the
   sentinel in out0-3.
   vflush cells (names vflush-*, droppable as one group): the prefix word 0xdc0100e4 (negate
   lane 0, identity swizzle), then vflush 0xffff040d, then vmov.s S200,S000 and vmov.s S201,S000
   (vflush-vmov), or vadd.s S200,S000,S010 and vadd.s S201,S000,S010 (vflush-vadd); the
   control vflush-ctrl runs the prefix and the two vmov.s without the vflush. The prefix
   applied if C200 lane 0 is the negation of C000 lane 0 (vmov) and C200 lane 1 is its plain
   value (the prefix is consumed by the first instruction). C000 = 3, 5, 7, 9 and
   C010 = 11, 13, 15, 17 as floats. result = the vflush word 0xffff040d for all three.
   Words: vus2i 0xd03a, vs2i 0xd03b, vsgn 0xd04a in the top half, size in bits 15 and 7,
   source in bits 14:8 and destination in bits 6:0. */

#define VU_IN_WORDS 12u
#define VU_OUT_WORDS 8u
#define VU_BACKGROUND 0x7a5a5a00u
#define VU_SRC_WORDS 12u
#define VU_IMAGES 3u
#define VU_VFLUSH_WORD 0xffff040du

typedef void (*vu_fn)(const uint32_t *in, uint32_t *out);

#define VU_CELL_FN(fname, body)                                                        \
    static void fname(const uint32_t *in, uint32_t *out)                               \
    {                                                                                  \
        __asm__ volatile(                                                              \
            "lv.q C000, 0(%[in])\n\t"                                                  \
            "lv.q C010, 16(%[in])\n\t"                                                 \
            "lv.q C200, 32(%[in])\n\t"                                                 \
            body                                                                       \
            "sv.q C200, 0(%[out])\n\t"                                                 \
            "sv.q C000, 16(%[out])\n\t"                                                \
            :                                                                          \
            : [in] "r"(in), [out] "r"(out)                                             \
            : "memory");                                                               \
    }

VU_CELL_FN(vu_vus2i_s,
    ".word 0xd03a0008\n\t")
VU_CELL_FN(vu_vus2i_p,
    ".word 0xd03a0088\n\t")
VU_CELL_FN(vu_vs2i_s,
    ".word 0xd03b0008\n\t")
VU_CELL_FN(vu_vs2i_p,
    ".word 0xd03b0088\n\t")
VU_CELL_FN(vu_vsgn_p,
    ".word 0xd04a0088\n\t")
VU_CELL_FN(vu_vsgn_t,
    ".word 0xd04a8008\n\t")
VU_CELL_FN(vu_vsgn_q,
    ".word 0xd04a8088\n\t")
VU_CELL_FN(vu_vflush_vmov,
    ".word 0xdc0100e4\n\t"
    ".word 0xffff040d\n\t"
    ".word 0xd0000008\n\t"
    ".word 0xd0000028\n\t")
VU_CELL_FN(vu_vflush_vadd,
    ".word 0xdc0100e4\n\t"
    ".word 0xffff040d\n\t"
    ".word 0x60040008\n\t"
    ".word 0x60040028\n\t")
VU_CELL_FN(vu_vflush_ctrl,
    ".word 0xdc0100e4\n\t"
    ".word 0xd0000008\n\t"
    ".word 0xd0000028\n\t")

/* The twelve source words (one record per word, per form). */
static const uint32_t s_vu_src[VU_SRC_WORDS] = {
    0x00000000u, 0x00000001u, 0x00008000u, 0x0000ffffu, 0xffff0000u, 0x80000000u,
    0x7fff8001u, 0xffff0001u, 0x12345678u, 0xffffffffu, 0x00010000u, 0x80008000u,
};

/* Float lanes for vsgn: the ten requested classes plus a negative qNaN, a signalling NaN. */
static const uint32_t s_vu_float[VU_IMAGES][4] = {
    {0x3f800000u, 0xbf800000u, 0x00000000u, 0x80000000u},
    {0x40200000u, 0xc0600000u, 0x7fc00000u, 0x7f800000u},
    {0x00000001u, 0x80000001u, 0xffc00000u, 0x7f800001u},
};

/* The vflush cell operands as floats: 3, 5, 7, 9 (C000) and 11, 13, 15, 17 (C010). */
static const uint32_t s_vu_flush[8] = {
    0x40400000u, 0x40a00000u, 0x40e00000u, 0x41100000u,
    0x41300000u, 0x41500000u, 0x41700000u, 0x41880000u,
};

struct vu_form {
    const char *stem;
    uint32_t word;
    uint32_t kind;  /* 0 = rotated source words, 1 = float images, 2 = vflush operands */
    uint32_t count; /* records per pass */
    vu_fn fn;
};
static const struct vu_form s_vu_forms[10] = {
    {"vus2i-s", 0xd03a0008u, 0u, 12u, vu_vus2i_s},
    {"vus2i-p", 0xd03a0088u, 0u, 12u, vu_vus2i_p},
    {"vs2i-s", 0xd03b0008u, 0u, 12u, vu_vs2i_s},
    {"vs2i-p", 0xd03b0088u, 0u, 12u, vu_vs2i_p},
    {"vsgn-p", 0xd04a0088u, 1u, 3u, vu_vsgn_p},
    {"vsgn-t", 0xd04a8008u, 1u, 3u, vu_vsgn_t},
    {"vsgn-q", 0xd04a8088u, 1u, 3u, vu_vsgn_q},
    {"vflush-vmov", VU_VFLUSH_WORD, 2u, 1u, vu_vflush_vmov},
    {"vflush-vadd", VU_VFLUSH_WORD, 2u, 1u, vu_vflush_vadd},
    {"vflush-ctrl", VU_VFLUSH_WORD, 2u, 1u, vu_vflush_ctrl},
};

static uint32_t s_vu_in[VU_IN_WORDS] __attribute__((aligned(64)));
static uint32_t s_vu_out[VU_OUT_WORDS] __attribute__((aligned(64)));

static void vu_cell(const char *name, const struct vu_form *form, unsigned index)
{
    for (unsigned i = 0; i < 4u; i++) {
        if (form->kind == 0u) {
            s_vu_in[i] = s_vu_src[(index + i) % VU_SRC_WORDS];
        } else if (form->kind == 1u) {
            s_vu_in[i] = s_vu_float[index][i];
        } else {
            s_vu_in[i] = s_vu_flush[i];
        }
        s_vu_in[4u + i] = form->kind == 2u ? s_vu_flush[4u + i] : (VU_BACKGROUND | (4u + i));
        s_vu_in[8u + i] = VU_BACKGROUND | (8u + i);
    }
    for (unsigned i = 0; i < VU_OUT_WORDS; i++) {
        s_vu_out[i] = 0xffffffffu;
    }
    form->fn(s_vu_in, s_vu_out);
    uint32_t w[VU_OUT_WORDS];
    for (unsigned i = 0; i < VU_OUT_WORDS; i++) {
        w[i] = s_vu_out[i];
    }
    hle_record(name, "PASS", form->word, w, VU_OUT_WORDS);
}

/* Rows <stem>-p<pass>-<index>, one step <stem>-p<pass> first. */
static void vu_group(unsigned pass, unsigned form)
{
    char name[64];
    snprintf(name, sizeof(name), "%s-p%u", s_vu_forms[form].stem, pass);
    hle_step(name);
    const unsigned n = s_vu_forms[form].count;
    for (unsigned index = 0; index < n; index++) {
        snprintf(name, sizeof(name), "%s-p%u-%u", s_vu_forms[form].stem, pass, index);
        vu_cell(name, &s_vu_forms[form], index);
    }
}

static int hle_run(void)
{
    /* Traps off before the first VFPU cell, as in the vfpu-compare case. */
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    for (unsigned pass = 0; pass < 2u; pass++) {
        for (unsigned form = 0; form < 10u; form++) {
            vu_group(pass, form);
        }
    }
    return 1;
}

#endif /* HLE_CASE_VFPU_VUS2I */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_VFPU_BASIC_ARITH

/* #326 basic VFPU arithmetic (PSP-VFPU-BASICARITH-001), one launch, register-only: the raw
   result words of vadd, vsub, vmul, vmin and vmax at .s/.p/.t/.q, vscl.p/.q, vdiv.s, vneg,
   vabs and vmov at .s/.p/.q, and the constant forms vzero, vone and vidt, over 20 operand
   classes: +-0, +-smallest denormal, +-largest denormal, +-smallest normal, +-1, +-2^127,
   +-FLT_MAX, +-inf, and a quiet and a signalling NaN of each sign with distinct payloads.
   Every cell is one instruction word run over the whole register image with no prefix
   (identity), emitted as a word; encodings are VFPU op 0x60 (vadd 0x00, vsub 0x80, vdiv 0x380
   in bits 23-31), 0x64 (vmul), 0x65 (vscl), 0x6d (vmin 0x00, vmax 0x80) and 0xd0 (op field in
   bits 16-22: vmov 0, vabs 1, vneg 2, vidt 3, vzero 6, vone 7), vd C200 (0x08), vs C000 (0),
   vt C010 or S010 (1), size bit 7 (.p), bit 15 (.t) or both (.q); all were checked against
   psp-as except vidt.s, which the assembler refuses (its word is the vidt pattern with no
   size bits). Binary forms: all 400 ordered pairs (A, B) of the 20 operands, A-major, run
   lane-packed (n lanes of one instruction per execution, the last execution of a record
   padded with its final pair); a record holds up to 60 pairs (7 records). vscl: one scalar B
   per execution, A packed across the lanes, 3 scalars per record (7 records). Unary forms:
   the 20 operands in order, one record. Constant forms: the four words C200 lanes 0-3 after
   the word, over a background image. Row <form>-<size>-p<pass>[-<record>]: result = the
   instruction word, out0.. = the destination words of each execution in order (n per
   execution). POWER RISK: vidt.s may raise a reserved-instruction exception that stops the
   launch (nothing here catches it); it runs after everything else in both passes, behind a
   durable step marker, and needs the owner present. */

#include "hle_vfpu_cells.h"

#define BA_OPERANDS 20u
#define BA_PAIRS (BA_OPERANDS * BA_OPERANDS)
#define BA_PAIRS_PER_REC 60u
#define BA_BIN_RECORDS 7u
#define BA_SCL_PER_REC 3u
#define BA_MAX_WORDS 64u
#define BA_DST 32u

typedef void (*ba_fn)(const uint32_t *in, uint32_t *out, uint32_t *cc);

/* Source C000 (image 0-3), second operand C010/S010 (image 4-7), destination C200 (image 32). */
TN_CELL_FN(ba_vadd_s, TN_PFX_NONE, "0x60010008")
TN_CELL_FN(ba_vadd_p, TN_PFX_NONE, "0x60010088")
TN_CELL_FN(ba_vadd_t, TN_PFX_NONE, "0x60018008")
TN_CELL_FN(ba_vadd_q, TN_PFX_NONE, "0x60018088")
TN_CELL_FN(ba_vsub_s, TN_PFX_NONE, "0x60810008")
TN_CELL_FN(ba_vsub_p, TN_PFX_NONE, "0x60810088")
TN_CELL_FN(ba_vsub_t, TN_PFX_NONE, "0x60818008")
TN_CELL_FN(ba_vsub_q, TN_PFX_NONE, "0x60818088")
TN_CELL_FN(ba_vmul_s, TN_PFX_NONE, "0x64010008")
TN_CELL_FN(ba_vmul_p, TN_PFX_NONE, "0x64010088")
TN_CELL_FN(ba_vmul_t, TN_PFX_NONE, "0x64018008")
TN_CELL_FN(ba_vmul_q, TN_PFX_NONE, "0x64018088")
TN_CELL_FN(ba_vmin_s, TN_PFX_NONE, "0x6d010008")
TN_CELL_FN(ba_vmin_p, TN_PFX_NONE, "0x6d010088")
TN_CELL_FN(ba_vmin_t, TN_PFX_NONE, "0x6d018008")
TN_CELL_FN(ba_vmin_q, TN_PFX_NONE, "0x6d018088")
TN_CELL_FN(ba_vmax_s, TN_PFX_NONE, "0x6d810008")
TN_CELL_FN(ba_vmax_p, TN_PFX_NONE, "0x6d810088")
TN_CELL_FN(ba_vmax_t, TN_PFX_NONE, "0x6d818008")
TN_CELL_FN(ba_vmax_q, TN_PFX_NONE, "0x6d818088")
TN_CELL_FN(ba_vscl_p, TN_PFX_NONE, "0x65010088")
TN_CELL_FN(ba_vscl_q, TN_PFX_NONE, "0x65018088")
TN_CELL_FN(ba_vdiv_s, TN_PFX_NONE, "0x63810008")
TN_CELL_FN(ba_vmov_s, TN_PFX_NONE, "0xd0000008")
TN_CELL_FN(ba_vmov_p, TN_PFX_NONE, "0xd0000088")
TN_CELL_FN(ba_vmov_q, TN_PFX_NONE, "0xd0008088")
TN_CELL_FN(ba_vabs_s, TN_PFX_NONE, "0xd0010008")
TN_CELL_FN(ba_vabs_p, TN_PFX_NONE, "0xd0010088")
TN_CELL_FN(ba_vabs_q, TN_PFX_NONE, "0xd0018088")
TN_CELL_FN(ba_vneg_s, TN_PFX_NONE, "0xd0020008")
TN_CELL_FN(ba_vneg_p, TN_PFX_NONE, "0xd0020088")
TN_CELL_FN(ba_vneg_q, TN_PFX_NONE, "0xd0028088")
TN_CELL_FN(ba_vzero_s, TN_PFX_NONE, "0xd0060008")
TN_CELL_FN(ba_vzero_p, TN_PFX_NONE, "0xd0060088")
TN_CELL_FN(ba_vzero_q, TN_PFX_NONE, "0xd0068088")
TN_CELL_FN(ba_vone_s, TN_PFX_NONE, "0xd0070008")
TN_CELL_FN(ba_vone_p, TN_PFX_NONE, "0xd0070088")
TN_CELL_FN(ba_vone_q, TN_PFX_NONE, "0xd0078088")
TN_CELL_FN(ba_vidt_s, TN_PFX_NONE, "0xd0030008")
TN_CELL_FN(ba_vidt_p, TN_PFX_NONE, "0xd0030088")
TN_CELL_FN(ba_vidt_q, TN_PFX_NONE, "0xd0038088")

/* The 20 operand classes. Quiet NaNs 0x7fc00011 and 0xffc00022, signalling NaNs 0x7f800033 and
   0xff800044 (quiet bit clear, non-zero payload). */
static const uint32_t s_ba_ops[BA_OPERANDS] = {
    0x00000000u, 0x80000000u,   /* +0, -0 */
    0x00000001u, 0x80000001u,   /* +-smallest denormal */
    0x007fffffu, 0x807fffffu,   /* +-largest denormal */
    0x00800000u, 0x80800000u,   /* +-smallest normal */
    0x3f800000u, 0xbf800000u,   /* +-1 */
    0x7f000000u, 0xff000000u,   /* +-2^127 */
    0x7f7fffffu, 0xff7fffffu,   /* +-FLT_MAX */
    0x7f800000u, 0xff800000u,   /* +-inf */
    0x7fc00011u, 0xffc00022u,   /* quiet NaN, both signs */
    0x7f800033u, 0xff800044u,   /* signalling NaN, both signs */
};

struct ba_form {
    const char *stem;
    uint32_t word;
    ba_fn fn;
    unsigned lanes;
};

/* Binary forms, five operations at four widths, in run order. */
static const struct ba_form s_ba_bin[20] = {
    {"vadd-s", 0x60010008u, ba_vadd_s, 1}, {"vadd-p", 0x60010088u, ba_vadd_p, 2},
    {"vadd-t", 0x60018008u, ba_vadd_t, 3}, {"vadd-q", 0x60018088u, ba_vadd_q, 4},
    {"vsub-s", 0x60810008u, ba_vsub_s, 1}, {"vsub-p", 0x60810088u, ba_vsub_p, 2},
    {"vsub-t", 0x60818008u, ba_vsub_t, 3}, {"vsub-q", 0x60818088u, ba_vsub_q, 4},
    {"vmul-s", 0x64010008u, ba_vmul_s, 1}, {"vmul-p", 0x64010088u, ba_vmul_p, 2},
    {"vmul-t", 0x64018008u, ba_vmul_t, 3}, {"vmul-q", 0x64018088u, ba_vmul_q, 4},
    {"vmin-s", 0x6d010008u, ba_vmin_s, 1}, {"vmin-p", 0x6d010088u, ba_vmin_p, 2},
    {"vmin-t", 0x6d018008u, ba_vmin_t, 3}, {"vmin-q", 0x6d018088u, ba_vmin_q, 4},
    {"vmax-s", 0x6d810008u, ba_vmax_s, 1}, {"vmax-p", 0x6d810088u, ba_vmax_p, 2},
    {"vmax-t", 0x6d818008u, ba_vmax_t, 3}, {"vmax-q", 0x6d818088u, ba_vmax_q, 4},
};
/* vdiv.s (one lane) and vscl.p/.q (scalar second operand). */
static const struct ba_form s_ba_div = {"vdiv-s", 0x63810008u, ba_vdiv_s, 1};
static const struct ba_form s_ba_scl[2] = {
    {"vscl-p", 0x65010088u, ba_vscl_p, 2}, {"vscl-q", 0x65018088u, ba_vscl_q, 4},
};
/* vmov, vabs, vneg at .s/.p/.q over the 20 operands. */
static const struct ba_form s_ba_unary[9] = {
    {"vmov-s", 0xd0000008u, ba_vmov_s, 1}, {"vmov-p", 0xd0000088u, ba_vmov_p, 2},
    {"vmov-q", 0xd0008088u, ba_vmov_q, 4}, {"vabs-s", 0xd0010008u, ba_vabs_s, 1},
    {"vabs-p", 0xd0010088u, ba_vabs_p, 2}, {"vabs-q", 0xd0018088u, ba_vabs_q, 4},
    {"vneg-s", 0xd0020008u, ba_vneg_s, 1}, {"vneg-p", 0xd0020088u, ba_vneg_p, 2},
    {"vneg-q", 0xd0028088u, ba_vneg_q, 4},
};
/* vzero, vone, vidt: vidt.s is run apart from the rest, last (POWER RISK). */
static const struct ba_form s_ba_const[8] = {
    {"vzero-s", 0xd0060008u, ba_vzero_s, 1}, {"vzero-p", 0xd0060088u, ba_vzero_p, 2},
    {"vzero-q", 0xd0068088u, ba_vzero_q, 4}, {"vone-s", 0xd0070008u, ba_vone_s, 1},
    {"vone-p", 0xd0070088u, ba_vone_p, 2},   {"vone-q", 0xd0078088u, ba_vone_q, 4},
    {"vidt-p", 0xd0030088u, ba_vidt_p, 2},   {"vidt-q", 0xd0038088u, ba_vidt_q, 4},
};
static const struct ba_form s_ba_vidt_s = {"vidt-s", 0xd0030008u, ba_vidt_s, 1};

static uint32_t s_ba_in[TN_IMAGE_WORDS] __attribute__((aligned(64)));
static uint32_t s_ba_out[TN_IMAGE_WORDS] __attribute__((aligned(64)));

/* One execution: background image word i = 0x3f800000 | i << 16, A over image 0-3, B over
   image 4-7, then the word; the first n destination words (image 32..) go to dst. */
static void ba_exec(ba_fn fn, const uint32_t *a, const uint32_t *b, uint32_t *dst, unsigned n)
{
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        s_ba_in[i] = 0x3f800000u | (i << 16);
    }
    for (unsigned i = 0; i < 4u; i++) {
        s_ba_in[i] = a[i];
        s_ba_in[4u + i] = b[i];
    }
    uint32_t cc[2] = {0u, 0u};
    fn(s_ba_in, s_ba_out, cc);
    for (unsigned i = 0; i < n; i++) {
        dst[i] = s_ba_out[BA_DST + i];
    }
}

/* Row <form>-p<pass>-<rec>: pairs 60*rec .. (A-major k = 20 * a + b) of the 400, lane l of
   execution e taking pair 60*rec + e*n + l, clamped to the last pair of the record. */
static void ba_binary_record(const char *name, const struct ba_form *f, unsigned rec)
{
    uint32_t w[BA_MAX_WORDS];
    const unsigned first = rec * BA_PAIRS_PER_REC;
    unsigned len = BA_PAIRS - first;
    if (len > BA_PAIRS_PER_REC) {
        len = BA_PAIRS_PER_REC;
    }
    const unsigned n = f->lanes;
    const unsigned nexec = (len + n - 1u) / n;
    unsigned count = 0;
    for (unsigned e = 0; e < nexec; e++) {
        uint32_t a[4] = {0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u};
        uint32_t b[4] = {0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u};
        for (unsigned l = 0; l < n; l++) {
            unsigned k = e * n + l;
            if (k >= len) {
                k = len - 1u;
            }
            k += first;
            a[l] = s_ba_ops[k / BA_OPERANDS];
            b[l] = s_ba_ops[k % BA_OPERANDS];
        }
        ba_exec(f->fn, a, b, &w[count], n);
        count += n;
    }
    hle_record(name, "PASS", f->word, w, count);
}

/* Row vscl-<size>-p<pass>-<rec>: scalars 3*rec .. 3*rec + 2 (operand index), each over the 20
   operands A packed n to an execution. */
static void ba_scale_record(const char *name, const struct ba_form *f, unsigned rec)
{
    uint32_t w[BA_MAX_WORDS];
    const unsigned n = f->lanes;
    unsigned count = 0;
    unsigned s_end = (rec + 1u) * BA_SCL_PER_REC;
    if (s_end > BA_OPERANDS) {
        s_end = BA_OPERANDS;
    }
    for (unsigned s = rec * BA_SCL_PER_REC; s < s_end; s++) {
        for (unsigned e = 0; e < BA_OPERANDS / n; e++) {
            uint32_t a[4] = {0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u};
            uint32_t b[4] = {s_ba_ops[s], 0x3f800000u, 0x3f800000u, 0x3f800000u};
            for (unsigned l = 0; l < n; l++) {
                a[l] = s_ba_ops[e * n + l];
            }
            ba_exec(f->fn, a, b, &w[count], n);
            count += n;
        }
    }
    hle_record(name, "PASS", f->word, w, count);
}

/* Row <form>-p<pass>: the 20 operands in order over the lanes, n to an execution. */
static void ba_unary_record(const char *name, const struct ba_form *f)
{
    uint32_t w[BA_MAX_WORDS];
    const unsigned n = f->lanes;
    unsigned count = 0;
    for (unsigned e = 0; e < BA_OPERANDS / n; e++) {
        uint32_t a[4] = {0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u};
        const uint32_t b[4] = {0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u};
        for (unsigned l = 0; l < n; l++) {
            a[l] = s_ba_ops[e * n + l];
        }
        ba_exec(f->fn, a, b, &w[count], n);
        count += n;
    }
    hle_record(name, "PASS", f->word, w, count);
}

/* Row <form>-p<pass>: C200 lanes 0-3 after the word over the background image. */
static void ba_const_record(const char *name, const struct ba_form *f)
{
    uint32_t w[4];
    const uint32_t a[4] = {0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u};
    ba_exec(f->fn, a, a, w, 4u);
    hle_record(name, "PASS", f->word, w, 4u);
}

/* One durable step per form group (form-p<pass>) before its first cell. */
static void ba_binary_group(unsigned pass, const struct ba_form *f)
{
    char name[64];
    snprintf(name, sizeof(name), "%s-p%u", f->stem, pass);
    hle_step(name);
    for (unsigned rec = 0; rec < BA_BIN_RECORDS; rec++) {
        snprintf(name, sizeof(name), "%s-p%u-%u", f->stem, pass, rec);
        ba_binary_record(name, f, rec);
    }
}

static void ba_scale_group(unsigned pass, const struct ba_form *f)
{
    char name[64];
    snprintf(name, sizeof(name), "%s-p%u", f->stem, pass);
    hle_step(name);
    for (unsigned rec = 0; rec < BA_BIN_RECORDS; rec++) {
        snprintf(name, sizeof(name), "%s-p%u-%u", f->stem, pass, rec);
        ba_scale_record(name, f, rec);
    }
}

static void ba_unary_group(unsigned pass, const struct ba_form *f)
{
    char name[64];
    snprintf(name, sizeof(name), "%s-p%u", f->stem, pass);
    hle_step(name);
    ba_unary_record(name, f);
}

static void ba_const_group(unsigned pass, const struct ba_form *f)
{
    char name[64];
    snprintf(name, sizeof(name), "%s-p%u", f->stem, pass);
    hle_step(name);
    ba_const_record(name, f);
}

static int hle_run(void)
{
    /* Traps off before the first VFPU cell, as in the vfpu-compare case. */
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    for (unsigned pass = 0; pass < 2u; pass++) {
        for (unsigned form = 0; form < 20u; form++) {
            ba_binary_group(pass, &s_ba_bin[form]);
        }
        for (unsigned form = 0; form < 2u; form++) {
            ba_scale_group(pass, &s_ba_scl[form]);
        }
        ba_binary_group(pass, &s_ba_div);
        for (unsigned form = 0; form < 9u; form++) {
            ba_unary_group(pass, &s_ba_unary[form]);
        }
        for (unsigned form = 0; form < 8u; form++) {
            ba_const_group(pass, &s_ba_const[form]);
        }
    }
    /* The one form the assembler refuses runs last, in both passes. */
    for (unsigned pass = 0; pass < 2u; pass++) {
        ba_const_group(pass, &s_ba_vidt_s);
    }
    return 1;
}

#endif /* HLE_CASE_VFPU_BASIC_ARITH */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_VFPU_WIDE_DOT

/* #326 wide dot-product and matrix VFPU forms (PSP-VFPU-WIDEDOT-001), two passes, register-only:
   the raw result words of vdot.p, vdot.q, vhdp.q, vcrsp.t, vqmul.q, vtfm4.q, vhtfm4.q, vmmul.q (two
   source-matrix encodings), vmscl.q, vmzero.q, vmidt.q, vmone.q, vmmov.q and vrot.q. Every cell
   is one instruction word run over the whole register image with no prefix word issued (the
   prefixes stay identity), emitted as a word. Every word was assembled with psp-as from the
   mnemonic named here and none was refused: vdot.p S200, C000, C010 (0x64810088); vdot.q
   (0x64818088); vhdp.q (0x66018088); vcrsp.t C200, C000, C010 (0xf2818008); vqmul.q C200, C000,
   C010 (0xf2818088); vtfm4.q C200, M100, C000 (0xf1808488); vhtfm4.q C200, M100, C000 (0xf1808408;
   the vector is .t, the matrix 4x4); vmmul.q M200, E100, M000 (0xf0008488) and vmmul.q M200, M100,
   M000 (0xf000a488, the transposed first-source encoding); vmscl.q M200, M100, S010 (0xf2018488);
   vmzero.q M200 (0xf3868088); vmidt.q M200 (0xf3838088); vmone.q M200 (0xf3878088); vmmov.q M200,
   M100 (0xf3808488); vrot.q C200, S000, [c,s,0,0] (0xf3a48088). Image layout (word m*16 + c*4 + r):
   vector sources C000 (a, image 0-3) and C010 (b, image 4-7), the matrix of vtfm/vhtfm/vmscl/vmmov
   M100 (image 16-31), vmmul vs = M100 and vt = M000 (image 0-15), the destination S200/C200/M200
   (image 32..). The 80-word limit of a record (the writer's 1536-byte line) bounds a record.
   Operands. Base vectors a = (1.5, -2.25, 3.0, 0.625), b = (0.75, 4.0, -1.5, 2.5), base matrix a
   fixed 16-word finite table (the vmmul vt matrix a fixed permutation of it). Special words: +qNaN
   0x7fc00011, -NaN 0xffc00022, sNaN 0x7f800033, +inf, -inf, +denormal 0x00000001, -denormal
   0x80000001 and -0, each placed in one lane at a time of a, then of b (vdot, vhdp, vcrsp, vqmul),
   in one lane at a time of the vector and in each of the four diagonal matrix elements (vtfm,
   vhtfm), and in each of the four diagonal elements of each source matrix (vmmul). The matrix
   sweep is the diagonal positions only (16 positions x 8 values x 2 matrices would need many times
   the records); vmscl takes the specials in the scalar and in matrix element 0, vmmov in element
   5. Order-sensitive four-term sums (b or the matrix all ones, the sum shown by the rounding):
   (1, 2^-24, 2^-24, 2^-24), the same reversed, (2^24, 1, 1, 1), the same reversed, (1e8, 1,
   -1e8, 1), (1, 1e8, 1, -1e8), (1e8, -1e8, 1, 1) and (1, 1, 1e8, -1e8); for vmmul four of the
   vectors fill every column of one matrix while the other is all ones, in both arrangements; vdot.p
   has no order cells (a two-term sum is order free); vcrsp.t takes the first three lanes. vrot:
   sixteen angle words (0, -0, 0.25, 0.5, 1, 1.5, 2, 3, 4, 100, -0.5, -1, +inf, -inf, qNaN and the
   smallest denormal).
   Row <form>-<size>-p<pass>[-<record>] (one record when the cells fit, else numbered): result =
   the instruction word; per cell in order, the recorded source image words (a then b for the
   vector forms, vector then matrix for vtfm/vhtfm, image 0-31 for vmmul, scalar then matrix for
   vmscl, the matrix for vmmov, the angle for vrot) followed by the destination words. A durable
   step marker comes before every form group. Nothing here writes memory outside the probe's
   own image and output buffers, no control register is written, and no reserved word is run. */

#include "hle_vfpu_cells.h"

#define WD_MAX_WORDS 80u
#define WD_SPECIALS 8u
#define WD_ORDERS 8u
#define WD_FORMS 15u
#define WD_DST 32u
#define WD_ONE 0x3f800000u

enum { WD_VEC, WD_TFM, WD_MMUL, WD_MSCL, WD_MMOV, WD_CONST, WD_ROT };

typedef void (*wd_fn)(const uint32_t *in, uint32_t *out, uint32_t *cc);

TN_CELL_FN(wd_vdot_p, TN_PFX_NONE, "0x64810088")
TN_CELL_FN(wd_vdot_q, TN_PFX_NONE, "0x64818088")
TN_CELL_FN(wd_vhdp_q, TN_PFX_NONE, "0x66018088")
TN_CELL_FN(wd_vcrsp_t, TN_PFX_NONE, "0xf2818008")
TN_CELL_FN(wd_vqmul_q, TN_PFX_NONE, "0xf2818088")
TN_CELL_FN(wd_vtfm_q, TN_PFX_NONE, "0xf1808488")
TN_CELL_FN(wd_vhtfm_q, TN_PFX_NONE, "0xf1808408")
TN_CELL_FN(wd_vmmul_q, TN_PFX_NONE, "0xf0008488")
TN_CELL_FN(wd_vmmul_qt, TN_PFX_NONE, "0xf000a488")
TN_CELL_FN(wd_vmscl_q, TN_PFX_NONE, "0xf2018488")
TN_CELL_FN(wd_vmzero_q, TN_PFX_NONE, "0xf3868088")
TN_CELL_FN(wd_vmidt_q, TN_PFX_NONE, "0xf3838088")
TN_CELL_FN(wd_vmone_q, TN_PFX_NONE, "0xf3878088")
TN_CELL_FN(wd_vmmov_q, TN_PFX_NONE, "0xf3808488")
TN_CELL_FN(wd_vrot_q, TN_PFX_NONE, "0xf3a48088")

struct wd_form {
    const char *stem;
    uint32_t word;
    wd_fn fn;
    unsigned kind;
    unsigned lanes;   /* vector lanes (WD_VEC, WD_TFM) */
    unsigned nscen;   /* cells */
    unsigned nout;    /* destination words recorded per cell */
    unsigned pos0, cnt0, pos1, cnt1; /* recorded source image words: two runs */
};

static const struct wd_form s_wd_forms[WD_FORMS] = {
    {"vdot-p", 0x64810088u, wd_vdot_p, WD_VEC, 2, 33, 1, 0, 2, 4, 2},
    {"vdot-q", 0x64818088u, wd_vdot_q, WD_VEC, 4, 73, 1, 0, 4, 4, 4},
    {"vhdp-q", 0x66018088u, wd_vhdp_q, WD_VEC, 4, 73, 1, 0, 4, 4, 4},
    {"vcrsp-t", 0xf2818008u, wd_vcrsp_t, WD_VEC, 3, 57, 3, 0, 3, 4, 3},
    {"vqmul-q", 0xf2818088u, wd_vqmul_q, WD_VEC, 4, 73, 4, 0, 4, 4, 4},
    {"vtfm-q", 0xf1808488u, wd_vtfm_q, WD_TFM, 4, 73, 4, 0, 4, 16, 16},
    {"vhtfm-q", 0xf1808408u, wd_vhtfm_q, WD_TFM, 3, 65, 4, 0, 3, 16, 16},
    {"vmmul-q", 0xf0008488u, wd_vmmul_q, WD_MMUL, 0, 73, 16, 0, 16, 16, 16},
    {"vmmul-qt", 0xf000a488u, wd_vmmul_qt, WD_MMUL, 0, 1, 16, 0, 16, 16, 16},
    {"vmscl-q", 0xf2018488u, wd_vmscl_q, WD_MSCL, 0, 17, 16, 4, 1, 16, 16},
    {"vmzero-q", 0xf3868088u, wd_vmzero_q, WD_CONST, 0, 1, 16, 0, 0, 0, 0},
    {"vmidt-q", 0xf3838088u, wd_vmidt_q, WD_CONST, 0, 1, 16, 0, 0, 0, 0},
    {"vmone-q", 0xf3878088u, wd_vmone_q, WD_CONST, 0, 1, 16, 0, 0, 0, 0},
    {"vmmov-q", 0xf3808488u, wd_vmmov_q, WD_MMOV, 0, 9, 16, 16, 16, 0, 0},
    {"vrot-q", 0xf3a48088u, wd_vrot_q, WD_ROT, 0, 16, 4, 0, 1, 0, 0},
};

/* +qNaN, -NaN, sNaN (quiet bit clear, non-zero payload), +inf, -inf, +-smallest denormal, -0. */
static const uint32_t s_wd_spec[WD_SPECIALS] = {
    0x7fc00011u, 0xffc00022u, 0x7f800033u, 0x7f800000u,
    0xff800000u, 0x00000001u, 0x80000001u, 0x80000000u,
};
static const uint32_t s_wd_va[4] = {0x3fc00000u, 0xc0100000u, 0x40400000u, 0x3f200000u};
static const uint32_t s_wd_vb[4] = {0x3f400000u, 0x40800000u, 0xbfc00000u, 0x40200000u};
/* 1, 2, -3, 0.5, -1.5, 4, 0.25, -2, 3.5, -0.75, 1.25, 6, -4.5, 0.125, 2.75, -1. */
static const uint32_t s_wd_mat[16] = {
    0x3f800000u, 0x40000000u, 0xc0400000u, 0x3f000000u, 0xbfc00000u, 0x40800000u,
    0x3e800000u, 0xc0000000u, 0x40600000u, 0xbf400000u, 0x3fa00000u, 0x40c00000u,
    0xc0900000u, 0x3e000000u, 0x40300000u, 0xbf800000u,
};
/* 2^-24 = 0x33800000, 2^24 = 0x4b800000, 1e8 = 0x4cbebc20. */
static const uint32_t s_wd_ord[WD_ORDERS][4] = {
    {0x3f800000u, 0x33800000u, 0x33800000u, 0x33800000u},
    {0x33800000u, 0x33800000u, 0x33800000u, 0x3f800000u},
    {0x4b800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u},
    {0x3f800000u, 0x3f800000u, 0x3f800000u, 0x4b800000u},
    {0x4cbebc20u, 0x3f800000u, 0xccbebc20u, 0x3f800000u},
    {0x3f800000u, 0x4cbebc20u, 0x3f800000u, 0xccbebc20u},
    {0x4cbebc20u, 0xccbebc20u, 0x3f800000u, 0x3f800000u},
    {0x3f800000u, 0x3f800000u, 0x4cbebc20u, 0xccbebc20u},
};
static const uint32_t s_wd_angle[16] = {
    0x00000000u, 0x80000000u, 0x3e800000u, 0x3f000000u, 0x3f800000u, 0x3fc00000u,
    0x40000000u, 0x40400000u, 0x40800000u, 0x42c80000u, 0xbf000000u, 0xbf800000u,
    0x7f800000u, 0xff800000u, 0x7fc00011u, 0x00000001u,
};

static uint32_t s_wd_in[TN_IMAGE_WORDS] __attribute__((aligned(64)));
static uint32_t s_wd_out[TN_IMAGE_WORDS] __attribute__((aligned(64)));

/* Overlays the cell's operands on the background image s_wd_in (word i = 0x3f800000 | i << 16).
   Cell 0 is the finite base; the sweep and order cells follow as the comment above lists them. */
static void wd_build(const struct wd_form *f, unsigned idx)
{
    uint32_t *img = s_wd_in;
    const unsigned n = f->lanes;
    switch (f->kind) {
    case WD_VEC: {
        uint32_t a[4] = {WD_ONE, WD_ONE, WD_ONE, WD_ONE};
        uint32_t b[4] = {WD_ONE, WD_ONE, WD_ONE, WD_ONE};
        for (unsigned l = 0; l < n; l++) {
            a[l] = s_wd_va[l];
            b[l] = s_wd_vb[l];
        }
        if (idx >= 1u + 16u * n) {
            const unsigned o = idx - 1u - 16u * n;
            for (unsigned l = 0; l < n; l++) {
                a[l] = s_wd_ord[o][l];
                b[l] = WD_ONE;
            }
        } else if (idx >= 1u) {
            const unsigned k = idx - 1u;
            const unsigned lane = (k / 8u) % n;
            if (k / (8u * n) == 0u) {
                a[lane] = s_wd_spec[k % 8u];
            } else {
                b[lane] = s_wd_spec[k % 8u];
            }
        }
        for (unsigned l = 0; l < n; l++) {
            img[l] = a[l];
            img[4u + l] = b[l];
        }
        break;
    }
    case WD_TFM: {
        uint32_t v[4] = {WD_ONE, WD_ONE, WD_ONE, WD_ONE};
        uint32_t m[16];
        for (unsigned l = 0; l < n; l++) {
            v[l] = s_wd_va[l];
        }
        for (unsigned i = 0; i < 16u; i++) {
            m[i] = s_wd_mat[i];
        }
        if (idx >= 1u + 8u * n + 32u) {
            const unsigned o = idx - 1u - 8u * n - 32u;
            for (unsigned l = 0; l < n; l++) {
                v[l] = s_wd_ord[o][l];
            }
            for (unsigned i = 0; i < 16u; i++) {
                m[i] = WD_ONE;
            }
        } else if (idx >= 1u + 8u * n) {
            const unsigned k = idx - 1u - 8u * n;
            m[5u * (k / 8u)] = s_wd_spec[k % 8u];
        } else if (idx >= 1u) {
            const unsigned k = idx - 1u;
            v[k / 8u] = s_wd_spec[k % 8u];
        }
        for (unsigned l = 0; l < n; l++) {
            img[l] = v[l];
        }
        for (unsigned i = 0; i < 16u; i++) {
            img[16u + i] = m[i];
        }
        break;
    }
    case WD_MMUL: {
        /* vt = image 0-15 (mb), vs = image 16-31 (ma). */
        uint32_t ma[16];
        uint32_t mb[16];
        for (unsigned i = 0; i < 16u; i++) {
            ma[i] = s_wd_mat[i];
            mb[i] = s_wd_mat[(5u * i + 3u) & 15u];
        }
        if (idx >= 65u) {
            const unsigned j = idx - 65u;
            const uint32_t *vec = s_wd_ord[(j % 4u) * 2u];
            for (unsigned i = 0; i < 16u; i++) {
                if (j / 4u == 0u) {
                    ma[i] = WD_ONE;
                    mb[i] = vec[i % 4u];
                } else {
                    mb[i] = WD_ONE;
                    ma[i] = vec[i % 4u];
                }
            }
        } else if (idx >= 33u) {
            const unsigned k = idx - 33u;
            mb[5u * (k / 8u)] = s_wd_spec[k % 8u];
        } else if (idx >= 1u) {
            const unsigned k = idx - 1u;
            ma[5u * (k / 8u)] = s_wd_spec[k % 8u];
        }
        for (unsigned i = 0; i < 16u; i++) {
            img[i] = mb[i];
            img[16u + i] = ma[i];
        }
        break;
    }
    case WD_MSCL: {
        uint32_t m[16];
        uint32_t scalar = 0x40200000u;
        for (unsigned i = 0; i < 16u; i++) {
            m[i] = s_wd_mat[i];
        }
        if (idx >= 9u) {
            m[0] = s_wd_spec[idx - 9u];
        } else if (idx >= 1u) {
            scalar = s_wd_spec[idx - 1u];
        }
        img[4] = scalar;
        for (unsigned i = 0; i < 16u; i++) {
            img[16u + i] = m[i];
        }
        break;
    }
    case WD_MMOV: {
        for (unsigned i = 0; i < 16u; i++) {
            img[16u + i] = s_wd_mat[i];
        }
        if (idx >= 1u) {
            img[16u + 5u] = s_wd_spec[idx - 1u];
        }
        break;
    }
    case WD_ROT:
        img[0] = s_wd_angle[idx];
        break;
    default:
        break;
    }
}

/* One cell: background image, operand overlay, the word; dst gets the recorded source words and
   then the destination words (image 32..), and the number of words is returned. */
static unsigned wd_exec(const struct wd_form *f, unsigned idx, uint32_t *dst)
{
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        s_wd_in[i] = 0x3f800000u | (i << 16);
    }
    wd_build(f, idx);
    const unsigned c0 = f->cnt0;
    const unsigned c1 = f->cnt1;
    const unsigned nout = f->nout;
    unsigned count = 0;
    for (unsigned i = 0; i < c0; i++) {
        dst[count++] = s_wd_in[f->pos0 + i];
    }
    for (unsigned i = 0; i < c1; i++) {
        dst[count++] = s_wd_in[f->pos1 + i];
    }
    uint32_t cc[2] = {0u, 0u};
    f->fn(s_wd_in, s_wd_out, cc);
    for (unsigned i = 0; i < nout; i++) {
        dst[count++] = s_wd_out[WD_DST + i];
    }
    return count;
}

/* One durable step per form group, then its records: as many cells as fit in WD_MAX_WORDS. */
static void wd_group(unsigned pass, const struct wd_form *f)
{
    char name[64];
    uint32_t w[WD_MAX_WORDS];
    const unsigned per = WD_MAX_WORDS / (f->cnt0 + f->cnt1 + f->nout);
    const unsigned nrec = (f->nscen + per - 1u) / per;
    snprintf(name, sizeof(name), "%s-p%u", f->stem, pass);
    hle_step(name);
    for (unsigned rec = 0; rec < nrec; rec++) {
        const unsigned first = rec * per;
        unsigned last = first + per;
        if (last > f->nscen) {
            last = f->nscen;
        }
        unsigned count = 0;
        for (unsigned s = first; s < last; s++) {
            count += wd_exec(f, s, &w[count]);
        }
        if (nrec > 1u) {
            snprintf(name, sizeof(name), "%s-p%u-%u", f->stem, pass, rec);
        }
        hle_record(name, "PASS", f->word, w, count);
    }
}

static int hle_run(void)
{
    /* Traps off before the first VFPU cell, as in the vfpu-compare case. */
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    for (unsigned pass = 0; pass < 2u; pass++) {
        for (unsigned form = 0; form < WD_FORMS; form++) {
            wd_group(pass, &s_wd_forms[form]);
        }
    }
    return 1;
}

#endif /* HLE_CASE_VFPU_WIDE_DOT */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_VFPU_MFV_MTV

/* #326 mtv.s then mfv.s (PSP-VFPU-MFVMTV-001), one launch, register-only: whether any bit of a
   general-register word changes on the way into a VFPU register and back out. For each of 17
   words (+0, -0, +smallest denormal 0x00000001, -smallest denormal 0x80000001, +/-largest
   denormal, quiet NaNs 0x7fc00011 and 0xffc00022, signalling NaNs 0x7f800033 and 0xff800044,
   +/-inf, FLT_MAX, 1.0, 0xdeadbeef, 0xffffffff and a closing +0) $t0 is moved into a VFPU
   register with mtv and read back into $t1 with mfv, through two registers: S000 (mtv word
   0x48e80000, mfv word 0x48690000) and S733 (0x48e8007f and 0x4869007f). The four words were
   checked against psp-as (mtv $t0,S000; mfv $t1,S000; mtv $t0,S733; mfv $t1,S733) and are
   emitted as words. The closing +0 follows 0xffffffff so a stale-state effect would show.
   Row mtv-mfv-p<pass>: result = the mtv S000 word, out0..16 = the S000 read-back words in the
   input order; row mtv-mfv2-p<pass>: result = the mtv S733 word, out0..16 = the S733 read-back
   words. No control register is written (mtvc is already measured) and no memory is written
   beyond the probe's scratch words. */

#define MM_WORDS 17u

static const uint32_t s_mm_in[MM_WORDS] = {
    0x00000000u, 0x80000000u,   /* +0, -0 */
    0x00000001u, 0x80000001u,   /* +-smallest denormal */
    0x007fffffu, 0x807fffffu,   /* +-largest denormal */
    0x7fc00011u, 0xffc00022u,   /* quiet NaN, both signs */
    0x7f800033u, 0xff800044u,   /* signalling NaN, both signs */
    0x7f800000u, 0xff800000u,   /* +-inf */
    0x7f7fffffu, 0x3f800000u,   /* FLT_MAX, 1.0 */
    0xdeadbeefu, 0xffffffffu,   /* patterns */
    0x00000000u,                /* closing +0 */
};

/* out[0] = the word through S000, out[1] = through S733. */
static void mm_cell(uint32_t word, uint32_t *out)
{
    __asm__ volatile(
        "move $t0, %[w]\n\t"
        ".word 0x48e80000\n\t" /* mtv $t0, S000 */
        ".word 0x48690000\n\t" /* mfv $t1, S000 */
        "sw $t1, 0(%[out])\n\t"
        ".word 0x48e8007f\n\t" /* mtv $t0, S733 */
        ".word 0x4869007f\n\t" /* mfv $t1, S733 */
        "sw $t1, 4(%[out])\n\t"
        :
        : [w] "r"(word), [out] "r"(out)
        : "t0", "t1", "memory");
}

static void mm_pass(unsigned pass)
{
    char name[64];
    uint32_t a[MM_WORDS];
    uint32_t b[MM_WORDS];
    snprintf(name, sizeof(name), "mtv-mfv-p%u", pass);
    hle_step(name);
    for (unsigned i = 0; i < MM_WORDS; i++) {
        uint32_t pair[2] = {0u, 0u};
        mm_cell(s_mm_in[i], pair);
        a[i] = pair[0];
        b[i] = pair[1];
    }
    hle_record(name, "PASS", 0x48e80000u, a, MM_WORDS);
    snprintf(name, sizeof(name), "mtv-mfv2-p%u", pass);
    hle_record(name, "PASS", 0x48e8007fu, b, MM_WORDS);
}

static int hle_run(void)
{
    /* Traps off before the first VFPU cell, as in the vfpu-compare case. */
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    for (unsigned pass = 0; pass < 2u; pass++) {
        mm_pass(pass);
    }
    return 1;
}

#endif /* HLE_CASE_VFPU_MFV_MTV */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_VFPU_CONSTANTS

/* #326 VFPU constants (PSP-VFPU-CONSTANTS-001), two passes, register-only: the raw destination
   words of vcst.s and vcst.q for every constant index 0-31. Encoding 0xd0600000 | index << 16,
   size bits 0x0008 (.s, vd S200) or 0x8088 (.q, vd C200). psp-as accepts only the named
   constants VFPU_HUGE .. VFPU_SQRT3_2 (indices 1-19) and refuses a numeric index; the words it
   emits for those 19 names match these encodings, so every cell (index 0 and the reserved
   indices 20-31 included) is emitted as a word. Every cell runs over an image whose 128 words
   are all the sentinel 0x7a5a5a5a (TN_SENTINEL), so a destination lane the instruction leaves
   unwritten reads back as the sentinel. Rows: vcst-<s|q>-p<pass> (indices 0-19: result = the
   instruction word of index 0, out = lane 0 of C200 (.s) or lanes 0-3 (.q) per index in order,
   then one guard word, the first destination word past the written lanes, read after the last
   index) and vcst-<s|q>-res-p<pass> (indices 20-31, same layout, result = the instruction word
   of index 20). The reserved indices run after both passes of the rest, one step marker each,
   in the order p0 .s, p0 .q, p1 .s, p1 .q. POWER RISK: a reserved index may raise a
   reserved-instruction exception that stops the launch (nothing here catches it); it needs the
   owner present. No control register is written. */

#include "hle_vfpu_cells.h"

#define VC_INDICES 32u
#define VC_CORE 20u
#define VC_DST 32u
#define VC_MAX_WORDS 96u

typedef void (*vc_fn)(const uint32_t *in, uint32_t *out, uint32_t *cc);

/* Destination C200 (image 32..35); vd 0x08, no source operand. */
TN_CELL_FN(vc_s_0, TN_PFX_NONE, "0xd0600008")
TN_CELL_FN(vc_s_1, TN_PFX_NONE, "0xd0610008")
TN_CELL_FN(vc_s_2, TN_PFX_NONE, "0xd0620008")
TN_CELL_FN(vc_s_3, TN_PFX_NONE, "0xd0630008")
TN_CELL_FN(vc_s_4, TN_PFX_NONE, "0xd0640008")
TN_CELL_FN(vc_s_5, TN_PFX_NONE, "0xd0650008")
TN_CELL_FN(vc_s_6, TN_PFX_NONE, "0xd0660008")
TN_CELL_FN(vc_s_7, TN_PFX_NONE, "0xd0670008")
TN_CELL_FN(vc_s_8, TN_PFX_NONE, "0xd0680008")
TN_CELL_FN(vc_s_9, TN_PFX_NONE, "0xd0690008")
TN_CELL_FN(vc_s_10, TN_PFX_NONE, "0xd06a0008")
TN_CELL_FN(vc_s_11, TN_PFX_NONE, "0xd06b0008")
TN_CELL_FN(vc_s_12, TN_PFX_NONE, "0xd06c0008")
TN_CELL_FN(vc_s_13, TN_PFX_NONE, "0xd06d0008")
TN_CELL_FN(vc_s_14, TN_PFX_NONE, "0xd06e0008")
TN_CELL_FN(vc_s_15, TN_PFX_NONE, "0xd06f0008")
TN_CELL_FN(vc_s_16, TN_PFX_NONE, "0xd0700008")
TN_CELL_FN(vc_s_17, TN_PFX_NONE, "0xd0710008")
TN_CELL_FN(vc_s_18, TN_PFX_NONE, "0xd0720008")
TN_CELL_FN(vc_s_19, TN_PFX_NONE, "0xd0730008")
TN_CELL_FN(vc_s_20, TN_PFX_NONE, "0xd0740008")
TN_CELL_FN(vc_s_21, TN_PFX_NONE, "0xd0750008")
TN_CELL_FN(vc_s_22, TN_PFX_NONE, "0xd0760008")
TN_CELL_FN(vc_s_23, TN_PFX_NONE, "0xd0770008")
TN_CELL_FN(vc_s_24, TN_PFX_NONE, "0xd0780008")
TN_CELL_FN(vc_s_25, TN_PFX_NONE, "0xd0790008")
TN_CELL_FN(vc_s_26, TN_PFX_NONE, "0xd07a0008")
TN_CELL_FN(vc_s_27, TN_PFX_NONE, "0xd07b0008")
TN_CELL_FN(vc_s_28, TN_PFX_NONE, "0xd07c0008")
TN_CELL_FN(vc_s_29, TN_PFX_NONE, "0xd07d0008")
TN_CELL_FN(vc_s_30, TN_PFX_NONE, "0xd07e0008")
TN_CELL_FN(vc_s_31, TN_PFX_NONE, "0xd07f0008")
TN_CELL_FN(vc_q_0, TN_PFX_NONE, "0xd0608088")
TN_CELL_FN(vc_q_1, TN_PFX_NONE, "0xd0618088")
TN_CELL_FN(vc_q_2, TN_PFX_NONE, "0xd0628088")
TN_CELL_FN(vc_q_3, TN_PFX_NONE, "0xd0638088")
TN_CELL_FN(vc_q_4, TN_PFX_NONE, "0xd0648088")
TN_CELL_FN(vc_q_5, TN_PFX_NONE, "0xd0658088")
TN_CELL_FN(vc_q_6, TN_PFX_NONE, "0xd0668088")
TN_CELL_FN(vc_q_7, TN_PFX_NONE, "0xd0678088")
TN_CELL_FN(vc_q_8, TN_PFX_NONE, "0xd0688088")
TN_CELL_FN(vc_q_9, TN_PFX_NONE, "0xd0698088")
TN_CELL_FN(vc_q_10, TN_PFX_NONE, "0xd06a8088")
TN_CELL_FN(vc_q_11, TN_PFX_NONE, "0xd06b8088")
TN_CELL_FN(vc_q_12, TN_PFX_NONE, "0xd06c8088")
TN_CELL_FN(vc_q_13, TN_PFX_NONE, "0xd06d8088")
TN_CELL_FN(vc_q_14, TN_PFX_NONE, "0xd06e8088")
TN_CELL_FN(vc_q_15, TN_PFX_NONE, "0xd06f8088")
TN_CELL_FN(vc_q_16, TN_PFX_NONE, "0xd0708088")
TN_CELL_FN(vc_q_17, TN_PFX_NONE, "0xd0718088")
TN_CELL_FN(vc_q_18, TN_PFX_NONE, "0xd0728088")
TN_CELL_FN(vc_q_19, TN_PFX_NONE, "0xd0738088")
TN_CELL_FN(vc_q_20, TN_PFX_NONE, "0xd0748088")
TN_CELL_FN(vc_q_21, TN_PFX_NONE, "0xd0758088")
TN_CELL_FN(vc_q_22, TN_PFX_NONE, "0xd0768088")
TN_CELL_FN(vc_q_23, TN_PFX_NONE, "0xd0778088")
TN_CELL_FN(vc_q_24, TN_PFX_NONE, "0xd0788088")
TN_CELL_FN(vc_q_25, TN_PFX_NONE, "0xd0798088")
TN_CELL_FN(vc_q_26, TN_PFX_NONE, "0xd07a8088")
TN_CELL_FN(vc_q_27, TN_PFX_NONE, "0xd07b8088")
TN_CELL_FN(vc_q_28, TN_PFX_NONE, "0xd07c8088")
TN_CELL_FN(vc_q_29, TN_PFX_NONE, "0xd07d8088")
TN_CELL_FN(vc_q_30, TN_PFX_NONE, "0xd07e8088")
TN_CELL_FN(vc_q_31, TN_PFX_NONE, "0xd07f8088")

static const vc_fn s_vc_s[VC_INDICES] = {
    vc_s_0, vc_s_1, vc_s_2, vc_s_3,
    vc_s_4, vc_s_5, vc_s_6, vc_s_7,
    vc_s_8, vc_s_9, vc_s_10, vc_s_11,
    vc_s_12, vc_s_13, vc_s_14, vc_s_15,
    vc_s_16, vc_s_17, vc_s_18, vc_s_19,
    vc_s_20, vc_s_21, vc_s_22, vc_s_23,
    vc_s_24, vc_s_25, vc_s_26, vc_s_27,
    vc_s_28, vc_s_29, vc_s_30, vc_s_31,
};
static const vc_fn s_vc_q[VC_INDICES] = {
    vc_q_0, vc_q_1, vc_q_2, vc_q_3,
    vc_q_4, vc_q_5, vc_q_6, vc_q_7,
    vc_q_8, vc_q_9, vc_q_10, vc_q_11,
    vc_q_12, vc_q_13, vc_q_14, vc_q_15,
    vc_q_16, vc_q_17, vc_q_18, vc_q_19,
    vc_q_20, vc_q_21, vc_q_22, vc_q_23,
    vc_q_24, vc_q_25, vc_q_26, vc_q_27,
    vc_q_28, vc_q_29, vc_q_30, vc_q_31,
};

static uint32_t s_vc_in[TN_IMAGE_WORDS] __attribute__((aligned(64)));
static uint32_t s_vc_out[TN_IMAGE_WORDS] __attribute__((aligned(64)));

/* One execution over the sentinel image: the first n destination words go to dst, the guard
   word (the one after them) is returned. */
static uint32_t vc_exec(vc_fn fn, uint32_t *dst, unsigned n)
{
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        s_vc_in[i] = TN_SENTINEL;
    }
    uint32_t cc[2] = {0u, 0u};
    fn(s_vc_in, s_vc_out, cc);
    for (unsigned i = 0; i < n; i++) {
        dst[i] = s_vc_out[VC_DST + i];
    }
    return s_vc_out[VC_DST + n];
}

/* Row <name>: nidx indices from first, n lanes each, then the guard word. */
static void vc_record(const char *name, const vc_fn *fns, uint32_t base_word, unsigned first,
                      unsigned nidx, unsigned n)
{
    uint32_t w[VC_MAX_WORDS];
    unsigned count = 0;
    uint32_t guard = 0;
    for (unsigned k = 0; k < nidx; k++) {
        guard = vc_exec(fns[first + k], &w[count], n);
        count += n;
    }
    w[count] = guard;
    hle_record(name, "PASS", base_word | (first << 16), w, count + 1u);
}

static void vc_core(unsigned pass)
{
    char name[64];
    snprintf(name, sizeof(name), "vcst-s-p%u", pass);
    hle_step(name);
    vc_record(name, s_vc_s, 0xd0600008u, 0u, VC_CORE, 1u);
    snprintf(name, sizeof(name), "vcst-q-p%u", pass);
    hle_step(name);
    vc_record(name, s_vc_q, 0xd0608088u, 0u, VC_CORE, 4u);
}

static void vc_reserved(unsigned pass)
{
    char name[64];
    snprintf(name, sizeof(name), "vcst-s-res-p%u", pass);
    hle_step(name);
    vc_record(name, s_vc_s, 0xd0600008u, VC_CORE, VC_INDICES - VC_CORE, 1u);
    snprintf(name, sizeof(name), "vcst-q-res-p%u", pass);
    hle_step(name);
    vc_record(name, s_vc_q, 0xd0608088u, VC_CORE, VC_INDICES - VC_CORE, 4u);
}

static int hle_run(void)
{
    /* Traps off before the first VFPU cell, as in the vfpu-compare case. */
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    for (unsigned pass = 0; pass < 2u; pass++) {
        vc_core(pass);
    }
    /* The reserved indices run last, in both passes. */
    for (unsigned pass = 0; pass < 2u; pass++) {
        vc_reserved(pass);
    }
    return 1;
}

#endif /* HLE_CASE_VFPU_CONSTANTS */

/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_VFPU_ROUND_SEP

/* #326 VFPU rounding separation (PSP-VFPU-ROUNDSEP-001), two passes, register-only: the raw
   result words of vdot.q (0x64818088), vmscl.q (0xf2018488), vmul.s (0x64010008), vadd.s
   (0x60010008) and vsub.s (0x60810008), over operands whose exact result lies between two
   float32 values, so the stored word shows whether the multi-term adder and the single product
   round to nearest even or truncate. Every cell is one instruction word run over the whole
   register image with no prefix word issued (the prefixes stay identity), emitted as a word;
   the words are the ones of the vfpu-wide-dot-matrix and vfpu-basic-arith cases. Image layout
   (word m*16 + c*4 + r): vector C000 (a, image 0-3), C010 (b, image 4-7), the matrix of vmscl M100
   (image 16-31), the destination C200/M200 (image 32..). vmscl multiplies the matrix by the scalar
   S010 (image 4); matrix elements 1-15 are the fixed finite table of the wide-dot case. The scalar
   forms use lane 0 of C000 and S010. Cells: vdot.q (a; b) = (0x3f800003, 0x3f800000, 0, 0; 1, 1,
   0, 0), (0x3f800003, 0x3f800003, 0x3f800000, 0; 1, 1, 1, 1), (0x3f800001, 0x3f800000, 0, 0; 1,
   1, 0, 0), the first with a negated, (0x3f800007, 0x3f800000, 0, 0; 1, 1, 0, 0) and two exact
   halves, (0x3f800005, 0x3f800000, 0, 0; 1, 1, 0, 0) above 2.0 and (0x40000001, 0x40000000, 0,
   0; 1, 1, 0, 0) above 4.0; vmscl.q (scalar, element 0) = (0x3fc00001, 0x3fc00001), (0x3fc00001,
   0xbfc00001), (0xbfc00001, 0x3fc00001), (0xbfc00001, 0xbfc00001), (0x3f800001, 0x3f800001),
   (0x3fc00003, 0x3fc00001); vmul.s (a0, S010) over (0x3fc00001, 0x3fc00001), (0x3fc00001,
   0xbfc00001), (0x3f800001, 0x3f800001), (0x3fc00003, 0x3fc00001); vadd.s over (0x3f800003,
   0x3f800000), (0x3f800001, 0x3f000000), (0xbf800003, 0xbf800000); vsub.s over (0x3f800003,
   0xbf800000).
   Row <form>-p<pass>[-<record>] (one record when the cells fit, else numbered): result = the
   instruction word; per cell in order, the recorded source image words (a then b for vdot.q;
   the scalar, then the matrix for vmscl.q; a0 then S010 for the scalar forms) followed by the
   destination words (1 for vdot.q and the scalar forms, 16 for vmscl.q). A durable step marker
   comes before every form group. Nothing here writes memory outside the probe's own image and
   output buffers, no control register is written, and no reserved word is run. */

#include "hle_vfpu_cells.h"

#define RS_MAX_WORDS 80u
#define RS_FORMS 5u
#define RS_DST 32u
#define RS_ONE 0x3f800000u

enum { RS_VDOT, RS_MSCL, RS_SCAL };

typedef void (*rs_fn)(const uint32_t *in, uint32_t *out, uint32_t *cc);

TN_CELL_FN(rs_vdot_q, TN_PFX_NONE, "0x64818088")
TN_CELL_FN(rs_vmscl_q, TN_PFX_NONE, "0xf2018488")
TN_CELL_FN(rs_vmul_s, TN_PFX_NONE, "0x64010008")
TN_CELL_FN(rs_vadd_s, TN_PFX_NONE, "0x60010008")
TN_CELL_FN(rs_vsub_s, TN_PFX_NONE, "0x60810008")

struct rs_form {
    const char *stem;
    uint32_t word;
    rs_fn fn;
    unsigned kind;
    const uint32_t *cells; /* nscen rows of stride operand words */
    unsigned stride;
    unsigned nscen;        /* cells */
    unsigned nout;         /* destination words recorded per cell */
    unsigned pos0, cnt0, pos1, cnt1; /* recorded source image words: two runs */
};

/* vdot.q: a0..a3, b0..b3 (image 0-7). */
static const uint32_t s_rs_vdot[7][8] = {
    {0x3f800003u, 0x3f800000u, 0u, 0u, RS_ONE, RS_ONE, 0u, 0u},
    {0x3f800003u, 0x3f800003u, 0x3f800000u, 0u, RS_ONE, RS_ONE, RS_ONE, RS_ONE},
    {0x3f800001u, 0x3f800000u, 0u, 0u, RS_ONE, RS_ONE, 0u, 0u},
    {0xbf800003u, 0xbf800000u, 0u, 0u, RS_ONE, RS_ONE, 0u, 0u},
    {0x3f800007u, 0x3f800000u, 0u, 0u, RS_ONE, RS_ONE, 0u, 0u},
    {0x3f800005u, 0x3f800000u, 0u, 0u, RS_ONE, RS_ONE, 0u, 0u},
    {0x40000001u, 0x40000000u, 0u, 0u, RS_ONE, RS_ONE, 0u, 0u},
};
/* vmscl.q: scalar (image 4), matrix element 0 (image 16). */
static const uint32_t s_rs_mscl[6][2] = {
    {0x3fc00001u, 0x3fc00001u}, {0x3fc00001u, 0xbfc00001u},
    {0xbfc00001u, 0x3fc00001u}, {0xbfc00001u, 0xbfc00001u},
    {0x3f800001u, 0x3f800001u}, {0x3fc00003u, 0x3fc00001u},
};
/* Scalar forms: lane 0 of C000 (image 0), S010 (image 4). */
static const uint32_t s_rs_vmul[4][2] = {
    {0x3fc00001u, 0x3fc00001u}, {0x3fc00001u, 0xbfc00001u},
    {0x3f800001u, 0x3f800001u}, {0x3fc00003u, 0x3fc00001u},
};
static const uint32_t s_rs_vadd[3][2] = {
    {0x3f800003u, 0x3f800000u}, {0x3f800001u, 0x3f000000u}, {0xbf800003u, 0xbf800000u},
};
static const uint32_t s_rs_vsub[1][2] = {
    {0x3f800003u, 0xbf800000u},
};

static const struct rs_form s_rs_forms[RS_FORMS] = {
    {"vdot-rs", 0x64818088u, rs_vdot_q, RS_VDOT, &s_rs_vdot[0][0], 8, 7, 1, 0, 4, 4, 4},
    {"vmscl-rs", 0xf2018488u, rs_vmscl_q, RS_MSCL, &s_rs_mscl[0][0], 2, 6, 16, 4, 1, 16, 16},
    {"vmul-rs", 0x64010008u, rs_vmul_s, RS_SCAL, &s_rs_vmul[0][0], 2, 4, 1, 0, 1, 4, 1},
    {"vadd-rs", 0x60010008u, rs_vadd_s, RS_SCAL, &s_rs_vadd[0][0], 2, 3, 1, 0, 1, 4, 1},
    {"vsub-rs", 0x60810008u, rs_vsub_s, RS_SCAL, &s_rs_vsub[0][0], 2, 1, 1, 0, 1, 4, 1},
};

/* 1, 2, -3, 0.5, -1.5, 4, 0.25, -2, 3.5, -0.75, 1.25, 6, -4.5, 0.125, 2.75, -1. */
static const uint32_t s_rs_mat[16] = {
    0x3f800000u, 0x40000000u, 0xc0400000u, 0x3f000000u, 0xbfc00000u, 0x40800000u,
    0x3e800000u, 0xc0000000u, 0x40600000u, 0xbf400000u, 0x3fa00000u, 0x40c00000u,
    0xc0900000u, 0x3e000000u, 0x40300000u, 0xbf800000u,
};

static uint32_t s_rs_in[TN_IMAGE_WORDS] __attribute__((aligned(64)));
static uint32_t s_rs_out[TN_IMAGE_WORDS] __attribute__((aligned(64)));

/* Overlays the cell's operands on the background image s_rs_in (word i = 0x3f800000 | i << 16). */
static void rs_build(const struct rs_form *f, unsigned idx)
{
    const uint32_t *v = &f->cells[idx * f->stride];
    uint32_t *img = s_rs_in;
    switch (f->kind) {
    case RS_VDOT:
        for (unsigned i = 0; i < 8u; i++) {
            img[i] = v[i];
        }
        break;
    case RS_MSCL:
        img[4] = v[0];
        for (unsigned i = 0; i < 16u; i++) {
            img[16u + i] = s_rs_mat[i];
        }
        img[16] = v[1];
        break;
    default:
        img[0] = v[0];
        img[4] = v[1];
        break;
    }
}

/* One cell: background image, operand overlay, the word; dst gets the recorded source words and
   then the destination words (image 32..), and the number of words is returned. */
static unsigned rs_exec(const struct rs_form *f, unsigned idx, uint32_t *dst)
{
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        s_rs_in[i] = 0x3f800000u | (i << 16);
    }
    rs_build(f, idx);
    const unsigned c0 = f->cnt0;
    const unsigned c1 = f->cnt1;
    const unsigned nout = f->nout;
    unsigned count = 0;
    for (unsigned i = 0; i < c0; i++) {
        dst[count++] = s_rs_in[f->pos0 + i];
    }
    for (unsigned i = 0; i < c1; i++) {
        dst[count++] = s_rs_in[f->pos1 + i];
    }
    uint32_t cc[2] = {0u, 0u};
    f->fn(s_rs_in, s_rs_out, cc);
    for (unsigned i = 0; i < nout; i++) {
        dst[count++] = s_rs_out[RS_DST + i];
    }
    return count;
}

/* One durable step per form group, then its records: as many cells as fit in RS_MAX_WORDS. */
static void rs_group(unsigned pass, const struct rs_form *f)
{
    char name[64];
    uint32_t w[RS_MAX_WORDS];
    const unsigned per = RS_MAX_WORDS / (f->cnt0 + f->cnt1 + f->nout);
    const unsigned nrec = (f->nscen + per - 1u) / per;
    snprintf(name, sizeof(name), "%s-p%u", f->stem, pass);
    hle_step(name);
    for (unsigned rec = 0; rec < nrec; rec++) {
        const unsigned first = rec * per;
        unsigned last = first + per;
        if (last > f->nscen) {
            last = f->nscen;
        }
        unsigned count = 0;
        for (unsigned s = first; s < last; s++) {
            count += rs_exec(f, s, &w[count]);
        }
        if (nrec > 1u) {
            snprintf(name, sizeof(name), "%s-p%u-%u", f->stem, pass, rec);
        }
        hle_record(name, "PASS", f->word, w, count);
    }
}

static int hle_run(void)
{
    /* Traps off before the first VFPU cell, as in the vfpu-compare case. */
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    for (unsigned pass = 0; pass < 2u; pass++) {
        for (unsigned form = 0; form < RS_FORMS; form++) {
            rs_group(pass, &s_rs_forms[form]);
        }
    }
    return 1;
}

#endif /* HLE_CASE_VFPU_ROUND_SEP */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_VFPU_PREFIX_STATE

/* #326 VFPU prefix state (PSP-VFPU-PREFIX-001), two passes, register-only: what a non-identity
   vpfxs, vpfxt or vpfxd does to the measured arithmetic forms, how long a pending prefix lives,
   and the raw result words of vfim.s and viim.s with and without a pending vpfxd. Every word was
   assembled with psp-as from the mnemonic named here and none was refused; each is emitted as a
   word so no assembler decides it at build time. Each cell loads the whole register image (32
   lv.q), issues the identity prefixes vpfxs x,y,z,w (0xdc0000e4), vpfxt x,y,z,w (0xdd0000e4) and
   vpfxd ,,, (0xde000000), reads VFPU_PFXS, VFPU_PFXT and VFPU_PFXD with mfvc $t1 (controls 0-2,
   imm 128-130: 0x48690080-0x48690082, the encoding hle_vfpu_cells.h uses for VFPU_CC, control 3),
   runs its words, reads the three control words again, stores the image (32 sv.q) and issues the
   identity prefixes once more, so no prefix carries into the next cell. Background image word i =
   0x3f800000 | i << 16; the destination C200 (image 32-35) starts as the sentinel 0x7a5a5a5a.
   a. Prefixed forms: vadd.q, vmul.q, vdot.q (S200), vmin.q, vmov.q under the vpfxs words y,x,w,z
      (0xdc0000b1), |x|,|y|,|z|,|w| (0xdc000fe4), -x,-y,-z,-w (0xdc0f00e4), the constants 0,1,2,1/2
      (0xdc00f0e4) and 3,1/3,1/4,1/6 (0xdc00ffe4), and -|x|,-0,-1,-1/2 (0xdc0fe1d0); the same five
      forms under vpfxt y,x,w,z (0xdd0000b1), |x|,|y|,|z|,|w| (0xdd000fe4), -x,-y,-z,-w
      (0xdd0f00e4), 0,1,2,1/2 (0xdd00f0e4) and 3,1/3,1/4,1/6 (0xdd00ffe4); vadd.q, vmov.q, vneg.q,
      vzero.q and vidt.q under vpfxd ,m,,m (0xde000a00), [0:1] on every lane (0xde000055), [-1:1]
      on every lane (0xde0000ff) and [0:1],m,[-1:1], (0xde000231). Each runs over three operand
      sets (A = C000, B = C010): a finite set, (-qNaN, +inf, +denormal, -0) against (1, -inf,
      -denormal, +0), and (+0, +sNaN, -largest denormal, -1) against (-0, 2, +inf, +qNaN), so the
      order of the prefix and the flush/NaN rules is visible (abs of a negative NaN, negate of +0,
      saturate of NaN, inf and a denormal).
   b. Prefix lifetime: S000 = -2. vpfxs -x,y,z,w (0xdc0100e4), then nothing, vnop (0xffff0000),
      vsync (0xffff0320), vflush (0xffff040d), a taken branch b 1f with a nop delay slot, add.s
      $f0, $f0, $f0 (0x46000000), mtv $t1, S733 (0x48e9007f), mfv $t1, S733 (0x4869007f), lv.q C730,
      vpfxt -x,-y,-z,-w (0xdd0f00e4) or the three mfvc reads, then vmov.s S200, S000 (0xd0000008)
      and vmov.s S201, S000 (0xd0000028): the first shows whether the prefix survived the
      instruction between, the second whether one VFPU instruction consumed it. The same for vpfxd
      [0:1],,, (0xde000001) before nothing, vnop, vsync and vflush.
   c. vfim.s S200 with the half images 0, 1.0, -2.0, +inf, NaN (0x7c01), -0, the smallest normal
      half (0x0400) and 65504 (0xdf880000 | half), and viim.s S200 with 0, 1, -1, 32767 and -32768
      (0x8000; 0xdf080000 | imm), each with no vpfxd, vpfxd [0:1],,, (0xde000001), [-1:1],,,
      (0xde000003) and m,,, (0xde000100) pending. The assembler rounds a subnormal half literal to
      +0, so no denormal half image is run.
   Rows per pass p: operands-p<p> (result 0; the three sets, A then B, 24 words);
   <pfxs|pfxt|pfxd>-<form>-<k>-p<p> (result = the prefix word; out0 = the instruction word, then
   C200 lanes 0-3 for each set, 13 words); life-<cell>-p<p> (result = the prefix word; PFXS, PFXT,
   PFXD before, the three mfvc words read between (the sentinel except in the mfvc cell), the
   three after, S200, S201: 11 words); vfim-<d>-p<p> and viim-<d>-p<p> (result = the vpfxd word,
   0 for none; per immediate the instruction word and S200, 16 and 10 words). A durable step
   marker comes before every record. Nothing here writes memory outside the probe's own image,
   output and control buffers, no control register is written, and no reserved word is run. */

#include "hle_vfpu_cells.h"

#define PS_DST 32u
#define PS_SETS 3u
#define PS_ARITH 75u
#define PS_LIFE 15u
#define PS_VFIM 8u
#define PS_VIIM 5u
#define PS_IMMD 4u
#define PS_CTL 9u
#define PS_MAX_WORDS 24u

/* The identity prefixes vpfxs x,y,z,w, vpfxt x,y,z,w and vpfxd ,,,. */
#define PS_RESET ".word 0xdc0000e4\n\t.word 0xdd0000e4\n\t.word 0xde000000\n\t"
/* mfvc $t1 of VFPU_PFXS, VFPU_PFXT and VFPU_PFXD, stored at three byte offsets of ctl. */
#define PS_CTL_READ(o0, o1, o2)                                                                  \
    ".word 0x48690080\n\tsw $t1, " o0 "(%[ctl])\n\t.word 0x48690081\n\tsw $t1, " o1 "(%[ctl])\n\t" \
    ".word 0x48690082\n\tsw $t1, " o2 "(%[ctl])\n\t"

typedef void (*ps_fn)(const uint32_t *in, uint32_t *out, uint32_t *ctl);

#define PS_CELL_FN(fname, body)                                                    \
    static void fname(const uint32_t *in, uint32_t *out, uint32_t *ctl)            \
    {                                                                              \
        __asm__ volatile(                                                          \
            TN_LOAD_ALL                                                            \
            PS_RESET                                                               \
            PS_CTL_READ("0", "4", "8")                                             \
            body                                                                   \
            PS_CTL_READ("24", "28", "32")                                          \
            TN_STORE_ALL                                                           \
            PS_RESET                                                               \
            :                                                                      \
            : [in] "r"(in), [out] "r"(out), [ctl] "r"(ctl)                         \
            : "t1", "$f0", "memory");                                              \
    }

struct ps_arith {
    const char *name;
    uint32_t pfx;
    uint32_t word;
    ps_fn fn;
};

struct ps_life {
    const char *name;
    uint32_t pfx;
    ps_fn fn;
};

PS_CELL_FN(ps_a0, ".word 0xdc0000b1\n\t.word 0x60018088\n\t")
PS_CELL_FN(ps_a1, ".word 0xdc000fe4\n\t.word 0x60018088\n\t")
PS_CELL_FN(ps_a2, ".word 0xdc0f00e4\n\t.word 0x60018088\n\t")
PS_CELL_FN(ps_a3, ".word 0xdc00f0e4\n\t.word 0x60018088\n\t")
PS_CELL_FN(ps_a4, ".word 0xdc00ffe4\n\t.word 0x60018088\n\t")
PS_CELL_FN(ps_a5, ".word 0xdc0fe1d0\n\t.word 0x60018088\n\t")
PS_CELL_FN(ps_a6, ".word 0xdc0000b1\n\t.word 0x64018088\n\t")
PS_CELL_FN(ps_a7, ".word 0xdc000fe4\n\t.word 0x64018088\n\t")
PS_CELL_FN(ps_a8, ".word 0xdc0f00e4\n\t.word 0x64018088\n\t")
PS_CELL_FN(ps_a9, ".word 0xdc00f0e4\n\t.word 0x64018088\n\t")
PS_CELL_FN(ps_a10, ".word 0xdc00ffe4\n\t.word 0x64018088\n\t")
PS_CELL_FN(ps_a11, ".word 0xdc0fe1d0\n\t.word 0x64018088\n\t")
PS_CELL_FN(ps_a12, ".word 0xdc0000b1\n\t.word 0x64818088\n\t")
PS_CELL_FN(ps_a13, ".word 0xdc000fe4\n\t.word 0x64818088\n\t")
PS_CELL_FN(ps_a14, ".word 0xdc0f00e4\n\t.word 0x64818088\n\t")
PS_CELL_FN(ps_a15, ".word 0xdc00f0e4\n\t.word 0x64818088\n\t")
PS_CELL_FN(ps_a16, ".word 0xdc00ffe4\n\t.word 0x64818088\n\t")
PS_CELL_FN(ps_a17, ".word 0xdc0fe1d0\n\t.word 0x64818088\n\t")
PS_CELL_FN(ps_a18, ".word 0xdc0000b1\n\t.word 0x6d018088\n\t")
PS_CELL_FN(ps_a19, ".word 0xdc000fe4\n\t.word 0x6d018088\n\t")
PS_CELL_FN(ps_a20, ".word 0xdc0f00e4\n\t.word 0x6d018088\n\t")
PS_CELL_FN(ps_a21, ".word 0xdc00f0e4\n\t.word 0x6d018088\n\t")
PS_CELL_FN(ps_a22, ".word 0xdc00ffe4\n\t.word 0x6d018088\n\t")
PS_CELL_FN(ps_a23, ".word 0xdc0fe1d0\n\t.word 0x6d018088\n\t")
PS_CELL_FN(ps_a24, ".word 0xdc0000b1\n\t.word 0xd0008088\n\t")
PS_CELL_FN(ps_a25, ".word 0xdc000fe4\n\t.word 0xd0008088\n\t")
PS_CELL_FN(ps_a26, ".word 0xdc0f00e4\n\t.word 0xd0008088\n\t")
PS_CELL_FN(ps_a27, ".word 0xdc00f0e4\n\t.word 0xd0008088\n\t")
PS_CELL_FN(ps_a28, ".word 0xdc00ffe4\n\t.word 0xd0008088\n\t")
PS_CELL_FN(ps_a29, ".word 0xdc0fe1d0\n\t.word 0xd0008088\n\t")
PS_CELL_FN(ps_a30, ".word 0xdd0000b1\n\t.word 0x60018088\n\t")
PS_CELL_FN(ps_a31, ".word 0xdd000fe4\n\t.word 0x60018088\n\t")
PS_CELL_FN(ps_a32, ".word 0xdd0f00e4\n\t.word 0x60018088\n\t")
PS_CELL_FN(ps_a33, ".word 0xdd00f0e4\n\t.word 0x60018088\n\t")
PS_CELL_FN(ps_a34, ".word 0xdd00ffe4\n\t.word 0x60018088\n\t")
PS_CELL_FN(ps_a35, ".word 0xdd0000b1\n\t.word 0x64018088\n\t")
PS_CELL_FN(ps_a36, ".word 0xdd000fe4\n\t.word 0x64018088\n\t")
PS_CELL_FN(ps_a37, ".word 0xdd0f00e4\n\t.word 0x64018088\n\t")
PS_CELL_FN(ps_a38, ".word 0xdd00f0e4\n\t.word 0x64018088\n\t")
PS_CELL_FN(ps_a39, ".word 0xdd00ffe4\n\t.word 0x64018088\n\t")
PS_CELL_FN(ps_a40, ".word 0xdd0000b1\n\t.word 0x64818088\n\t")
PS_CELL_FN(ps_a41, ".word 0xdd000fe4\n\t.word 0x64818088\n\t")
PS_CELL_FN(ps_a42, ".word 0xdd0f00e4\n\t.word 0x64818088\n\t")
PS_CELL_FN(ps_a43, ".word 0xdd00f0e4\n\t.word 0x64818088\n\t")
PS_CELL_FN(ps_a44, ".word 0xdd00ffe4\n\t.word 0x64818088\n\t")
PS_CELL_FN(ps_a45, ".word 0xdd0000b1\n\t.word 0x6d018088\n\t")
PS_CELL_FN(ps_a46, ".word 0xdd000fe4\n\t.word 0x6d018088\n\t")
PS_CELL_FN(ps_a47, ".word 0xdd0f00e4\n\t.word 0x6d018088\n\t")
PS_CELL_FN(ps_a48, ".word 0xdd00f0e4\n\t.word 0x6d018088\n\t")
PS_CELL_FN(ps_a49, ".word 0xdd00ffe4\n\t.word 0x6d018088\n\t")
PS_CELL_FN(ps_a50, ".word 0xdd0000b1\n\t.word 0xd0008088\n\t")
PS_CELL_FN(ps_a51, ".word 0xdd000fe4\n\t.word 0xd0008088\n\t")
PS_CELL_FN(ps_a52, ".word 0xdd0f00e4\n\t.word 0xd0008088\n\t")
PS_CELL_FN(ps_a53, ".word 0xdd00f0e4\n\t.word 0xd0008088\n\t")
PS_CELL_FN(ps_a54, ".word 0xdd00ffe4\n\t.word 0xd0008088\n\t")
PS_CELL_FN(ps_a55, ".word 0xde000a00\n\t.word 0x60018088\n\t")
PS_CELL_FN(ps_a56, ".word 0xde000055\n\t.word 0x60018088\n\t")
PS_CELL_FN(ps_a57, ".word 0xde0000ff\n\t.word 0x60018088\n\t")
PS_CELL_FN(ps_a58, ".word 0xde000231\n\t.word 0x60018088\n\t")
PS_CELL_FN(ps_a59, ".word 0xde000a00\n\t.word 0xd0008088\n\t")
PS_CELL_FN(ps_a60, ".word 0xde000055\n\t.word 0xd0008088\n\t")
PS_CELL_FN(ps_a61, ".word 0xde0000ff\n\t.word 0xd0008088\n\t")
PS_CELL_FN(ps_a62, ".word 0xde000231\n\t.word 0xd0008088\n\t")
PS_CELL_FN(ps_a63, ".word 0xde000a00\n\t.word 0xd0028088\n\t")
PS_CELL_FN(ps_a64, ".word 0xde000055\n\t.word 0xd0028088\n\t")
PS_CELL_FN(ps_a65, ".word 0xde0000ff\n\t.word 0xd0028088\n\t")
PS_CELL_FN(ps_a66, ".word 0xde000231\n\t.word 0xd0028088\n\t")
PS_CELL_FN(ps_a67, ".word 0xde000a00\n\t.word 0xd0068088\n\t")
PS_CELL_FN(ps_a68, ".word 0xde000055\n\t.word 0xd0068088\n\t")
PS_CELL_FN(ps_a69, ".word 0xde0000ff\n\t.word 0xd0068088\n\t")
PS_CELL_FN(ps_a70, ".word 0xde000231\n\t.word 0xd0068088\n\t")
PS_CELL_FN(ps_a71, ".word 0xde000a00\n\t.word 0xd0038088\n\t")
PS_CELL_FN(ps_a72, ".word 0xde000055\n\t.word 0xd0038088\n\t")
PS_CELL_FN(ps_a73, ".word 0xde0000ff\n\t.word 0xd0038088\n\t")
PS_CELL_FN(ps_a74, ".word 0xde000231\n\t.word 0xd0038088\n\t")
PS_CELL_FN(ps_l0, ".word 0xdc0100e4\n\t" ".word 0xd0000008\n\t.word 0xd0000028\n\t")
PS_CELL_FN(ps_l1, ".word 0xdc0100e4\n\t" ".word 0xffff0000\n\t" ".word 0xd0000008\n\t.word 0xd0000028\n\t")
PS_CELL_FN(ps_l2, ".word 0xdc0100e4\n\t" ".word 0xffff0320\n\t" ".word 0xd0000008\n\t.word 0xd0000028\n\t")
PS_CELL_FN(ps_l3, ".word 0xdc0100e4\n\t" ".word 0xffff040d\n\t" ".word 0xd0000008\n\t.word 0xd0000028\n\t")
PS_CELL_FN(ps_l4, ".word 0xdc0100e4\n\t" ".set push\n\t.set noreorder\n\tb 1f\n\tnop\n1:\n\t.set pop\n\t" ".word 0xd0000008\n\t.word 0xd0000028\n\t")
PS_CELL_FN(ps_l5, ".word 0xdc0100e4\n\t" ".word 0x46000000\n\t" ".word 0xd0000008\n\t.word 0xd0000028\n\t")
PS_CELL_FN(ps_l6, ".word 0xdc0100e4\n\t" ".word 0x48e9007f\n\t" ".word 0xd0000008\n\t.word 0xd0000028\n\t")
PS_CELL_FN(ps_l7, ".word 0xdc0100e4\n\t" ".word 0x4869007f\n\t" ".word 0xd0000008\n\t.word 0xd0000028\n\t")
PS_CELL_FN(ps_l8, ".word 0xdc0100e4\n\t" "lv.q C730, 496(%[in])\n\t" ".word 0xd0000008\n\t.word 0xd0000028\n\t")
PS_CELL_FN(ps_l9, ".word 0xdc0100e4\n\t" ".word 0xdd0f00e4\n\t" ".word 0xd0000008\n\t.word 0xd0000028\n\t")
PS_CELL_FN(ps_l10, ".word 0xdc0100e4\n\t" PS_CTL_READ("12", "16", "20") ".word 0xd0000008\n\t.word 0xd0000028\n\t")
PS_CELL_FN(ps_l11, ".word 0xde000001\n\t" ".word 0xd0000008\n\t.word 0xd0000028\n\t")
PS_CELL_FN(ps_l12, ".word 0xde000001\n\t" ".word 0xffff0000\n\t" ".word 0xd0000008\n\t.word 0xd0000028\n\t")
PS_CELL_FN(ps_l13, ".word 0xde000001\n\t" ".word 0xffff0320\n\t" ".word 0xd0000008\n\t.word 0xd0000028\n\t")
PS_CELL_FN(ps_l14, ".word 0xde000001\n\t" ".word 0xffff040d\n\t" ".word 0xd0000008\n\t.word 0xd0000028\n\t")
PS_CELL_FN(ps_f0_0, ".word 0xdf880000\n\t")
PS_CELL_FN(ps_f0_1, ".word 0xdf883c00\n\t")
PS_CELL_FN(ps_f0_2, ".word 0xdf88c000\n\t")
PS_CELL_FN(ps_f0_3, ".word 0xdf887c00\n\t")
PS_CELL_FN(ps_f0_4, ".word 0xdf887c01\n\t")
PS_CELL_FN(ps_f0_5, ".word 0xdf888000\n\t")
PS_CELL_FN(ps_f0_6, ".word 0xdf880400\n\t")
PS_CELL_FN(ps_f0_7, ".word 0xdf887bff\n\t")
PS_CELL_FN(ps_i0_0, ".word 0xdf080000\n\t")
PS_CELL_FN(ps_i0_1, ".word 0xdf080001\n\t")
PS_CELL_FN(ps_i0_2, ".word 0xdf08ffff\n\t")
PS_CELL_FN(ps_i0_3, ".word 0xdf087fff\n\t")
PS_CELL_FN(ps_i0_4, ".word 0xdf088000\n\t")
PS_CELL_FN(ps_f1_0, ".word 0xde000001\n\t.word 0xdf880000\n\t")
PS_CELL_FN(ps_f1_1, ".word 0xde000001\n\t.word 0xdf883c00\n\t")
PS_CELL_FN(ps_f1_2, ".word 0xde000001\n\t.word 0xdf88c000\n\t")
PS_CELL_FN(ps_f1_3, ".word 0xde000001\n\t.word 0xdf887c00\n\t")
PS_CELL_FN(ps_f1_4, ".word 0xde000001\n\t.word 0xdf887c01\n\t")
PS_CELL_FN(ps_f1_5, ".word 0xde000001\n\t.word 0xdf888000\n\t")
PS_CELL_FN(ps_f1_6, ".word 0xde000001\n\t.word 0xdf880400\n\t")
PS_CELL_FN(ps_f1_7, ".word 0xde000001\n\t.word 0xdf887bff\n\t")
PS_CELL_FN(ps_i1_0, ".word 0xde000001\n\t.word 0xdf080000\n\t")
PS_CELL_FN(ps_i1_1, ".word 0xde000001\n\t.word 0xdf080001\n\t")
PS_CELL_FN(ps_i1_2, ".word 0xde000001\n\t.word 0xdf08ffff\n\t")
PS_CELL_FN(ps_i1_3, ".word 0xde000001\n\t.word 0xdf087fff\n\t")
PS_CELL_FN(ps_i1_4, ".word 0xde000001\n\t.word 0xdf088000\n\t")
PS_CELL_FN(ps_f2_0, ".word 0xde000003\n\t.word 0xdf880000\n\t")
PS_CELL_FN(ps_f2_1, ".word 0xde000003\n\t.word 0xdf883c00\n\t")
PS_CELL_FN(ps_f2_2, ".word 0xde000003\n\t.word 0xdf88c000\n\t")
PS_CELL_FN(ps_f2_3, ".word 0xde000003\n\t.word 0xdf887c00\n\t")
PS_CELL_FN(ps_f2_4, ".word 0xde000003\n\t.word 0xdf887c01\n\t")
PS_CELL_FN(ps_f2_5, ".word 0xde000003\n\t.word 0xdf888000\n\t")
PS_CELL_FN(ps_f2_6, ".word 0xde000003\n\t.word 0xdf880400\n\t")
PS_CELL_FN(ps_f2_7, ".word 0xde000003\n\t.word 0xdf887bff\n\t")
PS_CELL_FN(ps_i2_0, ".word 0xde000003\n\t.word 0xdf080000\n\t")
PS_CELL_FN(ps_i2_1, ".word 0xde000003\n\t.word 0xdf080001\n\t")
PS_CELL_FN(ps_i2_2, ".word 0xde000003\n\t.word 0xdf08ffff\n\t")
PS_CELL_FN(ps_i2_3, ".word 0xde000003\n\t.word 0xdf087fff\n\t")
PS_CELL_FN(ps_i2_4, ".word 0xde000003\n\t.word 0xdf088000\n\t")
PS_CELL_FN(ps_f3_0, ".word 0xde000100\n\t.word 0xdf880000\n\t")
PS_CELL_FN(ps_f3_1, ".word 0xde000100\n\t.word 0xdf883c00\n\t")
PS_CELL_FN(ps_f3_2, ".word 0xde000100\n\t.word 0xdf88c000\n\t")
PS_CELL_FN(ps_f3_3, ".word 0xde000100\n\t.word 0xdf887c00\n\t")
PS_CELL_FN(ps_f3_4, ".word 0xde000100\n\t.word 0xdf887c01\n\t")
PS_CELL_FN(ps_f3_5, ".word 0xde000100\n\t.word 0xdf888000\n\t")
PS_CELL_FN(ps_f3_6, ".word 0xde000100\n\t.word 0xdf880400\n\t")
PS_CELL_FN(ps_f3_7, ".word 0xde000100\n\t.word 0xdf887bff\n\t")
PS_CELL_FN(ps_i3_0, ".word 0xde000100\n\t.word 0xdf080000\n\t")
PS_CELL_FN(ps_i3_1, ".word 0xde000100\n\t.word 0xdf080001\n\t")
PS_CELL_FN(ps_i3_2, ".word 0xde000100\n\t.word 0xdf08ffff\n\t")
PS_CELL_FN(ps_i3_3, ".word 0xde000100\n\t.word 0xdf087fff\n\t")
PS_CELL_FN(ps_i3_4, ".word 0xde000100\n\t.word 0xdf088000\n\t")

static const struct ps_arith s_ps_arith[PS_ARITH] = {
    {"pfxs-vadd-q-0", 0xdc0000b1u, 0x60018088u, ps_a0},
    {"pfxs-vadd-q-1", 0xdc000fe4u, 0x60018088u, ps_a1},
    {"pfxs-vadd-q-2", 0xdc0f00e4u, 0x60018088u, ps_a2},
    {"pfxs-vadd-q-3", 0xdc00f0e4u, 0x60018088u, ps_a3},
    {"pfxs-vadd-q-4", 0xdc00ffe4u, 0x60018088u, ps_a4},
    {"pfxs-vadd-q-5", 0xdc0fe1d0u, 0x60018088u, ps_a5},
    {"pfxs-vmul-q-0", 0xdc0000b1u, 0x64018088u, ps_a6},
    {"pfxs-vmul-q-1", 0xdc000fe4u, 0x64018088u, ps_a7},
    {"pfxs-vmul-q-2", 0xdc0f00e4u, 0x64018088u, ps_a8},
    {"pfxs-vmul-q-3", 0xdc00f0e4u, 0x64018088u, ps_a9},
    {"pfxs-vmul-q-4", 0xdc00ffe4u, 0x64018088u, ps_a10},
    {"pfxs-vmul-q-5", 0xdc0fe1d0u, 0x64018088u, ps_a11},
    {"pfxs-vdot-q-0", 0xdc0000b1u, 0x64818088u, ps_a12},
    {"pfxs-vdot-q-1", 0xdc000fe4u, 0x64818088u, ps_a13},
    {"pfxs-vdot-q-2", 0xdc0f00e4u, 0x64818088u, ps_a14},
    {"pfxs-vdot-q-3", 0xdc00f0e4u, 0x64818088u, ps_a15},
    {"pfxs-vdot-q-4", 0xdc00ffe4u, 0x64818088u, ps_a16},
    {"pfxs-vdot-q-5", 0xdc0fe1d0u, 0x64818088u, ps_a17},
    {"pfxs-vmin-q-0", 0xdc0000b1u, 0x6d018088u, ps_a18},
    {"pfxs-vmin-q-1", 0xdc000fe4u, 0x6d018088u, ps_a19},
    {"pfxs-vmin-q-2", 0xdc0f00e4u, 0x6d018088u, ps_a20},
    {"pfxs-vmin-q-3", 0xdc00f0e4u, 0x6d018088u, ps_a21},
    {"pfxs-vmin-q-4", 0xdc00ffe4u, 0x6d018088u, ps_a22},
    {"pfxs-vmin-q-5", 0xdc0fe1d0u, 0x6d018088u, ps_a23},
    {"pfxs-vmov-q-0", 0xdc0000b1u, 0xd0008088u, ps_a24},
    {"pfxs-vmov-q-1", 0xdc000fe4u, 0xd0008088u, ps_a25},
    {"pfxs-vmov-q-2", 0xdc0f00e4u, 0xd0008088u, ps_a26},
    {"pfxs-vmov-q-3", 0xdc00f0e4u, 0xd0008088u, ps_a27},
    {"pfxs-vmov-q-4", 0xdc00ffe4u, 0xd0008088u, ps_a28},
    {"pfxs-vmov-q-5", 0xdc0fe1d0u, 0xd0008088u, ps_a29},
    {"pfxt-vadd-q-0", 0xdd0000b1u, 0x60018088u, ps_a30},
    {"pfxt-vadd-q-1", 0xdd000fe4u, 0x60018088u, ps_a31},
    {"pfxt-vadd-q-2", 0xdd0f00e4u, 0x60018088u, ps_a32},
    {"pfxt-vadd-q-3", 0xdd00f0e4u, 0x60018088u, ps_a33},
    {"pfxt-vadd-q-4", 0xdd00ffe4u, 0x60018088u, ps_a34},
    {"pfxt-vmul-q-0", 0xdd0000b1u, 0x64018088u, ps_a35},
    {"pfxt-vmul-q-1", 0xdd000fe4u, 0x64018088u, ps_a36},
    {"pfxt-vmul-q-2", 0xdd0f00e4u, 0x64018088u, ps_a37},
    {"pfxt-vmul-q-3", 0xdd00f0e4u, 0x64018088u, ps_a38},
    {"pfxt-vmul-q-4", 0xdd00ffe4u, 0x64018088u, ps_a39},
    {"pfxt-vdot-q-0", 0xdd0000b1u, 0x64818088u, ps_a40},
    {"pfxt-vdot-q-1", 0xdd000fe4u, 0x64818088u, ps_a41},
    {"pfxt-vdot-q-2", 0xdd0f00e4u, 0x64818088u, ps_a42},
    {"pfxt-vdot-q-3", 0xdd00f0e4u, 0x64818088u, ps_a43},
    {"pfxt-vdot-q-4", 0xdd00ffe4u, 0x64818088u, ps_a44},
    {"pfxt-vmin-q-0", 0xdd0000b1u, 0x6d018088u, ps_a45},
    {"pfxt-vmin-q-1", 0xdd000fe4u, 0x6d018088u, ps_a46},
    {"pfxt-vmin-q-2", 0xdd0f00e4u, 0x6d018088u, ps_a47},
    {"pfxt-vmin-q-3", 0xdd00f0e4u, 0x6d018088u, ps_a48},
    {"pfxt-vmin-q-4", 0xdd00ffe4u, 0x6d018088u, ps_a49},
    {"pfxt-vmov-q-0", 0xdd0000b1u, 0xd0008088u, ps_a50},
    {"pfxt-vmov-q-1", 0xdd000fe4u, 0xd0008088u, ps_a51},
    {"pfxt-vmov-q-2", 0xdd0f00e4u, 0xd0008088u, ps_a52},
    {"pfxt-vmov-q-3", 0xdd00f0e4u, 0xd0008088u, ps_a53},
    {"pfxt-vmov-q-4", 0xdd00ffe4u, 0xd0008088u, ps_a54},
    {"pfxd-vadd-q-0", 0xde000a00u, 0x60018088u, ps_a55},
    {"pfxd-vadd-q-1", 0xde000055u, 0x60018088u, ps_a56},
    {"pfxd-vadd-q-2", 0xde0000ffu, 0x60018088u, ps_a57},
    {"pfxd-vadd-q-3", 0xde000231u, 0x60018088u, ps_a58},
    {"pfxd-vmov-q-0", 0xde000a00u, 0xd0008088u, ps_a59},
    {"pfxd-vmov-q-1", 0xde000055u, 0xd0008088u, ps_a60},
    {"pfxd-vmov-q-2", 0xde0000ffu, 0xd0008088u, ps_a61},
    {"pfxd-vmov-q-3", 0xde000231u, 0xd0008088u, ps_a62},
    {"pfxd-vneg-q-0", 0xde000a00u, 0xd0028088u, ps_a63},
    {"pfxd-vneg-q-1", 0xde000055u, 0xd0028088u, ps_a64},
    {"pfxd-vneg-q-2", 0xde0000ffu, 0xd0028088u, ps_a65},
    {"pfxd-vneg-q-3", 0xde000231u, 0xd0028088u, ps_a66},
    {"pfxd-vzero-q-0", 0xde000a00u, 0xd0068088u, ps_a67},
    {"pfxd-vzero-q-1", 0xde000055u, 0xd0068088u, ps_a68},
    {"pfxd-vzero-q-2", 0xde0000ffu, 0xd0068088u, ps_a69},
    {"pfxd-vzero-q-3", 0xde000231u, 0xd0068088u, ps_a70},
    {"pfxd-vidt-q-0", 0xde000a00u, 0xd0038088u, ps_a71},
    {"pfxd-vidt-q-1", 0xde000055u, 0xd0038088u, ps_a72},
    {"pfxd-vidt-q-2", 0xde0000ffu, 0xd0038088u, ps_a73},
    {"pfxd-vidt-q-3", 0xde000231u, 0xd0038088u, ps_a74},
};

static const struct ps_life s_ps_life[PS_LIFE] = {
    {"s-direct", 0xdc0100e4u, ps_l0},
    {"s-vnop", 0xdc0100e4u, ps_l1},
    {"s-vsync", 0xdc0100e4u, ps_l2},
    {"s-vflush", 0xdc0100e4u, ps_l3},
    {"s-branch", 0xdc0100e4u, ps_l4},
    {"s-adds", 0xdc0100e4u, ps_l5},
    {"s-mtv", 0xdc0100e4u, ps_l6},
    {"s-mfv", 0xdc0100e4u, ps_l7},
    {"s-lvq", 0xdc0100e4u, ps_l8},
    {"s-vpfxt", 0xdc0100e4u, ps_l9},
    {"s-mfvc", 0xdc0100e4u, ps_l10},
    {"d-direct", 0xde000001u, ps_l11},
    {"d-vnop", 0xde000001u, ps_l12},
    {"d-vsync", 0xde000001u, ps_l13},
    {"d-vflush", 0xde000001u, ps_l14},
};

static const uint32_t s_ps_vfim_words[PS_VFIM] = {
    0xdf880000u, 0xdf883c00u, 0xdf88c000u, 0xdf887c00u, 0xdf887c01u, 0xdf888000u, 0xdf880400u, 0xdf887bffu,
};
static const uint32_t s_ps_viim_words[PS_VIIM] = {
    0xdf080000u, 0xdf080001u, 0xdf08ffffu, 0xdf087fffu, 0xdf088000u,
};
static const uint32_t s_ps_imm_pfx[PS_IMMD] = {
    0x00000000u, 0xde000001u, 0xde000003u, 0xde000100u,
};
static const char *const s_ps_imm_names[PS_IMMD] = {"none", "sat01", "sat11", "mask"};
static const ps_fn s_ps_vfim_fns[PS_IMMD][PS_VFIM] = {
    {ps_f0_0, ps_f0_1, ps_f0_2, ps_f0_3, ps_f0_4, ps_f0_5, ps_f0_6, ps_f0_7},
    {ps_f1_0, ps_f1_1, ps_f1_2, ps_f1_3, ps_f1_4, ps_f1_5, ps_f1_6, ps_f1_7},
    {ps_f2_0, ps_f2_1, ps_f2_2, ps_f2_3, ps_f2_4, ps_f2_5, ps_f2_6, ps_f2_7},
    {ps_f3_0, ps_f3_1, ps_f3_2, ps_f3_3, ps_f3_4, ps_f3_5, ps_f3_6, ps_f3_7},
};
static const ps_fn s_ps_viim_fns[PS_IMMD][PS_VIIM] = {
    {ps_i0_0, ps_i0_1, ps_i0_2, ps_i0_3, ps_i0_4},
    {ps_i1_0, ps_i1_1, ps_i1_2, ps_i1_3, ps_i1_4},
    {ps_i2_0, ps_i2_1, ps_i2_2, ps_i2_3, ps_i2_4},
    {ps_i3_0, ps_i3_1, ps_i3_2, ps_i3_3, ps_i3_4},
};

/* Operand sets, A (C000) then B (C010). qNaN 0x7fc00011/0xffc00022, sNaN 0x7f800033. */
static const uint32_t s_ps_sets[PS_SETS][8] = {
    {0x3fc00000u, 0xc0100000u, 0x40400000u, 0x3f200000u,    /* 1.5, -2.25, 3, 0.625 */
     0x3f400000u, 0x40800000u, 0xbfc00000u, 0x40200000u},   /* 0.75, 4, -1.5, 2.5 */
    {0xffc00022u, 0x7f800000u, 0x00000001u, 0x80000000u,    /* -qNaN, +inf, +denormal, -0 */
     0x3f800000u, 0xff800000u, 0x80000001u, 0x00000000u},   /* 1, -inf, -denormal, +0 */
    {0x00000000u, 0x7f800033u, 0x807fffffu, 0xbf800000u,    /* +0, +sNaN, -largest denormal, -1 */
     0x80000000u, 0x40000000u, 0x7f800000u, 0x7fc00011u},   /* -0, 2, +inf, +qNaN */
};

static uint32_t s_ps_in[TN_IMAGE_WORDS] __attribute__((aligned(64)));
static uint32_t s_ps_out[TN_IMAGE_WORDS] __attribute__((aligned(64)));
static uint32_t s_ps_ctl[PS_CTL] __attribute__((aligned(16)));

/* Background image, the destination C200 as the sentinel, the control buffer as the sentinel. */
static void ps_image(void)
{
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        s_ps_in[i] = 0x3f800000u | (i << 16);
    }
    for (unsigned i = 0; i < 4u; i++) {
        s_ps_in[PS_DST + i] = TN_SENTINEL;
    }
    for (unsigned i = 0; i < PS_CTL; i++) {
        s_ps_ctl[i] = TN_SENTINEL;
    }
}

static void ps_operands(unsigned pass)
{
    char name[64];
    uint32_t w[PS_MAX_WORDS];
    snprintf(name, sizeof(name), "operands-p%u", pass);
    hle_step(name);
    for (unsigned s = 0; s < PS_SETS; s++) {
        for (unsigned i = 0; i < 8u; i++) {
            w[s * 8u + i] = s_ps_sets[s][i];
        }
    }
    hle_record(name, "PASS", 0u, w, PS_SETS * 8u);
}

/* Row <name>-p<pass>: the instruction word, then C200 lanes 0-3 for each operand set. */
static void ps_arith_record(unsigned pass, const struct ps_arith *a)
{
    char name[64];
    uint32_t w[PS_MAX_WORDS];
    snprintf(name, sizeof(name), "%s-p%u", a->name, pass);
    hle_step(name);
    unsigned count = 0;
    w[count++] = a->word;
    for (unsigned s = 0; s < PS_SETS; s++) {
        ps_image();
        for (unsigned i = 0; i < 8u; i++) {
            s_ps_in[i] = s_ps_sets[s][i];
        }
        a->fn(s_ps_in, s_ps_out, s_ps_ctl);
        for (unsigned i = 0; i < 4u; i++) {
            w[count++] = s_ps_out[PS_DST + i];
        }
    }
    hle_record(name, "PASS", a->pfx, w, count);
}

/* Row life-<name>-p<pass>: S000 = -2, S200 and S201 the sentinel; the nine control words, S200,
   S201. */
static void ps_life_record(unsigned pass, const struct ps_life *l)
{
    char name[64];
    uint32_t w[PS_MAX_WORDS];
    snprintf(name, sizeof(name), "life-%s-p%u", l->name, pass);
    hle_step(name);
    ps_image();
    s_ps_in[0] = 0xc0000000u;
    l->fn(s_ps_in, s_ps_out, s_ps_ctl);
    unsigned count = 0;
    for (unsigned i = 0; i < PS_CTL; i++) {
        w[count++] = s_ps_ctl[i];
    }
    w[count++] = s_ps_out[PS_DST];
    w[count++] = s_ps_out[PS_DST + 1u];
    hle_record(name, "PASS", l->pfx, w, count);
}

/* Row <stem>-<d>-p<pass>: per immediate word, the word and S200 (sentinel before). */
static void ps_imm_record(unsigned pass, const char *stem, unsigned d, const uint32_t *words,
                          const ps_fn *fns, unsigned n)
{
    char name[64];
    uint32_t w[PS_MAX_WORDS];
    snprintf(name, sizeof(name), "%s-%s-p%u", stem, s_ps_imm_names[d], pass);
    hle_step(name);
    unsigned count = 0;
    for (unsigned j = 0; j < n; j++) {
        ps_image();
        fns[j](s_ps_in, s_ps_out, s_ps_ctl);
        w[count++] = words[j];
        w[count++] = s_ps_out[PS_DST];
    }
    hle_record(name, "PASS", s_ps_imm_pfx[d], w, count);
}

static int hle_run(void)
{
    /* Traps off before the first VFPU cell, as in the vfpu-compare case. */
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    for (unsigned pass = 0; pass < 2u; pass++) {
        ps_operands(pass);
        for (unsigned i = 0; i < PS_ARITH; i++) {
            ps_arith_record(pass, &s_ps_arith[i]);
        }
        for (unsigned i = 0; i < PS_LIFE; i++) {
            ps_life_record(pass, &s_ps_life[i]);
        }
        for (unsigned d = 0; d < PS_IMMD; d++) {
            ps_imm_record(pass, "vfim", d, s_ps_vfim_words, s_ps_vfim_fns[d], PS_VFIM);
        }
        for (unsigned d = 0; d < PS_IMMD; d++) {
            ps_imm_record(pass, "viim", d, s_ps_viim_words, s_ps_viim_fns[d], PS_VIIM);
        }
    }
    return 1;
}

#endif /* HLE_CASE_VFPU_PREFIX_STATE */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_VFPU_WIDER_OPS

/* #326 wider VFPU forms (PSP-VFPU-WIDEROPS-001), two passes, register-only: the raw result words
   of forms measured so far only at one width. Every cell is one instruction word run over the
   whole register image with no prefix word issued (the prefixes stay identity), emitted as a
   word. Every word was assembled with psp-as from the mnemonic named here and none was refused:
   vdiv.p/.t/.q C200, C000, C010 (0x63810088, 0x63818008, 0x63818088); vsin.p/.t/.q C200, C000
   (0xd0120088, 0xd0128008, 0xd0128088), vcos (0xd0130088, 0xd0138008, 0xd0138088), vrcp
   (0xd0100088, 0xd0108008, 0xd0108088), vrsq.q (0xd0118088), vsqrt.q (0xd0168088), vocp.s S200,
   S000 (0xd0440008) and vocp.q (0xd0448088); vdot.t S200, C000, C010 (0x64818008), vhdp.p
   (0x66010088), vhdp.t (0x66018008), vscl.t C200, C000, S010 (0x65018008), vcrs.t C200, C000, C010
   (0x66818008), vmmul.p/.t M200, E100, M000 (0xf0000488, 0xf0008408), vtfm2.p C200, M100, C000
   (0xf0800488), vtfm3.t (0xf1008408), vhtfm2.p (0xf0800408), vhtfm3.t (0xf1000488), vmscl.p/.t
   M200, M100, S010 (0xf2010488, 0xf2018408), vmmov.p/.t M200, M100 (0xf3800488, 0xf3808408),
   vmidt.p/.t M200 (0xf3830088, 0xf3838008), vmzero.p/.t (0xf3860088, 0xf3868008), vmone.p/.t
   (0xf3870088, 0xf3878008); vidt.q of C200, C210, C220, C230 and R200-R203 (0xd0038088 |
   register), vidt.p of C200, C202, C210, C212, C220, C222, C230, C232, R200-R203 and R220-R223
   (0xd0030088 | register; psp-as refuses C201, C203 and R210 as pair registers), vzero.t C200
   (0xd0068008) and vone.t C200 (0xd0078008).
   Image layout (word m*16 + c*4 + r): A = C000 (image 0-3), B = C010 (image 4-7, S010 = image 4),
   M000 image 0-15, M100 image 16-31, the destination C200/M200 image 32-47. Background image word
   i = 0x3f800000 | i << 16; image 32-47 start as the sentinel 0x7a5a5a5a in every cell.
   a. vdiv: the 20 operand classes of vfpu-basic-arith (A) against B = 1, +0, +inf, +qNaN
      0x7fc00011 and the smallest denormal, 100 pairs A-major, lane-packed n to an execution (the
      last execution of a record padded with its final pair), 50 pairs a record.
   b. vsin, vcos, vrcp, vrsq, vsqrt and vocp: the 20 special encodings of the vfpu-transcendental
      corpus in order, lane-packed n to an execution (.t padded with the last).
   c. The dot and matrix widths: a finite order-sensitive cell (the vector or every row and column
      of the matrix a permutation of 1e8, 1, -1e8 at .t or 2^24, 1 at .p, the other operand ones),
      then the same with its first source word replaced by +qNaN 0x7fc00011, +inf and the
      smallest denormal; vmidt, vmzero and vmone take one cell.
   d. vidt.q and vidt.p into every legal column and row register of M200, vzero.t and vone.t.
   Rows: <vdiv>-p<pass>-<record> (result = the word; the destination words of each execution, n
   per execution); <form>-p<pass> for b (the same over the 20 specials); <form>-c<cell>-p<pass>
   for a four-cell form of c and <form>-p<pass> for a one-cell form of c and d (result = the
   word; the recorded source words, then C200 lanes 0-3 or M200 words 0-15). A durable step marker
   comes before every record. Nothing here writes memory outside the probe's own image and output
   buffers, no control register is written, and no reserved word is run. */

#include "hle_vfpu_cells.h"

#define WO_DST 32u
#define WO_MAX_WORDS 64u
#define WO_OPERANDS 20u
#define WO_DIV_B 5u
#define WO_DIV_PAIRS (WO_OPERANDS * WO_DIV_B)
#define WO_DIV_PER_REC 50u
#define WO_DIV_RECORDS 2u
#define WO_DIV_FORMS 3u
#define WO_UNARY_FORMS 13u
#define WO_FORMS 47u
#define WO_SRC_WORDS 114u
#define WO_ONE 0x3f800000u

typedef void (*wo_fn)(const uint32_t *in, uint32_t *out, uint32_t *cc);

struct wo_lane_form {
    const char *stem;
    uint32_t word;
    wo_fn fn;
    unsigned lanes;
};

struct wo_form {
    const char *stem;
    uint32_t word;
    wo_fn fn;
    unsigned src0;   /* first entry in s_wo_src_idx / s_wo_src_val */
    unsigned nsrc;   /* recorded source words */
    unsigned ndst;   /* 4 (C200) or 16 (M200) */
    unsigned ncells; /* 4 (finite, qNaN, inf, denormal) or 1 */
};

TN_CELL_FN(wo_vdiv_p, TN_PFX_NONE, "0x63810088")
TN_CELL_FN(wo_vdiv_t, TN_PFX_NONE, "0x63818008")
TN_CELL_FN(wo_vdiv_q, TN_PFX_NONE, "0x63818088")
TN_CELL_FN(wo_vsin_p, TN_PFX_NONE, "0xd0120088")
TN_CELL_FN(wo_vsin_t, TN_PFX_NONE, "0xd0128008")
TN_CELL_FN(wo_vsin_q, TN_PFX_NONE, "0xd0128088")
TN_CELL_FN(wo_vcos_p, TN_PFX_NONE, "0xd0130088")
TN_CELL_FN(wo_vcos_t, TN_PFX_NONE, "0xd0138008")
TN_CELL_FN(wo_vcos_q, TN_PFX_NONE, "0xd0138088")
TN_CELL_FN(wo_vrcp_p, TN_PFX_NONE, "0xd0100088")
TN_CELL_FN(wo_vrcp_t, TN_PFX_NONE, "0xd0108008")
TN_CELL_FN(wo_vrcp_q, TN_PFX_NONE, "0xd0108088")
TN_CELL_FN(wo_vrsq_q, TN_PFX_NONE, "0xd0118088")
TN_CELL_FN(wo_vsqrt_q, TN_PFX_NONE, "0xd0168088")
TN_CELL_FN(wo_vocp_s, TN_PFX_NONE, "0xd0440008")
TN_CELL_FN(wo_vocp_q, TN_PFX_NONE, "0xd0448088")
TN_CELL_FN(wo_vdot_t, TN_PFX_NONE, "0x64818008")
TN_CELL_FN(wo_vhdp_p, TN_PFX_NONE, "0x66010088")
TN_CELL_FN(wo_vhdp_t, TN_PFX_NONE, "0x66018008")
TN_CELL_FN(wo_vscl_t, TN_PFX_NONE, "0x65018008")
TN_CELL_FN(wo_vcrs_t, TN_PFX_NONE, "0x66818008")
TN_CELL_FN(wo_vmmul_p, TN_PFX_NONE, "0xf0000488")
TN_CELL_FN(wo_vmmul_t, TN_PFX_NONE, "0xf0008408")
TN_CELL_FN(wo_vtfm2_p, TN_PFX_NONE, "0xf0800488")
TN_CELL_FN(wo_vtfm3_t, TN_PFX_NONE, "0xf1008408")
TN_CELL_FN(wo_vhtfm2_p, TN_PFX_NONE, "0xf0800408")
TN_CELL_FN(wo_vhtfm3_t, TN_PFX_NONE, "0xf1000488")
TN_CELL_FN(wo_vmscl_p, TN_PFX_NONE, "0xf2010488")
TN_CELL_FN(wo_vmscl_t, TN_PFX_NONE, "0xf2018408")
TN_CELL_FN(wo_vmmov_p, TN_PFX_NONE, "0xf3800488")
TN_CELL_FN(wo_vmmov_t, TN_PFX_NONE, "0xf3808408")
TN_CELL_FN(wo_vmidt_p, TN_PFX_NONE, "0xf3830088")
TN_CELL_FN(wo_vmidt_t, TN_PFX_NONE, "0xf3838008")
TN_CELL_FN(wo_vmzero_p, TN_PFX_NONE, "0xf3860088")
TN_CELL_FN(wo_vmzero_t, TN_PFX_NONE, "0xf3868008")
TN_CELL_FN(wo_vmone_p, TN_PFX_NONE, "0xf3870088")
TN_CELL_FN(wo_vmone_t, TN_PFX_NONE, "0xf3878008")
TN_CELL_FN(wo_vidt_q_c200, TN_PFX_NONE, "0xd0038088")
TN_CELL_FN(wo_vidt_q_c210, TN_PFX_NONE, "0xd0038089")
TN_CELL_FN(wo_vidt_q_c220, TN_PFX_NONE, "0xd003808a")
TN_CELL_FN(wo_vidt_q_c230, TN_PFX_NONE, "0xd003808b")
TN_CELL_FN(wo_vidt_q_r200, TN_PFX_NONE, "0xd00380a8")
TN_CELL_FN(wo_vidt_q_r201, TN_PFX_NONE, "0xd00380a9")
TN_CELL_FN(wo_vidt_q_r202, TN_PFX_NONE, "0xd00380aa")
TN_CELL_FN(wo_vidt_q_r203, TN_PFX_NONE, "0xd00380ab")
TN_CELL_FN(wo_vidt_p_c200, TN_PFX_NONE, "0xd0030088")
TN_CELL_FN(wo_vidt_p_c202, TN_PFX_NONE, "0xd00300c8")
TN_CELL_FN(wo_vidt_p_c210, TN_PFX_NONE, "0xd0030089")
TN_CELL_FN(wo_vidt_p_c212, TN_PFX_NONE, "0xd00300c9")
TN_CELL_FN(wo_vidt_p_c220, TN_PFX_NONE, "0xd003008a")
TN_CELL_FN(wo_vidt_p_c222, TN_PFX_NONE, "0xd00300ca")
TN_CELL_FN(wo_vidt_p_c230, TN_PFX_NONE, "0xd003008b")
TN_CELL_FN(wo_vidt_p_c232, TN_PFX_NONE, "0xd00300cb")
TN_CELL_FN(wo_vidt_p_r200, TN_PFX_NONE, "0xd00300a8")
TN_CELL_FN(wo_vidt_p_r201, TN_PFX_NONE, "0xd00300a9")
TN_CELL_FN(wo_vidt_p_r202, TN_PFX_NONE, "0xd00300aa")
TN_CELL_FN(wo_vidt_p_r203, TN_PFX_NONE, "0xd00300ab")
TN_CELL_FN(wo_vidt_p_r220, TN_PFX_NONE, "0xd00300e8")
TN_CELL_FN(wo_vidt_p_r221, TN_PFX_NONE, "0xd00300e9")
TN_CELL_FN(wo_vidt_p_r222, TN_PFX_NONE, "0xd00300ea")
TN_CELL_FN(wo_vidt_p_r223, TN_PFX_NONE, "0xd00300eb")
TN_CELL_FN(wo_vzero_t, TN_PFX_NONE, "0xd0068008")
TN_CELL_FN(wo_vone_t, TN_PFX_NONE, "0xd0078008")

static const struct wo_lane_form s_wo_div[WO_DIV_FORMS] = {
    {"vdiv-p", 0x63810088u, wo_vdiv_p, 2},
    {"vdiv-t", 0x63818008u, wo_vdiv_t, 3},
    {"vdiv-q", 0x63818088u, wo_vdiv_q, 4},
};
static const struct wo_lane_form s_wo_unary[WO_UNARY_FORMS] = {
    {"vsin-p", 0xd0120088u, wo_vsin_p, 2},
    {"vsin-t", 0xd0128008u, wo_vsin_t, 3},
    {"vsin-q", 0xd0128088u, wo_vsin_q, 4},
    {"vcos-p", 0xd0130088u, wo_vcos_p, 2},
    {"vcos-t", 0xd0138008u, wo_vcos_t, 3},
    {"vcos-q", 0xd0138088u, wo_vcos_q, 4},
    {"vrcp-p", 0xd0100088u, wo_vrcp_p, 2},
    {"vrcp-t", 0xd0108008u, wo_vrcp_t, 3},
    {"vrcp-q", 0xd0108088u, wo_vrcp_q, 4},
    {"vrsq-q", 0xd0118088u, wo_vrsq_q, 4},
    {"vsqrt-q", 0xd0168088u, wo_vsqrt_q, 4},
    {"vocp-s", 0xd0440008u, wo_vocp_s, 1},
    {"vocp-q", 0xd0448088u, wo_vocp_q, 4},
};

/* Source image indices and base words of the cell forms, 114 in all. */
static const uint8_t s_wo_src_idx[WO_SRC_WORDS] = {
    0u, 1u, 2u, 4u, 5u, 6u, 0u, 1u, 4u, 5u, 0u, 1u, 2u, 4u, 5u, 6u,
    0u, 1u, 2u, 4u, 0u, 1u, 2u, 4u, 5u, 6u, 0u, 1u, 4u, 5u, 16u, 17u,
    20u, 21u, 0u, 1u, 2u, 4u, 5u, 6u, 8u, 9u, 10u, 16u, 17u, 18u, 20u, 21u,
    22u, 24u, 25u, 26u, 0u, 1u, 16u, 17u, 20u, 21u, 0u, 1u, 2u, 16u, 17u, 18u,
    20u, 21u, 22u, 24u, 25u, 26u, 0u, 16u, 17u, 20u, 21u, 0u, 1u, 16u, 17u, 18u,
    20u, 21u, 22u, 24u, 25u, 26u, 4u, 16u, 17u, 20u, 21u, 4u, 16u, 17u, 18u, 20u,
    21u, 22u, 24u, 25u, 26u, 16u, 17u, 20u, 21u, 16u, 17u, 18u, 20u, 21u, 22u, 24u,
    25u, 26u,
};
static const uint32_t s_wo_src_val[WO_SRC_WORDS] = {
    0x4cbebc20u, 0x3f800000u, 0xccbebc20u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
    0x4b800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u, 0x4cbebc20u, 0x3f800000u,
    0x3f800000u, 0x3f800000u, 0x3f800000u, 0xccbebc20u, 0x4cbebc20u, 0x3f800000u,
    0xccbebc20u, 0x3eaaaaabu, 0x4cbebc20u, 0x3f800000u, 0xccbebc20u, 0x3f400000u,
    0x40800000u, 0xbfc00000u, 0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
    0x4b800000u, 0x3f800000u, 0x3f800000u, 0x4b800000u, 0x3f800000u, 0x3f800000u,
    0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
    0x3f800000u, 0x4cbebc20u, 0x3f800000u, 0xccbebc20u, 0x3f800000u, 0xccbebc20u,
    0x4cbebc20u, 0xccbebc20u, 0x4cbebc20u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
    0x4b800000u, 0x3f800000u, 0x3f800000u, 0x4b800000u, 0x3f800000u, 0x3f800000u,
    0x3f800000u, 0x4cbebc20u, 0x3f800000u, 0xccbebc20u, 0x3f800000u, 0xccbebc20u,
    0x4cbebc20u, 0xccbebc20u, 0x4cbebc20u, 0x3f800000u, 0x3f800000u, 0x4b800000u,
    0x3f800000u, 0x3f800000u, 0x4b800000u, 0x3f800000u, 0x3f800000u, 0x4cbebc20u,
    0x3f800000u, 0xccbebc20u, 0x3f800000u, 0xccbebc20u, 0x4cbebc20u, 0xccbebc20u,
    0x4cbebc20u, 0x3f800000u, 0x3eaaaaabu, 0x4b800000u, 0x3f800000u, 0x3f800000u,
    0x4b800000u, 0x3eaaaaabu, 0x4cbebc20u, 0x3f800000u, 0xccbebc20u, 0x3f800000u,
    0xccbebc20u, 0x4cbebc20u, 0xccbebc20u, 0x4cbebc20u, 0x3f800000u, 0x4b800000u,
    0x3f800000u, 0x3f800000u, 0x4b800000u, 0x4cbebc20u, 0x3f800000u, 0xccbebc20u,
    0x3f800000u, 0xccbebc20u, 0x4cbebc20u, 0xccbebc20u, 0x4cbebc20u, 0x3f800000u,
};

/* stem, word, cell function, first source, sources, destination words, cells. */
static const struct wo_form s_wo_forms[WO_FORMS] = {
    {"vdot-t", 0x64818008u, wo_vdot_t, 0, 6, 4, 4},
    {"vhdp-p", 0x66010088u, wo_vhdp_p, 6, 4, 4, 4},
    {"vhdp-t", 0x66018008u, wo_vhdp_t, 10, 6, 4, 4},
    {"vscl-t", 0x65018008u, wo_vscl_t, 16, 4, 4, 4},
    {"vcrs-t", 0x66818008u, wo_vcrs_t, 20, 6, 4, 4},
    {"vmmul-p", 0xf0000488u, wo_vmmul_p, 26, 8, 16, 4},
    {"vmmul-t", 0xf0008408u, wo_vmmul_t, 34, 18, 16, 4},
    {"vtfm2-p", 0xf0800488u, wo_vtfm2_p, 52, 6, 4, 4},
    {"vtfm3-t", 0xf1008408u, wo_vtfm3_t, 58, 12, 4, 4},
    {"vhtfm2-p", 0xf0800408u, wo_vhtfm2_p, 70, 5, 4, 4},
    {"vhtfm3-t", 0xf1000488u, wo_vhtfm3_t, 75, 11, 4, 4},
    {"vmscl-p", 0xf2010488u, wo_vmscl_p, 86, 5, 16, 4},
    {"vmscl-t", 0xf2018408u, wo_vmscl_t, 91, 10, 16, 4},
    {"vmmov-p", 0xf3800488u, wo_vmmov_p, 101, 4, 16, 4},
    {"vmmov-t", 0xf3808408u, wo_vmmov_t, 105, 9, 16, 4},
    {"vmidt-p", 0xf3830088u, wo_vmidt_p, 114, 0, 16, 1},
    {"vmidt-t", 0xf3838008u, wo_vmidt_t, 114, 0, 16, 1},
    {"vmzero-p", 0xf3860088u, wo_vmzero_p, 114, 0, 16, 1},
    {"vmzero-t", 0xf3868008u, wo_vmzero_t, 114, 0, 16, 1},
    {"vmone-p", 0xf3870088u, wo_vmone_p, 114, 0, 16, 1},
    {"vmone-t", 0xf3878008u, wo_vmone_t, 114, 0, 16, 1},
    {"vidt-q-c200", 0xd0038088u, wo_vidt_q_c200, 114, 0, 16, 1},
    {"vidt-q-c210", 0xd0038089u, wo_vidt_q_c210, 114, 0, 16, 1},
    {"vidt-q-c220", 0xd003808au, wo_vidt_q_c220, 114, 0, 16, 1},
    {"vidt-q-c230", 0xd003808bu, wo_vidt_q_c230, 114, 0, 16, 1},
    {"vidt-q-r200", 0xd00380a8u, wo_vidt_q_r200, 114, 0, 16, 1},
    {"vidt-q-r201", 0xd00380a9u, wo_vidt_q_r201, 114, 0, 16, 1},
    {"vidt-q-r202", 0xd00380aau, wo_vidt_q_r202, 114, 0, 16, 1},
    {"vidt-q-r203", 0xd00380abu, wo_vidt_q_r203, 114, 0, 16, 1},
    {"vidt-p-c200", 0xd0030088u, wo_vidt_p_c200, 114, 0, 16, 1},
    {"vidt-p-c202", 0xd00300c8u, wo_vidt_p_c202, 114, 0, 16, 1},
    {"vidt-p-c210", 0xd0030089u, wo_vidt_p_c210, 114, 0, 16, 1},
    {"vidt-p-c212", 0xd00300c9u, wo_vidt_p_c212, 114, 0, 16, 1},
    {"vidt-p-c220", 0xd003008au, wo_vidt_p_c220, 114, 0, 16, 1},
    {"vidt-p-c222", 0xd00300cau, wo_vidt_p_c222, 114, 0, 16, 1},
    {"vidt-p-c230", 0xd003008bu, wo_vidt_p_c230, 114, 0, 16, 1},
    {"vidt-p-c232", 0xd00300cbu, wo_vidt_p_c232, 114, 0, 16, 1},
    {"vidt-p-r200", 0xd00300a8u, wo_vidt_p_r200, 114, 0, 16, 1},
    {"vidt-p-r201", 0xd00300a9u, wo_vidt_p_r201, 114, 0, 16, 1},
    {"vidt-p-r202", 0xd00300aau, wo_vidt_p_r202, 114, 0, 16, 1},
    {"vidt-p-r203", 0xd00300abu, wo_vidt_p_r203, 114, 0, 16, 1},
    {"vidt-p-r220", 0xd00300e8u, wo_vidt_p_r220, 114, 0, 16, 1},
    {"vidt-p-r221", 0xd00300e9u, wo_vidt_p_r221, 114, 0, 16, 1},
    {"vidt-p-r222", 0xd00300eau, wo_vidt_p_r222, 114, 0, 16, 1},
    {"vidt-p-r223", 0xd00300ebu, wo_vidt_p_r223, 114, 0, 16, 1},
    {"vzero-t", 0xd0068008u, wo_vzero_t, 114, 0, 4, 1},
    {"vone-t", 0xd0078008u, wo_vone_t, 114, 0, 4, 1},
};

/* The 20 operand classes of vfpu-basic-arith: +-0, +-smallest denormal, +-largest denormal,
   +-smallest normal, +-1, +-2^127, +-FLT_MAX, +-inf, quiet and signalling NaNs of both signs. */
static const uint32_t s_wo_ops[WO_OPERANDS] = {
    0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x007fffffu, 0x807fffffu,
    0x00800000u, 0x80800000u, 0x3f800000u, 0xbf800000u, 0x7f000000u, 0xff000000u,
    0x7f7fffffu, 0xff7fffffu, 0x7f800000u, 0xff800000u, 0x7fc00011u, 0xffc00022u,
    0x7f800033u, 0xff800044u,
};
/* The vdiv divisors: 1, +0, +inf, +qNaN, the smallest denormal. */
static const uint32_t s_wo_div_b[WO_DIV_B] = {
    0x3f800000u, 0x00000000u, 0x7f800000u, 0x7fc00011u, 0x00000001u,
};
/* The 20 special encodings of the vfpu-transcendental corpus (probe.c). */
static const uint32_t s_wo_trans[WO_OPERANDS] = {
    0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x3f800000u, 0xbf800000u,
    0x3f000000u, 0xbf000000u, 0x40000000u, 0xc0000000u, 0x00800000u, 0x80800000u,
    0x7f000000u, 0xff000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u,
    0x7f800001u, 0xff800001u,
};
/* The first source word of cells 1-3: +qNaN, +inf, the smallest denormal. */
static const uint32_t s_wo_spec[3] = {0x7fc00011u, 0x7f800000u, 0x00000001u};

static uint32_t s_wo_in[TN_IMAGE_WORDS] __attribute__((aligned(64)));
static uint32_t s_wo_out[TN_IMAGE_WORDS] __attribute__((aligned(64)));

/* Background image, image 32-47 the sentinel. */
static void wo_image(void)
{
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        s_wo_in[i] = 0x3f800000u | (i << 16);
    }
    for (unsigned i = 0; i < 16u; i++) {
        s_wo_in[WO_DST + i] = TN_SENTINEL;
    }
}

/* One lane-packed execution: A over image 0-3, B over image 4-7; n destination words to dst. */
static void wo_lane_exec(wo_fn fn, const uint32_t *a, const uint32_t *b, uint32_t *dst, unsigned n)
{
    wo_image();
    for (unsigned i = 0; i < 4u; i++) {
        s_wo_in[i] = a[i];
        s_wo_in[4u + i] = b[i];
    }
    uint32_t cc[2] = {0u, 0u};
    fn(s_wo_in, s_wo_out, cc);
    for (unsigned i = 0; i < n; i++) {
        dst[i] = s_wo_out[WO_DST + i];
    }
}

/* Row <vdiv>-p<pass>-<rec>: pairs 50*rec .. (k = 5 * a + b), lane l of execution e taking pair
   50*rec + e*n + l, clamped to the last pair of the record. */
static void wo_div_group(unsigned pass, const struct wo_lane_form *f)
{
    char name[64];
    uint32_t w[WO_MAX_WORDS];
    const unsigned n = f->lanes;
    const unsigned nexec = (WO_DIV_PER_REC + n - 1u) / n;
    for (unsigned rec = 0; rec < WO_DIV_RECORDS; rec++) {
        snprintf(name, sizeof(name), "%s-p%u-%u", f->stem, pass, rec);
        hle_step(name);
        unsigned count = 0;
        for (unsigned e = 0; e < nexec; e++) {
            uint32_t a[4] = {WO_ONE, WO_ONE, WO_ONE, WO_ONE};
            uint32_t b[4] = {WO_ONE, WO_ONE, WO_ONE, WO_ONE};
            for (unsigned l = 0; l < n; l++) {
                unsigned k = e * n + l;
                if (k >= WO_DIV_PER_REC) {
                    k = WO_DIV_PER_REC - 1u;
                }
                k += rec * WO_DIV_PER_REC;
                a[l] = s_wo_ops[k / WO_DIV_B];
                b[l] = s_wo_div_b[k % WO_DIV_B];
            }
            wo_lane_exec(f->fn, a, b, &w[count], n);
            count += n;
        }
        hle_record(name, "PASS", f->word, w, count);
    }
}

/* Row <form>-p<pass>: the 20 specials over the lanes, n to an execution, the last clamped. */
static void wo_unary_group(unsigned pass, const struct wo_lane_form *f)
{
    char name[64];
    uint32_t w[WO_MAX_WORDS];
    const unsigned n = f->lanes;
    const unsigned nexec = (WO_OPERANDS + n - 1u) / n;
    snprintf(name, sizeof(name), "%s-p%u", f->stem, pass);
    hle_step(name);
    unsigned count = 0;
    for (unsigned e = 0; e < nexec; e++) {
        uint32_t a[4] = {WO_ONE, WO_ONE, WO_ONE, WO_ONE};
        const uint32_t b[4] = {WO_ONE, WO_ONE, WO_ONE, WO_ONE};
        for (unsigned l = 0; l < n; l++) {
            unsigned k = e * n + l;
            if (k >= WO_OPERANDS) {
                k = WO_OPERANDS - 1u;
            }
            a[l] = s_wo_trans[k];
        }
        wo_lane_exec(f->fn, a, b, &w[count], n);
        count += n;
    }
    hle_record(name, "PASS", f->word, w, count);
}

/* Row <form>[-c<cell>]-p<pass>: the source words (cell 1-3 replace the first), the word, then
   the destination words. */
static void wo_form_group(unsigned pass, const struct wo_form *f)
{
    char name[64];
    uint32_t w[WO_MAX_WORDS];
    const unsigned nin = f->nsrc;
    const unsigned nout = f->ndst;
    const unsigned ncells = f->ncells;
    for (unsigned cell = 0; cell < ncells; cell++) {
        if (ncells > 1u) {
            snprintf(name, sizeof(name), "%s-c%u-p%u", f->stem, cell, pass);
        } else {
            snprintf(name, sizeof(name), "%s-p%u", f->stem, pass);
        }
        hle_step(name);
        wo_image();
        for (unsigned j = 0; j < nin; j++) {
            s_wo_in[s_wo_src_idx[f->src0 + j]] = s_wo_src_val[f->src0 + j];
        }
        if (cell > 0u) {
            s_wo_in[s_wo_src_idx[f->src0]] = s_wo_spec[cell - 1u];
        }
        unsigned count = 0;
        for (unsigned j = 0; j < nin; j++) {
            w[count++] = s_wo_in[s_wo_src_idx[f->src0 + j]];
        }
        uint32_t cc[2] = {0u, 0u};
        f->fn(s_wo_in, s_wo_out, cc);
        for (unsigned i = 0; i < nout; i++) {
            w[count++] = s_wo_out[WO_DST + i];
        }
        hle_record(name, "PASS", f->word, w, count);
    }
}

static int hle_run(void)
{
    /* Traps off before the first VFPU cell, as in the vfpu-compare case. */
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    for (unsigned pass = 0; pass < 2u; pass++) {
        for (unsigned form = 0; form < WO_DIV_FORMS; form++) {
            wo_div_group(pass, &s_wo_div[form]);
        }
        for (unsigned form = 0; form < WO_UNARY_FORMS; form++) {
            wo_unary_group(pass, &s_wo_unary[form]);
        }
        for (unsigned form = 0; form < WO_FORMS; form++) {
            wo_form_group(pass, &s_wo_forms[form]);
        }
    }
    return 1;
}

#endif /* HLE_CASE_VFPU_WIDER_OPS */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_VFPU_RESERVED_WIDTHS

/* #326 reserved VFPU widths (PSP-VFPU-RESERVED-001), two passes, register-only: the raw result
   words of the vus2i/vs2i encodings at the widths psp-as refuses. vus2i and vs2i (top halves
   0xd03a and 0xd03b, size in bits 15 and 7, source in bits 14:8, destination in bits 6:0) are
   defined at .s and .p only (measured by vfpu-vus2i); here they run at .t (bit 15) and .q (bits
   15 and 7) with source C000 and destination C200: vus2i.t 0xd03a8008, vus2i.q 0xd03a8088, vs2i.t
   0xd03b8008 and vs2i.q 0xd03b8088. psp-as refuses every one of the four mnemonics, and
   psp-objdump decodes none of the words, so they are emitted as hand-encoded words. vdiv has no
   reserved width: all four size encodings are .s/.p/.t/.q, so no vdiv word is run. POWER RISK:
   a reserved word may raise a reserved-instruction exception that stops the launch (nothing here
   catches it); every cell has its own durable step marker, the case is its own PRX, and it needs
   the owner present. Each cell sets the whole register image to the sentinel 0x7a5a5a5a, then
   C000 and C010 to fixed integer patterns, runs the one word with identity prefixes and stores
   the image. Row <form>-p<pass>: result = the word; out0-15 = M200 after the word (image 32-47,
   so a destination wider than a quad is visible), out16-19 = C000 after the word. Pass 0 runs
   vus2i.t, vus2i.q, vs2i.t, vs2i.q, then pass 1 repeats them. Nothing here writes memory
   outside the probe's own image and output buffers, and no control register is written. */

#include "hle_vfpu_cells.h"

#define RW_DST 32u
#define RW_FORMS 4u
#define RW_WORDS 20u

typedef void (*rw_fn)(const uint32_t *in, uint32_t *out, uint32_t *cc);

TN_CELL_FN(rw_vus2i_t, TN_PFX_NONE, "0xd03a8008")
TN_CELL_FN(rw_vus2i_q, TN_PFX_NONE, "0xd03a8088")
TN_CELL_FN(rw_vs2i_t, TN_PFX_NONE, "0xd03b8008")
TN_CELL_FN(rw_vs2i_q, TN_PFX_NONE, "0xd03b8088")

struct rw_form {
    const char *stem;
    uint32_t word;
    rw_fn fn;
};

static const struct rw_form s_rw_forms[RW_FORMS] = {
    {"vus2i-t", 0xd03a8008u, rw_vus2i_t},
    {"vus2i-q", 0xd03a8088u, rw_vus2i_q},
    {"vs2i-t", 0xd03b8008u, rw_vs2i_t},
    {"vs2i-q", 0xd03b8088u, rw_vs2i_q},
};

/* C000 then C010: signed and unsigned 16-bit halves at both ends of their ranges. */
static const uint32_t s_rw_src[8] = {
    0x80017fffu, 0x0000ffffu, 0x12345678u, 0xfedcba98u,
    0x11112222u, 0x33334444u, 0x55556666u, 0x77778888u,
};

static uint32_t s_rw_in[TN_IMAGE_WORDS] __attribute__((aligned(64)));
static uint32_t s_rw_out[TN_IMAGE_WORDS] __attribute__((aligned(64)));

static void rw_cell(unsigned pass, const struct rw_form *f)
{
    char name[64];
    uint32_t w[RW_WORDS];
    snprintf(name, sizeof(name), "%s-p%u", f->stem, pass);
    hle_step(name);
    for (unsigned i = 0; i < TN_IMAGE_WORDS; i++) {
        s_rw_in[i] = TN_SENTINEL;
    }
    for (unsigned i = 0; i < 8u; i++) {
        s_rw_in[i] = s_rw_src[i];
    }
    uint32_t cc[2] = {0u, 0u};
    f->fn(s_rw_in, s_rw_out, cc);
    for (unsigned i = 0; i < 16u; i++) {
        w[i] = s_rw_out[RW_DST + i];
    }
    for (unsigned i = 0; i < 4u; i++) {
        w[16u + i] = s_rw_out[i];
    }
    hle_record(name, "PASS", f->word, w, RW_WORDS);
}

static int hle_run(void)
{
    /* Traps off before the first VFPU cell, as in the vfpu-compare case. */
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    for (unsigned pass = 0; pass < 2u; pass++) {
        for (unsigned form = 0; form < RW_FORMS; form++) {
            rw_cell(pass, &s_rw_forms[form]);
        }
    }
    return 1;
}

#endif /* HLE_CASE_VFPU_RESERVED_WIDTHS */

#if PSP_ORACLE_CASE == HLE_CASE_VFPU_CC_CMOV

/* #326 VFPU condition codes and conditional moves (PSP-VFPU-CCCMOV-001), two passes,
   register-only: no memory or host I/O runs between a cell's register seeding and its readback.
   Every VFPU word is a valid form (vcmp conditions 0-15, vcmovt/vcmovf imm3 0-6, mfvc/mtvc of
   VFPU_CC), so no exception is expected; a fault would still stop the launch (POWER RISK, owner
   present). An identity vpfxs (and vpfxt for vcmp) is issued right before every word. Each form
   group writes a durable step marker first. Rows:
   - vcmp-c<hh>-<s|q>-p<pass>: result = the word; out0/out1 = VFPU_CC read back after the
     preset 0x00 / 0x3f; then for each of 4 operand sets (C000 = a, C010 = b) and each preset
     (0x00 then 0x3f) seven words: VFPU_CC at once after the word, after a vsync, after a vflush,
     and the four C200 words after a dependent vcmovt.q imm3 6 from C020 (a lane moves when its
     own CC bit is set). A lane beyond the width that keeps its old bit shows as 0 under 0x00 and
     1 under 0x3f; bits 4 (OR) and 5 (AND) are recomputed;
   - vcmov<t|f><imm>-<s|p|q>-p<pass>: result = the word; for each of nine preset VFPU_CC values
     the readback before, VFPU_CC after, and the four words of C200 after the word (source C000,
     C200 preloaded with distinct words so a lane that is not moved keeps them);
   - cc-roundtrip-p<pass>: mtvc VFPU_CC with twelve words, each read back with mfvc;
   - cc-initial-p<pass>: mfvc VFPU_CC with no write first (pass 0: state at launch). */

#define CM_SETS 4u
#define CM_PRESETS 9u
#define CM_RT_VALUES 12u
#define CM_CMP_WORDS_OUT 58u
#define CM_CMOV_WORDS_OUT 54u
#define CM_PASSES 2u

typedef void (*cm_cmp_fn)(const uint32_t *a, const uint32_t *b, const uint32_t *src,
                          const uint32_t *dst, uint32_t *out, uint32_t *cc, uint32_t pre);
typedef void (*cm_cmov_fn)(const uint32_t *src, const uint32_t *dst, uint32_t *out, uint32_t *cc,
                           uint32_t pre);

#define CM_SIZE_S 0x0u
#define CM_SIZE_P 0x80u
#define CM_SIZE_T 0x8000u
#define CM_SIZE_Q 0x8080u
/* vcmp: op 0x6c, cond in bits 0-3, vs = C000 / S000 (0), vt = C010 / S010 (1 << 16). */
#define CM_CMP_WORD(c, size) (0x6c000000u | (c) | 0x10000u | (size))
/* The dependent readout after a vcmp: vcmovt.q imm3 6 (each lane moves when its own CC bit is set,
   whatever the preset) from C020 into C200, so the CC bits show through the data path. */
#define CM_RD_WORD (CM_CMOV_WORD(0u, 6u, CM_SIZE_Q) | (2u << 8))
/* vcmovt 0xd2a00000, vcmovf 0xd2a80000, imm3 in bits 16-18, vs = C000 / S000, vd = C200 / S200. */
#define CM_CMOV_WORD(f, imm, size) \
    (0xd2a00000u | ((f) ? 0x80000u : 0u) | ((imm) << 16) | (size) | 0x08u)

/* mtvc $t0, VFPU_CC; lv.q a, b, source C020 and destination C200; mfvc readback; identity vpfxs and
   vpfxt; the vcmp word; then VFPU_CC three ways: mfvc at once (cc[1]), mfvc after a vsync
   0xffff0320 (cc[2]), mfvc after a vflush 0xffff040d (cc[3]); then the dependent vcmovt.q imm3 6
   and the four words of C200 stored to out. */
#define CM_CMP_FN(fname, word)                                                         \
    static void fname(const uint32_t *a, const uint32_t *b, const uint32_t *src,       \
                      const uint32_t *dst, uint32_t *out, uint32_t *cc, uint32_t pre)  \
    {                                                                                  \
        __asm__ volatile(                                                              \
            "move $t0, %[pre]\n\t"                                                     \
            ".word 0x48e80083\n\t" /* mtvc $t0, VFPU_CC */                             \
            "lv.q C000, 0(%[a])\n\t"                                                   \
            "lv.q C010, 0(%[b])\n\t"                                                   \
            "lv.q C020, 0(%[src])\n\t"                                                 \
            "lv.q C200, 0(%[dst])\n\t"                                                 \
            ".word 0x48690083\n\t" /* mfvc $t1, VFPU_CC */                             \
            "sw $t1, 0(%[cc])\n\t"                                                     \
            ".word 0xdc0000e4\n\t" /* vpfxs identity */                                \
            ".word 0xdd0000e4\n\t" /* vpfxt identity */                                \
            ".word %[w]\n\t"                                                           \
            ".word 0x48690083\n\t"                                                     \
            "sw $t1, 4(%[cc])\n\t"                                                     \
            ".word 0xffff0320\n\t" /* vsync */                                         \
            ".word 0x48690083\n\t"                                                     \
            "sw $t1, 8(%[cc])\n\t"                                                     \
            ".word 0xffff040d\n\t" /* vflush */                                        \
            ".word 0x48690083\n\t"                                                     \
            "sw $t1, 12(%[cc])\n\t"                                                    \
            ".word %[rd]\n\t"                                                          \
            "sv.q C200, 0(%[out])\n\t"                                                 \
            :                                                                          \
            : [a] "r"(a), [b] "r"(b), [src] "r"(src), [dst] "r"(dst), [out] "r"(out),  \
              [cc] "r"(cc), [pre] "r"(pre), [w] "i"(word), [rd] "i"(CM_RD_WORD)        \
            : "t0", "t1", "memory");                                                   \
    }

#define CM_CMOV_FN(fname, word)                                                        \
    static void fname(const uint32_t *src, const uint32_t *dst, uint32_t *out,         \
                      uint32_t *cc, uint32_t pre)                                      \
    {                                                                                  \
        __asm__ volatile(                                                              \
            "move $t0, %[pre]\n\t"                                                     \
            ".word 0x48e80083\n\t"                                                     \
            "lv.q C000, 0(%[src])\n\t"                                                 \
            "lv.q C200, 0(%[dst])\n\t"                                                 \
            ".word 0x48690083\n\t"                                                     \
            "sw $t1, 0(%[cc])\n\t"                                                     \
            ".word 0xdc0000e4\n\t" /* vpfxs identity */                                \
            ".word %[w]\n\t"                                                           \
            ".word 0x48690083\n\t"                                                     \
            "sw $t1, 4(%[cc])\n\t"                                                     \
            "sv.q C200, 0(%[out])\n\t"                                                 \
            :                                                                          \
            : [src] "r"(src), [dst] "r"(dst), [out] "r"(out), [cc] "r"(cc),            \
              [pre] "r"(pre), [w] "i"(word)                                            \
            : "t0", "t1", "memory");                                                   \
    }

#define CM_DEF_CMP(c)                                          \
    CM_CMP_FN(cm_cmp##c##_s, CM_CMP_WORD(c, CM_SIZE_S))        \
    CM_CMP_FN(cm_cmp##c##_q, CM_CMP_WORD(c, CM_SIZE_Q))
#define CM_DEF_CMOV(f, imm)                                                   \
    CM_CMOV_FN(cm_cmov##f##imm##_s, CM_CMOV_WORD(f, imm, CM_SIZE_S))          \
    CM_CMOV_FN(cm_cmov##f##imm##_p, CM_CMOV_WORD(f, imm, CM_SIZE_P))          \
    CM_CMOV_FN(cm_cmov##f##imm##_q, CM_CMOV_WORD(f, imm, CM_SIZE_Q))

CM_DEF_CMP(0)  CM_DEF_CMP(1)  CM_DEF_CMP(2)  CM_DEF_CMP(3)
CM_DEF_CMP(4)  CM_DEF_CMP(5)  CM_DEF_CMP(6)  CM_DEF_CMP(7)
CM_DEF_CMP(8)  CM_DEF_CMP(9)  CM_DEF_CMP(10) CM_DEF_CMP(11)
CM_DEF_CMP(12) CM_DEF_CMP(13) CM_DEF_CMP(14) CM_DEF_CMP(15)
CM_DEF_CMOV(0, 0) CM_DEF_CMOV(0, 1) CM_DEF_CMOV(0, 2) CM_DEF_CMOV(0, 3)
CM_DEF_CMOV(0, 4) CM_DEF_CMOV(0, 5) CM_DEF_CMOV(0, 6)
CM_DEF_CMOV(1, 0) CM_DEF_CMOV(1, 1) CM_DEF_CMOV(1, 2) CM_DEF_CMOV(1, 3)
CM_DEF_CMOV(1, 4) CM_DEF_CMOV(1, 5) CM_DEF_CMOV(1, 6)

#define CM_CMP_ROW(c) {cm_cmp##c##_s, cm_cmp##c##_q}
static const cm_cmp_fn s_cm_cmp[16][2] = {
    CM_CMP_ROW(0),  CM_CMP_ROW(1),  CM_CMP_ROW(2),  CM_CMP_ROW(3),
    CM_CMP_ROW(4),  CM_CMP_ROW(5),  CM_CMP_ROW(6),  CM_CMP_ROW(7),
    CM_CMP_ROW(8),  CM_CMP_ROW(9),  CM_CMP_ROW(10), CM_CMP_ROW(11),
    CM_CMP_ROW(12), CM_CMP_ROW(13), CM_CMP_ROW(14), CM_CMP_ROW(15),
};
#define CM_CMOV_ROW(f, imm) {cm_cmov##f##imm##_s, cm_cmov##f##imm##_p, cm_cmov##f##imm##_q}
static const cm_cmov_fn s_cm_cmov[2][7][3] = {
    {CM_CMOV_ROW(0, 0), CM_CMOV_ROW(0, 1), CM_CMOV_ROW(0, 2), CM_CMOV_ROW(0, 3),
     CM_CMOV_ROW(0, 4), CM_CMOV_ROW(0, 5), CM_CMOV_ROW(0, 6)},
    {CM_CMOV_ROW(1, 0), CM_CMOV_ROW(1, 1), CM_CMOV_ROW(1, 2), CM_CMOV_ROW(1, 3),
     CM_CMOV_ROW(1, 4), CM_CMOV_ROW(1, 5), CM_CMOV_ROW(1, 6)},
};
static const char s_cm_widths[4] = {'s', 'p', 't', 'q'};

/* Operand sets: a lanes 0-3 then b lanes 0-3. Finite orderings, signed zeros and denormals,
   infinities, quiet and signalling NaNs of both signs. */
static const uint32_t s_cm_sets[4][8] = {
    {0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u,
     0x40000000u, 0x40000000u, 0x3f800000u, 0x40a00000u}, /* lt eq gt lt */
    {0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u,
     0x80000000u, 0x00000000u, 0x00000000u, 0x00000001u}, /* signed zeros and denormals */
    {0x7f800000u, 0xff800000u, 0x7f800000u, 0xff800000u,
     0x7f800000u, 0x7f800000u, 0x3f800000u, 0xbf800000u}, /* infinities */
    {0x7fc00000u, 0x7f800001u, 0xffc00000u, 0x3f800000u,
     0x3f800000u, 0x7fc00000u, 0x7fc00000u, 0x3f800000u}, /* quiet, signalling, negative NaN */
};
static const uint32_t s_cm_presets[CM_PRESETS] = {0x00u, 0x3fu, 0x15u, 0x01u, 0x02u,
                                                  0x04u, 0x08u, 0x10u, 0x20u};
static const uint32_t s_cm_rt[CM_RT_VALUES] = {0x00u, 0x3fu, 0x15u, 0x2au, 0x01u, 0x10u,
                                               0x20u, 0x40u, 0x80u, 0xffu, 0x100u, 0xffffffffu};
/* cmov source: 1.0, -0, qNaN, smallest denormal; destination preload: four other words. */
static const uint32_t s_cm_cmov_src[4] __attribute__((aligned(16))) = {
    0x3f800000u, 0x80000000u, 0x7fc00000u, 0x00000001u};
static const uint32_t s_cm_cmov_dst[4] __attribute__((aligned(16))) = {
    0xbf800000u, 0x40400000u, 0xc0800000u, 0x41200000u};
static uint32_t s_cm_a[4] __attribute__((aligned(16)));
static uint32_t s_cm_b[4] __attribute__((aligned(16)));
static uint32_t s_cm_out[4] __attribute__((aligned(16)));

static void cm_cmp_group(unsigned pass, unsigned cond)
{
    char name[64];
    snprintf(name, sizeof(name), "vcmp-c%02x-p%u", cond, pass);
    hle_step(name);
    for (unsigned w = 0; w < 2u; w++) {
        const uint32_t sizes[2] = {CM_SIZE_S, CM_SIZE_Q};
        const uint32_t word = CM_CMP_WORD(cond, sizes[w]);
        uint32_t rec[CM_CMP_WORDS_OUT];
        for (unsigned i = 0; i < CM_CMP_WORDS_OUT; i++) {
            rec[i] = 0xffffffffu;
        }
        for (unsigned set = 0; set < CM_SETS; set++) {
            for (unsigned i = 0; i < 4u; i++) {
                s_cm_a[i] = s_cm_sets[set][i];
                s_cm_b[i] = s_cm_sets[set][4u + i];
            }
            for (unsigned p = 0; p < 2u; p++) {
                uint32_t cc[4] = {0u, 0u, 0u, 0u};
                s_cm_out[0] = s_cm_out[1] = s_cm_out[2] = s_cm_out[3] = 0xffffffffu;
                s_cm_cmp[cond][w](s_cm_a, s_cm_b, s_cm_cmov_src, s_cm_cmov_dst, s_cm_out, cc,
                                  p == 0u ? 0x00u : 0x3fu);
                rec[p] = cc[0];
                const unsigned base = 2u + 14u * set + 7u * p;
                rec[base] = cc[1];
                rec[base + 1u] = cc[2];
                rec[base + 2u] = cc[3];
                for (unsigned i = 0; i < 4u; i++) {
                    rec[base + 3u + i] = s_cm_out[i];
                }
            }
        }
        snprintf(name, sizeof(name), "vcmp-c%02x-%c-p%u", cond, s_cm_widths[w == 1u ? 3u : w],
                 pass);
        hle_record(name, "PASS", word, rec, CM_CMP_WORDS_OUT);
    }
}

static void cm_cmov_group(unsigned pass, unsigned f, unsigned imm)
{
    char name[64];
    snprintf(name, sizeof(name), "vcmov%c%u-p%u", f ? 'f' : 't', imm, pass);
    hle_step(name);
    for (unsigned w = 0; w < 3u; w++) {
        const uint32_t sizes[3] = {CM_SIZE_S, CM_SIZE_P, CM_SIZE_Q};
        const uint32_t word = CM_CMOV_WORD(f, imm, sizes[w]);
        uint32_t rec[CM_CMOV_WORDS_OUT];
        for (unsigned i = 0; i < CM_CMOV_WORDS_OUT; i++) {
            rec[i] = 0xffffffffu;
        }
        for (unsigned p = 0; p < CM_PRESETS; p++) {
            uint32_t cc[2] = {0u, 0u};
            s_cm_out[0] = s_cm_out[1] = s_cm_out[2] = s_cm_out[3] = 0xffffffffu;
            s_cm_cmov[f][imm][w](s_cm_cmov_src, s_cm_cmov_dst, s_cm_out, cc, s_cm_presets[p]);
            rec[6u * p] = cc[0];
            rec[6u * p + 1u] = cc[1];
            for (unsigned i = 0; i < 4u; i++) {
                rec[6u * p + 2u + i] = s_cm_out[i];
            }
        }
        snprintf(name, sizeof(name), "vcmov%c%u-%c-p%u", f ? 'f' : 't', imm,
                 s_cm_widths[w == 2u ? 3u : w], pass);
        hle_record(name, "PASS", word, rec, CM_CMOV_WORDS_OUT);
    }
}

/* mtvc $t1, VFPU_CC; mfvc $t1, VFPU_CC. */
static uint32_t cm_cc_roundtrip(uint32_t value)
{
    uint32_t back;
    __asm__ volatile(
        "move $t1, %[v]\n\t"
        ".word 0x48e90083\n\t"
        ".word 0x48690083\n\t"
        "move %[o], $t1\n\t"
        : [o] "=&r"(back)
        : [v] "r"(value)
        : "t1", "memory");
    return back;
}

static uint32_t cm_cc_read(void)
{
    uint32_t back;
    __asm__ volatile(
        ".word 0x48690083\n\t"
        "move %[o], $t1\n\t"
        : [o] "=&r"(back)
        :
        : "t1", "memory");
    return back;
}

static int hle_run(void)
{
    /* Traps off before the first VFPU cell, as in the vfpu-compare case. */
    __asm__ volatile("ctc1 $0, $31" ::: "memory");
    for (unsigned pass = 0; pass < CM_PASSES; pass++) {
        char name[64];
        for (unsigned cond = 0; cond < 16u; cond++) {
            cm_cmp_group(pass, cond);
        }
        for (unsigned f = 0; f < 2u; f++) {
            for (unsigned imm = 0; imm < 7u; imm++) {
                cm_cmov_group(pass, f, imm);
            }
        }
        snprintf(name, sizeof(name), "cc-initial-p%u", pass);
        hle_step(name);
        uint32_t init[1];
        init[0] = cm_cc_read();
        hle_record(name, "PASS", 0x48690083u, init, 1u);
        snprintf(name, sizeof(name), "cc-roundtrip-p%u", pass);
        hle_step(name);
        uint32_t rt[CM_RT_VALUES];
        for (unsigned i = 0; i < CM_RT_VALUES; i++) {
            rt[i] = cm_cc_roundtrip(s_cm_rt[i]);
        }
        (void)cm_cc_roundtrip(0u);
        hle_record(name, "PASS", 0x48e90083u, rt, CM_RT_VALUES);
    }
    return 1;
}

#endif /* HLE_CASE_VFPU_CC_CMOV */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_INTR_CELLS

/* #325 interrupt and timer delivery cells (PSP-HLE-INTR-001), one launch, user mode:
   (a) a VBLANK sub-interrupt left pending while interrupts are suspended
       (sceKernelCpuSuspendIntr) for 20 ms and 50 ms: how many handler entries follow the
       resume, and when;
   (b) re-pending while the handler runs: one entry holds for 25 ms (longer than a frame);
       when do the next entries come;
   (c) VTimer and alarm handlers that return a reschedule interval, then 0: the scheduled,
       actual and system times of every call.
   Every row is sequence numbers and system-time low words relative to a reference time
   recorded in the same row. Waits are bounded; the handler is released and the timers
   deleted before the end. Interrupts are never suspended for longer than 50 ms, and the
   one long handler entry is 25 ms. */

#include <pspintrman.h>

#define IC_VBL_SLOTS 64u
static volatile uint32_t s_ic_vbl_count;
static volatile uint32_t s_ic_vbl_in[IC_VBL_SLOTS];
static volatile uint32_t s_ic_vbl_out[IC_VBL_SLOTS];
static volatile uint32_t s_ic_vbl_hold_us;

static int ic_vbl_handler(int sub, void *arg)
{
    (void)sub;
    (void)arg;
    const uint32_t n = s_ic_vbl_count;
    const uint32_t t = sceKernelGetSystemTimeLow();
    if (n < IC_VBL_SLOTS) {
        s_ic_vbl_in[n] = t;
    }
    const uint32_t hold = s_ic_vbl_hold_us;
    if (hold != 0u) {
        s_ic_vbl_hold_us = 0u;
        while ((uint32_t)(sceKernelGetSystemTimeLow() - t) < hold) {
        }
    }
    if (n < IC_VBL_SLOTS) {
        s_ic_vbl_out[n] = sceKernelGetSystemTimeLow();
    }
    s_ic_vbl_count = n + 1u;
    return -1;
}

static void ic_vbl_reset(void)
{
    s_ic_vbl_count = 0u;
    for (unsigned i = 0; i < IC_VBL_SLOTS; i++) {
        s_ic_vbl_in[i] = 0u;
        s_ic_vbl_out[i] = 0u;
    }
}

static void ic_spin_us(uint32_t us)
{
    const uint32_t t0 = sceKernelGetSystemTimeLow();
    while ((uint32_t)(sceKernelGetSystemTimeLow() - t0) < us) {
    }
}

/* Waits (bounded, 100 ms) for the next handler entry, so a cell starts just after a frame. */
static void ic_vbl_align(void)
{
    const uint32_t c = s_ic_vbl_count;
    for (unsigned i = 0; i < 100u && s_ic_vbl_count == c; i++) {
        sceKernelDelayThread(1000);
    }
}

/* Row <name>: result = number of entries; out0..: entry times (and exit times when
   with_exit) minus ref, entries first..first+n. */
static void ic_vbl_times(const char *name, uint32_t ref, uint32_t first, uint32_t n, int with_exit)
{
    uint32_t w[16];
    uint32_t k = 0;
    for (uint32_t i = first; i < first + n && k < 16u; i++) {
        w[k++] = i < s_ic_vbl_count && i < IC_VBL_SLOTS ? s_ic_vbl_in[i] - ref : 0xffffffffu;
        if (with_exit && k < 16u) {
            w[k++] = i < s_ic_vbl_count && i < IC_VBL_SLOTS ? s_ic_vbl_out[i] - ref : 0xffffffffu;
        }
    }
    hle_record(name, "PASS", s_ic_vbl_count, w, k);
}

/* (a) Row vbl-masked-<ms>: result = entries during the suspension (should be 0); out0 =
   entries before; out1 = entries during; out2 = entries 200 us after the resume; out3 =
   entries 30 ms after the resume; out4 = suspension length (system time); out5 = the
   suspend return word; out6.. = entry times after the resume minus the resume time. */
static void ic_vbl_masked(const char *name, uint32_t us)
{
    hle_step(name);
    ic_vbl_align();
    const uint32_t before = s_ic_vbl_count;
    const unsigned int flags = sceKernelCpuSuspendIntr();
    const uint32_t ts = sceKernelGetSystemTimeLow();
    ic_spin_us(us);
    const uint32_t during = s_ic_vbl_count;
    const uint32_t tr = sceKernelGetSystemTimeLow();
    sceKernelCpuResumeIntr(flags);
    ic_spin_us(200u);
    const uint32_t after200 = s_ic_vbl_count;
    sceKernelDelayThread(30000);
    const uint32_t after30 = s_ic_vbl_count;
    uint32_t w[14] = {before, during, after200, after30, tr - ts, (uint32_t)flags};
    for (unsigned i = 0; i < 8u; i++) {
        const uint32_t e = during + i;
        w[6u + i] = e < after30 && e < IC_VBL_SLOTS ? s_ic_vbl_in[e] - tr : 0xffffffffu;
    }
    hle_record(name, "PASS", during - before, w, 14);
}

/* (c) VTimer and alarm handlers. Each call stores its sequence number, the scheduled and
   actual VTimer times (low words; alarms: 0) and the system time; it returns
   s_ic_ret[n] (0 ends the series). */
#define IC_TCALLS 8u
struct ic_tcall {
    volatile uint32_t sched;
    volatile uint32_t actual;
    volatile uint32_t sys;
};
static struct ic_tcall s_ic_tcall[IC_TCALLS];
static volatile uint32_t s_ic_tcount;
static uint32_t s_ic_ret[IC_TCALLS];

static SceUInt ic_vtimer_handler(SceUID uid, SceKernelSysClock *sched, SceKernelSysClock *actual, void *common)
{
    (void)uid;
    (void)common;
    const uint32_t n = s_ic_tcount;
    if (n < IC_TCALLS) {
        s_ic_tcall[n].sched = sched ? sched->low : 0xffffffffu;
        s_ic_tcall[n].actual = actual ? actual->low : 0xffffffffu;
        s_ic_tcall[n].sys = sceKernelGetSystemTimeLow();
    }
    s_ic_tcount = n + 1u;
    return n < IC_TCALLS ? s_ic_ret[n] : 0u;
}

static SceUInt ic_alarm_handler(void *common)
{
    (void)common;
    const uint32_t n = s_ic_tcount;
    if (n < IC_TCALLS) {
        s_ic_tcall[n].sched = 0u;
        s_ic_tcall[n].actual = 0u;
        s_ic_tcall[n].sys = sceKernelGetSystemTimeLow();
    }
    s_ic_tcount = n + 1u;
    return n < IC_TCALLS ? s_ic_ret[n] : 0u;
}

static void ic_tcalls_reset(const uint32_t *rets)
{
    s_ic_tcount = 0u;
    for (unsigned i = 0; i < IC_TCALLS; i++) {
        s_ic_tcall[i].sched = 0u;
        s_ic_tcall[i].actual = 0u;
        s_ic_tcall[i].sys = 0u;
        s_ic_ret[i] = rets[i];
    }
}

/* Rows <name>-call<i>: result = the call's sequence number; out0 = scheduled VTimer time
   low; out1 = actual VTimer time low; out2 = system time minus ref; out3 = the value that
   call returned. Row <name>: result = calls seen at the end; out0 = calls 150 ms after the
   series ended (a call after a return of 0 shows as a higher count); out1 = the set
   call's return; out2 = ref. */
static void ic_tcalls_rows(const char *name, uint32_t ref, int set_rc, uint32_t calls_at_end)
{
    char row[64];
    const uint32_t n = s_ic_tcount < IC_TCALLS ? s_ic_tcount : IC_TCALLS;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t w[4] = {s_ic_tcall[i].sched, s_ic_tcall[i].actual, s_ic_tcall[i].sys - ref, s_ic_ret[i]};
        snprintf(row, sizeof(row), "%s-call%u", name, (unsigned int)i);
        hle_record(row, "PASS", i + 1u, w, 4);
    }
    const uint32_t w[3] = {s_ic_tcount, (uint32_t)set_rc, ref};
    hle_record(name, "PASS", calls_at_end, w, 3);
}

static void ic_vtimer_series(const char *name, const uint32_t *rets, uint32_t first_us)
{
    hle_step(name);
    ic_tcalls_reset(rets);
    const SceUID vt = sceKernelCreateVTimer("ic_vt", NULL);
    if (vt < 0) {
        hle_record(name, "FAIL", (uint32_t)vt, NULL, 0);
        return;
    }
    (void)sceKernelStartVTimer(vt);
    SceKernelSysClock due;
    due.low = first_us;
    due.hi = 0u;
    const uint32_t ref = sceKernelGetSystemTimeLow();
    const int rc = sceKernelSetVTimerHandler(vt, &due, ic_vtimer_handler, NULL);
    sceKernelDelayThread(250000);
    const uint32_t at_end = s_ic_tcount;
    sceKernelDelayThread(150000);
    const uint32_t later = s_ic_tcount;
    (void)sceKernelStopVTimer(vt);
    (void)sceKernelDeleteVTimer(vt);
    ic_tcalls_rows(name, ref, rc, at_end);
    const uint32_t w[1] = {later};
    char row[64];
    snprintf(row, sizeof(row), "%s-after-zero", name);
    hle_record(row, "PASS", later - at_end, w, 1);
}

static void ic_alarm_series(const char *name, const uint32_t *rets, uint32_t first_us)
{
    hle_step(name);
    ic_tcalls_reset(rets);
    const uint32_t ref = sceKernelGetSystemTimeLow();
    const SceUID alarm = sceKernelSetAlarm(first_us, ic_alarm_handler, NULL);
    sceKernelDelayThread(250000);
    const uint32_t at_end = s_ic_tcount;
    sceKernelDelayThread(150000);
    const uint32_t later = s_ic_tcount;
    if (alarm >= 0) {
        (void)sceKernelCancelAlarm(alarm);
    }
    ic_tcalls_rows(name, ref, alarm, at_end);
    const uint32_t w[1] = {later};
    char row[64];
    snprintf(row, sizeof(row), "%s-after-zero", name);
    hle_record(row, "PASS", later - at_end, w, 1);
}

static int hle_run(void)
{
    /* (c) first: timers need no interrupt handler of ours. */
    static const uint32_t rets_20ms[IC_TCALLS] = {20000u, 20000u, 20000u, 20000u, 20000u, 0u, 0u, 0u};
    static const uint32_t rets_small[IC_TCALLS] = {1u, 1u, 1u, 1u, 0u, 0u, 0u, 0u};
    static const uint32_t rets_mixed[IC_TCALLS] = {5000u, 30000u, 1000u, 0u, 0u, 0u, 0u, 0u};
    ic_vtimer_series("vtimer-ret-20ms", rets_20ms, 10000u);
    ic_vtimer_series("vtimer-ret-1us", rets_small, 10000u);
    ic_vtimer_series("vtimer-ret-mixed", rets_mixed, 10000u);
    ic_alarm_series("alarm-ret-20ms", rets_20ms, 10000u);
    ic_alarm_series("alarm-ret-1us", rets_small, 10000u);

    /* (a)/(b): the VBLANK sub-interrupt. */
    /* Sub-interrupt slot 0 of VBLANK is taken in the XMB context (RegisterSubIntrHandler answered
       0x80020067 there, measured 2026-10-24): take the first free slot of 0..15 and record it. */
    hle_step("vbl-register");
    ic_vbl_reset();
    int slot = -1, reg = -1;
    uint32_t tries[16];
    for (int s = 0; s < 16; s++) {
        tries[s] = 0u;
        if (slot < 0) {
            reg = sceKernelRegisterSubIntrHandler(PSP_VBLANK_INT, s, (void *)ic_vbl_handler, NULL);
            tries[s] = (uint32_t)reg;
            if (reg >= 0) {
                slot = s;
            }
        }
    }
    const int en = slot >= 0 ? sceKernelEnableSubIntr(PSP_VBLANK_INT, slot) : -1;
    {
        /* result = the slot used (0xffffffff: none); out0 = Enable's return; out1-16 = Register's
           return per slot tried (0 = not tried). */
        uint32_t w[17];
        w[0] = (uint32_t)en;
        for (int s = 0; s < 16; s++) {
            w[1 + s] = tries[s];
        }
        hle_record("vbl-register", slot >= 0 && en >= 0 ? "PASS" : "FAIL", (uint32_t)slot, w, 17);
    }
    if (slot >= 0 && en >= 0) {
        hle_step("vbl-free");
        ic_vbl_align();
        ic_vbl_reset();
        const uint32_t ref = sceKernelGetSystemTimeLow();
        sceKernelDelayThread(200000);
        ic_vbl_times("vbl-free", ref, 0u, 12u, 0);

        ic_vbl_masked("vbl-masked-20ms", 20000u);
        ic_vbl_masked("vbl-masked-50ms", 50000u);
        ic_vbl_masked("vbl-masked-20ms-again", 20000u);

        hle_step("vbl-hold-25ms");
        ic_vbl_align();
        ic_vbl_reset();
        const uint32_t ref2 = sceKernelGetSystemTimeLow();
        s_ic_vbl_hold_us = 25000u;
        sceKernelDelayThread(200000);
        ic_vbl_times("vbl-hold-25ms", ref2, 0u, 8u, 1);
    }
    /* A registered handler is released on every path, including the one where Enable
       refused the slot (no cell ran then, so only this record follows vbl-register). */
    if (slot >= 0) {
        hle_step("vbl-release");
        const int dis = sceKernelDisableSubIntr(PSP_VBLANK_INT, slot);
        const int rel = sceKernelReleaseSubIntrHandler(PSP_VBLANK_INT, slot);
        const uint32_t w[2] = {(uint32_t)dis, (uint32_t)rel};
        hle_record("vbl-release", "PASS", (uint32_t)rel, w, 2);
    }
    return 1;
}

#endif /* HLE_CASE_INTR_CELLS */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_INTR_CAUSE

/* #325 COP0 interrupt state (PSP-INTR-CAUSE-001), one launch. The questions are the LLE
   INTC model's (src/rt/cpu_lle.h): which Cause.IP bit the interrupt controller drives,
   whether a VBLANK left pending while interrupts are suspended shows there, and what
   sceKernelCpuSuspendIntr changes (Status or the Allegrex interrupt-enable word).
   Each snapshot is one kuKernelCall of ca_cop0_read, which runs in kernel mode and only
   reads COP0 Cause ($13), Status ($12) and Count ($9) and the interrupt-enable word
   (mfic $0), plus the system time and sceDisplayGetVcount right after it.
   - cause-enabled-<i>: four snapshots with interrupts enabled, just after a VBLANK start;
   - per pass p (two passes): wait for a VBLANK start, suspend interrupts, take a snapshot
     every 1 ms for 24 ms (the next VBLANK falls inside), resume, take one more snapshot
     200 us later, wait 30 ms. Rows cause-masked-p<p>-t<NN>, cause-resumed-p<p> and the
     window summary cause-window-p<p>.
   Nothing is installed: no handler, timer or alarm. Interrupts are suspended for about
   24 ms per pass, below the 50 ms window hle-intr-cells measured. Every row is PASS: it
   records what was read. */

#include <kubridge.h>
#include <pspdisplay.h>

#define CA_SNAPS 25u
#define CA_STEP_US 1000u
#define CA_PASSES 2u

/* Kernel mode, through kuKernelCall with arg1 = four output words: Cause, Status, Count
   and the interrupt-enable word. Reads only; writes nothing but `out`. */
static int ca_cop0_read(uint32_t *out)
{
    uint32_t cause, status, count, ie;
    __asm__ volatile("mfc0 %0, $13\n\t"
                     "mfc0 %1, $12\n\t"
                     "mfc0 %2, $9\n\t"
                     "mfic %3, $0"
                     : "=r"(cause), "=r"(status), "=r"(count), "=r"(ie));
    out[0] = cause;
    out[1] = status;
    out[2] = count;
    out[3] = ie;
    return 0;
}

struct ca_snapshot {
    uint32_t cop0[4];
    uint32_t sys;
    uint32_t vcount;
    uint32_t rc;
};

/* One snapshot; cop0 keeps HLE_SENTINEL when the bridge did not run the reader. */
static void ca_snap(struct ca_snapshot *s)
{
    KernelCallArg arg;
    memset(&arg, 0, sizeof(arg));
    for (unsigned i = 0; i < 4u; i++) {
        s->cop0[i] = HLE_SENTINEL;
    }
    arg.arg1 = (uint32_t)s->cop0;
    s->rc = (uint32_t)kuKernelCall((void *)ca_cop0_read, &arg);
    s->sys = sceKernelGetSystemTimeLow();
    s->vcount = (uint32_t)sceDisplayGetVcount();
}

/* Row <name>: result = Cause; out0 = Status; out1 = Count; out2 = the interrupt-enable
   word; out3 = system time minus ref; out4 = sceDisplayGetVcount; out5 = kuKernelCall's
   return. */
static void ca_row(const char *name, const struct ca_snapshot *s, uint32_t ref)
{
    const uint32_t w[6] = {s->cop0[1], s->cop0[2], s->cop0[3], s->sys - ref, s->vcount, s->rc};
    hle_record(name, "PASS", s->cop0[0], w, 6);
}

static void ca_spin_until(uint32_t base, uint32_t us)
{
    while ((uint32_t)(sceKernelGetSystemTimeLow() - base) < us) {
    }
}

/* One suspended window. Row cause-window-p<p>: result = suspension length (system
   time); out0 = the suspend return word; out1 = vcount before the suspend; out2 = vcount
   at the resume; out3 = vcount 30 ms after the resume. */
static void ca_masked(unsigned pass)
{
    static struct ca_snapshot snaps[CA_SNAPS];
    struct ca_snapshot resumed;
    char name[48];
    snprintf(name, sizeof(name), "cause-window-p%u", pass);
    hle_step(name);
    sceDisplayWaitVblankStart();
    const uint32_t v0 = (uint32_t)sceDisplayGetVcount();
    const unsigned int flags = sceKernelCpuSuspendIntr();
    const uint32_t ts = sceKernelGetSystemTimeLow();
    for (unsigned k = 0; k < CA_SNAPS; k++) {
        ca_spin_until(ts, k * CA_STEP_US);
        ca_snap(&snaps[k]);
    }
    const uint32_t vr = (uint32_t)sceDisplayGetVcount();
    const uint32_t tr = sceKernelGetSystemTimeLow();
    sceKernelCpuResumeIntr(flags);
    ca_spin_until(tr, 200u);
    ca_snap(&resumed);
    sceKernelDelayThread(30000);
    const uint32_t v30 = (uint32_t)sceDisplayGetVcount();
    for (unsigned k = 0; k < CA_SNAPS; k++) {
        snprintf(name, sizeof(name), "cause-masked-p%u-t%02u", pass, k);
        ca_row(name, &snaps[k], ts);
    }
    snprintf(name, sizeof(name), "cause-resumed-p%u", pass);
    ca_row(name, &resumed, tr);
    const uint32_t w[4] = {(uint32_t)flags, v0, vr, v30};
    snprintf(name, sizeof(name), "cause-window-p%u", pass);
    hle_record(name, "PASS", tr - ts, w, 4);
}

static int hle_run(void)
{
    struct ca_snapshot enabled[4];
    char name[48];
    hle_step("cause-enabled");
    sceDisplayWaitVblankStart();
    const uint32_t ref = sceKernelGetSystemTimeLow();
    for (unsigned i = 0; i < 4u; i++) {
        ca_snap(&enabled[i]);
    }
    for (unsigned i = 0; i < 4u; i++) {
        snprintf(name, sizeof(name), "cause-enabled-%u", i);
        ca_row(name, &enabled[i], ref);
    }
    for (unsigned pass = 0; pass < CA_PASSES; pass++) {
        ca_masked(pass);
    }
    return 1;
}

#endif /* HLE_CASE_INTR_CAUSE */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_NULL_CODE_REGISTRATION

#include <pspdisplay.h>
#include <pspge.h>
#include <pspintrman.h>

/* Capture before a C prologue can change sp or gp. Each row is ra, sp, gp,
   a0-a3 and t0-t3, the eight integer argument registers in the PSP ABI. */
enum { KE_THREAD, KE_CALLBACK, KE_ALARM, KE_VTIMER, KE_VBLANK, KE_ENTRY_COUNT };
static int ke_null_code_registration(void);
static volatile uint32_t s_ke_regs[KE_ENTRY_COUNT][11];
static volatile uint32_t s_ke_seen[KE_ENTRY_COUNT];

#define KE_ENTRY_STUB(name, body, offset) \
    __asm__(".set push\n\t" \
            ".set noreorder\n\t" \
            ".text\n\t" \
            ".align 2\n\t" \
            ".globl " #name "\n\t" \
            #name ":\n\t" \
            "la $12, s_ke_regs\n\t" \
            "addiu $12, $12, " #offset "\n\t" \
            "sw $31, 0($12)\n\t" \
            "sw $29, 4($12)\n\t" \
            "sw $28, 8($12)\n\t" \
            "sw $4, 12($12)\n\t" \
            "sw $5, 16($12)\n\t" \
            "sw $6, 20($12)\n\t" \
            "sw $7, 24($12)\n\t" \
            "sw $8, 28($12)\n\t" \
            "sw $9, 32($12)\n\t" \
            "sw $10, 36($12)\n\t" \
            "sw $11, 40($12)\n\t" \
            "j " #body "\n\t" \
            "nop\n\t" \
            ".set pop\n\t");

extern int ke_thread_entry(SceSize args, void *argp);
extern int ke_callback_entry(int notify_count, int notify_arg, void *common);
extern SceUInt ke_alarm_entry(void *common);
extern SceUInt ke_vtimer_entry(SceUID uid, SceKernelSysClock *scheduled,
                               SceKernelSysClock *actual, void *common);
extern int ke_vblank_entry(int sub, void *arg);

KE_ENTRY_STUB(ke_thread_entry, ke_thread_entry_body, 0)
KE_ENTRY_STUB(ke_callback_entry, ke_callback_entry_body, 44)
KE_ENTRY_STUB(ke_alarm_entry, ke_alarm_entry_body, 88)
KE_ENTRY_STUB(ke_vtimer_entry, ke_vtimer_entry_body, 132)
KE_ENTRY_STUB(ke_vblank_entry, ke_vblank_entry_body, 176)

__attribute__((used, noinline)) static int ke_thread_entry_body(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    s_ke_seen[KE_THREAD]++;
    return 0x13579bdf;
}

__attribute__((used, noinline)) static int ke_callback_entry_body(
    int notify_count, int notify_arg, void *common)
{
    (void)notify_count;
    (void)notify_arg;
    (void)common;
    s_ke_seen[KE_CALLBACK]++;
    return 0;
}

__attribute__((used, noinline)) static SceUInt ke_alarm_entry_body(void *common)
{
    (void)common;
    s_ke_seen[KE_ALARM]++;
    return 0u;
}

__attribute__((used, noinline)) static SceUInt ke_vtimer_entry_body(
    SceUID uid, SceKernelSysClock *scheduled, SceKernelSysClock *actual, void *common)
{
    (void)uid;
    (void)scheduled;
    (void)actual;
    (void)common;
    s_ke_seen[KE_VTIMER]++;
    return 0u;
}

__attribute__((used, noinline)) static int ke_vblank_entry_body(int sub, void *arg)
{
    (void)sub;
    (void)arg;
    s_ke_seen[KE_VBLANK]++;
    return -1;
}

static void ke_record_entry(const char *case_id, unsigned row)
{
    uint32_t words[11];
    for (unsigned i = 0; i < 11u; i++) {
        words[i] = s_ke_regs[row][i];
    }
    hle_record(case_id, "PASS", s_ke_seen[row], words, 11u);
}

static int hle_run(void)
{
    const uint32_t thread_arg = 0x13579bdfu;
    SceKernelThreadInfo thread_info;
    memset(&thread_info, 0, sizeof(thread_info));
    thread_info.size = sizeof(thread_info);

    hle_step("thread-entry-create");
    const SceUID thread = sceKernelCreateThread(
        "ke_entry", ke_thread_entry, 0x20, 0x1000, THREAD_ATTR_USER, NULL);
    int thread_start = -1, thread_wait = -1, thread_refer = -1, thread_delete = -1;
    uint32_t thread_exit = 0xffffffffu;
    if (thread >= 0) {
        hle_step("thread-entry-start");
        thread_start = sceKernelStartThread((SceUID)thread, sizeof(thread_arg),
                                            (void *)&thread_arg);
        if (thread_start >= 0) {
            SceUInt timeout = 300000u;
            hle_step("thread-entry-wait");
            thread_wait = sceKernelWaitThreadEnd((SceUID)thread, &timeout);
        }
        thread_refer = sceKernelReferThreadStatus((SceUID)thread, &thread_info);
        if (thread_refer >= 0) {
            thread_exit = (uint32_t)thread_info.exitStatus;
        }
        hle_step("thread-entry-delete");
        thread_delete = sceKernelDeleteThread((SceUID)thread);
    }
    ke_record_entry("thread-entry", KE_THREAD);
    const uint32_t thread_setup[6] = {
        (uint32_t)thread, (uint32_t)thread_start, (uint32_t)thread_wait,
        (uint32_t)thread_refer, thread_exit, (uint32_t)thread_delete,
    };
    hle_record("thread-entry-setup", "PASS", s_ke_seen[KE_THREAD], thread_setup, 6u);

    hle_step("callback-entry-create");
    const SceUID callback = sceKernelCreateCallback(
        "ke_entry", ke_callback_entry, (void *)(uintptr_t)0x2468ace0u);
    int callback_notify = -1, callback_check = -1, callback_delete = -1;
    if (callback >= 0) {
        hle_step("callback-entry-notify");
        callback_notify = sceKernelNotifyCallback((SceUID)callback, 0x10203040);
        hle_step("callback-entry-check");
        callback_check = sceKernelCheckCallback();
        hle_step("callback-entry-delete");
        callback_delete = sceKernelDeleteCallback((SceUID)callback);
    }
    ke_record_entry("callback-entry", KE_CALLBACK);
    const uint32_t callback_setup[4] = {
        (uint32_t)callback, (uint32_t)callback_notify,
        (uint32_t)callback_check, (uint32_t)callback_delete,
    };
    hle_record("callback-entry-setup", "PASS", s_ke_seen[KE_CALLBACK], callback_setup, 4u);

    hle_step("alarm-entry-set");
    const SceUID alarm = sceKernelSetAlarm(
        10000u, ke_alarm_entry, (void *)(uintptr_t)0x11223344u);
    if (alarm >= 0) {
        hle_step("alarm-entry-wait");
        sceKernelDelayThread(100000u);
    }
    int alarm_cancel = alarm >= 0 ? sceKernelCancelAlarm((SceUID)alarm) : -1;
    ke_record_entry("alarm-entry", KE_ALARM);
    const uint32_t alarm_setup[2] = {(uint32_t)alarm, (uint32_t)alarm_cancel};
    hle_record("alarm-entry-setup", "PASS", s_ke_seen[KE_ALARM], alarm_setup, 2u);

    hle_step("vtimer-entry-create");
    const SceUID vtimer = sceKernelCreateVTimer("ke_entry", NULL);
    int vtimer_start = -1, vtimer_set = -1, vtimer_stop = -1, vtimer_delete = -1;
    if (vtimer >= 0) {
        SceKernelSysClock due;
        due.low = 10000u;
        due.hi = 0u;
        hle_step("vtimer-entry-start");
        vtimer_start = sceKernelStartVTimer((SceUID)vtimer);
        if (vtimer_start >= 0) {
            hle_step("vtimer-entry-set");
            vtimer_set = sceKernelSetVTimerHandler(
                (SceUID)vtimer, &due, ke_vtimer_entry, (void *)(uintptr_t)0x55667788u);
            hle_step("vtimer-entry-wait");
            sceKernelDelayThread(100000u);
        }
        hle_step("vtimer-entry-stop");
        vtimer_stop = sceKernelStopVTimer((SceUID)vtimer);
        vtimer_delete = sceKernelDeleteVTimer((SceUID)vtimer);
    }
    ke_record_entry("vtimer-entry", KE_VTIMER);
    const uint32_t vtimer_setup[5] = {
        (uint32_t)vtimer, (uint32_t)vtimer_start, (uint32_t)vtimer_set,
        (uint32_t)vtimer_stop, (uint32_t)vtimer_delete,
    };
    hle_record("vtimer-entry-setup", "PASS", s_ke_seen[KE_VTIMER], vtimer_setup, 5u);

    hle_step("vblank-entry-register");
    int slot = -1, vblank_register = -1;
    uint32_t attempts = 0u;
    for (int candidate = 0; candidate < 16; candidate++) {
        attempts++;
        vblank_register = sceKernelRegisterSubIntrHandler(
            PSP_VBLANK_INT, candidate, (void *)ke_vblank_entry,
            (void *)(uintptr_t)0x89abcdefu);
        if (vblank_register >= 0) {
            slot = candidate;
            break;
        }
    }
    int vblank_enable = -1, vblank_disable = -1, vblank_release = -1;
    if (slot >= 0) {
        hle_step("vblank-entry-enable");
        vblank_enable = sceKernelEnableSubIntr(PSP_VBLANK_INT, slot);
        if (vblank_enable >= 0) {
            sceKernelDelayThread(50000u);
        }
        hle_step("vblank-entry-disable");
        vblank_disable = sceKernelDisableSubIntr(PSP_VBLANK_INT, slot);
        vblank_release = sceKernelReleaseSubIntrHandler(PSP_VBLANK_INT, slot);
    }
    ke_record_entry("vblank-subintr-entry", KE_VBLANK);
    const uint32_t vblank_setup[6] = {
        (uint32_t)vblank_register, (uint32_t)slot, attempts,
        (uint32_t)vblank_enable, (uint32_t)vblank_disable, (uint32_t)vblank_release,
    };
    hle_record("vblank-subintr-setup", "PASS", s_ke_seen[KE_VBLANK], vblank_setup, 6u);
    return ke_null_code_registration();
}

static volatile int s_null_ge_drawsync_rc = -1;
static volatile int s_null_ge_drawsync_returned;

static int null_ge_drawsync_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    s_null_ge_drawsync_rc = sceGeDrawSync(0);
    s_null_ge_drawsync_returned = 1;
    return 0;
}

static int null_ge_drawsync_bounded(uint32_t out[6])
{
    hle_step("ge-drawsync-thread-create");
    const SceUID thread = sceKernelCreateThread(
        "null_ge_sync", null_ge_drawsync_thread, 0x20, 0x1000, THREAD_ATTR_USER, NULL);
    int start_rc = -1, wait_rc = -1, terminate_delete_rc = -1, delete_rc = -1;
    if (thread >= 0) {
        hle_step("ge-drawsync-thread-start");
        start_rc = sceKernelStartThread((SceUID)thread, 0u, NULL);
        if (start_rc >= 0) {
            SceUInt timeout = 200000u;
            hle_step("ge-drawsync-wait-bounded");
            wait_rc = sceKernelWaitThreadEnd(thread, &timeout);
        } else {
            wait_rc = start_rc;
        }
        if (wait_rc < 0) {
            hle_step("ge-drawsync-terminate-timeout");
            terminate_delete_rc = sceKernelTerminateDeleteThread(thread);
        } else {
            delete_rc = sceKernelDeleteThread(thread);
        }
    }
    out[0] = (uint32_t)thread;
    out[1] = (uint32_t)start_rc;
    out[2] = (uint32_t)wait_rc;
    out[3] = (uint32_t)terminate_delete_rc;
    out[4] = (uint32_t)delete_rc;
    out[5] = s_null_ge_drawsync_returned
                 ? (uint32_t)s_null_ge_drawsync_rc : 0xffffffffu;
    return s_null_ge_drawsync_returned && wait_rc >= 0;
}

static int ke_null_code_registration(void)
{
    SceKernelThreadInfo thread_info;
    memset(&thread_info, 0, sizeof(thread_info));
    thread_info.size = sizeof(thread_info);
    hle_step("create-thread-null");
    const SceUID thread = sceKernelCreateThread(
        "null_entry", NULL, 0x20, 0x1000, THREAD_ATTR_USER, NULL);
    int thread_refer = -1, thread_delete = -1;
    uint32_t thread_entry = 0xffffffffu;
    if (thread >= 0) {
        thread_refer = sceKernelReferThreadStatus((SceUID)thread, &thread_info);
        if (thread_refer >= 0) {
            thread_entry = (uint32_t)(uintptr_t)thread_info.entry;
        }
        thread_delete = sceKernelDeleteThread((SceUID)thread);
    }
    const uint32_t thread_out[3] = {
        (uint32_t)thread_refer, thread_entry, (uint32_t)thread_delete,
    };
    hle_record("create-thread-null", "PASS", (uint32_t)thread, thread_out, 3u);

    SceKernelCallbackInfo callback_info;
    memset(&callback_info, 0, sizeof(callback_info));
    callback_info.size = sizeof(callback_info);
    hle_step("create-callback-null");
    const SceUID callback = sceKernelCreateCallback("null_entry", NULL, NULL);
    int callback_refer = -1, callback_delete = -1;
    uint32_t callback_entry = 0xffffffffu;
    if (callback >= 0) {
        callback_refer = sceKernelReferCallbackStatus((SceUID)callback, &callback_info);
        if (callback_refer >= 0) {
            callback_entry = (uint32_t)(uintptr_t)callback_info.callback;
        }
        callback_delete = sceKernelDeleteCallback((SceUID)callback);
    }
    const uint32_t callback_out[3] = {
        (uint32_t)callback_refer, callback_entry, (uint32_t)callback_delete,
    };
    hle_record("create-callback-null", "PASS", (uint32_t)callback, callback_out, 3u);

    hle_step("register-subintr-null");
    int slot = -1, register_rc = -1, release_rc = -1;
    uint32_t attempts = 0u;
    for (int candidate = 0; candidate < 16; candidate++) {
        attempts++;
        register_rc = sceKernelRegisterSubIntrHandler(
            PSP_VBLANK_INT, candidate, NULL, (void *)(uintptr_t)0x89abcdefu);
        if (register_rc >= 0) {
            slot = candidate;
            release_rc = sceKernelReleaseSubIntrHandler(PSP_VBLANK_INT, candidate);
            break;
        }
    }
    const uint32_t subintr_out[3] = {(uint32_t)slot, (uint32_t)release_rc, attempts};
    hle_record("register-subintr-null", "PASS", (uint32_t)register_rc, subintr_out, 3u);
    /* Never enable a NULL sub-interrupt handler: delivery runs in kernel interrupt context. */
    const uint32_t subintr_not_invoked[1] = {1u};
    hle_record("register-subintr-null-not-invoked", "SKIP", 0u, subintr_not_invoked, 1u);

    uint32_t draw_sync_out[6];
    const int draw_sync_complete = null_ge_drawsync_bounded(draw_sync_out);
    const int draw_sync = (int)s_null_ge_drawsync_rc;
    hle_record("ge-drawsync-idle", draw_sync_complete ? "PASS" : "SKIP",
               (uint32_t)draw_sync, draw_sync_out, 6u);
    PspGeCallbackData ge_callback;
    memset(&ge_callback, 0, sizeof(ge_callback));
    int ge_callback_id = -1;
    if (draw_sync_complete && draw_sync == 0) {
        hle_step("ge-set-callback-null");
        ge_callback_id = sceGeSetCallback(&ge_callback);
    }
    const int ge_unset = ge_callback_id >= 0 ? sceGeUnsetCallback(ge_callback_id) : -1;
    const uint32_t ge_out[2] = {(uint32_t)draw_sync, (uint32_t)ge_unset};
    hle_record("ge-set-callback-null", ge_callback_id >= 0 ? "PASS" : "SKIP",
               (uint32_t)(draw_sync_complete && draw_sync == 0
                              ? ge_callback_id : draw_sync), ge_out, 2u);
    /* No display list is queued: a GE finish callback would run in interrupt context. */
    const uint32_t ge_not_invoked[1] = {1u};
    hle_record("ge-set-callback-null-not-invoked", "SKIP", 0u, ge_not_invoked, 1u);
    return 1;
}

#endif /* HLE_CASE_NULL_CODE_REGISTRATION */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_NULL_THREAD_INVOCATION

static int hle_run(void)
{
    hle_step("null-thread-create-for-invocation");
    const SceUID thread = sceKernelCreateThread(
        "null_entry_run", NULL, 0x20, 0x1000, THREAD_ATTR_USER, NULL);
    if (thread < 0) {
        const uint32_t armed[2] = {(uint32_t)thread, 0xffffffffu};
        const uint32_t started[6] = {0xffffffffu, 0xffffffffu, 0xffffffffu,
                                     0xffffffffu, 0xffffffffu, 0xffffffffu};
        hle_record("null-thread-armed", "SKIP", (uint32_t)thread, armed, 2u);
        hle_record("null-thread-started", "SKIP", (uint32_t)thread, started, 6u);
        return 1;
    }

    const uint32_t armed[2] = {(uint32_t)thread, 0u};
    hle_record("null-thread-armed", "PASS", (uint32_t)thread, armed, 2u);
    hle_step("null-thread-start");
    const int start_rc = sceKernelStartThread((SceUID)thread, 0u, NULL);
    SceUInt timeout = 300000u;
    int wait_rc = start_rc < 0 ? start_rc : -1;
    if (start_rc >= 0) {
        hle_step("null-thread-wait");
        wait_rc = sceKernelWaitThreadEnd((SceUID)thread, &timeout);
    }
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    const int refer_rc = sceKernelReferThreadStatus((SceUID)thread, &info);
    int terminate_rc = -1;
    if (start_rc >= 0 && wait_rc < 0 && refer_rc >= 0 &&
        info.status != PSP_THREAD_STOPPED && info.status != PSP_THREAD_KILLED) {
        hle_step("null-thread-terminate-timeout");
        terminate_rc = sceKernelTerminateThread((SceUID)thread);
        SceUInt cleanup_timeout = 100000u;
        (void)sceKernelWaitThreadEnd((SceUID)thread, &cleanup_timeout);
    }
    hle_step("null-thread-delete");
    const int delete_rc = sceKernelDeleteThread((SceUID)thread);
    const uint32_t started[7] = {
        (uint32_t)start_rc, (uint32_t)wait_rc, (uint32_t)refer_rc,
        (uint32_t)info.status, (uint32_t)info.exitStatus,
        (uint32_t)terminate_rc, (uint32_t)delete_rc,
    };
    hle_record("null-thread-started", start_rc >= 0 ? "PASS" : "SKIP",
               (uint32_t)start_rc, started, 7u);
    return 1;
}

#endif /* HLE_CASE_NULL_THREAD_INVOCATION */

/* ------------------------------------------------------------------------- */
#if PSP_ORACLE_CASE == HLE_CASE_NULL_CALLBACK_INVOCATION

static volatile SceUID s_null_callback_uid = -1;
static volatile int s_null_callback_check_rc = -1;
static volatile int s_null_callback_check_returned;
static volatile int s_null_callback_worker_entered;

static int null_callback_worker(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    s_null_callback_worker_entered = 1;
    const SceUID callback = sceKernelCreateCallback("null_cb_run", NULL, NULL);
    s_null_callback_uid = callback;
    if (callback < 0) {
        const uint32_t armed[2] = {(uint32_t)callback, 0xffffffffu};
        hle_record("null-callback-armed", "SKIP", (uint32_t)callback, armed, 2u);
        return (int)callback;
    }
    hle_step("null-callback-notify");
    const int notify = sceKernelNotifyCallback((SceUID)callback, 0x10203040);
    const uint32_t armed[2] = {(uint32_t)callback, (uint32_t)notify};
    hle_record("null-callback-armed", notify >= 0 ? "PASS" : "SKIP",
               (uint32_t)callback, armed, 2u);
    if (notify < 0) {
        return notify;
    }
    hle_step("null-callback-check");
    const int check = sceKernelCheckCallback();
    s_null_callback_check_rc = check;
    s_null_callback_check_returned = 1;
    return check;
}

static int hle_run(void)
{
    hle_step("null-callback-worker-create");
    const SceUID thread = sceKernelCreateThread(
        "null_cb_worker", null_callback_worker, 0x20, 0x1000, THREAD_ATTR_USER, NULL);
    int start_rc = -1, wait_rc = -1, refer_rc = -1, delete_thread_rc = -1;
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (thread >= 0) {
        hle_step("null-callback-worker-start");
        start_rc = sceKernelStartThread((SceUID)thread, 0u, NULL);
        if (start_rc >= 0) {
            SceUInt timeout = 300000u;
            hle_step("null-callback-worker-wait");
            wait_rc = sceKernelWaitThreadEnd((SceUID)thread, &timeout);
        }
        refer_rc = sceKernelReferThreadStatus((SceUID)thread, &info);
        delete_thread_rc = sceKernelDeleteThread((SceUID)thread);
    }
    if (thread < 0 || !s_null_callback_worker_entered) {
        const uint32_t armed[2] = {0xffffffffu, 0xffffffffu};
        hle_record("null-callback-armed", "SKIP", (uint32_t)thread, armed, 2u);
    }
    int terminate_thread_rc = -1;
    if (thread >= 0 && start_rc >= 0 && wait_rc < 0 && refer_rc >= 0 &&
        info.status != PSP_THREAD_STOPPED && info.status != PSP_THREAD_KILLED) {
        hle_step("null-callback-worker-terminate-timeout");
        terminate_thread_rc = sceKernelTerminateThread((SceUID)thread);
        SceUInt cleanup_timeout = 100000u;
        (void)sceKernelWaitThreadEnd((SceUID)thread, &cleanup_timeout);
    }
    int delete_callback_rc = -1;
    if (s_null_callback_uid >= 0) {
        delete_callback_rc = sceKernelDeleteCallback((SceUID)s_null_callback_uid);
    }
    const uint32_t checked[7] = {
        s_null_callback_check_returned ? (uint32_t)s_null_callback_check_rc : 0xffffffffu,
        (uint32_t)wait_rc, (uint32_t)refer_rc, (uint32_t)info.exitStatus,
        (uint32_t)terminate_thread_rc, (uint32_t)delete_thread_rc,
        (uint32_t)delete_callback_rc,
    };
    hle_record("null-callback-checked",
               s_null_callback_check_returned ? "PASS" : "SKIP",
               (uint32_t)start_rc, checked, 7u);
    return 1;
}

#endif /* HLE_CASE_NULL_CALLBACK_INVOCATION */

/* ------------------------------------------------------------------------- */

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
