// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-10.
// See NOTICE.md for upstream lineage and modification provenance.

// with the exact entry register state captured by the PPSSPP oracle (the "# init" line in
// the oracle trace). The emitted trace is then diffed against the oracle with
// tools/tracediff.py to locate the first divergence in the reference interpreter.
//
// Usage: run_elf <elf> <oracle-trace> <out-trace> [max_steps]
//
// This is a bring-up driver, not the loader of record (that is Phase 2). It handles only
// PT_LOAD of a non-relocated ELF, which is what the golden homebrew is.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "interp.h"

using namespace ref;

namespace {

// The runtime's host return address (SR_HOST_RETURN_PC, src/rt/recomp.h); a test keeps the
// two equal.
constexpr uint32_t kHostReturnPc = 0xFFFFFFF0u;

std::vector<uint8_t> ReadFile(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
	if (fseek(f, 0, SEEK_END) != 0) { fprintf(stderr, "cannot seek %s\n", path); fclose(f); exit(2); }
	long n = ftell(f);
	if (n < 0) { fprintf(stderr, "cannot size %s\n", path); fclose(f); exit(2); }
	if (fseek(f, 0, SEEK_SET) != 0) { fprintf(stderr, "cannot rewind %s\n", path); fclose(f); exit(2); }
	size_t size = static_cast<size_t>(n);
	if (static_cast<long>(size) != n) { fprintf(stderr, "file too large %s\n", path); fclose(f); exit(2); }
	std::vector<uint8_t> data(size);
	if (size > 0 && fread(data.data(), 1, size, f) != size) { fprintf(stderr, "short read %s\n", path); fclose(f); exit(2); }
	fclose(f);
	return data;
}

uint32_t Rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
uint16_t Rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

bool RangeWithin(size_t total, uint64_t offset, uint64_t length) {
	return offset <= total && length <= static_cast<uint64_t>(total) - offset;
}

enum class GuestRangeStatus { kMapped, kUnmapped, kInvalid };

bool GuestRangeOverlapsModeledRam(uint32_t addr, uint32_t length) {
	if (length == 0) return false;
	uint64_t end = static_cast<uint64_t>(addr) + length;
	if (end > (static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1)) return true;
	for (uint64_t alias = 0; alias <= 0xe0000000ULL; alias += 0x20000000ULL) {
		uint64_t base = alias + Memory::kRamBase;
		uint64_t limit = base + Memory::kRamSize;
		if (static_cast<uint64_t>(addr) < limit && end > base) return true;
	}
	return false;
}

GuestRangeStatus ClassifyGuestRange(Memory *mem, uint32_t addr, uint32_t length, uint32_t *index) {
	if (!mem || !index) return GuestRangeStatus::kInvalid;
	if (length == 0) {
		*index = 0;
		return GuestRangeStatus::kUnmapped;
	}
	uint64_t last64 = static_cast<uint64_t>(addr) + length - 1;
	if (last64 > std::numeric_limits<uint32_t>::max()) return GuestRangeStatus::kInvalid;
	uint32_t first_index = 0, last_index = 0;
	bool first_mapped = mem->Translate(addr, &first_index);
	bool last_mapped = mem->Translate(static_cast<uint32_t>(last64), &last_index);
	if (!first_mapped && !last_mapped)
		return GuestRangeOverlapsModeledRam(addr, length) ? GuestRangeStatus::kInvalid : GuestRangeStatus::kUnmapped;
	if (!first_mapped || !last_mapped) return GuestRangeStatus::kInvalid;
	if (last_index < first_index) return GuestRangeStatus::kInvalid;
	if (static_cast<uint64_t>(first_index) + length > Memory::kRamSize) return GuestRangeStatus::kInvalid;
	*index = first_index;
	return GuestRangeStatus::kMapped;
}

[[noreturn]] void ElfError(const char *message) {
	fprintf(stderr, "%s\n", message);
	exit(2);
}

// Load PT_LOAD segments into guest memory. Returns the ELF entry point.
uint32_t LoadElf(const std::vector<uint8_t> &elf, Memory *mem) {
	if (!mem) ElfError("null memory");
	if (elf.size() < 52 || memcmp(elf.data(), "\x7f""ELF", 4) != 0) ElfError("not an ELF");
	// The fields below are read with the ELF32 little-endian layout, and nothing here
	// relocates: anything but a 32-bit little-endian MIPS executable is refused by name
	// (a PRX, e_type 0xFFA0, would otherwise run unrelocated).
	if (elf[4] != 1) ElfError("ELF class is not ELFCLASS32");
	if (elf[5] != 1) ElfError("ELF data encoding is not little-endian");
	if (Rd16(&elf[16]) != 2) ElfError("ELF type is not ET_EXEC (only a non-relocated executable is loaded)");
	if (Rd16(&elf[18]) != 8) ElfError("ELF machine is not EM_MIPS");
	uint32_t e_entry = Rd32(&elf[24]);
	uint32_t e_phoff = Rd32(&elf[28]);
	uint32_t e_shoff = Rd32(&elf[32]);
	uint16_t e_phentsize = Rd16(&elf[42]);
	uint16_t e_phnum = Rd16(&elf[44]);
	uint16_t e_shentsize = Rd16(&elf[46]);
	uint16_t e_shnum = Rd16(&elf[48]);
	uint16_t e_shstrndx = Rd16(&elf[50]);

	if (e_phnum != 0 && e_phentsize < 32) ElfError("ELF program header entry too small");
	uint64_t ph_table_size = static_cast<uint64_t>(e_phentsize) * e_phnum;
	if (!RangeWithin(elf.size(), e_phoff, ph_table_size)) ElfError("ELF program header table out of range");

	if (e_shnum != 0 || e_shoff != 0) {
		if (e_shnum != 0 && e_shentsize < 40) ElfError("ELF section header entry too small");
		uint64_t sh_table_size = static_cast<uint64_t>(e_shentsize) * e_shnum;
		if (!RangeWithin(elf.size(), e_shoff, sh_table_size)) ElfError("ELF section header table out of range");
		if (e_shnum != 0 && e_shstrndx >= e_shnum) ElfError("ELF section header string index out of range");
		for (uint16_t i = 0; i < e_shnum; i++) {
			uint64_t sh_offset = static_cast<uint64_t>(e_shoff) + static_cast<uint64_t>(i) * e_shentsize;
			if (!RangeWithin(elf.size(), sh_offset, 40)) ElfError("ELF section header out of range");
			const uint8_t *sh = elf.data() + static_cast<size_t>(sh_offset);
			uint32_t sh_type = Rd32(sh + 4);
			uint32_t sh_offset_val = Rd32(sh + 16);
			uint32_t sh_size = Rd32(sh + 20);
			if (sh_type != 8 && !RangeWithin(elf.size(), sh_offset_val, sh_size)) {
				ElfError("ELF section data out of range");
			}
		}
	}

	if ((e_entry & 3) != 0) ElfError("ELF entry address unaligned");
	uint32_t entry_index = 0;
	GuestRangeStatus entry_status = ClassifyGuestRange(mem, e_entry, 4, &entry_index);
	if (entry_status != GuestRangeStatus::kMapped) ElfError("ELF entry address out of mapped guest memory");

	for (uint16_t i = 0; i < e_phnum; i++) {
		uint64_t ph_offset = static_cast<uint64_t>(e_phoff) + static_cast<uint64_t>(i) * e_phentsize;
		if (!RangeWithin(elf.size(), ph_offset, 32)) ElfError("ELF program header out of range");
		const uint8_t *ph = elf.data() + static_cast<size_t>(ph_offset);
		uint32_t p_type = Rd32(ph + 0);
		uint32_t p_offset = Rd32(ph + 4);
		uint32_t p_vaddr = Rd32(ph + 8);
		uint32_t p_filesz = Rd32(ph + 16);
		uint32_t p_memsz = Rd32(ph + 20);
		uint32_t p_flags = Rd32(ph + 24);
		if (p_type != 1)  // PT_LOAD
			continue;
		if (p_filesz > p_memsz) ElfError("ELF PT_LOAD filesz exceeds memsz");
		if (!RangeWithin(elf.size(), p_offset, p_filesz)) ElfError("ELF PT_LOAD data out of range");
		uint32_t guest_index = 0;
		GuestRangeStatus guest_range = ClassifyGuestRange(mem, p_vaddr, p_memsz, &guest_index);
		if (guest_range == GuestRangeStatus::kInvalid) ElfError("ELF PT_LOAD guest range crosses modeled memory");
		if (guest_range == GuestRangeStatus::kUnmapped) {
			if (p_memsz == 0) continue;
			// Code outside modeled memory would never run as the ELF says, so it is refused.
			// Another segment there (GNU ld puts the ELF headers in a read-only PT_LOAD at
			// 0x00400000 when only -Ttext moves the code) is skipped, and the skip is named.
			if ((p_flags & 1u) != 0) ElfError("ELF executable PT_LOAD outside modeled guest memory");
			fprintf(stderr, "run_elf: program header %u (PT_LOAD at 0x%08x, memsz 0x%x) is outside modeled guest memory; skipped\n",
			        static_cast<unsigned>(i), p_vaddr, p_memsz);
			continue;
		}
		if (p_filesz != 0)
			memcpy(mem->RamData() + guest_index, elf.data() + p_offset, p_filesz);
		if (p_memsz > p_filesz)
			memset(mem->RamData() + guest_index + p_filesz, 0, p_memsz - p_filesz);
	}
	return e_entry;
}

bool ParseIndexedRegister(const char *name, char prefix, uint32_t *index, uint32_t max_index = 31) {
	if (!name || !index || name[0] != prefix) return false;
	if (name[1] < '0' || name[1] > '9') return false;
	char *end = nullptr;
	errno = 0;
	unsigned long long value = strtoull(name + 1, &end, 10);
	if (errno != 0 || !end || end == name + 1 || *end != '\0' || value > max_index) return false;
	*index = static_cast<uint32_t>(value);
	return true;
}

bool ParseHex32(const char *text, uint32_t *value) {
	if (!text || !value || *text == '\0' || *text == '-' || *text == '+' || *text == ' ' || *text == '\t') return false;
	const char *start = text;
	if (start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) {
		start += 2;
	}
	if (!((*start >= '0' && *start <= '9') || (*start >= 'a' && *start <= 'f') || (*start >= 'A' && *start <= 'F'))) {
		return false;
	}
	char *end = nullptr;
	errno = 0;
	unsigned long long parsed = strtoull(start, &end, 16);
	if (errno != 0 || !end || end == start || *end != '\0' || parsed > static_cast<unsigned long long>(std::numeric_limits<uint32_t>::max())) return false;
	*value = static_cast<uint32_t>(parsed);
	return true;
}

// A token of the init line that cannot be seeded stops the run by name: a register left at
// its default would make the first divergence look like an interpreter mismatch.
[[noreturn]] void InitTokenError(const char *trace_path, const char *what, const char *token) {
	fprintf(stderr, "%s: '# init' line has a %s: '%s'; refusing a partly seeded state\n",
	        trace_path, what, token);
	exit(2);
}

// Parse the "# init r0=.. ... fcr31=.. f0=.. .." line from the oracle trace into a CpuState.
// Every token must be <register>=<hex32> for r0-r31 (r0 only as zero), f0-f31, v0-v127 (as
// src/rt/driver.c seeds them), hi, lo or fcr31. A carriage return is a delimiter, so a CRLF
// trace seeds its last register too.
bool SeedFromInit(const char *trace_path, CpuState *s) {
	static const char kDelimiters[] = " \t\r\n";
	FILE *f = fopen(trace_path, "rb");
	if (!f) { fprintf(stderr, "cannot open %s\n", trace_path); exit(2); }
	char buf[8192];
	bool found = false;
	while (fgets(buf, sizeof(buf), f)) {
		if (strncmp(buf, "# init", 6) != 0)
			continue;
		found = true;
		char *tok = strtok(buf + 6, kDelimiters);
		while (tok) {
			char *eq = strchr(tok, '=');
			if (!eq) InitTokenError(trace_path, "token without a value", tok);
			*eq = 0;
			uint32_t val = 0, index = 0;
			const char *name = tok;
			if (!ParseHex32(eq + 1, &val)) {
				*eq = '=';
				InitTokenError(trace_path, "malformed value", tok);
			}
			if (ParseIndexedRegister(name, 'r', &index)) {
				if (index == 0 && val != 0) {
					*eq = '=';
					InitTokenError(trace_path, "nonzero r0", tok);
				}
				s->r[index] = val;
			} else if (ParseIndexedRegister(name, 'f', &index)) {
				s->fi[index] = val;
			} else if (ParseIndexedRegister(name, 'v', &index, 127)) {
				s->vi[index] = val;
			} else if (strcmp(name, "hi") == 0) {
				s->hi = val;
			} else if (strcmp(name, "lo") == 0) {
				s->lo = val;
			} else if (strcmp(name, "fcr31") == 0) {
				s->fcr31 = val;
			} else {
				*eq = '=';
				InitTokenError(trace_path, "unknown register", tok);
			}
			tok = strtok(nullptr, kDelimiters);
		}
		// fpcond is the interpreter's cached copy of FCR31 FCC0 (bit 23, cpu.h); bc1t and
		// bc1f read only the copy, so it follows the seeded fcr31.
		s->fpcond = (s->fcr31 >> 23) & 1u;
		break;
	}
	fclose(f);
	return found;
}

const char *ReasonName(StopReason r) {
	switch (r) {
		case StopReason::kRunning: return "running";
		case StopReason::kSyscall: return "syscall";
		case StopReason::kUnimplemented: return "unimplemented";
		case StopReason::kMemoryFault: return "memory-fault";
		case StopReason::kBreak: return "break";
		case StopReason::kStepLimit: return "step-limit";
	}
	return "?";
}

// The entry function returning to the host return address ends the run as a return. The
// interpreter sees it as a fetch fault at kHostReturnPc, which no instruction can occupy
// (it is outside modeled memory), so that stop is renamed and every other fault keeps its
// memory-fault name.
constexpr const char *kReturnedStop = "returned";

const char *StopName(const StepResult &res) {
	if (res.reason == StopReason::kMemoryFault && res.pc == kHostReturnPc) return kReturnedStop;
	return ReasonName(res.reason);
}

// Calls f with every stop name run_elf can report, in order: each StopReason after kRunning
// (through kStepLimit, the last) by its ReasonName, then "returned". --expect-stop and the
// usage text both come from here, so they cannot drift from what a run prints.
template <typename F> void ForEachStopName(F f) {
	for (int r = static_cast<int>(StopReason::kRunning) + 1; r <= static_cast<int>(StopReason::kStepLimit); r++)
		f(ReasonName(static_cast<StopReason>(r)));
	f(kReturnedStop);
}

// Process statuses. A successful exit means the observed stop matched what the
// caller declared; unsupported execution, a memory fault and a break each get a
// distinct nonzero status so an automation caller cannot confuse them with an
// expected guest exit or an expected bounded run. A guest BREAK stops the run only
// while SR_BREAK_FATAL is set (as for the runtime's sr_break); without it the break is
// logged and execution continues, so kExitBreak is reachable only with the variable set.
enum ExitStatus {
	kExitExpected = 0,
	kExitUnexpectedStop = 1,
	kExitUsage = 2,
	kExitUnimplemented = 3,
	kExitMemoryFault = 4,
	kExitBreak = 5,
	kExitTraceWrite = 6,  // the output trace could not be written completely
	kExitNoMemory = 7,    // the host could not supply the reference RAM
};

int ExitForUnexpectedStop(StopReason r) {
	switch (r) {
		case StopReason::kUnimplemented: return kExitUnimplemented;
		case StopReason::kMemoryFault: return kExitMemoryFault;
		case StopReason::kBreak: return kExitBreak;
		default: return kExitUnexpectedStop;
	}
}

bool IsStopReasonName(const char *name) {
	if (name == nullptr) return false;
	bool known = false;
	ForEachStopName([&](const char *stop) { known = known || std::strcmp(name, stop) == 0; });
	return known;
}

// Finish the output trace: a write that failed earlier (the sink's fwrite is unchecked) or
// a failed close leaves a truncated trace, which must not pass for a complete one.
bool CloseTrace(FILE *out, const char *path) {
	const bool write_failed = ferror(out) != 0;
	const bool close_failed = fclose(out) != 0;
	if (!write_failed && !close_failed) return true;
	fprintf(stderr, "run_elf: trace write failed for %s; the trace is incomplete\n", path);
	return false;
}

// The trace header names the loaded ELF by its file name.
const char *TargetName(const char *elf_path) {
	const char *name = elf_path;
	for (const char *p = elf_path; *p != '\0'; p++)
		if (*p == '/' || *p == '\\') name = p + 1;
	return name;
}

// Validate the instruction budget instead of trusting strtoull: "abc", "-1", "0"
// and an oversized value must be named errors, not a silent zero-step or wrapped run.
bool ParseMaxSteps(const char *text, unsigned long long *out) {
	if (text == nullptr || *text == '\0') return false;
	for (const char *p = text; *p != '\0'; p++) {
		if (*p < '0' || *p > '9') return false;
	}
	if (std::strlen(text) > 20) return false;
	unsigned long long value = 0;
	for (const char *p = text; *p != '\0'; p++) {
		unsigned long long digit = static_cast<unsigned long long>(*p - '0');
		if (value > (0xFFFFFFFFFFFFFFFFULL - digit) / 10ULL) return false;
		value = value * 10ULL + digit;
	}
	if (value == 0) return false;
	*out = value;
	return true;
}

void PrintUsage() {
	fprintf(stderr,
	        "usage: run_elf <elf> <oracle-trace> <out-trace> [max_steps] "
	        "[--expect-stop=<reason>]\n"
	        "  max_steps      positive decimal instruction budget (default 2000000)\n"
	        "  --expect-stop  ");
	const char *separator = "";
	ForEachStopName([&](const char *stop) { fprintf(stderr, "%s%s", separator, stop); separator = "|"; });
	fprintf(stderr,
	        "\n"
	        "exit 0 only when the observed stop matches the expectation; without\n"
	        "--expect-stop only a guest syscall exit succeeds.\n");
}

}  // namespace

#ifndef SR_SELFTEST_ONLY
int main(int argc, char **argv) {
	const char *expect_reason = nullptr;
	const char *positional[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
	int positional_count = 0;
	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];
		if (std::strncmp(arg, "--expect-stop=", 14) == 0) {
			expect_reason = arg + 14;
		} else if (std::strcmp(arg, "--expect-stop") == 0 && i + 1 < argc) {
			expect_reason = argv[++i];
		} else if (std::strncmp(arg, "--", 2) == 0) {
			fprintf(stderr, "unknown option %s\n", arg);
			PrintUsage();
			return kExitUsage;
		} else {
			if (positional_count >= 5) {
				PrintUsage();
				return kExitUsage;
			}
			positional[positional_count++] = arg;
		}
	}
	if (expect_reason != nullptr && !IsStopReasonName(expect_reason)) {
		fprintf(stderr, "unknown --expect-stop value '%s'\n", expect_reason);
		PrintUsage();
		return kExitUsage;
	}
	if (positional_count < 3 || positional_count > 4) {
		PrintUsage();
		return kExitUsage;
	}
	const char *elf_path = positional[0];
	const char *oracle_trace = positional[1];
	const char *out_path = positional[2];
	unsigned long long max_steps = 2000000ULL;
	if (positional_count == 4 && !ParseMaxSteps(positional[3], &max_steps)) {
		fprintf(stderr,
		        "invalid max_steps '%s': expected a positive decimal instruction budget\n",
		        positional[3]);
		return kExitUsage;
	}

	Memory mem;
	// Without its RAM every access faults, so the ELF load would report a false layout
	// fault; the real cause is named here instead.
	if (!mem.ok()) {
		fprintf(stderr, "run_elf: cannot allocate the %u MiB reference RAM\n",
		        static_cast<unsigned>(Memory::kRamSize >> 20));
		return kExitNoMemory;
	}
	std::vector<uint8_t> elf = ReadFile(elf_path);
	uint32_t entry = LoadElf(elf, &mem);

	CpuState s;
	// The module is entered as a host entry, as src/rt/driver.c's trace harness enters it
	// (#1375): an entry $ra the init line does not record is the host return address,
	// SR_HOST_RETURN_PC in src/rt/recomp.h, so both sides of a comparison start alike. A
	// recorded r31 replaces it.
	s.r[31] = kHostReturnPc;
	if (!SeedFromInit(oracle_trace, &s)) {
		fprintf(stderr, "no '# init' line in %s; rebuild oracle trace with init dump\n", oracle_trace);
		return kExitUsage;
	}
	s.pc = entry;

	FILE *out = fopen(out_path, "wb");
	if (!out) { fprintf(stderr, "cannot open %s for write\n", out_path); return kExitUsage; }
	TraceSink sink(out);
	sink.Header(TargetName(elf_path), entry);
	sink.InitDump(&s);

	StepResult res = Run(&s, &mem, max_steps, &sink);
	if (!CloseTrace(out, out_path)) return kExitTraceWrite;

	const char *ppm_path = getenv("SR_PPM_DUMP");
	if (ppm_path) {
		uint32_t fb_addr = 0x09000000;
		FILE *pf = fopen(ppm_path, "wb");
		if (pf) {
			fprintf(pf, "P6\n64 64\n255\n");
			for (int i = 0; i < 64 * 64; i++) {
				uint32_t pixel = mem.Read32(fb_addr + i * 4);
				uint8_t r = pixel & 0xFF;
				uint8_t g = (pixel >> 8) & 0xFF;
				uint8_t b = (pixel >> 16) & 0xFF;
				fputc(r, pf);
				fputc(g, pf);
				fputc(b, pf);
			}
			fclose(pf);
			fprintf(stderr, "[run_elf] Framebuffer PPM dumped to %s\n", ppm_path);
		}
	}

	const char *observed = StopName(res);
	fprintf(stderr, "stopped: reason=%s pc=0x%08x op=0x%08x\n", observed, res.pc, res.op);
	// Structured stop metadata: the process status alone cannot distinguish a run that
	// stopped because the caller's budget ran out from one where the guest exited, so
	// callers assert both the declared expectation and the executed instruction count.
	fprintf(stderr,
	        "run_elf: stop_reason=%s pc=0x%08x op=0x%08x executed=%llu expected=%s\n",
	        observed, res.pc, res.op, static_cast<unsigned long long>(res.executed),
	        expect_reason != nullptr ? expect_reason : "none");

	if (expect_reason != nullptr) {
		if (std::strcmp(observed, expect_reason) != 0) {
			fprintf(stderr, "run_elf: UNEXPECTED_STOP expected=%s observed=%s\n",
			        expect_reason, observed);
			return kExitUnexpectedStop;
		}
		return kExitExpected;
	}
	if (res.reason != StopReason::kSyscall) {
		fprintf(stderr,
		        "run_elf: UNEXPECTED_STOP expected=syscall observed=%s; pass "
		        "--expect-stop=<reason> to assert a bounded or faulting stop\n",
		        observed);
		// A return is not a fault: it keeps the plain unexpected-stop status, and only a
		// wild access gets the memory-fault one.
		if (std::strcmp(observed, kReturnedStop) == 0) return kExitUnexpectedStop;
		return ExitForUnexpectedStop(res.reason);
	}
	return kExitExpected;
}
#endif
