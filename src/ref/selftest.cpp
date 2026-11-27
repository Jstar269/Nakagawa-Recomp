// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-10.
// See NOTICE.md for upstream lineage and modification provenance.

// expected results and asserts the interpreter reproduces them. This is a real test: every
// expected value below is computed by hand from the MIPS semantics, not copied from the
// interpreter's own output. Exit code 0 means all checks passed.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdlib>
#include <new>

#include "interp.h"
#include "recomp.h"
#include "snapshot_ref.h"

#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

using namespace ref;

// While set, a non-throwing array allocation of the reference RAM's size (64 MiB) fails, as
// it does on a host that cannot supply it. Every other allocation goes to the library.
static volatile bool g_fail_ram_alloc = false;

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
	if (g_fail_ram_alloc && size >= Memory::kRamSize)
		return nullptr;
	return ::operator new[](size);
}

namespace {

int g_failures = 0;

void Check(const char *what, uint32_t got, uint32_t want) {
	if (got != want) {
		std::printf("FAIL %-28s got=0x%08x want=0x%08x\n", what, got, want);
		g_failures++;
	} else {
		std::printf("ok   %-28s 0x%08x\n", what, got);
	}
}

// Register indices.
enum { ZERO = 0, V0 = 2, A0 = 4, T0 = 8, T1 = 9, T2 = 10, T3 = 11, T4 = 12, T5 = 13, T6 = 14, T7 = 15, S0 = 16, S1 = 17, S2 = 18, S3 = 19, S4 = 20, S5 = 21, S6 = 22, S7 = 23, T8 = 24, T9 = 25, SP = 29, RA = 31 };

uint32_t R(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sa, uint32_t funct) {
	return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct;
}
uint32_t I(uint32_t opcode, uint32_t rs, uint32_t rt, uint16_t imm) {
	return (opcode << 26) | (rs << 21) | (rt << 16) | imm;
}
const uint32_t BREAK = 0x0000000D;
const uint32_t SYSCALL = 0x0000000C;

// Load a program at base and run it to its syscall instruction, returning the final state. Every
// program here ends in a syscall, so any other stop fails the case: a run that skips the syscall
// (a wrong branch or jump target) runs the zero words after it as nops into the step limit.
// Cases that expect another stop call Run directly.
ref::CpuState RunProgram(const std::vector<uint32_t> &prog, uint32_t base) {
	Memory mem;
	for (size_t i = 0; i < prog.size(); i++)
		mem.Write32(base + (uint32_t)i * 4, prog[i]);
	ref::CpuState s;
	s.pc = base;
	s.r[SP] = 0x08A00000;  // a valid RAM stack pointer
	StepResult res = Run(&s, &mem, 1000, nullptr);
	if (res.reason != StopReason::kSyscall) {
		std::printf("FAIL program did not end at its syscall: reason=%d pc=0x%08x op=0x%08x\n",
			(int)res.reason, res.pc, res.op);
		g_failures++;
	}
	return s;
}

void TestArithmetic() {
	const uint32_t base = 0x08900000;
	std::vector<uint32_t> p = {
		I(0x0F, 0, T0, 0x1234),       // lui   t0, 0x1234        -> 0x12340000
		I(0x0D, T0, T0, 0x5678),      // ori   t0, t0, 0x5678    -> 0x12345678
		I(0x09, ZERO, T1, 100),       // addiu t1, zero, 100      -> 100
		R(T0, T1, T2, 0, 0x21),       // addu  t2, t0, t1         -> 0x123456DC
		R(ZERO, T1, T3, 4, 0x00),     // sll   t3, t1, 4          -> 1600 (0x640)
		R(T2, T1, T4, 0, 0x23),       // subu  t4, t2, t1         -> 0x12345678
		R(T0, T1, T5, 0, 0x24),       // and   t5, t0, t1         -> 0x12345678 & 100 = 0x60
		R(T0, T1, T6, 0, 0x2A),       // slt   t6, t0, t1         -> (0x12345678 < 100) signed = 0
		I(0x0A, T1, T7, 0xFFFF),      // slti  t7, t1, -1         -> (100 < -1) = 0
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("lui+ori t0", s.r[T0], 0x12345678);
	Check("addiu t1", s.r[T1], 100);
	Check("addu t2", s.r[T2], 0x123456DC);
	Check("sll t3", s.r[T3], 0x640);
	Check("subu t4", s.r[T4], 0x12345678);
	Check("and t5", s.r[T5], 0x60);
	Check("slt t6", s.r[T6], 0);
	Check("slti t7", s.r[T7], 0);
}

void TestMulDiv() {
	const uint32_t base = 0x08900000;
	std::vector<uint32_t> p = {
		I(0x09, ZERO, T0, 7),         // addiu t0, zero, 7
		I(0x09, ZERO, T1, 6),         // addiu t1, zero, 6
		R(T0, T1, 0, 0, 0x18),        // mult  t0, t1            -> lo=42, hi=0
		R(0, 0, T2, 0, 0x12),         // mflo  t2                -> 42
		R(0, 0, T3, 0, 0x10),         // mfhi  t3                -> 0
		I(0x09, ZERO, T4, 100),       // addiu t4, zero, 100
		I(0x09, ZERO, T5, 7),         // addiu t5, zero, 7
		R(T4, T5, 0, 0, 0x1B),        // divu  t4, t5            -> lo=14, hi=2
		R(0, 0, T6, 0, 0x12),         // mflo  t6                -> 14
		R(0, 0, T7, 0, 0x10),         // mfhi  t7                -> 2
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("mult lo (mflo)", s.r[T2], 42);
	Check("mult hi (mfhi)", s.r[T3], 0);
	Check("divu lo (mflo)", s.r[T6], 14);
	Check("divu hi (mfhi)", s.r[T7], 2);
}

// div and divu where C's / and % are undefined or MIPS leaves the result unpredictable: a zero
// divisor and INT_MIN / -1 (#3031). Every row is a line of pspautotests
// tests/cpu/cpu_alu/cpu_div.expected at 257cfb5ad7, recorded on a PSP. HI and LO are seeded so a
// result the instruction failed to write is not mistaken for 0.
void TestDivPspEdgeCases() {
	const uint32_t base = 0x08900000;
	struct Row { const char *what; uint32_t funct, a, b, lo, hi; };
	const Row rows[] = {
		{"div 80000000 / -1", 0x1A, 0x80000000u, 0xFFFFFFFFu, 0x80000000u, 0x00000000u},
		{"div 00000000 / 0", 0x1A, 0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0x00000000u},
		{"div ffffffff / 0", 0x1A, 0xFFFFFFFFu, 0x00000000u, 0x00000001u, 0xFFFFFFFFu},
		{"div 80000000 / 0", 0x1A, 0x80000000u, 0x00000000u, 0x00000001u, 0x80000000u},
		{"div 80000000 / -7", 0x1A, 0x80000000u, 0xFFFFFFF9u, 0x12492492u, 0xFFFFFFFEu},
		{"divu 80000000 / ffffffff", 0x1B, 0x80000000u, 0xFFFFFFFFu, 0x00000000u, 0x80000000u},
		{"divu 00000001 / 0", 0x1B, 0x00000001u, 0x00000000u, 0x0000FFFFu, 0x00000001u},
		{"divu ffffffff / 0", 0x1B, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, 0xFFFFFFFFu},
	};
	for (const Row &row : rows) {
		std::vector<uint32_t> p = {
			I(0x0F, 0, T0, (uint16_t)(row.a >> 16)),     // lui   t0, a.hi
			I(0x0D, T0, T0, (uint16_t)row.a),            // ori   t0, t0, a.lo
			I(0x0F, 0, T1, (uint16_t)(row.b >> 16)),     // lui   t1, b.hi
			I(0x0D, T1, T1, (uint16_t)row.b),            // ori   t1, t1, b.lo
			I(0x0F, 0, T2, 0xA5A5),                      // lui   t2, 0xA5A5
			R(T2, 0, 0, 0, 0x11),                        // mthi  t2
			R(T2, 0, 0, 0, 0x13),                        // mtlo  t2
			R(T0, T1, 0, 0, row.funct),                  // div / divu t0, t1
			SYSCALL,
		};
		ref::CpuState s = RunProgram(p, base);
		char what[64];
		std::snprintf(what, sizeof what, "%s lo", row.what);
		Check(what, s.lo, row.lo);
		std::snprintf(what, sizeof what, "%s hi", row.what);
		Check(what, s.hi, row.hi);
	}
}

void TestMemory() {
	const uint32_t base = 0x08900000;
	std::vector<uint32_t> p = {
		I(0x0F, 0, T0, 0xDEAD),       // lui   t0, 0xDEAD
		I(0x0D, T0, T0, 0xBEEF),      // ori   t0, t0, 0xBEEF    -> 0xDEADBEEF
		I(0x2B, SP, T0, 0x10),        // sw    t0, 16(sp)
		I(0x23, SP, T1, 0x10),        // lw    t1, 16(sp)        -> 0xDEADBEEF
		I(0x28, SP, T0, 0x20),        // sb    t0, 32(sp)        (stores 0xEF)
		I(0x24, SP, T2, 0x20),        // lbu   t2, 32(sp)        -> 0xEF
		I(0x20, SP, T3, 0x20),        // lb    t3, 32(sp)        -> sign-extended 0xFFFFFFEF
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("lw roundtrip", s.r[T1], 0xDEADBEEF);
	Check("lbu byte", s.r[T2], 0xEF);
	Check("lb sign-extend", s.r[T3], 0xFFFFFFEF);
}

// lwl/lwr/swl/swr on a little-endian aligned word 0x44332211 (bytes 11 22 33 44 from the
// lowest address) with rt = 0xAABBCCDD. Every expected value is worked out by hand from the
// definition (LWL loads bytes k..0 into the top bytes of rt, LWR bytes k..3 into the low
// bytes, SWL stores the top bytes of rt to bytes k..0, SWR the low bytes to bytes k..3).
void TestUnalignedWordAccess() {
	const uint32_t code = 0x08900000;
	const uint32_t aligned = 0x08A00100;
	const uint32_t rt = 0xAABBCCDD;
	const uint32_t lwl_want[4] = {0x11BBCCDD, 0x2211CCDD, 0x332211DD, 0x44332211};
	const uint32_t lwr_want[4] = {0x44332211, 0xAA443322, 0xAABB4433, 0xAABBCC44};
	const uint32_t swl_want[4] = {0x443322AA, 0x4433AABB, 0x44AABBCC, 0xAABBCCDD};
	const uint32_t swr_want[4] = {0xAABBCCDD, 0xBBCCDD11, 0xCCDD2211, 0xDD332211};
	const struct { uint32_t opcode; const char *name; const uint32_t *want; bool store; } cases[] = {
		{0x22, "lwl", lwl_want, false}, {0x26, "lwr", lwr_want, false},
		{0x2A, "swl", swl_want, true}, {0x2E, "swr", swr_want, true},
	};
	for (const auto &c : cases) {
		for (uint32_t k = 0; k < 4; k++) {
			Memory mem;
			mem.Write32(aligned, 0x44332211);
			mem.Write32(code, I(c.opcode, A0, T0, (uint16_t)k));
			ref::CpuState s;
			s.pc = code;
			s.r[A0] = aligned;
			s.r[T0] = rt;
			Run(&s, &mem, 1, nullptr);
			char what[32];
			std::snprintf(what, sizeof what, "%s offset %u", c.name, k);
			Check(what, c.store ? mem.Read32(aligned) : s.r[T0], c.want[k]);
		}
	}
}

// A halfword or word access at an address that is not a multiple of its width, and a fetch at
// a pc that is not a multiple of 4, raise an address error on the PSP (PSP-A3-01; the runtime
// interpreter stops with MISALIGNED_DATA / MISALIGNED_PC). The reference stops with a memory
// fault at that address and leaves memory untouched; it used to read or write the straddling
// bytes as if the access were aligned.
void TestMisalignedAccessFaults() {
	const uint32_t code = 0x08900000;
	const uint32_t data = 0x08A00100;
	const struct { uint32_t op; uint16_t offset; const char *name; bool link; } cases[] = {
		{0x21, 1, "lh odd", false},  {0x25, 3, "lhu odd", false}, {0x29, 1, "sh odd", false},
		{0x23, 2, "lw +2", false},   {0x2B, 1, "sw +1", false},   {0x30, 2, "ll +2", false},
		{0x38, 2, "sc +2 linked", true}, {0x38, 3, "sc +3 unlinked", false},
		{0x31, 2, "lwc1 +2", false}, {0x39, 1, "swc1 +1", false},
	};
	for (const auto &c : cases) {
		Memory mem;
		mem.Write32(data, 0x44332211);
		mem.Write32(data + 4, 0x88776655);
		mem.Write32(code, I(c.op, A0, T0, c.offset));
		ref::CpuState s;
		s.pc = code;
		s.r[A0] = data;
		s.r[T0] = 0xAABBCCDD;
		s.llbit = c.link ? 1u : 0u;
		StepResult res = Run(&s, &mem, 1, nullptr);
		char what[48];
		std::snprintf(what, sizeof what, "%s stops", c.name);
		Check(what, (uint32_t)res.reason, (uint32_t)StopReason::kMemoryFault);
		std::snprintf(what, sizeof what, "%s fault addr", c.name);
		Check(what, mem.fault_addr(), data + c.offset);
		std::snprintf(what, sizeof what, "%s word 0 kept", c.name);
		Check(what, mem.Read32(data), 0x44332211);
		std::snprintf(what, sizeof what, "%s word 1 kept", c.name);
		Check(what, mem.Read32(data + 4), 0x88776655);
	}
	// A run entered at a pc that is not word aligned stops at the fetch, before anything executes
	// (a jr/jalr clears bits 1:0 of its target; TestJumpTargetLowBitsIgnored).
	Memory mem;
	mem.Write32(code, I(0x09, ZERO, T0, 1));
	mem.Write32(code + 4, I(0x09, ZERO, T0, 2));
	ref::CpuState s;
	s.pc = code + 2;
	StepResult res = Run(&s, &mem, 4, nullptr);
	Check("misaligned pc stops", (uint32_t)res.reason, (uint32_t)StopReason::kMemoryFault);
	Check("misaligned pc fault addr", mem.fault_addr(), code + 2);
	Check("misaligned pc executed", (uint32_t)res.executed, 0u);
	Check("misaligned pc no effect", s.r[T0], 0u);
}

// A scratch file for a trace: tmpfile() where the host allows it, else a named file in the
// temporary folder, removed by the caller through *path.
std::FILE *OpenScratch(char *path, size_t size) {
	path[0] = '\0';
	if (std::FILE *f = std::tmpfile()) return f;
	const char *dir = std::getenv("TMPDIR");
	if (dir == nullptr) dir = std::getenv("TEMP");
	if (dir == nullptr) dir = std::getenv("TMP");
	std::snprintf(path, size, "%s/nk_ref_selftest_trace.txt", dir != nullptr ? dir : ".");
	return std::fopen(path, "w+b");
}

// swl and swr write the aligned word that holds their byte; the trace records that word, as
// the generated code records it (tools/codegen.py: "(address) & ~3u", 4). The trace used to read
// back four bytes at the unaligned address: a different token from the generated code's, and a
// memory fault for a completed store at the last word of RAM.
void TestUnalignedStoreTraceRecordsTheAlignedWord() {
	const uint32_t code = 0x08900000;
	const struct {
		uint32_t op; uint32_t base; const char *name; const char *token;
	} cases[] = {
		{0x2A, 0x08A00101, "swl +1 traced", " m32[0x08a00100]=0x4433aabb\n"},
		{0x2E, 0x0BFFFFFD, "swr end of RAM traced", " m32[0x0bfffffc]=0xbbccdd00\n"},
	};
	for (const auto &c : cases) {
		Memory mem;
		mem.Write32(0x08A00100, 0x44332211);
		mem.Write32(code, I(c.op, A0, T0, 0));
		mem.Write32(code + 4, SYSCALL);
		ref::CpuState s;
		s.pc = code;
		s.r[A0] = c.base;
		s.r[T0] = 0xAABBCCDD;
		char path[512];
		std::FILE *out = OpenScratch(path, sizeof path);
		if (out == nullptr) {
			std::printf("FAIL %-28s cannot open a scratch trace file\n", c.name);
			g_failures++;
			continue;
		}
		TraceSink sink(out);
		StepResult res = Run(&s, &mem, 4, &sink);
		std::fflush(out);
		std::rewind(out);
		char text[1024] = {0};
		const size_t got = std::fread(text, 1, sizeof text - 1, out);
		text[got] = '\0';
		std::fclose(out);
		if (path[0] != '\0') std::remove(path);
		char what[48];
		std::snprintf(what, sizeof what, "%s stop", c.name);
		Check(what, (uint32_t)res.reason, (uint32_t)StopReason::kSyscall);
		const bool found = std::strstr(text, c.token) != nullptr;
		if (!found) std::printf("     trace: %s", text);
		Check(c.name, found ? 1u : 0u, 1u);
	}
}

// Allegrex rotates share the SRL/SRLV encodings: rs == 1 makes srl a rotate right by sa, and
// sa == 1 makes srlv a rotate right by the low five bits of rs. Values worked out by hand.
void TestAllegrexRotate() {
	const uint32_t code = 0x08900000;
	const struct { uint32_t word; uint32_t rs_value; uint32_t rt_value; uint32_t want; const char *name; } cases[] = {
		{R(1, T1, T0, 4, 0x02), 0, 0x12345678, 0x81234567, "rotr by 4"},
		{R(1, T1, T0, 1, 0x02), 0, 0x00000001, 0x80000000, "rotr by 1"},
		{R(1, T1, T0, 0, 0x02), 0, 0x12345678, 0x12345678, "rotr by 0"},
		{R(0, T1, T0, 4, 0x02), 0, 0x12345678, 0x01234567, "srl by 4 (rs 0)"},
		{R(T2, T1, T0, 1, 0x06), 36, 0x12345678, 0x81234567, "rotrv by 36 & 31"},
		{R(T2, T1, T0, 0, 0x06), 36, 0x12345678, 0x01234567, "srlv by 36 & 31"},
		{R(T2, T1, T0, 1, 0x06), 32, 0x12345678, 0x12345678, "rotrv by 32 & 31"},
	};
	for (const auto &c : cases) {
		Memory mem;
		mem.Write32(code, c.word);
		ref::CpuState s;
		s.pc = code;
		s.r[T1] = c.rt_value;
		s.r[T2] = c.rs_value;
		Run(&s, &mem, 1, nullptr);
		Check(c.name, s.r[T0], c.want);
	}
}

void TestBranchDelaySlot() {
	const uint32_t base = 0x08900000;
	// beq taken: the delay slot runs, the instruction after it is skipped.
	std::vector<uint32_t> p = {
		I(0x09, ZERO, T0, 5),         // 0x..00 addiu t0, zero, 5
		I(0x09, ZERO, T1, 5),         // 0x..04 addiu t1, zero, 5
		I(0x04, T0, T1, 2),           // 0x..08 beq t0,t1,+2 -> target 0x..08+4+8 = 0x..14
		I(0x09, ZERO, T2, 0x111),     // 0x..0c delay slot: addiu t2, zero, 0x111 (runs)
		I(0x09, ZERO, T3, 0x222),     // 0x..10 skipped (branch target is 0x..14)
		I(0x09, ZERO, T4, 0x333),     // 0x..14 addiu t4, zero, 0x333 (runs)
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("delay slot executed", s.r[T2], 0x111);
	Check("skipped after branch", s.r[T3], 0);     // must remain 0
	Check("branch target ran", s.r[T4], 0x333);
}

void TestDaybreak4GuestInterpreterOracle() {
	const uint32_t base = 0x08910000u;
	const uint32_t data = 0x08911000u;
	const uint32_t resume = 0x08910040u;
	const uint32_t program[] = {
		0x24081234u, // addiu t0, zero, 0x1234
		0xac880000u, // sw    t0, 0(a0)
		0x8c820000u, // lw    v0, 0(a0)
		0x03e00008u, // jr    ra
		0x24420001u, // addiu v0, v0, 1 -- return delay slot
	};

	Memory mem;
	for (size_t i = 0; i < sizeof(program) / sizeof(program[0]); ++i)
		mem.Write32(base + static_cast<uint32_t>(i) * 4u, program[i]);
	mem.Write32(resume, SYSCALL); // separate-oracle stand-in for the production AOT handoff
	mem.Write32(data, 0xfeedfaceu);

	ref::CpuState s;
	s.pc = base;
	s.r[V0] = 0xdeadbeefu;
	s.r[A0] = data;
	s.r[SP] = 0x08a00000u;
	s.r[RA] = resume;
	StepResult result = Run(&s, &mem, 16, nullptr);

	Check("daybreak4 oracle stop", static_cast<uint32_t>(result.reason),
		static_cast<uint32_t>(StopReason::kSyscall));
	Check("daybreak4 oracle store", mem.Read32(data), 0x00001234u);
	Check("daybreak4 oracle v0", s.r[V0], 0x00001235u);
	Check("daybreak4 oracle pc", result.pc, resume);
}

void TestReferenceArithmeticGuards() {
	const uint32_t base = 0x08900000;
	// A backward branch exercises the signed displacement path without relying
	// on a host compiler's treatment of a negative left shift.
	std::vector<uint32_t> branch = {
		I(0x09, ZERO, T0, 1),       // t0 = 1
		I(0x09, ZERO, T1, 1),       // t1 = 1
		I(0x04, T0, T1, 0xFFFF),    // beq -1: loop back to the delay slot
		I(0x09, ZERO, T2, 7),       // delay slot, then step limit
	};
	Memory branch_mem;
	for (size_t i = 0; i < branch.size(); ++i) branch_mem.Write32(base + (uint32_t)i * 4, branch[i]);
	ref::CpuState branch_state;
	branch_state.pc = base;
	StepResult branch_result = Run(&branch_state, &branch_mem, 20, nullptr);
	Check("backward branch remains defined", static_cast<uint32_t>(branch_result.reason),
		static_cast<uint32_t>(StopReason::kStepLimit));

	// Reserved SPECIAL3 field combinations must stop before deriving an
	// underflowed/oversized mask.
	const uint32_t invalid_ins = (0x1Fu << 26) | (T0 << 16) | (T1 << 21) | (5u << 11) | (10u << 6) | 0x04u;
	Memory invalid_mem;
	invalid_mem.Write32(base, invalid_ins);
	ref::CpuState invalid_state;
	invalid_state.pc = base;
	StepResult invalid_result = Run(&invalid_state, &invalid_mem, 4, nullptr);
	Check("invalid ins encoding stops", static_cast<uint32_t>(invalid_result.reason),
		static_cast<uint32_t>(StopReason::kUnimplemented));
}

uint32_t MTC1(uint32_t rt, uint32_t fs) { return (0x11u << 26) | (0x04u << 21) | (rt << 16) | (fs << 11); }
uint32_t MFC1(uint32_t rt, uint32_t fs) { return (0x11u << 26) | (0x00u << 21) | (rt << 16) | (fs << 11); }
uint32_t CTC1(uint32_t rt, uint32_t fs) { return (0x11u << 26) | (0x06u << 21) | (rt << 16) | (fs << 11); }
uint32_t FB(float v) { uint32_t b; std::memcpy(&b, &v, sizeof(b)); return b; }
uint32_t FPS(uint32_t ft, uint32_t fs, uint32_t fd, uint32_t funct) {
	return (0x11u << 26) | (0x10u << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct;
}
uint32_t FPW(uint32_t fs, uint32_t fd, uint32_t funct) {
	return (0x11u << 26) | (0x14u << 21) | (fs << 11) | (fd << 6) | funct;
}

void TestFpu() {
	const uint32_t base = 0x08900000;
	// f0=2.0 (0x40000000), f1=3.0 (0x40400000) loaded via GPR + mtc1.
	std::vector<uint32_t> p = {
		I(0x0F, ZERO, T0, 0x4000), MTC1(T0, 0),   // f0 = 2.0f
		I(0x0F, ZERO, T1, 0x4040), MTC1(T1, 1),   // f1 = 3.0f
		FPS(1, 0, 2, 0x00), MFC1(T2, 2),          // add.s f2,f0,f1 -> 5.0f; t2 = bits
		FPS(1, 0, 3, 0x02), MFC1(T3, 3),          // mul.s f3,f0,f1 -> 6.0f; t3 = bits
		FPS(1, 0, 4, 0x01), MFC1(T4, 4),          // sub.s f4,f0,f1 -> -1.0f; t4 = bits
		I(0x09, ZERO, T5, 7), MTC1(T5, 5),        // f5 = int 7 (bits)
		FPW(5, 6, 0x20), MFC1(T6, 6),             // cvt.s.w f6,f5 -> 7.0f; t6 = bits
		I(0x0F, ZERO, T7, 0x40F0), MTC1(T7, 7),   // f7 = 7.5f (0x40F00000)
		FPS(0, 7, 8, 0x24), MFC1(T0, 8),          // cvt.w.s f8,f7 -> 8 (round-half-to-even); t0 = 8
		FPS(0, 7, 9, 0x0D), MFC1(T1, 9),          // trunc.w.s f9,f7 -> 7; t1 = 7
		I(0x0F, ZERO, T8, 0x0000), I(0x0D, T8, T8, 0xFFFF), MTC1(T8, 10),  // f10 = bits 0x0000FFFF (65535)
		FPW(10, 11, 0x20), MFC1(S0, 11),          // cvt.s.w f11,f10 -> 65535.0f (bits 0x477FFF00); s0 = bits
		I(0x0F, ZERO, T8, 0xFFFF), I(0x0D, T8, T8, 0xFFFF), MTC1(T8, 12),  // f12 = bits 0xFFFFFFFF (-1)
		FPW(12, 13, 0x20), MFC1(S1, 13),          // cvt.s.w f13,f12 -> -1.0f; s1 = bits
		I(0x0F, ZERO, T8, 0x7FFF), I(0x0D, T8, T8, 0xFFFF), MTC1(T8, 14),  // f14 = bits 0x7FFFFFFF (INT32_MAX)
		FPW(14, 15, 0x20), MFC1(S2, 15),          // cvt.s.w f15,f14 -> 2147483648.0f (0x4F000000, rounds); s2 = bits
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("add.s 2+3=5.0", s.r[T2], 0x40A00000);
	Check("mul.s 2*3=6.0", s.r[T3], 0x40C00000);
	Check("sub.s 2-3=-1.0", s.r[T4], 0xBF800000);
	Check("cvt.s.w 7=7.0", s.r[T6], 0x40E00000);
	Check("cvt.w.s 7.5=8", s.r[T0], 8);
	Check("trunc.w.s 7.5=7", s.r[T1], 7);
	Check("cvt.s.w 65535", s.r[S0], 0x477FFF00u);
	Check("cvt.s.w -1", s.r[S1], 0xBF800000u);
	Check("cvt.s.w INT32_MAX rounds", s.r[S2], 0x4F000000u);
}

void TestFpuConvertEdgeCases() {
	const uint32_t base = 0x08900000;
	std::vector<uint32_t> p;
	// Load a float's bit pattern into f-register fs via lui + ori + mtc1.
	auto load_float = [&](uint32_t fs, float v) {
		const uint32_t bits = FB(v);
		p.push_back(I(0x0F, ZERO, T0, (uint16_t)(bits >> 16)));
		p.push_back(I(0x0D, T0, T0, (uint16_t)(bits & 0xFFFF)));
		p.push_back(MTC1(T0, fs));
	};
	// Convert f[fs] to word into f[fd] and copy the bits to a GPR for the assert.
	auto to_word = [&](uint32_t gpr, uint32_t fs, uint32_t fd, uint32_t funct) {
		p.push_back(FPS(0, fs, fd, funct));
		p.push_back(MFC1(gpr, fd));
	};

	load_float(2, 2.5f);    // round.w.s: nearest/even -> 2
	load_float(3, -2.5f);   // round.w.s: nearest/even -> -2
	load_float(4, 2.1f);    // ceil.w.s -> 3
	load_float(5, 2.9f);    // floor.w.s -> 2
	load_float(6, -2.5f);   // trunc.w.s -> -2
	load_float(7, 2.5f);    // cvt.w.s RN -> 2 (tie to even)
	load_float(8, 3.5f);    // cvt.w.s RN -> 4 (tie to even)
	load_float(9, 2.7f);    // cvt.w.s RZ -> 2
	load_float(10, -2.7f);  // cvt.w.s RZ -> -2
	load_float(11, 1e30f);
	load_float(12, 1e30f);
	load_float(13, -1e30f);
	load_float(14, 2147483648.0f);   // exactly 2^31
	load_float(15, -2147483648.0f);  // exactly -2^31
	load_float(16, 2147483520.0f);   // largest float below 2^31

	// +inf / -inf / NaN loaded from raw bit patterns (no float literal needed).
	auto load_raw = [&](uint32_t fs, uint32_t bits) {
		p.push_back(I(0x0F, ZERO, T0, (uint16_t)(bits >> 16)));
		p.push_back(I(0x0D, T0, T0, (uint16_t)(bits & 0xFFFF)));
		p.push_back(MTC1(T0, fs));
	};
	load_raw(19, 0x7F800000u);  // f19 = +inf
	load_raw(20, 0xFF800000u);  // f20 = -inf
	load_raw(21, 0x7FC00000u);  // f21 = NaN

	// In-range rounding paths (fcr31 = 0 = RN).
	to_word(T3, 2, 2, 0x0C);   // round.w.s 2.5  -> 2
	to_word(T4, 3, 3, 0x0C);   // round.w.s -2.5 -> -2
	to_word(T5, 4, 4, 0x0E);   // ceil.w.s 2.1   -> 3
	to_word(T6, 5, 5, 0x0F);   // floor.w.s 2.9  -> 2
	to_word(T7, 6, 6, 0x0D);   // trunc.w.s -2.5 -> -2
	to_word(T8, 7, 7, 0x24);   // cvt.w.s 2.5 RN -> 2 (tie to even)
	to_word(T9, 8, 8, 0x24);   // cvt.w.s 3.5 RN -> 4 (tie to even)

	// Switch fcr31 to RZ (1) and run the two RZ cases, then restore RN.
	p.push_back(I(0x09, ZERO, T0, 1));
	p.push_back(CTC1(T0, 31));
	// Destinations kept distinct from the f9/f10 sources: writing the integer
	// result back into the source register would destroy the value the RP/RM
	// cases below still need.
	to_word(T1, 9, 24, 0x24);  // cvt.w.s 2.7 RZ  -> 2
	to_word(T2, 10, 25, 0x24); // cvt.w.s -2.7 RZ -> -2

	// RP (ceil) and RM (floor) rounding modes, then restore RN.
	p.push_back(I(0x09, ZERO, T0, 2));
	p.push_back(CTC1(T0, 31));
	to_word(26, 9, 22, 0x24);  // cvt.w.s 2.7 RP  -> 3 (ceil)
	p.push_back(I(0x09, ZERO, T0, 3));
	p.push_back(CTC1(T0, 31));
	to_word(27, 10, 23, 0x24); // cvt.w.s -2.7 RM -> -3 (floor)
	p.push_back(I(0x09, ZERO, T0, 0));
	p.push_back(CTC1(T0, 31));

	// Overflow / boundary / non-finite paths.
	to_word(S0, 11, 11, 0x0D);  // trunc.w.s 1e30f -> 0x7FFFFFFF (positive clamp)
	to_word(S1, 12, 12, 0x0E);  // ceil.w.s 1e30f  -> 0x7FFFFFFF (signed saturation)
	to_word(S2, 13, 13, 0x0F);  // floor.w.s -1e30f -> 0x80000000
	to_word(S3, 14, 14, 0x0D);  // trunc.w.s 2^31  -> 0x7FFFFFFF
	to_word(S4, 15, 15, 0x0D);  // trunc.w.s -2^31 -> 0x80000000
	to_word(S5, 16, 16, 0x0D);  // trunc.w.s 2147483520.0f -> 2147483520
	to_word(S6, 19, 17, 0x0C);  // round.w.s +inf  -> 0x7FFFFFFF
	to_word(S7, 20, 18, 0x0C);  // round.w.s -inf  -> 0x80000000
	p.push_back(FPS(0, 21, 19, 0x0D));
	p.push_back(MFC1(T0, 19));  // trunc.w.s NaN   -> 0x7FFFFFFF
	p.push_back(SYSCALL);

	ref::CpuState s = RunProgram(p, base);
	Check("round.w.s 2.5 ties even", s.r[T3], 2u);
	Check("round.w.s -2.5", s.r[T4], 0xFFFFFFFEu);
	Check("ceil.w.s 2.1", s.r[T5], 3u);
	Check("floor.w.s 2.9", s.r[T6], 2u);
	Check("trunc.w.s -2.5", s.r[T7], 0xFFFFFFFEu);
	Check("cvt.w.s 2.5 RN", s.r[T8], 2u);
	Check("cvt.w.s 3.5 RN", s.r[T9], 4u);
	Check("cvt.w.s 2.7 RZ", s.r[T1], 2u);
	Check("cvt.w.s -2.7 RZ", s.r[T2], 0xFFFFFFFEu);
	Check("cvt.w.s 2.7 RP", s.r[26], 3u);
	Check("cvt.w.s -2.7 RM", s.r[27], 0xFFFFFFFDu);
	Check("trunc.w.s +1e30 clamp", s.r[S0], 0x7FFFFFFFu);
	Check("ceil.w.s +1e30 clamp", s.r[S1], 0x7FFFFFFFu);
	Check("floor.w.s -1e30", s.r[S2], 0x80000000u);
	Check("trunc.w.s 2^31", s.r[S3], 0x7FFFFFFFu);
	Check("trunc.w.s -2^31", s.r[S4], 0x80000000u);
	Check("trunc.w.s 2147483520", s.r[S5], 2147483520u);
	Check("round.w.s +inf", s.r[S6], 0x7FFFFFFFu);
	Check("round.w.s -inf", s.r[S7], 0x80000000u);
	Check("trunc.w.s NaN", s.r[T0], 0x7FFFFFFFu);
}

void TestArithmeticShifts() {
	const uint32_t base = 0x08900000;
	// sra/srav must sign-fill with portable bit logic (right-shifting a negative
	// signed value is implementation-defined C++).
	std::vector<uint32_t> p = {
		I(0x0F, ZERO, T0, 0x8000),        // lui t0, 0x8000 -> 0x80000000
		R(T0, T0, T1, 3, 0x03),           // sra t1, t0, 3  -> 0xF0000000
		I(0x09, ZERO, T2, 3),             // addiu t2, zero, 3
		R(T2, T0, T3, 0, 0x07),           // srav t3, t0, t2 -> 0xF0000000
		R(T0, T0, T4, 31, 0x03),          // sra t4, t0, 31  -> 0xFFFFFFFF
		R(ZERO, T0, T5, 0, 0x03),         // sra t5, t0, 0   -> 0x80000000
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("sra 0x80000000>>3", s.r[T1], 0xF0000000u);
	Check("srav 0x80000000>>3", s.r[T3], 0xF0000000u);
	Check("sra >>31 sign fill", s.r[T4], 0xFFFFFFFFu);
	Check("sra >>0 identity", s.r[T5], 0x80000000u);
}

uint32_t BC1(uint32_t tf, uint32_t likely, uint16_t off) {
	return (0x11u << 26) | (0x08u << 21) | ((likely & 1) << 17) | ((tf & 1) << 16) | off;
}

void TestFpuCompareBranch() {
	const uint32_t base = 0x08900000;
	// c.lt.s f0,f1 with f0=2.0 < f1=3.0 sets the FPU condition; bc1t must be taken and skip
	// the instruction after its delay slot.
	std::vector<uint32_t> p = {
		I(0x0F, ZERO, T0, 0x4000), MTC1(T0, 0),   // f0 = 2.0
		I(0x0F, ZERO, T1, 0x4040), MTC1(T1, 1),   // f1 = 3.0
		FPS(1, 0, 0, 0x3C),                       // c.lt.s f0,f1 -> condition true
		BC1(1, 0, 2),                             // bc1t +2 (taken)
		0,                                        // delay slot: nop
		I(0x09, ZERO, T2, 0xBAD),                 // skipped on taken branch
		I(0x09, ZERO, T2, 0x111),                 // branch target
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("c.lt.s+bc1t taken", s.r[T2], 0x111);
}

uint32_t CFC1(uint32_t rt, uint32_t fs) { return (0x11u << 26) | (0x02u << 21) | (rt << 16) | (fs << 11); }

// PSP-FPU-CC-001 (PSP-3000 / 6.61, 2026-10-15): the PSP FPU has FCC0 only. A ctc1 of
// 0xFE800000 reads back 0x00800000, c.eq.s with condition code 1..7 writes FCC0, and
// bc1t with condition code 1..7 tests FCC0. The encodings are the probe's own
// (tests/fixtures/psp_oracle/probe.c, run_fpu_cc): c.eq.s 0x460D6032 | (cc << 8) and bc1t
// 0x45010002 | (cc << 18).
void TestFpuConditionCodesMeasured() {
	const uint32_t base = 0x08900000;
	std::vector<uint32_t> p = {
		I(0x0F, ZERO, T0, 0x3F80), MTC1(T0, 12),   // f12 = 1.0
		MTC1(T0, 13),                              // f13 = 1.0
		I(0x0F, ZERO, T0, 0xFE80), CTC1(T0, 31),   // ctc1 0xFE800000
		CFC1(T1, 31),                              // t1 = FCR31 (measured 0x00800000)
		I(0x0F, ZERO, T0, 0xFE00), CTC1(T0, 31),   // ctc1 0xFE000000: no FCC0
		CFC1(T2, 31),                              // t2 = FCR31 (measured 0)
		0x460D6332u,                               // c.eq.s f12,f13 with cc 3: equal
		CFC1(T3, 31),                              // t3 = FCR31 (FCC0 only)
		0x450D0002u,                               // bc1t with cc 3, +2: taken
		0,                                         // delay slot
		I(0x09, ZERO, T4, 0xBAD),                  // skipped
		I(0x09, ZERO, T5, 0x111),                  // branch target
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("ctc1 0xFE800000 reads back", s.r[T1], 0x00800000u);
	Check("ctc1 0xFE000000 reads back", s.r[T2], 0x00000000u);
	Check("c.eq.s cc3 writes FCC0", s.r[T3], 0x00800000u);
	Check("bc1t cc3 tests FCC0", s.r[T4], 0u);
	Check("bc1t cc3 target ran", s.r[T5], 0x111u);
}

// pspautotests e37443a6 tests/cpu/fpu/fcr.expected (recorded on a PSP), lines 16, 18 and 24: a
// ctc1 of FO (0x00400000), FN (0x00200000) or bits 18..20 (0x001C0000) reads back 0, and line 14
// reads FS (0x01000000) back as written.
void TestFcr31UnwritableBitsRecorded() {
	const uint32_t base = 0x08900000;
	std::vector<uint32_t> p = {
		I(0x0F, ZERO, T0, 0x0040), CTC1(T0, 31), CFC1(T1, 31),   // FO
		I(0x0F, ZERO, T0, 0x0020), CTC1(T0, 31), CFC1(T2, 31),   // FN
		I(0x0F, ZERO, T0, 0x001C), CTC1(T0, 31), CFC1(T3, 31),   // bits 18..20
		I(0x0F, ZERO, T0, 0x0100), CTC1(T0, 31), CFC1(T4, 31),   // FS
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("ctc1 0x00400000 reads back", s.r[T1], 0x00000000u);
	Check("ctc1 0x00200000 reads back", s.r[T2], 0x00000000u);
	Check("ctc1 0x001C0000 reads back", s.r[T3], 0x00000000u);
	Check("ctc1 0x01000000 reads back", s.r[T4], 0x01000000u);
}

// PSP-LLSC-001 (PSP-3000 / 6.61, 2026-10-15): the probe's three ll/sc link windows with its
// values (tests/fixtures/psp_oracle/probe.c, run_llsc_link). On the console a plain ll/sc pair
// stores and reports 1, a second sc with no new ll stores and reports 1 again, and an sc to
// another address than ll's stores there and reports 1. Rows are 16 bytes, A at +0, B at +4.
void TestLlScMeasuredWindows() {
	const uint32_t base = 0x08900000;
	auto li = [](uint32_t rt, uint32_t value) {
		return std::vector<uint32_t>{I(0x0F, ZERO, rt, (uint16_t)(value >> 16)),
		                             I(0x0D, rt, rt, (uint16_t)(value & 0xFFFF))};
	};
	std::vector<uint32_t> p = {I(0x0F, ZERO, A0, 0x0890), I(0x0D, A0, A0, 0x1000)};
	for (uint32_t w : li(T9, 0x11111111u)) p.push_back(w);
	for (uint32_t w : li(T8, 0x44444444u)) p.push_back(w);
	for (uint16_t row = 0; row < 48; row += 16) {
		p.push_back(I(0x2B, A0, T9, row));              // sw A
		p.push_back(I(0x2B, A0, T8, row + 4));          // sw B
	}
	for (uint32_t w : li(T1, 0x22222222u)) p.push_back(w);
	for (uint32_t w : li(T3, 0x22222222u)) p.push_back(w);
	for (uint32_t w : li(T4, 0x33333333u)) p.push_back(w);
	for (uint32_t w : li(T6, 0x55555555u)) p.push_back(w);
	const std::vector<uint32_t> windows = {
		I(0x30, A0, T0, 0), I(0x38, A0, T1, 0),         // llsc-control
		I(0x30, A0, T2, 16), I(0x38, A0, T3, 16),       // llsc-second-sc
		I(0x38, A0, T4, 16),                            //   second sc, no new ll
		I(0x30, A0, T5, 32), I(0x38, A0, T6, 36),       // llsc-other-address
		I(0x23, A0, S0, 0), I(0x23, A0, S1, 4),         // A, B after each cell
		I(0x23, A0, S2, 16), I(0x23, A0, S3, 20),
		I(0x23, A0, S4, 32), I(0x23, A0, S5, 36),
		SYSCALL,
	};
	p.insert(p.end(), windows.begin(), windows.end());
	ref::CpuState s = RunProgram(p, base);
	Check("llsc-control ll loaded", s.r[T0], 0x11111111u);
	Check("llsc-control sc result", s.r[T1], 1u);
	Check("llsc-control A after", s.r[S0], 0x22222222u);
	Check("llsc-control B after", s.r[S1], 0x44444444u);
	Check("llsc-second-sc ll loaded", s.r[T2], 0x11111111u);
	Check("llsc-second-sc first sc", s.r[T3], 1u);
	Check("llsc-second-sc second sc", s.r[T4], 1u);
	Check("llsc-second-sc A after", s.r[S2], 0x33333333u);
	Check("llsc-second-sc B after", s.r[S3], 0x44444444u);
	Check("llsc-other-address ll", s.r[T5], 0x11111111u);
	Check("llsc-other-address sc", s.r[T6], 1u);
	Check("llsc-other-address A", s.r[S4], 0x11111111u);
	Check("llsc-other-address B", s.r[S5], 0x55555555u);
}

void TestR0IsZero() {
	const uint32_t base = 0x08900000;
	std::vector<uint32_t> p = {
		I(0x09, ZERO, ZERO, 1234),    // addiu zero, zero, 1234  -> write to r0 discarded
		R(ZERO, ZERO, T0, 0, 0x21),   // addu  t0, zero, zero     -> 0
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("r0 stays zero", s.r[ZERO], 0);
	Check("addu from zero", s.r[T0], 0);
}

void TestBreakContinues() {
	const uint32_t base = 0x08900000;
	// break must advance PC and continue (PPSSPP Int_Break model).
	// Compilers guard a division with a separate `bnez divisor; break 7`; the div itself never
	// traps. The explicit break below is what this case tests: execution continues past it.
	std::vector<uint32_t> p = {
		I(0x09, ZERO, T0, 0),         // addiu t0, zero, 0    -> t0 = 0
		I(0x09, ZERO, T1, 42),        // addiu t1, zero, 42   -> t1 = 42
		R(T0, T1, 0, 0, 0x1A),        // div   t0, t1         -> 0 / 42, no trap
		0x0000000D,                    // break (funct 0x0D)   -> must continue, not stop
		I(0x09, ZERO, T2, 99),        // addiu t2, zero, 99   -> should execute
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("execution continued past break", s.r[T2], 99);
}

void TestBreakCodePreservation() {
	const uint32_t base = 0x08900000;
	// test break code 0
	std::vector<uint32_t> p0 = {
		0x0000000D, // break 0
		I(0x09, ZERO, T2, 99),
		SYSCALL,
	};
	ref::CpuState s0 = RunProgram(p0, base);
	Check("break 0 continued and ran next", s0.r[T2], 99);

	// test break code 7
	std::vector<uint32_t> p7 = {
		0x000001CD, // break 7
		I(0x09, ZERO, T2, 107),
		SYSCALL,
	};
	ref::CpuState s7 = RunProgram(p7, base);
	Check("break 7 continued and ran next", s7.r[T2], 107);
}

void TestBreakFatalMode() {
	const uint32_t base = 0x08900000;
	std::vector<uint32_t> p = {
		0x000001CD, // break 7
		I(0x09, ZERO, T2, 107),
		SYSCALL,
	};

	// 1. SR_BREAK_FATAL = 1 (should stop)
	{
#ifdef _WIN32
		_putenv_s("SR_BREAK_FATAL", "1");
#else
		setenv("SR_BREAK_FATAL", "1", 1);
#endif
		Memory mem;
		for (size_t i = 0; i < p.size(); i++)
			mem.Write32(base + (uint32_t)i * 4, p[i]);
		ref::CpuState s;
		s.pc = base;
		StepResult res = Run(&s, &mem, 1000, nullptr);
#ifdef _WIN32
		_putenv_s("SR_BREAK_FATAL", "");
#else
		unsetenv("SR_BREAK_FATAL");
#endif
		Check("break fatal mode stopped at break", res.reason == StopReason::kBreak ? 1 : 0, 1);
		Check("break fatal mode stopped at correct PC", res.pc, base);
	}

	// 2. SR_BREAK_FATAL = 0 (should continue)
	{
#ifdef _WIN32
		_putenv_s("SR_BREAK_FATAL", "0");
#else
		setenv("SR_BREAK_FATAL", "0", 1);
#endif
		Memory mem;
		for (size_t i = 0; i < p.size(); i++)
			mem.Write32(base + (uint32_t)i * 4, p[i]);
		ref::CpuState s;
		s.pc = base;
		StepResult res = Run(&s, &mem, 1000, nullptr);
#ifdef _WIN32
		_putenv_s("SR_BREAK_FATAL", "");
#else
		unsetenv("SR_BREAK_FATAL");
#endif
		Check("break fatal=0 continued past break", res.reason == StopReason::kSyscall ? 1 : 0, 1);
		Check("break fatal=0 reached target register setting", s.r[T2], 107);
	}

	// 3. SR_BREAK_FATAL = "" / empty (should continue)
	{
#ifdef _WIN32
		// The Microsoft C runtime removes a variable set to an empty value, so on Windows
		// this case repeats case 4 (unset). The empty value itself is tested on POSIX.
		_putenv_s("SR_BREAK_FATAL", "");
#else
		setenv("SR_BREAK_FATAL", "", 1);
#endif
		Memory mem;
		for (size_t i = 0; i < p.size(); i++)
			mem.Write32(base + (uint32_t)i * 4, p[i]);
		ref::CpuState s;
		s.pc = base;
		StepResult res = Run(&s, &mem, 1000, nullptr);
#ifdef _WIN32
		_putenv_s("SR_BREAK_FATAL", "");
#else
		unsetenv("SR_BREAK_FATAL");
#endif
		Check("break empty continued past break", res.reason == StopReason::kSyscall ? 1 : 0, 1);
		Check("break empty reached target register setting", s.r[T2], 107);
	}

	// 4. SR_BREAK_FATAL unset (should continue)
	{
#ifdef _WIN32
		_putenv_s("SR_BREAK_FATAL", "");
#else
		unsetenv("SR_BREAK_FATAL");
#endif
		Memory mem;
		for (size_t i = 0; i < p.size(); i++)
			mem.Write32(base + (uint32_t)i * 4, p[i]);
		ref::CpuState s;
		s.pc = base;
		StepResult res = Run(&s, &mem, 1000, nullptr);
		Check("break unset continued past break", res.reason == StopReason::kSyscall ? 1 : 0, 1);
		Check("break unset reached target register setting", s.r[T2], 107);
	}
}

void TestSyscallCodePreservation() {
	const uint32_t base = 0x08900000;
	// Test syscall 0x123
	Memory mem1;
	uint32_t op1 = 0x000048ccu; // (0x123 << 6) | 0x0C
	mem1.Write32(base, op1);
	ref::CpuState s1;
	s1.pc = base;
	StepResult res1 = Run(&s1, &mem1, 1, nullptr);
	Check("syscall 0x123 reason is kSyscall", res1.reason == StopReason::kSyscall ? 1 : 0, 1);
	Check("syscall 0x123 op is preserved", res1.op, op1);
	Check("syscall 0x123 code is preserved", (res1.op >> 6) & 0xFFFFF, 0x123);

	// Test syscall 0x456
	Memory mem2;
	uint32_t op2 = 0x0001158cu; // (0x456 << 6) | 0x0C
	mem2.Write32(base, op2);
	ref::CpuState s2;
	s2.pc = base;
	StepResult res2 = Run(&s2, &mem2, 1, nullptr);
	Check("syscall 0x456 reason is kSyscall", res2.reason == StopReason::kSyscall ? 1 : 0, 1);
	Check("syscall 0x456 op is preserved", res2.op, op2);
	Check("syscall 0x456 code is preserved", (res2.op >> 6) & 0xFFFFF, 0x456);
}

void TestAllegrexByteBitOps() {
	const uint32_t base = 0x08900000;
	// wsbh, wsbw, seb, seh, bitrev on known patterns.
	std::vector<uint32_t> p = {
		I(0x0F, ZERO, T0, 0x1234), MTC1(T0, 0),   // reuse T0 path: t0 = 0x12340000
		I(0x0D, T0, T0, 0x5678),                  // t0 = 0x12345678
		(0x1Fu << 26) | (0u << 21) | (T0 << 16) | (T1 << 11) | (0x02u << 6) | 0x20u,  // wsbh t1, t0
		(0x1Fu << 26) | (0u << 21) | (T0 << 16) | (T2 << 11) | (0x03u << 6) | 0x20u,  // wsbw t2, t0
		(0x1Fu << 26) | (0u << 21) | (T0 << 16) | (T3 << 11) | (0x10u << 6) | 0x20u,  // seb  t3, t0
		(0x1Fu << 26) | (0u << 21) | (T0 << 16) | (T4 << 11) | (0x18u << 6) | 0x20u,  // seh  t4, t0
		(0x1Fu << 26) | (0u << 21) | (T0 << 16) | (T5 << 11) | (0x14u << 6) | 0x20u,  // bitrev t5, t0
		I(0x0D, ZERO, T0, 0x80F0),                                                     // t0 = 0x000080F0
		(0x1Fu << 26) | (0u << 21) | (T0 << 16) | (T6 << 11) | (0x10u << 6) | 0x20u,  // seb  t6, t0
		(0x1Fu << 26) | (0u << 21) | (T0 << 16) | (T7 << 11) | (0x18u << 6) | 0x20u,  // seh  t7, t0
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("wsbh 0x12345678", s.r[T1], 0x34127856u);
	Check("wsbw 0x12345678", s.r[T2], 0x78563412u);
	Check("seb  0x12345678", s.r[T3], 0x00000078u);
	Check("seh  0x12345678", s.r[T4], 0x00005678u);
	Check("bitrev 0x12345678", s.r[T5], 0x1E6A2C48u);
	// Sign bits set: a zero-extending seb or seh would give 0x000000F0 and 0x000080F0.
	Check("seb  0x000080F0", s.r[T6], 0xFFFFFFF0u);
	Check("seh  0x000080F0", s.r[T7], 0xFFFF80F0u);
}

// nk_ref_create returns null when the 64 MiB reference RAM cannot be allocated, so the snapshot
// runner, form sweep and verifier take their named out-of-memory stop. The RAM used to come
// from a throwing std::vector allocation inside a nothrow new of the small wrapper, so a
// failed allocation ended the process instead. A Memory without RAM faults on every access.
void TestSnapshotRefCreateReportsNoMemory() {
	g_fail_ram_alloc = true;
	NkRef *none = nk_ref_create();
	g_fail_ram_alloc = false;
	Check("nk_ref_create without RAM is null", none == nullptr ? 1u : 0u, 1u);
	if (none) nk_ref_destroy(none);
	NkRef *some = nk_ref_create();
	Check("nk_ref_create with RAM", some != nullptr ? 1u : 0u, 1u);
	if (some) nk_ref_destroy(some);

	g_fail_ram_alloc = true;
	Memory mem;
	g_fail_ram_alloc = false;
	mem.Write32(Memory::kRamBase, 1u);
	Check("Memory without RAM: store faults", mem.last_fault() ? 1u : 0u, 1u);
	mem.ClearFault();
	(void)mem.Read32(Memory::kRamBase);
	Check("Memory without RAM: load faults", mem.last_fault() ? 1u : 0u, 1u);
}

// nk_ref_run starts a call at entry_pc outside a delay slot. A caller's state taken inside a
// delay-slot window (the runtime keeps the branch PC in next_pc there, src/rt/cpu_lle.c) used
// to carry over, so the reference jumped to the stale next_pc after the first instruction.
void TestSnapshotRefRunStartsOutsideDelaySlot() {
	const uint32_t entry = 0x08900000;
	const uint32_t prog[] = {I(0x09, ZERO, T2, 0x111), I(0x09, ZERO, T3, 0x222), SYSCALL};
	NkRef *r = nk_ref_create();
	if (!r) {
		Check("nk_ref_create for nk_ref_run", 0u, 1u);
		return;
	}
	std::memcpy(nk_ref_ram(r) + (entry - NK_REF_RAM_BASE), prog, sizeof prog);

	ref::CpuState s;
	s.in_delay_slot = 1;
	s.next_pc = entry + 0x100;  // zero words there: nops into the step limit
	NkRefResult out;
	nk_ref_run(r, &s, entry, 0x0BAD0000u, nullptr, 0, 64, &out);
	Check("nk_ref_run slot state: stop", (uint32_t)out.stop, (uint32_t)NK_REF_SYSCALL);
	Check("nk_ref_run slot state: pc", out.pc, entry + 8);
	Check("nk_ref_run slot state: second insn", s.r[T3], 0x222u);

	// The first named import stub that the next fetch reaches ends the run there.
	ref::CpuState s2;
	const uint32_t stubs[] = {0x08A00000u, entry + 4, entry + 4};
	nk_ref_run(r, &s2, entry, 0x0BAD0000u, stubs, 3, 64, &out);
	Check("nk_ref_run import: stop", (uint32_t)out.stop, (uint32_t)NK_REF_IMPORT);
	Check("nk_ref_run import: pc", out.pc, entry + 4);
	Check("nk_ref_run import: executed", (uint32_t)out.executed, 1u);
	nk_ref_destroy(r);
}

void TestAllegrexExtIns() {
	const uint32_t base = 0x08900000;
	// ext: extract bits [15:8] from 0x12345678 -> 0x56
	// ins: insert 0xCD into bits [15:8] of 0x12345678 -> 0x1234CD78
	std::vector<uint32_t> p = {
		I(0x0F, ZERO, T0, 0x1234), I(0x0D, T0, T0, 0x5678),  // t0 = 0x12345678
		I(0x0F, ZERO, T1, 0x1234), I(0x0D, T1, T1, 0xABCD),  // t1 = 0x1234ABCD
		(0x1Fu << 26) | (T0 << 21) | (T3 << 16) | ((8-1) << 11) | (8 << 6) | 0x00u,  // ext t3, t0, 8, 8 -> 0x56
		R(T0, ZERO, T2, 0, 0x21),                                                    // addu t2, t0, zero -> t2 = 0x12345678
		(0x1Fu << 26) | (T1 << 21) | (T2 << 16) | ((8+8-1) << 11) | (8 << 6) | 0x04u,  // ins t2, t1, 8, 8
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("ext 8,8 from 0x12345678", s.r[T3], 0x56u);
	Check("ins 8,8 0xCD into 0x12345678", s.r[T2], 0x1234CD78u);
}

void TestAllegrexMaxMin() {
	const uint32_t base = 0x08900000;
	// max/min signed comparisons: -1 vs 0, 0 vs 1, INT32_MIN vs INT32_MAX
	std::vector<uint32_t> p = {
		I(0x09, ZERO, T0, 0xFFFF),  // t0 = -1 (addiu zero, t0, 0xFFFF)
		I(0x09, ZERO, T1, 0),       // t1 = 0
		R(T0, T1, T2, 0, 0x2C),     // max t2, t0, t1 -> 0
		R(T0, T1, T3, 0, 0x2D),     // min t3, t0, t1 -> -1

		I(0x09, ZERO, T4, 0),       // t4 = 0
		I(0x09, ZERO, T5, 1),       // t5 = 1
		R(T4, T5, T6, 0, 0x2C),     // max t6, t4, t5 -> 1
		R(T4, T5, T7, 0, 0x2D),     // min t7, t4, t5 -> 0

		I(0x0F, ZERO, T8, 0x8000), I(0x0D, T8, T8, 0x0000),  // t8 = INT32_MIN
		I(0x0F, ZERO, T9, 0x7FFF), I(0x0D, T9, T9, 0xFFFF),  // t9 = INT32_MAX
		R(T8, T9, T0, 0, 0x2C),     // max t0, t8, t9 -> INT32_MAX
		R(T8, T9, T1, 0, 0x2D),     // min t1, t8, t9 -> INT32_MIN
		SYSCALL,
	};
	ref::CpuState s = RunProgram(p, base);
	Check("max -1,0", s.r[T2], 0u);
	Check("min -1,0", s.r[T3], 0xFFFFFFFFu);
	Check("max 0,1", s.r[T6], 1u);
	Check("min 0,1", s.r[T7], 0u);
	Check("max INT32_MIN,INT32_MAX", s.r[T0], 0x7FFFFFFFu);
	Check("min INT32_MIN,INT32_MAX", s.r[T1], 0x80000000u);
}

// Load prog at base into a fresh Memory, seed s.pc and run up to budget instructions.
StepResult RunWords(Memory *mem, ref::CpuState *s, const std::vector<uint32_t> &prog, uint32_t base,
                    unsigned long long budget) {
	for (size_t i = 0; i < prog.size(); i++)
		mem->Write32(base + (uint32_t)i * 4, prog[i]);
	s->pc = base;
	return Run(s, mem, budget, nullptr);
}

// A jr/jalr target is latched at the transfer, before the link write (AGENTS.md section 7,
// PR #126): jalr t9, t9 jumps to t9's old value, as the generated code (_t = R(a) first) and
// the runtime interpreter do. It used to write the link first and jump to pc + 8.
void TestJalrLatchesTargetBeforeLink() {
	const uint32_t base = 0x08900000;
	const uint32_t target = 0x08900100;
	Memory mem;
	mem.Write32(target, I(0x09, ZERO, T2, 0x111));  // addiu t2, zero, 0x111
	mem.Write32(target + 4, SYSCALL);
	ref::CpuState s;
	s.r[T9] = target;
	StepResult res = RunWords(&mem, &s, {
		R(T9, 0, T9, 0, 0x09),        // jalr t9, t9
		0,                            // delay slot
		I(0x09, ZERO, T3, 0xBAD),     // pc + 8: must not run
		SYSCALL,
	}, base, 16);
	Check("jalr rd==rs stop", (uint32_t)res.reason, (uint32_t)StopReason::kSyscall);
	Check("jalr rd==rs took old target", s.r[T2], 0x111u);
	Check("jalr rd==rs skipped pc+8", s.r[T3], 0u);
	Check("jalr rd==rs link", s.r[T9], base + 8);
}

// The PSP ignores bits 1:0 of a jr/jalr target (pspautotests e37443a6
// tests/cpu/jump/unaligned.expected, recorded on a PSP): a jump to an aligned address + 1, + 2
// or + 3 runs the aligned word (`ran=003`), jalr links next+0 (`link=next+0`), and a bal there
// links from a PC with no low bits (`bal ra=next+0`). Cleared, a target of 3 is null.
void TestJumpTargetLowBitsIgnored() {
	const uint32_t base = 0x08900000;
	const uint32_t target = 0x08900100;
	for (uint32_t low = 1; low < 4; low++) {
		for (int link = 0; link < 2; link++) {
			Memory mem;
			mem.Write32(target, I(0x09, ZERO, T2, 0x111));     // addiu t2, zero, 0x111
			mem.Write32(target + 4, (0x01u << 26) | (0x11u << 16) | 1u);  // bal .+8
			mem.Write32(target + 8, 0);                          // delay slot
			mem.Write32(target + 12, SYSCALL);
			ref::CpuState s;
			s.r[T0] = target + low;
			StepResult res = RunWords(&mem, &s, {
				link ? R(T0, 0, T9, 0, 0x09) : R(T0, 0, 0, 0, 0x08),  // jalr t9, t0 / jr t0
				0,
				SYSCALL,
			}, base, 16);
			char what[64];
			const char *name = link ? "jalr" : "jr";
			std::snprintf(what, sizeof what, "%s +%u stop", name, low);
			Check(what, (uint32_t)res.reason, (uint32_t)StopReason::kSyscall);
			std::snprintf(what, sizeof what, "%s +%u stop pc", name, low);
			Check(what, res.pc, target + 12);
			std::snprintf(what, sizeof what, "%s +%u ran the aligned word", name, low);
			Check(what, s.r[T2], 0x111u);
			std::snprintf(what, sizeof what, "%s +%u bal link", name, low);
			Check(what, s.r[RA], target + 12);
			std::snprintf(what, sizeof what, "%s +%u link", name, low);
			Check(what, s.r[T9], link ? base + 8 : 0u);
			std::snprintf(what, sizeof what, "%s +%u target register kept", name, low);
			Check(what, s.r[T0], target + low);
		}
	}
	Memory mem;
	ref::CpuState s;
	s.r[T0] = 3;
	StepResult res = RunWords(&mem, &s, {R(T0, 0, 0, 0, 0x08), I(0x09, ZERO, T2, 1), SYSCALL},
	                          base, 8);
	Check("jr null + 3 stop", (uint32_t)res.reason, (uint32_t)StopReason::kMemoryFault);
	Check("jr null + 3 stop pc", res.pc, base);
	Check("jr null + 3 no slot", s.r[T2], 0u);

	// The low bits clear on the value latched at the transfer (PR #126), before the link
	// write and the delay slot: a jalr whose link register is its target register, and a jr
	// whose delay slot rewrites its target register, still reach the aligned old target.
	for (int form = 0; form < 2; form++) {
		Memory lm;
		lm.Write32(target, I(0x09, ZERO, T2, 0x111));  // addiu t2, zero, 0x111
		lm.Write32(target + 4, SYSCALL);
		ref::CpuState ls;
		ls.r[T0] = target + 2;
		StepResult lr = RunWords(&lm, &ls, {
			form ? R(T0, 0, 0, 0, 0x08) : R(T0, 0, T0, 0, 0x09),  // jr t0 / jalr t0, t0
			form ? I(0x09, ZERO, T0, 0x41) : 0u,                  // addiu t0, zero, 0x41 / nop
			SYSCALL,
		}, base, 8);
		const char *name = form ? "jr slot rewrites rs +2" : "jalr rd==rs +2";
		char what[64];
		std::snprintf(what, sizeof what, "%s stop", name);
		Check(what, (uint32_t)lr.reason, (uint32_t)StopReason::kSyscall);
		std::snprintf(what, sizeof what, "%s stop pc", name);
		Check(what, lr.pc, target + 4);
		std::snprintf(what, sizeof what, "%s ran the aligned old target", name);
		Check(what, ls.r[T2], 0x111u);
		std::snprintf(what, sizeof what, "%s register after", name);
		Check(what, ls.r[T0], form ? 0x41u : base + 8);
	}
}

// c.cond.s orders denormals from their raw bits, as sr_fpu_condition_s does for the generated
// code, whatever the host's ambient FP mode. With DAZ set in the process (a library may set
// it), a host float compare treats 1 ulp of denormal as 0: c.eq.s said equal and c.lt.s said
// not less, while the generated code says the opposite.
void TestCompareIgnoresAmbientDenormalMode() {
	struct Cell { const char *name; uint32_t a, b, funct, want; };
	const Cell cells[] = {
		{"c.eq.s denormal,+0", 0x00000001u, 0x00000000u, 0x32, 0},
		{"c.lt.s +0,denormal", 0x00000000u, 0x00000001u, 0x3C, 1},
		{"c.lt.s -denormal,+0", 0x80000001u, 0x00000000u, 0x3C, 1},
		{"c.eq.s -0,+0", 0x80000000u, 0x00000000u, 0x32, 1},
		{"c.lt.s -0,+0", 0x80000000u, 0x00000000u, 0x3C, 0},
		{"c.lt.s -2,-1", 0xC0000000u, 0xBF800000u, 0x3C, 1},
		{"c.le.s +inf,+inf", 0x7F800000u, 0x7F800000u, 0x3E, 1},
		{"c.un.s qnan,1", 0x7FC00000u, 0x3F800000u, 0x31, 1},
		{"c.ueq.s snan,1", 0x7F800001u, 0x3F800000u, 0x33, 1},
		{"c.olt.s 1,qnan", 0x3F800000u, 0x7FC00000u, 0x34, 0},
	};
#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)
	const unsigned saved = _mm_getcsr();
	_mm_setcsr(saved | 0x0040u);  // DAZ: denormal operands read as zero
	const bool daz = true;
#else
	const bool daz = false;
#endif
	for (const Cell &c : cells) {
		Memory mem;
		ref::CpuState s;
		s.fi[0] = c.a;
		s.fi[1] = c.b;
		StepResult res = RunWords(&mem, &s, {FPS(1, 0, 0, c.funct), SYSCALL}, 0x08900000, 4);
		char what[64];
		std::snprintf(what, sizeof what, "%s%s", c.name, daz ? " (DAZ)" : "");
		Check(what, s.fpcond, c.want);
		std::snprintf(what, sizeof what, "%s FCC0", c.name);
		Check(what, (s.fcr31 >> 23) & 1u, c.want);
		if (res.reason != StopReason::kSyscall) {
			std::printf("FAIL %-28s stopped with reason %d\n", c.name, (int)res.reason);
			g_failures++;
		}
	}
#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)
	_mm_setcsr(saved);
#endif
}

// An instruction that stops on an error does not complete: a faulting load leaves rt alone,
// ll leaves the link clear, pc names the instruction, a delay slot keeps its branch pending,
// and nothing is traced or counted. The reference used to write 0 to rt, set the link,
// advance pc, apply the branch and emit a trace line for an instruction that never ran
// (for a faulting store, a memory-write token for a write that never happened).
void TestErrorStopsDoNotCommit() {
	const uint32_t base = 0x08900000;
	const struct { uint32_t op; const char *name; StopReason why; } cases[] = {
		{I(0x23, A0, S0, 0), "lw fault", StopReason::kMemoryFault},
		{I(0x20, A0, S0, 0), "lb fault", StopReason::kMemoryFault},
		{I(0x30, A0, S0, 0), "ll fault", StopReason::kMemoryFault},
		{I(0x22, A0, S0, 1), "lwl fault", StopReason::kMemoryFault},
		{I(0x31, A0, 3, 0), "lwc1 fault", StopReason::kMemoryFault},
		{0xF0000000u, "unimplemented", StopReason::kUnimplemented},
		// A store to address 0 faults and writes nothing, so its trace has no m8/m16/m32
		// token: the reference used to trace a faulting sw as m32[0x00000000]=0x00000000.
		{I(0x2B, A0, S0, 0), "sw fault", StopReason::kMemoryFault},
		{I(0x29, A0, S0, 0), "sh fault", StopReason::kMemoryFault},
		{I(0x28, A0, S0, 0), "sb fault", StopReason::kMemoryFault},
		{I(0x2A, A0, S0, 1), "swl fault", StopReason::kMemoryFault},
	};
	for (const auto &c : cases) {
		Memory mem;
		ref::CpuState s;
		s.r[A0] = 0;  // outside modeled RAM
		s.r[S0] = 0x1234;
		s.fi[3] = 0x5678;
		char path[512];
		std::FILE *out = OpenScratch(path, sizeof path);
		if (out == nullptr) {
			std::printf("FAIL %-28s cannot open a scratch trace file\n", c.name);
			g_failures++;
			continue;
		}
		TraceSink sink(out);
		mem.Write32(base, c.op);
		s.pc = base;
		StepResult res = Run(&s, &mem, 4, &sink);
		std::fflush(out);
		const long traced = std::ftell(out);
		std::fclose(out);
		if (path[0] != '\0') std::remove(path);
		char what[64];
		std::snprintf(what, sizeof what, "%s stop", c.name);
		Check(what, (uint32_t)res.reason, (uint32_t)c.why);
		std::snprintf(what, sizeof what, "%s rt kept", c.name);
		Check(what, s.r[S0], 0x1234u);
		std::snprintf(what, sizeof what, "%s ft kept", c.name);
		Check(what, s.fi[3], 0x5678u);
		std::snprintf(what, sizeof what, "%s link clear", c.name);
		Check(what, s.llbit, 0u);
		std::snprintf(what, sizeof what, "%s pc", c.name);
		Check(what, s.pc, base);
		std::snprintf(what, sizeof what, "%s executed", c.name);
		Check(what, (uint32_t)res.executed, 0u);
		std::snprintf(what, sizeof what, "%s not traced", c.name);
		Check(what, (uint32_t)traced, 0u);
	}
	// A load fault in a delay slot: the branch completed, the slot did not.
	Memory mem;
	ref::CpuState s;
	s.r[S0] = 0x1234;
	StepResult res = RunWords(&mem, &s, {
		I(0x04, ZERO, ZERO, 3),       // beq zero, zero, +3
		I(0x23, A0, S0, 0),           // delay slot: lw s0, 0(a0) with a0 = 0 faults
	}, base, 8);
	Check("slot fault stop", (uint32_t)res.reason, (uint32_t)StopReason::kMemoryFault);
	Check("slot fault pc", s.pc, base + 4);
	Check("slot fault branch pending", s.in_delay_slot, 1u);
	Check("slot fault next_pc", s.next_pc, base + 16);
	Check("slot fault rt kept", s.r[S0], 0x1234u);
	Check("slot fault executed", (uint32_t)res.executed, 1u);
}

// A computed jr/jalr target of 0 is a null code pointer (#1375): the production lanes refuse
// it at the transfer, before the link write and the delay slot. The reference stops there with
// a memory fault at address 0; it used to link, run the slot and fault at the fetch of pc 0.
void TestNullCodePointerStopsAtTransfer() {
	const uint32_t base = 0x08900000;
	const struct { uint32_t op; const char *name; } cases[] = {
		{R(T0, 0, 0, 0, 0x08), "jr null"},
		{R(T0, 0, RA, 0, 0x09), "jalr null"},
	};
	for (const auto &c : cases) {
		Memory mem;
		ref::CpuState s;
		s.r[RA] = 0x0890AAAC;
		StepResult res = RunWords(&mem, &s, {c.op, I(0x09, ZERO, T2, 1), SYSCALL}, base, 8);
		char what[64];
		std::snprintf(what, sizeof what, "%s stop", c.name);
		Check(what, (uint32_t)res.reason, (uint32_t)StopReason::kMemoryFault);
		std::snprintf(what, sizeof what, "%s stop pc", c.name);
		Check(what, res.pc, base);
		std::snprintf(what, sizeof what, "%s fault addr", c.name);
		Check(what, mem.fault_addr(), 0u);
		std::snprintf(what, sizeof what, "%s no link", c.name);
		Check(what, s.r[RA], 0x0890AAACu);
		std::snprintf(what, sizeof what, "%s no slot", c.name);
		Check(what, s.r[T2], 0u);
		std::snprintf(what, sizeof what, "%s executed", c.name);
		Check(what, (uint32_t)res.executed, 0u);
	}
}

// Encodings with a reserved field set stop as unimplemented, as the runtime interpreter's
// decode and the generator refuse them; the reference used to run them as the plain form.
void TestReservedFieldsStop() {
	const uint32_t base = 0x08900000;
	const uint32_t target = 0x08900100;
	const struct { uint32_t op; const char *name; } cases[] = {
		{R(T0, 1, 0, 0, 0x08), "jr rt!=0"},
		{R(T0, 0, 0, 1, 0x08), "jr sa!=0"},
		{R(T0, 0, 1, 0, 0x08), "jr rd!=0"},
		{R(T0, 1, RA, 0, 0x09), "jalr rt!=0"},
		{R(T0, 0, RA, 1, 0x09), "jalr sa!=0"},
		{I(0x06, T1, 1, 2), "blez rt!=0"},
		{I(0x07, T1, 1, 2), "bgtz rt!=0"},
		{I(0x16, T1, 1, 2), "blezl rt!=0"},
		{I(0x17, T1, 1, 2), "bgtzl rt!=0"},
		{R(2, T1, T2, 4, 0x02), "srl rs=2"},
		{R(T1, T1, T2, 2, 0x06), "srlv sa=2"},
	};
	for (const auto &c : cases) {
		Memory mem;
		ref::CpuState s;
		s.r[T0] = target;
		s.r[T1] = 0x80;
		StepResult res = RunWords(&mem, &s, {c.op, 0, SYSCALL}, base, 8);
		char what[64];
		std::snprintf(what, sizeof what, "%s stop", c.name);
		Check(what, (uint32_t)res.reason, (uint32_t)StopReason::kUnimplemented);
		std::snprintf(what, sizeof what, "%s stop pc", c.name);
		Check(what, res.pc, base);
		std::snprintf(what, sizeof what, "%s executed", c.name);
		Check(what, (uint32_t)res.executed, 0u);
	}
	// The defined forms still run.
	Memory mem;
	ref::CpuState s;
	s.r[T1] = 0x80;
	RunWords(&mem, &s, {R(1, T1, T2, 4, 0x02), R(T1, T1, T3, 1, 0x06), SYSCALL}, base, 8);
	Check("rotr still runs", s.r[T2], 0x08u);
	Check("rotrv still runs", s.r[T3], 0x80u);
}

// A jump or branch in a delay slot is UNPREDICTABLE; the production lanes refuse it before
// any effect. The reference stops on it by name; it used to jump to the inner target, drop the
// outer one and skip the inner slot.
void TestControlTransferInDelaySlotStops() {
	const uint32_t base = 0x08900000;
	const struct { uint32_t op; const char *name; } cases[] = {
		{I(0x04, ZERO, ZERO, 8), "beq in slot"},
		{I(0x05, ZERO, ZERO, 8), "bne (not taken) in slot"},
		{(0x02u << 26) | ((0x08900200u >> 2) & 0x3FFFFFFu), "j in slot"},
		{R(T0, 0, 0, 0, 0x08), "jr in slot"},
		{BC1(1, 0, 8), "bc1t in slot"},
	};
	for (const auto &c : cases) {
		Memory mem;
		ref::CpuState s;
		s.r[T0] = 0x08900300;
		StepResult res = RunWords(&mem, &s, {
			I(0x04, ZERO, ZERO, 3),   // beq zero, zero, +3
			c.op,                     // its delay slot
			SYSCALL,
		}, base, 8);
		char what[64];
		std::snprintf(what, sizeof what, "%s stop", c.name);
		Check(what, (uint32_t)res.reason, (uint32_t)StopReason::kUnimplemented);
		std::snprintf(what, sizeof what, "%s stop pc", c.name);
		Check(what, res.pc, base + 4);
		std::snprintf(what, sizeof what, "%s executed", c.name);
		Check(what, (uint32_t)res.executed, 1u);
	}
}

// Behaviour pins for two cleanups: mul.s of inf and 0 (either order, either sign) is the
// canonical NaN 0x7fc00000 now that sr_fpu_mul_s alone classifies it, and cfc1 of FCR0 reads
// the FIR word through the shared SR_FCR0_FIR name. Both held before the cleanups too.
void TestMulInfZeroAndFirUnchanged() {
	const struct { const char *name; uint32_t a, b; } cells[] = {
		{"mul.s +inf*+0", 0x7F800000u, 0x00000000u},
		{"mul.s +0*+inf", 0x00000000u, 0x7F800000u},
		{"mul.s -inf*+0", 0xFF800000u, 0x00000000u},
		{"mul.s +inf*-0", 0x7F800000u, 0x80000000u},
	};
	for (const auto &c : cells) {
		Memory mem;
		ref::CpuState s;
		s.fi[1] = c.a;
		s.fi[2] = c.b;
		RunWords(&mem, &s, {FPS(2, 1, 3, 0x02), SYSCALL}, 0x08900000, 4);  // mul.s f3, f1, f2
		Check(c.name, s.fi[3], 0x7FC00000u);
	}
	Memory mem;
	ref::CpuState s;
	s.r[T0] = 0xDEADBEEFu;
	s.r[T1] = 0xDEADBEEFu;
	RunWords(&mem, &s, {
		(0x11u << 26) | (0x02u << 21) | (T0 << 16) | (0u << 11),  // cfc1 t0, $0
		(0x11u << 26) | (0x02u << 21) | (T1 << 16) | (1u << 11),  // cfc1 t1, $1
		SYSCALL,
	}, 0x08900000, 4);
	Check("cfc1 $0 is the FIR word", s.r[T0], 0x00003351u);
	Check("cfc1 $1 reads 0", s.r[T1], 0u);
}

}  // namespace

int main() {
	// BREAK's policy reads SR_BREAK_FATAL (interp.cpp). An inherited value must not change the
	// break cases; TestBreakFatalMode sets it itself.
#ifdef _WIN32
	_putenv_s("SR_BREAK_FATAL", "");
#else
	unsetenv("SR_BREAK_FATAL");
#endif
	TestArithmetic();
	TestMulDiv();
	TestDivPspEdgeCases();
	TestMemory();
	TestUnalignedWordAccess();
	TestMisalignedAccessFaults();
	TestUnalignedStoreTraceRecordsTheAlignedWord();
	TestAllegrexRotate();
	TestBranchDelaySlot();
	TestDaybreak4GuestInterpreterOracle();
	TestReferenceArithmeticGuards();
	TestFpu();
	TestFpuConvertEdgeCases();
	TestFpuCompareBranch();
	TestFpuConditionCodesMeasured();
	TestFcr31UnwritableBitsRecorded();
	TestLlScMeasuredWindows();
	TestArithmeticShifts();
	TestR0IsZero();
	TestBreakContinues();
	TestBreakCodePreservation();
	TestBreakFatalMode();
	TestSyscallCodePreservation();
	TestAllegrexByteBitOps();
	TestSnapshotRefCreateReportsNoMemory();
	TestSnapshotRefRunStartsOutsideDelaySlot();
	TestAllegrexExtIns();
	TestAllegrexMaxMin();
	TestJalrLatchesTargetBeforeLink();
	TestJumpTargetLowBitsIgnored();
	TestCompareIgnoresAmbientDenormalMode();
	TestErrorStopsDoNotCommit();
	TestNullCodePointerStopsAtTransfer();
	TestReservedFieldsStop();
	TestControlTransferInDelaySlotStops();
	TestMulInfZeroAndFirUnchanged();
	std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
	return g_failures == 0 ? 0 : 1;
}
