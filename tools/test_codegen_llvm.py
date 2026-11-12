# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The opt-in LLVM IR backend (``codegen.py --backend=llvm``, ``tools/codegen_llvm.py``, #330).

These tests pin the contract around the lowering; the executable veto is the
cosimulation corpus run with the backend (``mingw32-make cosim-selftest-llvm``) and
its mutation campaign (``mingw32-make cosim-mutants-llvm``):

* the default C output carries no trace of the backend: no ``.ll``, no manifest, no
  layout guard, and the same chunk text;
* with the backend, every lowered function is LLVM IR in its chunk's ``.ll``, its C
  chunk only declares and registers it, and every function kept in C is recorded
  with its reason; the C text of a kept function is unchanged;
* the order of effects the C emitter guarantees holds in the IR: a branch condition
  and a computed target are read before the delay slot, and ``jr $ra`` latches its
  target before the slot;
* an unsupported form (FPU, --lle-cpu, a runtime-placed module, an HST emission
  site) falls back with a reason, never an approximation;
* the runtime ABI the IR calls matches llvm_abi.h and recomp.h as Clang compiles
  them, and the CpuState offsets the IR uses compile against recomp.h.

Tests that need Clang, opt or a C compiler report SKIP when the tool is absent.
"""

from __future__ import annotations

import ast
import dataclasses
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import codegen  # noqa: E402
import codegen_llvm  # noqa: E402
from test_llvm_ir import llvm_tool, verify  # noqa: E402

BASE = 0x08900000
COSIM_GENERATOR = ROOT / "fixtures" / "cosim" / "generate.py"


class FakeElf:
    def __init__(self, words):
        self.words = words

    def read_at_vaddr(self, addr, size):
        if size != 4 or addr not in self.words:
            return None
        return self.words[addr].to_bytes(4, "little")


JR_RA = 0x03E00008
NOP = 0x00000000


def addiu(rt, rs, imm):
    return 0x09 << 26 | rs << 21 | rt << 16 | (imm & 0xFFFF)


def addu(rd, rs, rt):
    return rs << 21 | rt << 16 | rd << 11 | 0x21


def beq(rs, rt, off):
    return 0x04 << 26 | rs << 21 | rt << 16 | (off & 0xFFFF)


def jalr(rd, rs):
    return rs << 21 | rd << 11 | 0x09


def mul_s(fd, fs, ft):
    return 0x11 << 26 | 0x10 << 21 | ft << 16 | fs << 11 | fd << 6 | 0x02


def cache(op, rs, imm):
    return 0x2F << 26 | rs << 21 | op << 16 | (imm & 0xFFFF)


def program(*words, start=0x1000):
    return {start + 4 * i: w for i, w in enumerate(words)}


class LoweringTests(unittest.TestCase):
    def setUp(self):
        address_space = codegen.ADDRESS_SPACE
        self.addCleanup(setattr, codegen, "ADDRESS_SPACE", address_space)

    def lower(self, words, start=0x1000, end=0x1100, known=None, options=None):
        return codegen_llvm.lower(codegen, FakeElf(words), start, [(start, end)],
                                  known or {start}, c_text="void f(CpuState *s) { }",
                                  options=options)

    def ir(self, words, **kwargs):
        body = self.lower(words, **kwargs)
        module = codegen_llvm.chunk_module("t.ll", [body], {})
        return body, module.render()

    def assertVerifies(self, text):
        if llvm_tool("opt") is None:
            return
        result = verify(text)
        self.assertEqual(result.returncode, 0, result.stderr + text)

    def test_a_branchy_alu_function_lowers_and_verifies(self):
        words = program(addiu(4, 0, 1), beq(4, 5, 3), addu(6, 4, 5), addiu(6, 6, 1),
                        NOP, JR_RA, NOP)
        body, text = self.ir(words)
        self.assertIsInstance(body, codegen_llvm.LLVMBody)
        self.assertEqual(str(body), "void f_00001000(CpuState *s);  /* body in LLVM IR (#330) */")
        self.assertIn("define void @f_00001000(ptr %s) nounwind uwtable {", text)
        self.assertVerifies(text)

    def test_branch_condition_is_read_before_the_delay_slot(self):
        # beq r4, r5 with `addu r4, ...` in its slot: the slot overwrites r4, so the
        # condition must be the value before the slot (codegen.emit_function's `_c`).
        words = program(beq(4, 5, 3), addu(4, 4, 4), NOP, NOP, NOP, JR_RA, NOP)
        _, text = self.ir(words)
        lines = text.splitlines()
        cond = next(i for i, l in enumerate(lines) if "icmp eq i32" in l)
        branch_begin = next(i for i, l in enumerate(lines) if "@sr_llvm_begin(ptr %s, i32 4096," in l)
        slot_store = next(i for i, l in enumerate(lines) if l.startswith("  store") and "r4" in l)
        branch = next(i for i, l in enumerate(lines) if l.strip().startswith("br i1"))
        self.assertLess(cond, branch_begin)
        self.assertLess(branch_begin, slot_store)
        self.assertLess(slot_store, branch)
        self.assertVerifies(text)

    def test_computed_call_target_is_latched_before_the_slot(self):
        # jalr ra, r4 whose slot writes r4 (the PR #126 `jrslot` shape).
        words = program(jalr(31, 4), addiu(4, 0, 0x40), JR_RA, NOP)
        _, text = self.ir(words)
        lines = text.splitlines()
        latch = next(i for i, l in enumerate(lines) if "load i32, ptr %r4" in l)
        slot = next(i for i, l in enumerate(lines) if "@sr_llvm_begin(ptr %s, i32 4100," in l)
        dispatch = next(i for i, l in enumerate(lines) if "@dispatch_call_checked(" in l)
        self.assertLess(latch, slot)
        self.assertLess(slot, dispatch)
        latched = re.search(r"(%r4\.\d+) = load", lines[latch]).group(1)
        self.assertIn(f"i32 {latched}, i32 4104", lines[dispatch])
        self.assertVerifies(text)

    def test_computed_transfer_refuses_a_null_target_before_the_link_and_slot(self):
        # #1375: a latched computed target of 0 is a null code pointer, as in the C backend
        # (codegen.emit_code_pointer_check): the stop comes before the link store and slot.
        cases = (("jalr", program(jalr(31, 4), addiu(5, 0, 1), JR_RA, NOP)),
                 ("jr", program(4 << 21 | 0x08, addiu(5, 0, 1))))
        for name, words in cases:
            with self.subTest(name):
                _, text = self.ir(words)
                lines = text.splitlines()
                latch = next(i for i, l in enumerate(lines) if "load i32, ptr %r4" in l)
                latched = re.search(r"(%r4\.\d+) = load", lines[latch]).group(1)
                compare = next(i for i, l in enumerate(lines)
                               if "icmp eq i32" in l and f"{latched}, 0" in l)
                stop = next(i for i, l in enumerate(lines)
                            if "call void @sr_null_code_pointer(ptr %s, i32 4096)" in l)
                begin = next(i for i, l in enumerate(lines)
                             if "@sr_llvm_begin(ptr %s, i32 4096," in l)
                self.assertLess(latch, compare)
                self.assertLess(compare, stop)
                self.assertLess(stop, begin)
                self.assertEqual(lines[stop + 1].strip(), "unreachable")
                self.assertVerifies(text)

    def test_return_target_is_captured_before_the_slot_and_sp_restored(self):
        words = program(JR_RA, addiu(31, 0, 0))
        _, text = self.ir(words)
        lines = text.splitlines()
        capture = next(i for i, l in enumerate(lines) if "@sr_llvm_return_capture" in l)
        slot = next(i for i, l in enumerate(lines) if "@sr_llvm_begin(ptr %s, i32 4100," in l)
        self.assertLess(capture, slot)
        # A callable body restores its entry SP (loaded in the prologue) on return.
        entry_sp = re.search(r"(%r29\.\d+) = load i32", text).group(1)
        self.assertIn(f"store i32 {entry_sp}, ptr", text)
        self.assertVerifies(text)

    def test_several_continuation_exits_lower_to_terminated_blocks(self):
        # Two paths fall through into other entries (0x100c and 0x1014): each continuation
        # exit is a TAIL transfer. Found at title scale (HST, PAC-MAN CE): every exit but
        # the last left an empty, unterminated block behind.
        words = {
            0x1000: beq(4, 5, 3), 0x1004: NOP,
            0x1008: addiu(2, 0, 1),
            0x100C: JR_RA, 0x1010: addiu(2, 0, 2),
            0x1014: JR_RA, 0x1018: NOP,
        }
        known = {0x1000, 0x100C, 0x1014}
        insns, _labels, continuations = codegen.function_flow(
            FakeElf(words), 0x1000, [(0x1000, 0x1100)], known)
        self.assertEqual(continuations, {0x1008: 0x100C, 0x1010: 0x1014})
        _, text = self.ir(words, known=known)
        self.assertEqual(text.count("call void @sr_llvm_tail_record("), 2)
        self.assertIn("ptr @f_0000100c", text)
        self.assertIn("ptr @f_00001014", text)
        self.assertVerifies(text)

    def _c_text(self, words, options):
        elf, start = FakeElf(words), min(words)
        return "\n".join(codegen.emit_function(elf, start, [(start, start + 0x100)], {start},
                                               options=options))

    def test_stale_detect_cache_hook_is_lowered_inside_its_trace_record(self):
        # codegen._effect_cache: under --stale-detect, `cache op, imm(rs)` reports rs + imm to
        # the detector (sr_stale_note_cache_op); without it the word does nothing. The cache
        # op 0x14 sits in the rt field, so a lowering that read rt would load r20; the second
        # word's displacement is negative, so one that zero-extended it would add 65528.
        words = program(addiu(4, 0, 0x100), cache(0x14, 4, 0x40), cache(0x14, 4, -8), JR_RA, NOP)
        options = codegen.CodegenOptions(stale_detect=True)
        c_text = self._c_text(words, options)
        self.assertIn("sr_stale_note_cache_op((uint32_t)(s->r[4] + 0x00000040u));", c_text)
        self.assertIn("sr_stale_note_cache_op((uint32_t)(s->r[4] + 0xfffffff8u));", c_text)
        _, text = self.ir(words, options=options)
        lines = text.splitlines()
        for pc, displacement in ((4100, ", 64"), (4104, ", -8")):
            begin = next(i for i, l in enumerate(lines) if f"@sr_llvm_begin(ptr %s, i32 {pc}," in l)
            end = next(i for i, l in enumerate(lines) if i > begin and "@sr_llvm_end(" in l)
            add = next(i for i, l in enumerate(lines) if begin < i < end and " add i32 " in l)
            note = next(i for i, l in enumerate(lines)
                        if begin < i < end and "call void @sr_stale_note_cache_op(i32" in l)
            self.assertLess(add, note)
            self.assertIn(" add i32 %r4.", lines[add])
            self.assertTrue(lines[add].endswith(displacement), lines[add])
            self.assertIn(lines[add].split("=")[0].strip(), lines[note])
        self.assertIn("declare void @sr_stale_note_cache_op(i32)", text)
        self.assertVerifies(text)
        _, plain = self.ir(words)
        self.assertNotIn("sr_stale_note_cache_op", plain)

    def test_static_verify_checks_are_the_c_emitters_after_the_same_word(self):
        words = program(addiu(4, 0, 1), addiu(5, 4, 2), addu(6, 4, 5), JR_RA, NOP)
        options = codegen.CodegenOptions(static_verify=True)
        expected = [(int(pc, 16), int(reg), int(value, 16)) for pc, reg, value in re.findall(
            r"sr_sv_check\(s, (0x[0-9a-f]{8})u, (\d+), (0x[0-9a-f]{8})u\);",
            self._c_text(words, options))]
        self.assertEqual(expected, [(0x1008, 4, 1), (0x1008, 5, 3), (0x1008, 6, 4)])
        before = dict(codegen.SV_STATS)
        _, text = self.ir(words, options=options)
        self.assertEqual(codegen.SV_STATS, before)        # the summary counts C's points once
        lines = text.splitlines()
        rendered = [(int(pc), int(reg), int(value)) for pc, reg, value in re.findall(
            r"call void @sr_llvm_sv_check\(ptr %s, i32 (\d+), i32 (\d+), i32 (\d+)\)", text)]
        self.assertEqual(rendered, expected)
        word_end = max(i for i, l in enumerate(lines) if "@sr_llvm_end(" in l
                       and i < next(j for j, m in enumerate(lines) if "call void @sr_llvm_sv_check(" in m))
        word_begin = max(i for i, l in enumerate(lines) if i < word_end
                         and "@sr_llvm_begin(ptr %s, i32 4104," in l)
        self.assertLess(word_begin, word_end)
        return_begin = next(i for i, l in enumerate(lines)
                            if "@sr_llvm_begin(ptr %s, i32 4108," in l)
        self.assertTrue(all(word_end < i < return_begin for i, l in enumerate(lines)
                            if "call void @sr_llvm_sv_check(" in l))
        self.assertVerifies(text)
        _, plain = self.ir(words)
        self.assertNotIn("sr_llvm_sv_check", plain)

    def test_a_static_verify_check_off_a_straight_line_word_is_refused(self):
        real_plan = codegen_llvm.P.plan_function

        def misplaced(*args, **kwargs):
            plan = real_plan(*args, **kwargs)
            plan.sv_checks = {0x1004: ((4, 1),)}           # the jr's delay slot
            return plan

        words = program(addiu(4, 0, 1), JR_RA, addiu(5, 0, 2))
        self.assertIsInstance(self.lower(words), codegen_llvm.LLVMBody)
        with mock.patch.object(codegen_llvm.P, "plan_function", misplaced):
            with self.assertRaisesRegex(codegen_llvm.LLVMUnsupported, "static-verify check at "
                                        "0x00001004 is not on a straight-line word"):
                self.lower(words)

    def test_unsupported_forms_fall_back_with_a_reason(self):
        with self.assertRaisesRegex(codegen_llvm.LLVMUnsupported, "FPU at 0x00001000"):
            self.lower(program(mul_s(0, 1, 2), JR_RA, NOP))
        with self.assertRaisesRegex(codegen_llvm.LLVMUnsupported, "--lle-cpu"):
            self.lower(program(JR_RA, NOP), options=codegen.CodegenOptions(lle_cpu=True))
        codegen.ADDRESS_SPACE = codegen.AddressSpace("sr_m0_base", "m0")
        with self.assertRaisesRegex(codegen_llvm.LLVMUnsupported, "runtime-placed"):
            self.lower(program(JR_RA, NOP))

    def test_a_transfer_without_its_delay_slot_is_refused_not_crashed(self):
        # A transfer the planner left without a slot used to fail with AttributeError inside the
        # lowering; it must raise LLVMUnsupported, which codegen.py's llvm_body() turns into
        # "keep the C body" (an llvm_fallbacks entry) for that function.
        real_plan = codegen_llvm.P.plan_function

        def slotless(*args, **kwargs):
            plan = real_plan(*args, **kwargs)
            plan.steps = [
                dataclasses.replace(step, slot=None) if isinstance(step, codegen_llvm.P.Transfer) else step
                for step in plan.steps
            ]
            return plan

        words = program(beq(4, 5, 3), addu(4, 4, 4), NOP, NOP, NOP, JR_RA, NOP)
        # Control: the unmodified plan lowers.
        self.assertIsInstance(self.lower(words), codegen_llvm.LLVMBody)
        with mock.patch.object(codegen_llvm.P, "plan_function", slotless):
            with self.assertRaisesRegex(codegen_llvm.LLVMUnsupported, "no delay-slot operation"):
                self.lower(words)
        self.assertTrue(issubclass(codegen_llvm.LLVMUnsupported, Exception))

    def test_hst_emission_sites_cover_every_hst_address_in_emit_function(self):
        # Every `hst_profile and ... addr == <literal>` site in emit_function or
        # one of the `_emit_*` steps it was split into (#1531) emits something
        # other than the plain word; the LLVM backend must refuse each one. The last
        # such sites (two loop fast paths and the guest abort exit) were retired under
        # #363, so none may come back unnoticed.
        tree = ast.parse((ROOT / "tools" / "codegen.py").read_text(encoding="utf-8"))
        emitters = [n for n in ast.walk(tree)
                    if isinstance(n, ast.FunctionDef)
                    and (n.name == "emit_function" or n.name.startswith("_emit_"))]
        self.assertIn("emit_function", {n.name for n in emitters})
        literals = set()
        for node in (n for emit in emitters for n in ast.walk(emit)):
            if not isinstance(node, ast.If):
                continue
            names = {n.id for n in ast.walk(node.test) if isinstance(n, ast.Name)}
            if "hst_profile" not in names:
                continue
            for cmp in (n for n in ast.walk(node.test) if isinstance(n, ast.Compare)):
                if (isinstance(cmp.left, ast.Name) and cmp.left.id == "addr"
                        and isinstance(cmp.ops[0], ast.Eq)
                        and isinstance(cmp.comparators[0], ast.Constant)):
                    literals.add(cmp.comparators[0].value)
        self.assertEqual(literals, set(), "an HST-profile address site is back in an emitter")
        self.assertLessEqual(literals, codegen_llvm.hst_emission_sites(codegen))


class CodegenIntegrationTests(unittest.TestCase):
    """codegen.py run end to end over the cosimulation's source-owned guest."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        subprocess.run([sys.executable, str(COSIM_GENERATOR), "generate", "--out-dir",
                        str(root / "fixture")], check=True, capture_output=True)
        cls.runs = {}
        for backend in ("c", "llvm"):
            out = root / backend
            out.mkdir()
            result = subprocess.run(
                [sys.executable, str(ROOT / "tools" / "codegen.py"),
                 str(root / "fixture" / "guest.prx"), str(out / "g_recomp.c"),
                 f"--base=0x{BASE:08x}", "--target-chunk-bytes=2000", "--stack-census",
                 f"--backend={backend}"],
                capture_output=True, text=True, check=False)
            if result.returncode != 0:
                raise AssertionError(result.stdout + result.stderr)
            cls.runs[backend] = (out, result.stdout)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def files(self, backend, pattern):
        out, _ = self.runs[backend]
        return {p.name: p.read_text(encoding="ascii") for p in sorted(out.glob(pattern))}

    def manifest(self):
        out, _ = self.runs["llvm"]
        return json.loads((out / "g_recomp_llvm.json").read_text(encoding="ascii"))

    def test_default_backend_writes_no_llvm_output(self):
        out, stdout = self.runs["c"]
        self.assertEqual(list(out.glob("*.ll")), [])
        self.assertFalse((out / "g_recomp_llvm.json").exists())
        self.assertNotIn("LLVM backend", (out / "g_recomp.c").read_text(encoding="ascii"))
        self.assertNotIn("llvm backend", stdout)

    def test_every_function_is_lowered_or_recorded(self):
        document = self.manifest()
        self.assertEqual(document["backend"], codegen_llvm.BACKEND_ID)
        lowered = {int(a, 16) for a in document["lowered"]}
        kept = {int(row["address"], 16): row["reason"] for row in document["fallbacks"]}
        self.assertGreaterEqual(len(lowered), 20)
        self.assertTrue(all(reason for reason in kept.values()))
        _, stdout = self.runs["llvm"]
        self.assertIn(f"{len(lowered)} functions lowered to LLVM IR, {len(kept)} kept in C", stdout)
        ir = "\n".join(self.files("llvm", "*.ll").values())
        for addr in lowered:
            self.assertIn(f"define void @_sr_stack_census_body_{addr:08x}(ptr %s)", ir)
        for addr in kept:
            self.assertNotIn(f"_body_{addr:08x}(ptr %s)", ir)

    def test_c_chunks_declare_register_and_wrap_lowered_bodies(self):
        lowered = {int(a, 16) for a in self.manifest()["lowered"]}
        chunks = self.files("llvm", "g_recomp_*.c")
        for addr in lowered:
            text = next(t for t in chunks.values() if f"sr_register(0x{addr:08x}u" in t)
            self.assertIn(f"void _sr_stack_census_body_{addr:08x}(CpuState *s);  /* body in LLVM IR", text)
            self.assertIn(f"void f_{addr:08x}(CpuState *s) {{", text)          # the census wrapper
            self.assertNotIn(f"static void _sr_stack_census_body_{addr:08x}", text)

    def test_functions_kept_in_c_are_emitted_unchanged(self):
        kept = {int(row["address"], 16) for row in self.manifest()["fallbacks"]}

        def body(backend, addr):
            # The census-wrapped body through the end of its wrapper; a body that loads $sp
            # is not wrapped (codegen.GUEST_SP_ENTRIES), so it is the function alone.
            text = "\n".join(self.files(backend, "g_recomp_*.c").values())
            match = (re.search(rf"static void _sr_stack_census_body_{addr:08x}\(.*?"
                               rf"\nvoid f_{addr:08x}\(CpuState \*s\) \{{.*?\n\}}", text, re.S)
                     or re.search(rf"(?m)^void f_{addr:08x}\(CpuState \*s\) \{{.*?\n\}}", text, re.S))
            self.assertIsNotNone(match, f"0x{addr:08x} under {backend}")
            return match.group(0)
        self.assertTrue(kept)
        for addr in kept:
            self.assertEqual(body("c", addr), body("llvm", addr))

    def test_size_bounded_chunks_cut_at_the_same_functions(self):
        # An LLVM body weighs what its C body weighs, so --target-chunk-bytes puts the
        # same functions in each chunk under both backends.
        def chunks(backend):
            text = self.files(backend, "g_recomp_[0-9]*.c")
            return sorted(tuple(re.findall(r"sr_register\((0x[0-9a-f]+)u", t)) for t in text.values())
        self.assertEqual(chunks("c"), chunks("llvm"))

    def test_layout_guard_only_in_llvm_builds(self):
        out, _ = self.runs["llvm"]
        main_c = (out / "g_recomp.c").read_text(encoding="ascii")
        for line in codegen_llvm.layout_asserts():
            self.assertIn(line, main_c)

    @unittest.skipIf(llvm_tool("opt") is None, "SKIP: LLVM opt is not installed")
    def test_every_chunk_module_passes_the_verifier(self):
        for name, text in self.files("llvm", "*.ll").items():
            result = verify(text)
            self.assertEqual(result.returncode, 0, f"{name}: {result.stderr}")


def _compile_ir(source: str, defines=()) -> str:
    clang = llvm_tool("clang")
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "probe.c"
        path.write_text(source, encoding="ascii", newline="\n")
        result = subprocess.run(
            [clang, "-O0", "-w", "-S", "-emit-llvm", "-Isrc/rt", "-DSR_SDL3VK", *defines,
             str(path), "-o", "-"], cwd=ROOT, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise AssertionError(result.stderr)
    return result.stdout


def _signatures(ir: str) -> dict[str, str]:
    """name -> 'ret (params)' for every define/declare, without parameter attributes."""
    found = {}
    for match in re.finditer(r"^(?:define|declare)\b[^@\n]*?(\S+) @([\w.]+)\(([^)]*)\)", ir, re.M):
        ret, name, params = match.groups()
        types = [p.split()[0] for p in params.split(",") if p.strip()]
        found[name] = f"{ret} ({', '.join(types)})"
    return found


class PackageCacheTests(unittest.TestCase):
    """#316: an LLVM package never shares a cache key with a C package or a toolchain."""

    def setUp(self):
        from nk_core import package_cache
        self.pc = package_cache

    def test_c_backend_adds_nothing_to_the_key(self):
        for env in ({}, {"CODEGEN_BACKEND": "c"}, {"MAKEFLAGS": "-j4"}):
            self.assertEqual(self.pc.llvm_backend_options(env), {})

    def test_every_spelling_of_the_option_is_seen(self):
        for env in ({"CODEGEN_BACKEND": "llvm"}, {"MAKEFLAGS": "-j4 CODEGEN_BACKEND=llvm"},
                    {"CODEGEN_USER_ARGS": "--stack-census --backend=llvm"}):
            self.assertTrue(self.pc.llvm_backend_enabled(env), env)

    def test_a_missing_toolchain_fails_closed(self):
        env = {"CODEGEN_BACKEND": "llvm", "PATH": "", "LLVM_CLANG": "nk-no-such-clang"}
        with self.assertRaises(self.pc.PackageCacheError):
            self.pc.llvm_backend_options(env)

    @unittest.skipIf(llvm_tool("clang") is None or llvm_tool("llvm-link") is None,
                     "SKIP: Clang or llvm-link is not installed")
    def test_llvm_key_names_the_lowering_sources_and_toolchain(self):
        env = dict(os.environ, CODEGEN_BACKEND="llvm",
                   PATH=os.pathsep.join([str(Path(llvm_tool("clang")).parent),
                                         os.environ.get("PATH", "")]))
        options = self.pc.llvm_backend_options(env)["llvm_backend"]
        self.assertEqual(set(options["sources"]), set(self.pc.LLVM_BACKEND_SOURCES))
        self.assertIn("clang version", options["clang"])
        self.assertIn("LLVM version", options["llvm_link"])

    def test_the_key_names_every_tools_module_the_lowering_imports(self):
        # A change to any module the renderer imports changes the IR it writes, so each one
        # must move an LLVM package's key; tools/codegen.py is keyed for both backends.
        closure, pending = set(), ["codegen_llvm"]
        while pending:
            name = pending.pop()
            if name in closure or name == "codegen" or name in sys.stdlib_module_names:
                continue
            path = ROOT / "tools" / f"{name}.py"
            # Anything else must be a tools/ module this walk can read, or it fails here
            # instead of leaving the closure silently.
            self.assertTrue(path.is_file(), f"the lowering imports {name}, not found in tools/")
            closure.add(name)
            for node in ast.walk(ast.parse(path.read_text(encoding="utf-8"))):
                if isinstance(node, ast.Import):
                    pending.extend(alias.name.split(".")[0] for alias in node.names)
                elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
                    pending.append(node.module.split(".")[0])
        self.assertIn("lowering_plan", closure)
        keyed = {path for path in self.pc.LLVM_BACKEND_SOURCES if path.startswith("tools/")}
        self.assertEqual(keyed, {f"tools/{name}.py" for name in closure})
        # The Make rule that writes the .ll files depends on the same modules in an LLVM
        # build, and on none of them in a C build.
        text = (ROOT / "mk" / "pipeline.mk").read_text(encoding="utf-8")
        match = re.search(r"^CODEGEN_LLVM_PREREQ := \$\(if \$\(filter --backend=llvm,"
                          r"\$\(CODEGEN_USER_ARGS\)\),"
                          r"([^)]*)\)$", text, re.M)
        self.assertIsNotNone(match)
        self.assertEqual(set(match.group(1).split()), keyed)
        rule = re.search(r"^\$\(BUILD_DIR\)/\$\(GAME_NAME\)_recomp\.c [^:\n]*: \$\(GAME_INPUT_PREREQ\)"
                         r"[^\n]*", text, re.M)
        self.assertIsNotNone(rule)
        self.assertIn("$(CODEGEN_LLVM_PREREQ)", rule.group(0))

    def test_planner_and_cli_both_key_on_the_backend(self):
        for path in ("tools/title_codegen_plan.py", "tools/nk_cli.py"):
            text = (ROOT / path).read_text(encoding="utf-8")
            self.assertIn("package_cache.llvm_backend_options(", text, path)


class RuntimeAbiTests(unittest.TestCase):
    @unittest.skipIf(llvm_tool("clang") is None, "SKIP: Clang is not installed")
    def test_backend_declarations_match_the_c_runtime(self):
        names = sorted(codegen_llvm.ABI_FUNCTIONS)
        refs = ", ".join(f"(void *){n}" for n in names)
        ir = _compile_ir('#include "llvm_abi.h"\n'
                         f"void *nk_llvm_abi_probe[] = {{ {refs} }};\n")
        ir += _compile_ir(ROOT.joinpath("src/rt/llvm_abi.c").read_text(encoding="ascii"))
        signatures = _signatures(ir)
        for name in names:
            with self.subTest(name=name):
                self.assertIn(name, signatures)
                self.assertEqual(signatures[name], str(codegen_llvm.ABI_FUNCTIONS[name]))

    @unittest.skipIf(shutil.which(os.environ.get("CC", "gcc")) is None, "SKIP: no C compiler")
    def test_layout_asserts_hold_for_recomp_h(self):
        source = '#include "recomp.h"\n' + "\n".join(codegen_llvm.layout_asserts()) + "\n"
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "layout.c"
            path.write_text(source, encoding="ascii", newline="\n")
            result = subprocess.run(
                [shutil.which(os.environ.get("CC", "gcc")), "-fsyntax-only", "-Isrc/rt",
                 "-Isrc/core", "-DSR_SDL3VK", str(path)],
                cwd=ROOT, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_the_static_verify_shim_is_the_c_chunks_check(self):
        source = (ROOT / "src" / "rt" / "llvm_abi.c").read_text(encoding="ascii")
        shim = re.search(r"^void sr_llvm_sv_check\((.*?)\) (\{.*?^\})", source, re.M | re.S)
        chunk = re.search(r"^static void sr_sv_check\((.*?)\) (\{.*?^\})", codegen.SR_SV_CHECK,
                          re.M | re.S)
        self.assertIsNotNone(shim)
        self.assertIsNotNone(chunk)
        self.assertEqual(shim.groups(), chunk.groups())

    def test_abi_header_declares_every_shim_the_backend_calls(self):
        header = (ROOT / "src" / "rt" / "llvm_abi.h").read_text(encoding="ascii")
        shims = {n for n in codegen_llvm.ABI_FUNCTIONS if n.startswith("sr_llvm_")}
        declared = set(re.findall(r"\b(sr_llvm_\w+)\(", header))
        self.assertEqual(shims, declared)


class MutantAnchorTests(unittest.TestCase):
    """Every cosimulation mutant must still apply to the source it damages (#2227).

    Hosted CI runs only the default campaign, so a stale LLVM mutant would otherwise
    surface only when someone ran ``cosim-mutants-llvm``. ``Mutant.apply`` raises when
    a pattern matches a different number of times than declared.
    """

    @classmethod
    def setUpClass(cls):
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "cosim_mutate", ROOT / "fixtures" / "cosim" / "mutate.py")
        cls.mutate = importlib.util.module_from_spec(spec)
        sys.modules["cosim_mutate"] = cls.mutate
        spec.loader.exec_module(cls.mutate)

    def test_every_mutant_pattern_matches_its_source(self):
        m = self.mutate
        paths = {
            "interpreter": m.INTERPRETER, "fp-convert": m.FP_CONVERT,
            "fpu-reference": m.FPU_REFERENCE, "generator": m.GENERATOR,
            "split-generator": m.GENERATOR, "scheduler": m.SCHEDULER,
            "llvm-backend": m.LLVM_BACKEND,
        }
        for mutant in (*m.MUTANTS, *m.LLVM_MUTANTS):
            with self.subTest(mutant=mutant.name):
                source = paths[mutant.target].read_text(encoding="utf-8")
                self.assertNotEqual(mutant.apply(source), source)


if __name__ == "__main__":
    unittest.main()
