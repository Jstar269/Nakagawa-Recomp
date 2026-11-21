// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// Kernel semantics cells for the PSP-3000 oracle, one PRX and one launch:
//   CASE=kernel-semantics (PSP-KERNEL-SEM-001): #1287 VBLANK sub-interrupt delivery order,
//     #1390 controller sampling cells, #1383 volatile-lock types and allocations, #1380
//     exclusive create and 64-bit seeks in a probe-owned Memory Stick directory, and #1382
//     WLAN switch and power state. The console's MAC address and OpenPSID are never read.
//   CASE=io-seek-eof (PSP-IO-EOF-001): #1380 opens of a missing Memory Stick file without
//     PSP_O_CREAT, negative seeks, a write after a seek past the end (the size, the gap's
//     bytes and the tail) and a read past the end, in its own probe-owned directory.
//   CASE=io-umd-eof (PSP-IO-EOF-002): #1380 seeks past the end of UMD files (disc0:) and of
//     the raw UMD device (umd0:), reads there and across the end, negative seeks and seeks
//     past 32 bits. Read-only opens only; nothing is written to the Memory Stick or the disc,
//     and the UMD is never activated or changed (the build pins sceUmdUser to the two state
//     reads). Only counts, positions and return codes are recorded, never disc bytes.
//   CASE=safe-scalar (PSP-SAFE-SCALAR-001): #2640 cells the recompiled-game runtime stops at
//     by name or answers from an emulator: sceRtcCheckValid, sceRtcGetDayOfWeek,
//     sceRtcGetTime_t and sceRtcTickAddYears on out-of-range dates,
//     sceKernelGetThreadmanIdList for the thread event handler and alarm classes, and where
//     sceKernelCreateTlspl places a pool with and without attribute 0x4000. No device, dialog,
//     GE or radio call and no file but the host0 log; no handler is installed; an address is
//     recorded only as a difference from a block the probe allocated.
//
// These cells live apart from probe_hle_issues.c because they do what that file may not:
// they write and remove a Memory Stick file and directory, allocate partition memory, and
// change and restore the controller sampling mode and cycle.
//
// Record stream and log: the same writer, host0 round trip and module lifecycle as
// probe_hle_issues.c, from the shared probe_hle_harness.h (tools/test_psp_oracle.py pins
// the include). NAKAGAWA_PSP_META, a NAKAGAWA_PSP_STEP before each cell, one NAKAGAWA_PSP_TEST
// per measurement, a `kernel-semantics-done` terminal record whose out0 counts the
// measurements, the host0 round trip and a final NAKAGAWA_PSP_COMPLETE line.

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <pspintrman.h>
#include <pspctrl.h>
#include <pspsuspend.h>
#include <pspsysmem.h>
#include <pspwlan.h>
#if PSP_ORACLE_CASE == 137
#include <pspumd.h>
#endif
#if PSP_ORACLE_CASE == 141
#include <psprtc.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("PSP_HLE_MEASURE", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(64);

#ifndef PROBE_BUILD_COMMIT
#error PROBE_BUILD_COMMIT is required
#endif

#define KS_CASE_KERNEL_SEM 127
#define KS_CASE_IO_EOF 136
#define KS_CASE_IO_UMD_EOF 137
#define KS_CASE_SAFE_SCALAR 141
#if PSP_ORACLE_CASE == KS_CASE_KERNEL_SEM
#define HLE_CAMPAIGN_ID "kernel-semantics"
#define HLE_TEST_ID "PSP-KERNEL-SEM-001"
#define HLE_LOG "host0:/kernel_semantics_log.txt"
#elif PSP_ORACLE_CASE == KS_CASE_IO_EOF
#define HLE_CAMPAIGN_ID "io-seek-eof"
#define HLE_TEST_ID "PSP-IO-EOF-001"
#define HLE_LOG "host0:/io_seek_eof_log.txt"
#elif PSP_ORACLE_CASE == KS_CASE_IO_UMD_EOF
#define HLE_CAMPAIGN_ID "io-umd-eof"
#define HLE_TEST_ID "PSP-IO-EOF-002"
#define HLE_LOG "host0:/io_umd_eof_log.txt"
#elif PSP_ORACLE_CASE == KS_CASE_SAFE_SCALAR
#define HLE_CAMPAIGN_ID "safe-scalar"
#define HLE_TEST_ID "PSP-SAFE-SCALAR-001"
#define HLE_LOG "host0:/safe_scalar_log.txt"
#else
#error probe_kernel_semantics.c has no case for this PSP_ORACLE_CASE
#endif
#define HLE_DONE_ID HLE_CAMPAIGN_ID "-done"

#define HLE_SENTINEL 0xA5A5A5A5u
/* The same value as text, for the assembly wrappers (IC_V0_WRAPPER). */
#define HLE_SENTINEL_HEX "0xA5A5A5A5"

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
/* Kernel semantics cells for v0.0.2 (PSP-KERNEL-SEM-001), one launch, user mode:
   (a) #1287: four VBLANK sub-interrupt handlers registered at once, in the order 3, 0, 2, 1:
       the order every VBLANK calls them in and the (no, arg) each receives;
   (b) #1390: a blocking controller read with no unread sample; a short read after several
       samples accumulated, and what the next read finds; peek counts; the analog bytes in
       digital and in analog sampling mode; the sample spacing at cycle 5555 and the wait of a
       blocking read there;
   (c) #1383: TryLock and Lock of the volatile memory with types other than 0 while it is free,
       and partition-5 and fixed-address allocations while it is free and while it is held;
   (d) #1380: sceIoOpen with PSP_O_CREAT | PSP_O_EXCL in a probe-owned Memory Stick directory,
       ms0:/NKKSEM00 (created and removed by the probe; every io row is SKIP when it already
       exists): under the missing directory, for a new file, for the existing file; then
       64-bit seeks past 4 GiB on that 16-byte file, with no write past its end;
   (e) #1382: the WLAN switch and power state. The MAC address and the OpenPSID are
       console-bound values and are never read (no sceWlanGetEtherAddr or sceOpenPSID import;
       the build-time gate pins sceWlanDrv to the two state calls).
   Every row is emitted on every run with a fixed word count; a word that does not apply is
   HLE_SENTINEL. Each cell restores what it changed: the sampling mode and cycle, the
   sub-interrupt handlers (disabled and released), the volatile lock (unlocked), every block
   (freed) and the Memory Stick directory (removed). */


#if PSP_ORACLE_CASE == KS_CASE_KERNEL_SEM
/* ---- (a) VBLANK sub-interrupt delivery order --------------------------------------------- */
#define KS_VBL_LOG 48u
#define KS_VBL_SUBS 4u
static const int k_ks_vbl_order[KS_VBL_SUBS] = { 3, 0, 2, 1 };
static volatile uint32_t s_ks_vbl_count;
static volatile uint32_t s_ks_vbl_word[KS_VBL_LOG];
static volatile uint32_t s_ks_vbl_time[KS_VBL_LOG];

static int ks_vbl_handler(int sub, void *arg)
{
    const uint32_t n = s_ks_vbl_count;
    if (n < KS_VBL_LOG) {
        s_ks_vbl_word[n] = ((uint32_t)sub << 16) | ((uint32_t)(uintptr_t)arg & 0xffffu);
        s_ks_vbl_time[n] = sceKernelGetSystemTimeLow();
    }
    s_ks_vbl_count = n + 1u;
    return -1;
}

/* Rows subintr-register (8 words: the four Register codes, then the four Enable codes, in
   registration order), subintr-order (result = calls; 24 words: the first 24 calls as
   sub << 16 | arg), subintr-times (24 words: those calls' times minus the first call's) and
   subintr-release (8 words: the four Disable codes, then the four Release codes). */
static void ks_subintr_order(void)
{
    uint32_t w[24];
    s_ks_vbl_count = 0u;
    for (unsigned i = 0; i < KS_VBL_LOG; i++) {
        s_ks_vbl_word[i] = HLE_SENTINEL;
        s_ks_vbl_time[i] = 0u;
    }
    hle_step("subintr-register");
    for (unsigned i = 0; i < KS_VBL_SUBS; i++) {
        const int no = k_ks_vbl_order[i];
        w[i] = (uint32_t)sceKernelRegisterSubIntrHandler(PSP_VBLANK_INT, no, (void *)ks_vbl_handler,
                                                         (void *)(uintptr_t)(0xa000u + (unsigned)no));
    }
    for (unsigned i = 0; i < KS_VBL_SUBS; i++) {
        w[4u + i] = (uint32_t)sceKernelEnableSubIntr(PSP_VBLANK_INT, k_ks_vbl_order[i]);
    }
    hle_record("subintr-register", "PASS", 0u, w, 8u);
    sceKernelDelayThread(70000);   /* about four VBLANKs */
    for (unsigned i = 0; i < KS_VBL_SUBS; i++) {
        w[i] = (uint32_t)sceKernelDisableSubIntr(PSP_VBLANK_INT, k_ks_vbl_order[i]);
    }
    for (unsigned i = 0; i < KS_VBL_SUBS; i++) {
        w[4u + i] = (uint32_t)sceKernelReleaseSubIntrHandler(PSP_VBLANK_INT, k_ks_vbl_order[i]);
    }
    const uint32_t calls = s_ks_vbl_count;
    uint32_t order[24], times[24];
    for (unsigned i = 0; i < 24u; i++) {
        const int have = i < calls && i < KS_VBL_LOG;
        order[i] = have ? s_ks_vbl_word[i] : HLE_SENTINEL;
        times[i] = have ? s_ks_vbl_time[i] - s_ks_vbl_time[0] : HLE_SENTINEL;
    }
    hle_record("subintr-order", "PASS", calls, order, 24u);
    hle_record("subintr-times", "PASS", calls, times, 24u);
    hle_record("subintr-release", "PASS", 0u, w, 8u);
}

/* ---- (b) controller sampling -------------------------------------------------------------- */
static SceCtrlData s_ks_pad[64];
static SceCtrlData s_ks_pad2[64];

static uint32_t ks_analog_word(const SceCtrlData *d)
{
    return (uint32_t)d->Lx | ((uint32_t)d->Ly << 8) | ((uint32_t)d->Rsrv[0] << 16) |
           ((uint32_t)d->Rsrv[1] << 24);
}

/* Row ctrl-empty-read (6 words): three times, a drain read of 64, then a read of 1: its
   result and the time it waited. */
static void ks_ctrl_empty_read(void)
{
    uint32_t w[6];
    hle_step("ctrl-empty-read");
    for (unsigned i = 0; i < 3u; i++) {
        (void)sceCtrlReadBufferPositive(s_ks_pad, 64);
        const uint32_t t0 = sceKernelGetSystemTimeLow();
        w[2u * i] = (uint32_t)sceCtrlReadBufferPositive(s_ks_pad, 1);
        w[2u * i + 1u] = sceKernelGetSystemTimeLow() - t0;
    }
    hle_record("ctrl-empty-read", "PASS", 0u, w, 6u);
}

/* Rows ctrl-short-read-peek (8 words: the time stamps of the newest eight samples a peek of 64
   reports, oldest first, minus the drain time), ctrl-short-read (3 words: the result of a read
   of 2 after a 100 ms pause, then its two time stamps minus the drain time) and
   ctrl-short-read-next (3 words: the next read of 64's result, how long it waited, and its
   first time stamp minus the drain time). */
static void ks_ctrl_short_read(void)
{
    uint32_t w[8];
    hle_step("ctrl-short-read");
    (void)sceCtrlReadBufferPositive(s_ks_pad, 64);
    const uint32_t t0 = sceKernelGetSystemTimeLow();
    sceKernelDelayThread(100000);
    const int peeked = sceCtrlPeekBufferPositive(s_ks_pad, 64);
    for (unsigned i = 0; i < 8u; i++) {
        w[i] = peeked == 64 ? s_ks_pad[56u + i].TimeStamp - t0 : HLE_SENTINEL;
    }
    hle_record("ctrl-short-read-peek", "PASS", (uint32_t)peeked, w, 8u);
    const int got = sceCtrlReadBufferPositive(s_ks_pad2, 2);
    w[0] = (uint32_t)got;
    w[1] = got >= 1 ? s_ks_pad2[0].TimeStamp - t0 : HLE_SENTINEL;
    w[2] = got >= 2 ? s_ks_pad2[1].TimeStamp - t0 : HLE_SENTINEL;
    hle_record("ctrl-short-read", "PASS", (uint32_t)got, w, 3u);
    const uint32_t t1 = sceKernelGetSystemTimeLow();
    const int next = sceCtrlReadBufferPositive(s_ks_pad2, 64);
    w[0] = (uint32_t)next;
    w[1] = sceKernelGetSystemTimeLow() - t1;
    w[2] = next >= 1 ? s_ks_pad2[0].TimeStamp - t0 : HLE_SENTINEL;
    hle_record("ctrl-short-read-next", "PASS", (uint32_t)next, w, 3u);
}

/* Row ctrl-peek-count (3 words): peeks of 64, 3 and 1 right after a read. */
static void ks_ctrl_peek_count(void)
{
    uint32_t w[3];
    hle_step("ctrl-peek-count");
    (void)sceCtrlReadBufferPositive(s_ks_pad, 64);
    w[0] = (uint32_t)sceCtrlPeekBufferPositive(s_ks_pad, 64);
    w[1] = (uint32_t)sceCtrlPeekBufferPositive(s_ks_pad, 3);
    w[2] = (uint32_t)sceCtrlPeekBufferPositive(s_ks_pad, 1);
    hle_record("ctrl-peek-count", "PASS", 0u, w, 3u);
}

/* Rows ctrl-mode-digital, ctrl-mode-analog and ctrl-mode-digital-again (9 words: the
   SetSamplingMode result, then the newest eight samples' Lx | Ly << 8 | Rsrv0 << 16 |
   Rsrv1 << 24, 50 ms after the change): digital, analog, then digital again, which tells a
   centred stick from the last analog position kept. The stick is not touched. The mode is
   restored afterwards. */
static void ks_ctrl_modes(void)
{
    uint32_t w[9];
    int mode = -1;
    hle_step("ctrl-mode");
    (void)sceCtrlGetSamplingMode(&mode);
    static const char *const rows[3] = { "ctrl-mode-digital", "ctrl-mode-analog",
                                         "ctrl-mode-digital-again" };
    for (int pass = 0; pass < 3; pass++) {
        w[0] = (uint32_t)sceCtrlSetSamplingMode(pass == 1 ? 1 : 0);
        sceKernelDelayThread(50000);
        const int peeked = sceCtrlPeekBufferPositive(s_ks_pad, 8);
        for (unsigned i = 0; i < 8u; i++) {
            w[1u + i] = peeked == 8 ? ks_analog_word(&s_ks_pad[i]) : HLE_SENTINEL;
        }
        hle_record(rows[pass], "PASS", (uint32_t)mode, w, 9u);
    }
    if (mode == 0 || mode == 1) {
        (void)sceCtrlSetSamplingMode(mode);
    }
}

/* Rows ctrl-cycle-5555 (13 words: the SetSamplingCycle result, then the spacing between the
   first thirteen consecutive samples a read of 64 returns 50 ms after a drain; result = that
   read's result) and ctrl-cycle-wait (3 words: three blocking reads of 1 after a drain at cycle
   5555, the time each waited). The cycle is restored afterwards. */
static void ks_ctrl_cycle(void)
{
    uint32_t w[13];
    int cycle = -1;
    hle_step("ctrl-cycle");
    (void)sceCtrlGetSamplingCycle(&cycle);
    w[0] = (uint32_t)sceCtrlSetSamplingCycle(5555);
    sceKernelDelayThread(20000);
    (void)sceCtrlReadBufferPositive(s_ks_pad, 64);
    sceKernelDelayThread(50000);
    const int got = sceCtrlReadBufferPositive(s_ks_pad, 64);
    for (unsigned i = 0; i < 12u; i++) {
        w[1u + i] = (int)(i + 1u) < got ? s_ks_pad[i + 1u].TimeStamp - s_ks_pad[i].TimeStamp
                                        : HLE_SENTINEL;
    }
    hle_record("ctrl-cycle-5555", "PASS", (uint32_t)got, w, 13u);
    uint32_t waits[3];
    for (unsigned i = 0; i < 3u; i++) {
        (void)sceCtrlReadBufferPositive(s_ks_pad, 64);
        const uint32_t t0 = sceKernelGetSystemTimeLow();
        (void)sceCtrlReadBufferPositive(s_ks_pad, 1);
        waits[i] = sceKernelGetSystemTimeLow() - t0;
    }
    hle_record("ctrl-cycle-wait", "PASS", 0u, waits, 3u);
    if (cycle >= 0) {
        (void)sceCtrlSetSamplingCycle(cycle);
    }
}

/* ---- (c) volatile memory lock ------------------------------------------------------------- */
#define KS_VMEM_BASE 0x08400000u

/* Rows vmem-trylock-type-<1|2|m1> and vmem-lock-type-1 (4 words: the call's result, the address
   and size outputs (HLE_SENTINEL when unwritten), and the Unlock result when the call locked,
   else HLE_SENTINEL). The lock is free before each. */
static void ks_vmem_type(const char *row, int lock, int type)
{
    uint32_t w[4];
    void *addr = (void *)(uintptr_t)HLE_SENTINEL;
    int size = (int)HLE_SENTINEL;
    hle_step(row);
    const int rc = lock ? sceKernelVolatileMemLock(type, &addr, &size)
                        : sceKernelVolatileMemTryLock(type, &addr, &size);
    w[0] = (uint32_t)rc;
    w[1] = (uint32_t)(uintptr_t)addr;
    w[2] = (uint32_t)size;
    w[3] = rc == 0 ? (uint32_t)sceKernelVolatileMemUnlock(0) : HLE_SENTINEL;
    hle_record(row, "PASS", (uint32_t)rc, w, 4u);
}

/* One allocation cell: words result (uid or code), block head address and free result. */
static void ks_alloc_cell(uint32_t *w, int partition, int type, SceSize size, void *addr)
{
    const SceUID uid = sceKernelAllocPartitionMemory(partition, "nkksem", type, size, addr);
    w[0] = (uint32_t)uid;
    w[1] = uid > 0 ? (uint32_t)(uintptr_t)sceKernelGetBlockHeadAddr(uid) : HLE_SENTINEL;
    w[2] = uid > 0 ? (uint32_t)sceKernelFreePartitionMemory(uid) : HLE_SENTINEL;
}

/* Rows vmem-alloc-free and vmem-alloc-held (6 words: a 4 KiB Low block of partition 5, then a
   4 KiB Addr block of partition 2 at 0x08400000, each as result, head address, free result),
   while the lock is free and while this thread holds it; vmem-alloc-held's result is the
   TryLock result and vmem-unlock records the Unlock. */
static void ks_vmem_alloc(void)
{
    uint32_t w[6];
    hle_step("vmem-alloc-free");
    ks_alloc_cell(&w[0], 5, PSP_SMEM_Low, 0x1000, NULL);
    ks_alloc_cell(&w[3], 2, PSP_SMEM_Addr, 0x1000, (void *)(uintptr_t)KS_VMEM_BASE);
    hle_record("vmem-alloc-free", "PASS", 0u, w, 6u);
    hle_step("vmem-alloc-held");
    void *addr = NULL;
    int size = 0;
    const int locked = sceKernelVolatileMemTryLock(0, &addr, &size);
    if (locked == 0) {
        ks_alloc_cell(&w[0], 5, PSP_SMEM_Low, 0x1000, NULL);
        ks_alloc_cell(&w[3], 2, PSP_SMEM_Addr, 0x1000, (void *)(uintptr_t)KS_VMEM_BASE);
    } else {
        for (unsigned i = 0; i < 6u; i++) w[i] = HLE_SENTINEL;
    }
    hle_record("vmem-alloc-held", "PASS", (uint32_t)locked, w, 6u);
    uint32_t u = locked == 0 ? (uint32_t)sceKernelVolatileMemUnlock(0) : HLE_SENTINEL;
    hle_record("vmem-unlock", "PASS", u, &u, 1u);
}

/* ---- (d) Memory Stick exclusive create and 64-bit seeks ------------------------------------ */
#define KS_DIR "ms0:/NKKSEM00"
#define KS_FILE KS_DIR "/f.bin"
#define KS_IO_ROWS 7u
static const char *const k_ks_io_rows[KS_IO_ROWS] = {
    "io-missing-parent", "io-mkdir", "io-excl-new", "io-excl-existing", "io-seek64",
    "io-seek64-rdwr", "io-cleanup",
};
static const uint32_t k_ks_io_words[KS_IO_ROWS] = { 4u, 1u, 3u, 5u, 16u, 4u, 3u };

static uint32_t ks_close_if_open(SceUID fd)
{
    return fd >= 0 ? (uint32_t)sceIoClose(fd) : HLE_SENTINEL;
}

static void ks_io(void)
{
    uint32_t w[16];
    hle_step("io-precheck");
    const SceUID pre = sceIoDopen(KS_DIR);
    if (pre >= 0) {
        /* The directory exists: it is not the probe's, so nothing below may touch it. */
        (void)sceIoDclose(pre);
        for (unsigned r = 0; r < KS_IO_ROWS; r++) {
            for (unsigned i = 0; i < k_ks_io_words[r]; i++) w[i] = HLE_SENTINEL;
            hle_record(k_ks_io_rows[r], "SKIP", (uint32_t)pre, w, k_ks_io_words[r]);
        }
        return;
    }

    /* io-missing-parent: O_CREAT|O_EXCL and O_CREAT alone under the missing directory, each
       as result and close result. */
    hle_step("io-missing-parent");
    SceUID fd = sceIoOpen(KS_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
    w[0] = (uint32_t)fd;
    w[1] = ks_close_if_open(fd);
    fd = sceIoOpen(KS_FILE, PSP_O_RDWR | PSP_O_CREAT, 0777);
    w[2] = (uint32_t)fd;
    w[3] = ks_close_if_open(fd);
    if ((int)w[0] >= 0 || (int)w[2] >= 0) {
        (void)sceIoRemove(KS_FILE);
    }
    hle_record("io-missing-parent", "PASS", 0u, w, 4u);

    hle_step("io-mkdir");
    const int made = sceIoMkdir(KS_DIR, 0777);
    w[0] = (uint32_t)made;
    hle_record("io-mkdir", "PASS", (uint32_t)made, w, 1u);
    if (made < 0) {
        for (unsigned r = 2; r < KS_IO_ROWS; r++) {
            for (unsigned i = 0; i < k_ks_io_words[r]; i++) w[i] = HLE_SENTINEL;
            hle_record(k_ks_io_rows[r], "SKIP", (uint32_t)made, w, k_ks_io_words[r]);
        }
        return;
    }

    /* io-excl-new: O_CREAT|O_EXCL for the new file, the 16-byte write and the close. */
    hle_step("io-excl-new");
    static const char bytes[16] = { 'n', 'a', 'k', 'a', 'g', 'a', 'w', 'a',
                                     '-', 'k', 's', 'e', 'm', '-', '0', '1' };
    fd = sceIoOpen(KS_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
    w[0] = (uint32_t)fd;
    w[1] = fd >= 0 ? (uint32_t)sceIoWrite(fd, bytes, sizeof(bytes)) : HLE_SENTINEL;
    w[2] = ks_close_if_open(fd);
    hle_record("io-excl-new", "PASS", (uint32_t)fd, w, 3u);

    /* io-excl-existing: O_CREAT|O_EXCL, O_CREAT|O_EXCL|O_TRUNC and O_EXCL without O_CREAT on
       the existing file (each opened descriptor closed at once), then the file's size words
       from sceIoGetstat (low, high). */
    hle_step("io-excl-existing");
    fd = sceIoOpen(KS_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
    w[0] = (uint32_t)fd;
    (void)ks_close_if_open(fd);
    fd = sceIoOpen(KS_FILE, PSP_O_RDWR | PSP_O_CREAT | PSP_O_EXCL | PSP_O_TRUNC, 0777);
    w[1] = (uint32_t)fd;
    (void)ks_close_if_open(fd);
    fd = sceIoOpen(KS_FILE, PSP_O_RDONLY | PSP_O_EXCL, 0777);
    w[2] = (uint32_t)fd;
    (void)ks_close_if_open(fd);
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    const int gs = sceIoGetstat(KS_FILE, &st);
    w[3] = gs >= 0 ? (uint32_t)((uint64_t)st.st_size & 0xffffffffu) : (uint32_t)gs;
    w[4] = gs >= 0 ? (uint32_t)((uint64_t)st.st_size >> 32) : HLE_SENTINEL;
    hle_record("io-excl-existing", "PASS", 0u, w, 5u);

    /* io-seek64 on a read-only descriptor: Lseek SET 0x100000010 (low, high), Lseek CUR 0
       (low, high), Lseek32 CUR 0, a read of 4 there, Lseek END +0x100000000 (low, high),
       Lseek SET 0xFFFFFFFF (low, high), a read of 4 there, Lseek SET 0 (low, high), Lseek32
       SET 0x7FFFFFFF, a read of 4 there, then the close. */
    hle_step("io-seek64");
    fd = sceIoOpen(KS_FILE, PSP_O_RDONLY, 0777);
    if (fd >= 0) {
        uint8_t buf[4];
        SceOff r = sceIoLseek(fd, (SceOff)0x100000010LL, PSP_SEEK_SET);
        w[0] = (uint32_t)((uint64_t)r & 0xffffffffu); w[1] = (uint32_t)((uint64_t)r >> 32);
        r = sceIoLseek(fd, 0, PSP_SEEK_CUR);
        w[2] = (uint32_t)((uint64_t)r & 0xffffffffu); w[3] = (uint32_t)((uint64_t)r >> 32);
        w[4] = (uint32_t)sceIoLseek32(fd, 0, PSP_SEEK_CUR);
        w[5] = (uint32_t)sceIoRead(fd, buf, sizeof(buf));
        r = sceIoLseek(fd, (SceOff)0x100000000LL, PSP_SEEK_END);
        w[6] = (uint32_t)((uint64_t)r & 0xffffffffu); w[7] = (uint32_t)((uint64_t)r >> 32);
        r = sceIoLseek(fd, (SceOff)0xFFFFFFFFLL, PSP_SEEK_SET);
        w[8] = (uint32_t)((uint64_t)r & 0xffffffffu); w[9] = (uint32_t)((uint64_t)r >> 32);
        w[10] = (uint32_t)sceIoRead(fd, buf, sizeof(buf));
        r = sceIoLseek(fd, 0, PSP_SEEK_SET);
        w[11] = (uint32_t)((uint64_t)r & 0xffffffffu); w[12] = (uint32_t)((uint64_t)r >> 32);
        w[13] = (uint32_t)sceIoLseek32(fd, 0x7fffffff, PSP_SEEK_SET);
        w[14] = (uint32_t)sceIoRead(fd, buf, sizeof(buf));
        w[15] = (uint32_t)sceIoClose(fd);
        hle_record("io-seek64", "PASS", (uint32_t)fd, w, 16u);
    } else {
        for (unsigned i = 0; i < 16u; i++) w[i] = HLE_SENTINEL;
        hle_record("io-seek64", "FAIL", (uint32_t)fd, w, 16u);
    }

    /* io-seek64-rdwr on a read-write descriptor (nothing is written): Lseek SET 0x100000010
       (low, high), sceIoGetstat size low afterwards, then the close. */
    hle_step("io-seek64-rdwr");
    fd = sceIoOpen(KS_FILE, PSP_O_RDWR, 0777);
    if (fd >= 0) {
        SceOff r = sceIoLseek(fd, (SceOff)0x100000010LL, PSP_SEEK_SET);
        w[0] = (uint32_t)((uint64_t)r & 0xffffffffu); w[1] = (uint32_t)((uint64_t)r >> 32);
        w[2] = (uint32_t)sceIoClose(fd);
        memset(&st, 0, sizeof(st));
        const int gs2 = sceIoGetstat(KS_FILE, &st);
        w[3] = gs2 >= 0 ? (uint32_t)((uint64_t)st.st_size & 0xffffffffu) : (uint32_t)gs2;
        hle_record("io-seek64-rdwr", "PASS", (uint32_t)fd, w, 4u);
    } else {
        for (unsigned i = 0; i < 4u; i++) w[i] = HLE_SENTINEL;
        hle_record("io-seek64-rdwr", "FAIL", (uint32_t)fd, w, 4u);
    }

    /* io-cleanup: remove the file and the directory, then confirm the directory is gone. */
    hle_step("io-cleanup");
    w[0] = (uint32_t)sceIoRemove(KS_FILE);
    w[1] = (uint32_t)sceIoRmdir(KS_DIR);
    const SceUID post = sceIoDopen(KS_DIR);
    w[2] = (uint32_t)post;
    if (post >= 0) {
        (void)sceIoDclose(post);
    }
    hle_record("io-cleanup", "PASS", 0u, w, 3u);
}

/* ---- (e) WLAN state ------------------------------------------------------------------------ */
/* Row wlan-state (2 words): sceWlanGetSwitchState, sceWlanDevIsPowerOn. */
static void ks_wlan(void)
{
    uint32_t w[2];
    hle_step("wlan-state");
    w[0] = (uint32_t)sceWlanGetSwitchState();
    w[1] = (uint32_t)sceWlanDevIsPowerOn();
    hle_record("wlan-state", "PASS", 0u, w, 2u);
}

#endif /* KS_CASE_KERNEL_SEM */

#if PSP_ORACLE_CASE == KS_CASE_IO_EOF
/* ---- io-seek-eof: #1380 open without O_CREAT, negative seeks, past-end write and read ----- */
/* One launch, user mode, Memory Stick only, inside ms0:/NKKSEM01 (created and removed by the
   probe; every row is SKIP when the directory already exists, so nothing outside the probe's
   own directory is touched). Every row has a fixed word count; a word that does not apply is
   HLE_SENTINEL. A 64-bit seek result is recorded as (low, high). */
#define KE_DIR "ms0:/NKKSEM01"
#define KE_FILE KE_DIR "/e.bin"
#define KE_MISSING KE_DIR "/missing.bin"
#define KE_SIZE 16u
#define KE_PAST 0x20u
#define KE_TAIL 4u
#define KE_READ 0x30u
#define KE_FILL 0xEEu
#define KE_ROWS 9u
static const char *const k_ke_rows[KE_ROWS] = {
    "io-eof-mkdir", "io-eof-missing", "io-eof-create", "io-eof-seek-negative",
    "io-eof-seek-negative-rdonly", "io-eof-write-past", "io-eof-gap", "io-eof-read-past",
    "io-eof-cleanup",
};
static const uint32_t k_ke_words[KE_ROWS] = { 1u, 5u, 3u, 16u, 5u, 8u, 8u, 5u, 3u };

static void ke_skip_from(unsigned first, uint32_t result)
{
    uint32_t w[16];
    for (unsigned r = first; r < KE_ROWS; r++) {
        for (unsigned i = 0; i < k_ke_words[r]; i++) w[i] = HLE_SENTINEL;
        hle_record(k_ke_rows[r], "SKIP", result, w, k_ke_words[r]);
    }
}

static void ke_off(uint32_t *w, SceOff r)
{
    w[0] = (uint32_t)((uint64_t)r & 0xffffffffu);
    w[1] = (uint32_t)((uint64_t)r >> 32);
}

static uint32_t ke_le_word(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

static void ke_io(void)
{
    uint32_t w[16];
    SceIoStat st;
    hle_step("io-eof-precheck");
    const SceUID pre = sceIoDopen(KE_DIR);
    if (pre >= 0) {
        /* The directory exists: it is not the probe's, so nothing below may touch it. */
        (void)sceIoDclose(pre);
        ke_skip_from(0u, (uint32_t)pre);
        return;
    }

    hle_step("io-eof-mkdir");
    const int made = sceIoMkdir(KE_DIR, 0777);
    w[0] = (uint32_t)made;
    hle_record("io-eof-mkdir", "PASS", (uint32_t)made, w, 1u);
    if (made < 0) {
        ke_skip_from(1u, (uint32_t)made);
        return;
    }

    /* io-eof-missing: a missing file in the probe's directory opened write-only, read-write,
       read-only and write-only with O_TRUNC, each without PSP_O_CREAT, then sceIoGetstat of
       the path (did any open create it). An unexpected descriptor is closed and the file
       removed. */
    hle_step("io-eof-missing");
    static const int missing_flags[4] = {
        PSP_O_WRONLY, PSP_O_RDWR, PSP_O_RDONLY, PSP_O_WRONLY | PSP_O_TRUNC,
    };
    int opened = 0;
    for (unsigned i = 0; i < 4u; i++) {
        const SceUID mfd = sceIoOpen(KE_MISSING, missing_flags[i], 0777);
        w[i] = (uint32_t)mfd;
        if (mfd >= 0) {
            (void)sceIoClose(mfd);
            opened = 1;
        }
    }
    memset(&st, 0, sizeof(st));
    w[4] = (uint32_t)sceIoGetstat(KE_MISSING, &st);
    if (opened || (int)w[4] >= 0) {
        (void)sceIoRemove(KE_MISSING);
    }
    hle_record("io-eof-missing", "PASS", 0u, w, 5u);

    /* io-eof-create: the 16-byte file with O_CREAT|O_EXCL, its write and close. */
    hle_step("io-eof-create");
    static const char bytes[KE_SIZE] = { 'n', 'a', 'k', 'a', 'g', 'a', 'w', 'a',
                                         '-', 'e', 'o', 'f', '-', '0', '0', '1' };
    SceUID fd = sceIoOpen(KE_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_EXCL, 0777);
    w[0] = (uint32_t)fd;
    w[1] = fd >= 0 ? (uint32_t)sceIoWrite(fd, bytes, sizeof(bytes)) : HLE_SENTINEL;
    w[2] = fd >= 0 ? (uint32_t)sceIoClose(fd) : HLE_SENTINEL;
    hle_record("io-eof-create", "PASS", (uint32_t)fd, w, 3u);
    if (fd < 0 || (int)w[1] != (int)KE_SIZE) {
        ke_skip_from(3u, (uint32_t)fd);
        (void)sceIoRemove(KE_FILE);
        (void)sceIoRmdir(KE_DIR);
        return;
    }

    /* io-eof-seek-negative on a read-write descriptor: Lseek SET 8 (low), Lseek SET -1, CUR 0,
       Lseek CUR -16, CUR 0, Lseek END -32, CUR 0 (each low, high), Lseek32 SET -1, Lseek32
       CUR 0, then the close. Nothing is written. */
    hle_step("io-eof-seek-negative");
    fd = sceIoOpen(KE_FILE, PSP_O_RDWR, 0777);
    if (fd >= 0) {
        w[0] = (uint32_t)sceIoLseek(fd, 8, PSP_SEEK_SET);
        ke_off(&w[1], sceIoLseek(fd, -1, PSP_SEEK_SET));
        ke_off(&w[3], sceIoLseek(fd, 0, PSP_SEEK_CUR));
        ke_off(&w[5], sceIoLseek(fd, -16, PSP_SEEK_CUR));
        ke_off(&w[7], sceIoLseek(fd, 0, PSP_SEEK_CUR));
        ke_off(&w[9], sceIoLseek(fd, -32, PSP_SEEK_END));
        ke_off(&w[11], sceIoLseek(fd, 0, PSP_SEEK_CUR));
        w[13] = (uint32_t)sceIoLseek32(fd, -1, PSP_SEEK_SET);
        w[14] = (uint32_t)sceIoLseek32(fd, 0, PSP_SEEK_CUR);
        w[15] = (uint32_t)sceIoClose(fd);
        hle_record("io-eof-seek-negative", "PASS", (uint32_t)fd, w, 16u);
    } else {
        for (unsigned i = 0; i < 16u; i++) w[i] = HLE_SENTINEL;
        hle_record("io-eof-seek-negative", "FAIL", (uint32_t)fd, w, 16u);
    }

    /* io-eof-seek-negative-rdonly: Lseek SET -1 and CUR 0 (each low, high), then the close. */
    hle_step("io-eof-seek-negative-rdonly");
    fd = sceIoOpen(KE_FILE, PSP_O_RDONLY, 0777);
    if (fd >= 0) {
        ke_off(&w[0], sceIoLseek(fd, -1, PSP_SEEK_SET));
        ke_off(&w[2], sceIoLseek(fd, 0, PSP_SEEK_CUR));
        w[4] = (uint32_t)sceIoClose(fd);
        hle_record("io-eof-seek-negative-rdonly", "PASS", (uint32_t)fd, w, 5u);
    } else {
        for (unsigned i = 0; i < 5u; i++) w[i] = HLE_SENTINEL;
        hle_record("io-eof-seek-negative-rdonly", "FAIL", (uint32_t)fd, w, 5u);
    }

    /* io-eof-write-past: on a read-write descriptor, Lseek SET 0x20 (low, high), the size
       before the write (sceIoGetstat low), a 4-byte write there, Lseek CUR 0 (low), the close,
       then the size after (low, high). */
    hle_step("io-eof-write-past");
    static const char tail[KE_TAIL] = { 'T', 'A', 'I', 'L' };
    fd = sceIoOpen(KE_FILE, PSP_O_RDWR, 0777);
    if (fd >= 0) {
        ke_off(&w[0], sceIoLseek(fd, KE_PAST, PSP_SEEK_SET));
        memset(&st, 0, sizeof(st));
        const int gs = sceIoGetstat(KE_FILE, &st);
        w[2] = gs >= 0 ? (uint32_t)((uint64_t)st.st_size & 0xffffffffu) : (uint32_t)gs;
        w[3] = (uint32_t)sceIoWrite(fd, tail, sizeof(tail));
        w[4] = (uint32_t)sceIoLseek(fd, 0, PSP_SEEK_CUR);
        w[5] = (uint32_t)sceIoClose(fd);
        memset(&st, 0, sizeof(st));
        const int gs2 = sceIoGetstat(KE_FILE, &st);
        w[6] = gs2 >= 0 ? (uint32_t)((uint64_t)st.st_size & 0xffffffffu) : (uint32_t)gs2;
        w[7] = gs2 >= 0 ? (uint32_t)((uint64_t)st.st_size >> 32) : HLE_SENTINEL;
        hle_record("io-eof-write-past", "PASS", (uint32_t)fd, w, 8u);
    } else {
        for (unsigned i = 0; i < 8u; i++) w[i] = HLE_SENTINEL;
        hle_record("io-eof-write-past", "FAIL", (uint32_t)fd, w, 8u);
    }

    /* io-eof-gap: read 0x30 bytes from offset 0 into a buffer filled with 0xEE: the read
       result, the gap bytes 16..31 as four little-endian words, the bytes 32..35 (the tail),
       Lseek CUR 0 (low) and the close. */
    hle_step("io-eof-gap");
    static uint8_t buf[KE_READ];
    memset(buf, KE_FILL, sizeof(buf));
    fd = sceIoOpen(KE_FILE, PSP_O_RDONLY, 0777);
    if (fd >= 0) {
        w[0] = (uint32_t)sceIoRead(fd, buf, sizeof(buf));
        for (unsigned i = 0; i < 4u; i++) w[1 + i] = ke_le_word(&buf[KE_SIZE + 4u * i]);
        w[5] = ke_le_word(&buf[KE_PAST]);
        w[6] = (uint32_t)sceIoLseek(fd, 0, PSP_SEEK_CUR);
        w[7] = (uint32_t)sceIoClose(fd);
        hle_record("io-eof-gap", "PASS", (uint32_t)fd, w, 8u);
    } else {
        for (unsigned i = 0; i < 8u; i++) w[i] = HLE_SENTINEL;
        hle_record("io-eof-gap", "FAIL", (uint32_t)fd, w, 8u);
    }

    /* io-eof-read-past on a read-only descriptor: Lseek SET 0x40 (low, high), a read of 4
       there, Lseek CUR 0 (low), then the close. */
    hle_step("io-eof-read-past");
    fd = sceIoOpen(KE_FILE, PSP_O_RDONLY, 0777);
    if (fd >= 0) {
        uint8_t four[4];
        ke_off(&w[0], sceIoLseek(fd, 0x40, PSP_SEEK_SET));
        w[2] = (uint32_t)sceIoRead(fd, four, sizeof(four));
        w[3] = (uint32_t)sceIoLseek(fd, 0, PSP_SEEK_CUR);
        w[4] = (uint32_t)sceIoClose(fd);
        hle_record("io-eof-read-past", "PASS", (uint32_t)fd, w, 5u);
    } else {
        for (unsigned i = 0; i < 5u; i++) w[i] = HLE_SENTINEL;
        hle_record("io-eof-read-past", "FAIL", (uint32_t)fd, w, 5u);
    }

    /* io-eof-cleanup: remove the file and the directory, then confirm the directory is gone. */
    hle_step("io-eof-cleanup");
    w[0] = (uint32_t)sceIoRemove(KE_FILE);
    w[1] = (uint32_t)sceIoRmdir(KE_DIR);
    const SceUID post = sceIoDopen(KE_DIR);
    w[2] = (uint32_t)post;
    if (post >= 0) {
        (void)sceIoDclose(post);
    }
    hle_record("io-eof-cleanup", "PASS", 0u, w, 3u);
}
#endif /* KS_CASE_IO_EOF */

#if PSP_ORACLE_CASE == KS_CASE_IO_UMD_EOF
/* ---- io-umd-eof: #1380 seeks and reads past the end of UMD files and the UMD device ----- */
/* One launch, user mode, read-only. Two files every game disc carries are opened PSP_O_RDONLY
   on disc0:, then the raw device umd0:. Every row is emitted on every run with a fixed word
   count; a word that does not apply is HLE_SENTINEL, and a file's rows are SKIP when its open
   fails. A 64-bit seek result is (low, high). Before each group of seeks the position is set
   to a known value (8 for a file, sector 16 for the device), so a refused seek's effect on the
   position is visible. A read records only its count and how many bytes of a buffer filled
   with 0xEE it changed, never the bytes. Raw reads ask for at most two 2048-byte sectors into
   a 32 KiB buffer (PSP-UMDRAW-001: a raw count is in sectors). */
#define KU_FILL 0xEEu
#define KU_BUF 0x8000u
#define KU_FILES 2u
#define KU_FILE_ROWS 5u
#define KU_ROWS (1u + KU_FILES * KU_FILE_ROWS + 3u + 1u)
#define KU_WORDS_MAX 16u
static const char *const k_ku_paths[KU_FILES] = {
    "disc0:/UMD_DATA.BIN", "disc0:/PSP_GAME/PARAM.SFO",
};
static const char *const k_ku_rows[KU_ROWS] = {
    "umd-eof-state-before",
    "umd-eof-a-open", "umd-eof-a-seek-past", "umd-eof-a-read-past", "umd-eof-a-seek-negative",
    "umd-eof-a-seek-wide",
    "umd-eof-b-open", "umd-eof-b-seek-past", "umd-eof-b-read-past", "umd-eof-b-seek-negative",
    "umd-eof-b-seek-wide",
    "umd-eof-raw-open", "umd-eof-raw-seek-past", "umd-eof-raw-read-end",
    "umd-eof-state-after",
};
static const uint32_t k_ku_words[KU_ROWS] = {
    2u, 4u, 12u, 10u, 9u, 10u, 4u, 12u, 10u, 9u, 10u, 3u, 16u, 13u, 2u,
};
static uint8_t s_ku_buf[KU_BUF] __attribute__((aligned(64)));

/* Words first .. first + count - 1 of a KU_WORDS_MAX-word row become HLE_SENTINEL. */
static void ku_sentinel(uint32_t *w, uint32_t first, uint32_t count)
{
    for (uint32_t i = 0; i < KU_WORDS_MAX; i++) {
        if (i >= first && i - first < count) w[i] = HLE_SENTINEL;
    }
}

static void ku_skip(unsigned row, uint32_t result)
{
    uint32_t w[KU_WORDS_MAX];
    ku_sentinel(w, 0u, k_ku_words[row]);
    hle_record(k_ku_rows[row], "SKIP", result, w, k_ku_words[row]);
}

static void ku_off(uint32_t *w, SceOff r)
{
    w[0] = (uint32_t)((uint64_t)r & 0xffffffffu);
    w[1] = (uint32_t)((uint64_t)r >> 32);
}

/* w[0] = the read's count, w[1] = how many buffer bytes it changed from KU_FILL. The buffer is
   written back and invalidated before the read and again after it. The second pass must write
   back: a disc0: read stores through the data cache, and a plain invalidate after it discarded
   those lines, so the 2026-11-04 capture of 11ec52e8 counted 0 changed bytes for file reads. */
static void ku_read(SceUID fd, SceSize count, uint32_t *w)
{
    memset(s_ku_buf, KU_FILL, sizeof(s_ku_buf));
    sceKernelDcacheWritebackInvalidateRange(s_ku_buf, sizeof(s_ku_buf));
    const int got = sceIoRead(fd, s_ku_buf, count);
    sceKernelDcacheWritebackInvalidateRange(s_ku_buf, sizeof(s_ku_buf));
    uint32_t changed = 0;
    for (uint32_t i = 0; i < KU_BUF; i++) changed += s_ku_buf[i] != KU_FILL;
    w[0] = (uint32_t)got;
    w[1] = changed;
}

static void ku_state(unsigned row)
{
    uint32_t w[2];
    hle_step(k_ku_rows[row]);
    w[0] = (uint32_t)sceUmdCheckMedium();
    w[1] = (uint32_t)sceUmdGetDriveStat();
    hle_record(k_ku_rows[row], "PASS", 0u, w, 2u);
}

static void ku_file(unsigned f)
{
    const unsigned base = 1u + f * KU_FILE_ROWS;
    uint32_t w[KU_WORDS_MAX];

    /* open: the descriptor, Lseek END 0 (low, high), which is the size, then Lseek SET 0. */
    hle_step(k_ku_rows[base]);
    const SceUID fd = sceIoOpen(k_ku_paths[f], PSP_O_RDONLY, 0);
    if (fd < 0) {
        ku_sentinel(w, 0u, 4u);
        w[0] = (uint32_t)fd;
        hle_record(k_ku_rows[base], "PASS", (uint32_t)fd, w, 4u);
        for (unsigned r = 1; r < KU_FILE_ROWS; r++) ku_skip(base + r, (uint32_t)fd);
        return;
    }
    const SceOff size = sceIoLseek(fd, 0, PSP_SEEK_END);
    w[0] = (uint32_t)fd;
    ku_off(&w[1], size);
    w[3] = (uint32_t)sceIoLseek(fd, 0, PSP_SEEK_SET);
    hle_record(k_ku_rows[base], "PASS", (uint32_t)fd, w, 4u);
    const int sized = size >= 4 && size < 0x10000000;

    /* seek-past: Lseek SET 8 (low); Lseek END +16, CUR 0; Lseek SET size + 0x1000, CUR 0 (each
       low, high); Lseek32 END +16, Lseek32 CUR 0; then whether the size was usable. */
    hle_step(k_ku_rows[base + 1u]);
    w[0] = (uint32_t)sceIoLseek(fd, 8, PSP_SEEK_SET);
    ku_off(&w[1], sceIoLseek(fd, 16, PSP_SEEK_END));
    ku_off(&w[3], sceIoLseek(fd, 0, PSP_SEEK_CUR));
    if (sized) {
        ku_off(&w[5], sceIoLseek(fd, size + 0x1000, PSP_SEEK_SET));
    } else {
        w[5] = w[6] = HLE_SENTINEL;
    }
    ku_off(&w[7], sceIoLseek(fd, 0, PSP_SEEK_CUR));
    w[9] = (uint32_t)sceIoLseek32(fd, 16, PSP_SEEK_END);
    w[10] = (uint32_t)sceIoLseek32(fd, 0, PSP_SEEK_CUR);
    w[11] = sized ? 1u : 0u;
    hle_record(k_ku_rows[base + 1u], "PASS", (uint32_t)fd, w, 12u);

    /* read-past: Lseek END +16 (low), a 16-byte read there (count, changed), Lseek CUR 0
       (low); Lseek END -4 (low), a 16-byte read across the end (count, changed), Lseek CUR 0
       (low); Lseek END 0 (low) and a 16-byte read at the end (count). */
    hle_step(k_ku_rows[base + 2u]);
    w[0] = (uint32_t)sceIoLseek(fd, 16, PSP_SEEK_END);
    ku_read(fd, 16u, &w[1]);
    w[3] = (uint32_t)sceIoLseek(fd, 0, PSP_SEEK_CUR);
    if (sized) {
        w[4] = (uint32_t)sceIoLseek(fd, -4, PSP_SEEK_END);
        ku_read(fd, 16u, &w[5]);
        w[7] = (uint32_t)sceIoLseek(fd, 0, PSP_SEEK_CUR);
    } else {
        ku_sentinel(w, 4u, 4u);
    }
    w[8] = (uint32_t)sceIoLseek(fd, 0, PSP_SEEK_END);
    {
        uint32_t at_end[2];
        ku_read(fd, 16u, at_end);
        w[9] = at_end[0];
    }
    hle_record(k_ku_rows[base + 2u], "PASS", (uint32_t)fd, w, 10u);

    /* seek-negative: Lseek SET 8 (low); Lseek SET -1 (low, high), CUR 0 (low); Lseek END
       -(size + 1) (low, high), CUR 0 (low); Lseek32 SET -1, Lseek32 CUR 0. */
    hle_step(k_ku_rows[base + 3u]);
    w[0] = (uint32_t)sceIoLseek(fd, 8, PSP_SEEK_SET);
    ku_off(&w[1], sceIoLseek(fd, -1, PSP_SEEK_SET));
    w[3] = (uint32_t)sceIoLseek(fd, 0, PSP_SEEK_CUR);
    if (sized) {
        ku_off(&w[4], sceIoLseek(fd, -(size + 1), PSP_SEEK_END));
    } else {
        w[4] = w[5] = HLE_SENTINEL;
    }
    w[6] = (uint32_t)sceIoLseek(fd, 0, PSP_SEEK_CUR);
    w[7] = (uint32_t)sceIoLseek32(fd, -1, PSP_SEEK_SET);
    w[8] = (uint32_t)sceIoLseek32(fd, 0, PSP_SEEK_CUR);
    hle_record(k_ku_rows[base + 3u], "PASS", (uint32_t)fd, w, 9u);

    /* seek-wide: Lseek SET 8 (low); Lseek SET 0xFFFFFFFF, CUR 0; Lseek SET 0x100000000, CUR 0
       (each low, high); then the close. */
    hle_step(k_ku_rows[base + 4u]);
    w[0] = (uint32_t)sceIoLseek(fd, 8, PSP_SEEK_SET);
    ku_off(&w[1], sceIoLseek(fd, (SceOff)0xFFFFFFFFu, PSP_SEEK_SET));
    ku_off(&w[3], sceIoLseek(fd, 0, PSP_SEEK_CUR));
    ku_off(&w[5], sceIoLseek(fd, (SceOff)0x100000000ll, PSP_SEEK_SET));
    ku_off(&w[7], sceIoLseek(fd, 0, PSP_SEEK_CUR));
    w[9] = (uint32_t)sceIoClose(fd);
    hle_record(k_ku_rows[base + 4u], "PASS", (uint32_t)fd, w, 10u);
}

static void ku_raw(void)
{
    const unsigned base = 1u + KU_FILES * KU_FILE_ROWS;
    uint32_t w[KU_WORDS_MAX];

    /* raw-open: umd0: PSP_O_RDONLY: the descriptor and Lseek END 0 (low, high), the last
       sector (PSP-UMDRAW-001). */
    hle_step(k_ku_rows[base]);
    const SceUID fd = sceIoOpen("umd0:", PSP_O_RDONLY, 0);
    if (fd < 0) {
        ku_sentinel(w, 0u, 3u);
        w[0] = (uint32_t)fd;
        hle_record(k_ku_rows[base], "PASS", (uint32_t)fd, w, 3u);
        ku_skip(base + 1u, (uint32_t)fd);
        ku_skip(base + 2u, (uint32_t)fd);
        return;
    }
    const SceOff last = sceIoLseek(fd, 0, PSP_SEEK_END);
    w[0] = (uint32_t)fd;
    ku_off(&w[1], last);
    hle_record(k_ku_rows[base], "PASS", (uint32_t)fd, w, 3u);
    const int sized = last > 16 && last < 0x10000000;

    /* raw-seek-past: Lseek SET 16 (low); Lseek SET last + 1 (low, high), CUR 0 (low); Lseek
       SET last + 2 (low, high), CUR 0 (low); Lseek END +1 (low, high), CUR 0 (low); Lseek SET
       0x100000000 (low, high), CUR 0 (low); Lseek32 SET last + 1, Lseek32 CUR 0; then whether
       the last sector was usable. */
    hle_step(k_ku_rows[base + 1u]);
    w[0] = (uint32_t)sceIoLseek(fd, 16, PSP_SEEK_SET);
    if (sized) {
        ku_off(&w[1], sceIoLseek(fd, last + 1, PSP_SEEK_SET));
        w[3] = (uint32_t)sceIoLseek(fd, 0, PSP_SEEK_CUR);
        ku_off(&w[4], sceIoLseek(fd, last + 2, PSP_SEEK_SET));
        w[6] = (uint32_t)sceIoLseek(fd, 0, PSP_SEEK_CUR);
    } else {
        ku_sentinel(w, 1u, 6u);
    }
    ku_off(&w[7], sceIoLseek(fd, 1, PSP_SEEK_END));
    w[9] = (uint32_t)sceIoLseek(fd, 0, PSP_SEEK_CUR);
    ku_off(&w[10], sceIoLseek(fd, (SceOff)0x100000000ll, PSP_SEEK_SET));
    w[12] = (uint32_t)sceIoLseek(fd, 0, PSP_SEEK_CUR);
    w[13] = sized ? (uint32_t)sceIoLseek32(fd, (int)(last + 1), PSP_SEEK_SET) : HLE_SENTINEL;
    w[14] = (uint32_t)sceIoLseek32(fd, 0, PSP_SEEK_CUR);
    w[15] = sized ? 1u : 0u;
    hle_record(k_ku_rows[base + 1u], "PASS", (uint32_t)fd, w, 16u);

    /* raw-read-end: Lseek SET last (low), a one-sector read (count, changed), CUR 0 (low);
       Lseek SET last (low), a two-sector read across the end (count, changed), CUR 0 (low);
       Lseek SET last + 1 (low), a one-sector read (count, changed), CUR 0 (low); the close. */
    hle_step(k_ku_rows[base + 2u]);
    if (sized) {
        w[0] = (uint32_t)sceIoLseek(fd, last, PSP_SEEK_SET);
        ku_read(fd, 1u, &w[1]);
        w[3] = (uint32_t)sceIoLseek(fd, 0, PSP_SEEK_CUR);
        w[4] = (uint32_t)sceIoLseek(fd, last, PSP_SEEK_SET);
        ku_read(fd, 2u, &w[5]);
        w[7] = (uint32_t)sceIoLseek(fd, 0, PSP_SEEK_CUR);
        w[8] = (uint32_t)sceIoLseek(fd, last + 1, PSP_SEEK_SET);
        ku_read(fd, 1u, &w[9]);
        w[11] = (uint32_t)sceIoLseek(fd, 0, PSP_SEEK_CUR);
    } else {
        ku_sentinel(w, 0u, 12u);
    }
    w[12] = (uint32_t)sceIoClose(fd);
    hle_record(k_ku_rows[base + 2u], "PASS", (uint32_t)fd, w, 13u);
}

static void ku_io(void)
{
    ku_state(0u);
    for (unsigned f = 0; f < KU_FILES; f++) ku_file(f);
    ku_raw();
    ku_state(KU_ROWS - 1u);
}
#endif /* KS_CASE_IO_UMD_EOF */

#if PSP_ORACLE_CASE == KS_CASE_SAFE_SCALAR
/* ---- safe-scalar: #2640 RTC edge dates, thread-list classes 9 and 10, TLS pool placement --- */
/* One launch, user mode. The cells are ones src/rt answers with a named stop (hle_rtc.c,
   hle_kernel_thread.c) or from an emulator (sceKernelCreateTlspl attribute 0x4000:
   "PPSSPP-derived, not PSP-measured"). Nothing here opens a device or a file other than the
   host0 log, starts a dialog, touches the GE or a radio, or installs a handler. Every pointer
   the kernel receives is a probe-owned buffer or a documented optional NULL, and every month
   stays within 0..24, so a table lookup by month cannot leave the module's own data. The order
   is by rising cost of a failure: date arithmetic, two list reads, then the pools.
   Every row is emitted on every run with a fixed word count; a word that does not apply is
   HLE_SENTINEL, and a row whose object was not obtained is SKIP. No address is recorded: an
   address appears only as a difference from a block the probe allocated at the low or the
   high end of the user partition's free space, or as its low byte. */
#define SS_PART 2
#define SS_BRACKET 0x100u
#define SS_TLS_BLOCK 0x100u
#define SS_TLS_BLOCKS 4u
#define SS_TLS_HIGHMEM 0x4000u
#define SS_TLS_INFO_WORDS 16u
#define SS_TLS_INFO_SIZE 60u
#define SS_ID_SLOTS 16u
#define SS_WORDS_MAX 8u
#define SS_THREAD_STACK 0x2000
#define SS_THREAD_PRIORITY 0x20
#define SS_THREAD_WAIT_US 1000000u
/* Tick 0 is 0001-01-01 00:00:00 and a tick is a microsecond: year 10000 begins 3652059 days
   in, year 10001 3652425 days in. */
#define SS_TICK_Y10000 315537897600000000ull
#define SS_TICK_Y10001 315569520000000000ull

/* The PSPSDK headers do not declare the TLS pool calls. Their import stubs are in
   threadman_user_imports.S (ThreadManForUser) and kernel_library_tls_imports.S
   (sceKernelGetTlsAddr, a Kernel_Library call). */
SceUID sceKernelCreateTlspl(const char *name, int partition, SceUInt attr, SceSize block_size,
                            SceUInt blocks, void *option);
int sceKernelDeleteTlspl(SceUID uid);
int sceKernelFreeTlspl(SceUID uid);
int sceKernelReferTlsplStatus(SceUID uid, void *info);
void *sceKernelGetTlsAddr(SceUID uid);

/* ---- (a) RTC: the dates src/rt/hle_rtc.c stops at ------------------------------------------ */
typedef struct {
    const char *row;
    uint16_t year, month, day, hour, minute, second;
} SsDate;

typedef struct {
    const char *row;
    int year, month, day;
} SsDow;

typedef struct {
    const char *row;
    uint64_t tick;
    int years;
} SsAdd;

/* sceRtcCheckValid: a day of 0, and a day past the month's length but not past 31. The last
   row also has hour 24, so the result shows which field is checked first. Words: year, month,
   day, hour as passed. */
#define SS_VALID_ROWS 7u
#define SS_VALID_WORDS 4u
static const SsDate k_ss_valid[SS_VALID_ROWS] = {
    { "rtc-valid-day-0", 2024, 1, 0, 0, 0, 0 },
    { "rtc-valid-feb-29-2023", 2023, 2, 29, 0, 0, 0 },
    { "rtc-valid-feb-29-1900", 1900, 2, 29, 0, 0, 0 },
    { "rtc-valid-feb-30-2024", 2024, 2, 30, 0, 0, 0 },
    { "rtc-valid-feb-31-2024", 2024, 2, 31, 0, 0, 0 },
    { "rtc-valid-apr-31-2024", 2024, 4, 31, 0, 0, 0 },
    { "rtc-valid-day-0-hour-24", 2024, 1, 0, 24, 0, 0 },
};

/* sceRtcGetDayOfWeek: a month of 0 or 13..24 (one triple twice, to see whether the answer
   repeats), a year of 0 and of 10000, and a day past 1000000. Words: year, month, day as
   passed. */
#define SS_DOW_ROWS 9u
#define SS_DOW_WORDS 3u
static const SsDow k_ss_dow[SS_DOW_ROWS] = {
    { "rtc-dow-month-0", 2026, 0, 1 },
    { "rtc-dow-month-13", 2026, 13, 1 },
    { "rtc-dow-month-13-again", 2026, 13, 1 },
    { "rtc-dow-month-14", 2026, 14, 15 },
    { "rtc-dow-month-24", 2026, 24, 1 },
    { "rtc-dow-year-0", 0, 1, 1 },
    { "rtc-dow-year-10000", 10000, 1, 1 },
    { "rtc-dow-day-1000001", 2026, 1, 1000001 },
    { "rtc-dow-day-max", 2026, 1, 0x7fffffff },
};

/* sceRtcGetTime_t: a date before 1970, the first second past 32 bits (2106-02-07 06:28:16),
   the last valid date, and one field out of range at a time. Words: the two output words,
   both preset to HLE_SENTINEL, so an unwritten output and a 64-bit store are both visible. */
#define SS_TIME_ROWS 11u
#define SS_TIME_WORDS 2u
static const SsDate k_ss_time[SS_TIME_ROWS] = {
    { "rtc-time-1969", 1969, 12, 31, 23, 59, 59 },
    { "rtc-time-1601", 1601, 1, 1, 0, 0, 0 },
    { "rtc-time-2106-past", 2106, 2, 7, 6, 28, 16 },
    { "rtc-time-9999", 9999, 12, 31, 23, 59, 59 },
    { "rtc-time-month-0", 2024, 0, 10, 0, 0, 0 },
    { "rtc-time-month-13", 2024, 13, 1, 0, 0, 0 },
    { "rtc-time-day-0", 2024, 1, 0, 0, 0, 0 },
    { "rtc-time-day-32", 2024, 1, 32, 0, 0, 0 },
    { "rtc-time-feb-30", 2024, 2, 30, 0, 0, 0 },
    { "rtc-time-hour-24", 2024, 1, 1, 24, 0, 0 },
    { "rtc-time-second-60", 2024, 1, 1, 0, 0, 60 },
};

/* sceRtcTickAddYears from a source tick past year 9999. Words: the destination's low and high
   words, both preset to HLE_SENTINEL, then the year count as passed. */
#define SS_ADD_ROWS 4u
#define SS_ADD_WORDS 3u
static const SsAdd k_ss_add[SS_ADD_ROWS] = {
    { "rtc-addyears-y10000-plus-0", SS_TICK_Y10000, 0 },
    { "rtc-addyears-y10000-plus-1", SS_TICK_Y10000, 1 },
    { "rtc-addyears-y10000-minus-1", SS_TICK_Y10000, -1 },
    { "rtc-addyears-y10001-minus-2", SS_TICK_Y10001, -2 },
};

static void ss_date(ScePspDateTime *out, const SsDate *in)
{
    memset(out, 0, sizeof(*out));
    out->year = in->year;
    out->month = in->month;
    out->day = in->day;
    out->hour = in->hour;
    out->minute = in->minute;
    out->second = in->second;
}

static void ss_rtc(void)
{
    uint32_t w[SS_WORDS_MAX];
    ScePspDateTime date;
    for (unsigned i = 0; i < SS_VALID_ROWS; i++) {
        const SsDate *in = &k_ss_valid[i];
        ss_date(&date, in);
        hle_step(in->row);
        const int rc = sceRtcCheckValid(&date);
        w[0] = in->year;
        w[1] = in->month;
        w[2] = in->day;
        w[3] = in->hour;
        hle_record(in->row, "PASS", (uint32_t)rc, w, SS_VALID_WORDS);
    }
    for (unsigned i = 0; i < SS_DOW_ROWS; i++) {
        const SsDow *in = &k_ss_dow[i];
        hle_step(in->row);
        const int rc = sceRtcGetDayOfWeek(in->year, in->month, in->day);
        w[0] = (uint32_t)in->year;
        w[1] = (uint32_t)in->month;
        w[2] = (uint32_t)in->day;
        hle_record(in->row, "PASS", (uint32_t)rc, w, SS_DOW_WORDS);
    }
    for (unsigned i = 0; i < SS_TIME_ROWS; i++) {
        const SsDate *in = &k_ss_time[i];
        ss_date(&date, in);
        w[0] = HLE_SENTINEL;
        w[1] = HLE_SENTINEL;
        hle_step(in->row);
        const int rc = sceRtcGetTime_t(&date, (void *)w);
        hle_record(in->row, "PASS", (uint32_t)rc, w, SS_TIME_WORDS);
    }
    for (unsigned i = 0; i < SS_ADD_ROWS; i++) {
        const SsAdd *in = &k_ss_add[i];
        uint64_t src = in->tick;
        uint64_t dest = ((uint64_t)HLE_SENTINEL << 32) | HLE_SENTINEL;
        hle_step(in->row);
        const int rc = sceRtcTickAddYears(&dest, &src, in->years);
        w[0] = (uint32_t)dest;
        w[1] = (uint32_t)(dest >> 32);
        w[2] = (uint32_t)in->years;
        hle_record(in->row, "PASS", (uint32_t)rc, w, SS_ADD_WORDS);
    }
}

/* ---- (b) sceKernelGetThreadmanIdList for the two classes src/rt has no table for ----------- */
/* The thread event handler class (9) and the alarm class (10), with nothing of either class
   created by the probe. Words: the count the call stored (preset to HLE_SENTINEL) and how many
   of the 16 slots it wrote. The identifiers themselves are not recorded. */
#define SS_TMID_WORDS 2u
static void ss_tmid_row(const char *row, enum SceKernelIdListType type)
{
    uint32_t ids[SS_ID_SLOTS];
    uint32_t w[SS_TMID_WORDS];
    int count = (int)HLE_SENTINEL;
    for (unsigned i = 0; i < SS_ID_SLOTS; i++) ids[i] = HLE_SENTINEL;
    hle_step(row);
    const int rc = sceKernelGetThreadmanIdList(type, (SceUID *)ids, (int)SS_ID_SLOTS, &count);
    uint32_t written = 0;
    for (unsigned i = 0; i < SS_ID_SLOTS; i++) written += ids[i] != HLE_SENTINEL;
    w[0] = (uint32_t)count;
    w[1] = written;
    hle_record(row, "PASS", (uint32_t)rc, w, SS_TMID_WORDS);
}

static void ss_tmid(void)
{
    ss_tmid_row("tmid-9-list", SCE_KERNEL_TMID_ThreadEventHandler);
    ss_tmid_row("tmid-10-list", SCE_KERNEL_TMID_Alarm);
}

/* ---- (c) TLS pools: where a pool lies with and without attribute 0x4000 ------------------- */
/* The first row holds how far the free memory of the user partition is from one run: its total
   free size minus its largest free block, 0 when it is one run. Two 0x100 blocks then bracket
   the free space, one from its low end and one from its high end. A pool of four 0x100 blocks
   is created without the attribute, and two with it. After each of the first two, a fresh
   0x100 block from each end shows which end moved (tls-place-*), and sceKernelGetTlsAddr gives
   the block's distance from both brackets (tls-addr-*). A second thread asks for its own block
   of the first two pools. Everything is released: the blocks the threads hold, the three
   pools, the brackets; the last row holds what the partition's free sizes differ by from the
   start.
   Every path emits all sixteen rows, each once and in this order: a row whose object was not
   obtained is SKIP with its words left at HLE_SENTINEL (tools/test_psp_oracle.py walks every
   path of these functions and counts). */
enum {
    SS_T_FRAGMENTATION, SS_T_BRACKETS, SS_T_CREATE_NORMAL, SS_T_PLACE_NORMAL, SS_T_ADDR_NORMAL,
    SS_T_STATUS_NORMAL, SS_T_CREATE_HIGH, SS_T_PLACE_HIGH, SS_T_ADDR_HIGH, SS_T_STATUS_HIGH,
    SS_T_CREATE_HIGH_2, SS_T_ADDR_HIGH_2, SS_T_THREAD, SS_T_FREE, SS_T_DELETE, SS_T_CLEANUP,
};
#define SS_TLS_ROWS 16u
static const char *const k_ss_tls_rows[SS_TLS_ROWS] = {
    "tls-fragmentation", "tls-brackets", "tls-create-normal", "tls-place-normal",
    "tls-addr-normal", "tls-status-normal", "tls-create-high", "tls-place-high",
    "tls-addr-high", "tls-status-high", "tls-create-high-2", "tls-addr-high-2", "tls-thread",
    "tls-free", "tls-delete", "tls-cleanup",
};
static const uint32_t k_ss_tls_words[SS_TLS_ROWS] = {
    1u, 7u, 2u, 6u, 6u, 8u, 2u, 6u, 6u, 8u, 2u, 6u, 6u, 3u, 3u, 4u,
};

typedef struct {
    SceUID uid;
    uint32_t head;
} SsBlock;

static SceUID s_ss_t_pool[2];
static volatile uint32_t s_ss_t_addr[2];
static volatile uint32_t s_ss_t_free[2];

/* 1 when the identifier was obtained, else the value the call returned. */
static uint32_t ss_obtained(SceUID uid)
{
    return uid > 0 ? 1u : (uint32_t)uid;
}

static void ss_fill(uint32_t *w)
{
    for (unsigned i = 0; i < SS_WORDS_MAX; i++) w[i] = HLE_SENTINEL;
}

static void ss_tls_rec(unsigned row, const char *status, uint32_t result, const uint32_t *w)
{
    hle_record(k_ss_tls_rows[row], status, result, w, k_ss_tls_words[row]);
}

static void ss_tls_skip_from(unsigned first)
{
    uint32_t w[SS_WORDS_MAX];
    ss_fill(w);
    for (unsigned r = first; r < SS_TLS_ROWS; r++) ss_tls_rec(r, "SKIP", 0u, w);
}

static SsBlock ss_block(const char *name, int type)
{
    SsBlock b;
    b.uid = sceKernelAllocPartitionMemory(SS_PART, name, type, SS_BRACKET, NULL);
    b.head = b.uid > 0 ? (uint32_t)(uintptr_t)sceKernelGetBlockHeadAddr(b.uid) : 0u;
    return b;
}

static uint32_t ss_block_free(const SsBlock *b)
{
    return b->uid > 0 ? (uint32_t)sceKernelFreePartitionMemory(b->uid) : HLE_SENTINEL;
}

/* Word: the partition's total free size minus its largest free block, before the probe
   allocates anything in it. 0 means the free memory is one run, so the two brackets are its
   two ends and the tls-place rows read as their comment says. Anything else means several
   runs (or that the two calls round differently): the tls-addr rows then decide where a pool
   lies and the tls-place rows are a cross-check only. A size difference, not an address. */
static void ss_tls_fragmentation(void)
{
    uint32_t w[SS_WORDS_MAX];
    ss_fill(w);
    hle_step(k_ss_tls_rows[SS_T_FRAGMENTATION]);
    const uint32_t total = (uint32_t)sceKernelTotalFreeMemSize();
    w[0] = total - (uint32_t)sceKernelMaxFreeMemSize();
    ss_tls_rec(SS_T_FRAGMENTATION, "PASS", 0u, w);
}

/* Result: 1 when the pool was created, else the error. Words: how much the partition's total
   and largest free sizes fell across the create. */
static SceUID ss_tls_create(unsigned row, const char *name, SceUInt attr)
{
    uint32_t w[SS_WORDS_MAX];
    ss_fill(w);
    hle_step(k_ss_tls_rows[row]);
    const uint32_t total = (uint32_t)sceKernelTotalFreeMemSize();
    const uint32_t max = (uint32_t)sceKernelMaxFreeMemSize();
    const SceUID uid = sceKernelCreateTlspl(name, SS_PART, attr, SS_TLS_BLOCK, SS_TLS_BLOCKS,
                                            NULL);
    w[0] = total - (uint32_t)sceKernelTotalFreeMemSize();
    w[1] = max - (uint32_t)sceKernelMaxFreeMemSize();
    ss_tls_rec(row, "PASS", ss_obtained(uid), w);
    return uid;
}

/* Words: whether each fresh block was obtained, the fresh low block's head minus the low
   bracket's, the high bracket's head minus the fresh high block's, and the two frees. With
   nothing new at an end the distance is one bracket, 0x100; a pool there adds its size. That
   reading needs the free memory to have been one run (tls-fragmentation word 0): a block
   asked from an end takes the first hole it fits, so with several runs a pool can lie at an
   end and still leave this distance at 0x100. The tls-addr rows then decide. */
static void ss_tls_place(unsigned row, SceUID pool, const SsBlock *lo, const SsBlock *hi)
{
    uint32_t w[SS_WORDS_MAX];
    ss_fill(w);
    if (pool <= 0) {
        ss_tls_rec(row, "SKIP", 0u, w);
        return;
    }
    hle_step(k_ss_tls_rows[row]);
    const SsBlock low = ss_block("nkssl2", PSP_SMEM_Low);
    const SsBlock high = ss_block("nkssh2", PSP_SMEM_High);
    w[0] = ss_obtained(low.uid);
    w[1] = ss_obtained(high.uid);
    if (low.uid > 0) {
        w[2] = low.head - lo->head;
    }
    if (high.uid > 0) {
        w[3] = hi->head - high.head;
    }
    w[4] = ss_block_free(&low);
    w[5] = ss_block_free(&high);
    ss_tls_rec(row, "PASS", 0u, w);
}

/* Words: whether an address came back, the address minus the low bracket's head, the high
   bracket's head minus the address, the address's low byte, whether a second call gave the
   same address, and the address minus that of the pool named in ref (0: none). */
static uint32_t ss_tls_addr(unsigned row, SceUID pool, const SsBlock *lo, const SsBlock *hi,
                            uint32_t ref)
{
    uint32_t w[SS_WORDS_MAX];
    ss_fill(w);
    if (pool <= 0) {
        ss_tls_rec(row, "SKIP", 0u, w);
        return 0u;
    }
    hle_step(k_ss_tls_rows[row]);
    const uint32_t addr = (uint32_t)(uintptr_t)sceKernelGetTlsAddr(pool);
    const uint32_t again = (uint32_t)(uintptr_t)sceKernelGetTlsAddr(pool);
    w[0] = addr != 0u;
    if (addr != 0u) {
        w[1] = addr - lo->head;
        w[2] = hi->head - addr;
        w[3] = addr & 0xffu;
        w[4] = again == addr;
        if (ref != 0u) {
            w[5] = addr - ref;
        }
    }
    ss_tls_rec(row, "PASS", 0u, w);
    return addr;
}

/* Words: the status structure's size word, then attr, index, block size, block count, free
   blocks and waiting threads, then the word after the 60 bytes asked for (preset to
   HLE_SENTINEL). The name is the probe's own and is not recorded.
   The buffer must hold more than the structure whatever the size word says: a PSP writes all
   60 bytes for any non-zero size word (pspautotests tests/threads/tls/refer.expected), so a
   shorter buffer would be overrun on the probe's stack. */
static void ss_tls_status(unsigned row, SceUID pool)
{
    uint32_t w[SS_WORDS_MAX];
    uint32_t info[SS_TLS_INFO_WORDS];
    _Static_assert(SS_TLS_INFO_SIZE == 60u, "SceKernelTlsplInfo is 60 bytes");
    _Static_assert(sizeof(info) >= SS_TLS_INFO_SIZE + sizeof(info[0]),
                   "the status buffer holds the 60 bytes the kernel writes and one guard word");
    ss_fill(w);
    if (pool <= 0) {
        ss_tls_rec(row, "SKIP", 0u, w);
        return;
    }
    for (unsigned i = 0; i < SS_TLS_INFO_WORDS; i++) info[i] = HLE_SENTINEL;
    info[0] = SS_TLS_INFO_SIZE;
    hle_step(k_ss_tls_rows[row]);
    const int rc = sceKernelReferTlsplStatus(pool, info);
    w[0] = info[0];
    for (unsigned i = 0; i < 6; i++) w[1 + i] = info[9 + i];
    w[7] = info[15];
    ss_tls_rec(row, "PASS", (uint32_t)rc, w);
}

/* The second thread: its own block of each pool, then that block freed. */
static int ss_tls_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    for (unsigned i = 0; i < 2; i++) {
        if (s_ss_t_pool[i] > 0) {
            s_ss_t_addr[i] = (uint32_t)(uintptr_t)sceKernelGetTlsAddr(s_ss_t_pool[i]);
            s_ss_t_free[i] = (uint32_t)sceKernelFreeTlspl(s_ss_t_pool[i]);
        }
    }
    sceKernelExitThread(0);
    return 0;
}

/* Result: sceKernelStartThread. Words: whether the thread was created, the bounded wait for
   its end (the start's own error when it did not start), its address in each of the two pools
   minus the main thread's, and its two frees. Each pool has four blocks and two threads ask,
   so no request can wait for a block. */
static void ss_tls_second_thread(SceUID normal, SceUID high, uint32_t addr_normal,
                                 uint32_t addr_high)
{
    uint32_t w[SS_WORDS_MAX];
    ss_fill(w);
    if (normal <= 0 && high <= 0) {
        ss_tls_rec(SS_T_THREAD, "SKIP", 0u, w);
        return;
    }
    s_ss_t_pool[0] = normal;
    s_ss_t_pool[1] = high;
    for (unsigned i = 0; i < 2; i++) {
        s_ss_t_addr[i] = 0u;
        s_ss_t_free[i] = HLE_SENTINEL;
    }
    hle_step(k_ss_tls_rows[SS_T_THREAD]);
    const SceUID thid = sceKernelCreateThread("nkss_tls", ss_tls_thread, SS_THREAD_PRIORITY,
                                              SS_THREAD_STACK, 0, NULL);
    w[0] = ss_obtained(thid);
    if (thid <= 0) {
        ss_tls_rec(SS_T_THREAD, "SKIP", 0u, w);
        return;
    }
    const int started = sceKernelStartThread(thid, 0, NULL);
    SceUInt timeout = SS_THREAD_WAIT_US;
    const int waited = started >= 0 ? sceKernelWaitThreadEnd(thid, &timeout) : started;
    w[1] = (uint32_t)waited;
    if (started < 0) {
        /* Never started: the thread is dormant and is deleted as it is. */
        (void)sceKernelDeleteThread(thid);
    } else if (waited >= 0) {
        if (s_ss_t_addr[0] != 0u && addr_normal != 0u) {
            w[2] = s_ss_t_addr[0] - addr_normal;
        }
        if (s_ss_t_addr[1] != 0u && addr_high != 0u) {
            w[3] = s_ss_t_addr[1] - addr_high;
        }
        w[4] = s_ss_t_free[0];
        w[5] = s_ss_t_free[1];
        (void)sceKernelDeleteThread(thid);
    } else {
        (void)sceKernelTerminateDeleteThread(thid);
    }
    ss_tls_rec(SS_T_THREAD, "PASS", (uint32_t)started, w);
}

static void ss_tls(void)
{
    uint32_t w[SS_WORDS_MAX];
    ss_tls_fragmentation();
    ss_fill(w);
    hle_step(k_ss_tls_rows[SS_T_BRACKETS]);
    const uint32_t total0 = (uint32_t)sceKernelTotalFreeMemSize();
    const uint32_t max0 = (uint32_t)sceKernelMaxFreeMemSize();
    const SsBlock lo = ss_block("nksslo", PSP_SMEM_Low);
    const SsBlock hi = ss_block("nksshi", PSP_SMEM_High);
    w[0] = ss_obtained(lo.uid);
    w[1] = ss_obtained(hi.uid);
    if (lo.uid > 0 && hi.uid > 0) {
        w[2] = hi.head - lo.head;
        w[3] = lo.head & 0xffu;
        w[4] = hi.head & 0xffu;
    }
    w[5] = total0 - (uint32_t)sceKernelTotalFreeMemSize();
    w[6] = max0 - (uint32_t)sceKernelMaxFreeMemSize();
    if (lo.uid <= 0 || hi.uid <= 0) {
        ss_tls_rec(SS_T_BRACKETS, "SKIP", 0u, w);
        (void)ss_block_free(&lo);
        (void)ss_block_free(&hi);
        ss_tls_skip_from(SS_T_CREATE_NORMAL);
        return;
    }
    ss_tls_rec(SS_T_BRACKETS, "PASS", 0u, w);

    const SceUID normal = ss_tls_create(SS_T_CREATE_NORMAL, "nktls0", 0u);
    ss_tls_place(SS_T_PLACE_NORMAL, normal, &lo, &hi);
    const uint32_t addr_normal = ss_tls_addr(SS_T_ADDR_NORMAL, normal, &lo, &hi, 0u);
    ss_tls_status(SS_T_STATUS_NORMAL, normal);

    const SceUID high = ss_tls_create(SS_T_CREATE_HIGH, "nktls1", SS_TLS_HIGHMEM);
    ss_tls_place(SS_T_PLACE_HIGH, high, &lo, &hi);
    const uint32_t addr_high = ss_tls_addr(SS_T_ADDR_HIGH, high, &lo, &hi, addr_normal);
    ss_tls_status(SS_T_STATUS_HIGH, high);

    const SceUID high2 = ss_tls_create(SS_T_CREATE_HIGH_2, "nktls2", SS_TLS_HIGHMEM);
    (void)ss_tls_addr(SS_T_ADDR_HIGH_2, high2, &lo, &hi, addr_high);

    ss_tls_second_thread(normal, high, addr_normal, addr_high);

    /* The main thread's block of each pool, then the pools, then the brackets. */
    ss_fill(w);
    hle_step(k_ss_tls_rows[SS_T_FREE]);
    w[0] = normal > 0 ? (uint32_t)sceKernelFreeTlspl(normal) : HLE_SENTINEL;
    w[1] = high > 0 ? (uint32_t)sceKernelFreeTlspl(high) : HLE_SENTINEL;
    w[2] = high2 > 0 ? (uint32_t)sceKernelFreeTlspl(high2) : HLE_SENTINEL;
    ss_tls_rec(SS_T_FREE, "PASS", 0u, w);

    ss_fill(w);
    hle_step(k_ss_tls_rows[SS_T_DELETE]);
    w[0] = normal > 0 ? (uint32_t)sceKernelDeleteTlspl(normal) : HLE_SENTINEL;
    w[1] = high > 0 ? (uint32_t)sceKernelDeleteTlspl(high) : HLE_SENTINEL;
    w[2] = high2 > 0 ? (uint32_t)sceKernelDeleteTlspl(high2) : HLE_SENTINEL;
    ss_tls_rec(SS_T_DELETE, "PASS", 0u, w);

    ss_fill(w);
    hle_step(k_ss_tls_rows[SS_T_CLEANUP]);
    w[0] = ss_block_free(&lo);
    w[1] = ss_block_free(&hi);
    w[2] = total0 - (uint32_t)sceKernelTotalFreeMemSize();
    w[3] = max0 - (uint32_t)sceKernelMaxFreeMemSize();
    ss_tls_rec(SS_T_CLEANUP, "PASS", 0u, w);
}
#endif /* KS_CASE_SAFE_SCALAR */

static int hle_run(void)
{
#if PSP_ORACLE_CASE == KS_CASE_IO_EOF
    ke_io();
    return 1;
#elif PSP_ORACLE_CASE == KS_CASE_IO_UMD_EOF
    ku_io();
    return 1;
#elif PSP_ORACLE_CASE == KS_CASE_SAFE_SCALAR
    ss_rtc();
    ss_tmid();
    ss_tls();
    return 1;
#else
    ks_subintr_order();
    ks_ctrl_empty_read();
    ks_ctrl_short_read();
    ks_ctrl_peek_count();
    ks_ctrl_modes();
    ks_ctrl_cycle();
    ks_vmem_type("vmem-trylock-type-1", 0, 1);
    ks_vmem_type("vmem-trylock-type-2", 0, 2);
    ks_vmem_type("vmem-trylock-type-m1", 0, -1);
    ks_vmem_type("vmem-lock-type-1", 1, 1);
    ks_vmem_alloc();
    ks_io();
    ks_wlan();
    return 1;
#endif
}

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
