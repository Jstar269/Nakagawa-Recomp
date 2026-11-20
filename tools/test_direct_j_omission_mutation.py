# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Compiled mutation proof for omitted direct-j execution boundaries."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
from typing import NamedTuple
import unittest

try:
    import scratch_dir
except ModuleNotFoundError:
    from tools import scratch_dir


ROOT = Path(__file__).resolve().parent.parent
CODEGEN = ROOT / "tools" / "codegen.py"
CC = shutil.which("gcc")


class _Outcome(NamedTuple):
    """Each stage of one fixture run, separately: codegen, the C compile, then the run.

    A stage that did not run (a failed stage stops the rest) is None, so a compile
    failure can never be mistaken for a failing run."""

    gen_rc: int
    gen_out: str
    cc_rc: int | None
    cc_out: str
    run_rc: int | None
    run_out: str


def _synthetic_elf(words: dict[int, int], entry: int) -> bytes:
    """Build the small executable used by the direct-j omission campaign."""
    import struct

    lo = min(words)
    hi = max(words) + 4
    text_size = hi - lo
    data_addr = (hi + 0xFF) & ~0xFF
    segment = bytearray(data_addr - lo + 4)
    for address, word in words.items():
        struct.pack_into("<I", segment, address - lo, word & 0xFFFFFFFF)
    data_off = 0x100
    shstr = b"\0.text\0.data\0.shstrtab\0"
    shstr_off = data_off + len(segment)
    shoff = (shstr_off + len(shstr) + 3) & ~3
    blob = bytearray(shoff + 4 * 40)
    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
    blob[:52] = ident + struct.pack(
        "<HHIIIIIHHHHHH", 2, 8, 1, entry, 52, shoff, 0x50001000,
        52, 32, 1, 40, 4, 3,
    )
    blob[52:84] = struct.pack(
        "<8I", 1, data_off, lo, lo, len(segment), len(segment), 7, 0x1000
    )
    blob[data_off:data_off + len(segment)] = segment
    blob[shstr_off:shstr_off + len(shstr)] = shstr
    sections = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (1, 1, 6, lo, data_off, text_size, 0, 0, 4, 0),
        (7, 1, 3, data_addr, data_off + data_addr - lo, 4, 0, 0, 4, 0),
        (13, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0),
    ]
    for index, section in enumerate(sections):
        struct.pack_into("<10I", blob, shoff + index * 40, *section)
    return bytes(blob)


def _write_harness(work: Path, target: int, owner: int) -> None:
    (work / "recomp.h").write_text(
        """\
#ifndef TEST_RECOMP_H
#define TEST_RECOMP_H
#include <stdint.h>
#define SR_CPUSTATE_ABI_VERSION 3u
typedef struct CpuState { uint32_t r[32]; uint32_t pc; } CpuState;
uint32_t test_mem_r32(uint32_t addr);
void test_mem_w32(uint32_t addr, uint32_t value);
#define MEM_R32(a) test_mem_r32((uint32_t)(a))
#define MEM_W32(a, v) test_mem_w32((uint32_t)(a), (uint32_t)(v))
#define MEM_W32_PC(a, v, pc) test_mem_w32((uint32_t)(a), (uint32_t)(v))
#define SR_YIELD(s, pc) ((void)0)
#define sr_begin(s, pc, op) ((void)0)
#define sr_end(s, addr, size) ((void)0)
#define SR_AOT_RETURN_CAPTURE(s, target) ((void)(s), (void)(target))
void dispatch(CpuState *s, uint32_t target);
void dispatch_call(CpuState *s, uint32_t target, uint32_t resume_pc);
int dispatch_call_checked(CpuState *s, uint32_t target, uint32_t resume_pc);
void sr_register(uint32_t addr, void (*fn)(CpuState *));
void sr_exec_span_reset(void);
int sr_exec_span_register(uint32_t start, uint32_t end);
void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc);
void sr_hle_call(CpuState *s, uint32_t nid);
void sr_syscall(CpuState *s, uint32_t nid);
void sr_unimplemented(uint32_t addr, const char *reason);
typedef void (*TestBodyFn)(CpuState *);
void sr_tail_record(CpuState *s, uint32_t target, TestBodyFn fn,
                    uint32_t restore_sp, uint32_t owes_restore);
void sr_tail_drain(CpuState *s);
#define SR_TAIL_DRAIN(s) sr_tail_drain(s)
#endif
""",
        encoding="ascii",
    )
    (work / "harness.c").write_text(
        f"""\
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "direct_j_funcs.h"

static int dispatch_hits = 0;
uint32_t test_mem_r32(uint32_t addr) {{ (void)addr; return 0u; }}
void test_mem_w32(uint32_t addr, uint32_t value) {{ (void)addr; (void)value; }}
void sr_register(uint32_t addr, void (*fn)(CpuState *)) {{ (void)addr; (void)fn; }}
void sr_exec_span_reset(void) {{}}
int sr_exec_span_register(uint32_t start, uint32_t end) {{ (void)start; (void)end; return 1; }}
void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc) {{ (void)s; (void)code; (void)pc; abort(); }}
void sr_hle_call(CpuState *s, uint32_t nid) {{ (void)s; (void)nid; abort(); }}
void sr_syscall(CpuState *s, uint32_t nid) {{ (void)s; (void)nid; abort(); }}
void sr_unimplemented(uint32_t addr, const char *reason) {{ (void)addr; (void)reason; abort(); }}
static void dispatch_target(CpuState *s, uint32_t actual) {{
    if (actual != 0x{target:08x}u) abort();
    dispatch_hits++;
    s->r[2] = 0x22u;
    s->r[3] += 0x44u;
    s->pc = s->r[31];
}}
void dispatch(CpuState *s, uint32_t actual) {{ dispatch_target(s, actual); }}
void dispatch_call(CpuState *s, uint32_t actual, uint32_t resume_pc) {{
    (void)resume_pc;
    dispatch_target(s, actual);
}}
/* A generated call site's checked form: these stand-ins never abandon a caller. */
int dispatch_call_checked(CpuState *s, uint32_t target, uint32_t resume_pc) {{
    dispatch_call(s, target, resume_pc);
    return 0;
}}
/* Stand-in for the #855 TAIL protocol (src/rt/recomp.h, guest_interp.c). */
static struct {{ TestBodyFn fn; CpuState *state; uint32_t target, restore_sp; int pending, owes; }} test_tail;
void sr_tail_record(CpuState *s, uint32_t target, TestBodyFn fn,
                    uint32_t restore_sp, uint32_t owes_restore) {{
    if (test_tail.pending) abort();
    test_tail.fn = fn; test_tail.state = s; test_tail.target = target;
    test_tail.restore_sp = restore_sp; test_tail.owes = owes_restore != 0u;
    test_tail.pending = 1;
}}
void sr_tail_drain(CpuState *s) {{
    uint32_t chain_sp = 0u;
    int restores = 0;
    while (test_tail.pending) {{
        TestBodyFn fn = test_tail.fn;
        uint32_t target = test_tail.target;
        if (test_tail.state != s) abort();
        if (test_tail.owes && !restores) {{ chain_sp = test_tail.restore_sp; restores = 1; }}
        test_tail.pending = 0;
        if (fn) fn(s); else dispatch(s, target);
    }}
    if (restores) s->r[29] = chain_sp;
}}

int main(void) {{
    CpuState state = {{0}};
    state.r[31] = 0x12345678u;
    f_{owner:08x}(&state);
    SR_TAIL_DRAIN(&state);
    printf("dispatch_hits=%d v0=0x%08x v1=0x%08x\\n",
           dispatch_hits, state.r[2], state.r[3]);
    return dispatch_hits == 1 && state.r[2] == 0x22u && state.r[3] == 0x55u ? 0 : 1;
}}
""",
        encoding="ascii",
    )


def _generate_compile_run(codegen_path: Path, work: Path, target: int, owner: int,
                          cc: str | None = None) -> _Outcome:
    compiler = cc or CC
    assert compiler is not None, "gcc is required for the compiled mutation proof"
    elf = work / "direct_j.elf"
    generated = work / "direct_j.c"
    elf.write_bytes(_synthetic_elf({
        owner: 0x08000410,       # direct j target
        owner + 4: 0x24030011,   # delay slot: v1 = 0x11
        owner + 8: 0x0C000410,   # dead jal discovery anchor
        owner + 12: 0x24000000,
        target: 0x24020022,
        target + 4: 0x03E00008,
        target + 8: 0x24630044,
    }, owner))
    env = dict(os.environ)
    env["PYTHONPATH"] = str(ROOT / "tools") + os.pathsep + env.get("PYTHONPATH", "")
    generated_result = subprocess.run(
        [sys.executable, str(codegen_path), str(elf), str(generated),
         "--profile=hst", "--funcs-per-chunk=1", f"--omit-aot=0x{target:08x}"],
        cwd=ROOT, env=env, capture_output=True, text=True,
    )
    gen_out = generated_result.stderr + generated_result.stdout
    if generated_result.returncode != 0:
        return _Outcome(generated_result.returncode, gen_out, None, "", None, "")

    _write_harness(work, target, owner)
    chunks = sorted(work.glob("direct_j_[0-9]*.c"))
    exe = work / "direct_j.exe"
    compile_result = subprocess.run(
        [
            compiler, "-std=c11", "-O0", "-Wall", "-Wextra", "-I", str(work),
            str(work / "harness.c"), str(generated),
            *(str(path) for path in chunks), "-lm", "-o", str(exe),
        ],
        cwd=ROOT, capture_output=True, text=True,
    )
    cc_out = compile_result.stderr + compile_result.stdout
    if compile_result.returncode != 0:
        return _Outcome(0, gen_out, compile_result.returncode, cc_out, None, "")
    run_result = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True)
    return _Outcome(0, gen_out, 0, cc_out, run_result.returncode, run_result.stderr + run_result.stdout)


@unittest.skipUnless(CC, "gcc is required for the compiled mutation proof")
class DirectJOmissionMutationTests(unittest.TestCase):
    def test_direct_j_boundary_mutant_is_compiled_and_killed(self):
        assert CC is not None
        owner = 0x00001000
        target = 0x00001040
        original = CODEGEN.read_text(encoding="utf-8")
        anchor = (
            "        if target_pc in dispatch_boundaries:\n"
            "            return True\n"
            "        owner = resume_owners.get(target_pc)"
        )
        self.assertIn(anchor, original, "direct-j dispatch-boundary mutation anchor drifted")
        mutant = original.replace(anchor, "        owner = resume_owners.get(target_pc)", 1)
        self.assertNotEqual(mutant, original)

        with scratch_dir.TemporaryDirectory(prefix="direct_j_omission_mut_") as tmp:
            root = Path(tmp)
            pristine_dir = root / "pristine"
            mutant_dir = root / "mutant"
            pristine_dir.mkdir()
            mutant_dir.mkdir()

            pristine = _generate_compile_run(CODEGEN, pristine_dir, target, owner)
            self.assertEqual(pristine.gen_rc, 0, pristine.gen_out)
            self.assertEqual(pristine.cc_rc, 0, pristine.cc_out)
            self.assertEqual(pristine.run_rc, 0, "pristine direct-j fixture failed\n" + pristine.run_out)
            self.assertIn("dispatch_hits=1", pristine.run_out)

            mutant_codegen = root / "mutant_codegen.py"
            mutant_codegen.write_text(mutant, encoding="utf-8", newline="\n")
            outcome = _generate_compile_run(mutant_codegen, mutant_dir, target, owner)
            self.assertEqual(
                outcome.gen_rc, 0,
                "MUTANT_CODEGEN_FAILED: direct-j mutant did not generate\n" + outcome.gen_out,
            )
            self.assertEqual(
                outcome.cc_rc, 0,
                "MUTANT_BUILD_FAILED: direct-j mutant did not compile cleanly\n" + outcome.cc_out,
            )
            self.assertNotEqual(
                outcome.run_rc, 0,
                "MUTANT_SURVIVED: omitted direct-j target was not semantically required\n"
                + outcome.run_out,
            )
            self.assertIn(
                "dispatch_hits=0", outcome.run_out,
                "MUTANT_NON_SEMANTIC_FAILURE: direct-j mutant did not inline the target\n"
                + outcome.run_out,
            )
            print("direct-j-omission: MUTANT_EXECUTED_AND_SEMANTIC_TEST_FAILED")

    def test_a_compile_failure_is_reported_as_a_build_failure_not_a_run(self):
        # A compiler that rejects every input stands in for a mutant whose generated C
        # does not build: the run stage must not happen or inherit the compile status.
        with scratch_dir.TemporaryDirectory(prefix="direct_j_cc_fail_") as tmp:
            outcome = _generate_compile_run(CODEGEN, Path(tmp), 0x00001040, 0x00001000,
                                            cc=sys.executable)
        self.assertEqual(outcome.gen_rc, 0, outcome.gen_out)
        self.assertIsNotNone(outcome.cc_rc)
        self.assertNotEqual(outcome.cc_rc, 0, outcome.cc_out)
        self.assertIsNone(outcome.run_rc)


if __name__ == "__main__":
    unittest.main()
