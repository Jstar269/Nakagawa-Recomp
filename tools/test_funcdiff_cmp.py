# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
#
# Synthetic fail-closed tests for tools/funcdiff_cmp.py (issue #354).
# No retail assets, traces, or golden hashes: every fixture is synthesized here.
# The suite was first run against the pre-fix tool to record the failing-before
# behavior (empty recomp -> MATCH 0 steps, truncated oracle -> MATCH on the
# common prefix, entry beyond oracle -> MATCH 0 steps); now it pins the
# fail-closed contract.
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(TOOLS, "funcdiff_cmp.py")


def run_tool(argv):
    proc = subprocess.run(
        [sys.executable, "-I", TOOL] + argv,
        capture_output=True,
        text=True,
        cwd=TOOLS,
        encoding="utf-8",
        errors="replace",
        timeout=60,
    )
    return proc.returncode, proc.stdout + proc.stderr


class TestFuncdiffCmpFailClosed(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.d = self._tmp.name
        self.n = 0

    def path(self, lines, name=None):
        self.n += 1
        p = os.path.join(self.d, name or f"f{self.n}.trace")
        with open(p, "w", newline="\n") as f:
            if lines:
                f.write("\n".join(lines) + "\n")
        return p

    def steps(self, n, pc0=0x100, tag="a"):
        return [f"{i} pc=0x{pc0+i:08x} op=0x00000021 r1=0x{tag}0{i:02x}" for i in range(n)]

    def test_exact_nonempty_match_exits_zero(self):
        oracle = self.path(self.steps(4))
        mine = self.path(self.steps(4))
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 0, out)
        self.assertIn("MATCH: 4 steps identical", out)
        self.assertIn("oracle step 0", out)

    def test_first_step_mismatch_fails(self):
        oracle = self.path(self.steps(3))
        mine = self.path(self.steps(3, tag="b"))
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 1, out)
        self.assertIn("DIVERGENCE at my step 0", out)
        self.assertIn("oracle:", out)
        self.assertIn("recomp:", out)

    def test_divergence_after_matching_prefix_fails(self):
        oracle = self.path(self.steps(3))
        mine = self.path(self.steps(3)[:-1] + [self.steps(3)[-1].replace("pc=0x00000102", "pc=0x00000202")])
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 1, out)
        self.assertIn("DIVERGENCE at my step 2 (oracle step 2)", out)

    def test_empty_recomp_trace_fails(self):
        oracle = self.path(self.steps(3))
        mine = self.path([])
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 1, out)
        self.assertIn("empty recomp trace", out)
        self.assertNotIn("MATCH", out)

    def test_blank_only_recomp_trace_fails(self):
        oracle = self.path(self.steps(3))
        mine = self.path(["", ""])
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 1, out)

    def test_empty_oracle_fails(self):
        oracle = self.path([])
        mine = self.path(self.steps(2))
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 1, out)
        self.assertIn("no steps at or after entry-step 0", out)

    def test_entry_beyond_oracle_fails(self):
        oracle = self.path(self.steps(3))
        mine = self.path(self.steps(2))
        rc, out = run_tool([oracle, mine, "50"])
        self.assertEqual(rc, 1, out)
        self.assertIn("entry-step 50", out)

    def test_oracle_truncated_before_recomp_trace_ends_fails(self):
        oracle = self.path(self.steps(1))
        mine = self.path(self.steps(3))
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 1, out)
        self.assertIn("oracle truncated before recomp trace ends", out)
        self.assertIn("need 3 steps", out)
        self.assertIn("found 1", out)

    def test_matching_recomp_strict_prefix_with_enough_oracle_steps_passes(self):
        # The oracle legitimately continues beyond the requested recomp-length slice.
        oracle = self.path(self.steps(5))
        mine = self.path(self.steps(3))
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 0, out)
        self.assertIn("MATCH: 3 steps identical", out)

    def test_oracle_prefix_at_nonzero_entry_passes(self):
        oracle_lines = self.steps(6)
        oracle = self.path(oracle_lines)
        mine = self.path(oracle_lines[4:6])  # identical to oracle steps 4..5
        rc, out = run_tool([oracle, mine, "4"])
        self.assertEqual(rc, 0, out)
        self.assertIn("MATCH: 2 steps identical", out)
        self.assertIn("oracle step 4", out)

    # --- contiguous-step invariant: the oracle must supply the exact
    # requested slice entry..entry+need-1 (issue #354 follow-up) ---

    def test_exact_contiguous_slice_passes(self):
        oracle = self.path(self.steps(3))
        mine = self.path(self.steps(3))
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 0, out)
        self.assertIn("MATCH: 3 steps identical", out)

    def test_nonzero_entry_contiguous_slice_passes(self):
        oracle = self.path(self.steps(6))
        mine = self.path(self.steps(6)[4:6])
        rc, out = run_tool([oracle, mine, "4"])
        self.assertEqual(rc, 0, out)
        self.assertIn("MATCH: 2 steps identical", out)

    def test_shifted_slice_start_fails(self):
        # entry-step 4 requested, but the oracle's first record at or after the
        # entry is step 5 (step 4 absent): the slice must not silently shift.
        oracle_lines = self.steps(7)
        del oracle_lines[4]  # steps 0,1,2,3,5,6
        oracle = self.path(oracle_lines)
        mine = self.path(self.steps(2))
        rc, out = run_tool([oracle, mine, "4"])
        self.assertEqual(rc, 1, out)
        self.assertIn("oracle slice does not start at entry-step 4", out)
        self.assertIn("first record at or after entry has step 5", out)
        self.assertNotIn("MATCH", out)

    def test_middle_requested_step_deleted_fails(self):
        # Requested slice 4,5,6 but step 5 is absent: 4,6,... must fail with
        # the expected/observed step numbers, not compare step 6 as step 5.
        oracle_lines = self.steps(7)
        del oracle_lines[5]  # steps 0,1,2,3,4,6
        oracle = self.path(oracle_lines)
        mine = self.path(self.steps(3))
        rc, out = run_tool([oracle, mine, "4"])
        self.assertEqual(rc, 1, out)
        self.assertIn("oracle steps are not contiguous at 4+1 (expected step 5, observed 6)", out)
        self.assertNotIn("MATCH", out)

    def test_duplicate_requested_step_fails(self):
        # Requested slice 4,5,6 but step 4 is duplicated: the second 4 must be
        # rejected against the expected step 5.
        oracle_lines = self.steps(7)
        oracle_lines[5] = oracle_lines[4]  # steps 0,1,2,3,4,4,6
        oracle = self.path(oracle_lines)
        mine = self.path(self.steps(3))
        rc, out = run_tool([oracle, mine, "4"])
        self.assertEqual(rc, 1, out)
        self.assertIn("oracle steps are not contiguous at 4+1 (expected step 5, observed 4)", out)
        self.assertNotIn("MATCH", out)

    def test_backwards_out_of_order_requested_step_fails(self):
        # Requested slice 4,5,6 but the oracle emits ...4,6,5: out-of-order
        # steps must fail rather than being accepted as coverage.
        oracle_lines = self.steps(7)
        oracle_lines[5], oracle_lines[6] = oracle_lines[6], oracle_lines[5]
        oracle = self.path(oracle_lines)
        mine = self.path(self.steps(3))
        rc, out = run_tool([oracle, mine, "4"])
        self.assertEqual(rc, 1, out)
        self.assertIn("oracle steps are not contiguous at 4+1 (expected step 5, observed 6)", out)
        self.assertNotIn("MATCH", out)

    def test_extra_oracle_records_after_requested_slice_still_pass(self):
        # The oracle may continue beyond the requested slice; only the slice
        # itself must be exactly contiguous.
        oracle = self.path(self.steps(6))
        mine = self.path(self.steps(2))
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 0, out)
        self.assertIn("MATCH: 2 steps identical", out)

    def test_malformed_recomp_record_fails_explicitly(self):
        oracle = self.path(self.steps(3))
        mine = self.path(self.steps(3)[:1] + ["7 pc=0x1 onlytwofields"] + self.steps(3)[2:])
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 1, out)
        self.assertIn("malformed trace record", out)
        self.assertNotIn("MATCH", out)

    def test_malformed_oracle_record_cannot_shrink_coverage(self):
        # One malformed oracle record inside the requested slice must fail the run,
        # not silently shrink the compared coverage into a MATCH.
        oracle = self.path(self.steps(3)[:1] + ["garbage line"] + self.steps(3)[2:])
        mine = self.path(self.steps(3))
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 1, out)
        self.assertIn("malformed oracle record", out)
        self.assertNotIn("MATCH", out)

    def test_malformed_oracle_step_number_fails(self):
        oracle = self.path(["x1 pc=0x00000100 op=0x00000021"] + self.steps(2)[1:])
        mine = self.path(self.steps(2))
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 1, out)
        self.assertIn("malformed oracle step number", out)

    def test_write_token_without_equals_fails(self):
        oracle = self.path(self.steps(2)[:1] + ["1 pc=0x00000101 op=0x00000021 r1 junk"] + self.steps(2)[1:])
        mine = self.path(self.steps(2))
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 1, out)
        self.assertIn("malformed write token", out)

    def test_blank_oracle_line_fails(self):
        oracle = self.path([""] + self.steps(2))
        mine = self.path(self.steps(2))
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 1, out)
        self.assertIn("malformed oracle record", out)

    def test_comment_lines_are_ignored(self):
        oracle = self.path(["# header comment"] + self.steps(3))
        mine = self.path(["# header comment"] + self.steps(3))
        rc, out = run_tool([oracle, mine, "0"])
        self.assertEqual(rc, 0, out)
        self.assertIn("MATCH: 3 steps identical", out)

    def test_missing_recomp_file_is_a_usage_error(self):
        oracle = self.path(self.steps(2))
        rc, out = run_tool([oracle, os.path.join(self.d, "nope.trace"), "0"])
        self.assertEqual(rc, 2, out)
        self.assertIn("cannot read recomp trace", out)

    def test_missing_oracle_file_is_a_usage_error(self):
        mine = self.path(self.steps(2))
        rc, out = run_tool([os.path.join(self.d, "nope.trace"), mine, "0"])
        self.assertEqual(rc, 2, out)
        self.assertIn("cannot read oracle trace", out)

    def test_wrong_arity_and_bad_entry_step_are_usage_errors(self):
        t = self.path(self.steps(1))
        for argv, needle in (
            ([t], "usage:"),
            ([t, t, "notanumber"], "entry-step must be a nonnegative integer"),
            ([t, t, "-1"], "entry-step must be a nonnegative integer"),
        ):
            rc, out = run_tool(argv)
            self.assertEqual(rc, 2, out)
            self.assertIn(needle, out)

    def test_crlf_traces_compare_equal(self):
        p1 = os.path.join(self.d, "crlf_o.trace")
        p2 = os.path.join(self.d, "crlf_m.trace")
        with open(p1, "w", newline="\r\n") as f:
            f.write("\n".join(self.steps(2)) + "\n")
        with open(p2, "w", newline="\r\n") as f:
            f.write("\n".join(self.steps(2)) + "\n")
        rc, out = run_tool([p1, p2, "0"])
        self.assertEqual(rc, 0, out)
        self.assertIn("MATCH: 2 steps identical", out)


class TestFuncdiffCmpContractUnit(unittest.TestCase):
    def setUp(self):
        sys.path.insert(0, TOOLS)
        for name in list(sys.modules):
            if name == "funcdiff_cmp":
                del sys.modules[name]
        global funcdiff_cmp
        import funcdiff_cmp as m

        self.m = m

    def test_norm_rejects_short_and_shapeless_records(self):
        for bad in ("1 pc=0x1", "1 2 3", "step pc=0x1 op=0x2 w", ""):
            with self.assertRaises(self.m.TraceFormatError):
                self.m.norm(bad, 1, "synthetic")

    def test_norm_keeps_write_sets_as_sets(self):
        pc, op, writes = self.m.norm("3 pc=0x00000100 op=0x00000021 r1=0x1 r2=0x2", 1, "s")
        self.assertEqual((pc, op), ("pc=0x00000100", "op=0x00000021"))
        self.assertEqual(writes, frozenset({"r1=0x1", "r2=0x2"}))

    def test_oracle_slice_requires_full_coverage_but_not_file_end(self):
        import tempfile

        def slice_of(lines, entry, need):
            with tempfile.NamedTemporaryFile("w", suffix=".trace", delete=False, newline="\n") as fh:
                fh.write("\n".join(lines) + "\n")
                name = fh.name
            self.addCleanup(os.unlink, name)
            return self.m.load_oracle_slice(name, entry, need)

        rows = [f"{i} pc=0x0000{i:04x} op=0x00000021" for i in range(6)]
        got = slice_of(rows, 2, 3)
        self.assertEqual([r[0] for r in got], ["pc=0x00000002", "pc=0x00000003", "pc=0x00000004"])
        with self.assertRaises(self.m.TraceFormatError):
            slice_of([f"{i} pc=0x1 op=0x2" for i in range(3)], 0, 4)
        with self.assertRaises(self.m.TraceFormatError):
            slice_of(rows, 6, 1)


CC = shutil.which("gcc")
ROOT = os.path.dirname(TOOLS)
RT = os.path.join(ROOT, "src", "rt")
FUNC_ENTRY = 0x00001000
# Replaces the gate stub's HLE pair: a syscall becomes an import call (sr_hle_call) whose
# handler longjmps out, as an unimplemented NID does in hle.c.
FUNCDIFF_HLE_STUB = """\
#include "recomp.h"
uint32_t sr_syscall(CpuState *s, uint32_t nid) { (void)s; (void)nid; longjmp(g_hle_jmp, 1); }
void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc) { (void)pc; sr_hle_call(s, code); }
"""


@unittest.skipUnless(CC, "gcc is required to build src/rt/funcdiff.c")
class TestFuncdiffReplay(unittest.TestCase):
    """src/rt/funcdiff.c built headlessly (the codegen-gate link set with funcdiff.c as main)
    against a synthetic function, replaying synthetic reference traces."""

    WORDS = [
        (0x11 << 26) | (8 << 21) | (1 << 16) | 2,   # 0x1000 bc1t +2 (to 0x100c)
        0x00000000,                                  # 0x1004 nop (delay slot)
        0x3409BEEF,                                  # 0x1008 ori t1, zero, 0xbeef
        0x03E00008,                                  # 0x100c jr ra
        0x00000000,                                  # 0x1010 nop
        (0x210C << 6) | 0x0C,                        # 0x1014 syscall 0x210c (an import)
        0x03E00008,                                  # 0x1018 jr ra
        0x00000000,                                  # 0x101c nop
    ]

    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, TOOLS)
        from test_codegen_fp_convert import _synthetic_elf

        cls._tmp = tempfile.TemporaryDirectory()
        work = cls.work = cls._tmp.name
        elf = os.path.join(work, "f.elf")
        with open(elf, "wb") as fh:
            fh.write(_synthetic_elf(cls.WORDS))
        cls.image = os.path.join(work, "image.bin")
        with open(cls.image, "wb") as fh:
            fh.write(b"\0" * 16)
        gen = os.path.join(work, "gen.c")
        steps = [
            [sys.executable, os.path.join(TOOLS, "codegen.py"), elf, gen],
            [sys.executable, os.path.join(TOOLS, "title_runtime_config.py"),
             "--output", os.path.join(work, "sr_title_config.h")],
        ]
        for argv in steps:
            proc = subprocess.run(argv, capture_output=True, text=True, cwd=ROOT)
            if proc.returncode != 0:
                raise AssertionError(proc.stdout + proc.stderr)
        stub_src = os.path.join(work, "funcdiff_hle_stub.c")
        with open(stub_src, "w", encoding="ascii") as fh:
            fh.write(FUNCDIFF_HLE_STUB)
        flags = [CC, "-O0", "-w", "-D_CRT_SECURE_NO_WARNINGS", "-DSR_INSTRUCTION_TRACE",
                 "-DSR_GATE_BUILD", "-I", RT, "-I", work]
        gate_obj = os.path.join(work, "gate_stub.o")
        chunks = sorted(os.path.join(work, n) for n in os.listdir(work)
                        if n.startswith("gen_") and n.endswith(".c"))
        cls.exe = os.path.join(work, "funcdiff.exe")
        builds = [
            flags + ["-Dsr_syscall=gate_unused_sr_syscall",
                     "-Dsr_raw_syscall=gate_unused_sr_raw_syscall",
                     "-c", os.path.join(TOOLS, "gate_stub.c"), "-o", gate_obj],
            flags + ["-o", cls.exe, gen, *chunks, stub_src, gate_obj,
                     *(os.path.join(RT, n) for n in (
                         "funcdiff.c", "recomp.c", "guest_interp.c", "cpu_lle.c",
                         "domain_mode.c", "stale_code.c", "vfpu_tables.c", "title_config.c",
                         "nested_frames.c")),
                     os.path.join(ROOT, "src", "core", "nk_shared_helpers.c"), "-lm"],
        ]
        for argv in builds:
            proc = subprocess.run(argv, capture_output=True, text=True, cwd=ROOT)
            if proc.returncode != 0:
                raise AssertionError(proc.stdout + proc.stderr)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def replay(self, name, lines, target=FUNC_ENTRY, step=0):
        ref = os.path.join(self.work, name + ".ref")
        out = os.path.join(self.work, name + ".out")
        with open(ref, "w", encoding="ascii", newline="\n") as fh:
            fh.write("\n".join(lines) + "\n")
        proc = subprocess.run([self.exe, self.image, "08900000", ref, f"{target:x}", str(step), out],
                              capture_output=True, text=True, timeout=60)
        trace = ""
        if os.path.exists(out):
            with open(out, encoding="ascii") as fh:
                trace = fh.read()
            os.unlink(out)
        return proc, trace

    INIT = "# init r29=0x09f00000 r31=0x08800000"

    def test_replayed_fcr31_sets_the_fcc0_bc1t_reads(self):
        """Failing before: fcr31 was replayed without fpcond, so bc1t fell through to the ori."""
        proc, trace = self.replay("fcc0", [self.INIT + " fcr31=0x00800000",
                                           "0 pc=0x00001000 op=0x45010002"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertNotIn("r9=", trace)
        self.assertIn("pc=0x0000100c", trace)
        proc, trace = self.replay("fcc0_clear", [self.INIT + " fcr31=0x00000000",
                                                 "0 pc=0x00001000 op=0x45010002"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("r9=0x0000beef", trace)

    def test_stop_at_an_import_is_a_partial_run(self):
        """Failing before: the longjmp out at an import still exited 0."""
        proc, trace = self.replay("hle", [self.INIT, "0 pc=0x00001014 op=0x0008430c"],
                                  target=FUNC_ENTRY + 0x14)
        self.assertEqual(proc.returncode, 3, proc.stderr)
        self.assertIn("PARTIAL", proc.stderr)
        self.assertIn("pc=0x00001014", trace)

    def test_step_line_without_pc_field_is_refused(self):
        """Failing before: pc_s + 3 was parsed without the pc= prefix."""
        proc, _ = self.replay("nopc", [self.INIT, "0 xx 0x00000000",
                                       "1 pc=0x00001000 op=0x45010002"], step=1)
        self.assertEqual(proc.returncode, 2, proc.stderr)
        self.assertIn("no pc= and op= fields", proc.stderr)

    def test_line_longer_than_the_buffer_is_refused(self):
        """Failing before: fgets split the line and its tail was replayed as a step line."""
        filler = " ".join(f"r1=0x{i:08x}" for i in range(7000))   # about 98 KB
        proc, _ = self.replay("long", [self.INIT + " " + filler,
                                       "0 pc=0x00001000 op=0x45010002"])
        self.assertEqual(proc.returncode, 2, proc.stderr)
        self.assertIn("does not fit", proc.stderr)


if __name__ == "__main__":
    unittest.main()
