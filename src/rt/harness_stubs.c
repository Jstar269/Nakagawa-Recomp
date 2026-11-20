// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * The runtime symbols a module and recomp.c reference, for an isolated run, and the input
 * helpers of the harnesses that make one (see harness_stubs.h). Moved here from
 * src/rt/snapshot_runner.c and src/rt/verify_harness.c, which each carried a copy.
 *
 * A harness links the generated module, recomp.c and the dispatch and interpreter units,
 * but no scheduler, HLE, GE or host platform. These definitions stand in for those units.
 * None of them can change a compared result silently: the scheduler flag starts off
 * (sr_sched_on is 0, so SR_YIELD never calls sr_yield, until a harness turns it on), and
 * the GE and debug hooks are inert because nothing arms them. The calls that leave guest
 * code, sr_yield, sr_syscall and sr_raw_syscall, are each harness's own.
 */

#include "harness_stubs.h"
#include "recomp.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

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
void sr_main_module_register(const SrMainModuleRecord *record) { (void)record; }
/* recomp.c reads the SDL clock for diagnostics only; the Makefile builds each harness's
 * copy of recomp.c with SDL_GetTicksNS renamed to this, so no SDL symbol is defined here. */
uint64_t snapshot_runner_ticks_ns(void) { return 0u; }
/* g_sr_last_writer_enabled stays 0, so the guest accessors never call this. */
void sr_note_mem_write(uint32_t addr, uint32_t width, uint32_t val, uint32_t pc) {
    (void)addr; (void)width; (void)val; (void)pc;
}

uint8_t *sr_harness_read_image(const char *path, uint32_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *data = NULL;
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
    if (n < 0 || (unsigned long long)n > UINT32_MAX || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    data = (uint8_t *)malloc(n ? (size_t)n : 1u);
    if (data && n && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *len_out = (uint32_t)n;
    return data;
}

int sr_harness_parse_hex32(const char *text, uint32_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(text, &end, 16);
    if (errno || !end || end == text || *end != '\0' || v > UINT32_MAX) return 0;
    *out = (uint32_t)v;
    return 1;
}
