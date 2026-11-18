// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

#ifndef NAKAGAWA_GUEST_INTERP_H
#define NAKAGAWA_GUEST_INTERP_H

#include <stdint.h>

#include "recomp.h"

typedef enum SrGuestInterpResult {
    SR_GUEST_INTERP_AOT_HANDOFF       = 1,
    SR_GUEST_INTERP_CALL_RETURN       = 2,
    SR_GUEST_INTERP_EXCEPTION         = 3,
    SR_GUEST_INTERP_ERET              = 4,
    SR_GUEST_INTERP_NOT_EXECUTABLE    = -1,
    SR_GUEST_INTERP_MISALIGNED_PC     = -2,
    SR_GUEST_INTERP_FETCH_BOUNDARY    = -3,
    SR_GUEST_INTERP_UNSUPPORTED       = -4,
    SR_GUEST_INTERP_MEMORY_FAULT      = -5,
    SR_GUEST_INTERP_MISALIGNED_DATA   = -6,
    SR_GUEST_INTERP_FLOW_FATAL        = -7,
    SR_GUEST_INTERP_HOST_STACK_EXHAUSTED = -8,
    SR_GUEST_INTERP_HOST_STACK_BOUNDS_UNAVAILABLE = -9,
    /* jr/jalr read 0 from its target register (#1375): a null code pointer, even on
     * an image whose offset-0 function has guest address 0. */
    SR_GUEST_INTERP_NULL_CODE_POINTER = -10,
    /* An interpreted `jr $ra` returned past the linked call the interpreter runs for, to an
     * enclosing call's resume PC (#2016): a longjmp that call's caller cannot honour, since it
     * runs its own continuation. fault.pc is the `jr`, fault.address its target. */
    SR_GUEST_INTERP_RETURN_TARGET_MISMATCH = -11,
} SrGuestInterpResult;

typedef struct SrGuestInterpFault {
    uint32_t pc;
    uint32_t opcode;
    uint32_t address;
    int opcode_valid;
} SrGuestInterpFault;

/* A CALL into the interpreter carries the caller's already-decoded resume PC
 * separately from CpuState::$ra.  The callee is allowed to change $ra, including
 * in the return delay slot; the boundary remains the continuation selected by the
 * call instruction.  A NULL boundary means a TAIL transfer with no native caller
 * resume to protect. */
typedef struct SrGuestInterpCallBoundary {
    uint32_t resume_pc;
    /* A linked call owes its caller the $sp it was called with, as a callable native body
     * does (#1935); a live frame's continuation owes none. */
    uint32_t entry_sp;
    uint8_t owes_entry_sp;
    /* The innermost AOT return boundary is this call's own, begun by a generated call site
     * that honours its end check (dispatch_call_checked): native code run for the call
     * returns into it, and a return elsewhere unwinds as a direct call's does. */
    uint8_t site_boundary;
    /* Native code an interpreted linked call handed its live frame to: the rest of that
     * call, not a call of its own (dispatch_call_continue). */
    uint8_t continuation;
    /* The call is dispatched again under frames it already has, for exception or ERET flow
     * its code raised. The frames below hold the call's own entry $sp, so a body entered this
     * way is not the host entry's (SR_BOUNDARY_HOST_ENTRY in recomp.h). */
    uint8_t redispatch;
} SrGuestInterpCallBoundary;

/* The interpreter hands the live frame of the linked call `call` to the native body at
 * `target`, whose chain ends in that call's return (recomp.c). Returns 1 when that return
 * went somewhere other than the call's resume PC, a longjmp: the guest's $sp stands. */
int dispatch_call_continue(CpuState *s, uint32_t target, const SrGuestInterpCallBoundary *call);

/* Execute from entry until an unlinked transfer reaches a registered AOT destination
 * or a fail-closed boundary. A linked transfer into a registered body is a CALL: the
 * body runs as a nested native frame and interpretation resumes at the link address. There is deliberately no instruction-count escape hatch:
 * a guest loop is guest behavior, not permission to fabricate completion.
 *
 * IMPLEMENTED INSTRUCTION SET
 * ---------------------------
 * The authority for which forms the interpreter decodes is the decoder in
 * guest_interp.c itself, not a list in this comment. The cosim harness probes
 * that production decoder (run_form_census() in tests/fixtures/cosim/cosim_selftest.c)
 * and requires the decoded set to equal the form list
 * tests/fixtures/cosim/generate.py derives from the cells, each of which compares
 * the interpreted result against real codegen output; tools/test_cosim_fixture.py
 * pins that list to the declared inventory. Anything the decoder does not name --
 * including every control-transfer form it does not decode -- fails closed as
 * SR_GUEST_INTERP_UNSUPPORTED rather than being decoded as some unrelated
 * arithmetic form.
 *
 * The decoded families, for orientation only (the census above is the check):
 *
 *   ALU        shifts (sll srl rotr sra and the variable sllv srlv rotrv srav),
 *              add addu sub subu and or xor nor slt sltu, the immediates addi
 *              addiu slti sltiu andi ori xori lui, movz movn, clz clo, min max
 *              (rotr is the Allegrex srl with rs=1; any other nonzero rs is reserved)
 *   SPECIAL3   ext ins seb seh wsbh wsbw bitrev
 *   HI/LO      mult multu div divu madd maddu msub msubu mfhi mflo mthi mtlo
 *   memory     lb lbu lh lhu lw / sb sh sw / lwl lwr swl swr / lwc1 swc1
 *   atomic     ll sc (CpuState.llbit: ll sets it; sc stores and writes 1 only
 *              while it is set, else writes 0; same width-4 alignment and span
 *              checks as lw/sw, run before the link is consulted)
 *   control    beq bne blez bgtz bltz bgez bltzal bgezal, their branch-likely
 *              forms, bc1f bc1t (and likely), j jal jr jalr
 *   scalar FPU mfc1 mtc1 cfc1 ctc1, add.s sub.s mul.s div.s sqrt.s abs.s
 *              neg.s mov.s c.cond.s cvt.s.w cvt.w.s (via the src/rt/fp_convert.h
 *              helpers the generated code also calls, so guest FCR31 selects
 *              the result in both execution lanes)
 *   LLE CPU    mfc0 mtc0 syscall break eret (src/rt/cpu_lle.h helpers, gated
 *              on sr_cpu_lle_enabled(); with the gate off these keep the
 *              historical fail-closed SR_GUEST_INTERP_UNSUPPORTED so the
 *              default lane and the cosim form census are unchanged)
 *   LLE VFPU   lv.s sv.s lv.q sv.q lvl.q lvr.q svl.q svr.q address checks
 *              (widths 4/16, left/right width 0 bypass; gated on
 *              sr_cpu_lle_enabled() with the same historical UNSUPPORTED
 *              when off; the access itself runs via sr_vfpu_interp)
 *
 * The LLE gate is what keeps the cosim census honest: census probes run with
 * the default (disabled) configuration, so the newly decoded COP0/syscall
 * shapes still read as "not decoded" there, while LLE selftests enable the
 * gate explicitly.
 *
 * While SR_INSTRUCTION_TRACE is compiled in, execution emits the canonical
 * per-instruction trace (tools/TRACE_FORMAT.md) through the same sr_begin/sr_end
 * hooks the generated code uses, in the same branch-before-delay-slot order. */
SrGuestInterpResult sr_guest_interp_run(
    CpuState *s,
    uint32_t entry,
    SrGuestInterpFault *fault);

/* TAIL form: a computed step of a tail-chain drain (dispatch_tail_step). As above, except
 * that a handoff to a registered body records the transfer for the enclosing drain
 * (sr_tail_record) with the entry $sp the run owes, or the bounded release of a run that
 * loaded $sp out of its frame, as a native body entered at `entry` records it, instead of
 * dispatching it nested (#1935). */
SrGuestInterpResult sr_guest_interp_run_tail_step(
    CpuState *s,
    uint32_t entry,
    SrGuestInterpFault *fault);

/* CALL form: execute the interpreted callee, including its return instruction and
 * delay slot, then stop before executing boundary->resume_pc. */
SrGuestInterpResult sr_guest_interp_run_with_boundary(
    CpuState *s,
    uint32_t entry,
    const SrGuestInterpCallBoundary *boundary,
    SrGuestInterpFault *fault);

const char *sr_guest_interp_result_name(SrGuestInterpResult result);

#endif
