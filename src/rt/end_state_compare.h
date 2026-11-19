// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * The comparison of the two end states of one guest call: the translated code's and the
 * reference interpreter's (src/ref). The snapshot runner (src/rt/snapshot_runner.c, issue
 * #1842) and the sampled in-play checker (src/rt/verify.c, issue #1843) both report the
 * first difference in the text this unit writes, so a mismatch the checker captures
 * replays in the runner to the same first difference. Moved here from those two files,
 * which each carried a copy of the field walk.
 *
 * The fields are compared in this order and the first difference is reported: the stop
 * kind (return, syscall, import), the syscall PC and code or the import NID, r1..r31, hi,
 * lo, f0..f31 (raw bits), fcr31, the out-of-range access counts, then each memory span the
 * caller lists, in that order, from its lowest address up.
 *
 * The unit is header-only, so neither program needs another source in its link list.
 */
#ifndef SR_END_STATE_COMPARE_H
#define SR_END_STATE_COMPARE_H

#include "recomp.h"
#include "../ref/snapshot_ref.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Where one side's run of the call stopped. The values index sr_end_stop_names. */
typedef enum { SR_END_STOP_RETURN = 0, SR_END_STOP_SYSCALL, SR_END_STOP_IMPORT } SrEndStopKind;

static const char *const sr_end_stop_names[] = { "return", "syscall", "import" };

typedef struct {
    SrEndStopKind kind;
    uint32_t syscall_pc;      /* a syscall stop: the address of the syscall instruction */
    uint32_t syscall_code;    /* a syscall stop: the instruction's 20-bit code */
    uint32_t import_nid;      /* an import stop: the NID of the import */
} SrEndStop;

/* `size` bytes of guest memory at `addr`, as each side holds them. */
typedef struct {
    uint32_t addr;
    const uint8_t *aot;
    const uint8_t *ref;
    uint32_t size;
} SrEndSpan;

/* A buffer of this size holds every message sr_end_state_difference writes. */
#define SR_END_DIFFERENCE_MAX 112u

/* Lowest index in [from, to) where a and b differ, or `to`. */
static inline uint32_t sr_end_first_difference(const uint8_t *a, const uint8_t *b,
                                               uint32_t from, uint32_t to) {
    const uint32_t step = 4096u;
    uint32_t i = from;
    while (i < to) {
        uint32_t n = to - i < step ? to - i : step;
        if (memcmp(a + i, b + i, n) != 0) {
            for (uint32_t j = 0; j < n; j++)
                if (a[i + j] != b[i + j]) return i + j;
        }
        i += n;
    }
    return to;
}

/* 1 when the major opcode is one of the VFPU's, which the reference does not implement. */
static inline int sr_end_is_vfpu_opcode(uint32_t op) {
    switch (op >> 26) {
    case 0x12: case 0x18: case 0x19: case 0x1b: case 0x32: case 0x34: case 0x35:
    case 0x36: case 0x37: case 0x3a: case 0x3c: case 0x3d: case 0x3e: case 0x3f:
        return 1;
    default:
        return 0;
    }
}

/* The stop of a reference run that ended at a comparable place (a return, a named import
 * stub or a syscall). The reference does not know the import tables: import_nid is 0 and
 * a caller that can name the stub's import fills it in. */
static inline SrEndStop sr_end_stop_of_reference(const NkRefResult *rr) {
    SrEndStop stop = { SR_END_STOP_RETURN, 0u, 0u, 0u };
    if (rr->stop == NK_REF_IMPORT) {
        stop.kind = SR_END_STOP_IMPORT;
    } else if (rr->stop == NK_REF_SYSCALL) {
        stop.kind = SR_END_STOP_SYSCALL;
        stop.syscall_pc = rr->pc;
        stop.syscall_code = (rr->op >> 6) & 0xFFFFFu;
    }
    return stop;
}

static inline int sr_end_difference_hex(char *out, size_t cap, const char *field,
                                        uint32_t aot, uint32_t ref) {
    snprintf(out, cap, "%s aot=0x%08" PRIx32 " ref=0x%08" PRIx32, field, aot, ref);
    return 1;
}

/* Compare the translated side's end state with the reference's. Returns 0 when nothing
 * differs. Otherwise returns 1 and writes the first difference to out, as the text that
 * follows MISMATCH in a verdict line: `<field> aot=<value> ref=<value>`.
 *
 * compare_import_nid is 0 for a caller whose reference stop carries no NID (verify.c): an
 * import stop then matches on its kind alone. oor_loads and oor_stores are the translated
 * side's out-of-range accesses during the call; the reference ran to a comparable stop, so
 * it made none. */
static inline int sr_end_state_difference(const SrEndStop *aot_stop, const CpuState *aot,
                                          const SrEndStop *ref_stop, const CpuState *ref,
                                          int compare_import_nid,
                                          unsigned long oor_loads, unsigned long oor_stores,
                                          const SrEndSpan *spans, size_t span_count,
                                          char *out, size_t cap) {
    if (aot_stop->kind != ref_stop->kind) {
        snprintf(out, cap, "stop aot=%s ref=%s", sr_end_stop_names[aot_stop->kind],
                 sr_end_stop_names[ref_stop->kind]);
        return 1;
    }
    if (ref_stop->kind == SR_END_STOP_SYSCALL) {
        if (aot_stop->syscall_pc != ref_stop->syscall_pc)
            return sr_end_difference_hex(out, cap, "syscall_pc", aot_stop->syscall_pc,
                                         ref_stop->syscall_pc);
        if (aot_stop->syscall_code != ref_stop->syscall_code)
            return sr_end_difference_hex(out, cap, "syscall_code", aot_stop->syscall_code,
                                         ref_stop->syscall_code);
    } else if (ref_stop->kind == SR_END_STOP_IMPORT && compare_import_nid) {
        if (aot_stop->import_nid != ref_stop->import_nid)
            return sr_end_difference_hex(out, cap, "import_nid", aot_stop->import_nid,
                                         ref_stop->import_nid);
    }
    char name[16];
    for (int i = 1; i < 32; i++) {
        if (aot->r[i] != ref->r[i]) {
            snprintf(name, sizeof name, "r%d", i);
            return sr_end_difference_hex(out, cap, name, aot->r[i], ref->r[i]);
        }
    }
    if (aot->hi != ref->hi) return sr_end_difference_hex(out, cap, "hi", aot->hi, ref->hi);
    if (aot->lo != ref->lo) return sr_end_difference_hex(out, cap, "lo", aot->lo, ref->lo);
    for (int i = 0; i < 32; i++) {
        if (aot->fi[i] != ref->fi[i]) {
            snprintf(name, sizeof name, "f%d", i);
            return sr_end_difference_hex(out, cap, name, aot->fi[i], ref->fi[i]);
        }
    }
    if (aot->fcr31 != ref->fcr31)
        return sr_end_difference_hex(out, cap, "fcr31", aot->fcr31, ref->fcr31);
    if (oor_loads != 0u)
        return sr_end_difference_hex(out, cap, "oor_loads", (uint32_t)oor_loads, 0u);
    if (oor_stores != 0u)
        return sr_end_difference_hex(out, cap, "oor_stores", (uint32_t)oor_stores, 0u);
    for (size_t i = 0; i < span_count; i++) {
        const SrEndSpan *span = &spans[i];
        uint32_t at = sr_end_first_difference(span->aot, span->ref, 0u, span->size);
        if (at < span->size) {
            snprintf(out, cap, "mem[0x%08" PRIx32 "] aot=0x%02x ref=0x%02x", span->addr + at,
                     span->aot[at], span->ref[at]);
            return 1;
        }
    }
    return 0;
}

/* For the harness self-tests only (the --selftest-corrupt option of snapshot_runner and
 * verify_harness): change one compared field of the translated side at its stop, so both
 * programs must report that field. `field` is a name this unit reports (r1..r31, hi, lo,
 * f0..f31, fcr31, syscall_pc, syscall_code) or mem:<hex guest address> for one byte, which
 * `arena` (guest physical address 0) holds. Returns 0 for a name it does not know. */
static inline int sr_end_state_selftest_corrupt(const char *field, CpuState *aot,
                                                SrEndStop *aot_stop, uint8_t *arena,
                                                uint32_t arena_size) {
    char *end = NULL;
    if (!strncmp(field, "mem:", 4)) {
        unsigned long long addr = strtoull(field + 4, &end, 16);
        if (end == field + 4 || *end != '\0' || addr >= arena_size) return 0;
        arena[addr] ^= 0xFFu;
    } else if (!strcmp(field, "hi")) {
        aot->hi ^= 1u;
    } else if (!strcmp(field, "lo")) {
        aot->lo ^= 1u;
    } else if (!strcmp(field, "fcr31")) {
        aot->fcr31 ^= 1u;
    } else if (!strcmp(field, "syscall_pc")) {
        aot_stop->syscall_pc ^= 4u;
    } else if (!strcmp(field, "syscall_code")) {
        aot_stop->syscall_code ^= 1u;
    } else if ((field[0] == 'r' || field[0] == 'f') && field[1] != '\0') {
        unsigned long index = strtoul(field + 1, &end, 10);
        if (*end != '\0' || index > 31u || (field[0] == 'r' && index == 0u)) return 0;
        if (field[0] == 'r') aot->r[index] ^= 1u;
        else aot->fi[index] ^= 1u;
    } else {
        return 0;
    }
    return 1;
}

#endif
