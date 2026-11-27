// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-10.
// See NOTICE.md for upstream lineage and modification provenance.


#include "interp.h"
#include "../rt/fp_convert.h"
#include "../rt/strbuf.h"

#include <cmath>
#include <cstring>
#include <cstdlib>

namespace ref {

// ---- Trace sink ----------------------------------------------------------------------

void TraceSink::Header(const char *target, uint32_t start_pc) {
	std::fprintf(out_, "# psp-recomp trace v1 oracle=interp target=%s start_pc=0x%08x\n",
		target ? target : "unknown", start_pc);
}

void TraceSink::InitDump(const CpuState *s) {
	std::fprintf(out_, "# init");
	for (int i = 0; i < 32; i++)
		if (s->r[i] != 0) std::fprintf(out_, " r%d=0x%08x", i, s->r[i]);
	for (int i = 0; i < 32; i++)
		if (s->fi[i] != 0) std::fprintf(out_, " f%d=0x%08x", i, s->fi[i]);
	if (s->hi != 0) std::fprintf(out_, " hi=0x%08x", s->hi);
	if (s->lo != 0) std::fprintf(out_, " lo=0x%08x", s->lo);
	if (s->fcr31 != 0) std::fprintf(out_, " fcr31=0x%08x", s->fcr31);
	std::fprintf(out_, "\n");
}

void TraceSink::BeginStep(const CpuState *s, uint32_t pc, uint32_t op) {
	std::memcpy(r_, s->r, sizeof(r_));
	std::memcpy(fi_, s->fi, sizeof(fi_));
	hi_ = s->hi;
	lo_ = s->lo;
	fcr31_ = s->fcr31;
	pc_ = pc;
	op_ = op;
}

void TraceSink::EndStep(const CpuState *s, uint32_t mem_addr, int mem_size, const Memory *mem) {
	char line[4096];
	size_t n = 0;
	n = sr_buf_append(line, sizeof(line), n, "%llu pc=0x%08x op=0x%08x",
	                  (unsigned long long)step_, pc_, op_);
	for (int i = 1; i < 32; i++)
		if (s->r[i] != r_[i])
			n = sr_buf_append(line, sizeof(line), n, " r%d=0x%08x", i, s->r[i]);
	if (s->hi != hi_)
		n = sr_buf_append(line, sizeof(line), n, " hi=0x%08x", s->hi);
	if (s->lo != lo_)
		n = sr_buf_append(line, sizeof(line), n, " lo=0x%08x", s->lo);
	for (int i = 0; i < 32; i++)
		if (s->fi[i] != fi_[i])
			n = sr_buf_append(line, sizeof(line), n, " f%d=0x%08x", i, s->fi[i]);
	if (s->fcr31 != fcr31_)
		n = sr_buf_append(line, sizeof(line), n, " fcr31=0x%08x", s->fcr31);
	if (mem_size == 1)
		n = sr_buf_append(line, sizeof(line), n, " m8[0x%08x]=0x%02x", mem_addr, mem->Read8(mem_addr));
	else if (mem_size == 2)
		n = sr_buf_append(line, sizeof(line), n, " m16[0x%08x]=0x%04x", mem_addr, mem->Read16(mem_addr));
	else if (mem_size == 4)
		n = sr_buf_append(line, sizeof(line), n, " m32[0x%08x]=0x%08x", mem_addr, mem->Read32(mem_addr));
	if (n >= sizeof(line)) n = sizeof(line) - 1;
	line[n] = '\n';
	std::fwrite(line, 1, n + 1, out_);
	step_++;
}

// ---- Instruction field helpers -------------------------------------------------------

namespace {

inline uint32_t Rs(uint32_t op) { return (op >> 21) & 0x1F; }
inline uint32_t Rt(uint32_t op) { return (op >> 16) & 0x1F; }
inline uint32_t Rd(uint32_t op) { return (op >> 11) & 0x1F; }
inline uint32_t Sa(uint32_t op) { return (op >> 6) & 0x1F; }
inline uint32_t Funct(uint32_t op) { return op & 0x3F; }
inline int32_t SImm(uint32_t op) { return (int32_t)(int16_t)(op & 0xFFFF); }
inline uint32_t ZImm(uint32_t op) { return op & 0xFFFF; }

// Allegrex bitrev: reverse the 32 bits of a word. The reference keeps its own copy, apart
// from the runtime's sr_bitrev (src/rt/recomp.c), so the two lanes check each other and the
// reference object exports no symbol that clashes when it links beside the runtime.
inline uint32_t BitReverse(uint32_t x) {
	x = ((x >> 1) & 0x55555555u) | ((x << 1) & 0xaaaaaaaau);
	x = ((x >> 2) & 0x33333333u) | ((x << 2) & 0xccccccccu);
	x = ((x >> 4) & 0x0f0f0f0fu) | ((x << 4) & 0xf0f0f0f0u);
	x = ((x >> 8) & 0x00ff00ffu) | ((x << 8) & 0xff00ff00u);
	return (x >> 16) | (x << 16);
}

// MIPS branch displacement is a signed 16-bit word offset, but the guest PC
// addition wraps at 32 bits.  Convert to unsigned before scaling so a
// backwards branch never left-shifts a negative signed value (C++ UB).
inline uint32_t BranchTarget(uint32_t branch_pc, int32_t displacement) {
	return (branch_pc + 4u) + static_cast<uint32_t>(displacement) * 4u;
}

// Arithmetic right shift on a 32-bit word with defined behavior. Right-shifting
// a negative signed value is implementation-defined in C++, so the shift runs on
// the unsigned value with an explicit sign-fill (portable bit logic). Callers
// pass 0..31 (instruction shift fields are 5 bits); n == 0 is the identity.
inline uint32_t ArithShiftRight(uint32_t v, unsigned n) {
	if (n == 0) return v;
	const uint32_t shifted = v >> n;
	const uint32_t fill = (0u - (v >> 31)) << (32 - n);  // all-ones when the sign bit is set
	return shifted | fill;
}

inline void SetR(CpuState *s, uint32_t idx, uint32_t val) {
	if (idx != 0) s->r[idx] = val;
}

// Compute a store's address and access size from the opcode, or size 0 if not a store.
// Mirrors how the trace records memory writes (TRACE_FORMAT.md), so it matches the oracle.
void StoreInfo(const CpuState *s, uint32_t op, uint32_t *addr, int *size) {
	*size = 0;
	uint32_t opcode = op >> 26;
	switch (opcode) {
		case 0x28: *size = 1; break;   // sb
		case 0x29: *size = 2; break;   // sh
		case 0x2B: *size = 4; break;   // sw
		case 0x2A:                     // swl and swr record the aligned word they merge into,
		case 0x2E:                     // as the generated code does (tools/codegen.py)
			*size = 4;
			*addr = (s->r[Rs(op)] + SImm(op)) & ~3u;
			return;
		case 0x39: *size = 4; break;   // swc1
		case 0x38:                     // sc writes memory only while the link is set
			if (!s->llbit) return;
			*size = 4;
			break;
		default: return;
	}
	*addr = s->r[Rs(op)] + SImm(op);
}

// c.cond.s orders its operands from their raw bits, never through a host float compare: a
// host compare runs in the ambient FP environment, where DAZ (or FPCR.FZ) makes a denormal
// equal to zero, so the result would depend on the process instead of the guest. Returns
// whether the word is a NaN; otherwise *key is the signed magnitude, which orders like the
// value and gives +0 and -0 the same key. It is written independently of sr_fpu_condition_s,
// which the generated code uses, so the two lanes still cross-check each other.
bool CompareKey(uint32_t bits, int64_t *key) {
	const uint32_t magnitude = bits & 0x7FFFFFFFu;
	if (magnitude > 0x7F800000u) return true;  // exponent all ones, mantissa nonzero
	*key = (bits >> 31) != 0 ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude);
	return false;
}

// A jump or branch: the forms that own a delay slot (the runtime interpreter's
// is_control_opcode, less the traps, which have none).
bool IsControlTransfer(uint32_t op) {
	const uint32_t opcode = op >> 26;
	if (opcode == 0x00) return Funct(op) == 0x08 || Funct(op) == 0x09;  // jr, jalr
	switch (opcode) {
		case 0x01:                                   // REGIMM b*z / b*zal and likely forms
		case 0x02: case 0x03:                        // j, jal
		case 0x04: case 0x05: case 0x06: case 0x07:  // beq, bne, blez, bgtz
		case 0x14: case 0x15: case 0x16: case 0x17:  // the likely forms
			return true;
		case 0x11: case 0x12:                        // the COP1 / COP2 branch group
			return Rs(op) == 0x08;
		default:
			return false;
	}
}

}  // namespace

// ---- Execute one instruction ---------------------------------------------------------

// Advances s->pc and sets branch bookkeeping. Returns kRunning on success, or a stop reason
// for syscall / unimplemented / break / a null code pointer (kMemoryFault). Memory faults of
// loads and stores are detected by the caller via the Memory fault flag. Run restores the
// state of any instruction that stops on an error, so Execute need not undo its own writes.
static StopReason Execute(CpuState *s, Memory *mem, uint32_t op) {
	const uint32_t opcode = op >> 26;
	const uint32_t branch_pc = s->pc;
	s->pc += 4;  // default sequential advance; branches override via next_pc/in_delay_slot

	auto take_branch = [&](bool cond) {
		if (cond) {
			s->next_pc = BranchTarget(branch_pc, SImm(op));
			s->in_delay_slot = true;
		}
	};
	auto skip_likely = [&](bool cond) {
		// Branch-likely: when taken behaves like a normal branch; when not taken the delay
		// slot is annulled (skipped).
		if (cond) {
			s->next_pc = BranchTarget(branch_pc, SImm(op));
			s->in_delay_slot = true;
		} else {
			s->pc += 4;  // skip the delay-slot instruction
		}
	};

	switch (opcode) {
		case 0x00: {  // SPECIAL
			const uint32_t funct = Funct(op);
			switch (funct) {
				case 0x00: SetR(s, Rd(op), s->r[Rt(op)] << Sa(op)); return StopReason::kRunning;          // sll
				case 0x02: {  // srl; Allegrex rs == 1 makes it rotr (the form sweep, #1844, found it missing)
					// Other rs values are reserved: the production lanes refuse them
					// (guest_interp.c; codegen.py "SRL reserved rs field"), and so does the reference.
					if (Rs(op) > 1) return StopReason::kUnimplemented;
					const uint32_t v = s->r[Rt(op)], n = Sa(op);
					SetR(s, Rd(op), Rs(op) == 1 ? (n == 0 ? v : (v >> n) | (v << (32 - n))) : v >> n);
					return StopReason::kRunning;
				}
				case 0x03: SetR(s, Rd(op), ArithShiftRight(s->r[Rt(op)], Sa(op))); return StopReason::kRunning;  // sra
				case 0x04: SetR(s, Rd(op), s->r[Rt(op)] << (s->r[Rs(op)] & 31)); return StopReason::kRunning;          // sllv
				case 0x06: {  // srlv; Allegrex sa == 1 makes it rotrv, other sa values are reserved
					if (Sa(op) > 1) return StopReason::kUnimplemented;
					const uint32_t v = s->r[Rt(op)], n = s->r[Rs(op)] & 31;
					SetR(s, Rd(op), Sa(op) == 1 ? (n == 0 ? v : (v >> n) | (v << (32 - n))) : v >> n);
					return StopReason::kRunning;
				}
				case 0x07: SetR(s, Rd(op), ArithShiftRight(s->r[Rt(op)], s->r[Rs(op)] & 31)); return StopReason::kRunning;  // srav
				// jr and jalr latch their target before the link write (AGENTS.md section 7,
				// PR #126), so jalr with rd == rs jumps to the register's old value. Their
				// reserved fields (rt, rd and sa for jr; rt and sa for jalr) must be zero, as the
				// runtime interpreter's decode requires. A computed target of 0 is a null code
				// pointer (#1375): it stops at the transfer, before the link write and the delay
				// slot, as a memory fault at address 0, where the production lanes refuse it.
				// The PSP ignores bits 1:0 of the target (pspautotests e37443a6
				// tests/cpu/jump/unaligned.expected, recorded on a PSP), so the latched target
				// is the aligned one, as in the production lanes.
				case 0x08: {  // jr
					if ((op & 0x001FFFC0u) != 0u) return StopReason::kUnimplemented;
					const uint32_t target = s->r[Rs(op)] & ~3u;
					if (target == 0u) return StopReason::kMemoryFault;
					s->next_pc = target;
					s->in_delay_slot = true;
					return StopReason::kRunning;
				}
				case 0x09: {  // jalr
					if ((op & 0x001F07C0u) != 0u) return StopReason::kUnimplemented;
					const uint32_t target = s->r[Rs(op)] & ~3u;
					if (target == 0u) return StopReason::kMemoryFault;
					SetR(s, Rd(op), branch_pc + 8);
					s->next_pc = target;
					s->in_delay_slot = true;
					return StopReason::kRunning;
				}
				case 0x0A: if (s->r[Rt(op)] == 0) SetR(s, Rd(op), s->r[Rs(op)]); return StopReason::kRunning;  // movz
				case 0x0B: if (s->r[Rt(op)] != 0) SetR(s, Rd(op), s->r[Rs(op)]); return StopReason::kRunning;  // movn
				case 0x0C: return StopReason::kSyscall;   // syscall
			// break: PSP uses BREAK for assertions. Log/report and continue under compatibility
			// policy, unless SR_BREAK_FATAL is set to force halt (distinguishing from normal execution).
			// This is a controlled compatibility approximation, not full PSP BREAK exception emulation.
			case 0x0D: {
				uint32_t code = (op >> 6) & 0xFFFFF;
				std::fprintf(stderr, "[interp/cpu] BREAK 0x%x encountered at pc=0x%08x\n", code, branch_pc);
				const char *fatal = std::getenv("SR_BREAK_FATAL");
				if (fatal && fatal[0] != '\0' && std::strcmp(fatal, "0") != 0) {
					std::fprintf(stderr, "Fatal error: SR_BREAK_FATAL is set; stopping.\n");
					return StopReason::kBreak;
				}
				return StopReason::kRunning;
			}
				case 0x10: SetR(s, Rd(op), s->hi); return StopReason::kRunning;  // mfhi
				case 0x11: s->hi = s->r[Rs(op)]; return StopReason::kRunning;    // mthi
				case 0x12: SetR(s, Rd(op), s->lo); return StopReason::kRunning;  // mflo
				case 0x13: s->lo = s->r[Rs(op)]; return StopReason::kRunning;    // mtlo
				case 0x18: {  // mult
					int64_t prod = (int64_t)(int32_t)s->r[Rs(op)] * (int64_t)(int32_t)s->r[Rt(op)];
					s->lo = (uint32_t)prod; s->hi = (uint32_t)(prod >> 32); return StopReason::kRunning;
				}
				case 0x19: {  // multu
					uint64_t prod = (uint64_t)s->r[Rs(op)] * (uint64_t)s->r[Rt(op)];
					s->lo = (uint32_t)prod; s->hi = (uint32_t)(prod >> 32); return StopReason::kRunning;
				}
				case 0x1A: {  // div
					// INT_MIN / -1 and a zero divisor are tested before / and %, which are undefined
					// for both. HI is 0 for the overflow on the PSP (pspautotests
					// tests/cpu/cpu_alu/cpu_div.expected at 257cfb5ad7, #3031).
					int32_t a = (int32_t)s->r[Rs(op)], b = (int32_t)s->r[Rt(op)];
					if (a == (int32_t)0x80000000 && b == -1) { s->lo = 0x80000000; s->hi = 0; }
					else if (b != 0) { s->lo = (uint32_t)(a / b); s->hi = (uint32_t)(a % b); }
					else { s->lo = a < 0 ? 1u : 0xFFFFFFFFu; s->hi = (uint32_t)a; }
					return StopReason::kRunning;
				}
				case 0x1B: {  // divu
					uint32_t a = s->r[Rs(op)], b = s->r[Rt(op)];
					if (b != 0) { s->lo = a / b; s->hi = a % b; }
					else { s->lo = a <= 0xFFFF ? 0xFFFFu : 0xFFFFFFFFu; s->hi = a; }
					return StopReason::kRunning;
				}
				case 0x20:    // add (PSP does not trap on overflow in practice for our targets)
				case 0x21: SetR(s, Rd(op), s->r[Rs(op)] + s->r[Rt(op)]); return StopReason::kRunning;  // addu
				case 0x22:    // sub
				case 0x23: SetR(s, Rd(op), s->r[Rs(op)] - s->r[Rt(op)]); return StopReason::kRunning;  // subu
				case 0x24: SetR(s, Rd(op), s->r[Rs(op)] & s->r[Rt(op)]); return StopReason::kRunning;  // and
				case 0x25: SetR(s, Rd(op), s->r[Rs(op)] | s->r[Rt(op)]); return StopReason::kRunning;  // or
				case 0x26: SetR(s, Rd(op), s->r[Rs(op)] ^ s->r[Rt(op)]); return StopReason::kRunning;  // xor
				case 0x27: SetR(s, Rd(op), ~(s->r[Rs(op)] | s->r[Rt(op)])); return StopReason::kRunning;  // nor
				case 0x2A: SetR(s, Rd(op), (int32_t)s->r[Rs(op)] < (int32_t)s->r[Rt(op)] ? 1 : 0); return StopReason::kRunning;  // slt
				case 0x2B: SetR(s, Rd(op), s->r[Rs(op)] < s->r[Rt(op)] ? 1 : 0); return StopReason::kRunning;  // sltu
				// Allegrex places these in SPECIAL (not in a separate SPECIAL2 page like
				// generic MIPS32): clz/clo at 0x16/0x17, madd/maddu at 0x1C/0x1D, max/min at
				// 0x2C/0x2D, msub/msubu at 0x2E/0x2F. Encodings verified against PPSSPP's
				// MIPSTables.cpp SPECIAL table.
				case 0x16: {  // clz rd, rs
					uint32_t v = s->r[Rs(op)];
					SetR(s, Rd(op), v == 0 ? 32 : (uint32_t)__builtin_clz(v)); return StopReason::kRunning;
				}
				case 0x17: {  // clo rd, rs
					uint32_t v = s->r[Rs(op)];
					SetR(s, Rd(op), v == 0xFFFFFFFF ? 32 : (uint32_t)__builtin_clz(~v)); return StopReason::kRunning;
				}
				case 0x1C: {  // madd
					uint64_t acc = ((uint64_t)s->hi << 32) | (uint32_t)s->lo;
					acc += (uint64_t)((int64_t)(int32_t)s->r[Rs(op)] * (int64_t)(int32_t)s->r[Rt(op)]);
					s->lo = (uint32_t)acc; s->hi = (uint32_t)(acc >> 32); return StopReason::kRunning;
				}
				case 0x1D: {  // maddu
					uint64_t acc = ((uint64_t)s->hi << 32) | s->lo;
					acc += (uint64_t)s->r[Rs(op)] * (uint64_t)s->r[Rt(op)];
					s->lo = (uint32_t)acc; s->hi = (uint32_t)(acc >> 32); return StopReason::kRunning;
				}
				case 0x2C: {  // max rd, rs, rt (signed)
					int32_t a = (int32_t)s->r[Rs(op)], b = (int32_t)s->r[Rt(op)];
					SetR(s, Rd(op), (uint32_t)(a > b ? a : b)); return StopReason::kRunning;
				}
				case 0x2D: {  // min rd, rs, rt (signed)
					int32_t a = (int32_t)s->r[Rs(op)], b = (int32_t)s->r[Rt(op)];
					SetR(s, Rd(op), (uint32_t)(a < b ? a : b)); return StopReason::kRunning;
				}
				case 0x2E: {  // msub
					uint64_t acc = ((uint64_t)s->hi << 32) | (uint32_t)s->lo;
					acc -= (uint64_t)((int64_t)(int32_t)s->r[Rs(op)] * (int64_t)(int32_t)s->r[Rt(op)]);
					s->lo = (uint32_t)acc; s->hi = (uint32_t)(acc >> 32); return StopReason::kRunning;
				}
				case 0x2F: {  // msubu
					uint64_t acc = ((uint64_t)s->hi << 32) | s->lo;
					acc -= (uint64_t)s->r[Rs(op)] * (uint64_t)s->r[Rt(op)];
					s->lo = (uint32_t)acc; s->hi = (uint32_t)(acc >> 32); return StopReason::kRunning;
				}
				default: return StopReason::kUnimplemented;
			}
		}
		case 0x01: {  // REGIMM
			const uint32_t rt = Rt(op);
			int32_t v = (int32_t)s->r[Rs(op)];
			switch (rt) {
				case 0x00: take_branch(v < 0); return StopReason::kRunning;   // bltz
				case 0x01: take_branch(v >= 0); return StopReason::kRunning;  // bgez
				case 0x02: skip_likely(v < 0); return StopReason::kRunning;   // bltzl
				case 0x03: skip_likely(v >= 0); return StopReason::kRunning;  // bgezl
				case 0x10: SetR(s, 31, branch_pc + 8); take_branch(v < 0); return StopReason::kRunning;   // bltzal
				case 0x11: SetR(s, 31, branch_pc + 8); take_branch(v >= 0); return StopReason::kRunning;  // bgezal
				default: return StopReason::kUnimplemented;
			}
		}
		case 0x02: s->next_pc = (branch_pc & 0xF0000000) | ((op & 0x3FFFFFF) << 2); s->in_delay_slot = true; return StopReason::kRunning;  // j
		case 0x03: SetR(s, 31, branch_pc + 8); s->next_pc = (branch_pc & 0xF0000000) | ((op & 0x3FFFFFF) << 2); s->in_delay_slot = true; return StopReason::kRunning;  // jal
		case 0x04: take_branch(s->r[Rs(op)] == s->r[Rt(op)]); return StopReason::kRunning;  // beq
		case 0x05: take_branch(s->r[Rs(op)] != s->r[Rt(op)]); return StopReason::kRunning;  // bne
		// blez, bgtz and their likely forms have a reserved rt field that must be zero; the
		// production lanes refuse any other value, and so does the reference.
		case 0x06:  // blez
			if (Rt(op) != 0) return StopReason::kUnimplemented;
			take_branch((int32_t)s->r[Rs(op)] <= 0); return StopReason::kRunning;
		case 0x07:  // bgtz
			if (Rt(op) != 0) return StopReason::kUnimplemented;
			take_branch((int32_t)s->r[Rs(op)] > 0); return StopReason::kRunning;
		case 0x08:    // addi
		case 0x09: SetR(s, Rt(op), s->r[Rs(op)] + (uint32_t)SImm(op)); return StopReason::kRunning;  // addiu
		case 0x0A: SetR(s, Rt(op), (int32_t)s->r[Rs(op)] < SImm(op) ? 1 : 0); return StopReason::kRunning;  // slti
		case 0x0B: SetR(s, Rt(op), s->r[Rs(op)] < (uint32_t)SImm(op) ? 1 : 0); return StopReason::kRunning;  // sltiu
		case 0x0C: SetR(s, Rt(op), s->r[Rs(op)] & ZImm(op)); return StopReason::kRunning;  // andi
		case 0x0D: SetR(s, Rt(op), s->r[Rs(op)] | ZImm(op)); return StopReason::kRunning;  // ori
		case 0x0E: SetR(s, Rt(op), s->r[Rs(op)] ^ ZImm(op)); return StopReason::kRunning;  // xori
		case 0x0F: SetR(s, Rt(op), ZImm(op) << 16); return StopReason::kRunning;           // lui
		case 0x14: skip_likely(s->r[Rs(op)] == s->r[Rt(op)]); return StopReason::kRunning;  // beql
		case 0x15: skip_likely(s->r[Rs(op)] != s->r[Rt(op)]); return StopReason::kRunning;  // bnel
		case 0x16:  // blezl
			if (Rt(op) != 0) return StopReason::kUnimplemented;
			skip_likely((int32_t)s->r[Rs(op)] <= 0); return StopReason::kRunning;
		case 0x17:  // bgtzl
			if (Rt(op) != 0) return StopReason::kUnimplemented;
			skip_likely((int32_t)s->r[Rs(op)] > 0); return StopReason::kRunning;
		case 0x20: SetR(s, Rt(op), (uint32_t)(int32_t)(int8_t)mem->Read8(s->r[Rs(op)] + SImm(op))); return StopReason::kRunning;   // lb
		case 0x21: SetR(s, Rt(op), (uint32_t)(int32_t)(int16_t)mem->Read16(s->r[Rs(op)] + SImm(op))); return StopReason::kRunning; // lh
		case 0x23: SetR(s, Rt(op), mem->Read32(s->r[Rs(op)] + SImm(op))); return StopReason::kRunning;  // lw
		case 0x24: SetR(s, Rt(op), mem->Read8(s->r[Rs(op)] + SImm(op))); return StopReason::kRunning;   // lbu
		case 0x25: SetR(s, Rt(op), mem->Read16(s->r[Rs(op)] + SImm(op))); return StopReason::kRunning;  // lhu
		case 0x28: mem->Write8(s->r[Rs(op)] + SImm(op), (uint8_t)s->r[Rt(op)]); return StopReason::kRunning;   // sb
		case 0x29: mem->Write16(s->r[Rs(op)] + SImm(op), (uint16_t)s->r[Rt(op)]); return StopReason::kRunning; // sh
		case 0x2B: mem->Write32(s->r[Rs(op)] + SImm(op), s->r[Rt(op)]); return StopReason::kRunning;  // sw
		// Unaligned word access, little-endian (PSP). For the byte offset k = addr & 3 within the
		// aligned word: LWL loads bytes k..0 into the top k+1 bytes of rt, LWR loads bytes k..3
		// into the low 4-k bytes, SWL stores the top k+1 bytes of rt to bytes k..0, SWR stores the
		// low 4-k bytes of rt to bytes k..3. Same merge as PPSSPP and src/rt/recomp.c; the form
		// sweep (#1844) found the earlier big-endian-style masks here taking the wrong bytes.
		case 0x22: {  // lwl
			uint32_t addr = s->r[Rs(op)] + SImm(op);
			uint32_t word = mem->Read32(addr & ~3u);
			uint32_t shift = (addr & 3) * 8;
			SetR(s, Rt(op), (s->r[Rt(op)] & (0x00FFFFFFu >> shift)) | (word << (24 - shift)));
			return StopReason::kRunning;
		}
		case 0x26: {  // lwr
			uint32_t addr = s->r[Rs(op)] + SImm(op);
			uint32_t word = mem->Read32(addr & ~3u);
			uint32_t shift = (addr & 3) * 8;
			SetR(s, Rt(op), (s->r[Rt(op)] & (0xFFFFFF00u << (24 - shift))) | (word >> shift));
			return StopReason::kRunning;
		}
		case 0x2A: {  // swl
			uint32_t addr = s->r[Rs(op)] + SImm(op);
			uint32_t word = mem->Read32(addr & ~3u);
			uint32_t shift = (addr & 3) * 8;
			mem->Write32(addr & ~3u, (s->r[Rt(op)] >> (24 - shift)) | (word & (0xFFFFFF00u << shift)));
			return StopReason::kRunning;
		}
		case 0x2E: {  // swr
			uint32_t addr = s->r[Rs(op)] + SImm(op);
			uint32_t word = mem->Read32(addr & ~3u);
			uint32_t shift = (addr & 3) * 8;
			mem->Write32(addr & ~3u, (s->r[Rt(op)] << shift) | (word & (0x00FFFFFFu >> (24 - shift))));
			return StopReason::kRunning;
		}
		case 0x30: {  // ll: load the word and set the link (MIPS32 LLbit)
			uint32_t addr = s->r[Rs(op)] + SImm(op);
			SetR(s, Rt(op), mem->Read32(addr));
			s->llbit = 1;
			return StopReason::kRunning;
		}
		case 0x38: {  // sc: store and report 1 only while the link is set, else 0
			uint32_t addr = s->r[Rs(op)] + SImm(op);
			const uint32_t linked = s->llbit != 0 ? 1u : 0u;
			if (!mem->CheckAligned(addr, 4u)) return StopReason::kRunning;  // address error
			if (linked) mem->Write32(addr, s->r[Rt(op)]);
			SetR(s, Rt(op), linked);
			return StopReason::kRunning;
		}
		case 0x11: {  // COP1 (single-precision FPU)
			const uint32_t fmt = Rs(op);  // sub-op selector in the rs field
			const uint32_t ft = Rt(op);
			const uint32_t fs = Rd(op);   // bits 15:11
			const uint32_t fd = Sa(op);   // bits 10:6
			switch (fmt) {
				case 0x00: SetR(s, ft, s->fi[fs]); return StopReason::kRunning;   // mfc1
				case 0x02:  // cfc1
					SetR(s, ft, fs == 31 ? s->fcr31 : (fs == 0 ? SR_FCR0_FIR : 0u));
					return StopReason::kRunning;
				case 0x04: s->fi[fs] = s->r[ft]; return StopReason::kRunning;     // mtc1
				case 0x06:
					// ctc1 stores the guest FCSR except the bits the PSP does
					// not store (SR_FCR31_CTC1_UNWRITABLE: 18..22 and 25..31).
					// The cached fpcond mirror resyncs so FCC0 stays
					// architectural in fcr31.
					if (fs == 31) {
						s->fcr31 = s->r[ft] & ~SR_FCR31_CTC1_UNWRITABLE;
						s->fpcond = (s->fcr31 >> 23) & 1u;
					}
					return StopReason::kRunning;  // ctc1
				case 0x08: {  // bc1f / bc1t / bc1fl / bc1tl
					bool tf = (op >> 16) & 1;
					bool likely = (op >> 17) & 1;
					bool cond = s->fpcond != 0;
					if (likely) skip_likely(cond == tf); else take_branch(cond == tf);
					return StopReason::kRunning;
				}
				case 0x10: {  // fmt = S
					const uint32_t funct = Funct(op);
					const uint32_t fcr31 = s->fcr31;
					// Evidence-class note: the arithmetic and conversion cells
					// share the exact fp_convert.h helper with generated code,
					// so AOT-vs-ref agreement there is SHARED_HELPER
					// corroboration, not an independent semantic line.
					// Independent authority for the underlying contract is the
					// cited PSP_HARDWARE anchors in fp_convert.h. c.cond.s is
					// the exception: it is deliberately independent of
					// sr_fpu_condition_s (see CompareKey).
					switch (funct) {
						case 0x00: s->f[fd] = sr_fpu_add_s(s->f[fs], s->f[ft], fcr31); return StopReason::kRunning;  // add.s
						case 0x01: s->f[fd] = sr_fpu_sub_s(s->f[fs], s->f[ft], fcr31); return StopReason::kRunning;  // sub.s
						// mul.s: sr_fpu_mul_s canonicalizes inf*0 to 0x7fc00000 from the raw
						// bits (environment-blind) before it touches the FP environment.
						case 0x02: s->f[fd] = sr_fpu_mul_s(s->f[fs], s->f[ft], fcr31); return StopReason::kRunning;
						case 0x03: s->f[fd] = sr_fpu_div_s(s->f[fs], s->f[ft], fcr31); return StopReason::kRunning;  // div.s
						case 0x04: s->f[fd] = sr_fpu_sqrt_s(s->f[fs], fcr31); return StopReason::kRunning;  // sqrt.s
						case 0x05: s->f[fd] = std::fabs(s->f[fs]); return StopReason::kRunning;  // abs.s
						case 0x06: s->f[fd] = s->f[fs]; return StopReason::kRunning;             // mov.s
						case 0x07: s->f[fd] = -s->f[fs]; return StopReason::kRunning;            // neg.s
						case 0x0C: s->fi[fd] = sr_fpu_to_word(s->f[fs], 0x0C, s->fcr31); return StopReason::kRunning;  // round.w.s
						case 0x0D: s->fi[fd] = sr_fpu_to_word(s->f[fs], 0x0D, s->fcr31); return StopReason::kRunning;  // trunc.w.s
						case 0x0E: s->fi[fd] = sr_fpu_to_word(s->f[fs], 0x0E, s->fcr31); return StopReason::kRunning;  // ceil.w.s
						case 0x0F: s->fi[fd] = sr_fpu_to_word(s->f[fs], 0x0F, s->fcr31); return StopReason::kRunning;  // floor.w.s
						case 0x24: s->fi[fd] = sr_fpu_to_word(s->f[fs], 0x24, s->fcr31); return StopReason::kRunning;  // cvt.w.s
						default:
							if (funct >= 0x30) {  // c.cond.s
								uint32_t cond = funct & 0xF;
								int64_t a = 0, b = 0;
								const bool a_nan = CompareKey(s->fi[fs], &a);
								const bool b_nan = CompareKey(s->fi[ft], &b);
								bool unordered = a_nan || b_nan;
								bool less = !unordered && a < b;
								bool equal = !unordered && a == b;
								bool result = (unordered && (cond & 1)) || (equal && (cond & 2)) || (less && (cond & 4));
								s->fpcond = result ? 1 : 0;
								// FCC0 stays architectural in fcr31; the cached
								// fpcond mirror and bit 23 move together.
								s->fcr31 = (s->fcr31 & ~0x00800000u) | (s->fpcond << 23);
								return StopReason::kRunning;
							}
							return StopReason::kUnimplemented;
					}
				}
				case 0x14: {  // fmt = W
					if (Funct(op) == 0x20) {
						// cvt.s.w rounds per guest FCR31 RM (see fp_convert.h).
						s->f[fd] = sr_fpu_cvt_s_w(sr_u32_as_s32(s->fi[fs]), s->fcr31);
						return StopReason::kRunning;
					}
					return StopReason::kUnimplemented;
				}
				default: return StopReason::kUnimplemented;
			}
		}
		case 0x1F: {  // SPECIAL3
			const uint32_t funct = Funct(op);
			if (funct == 0x00) {  // ext rt, rs, pos, size
				uint32_t pos = Sa(op);
				uint32_t size = ((op >> 11) & 0x1F) + 1;
				if (pos + size > 32) return StopReason::kUnimplemented;
				uint32_t mask = size >= 32 ? 0xFFFFFFFFu : ((1u << size) - 1);
				SetR(s, Rt(op), (s->r[Rs(op)] >> pos) & mask);
				return StopReason::kRunning;
			}
			if (funct == 0x04) {  // ins rt, rs, pos, size
				uint32_t pos = Sa(op);
				uint32_t msb = (op >> 11) & 0x1F;
				if (msb < pos) return StopReason::kUnimplemented;
				uint32_t size = msb - pos + 1;
				uint32_t mask = (size >= 32 ? 0xFFFFFFFFu : ((1u << size) - 1)) << pos;
				SetR(s, Rt(op), (s->r[Rt(op)] & ~mask) | ((s->r[Rs(op)] << pos) & mask));
				return StopReason::kRunning;
			}
			if (funct == 0x20) {  // BSHFL: sub-op in sa field
				uint32_t sub = Sa(op);
				uint32_t v = s->r[Rt(op)];
				if (sub == 0x02) { SetR(s, Rd(op), ((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF)); return StopReason::kRunning; }  // wsbh
				if (sub == 0x03) { SetR(s, Rd(op), ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) | ((v & 0x00FF0000u) >> 8) | ((v >> 24) & 0x000000FFu)); return StopReason::kRunning; }  // wsbw
				if (sub == 0x10) { SetR(s, Rd(op), (uint32_t)(int32_t)(int8_t)v); return StopReason::kRunning; }   // seb
				if (sub == 0x18) { SetR(s, Rd(op), (uint32_t)(int32_t)(int16_t)v); return StopReason::kRunning; }  // seh
				if (sub == 0x14) { SetR(s, Rd(op), BitReverse(v)); return StopReason::kRunning; }  // bitrev
				return StopReason::kUnimplemented;
			}
			return StopReason::kUnimplemented;
		}
		case 0x31: s->fi[Rt(op)] = mem->Read32(s->r[Rs(op)] + SImm(op)); return StopReason::kRunning; // lwc1
		case 0x39: mem->Write32(s->r[Rs(op)] + SImm(op), s->fi[Rt(op)]); return StopReason::kRunning; // swc1
		default: return StopReason::kUnimplemented;
	}
}

StepResult Run(CpuState *s, Memory *mem, unsigned long long max_steps, TraceSink *sink) {
	for (unsigned long long i = 0; i < max_steps; i++) {
		// Clear the sticky fault flag at the start of each iteration. Without this,
		// a single transient fault (e.g. during speculative prefetch) would mark all
		// subsequent valid accesses as faulty forever.
		mem->ClearFault();
		uint32_t pc = s->pc;
		uint32_t op = mem->Read32(pc);
		// The fetch faulted, so the instruction at pc never executed.
		if (mem->last_fault())
			return {StopReason::kMemoryFault, pc, op, i};

		// A jump or branch in a delay slot is UNPREDICTABLE on MIPS, and the production lanes
		// refuse it before any effect. Running it would drop the outer target and the inner
		// slot, so the reference stops by name instead of making up a result.
		if (s->in_delay_slot && IsControlTransfer(op))
			return {StopReason::kUnimplemented, pc, op, i};

		uint32_t store_addr = 0;
		int store_size = 0;
		if (sink) StoreInfo(s, op, &store_addr, &store_size);  // only the trace uses it

		if (sink) sink->BeginStep(s, pc, op);
		// An instruction that stops on an error (an unimplemented form, a fatal BREAK, a null
		// code pointer or a memory fault) does not complete: the state goes back to what it was
		// before it, so pc names it, a faulting load leaves rt alone, ll leaves the link, and a
		// delay slot keeps its branch pending. It is neither traced nor counted as executed.
		const CpuState before = *s;
		bool was_in_delay_slot = s->in_delay_slot;
		StopReason reason = Execute(s, mem, op);
		if (mem->last_fault() || reason == StopReason::kUnimplemented ||
		    reason == StopReason::kBreak || reason == StopReason::kMemoryFault) {
			*s = before;
			return {mem->last_fault() ? StopReason::kMemoryFault : reason, pc, op, i};
		}
		if (s->in_delay_slot && was_in_delay_slot) {
			s->pc = s->next_pc;
			s->in_delay_slot = false;
		}
		if (sink) sink->EndStep(s, store_addr, store_size, mem);

		if (reason != StopReason::kRunning)
			return {reason, pc, op, i + 1};
	}
	return {StopReason::kStepLimit, s->pc, 0, max_steps};
}

}  // namespace ref
