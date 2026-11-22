# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Committed mutation regressions for the scalar FCR31 semantics slice.

The manual campaign that drove the original fix is encoded here so the kill
evidence stays reproducible instead of living only in a PR description.

Two complementary layers:

* Behavioral mutants (M1..M4, M9..M12): each one patches a COPY of
  ``fp_convert.h`` in a temporary directory (the selftest TU is copied beside
  it so quoted-include resolution picks up the mutant) and requires the
  optimized native selftest to FAIL. Following tools/test_build_truth.py's
  mutation pattern, every replacement asserts its anchor exists first, so a
  refactor that silently orphans a mutant cannot produce a vacuous pass.

  A behavioral kill is staged, never a bare nonzero status: the mutant must
  build, must actually execute, must exit nonzero, and must print the guest
  semantic diagnostic the mutation is supposed to break. A compiler error, a
  launch failure, a host toolchain problem or a timeout is INVALID evidence,
  never a kill. Every mutant also carries its expected diagnostic label so a
  mutant cannot be "killed" by an unrelated assertion, and a pristine control
  proves the same harness passes the unmutated header.

* Structural regressions (fast path removal, FCC0 coherence, cvt.s.w
  routing): instant source-shape anchors over tools/codegen.py,
  src/rt/fp_convert.h and src/ref/interp.cpp. These are the committed-form
  counterparts of mutants M5/M6/M7/M8 plus the Disposition-A rule that no
  native fast path may come back without a validated host-invariant design.
"""

from __future__ import annotations

import dataclasses
import functools
import hashlib
import os
import platform
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))

import scratch_dir  # noqa: E402

CC = shutil.which("gcc")

HEADER = ROOT / "src" / "rt" / "fp_convert.h"
SELFTEST = ROOT / "tests" / "native" / "fp_convert_selftest.c"
CODEGEN = ROOT / "tools" / "codegen.py"
REF_INTERP = ROOT / "src" / "ref" / "interp.cpp"

# Separate phase budgets: a wedged compiler and a wedged mutant are different
# failures and must be named differently (see classify_mutant_run).
BUILD_TIMEOUT_S = 300.0
RUN_TIMEOUT_S = 120.0
# fp_convert_selftest.c returns 1 after printing its own accounting line; any
# other nonzero status means the host killed the process, which is only
# accepted together with the expected guest semantic diagnostic.
SELFTEST_VERDICT_RC = 1
SELFTEST_SUMMARY = "fp_convert_selftest: "
SELFTEST_PRISTINE_MARKER = "fixed-vector checks passed"

SEMANTIC_KILL = "SEMANTIC_KILL"
SURVIVED = "SURVIVED"
INVALID_BUILD_TIMEOUT = "INVALID_BUILD_TIMEOUT"
INVALID_BUILD_LAUNCH_FAILED = "INVALID_BUILD_LAUNCH_FAILED"
INVALID_BUILD_FAILED = "INVALID_BUILD_FAILED"
INVALID_RUN_TIMEOUT = "INVALID_RUN_TIMEOUT"
INVALID_RUN_LAUNCH_FAILED = "INVALID_RUN_LAUNCH_FAILED"
INVALID_NON_SEMANTIC_FAILURE = "INVALID_NON_SEMANTIC_FAILURE"
INVALID_NO_SELFTEST_ACCOUNTING = "INVALID_NO_SELFTEST_ACCOUNTING"


@dataclasses.dataclass(frozen=True)
class StagedRun:
    """One staged mutant attempt: build status, run status, and raw output.

    ``build_rc``/``run_rc`` are ``None`` when the phase never produced a
    status (launch error, or timeout before any status was observable).
    """

    build_rc: int | None
    build_output: str
    run_rc: int | None
    run_output: str
    build_timed_out: bool = False
    run_timed_out: bool = False

    @property
    def combined_output(self) -> str:
        return self.build_output + self.run_output


def classify_mutant_run(staged: StagedRun, expected_diagnostics: tuple[str, ...]) -> str:
    """Name a staged mutant verdict.

    Only a mutant that BUILT, LAUNCHED, EXECUTED and printed every expected
    guest semantic diagnostic while exiting nonzero is a behavioral kill. Every
    other outcome is named so the failure text says which discipline was
    violated instead of collapsing everything into "nonzero".
    """
    if staged.build_timed_out or staged.build_rc is None:
        return INVALID_BUILD_TIMEOUT if staged.build_timed_out else INVALID_BUILD_LAUNCH_FAILED
    if staged.build_rc != 0:
        return INVALID_BUILD_FAILED
    if staged.run_timed_out or staged.run_rc is None:
        return INVALID_RUN_TIMEOUT if staged.run_timed_out else INVALID_RUN_LAUNCH_FAILED
    if staged.run_rc == 0:
        return SURVIVED
    if any(diagnostic not in staged.run_output for diagnostic in expected_diagnostics):
        return INVALID_NON_SEMANTIC_FAILURE
    if staged.run_rc == SELFTEST_VERDICT_RC and SELFTEST_SUMMARY not in staged.run_output:
        return INVALID_NO_SELFTEST_ACCOUNTING
    return SEMANTIC_KILL


def _build_mutated_selftest(header_text: str) -> StagedRun:
    """Compile and run the real selftest against a mutated header copy at -O2."""
    assert CC is not None
    with scratch_dir.TemporaryDirectory(prefix="fp_scalar_mut_") as tmp:
        work = Path(tmp)
        (work / "fp_convert.h").write_text(header_text, encoding="utf-8", newline="\n")
        shutil.copyfile(SELFTEST, work / "fp_convert_selftest.c")
        exe = work / "mutant_selftest.exe"
        command = [
            CC, "-O2", "-fno-strict-aliasing",
            "-DSR_SDL3VK", "-D_CRT_SECURE_NO_WARNINGS",
            "-I", str(work),
            str(work / "fp_convert_selftest.c"), "-lm", "-o", str(exe),
        ]
        try:
            compiled = subprocess.run(command, capture_output=True, text=True,
                                      timeout=BUILD_TIMEOUT_S)
        except subprocess.TimeoutExpired as exc:
            return StagedRun(None, _timeout_text(command, exc), None, "", build_timed_out=True)
        except OSError as exc:
            return StagedRun(None, f"compiler launch failed: {exc}", None, "")
        build_output = compiled.stderr + compiled.stdout
        if compiled.returncode != 0:
            # Build breakage is NOT a behavioral kill; keep it staged and named.
            return StagedRun(compiled.returncode, build_output, None, "")
        try:
            ran = subprocess.run([str(exe)], capture_output=True, text=True,
                                 timeout=RUN_TIMEOUT_S)
        except subprocess.TimeoutExpired as exc:
            return StagedRun(0, build_output, None, _timeout_text([str(exe)], exc),
                             run_timed_out=True)
        except OSError as exc:
            return StagedRun(0, build_output, None, f"mutant launch failed: {exc}")
        return StagedRun(0, build_output, ran.returncode, ran.stderr + ran.stdout)


def _timeout_text(command: list[str], exc: subprocess.TimeoutExpired) -> str:
    detail = exc.stderr or ""
    if isinstance(detail, bytes):
        detail = detail.decode("utf-8", "replace")
    return f"phase timeout\ncommand: {' '.join(command)}\n{detail}"


def _bounded_run(command: list, timeout_s: float, timeout_verdict: str,
                 **kwargs) -> subprocess.CompletedProcess:
    """One phase of a dedicated mutant harness, under the same budgets as the staged helper.

    A wedged compiler, generator or mutant fails the test by its INVALID name instead of
    hanging the suite until the CI job is killed; a timeout is never a kill.
    """
    try:
        return subprocess.run(command, capture_output=True, text=True, timeout=timeout_s,
                              **kwargs)
    except subprocess.TimeoutExpired as exc:
        raise AssertionError(
            f"{timeout_verdict}: a timed-out phase is not a semantic kill\n"
            + _timeout_text([str(part) for part in command], exc)) from None


# x86 machine names, for the guard that an x86 host never silently skips the MXCSR mutants.
X86_MACHINES = frozenset({"x86_64", "amd64", "i386", "i486", "i586", "i686", "x86"})


def _sse_backend_selected(predefined_macros: str) -> bool:
    """Whether a compiler with these predefined macros builds fp_convert.h's MXCSR backend.

    The header picks it under ``__SSE2__ || _M_X64 || __i386__``. Every behavioural mutant
    anchor (rm_to_mxcsr, the FTZ and DAZ bits, ``_mm_setcsr(saved)``) lives in that branch,
    and the M13 harness needs <xmmintrin.h>; an AArch64 compiler builds the FPCR backend
    instead, where those mutants are dead code that would SURVIVE for no semantic reason.
    """
    defined = {line.split()[1] for line in predefined_macros.splitlines()
               if line.startswith("#define ") and len(line.split()) >= 2}
    return bool(defined & {"__SSE2__", "_M_X64", "__i386__"})


@functools.lru_cache(maxsize=None)
def _cc_compiles_sse_backend(cc: str) -> bool:
    """Ask ``cc`` for its predefined macros; a compiler that cannot answer builds nothing."""
    try:
        probe = subprocess.run([cc, "-dM", "-E", "-x", "c", os.devnull], capture_output=True,
                               text=True, timeout=BUILD_TIMEOUT_S)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return probe.returncode == 0 and _sse_backend_selected(probe.stdout)


class FpMutationVerdictTests(unittest.TestCase):
    """The staged verdict policy, driven without a compiler.

    These lock the rule that build/launch/timeout failures are never semantic
    kills; the real mutant tests below supply the compiler-side evidence.
    """

    def setUp(self) -> None:
        # The staging tests mock subprocess.run, so the compiler only needs a name; without one,
        # _build_mutated_selftest's `assert CC is not None` fires on a host with no gcc on PATH.
        patcher = mock.patch.object(sys.modules[__name__], "CC", CC or "gcc")
        patcher.start()
        self.addCleanup(patcher.stop)

    def _staged(
        self,
        *,
        build_rc: int | None = 0,
        build_output: str = "",
        run_rc: int | None = 1,
        run_output: str = "FAIL: guest rounding wrong\n",
        build_timed_out: bool = False,
        run_timed_out: bool = False,
    ) -> StagedRun:
        return StagedRun(
            build_rc, build_output, run_rc, run_output, build_timed_out, run_timed_out
        )

    def test_the_mocked_staging_tests_run_on_a_host_without_gcc(self):
        """The verdict tests mock the compiler, so a host with no gcc on PATH still runs them."""
        names = ("test_mocked_compiler_failure_never_reaches_execution",
                 "test_mocked_launch_failure_after_clean_build_is_invalid",
                 "test_mocked_run_timeout_is_invalid_not_a_kill")
        result = unittest.TestResult()
        with mock.patch.object(sys.modules[__name__], "CC", None):
            for name in names:
                type(self)(name).run(result)
        self.assertEqual([str(test) for test, _ in result.errors + result.failures], [])
        self.assertEqual(result.testsRun, len(names))

    def test_the_sse_backend_is_read_from_the_compiler_macros(self):
        self.assertTrue(_sse_backend_selected("#define __x86_64__ 1\n#define __SSE2__ 1\n"))
        self.assertTrue(_sse_backend_selected("#define __i386__ 1\n"))
        self.assertFalse(_sse_backend_selected("#define __aarch64__ 1\n#define __ARM_NEON 1\n"))
        self.assertFalse(_sse_backend_selected("#define __SSE2_MATH__ 1\n"))

    @unittest.skipUnless(platform.machine().lower() in X86_MACHINES, "an x86 host required")
    @unittest.skipUnless(CC, "TOOLCHAIN_MISSING: gcc required")
    def test_an_x86_host_compiles_the_sse_backend(self):
        """The skip below must never fire where the MXCSR mutants are live code."""
        self.assertTrue(_cc_compiles_sse_backend(CC),
                        f"{CC} on {platform.machine()} reported no SSE backend macro")

    def test_mxcsr_mutants_skip_where_the_sse_backend_is_not_built(self):
        """On an AArch64 compiler the MXCSR mutant text is dead code and the M13 harness's
        <xmmintrin.h> is absent: those tests skip by name instead of failing falsely."""
        names = ("test_M1_ignore_rounding_mode", "test_M9_helper_leaks_modified_mxcsr",
                 "test_M12_drop_volatile_result_window",
                 "test_M13_partial_restore_preserves_stickies")
        module = sys.modules[__name__]
        result = unittest.TestResult()
        with mock.patch.object(module, "_cc_compiles_sse_backend", return_value=False), \
             mock.patch.object(module, "_build_mutated_selftest",
                               side_effect=AssertionError("built a dead-code mutant")), \
             mock.patch.object(subprocess, "run",
                               side_effect=AssertionError("compiled an x86-only harness")):
            for name in names:
                FpScalarHeaderMutantTests(name).run(result)
        self.assertEqual([str(test) for test, _ in result.errors + result.failures], [])
        self.assertEqual(len(result.skipped), len(names))
        for _, reason in result.skipped:
            self.assertIn("MXCSR backend", reason)

    def test_dedicated_harness_phases_are_bounded(self):
        """M13 and the codegen-copy mutants bound every phase; a wedged run is named INVALID."""
        def fake_run(command, **kwargs):
            if "timeout" not in kwargs:
                raise RuntimeError(f"unbounded phase {command[0]}: a wedged mutant would "
                                   "hang the suite")
            if Path(command[0]).name in {"m13.exe", "mutant_test.exe"}:
                raise subprocess.TimeoutExpired(cmd=command, timeout=kwargs["timeout"])
            return subprocess.CompletedProcess(command, 0, "", "")

        module = sys.modules[__name__]
        cases = ((FpScalarHeaderMutantTests, "test_M13_partial_restore_preserves_stickies"),
                 (FastPathRemovedStructuralTests,
                  "test_behavioral_mul_daz_precheck_mutant_is_killed"),
                 (Ctc1MeasuredMaskTests, "test_behavioral_wholesale_ctc1_mutant_is_killed"))
        with mock.patch.object(module, "_cc_compiles_sse_backend", return_value=True), \
             mock.patch.object(subprocess, "run", side_effect=fake_run):
            for cls, name in cases:
                with self.subTest(test=name):
                    with self.assertRaisesRegex(AssertionError, INVALID_RUN_TIMEOUT):
                        getattr(cls(name), name)()

    def test_build_failure_is_never_a_semantic_kill(self):
        staged = self._staged(build_rc=1, build_output="error: 'sr_rm_to' undeclared",
                              run_rc=None, run_output="")
        self.assertEqual(classify_mutant_run(staged, ("FAIL: guest rounding wrong",)),
                         INVALID_BUILD_FAILED)

    def test_build_launch_failure_is_never_a_semantic_kill(self):
        staged = self._staged(build_rc=None, build_output="compiler launch failed",
                              run_rc=None, run_output="")
        self.assertEqual(classify_mutant_run(staged, ("FAIL: guest rounding wrong",)),
                         INVALID_BUILD_LAUNCH_FAILED)

    def test_build_timeout_is_never_a_semantic_kill(self):
        staged = self._staged(build_rc=None, build_output="phase timeout",
                              run_rc=None, run_output="", build_timed_out=True)
        self.assertEqual(classify_mutant_run(staged, ("FAIL: guest rounding wrong",)),
                         INVALID_BUILD_TIMEOUT)

    def test_run_launch_failure_is_never_a_semantic_kill(self):
        staged = self._staged(run_rc=None, run_output="mutant launch failed")
        self.assertEqual(classify_mutant_run(staged, ("FAIL: guest rounding wrong",)),
                         INVALID_RUN_LAUNCH_FAILED)

    def test_run_timeout_is_never_a_semantic_kill(self):
        staged = self._staged(run_rc=None, run_output="phase timeout", run_timed_out=True)
        self.assertEqual(classify_mutant_run(staged, ("FAIL: guest rounding wrong",)),
                         INVALID_RUN_TIMEOUT)

    def test_surviving_mutant_is_reported_as_survived(self):
        staged = self._staged(run_rc=0,
                              run_output="fp_convert_selftest: all 338 fixed-vector checks passed\n")
        self.assertEqual(classify_mutant_run(staged, ("FAIL: guest rounding wrong",)),
                         SURVIVED)

    def test_nonzero_without_expected_diagnostic_is_not_a_kill(self):
        staged = self._staged(run_output="FAIL: an unrelated vector row\n")
        self.assertEqual(classify_mutant_run(staged, ("FAIL: guest rounding wrong",)),
                         INVALID_NON_SEMANTIC_FAILURE)

    def test_selftest_verdict_needs_its_own_accounting_line(self):
        staged = self._staged(run_rc=SELFTEST_VERDICT_RC,
                              run_output="FAIL: guest rounding wrong\n")
        self.assertEqual(classify_mutant_run(staged, ("FAIL: guest rounding wrong",)),
                         INVALID_NO_SELFTEST_ACCOUNTING)

    def test_executed_semantic_failure_is_a_kill(self):
        staged = self._staged(
            run_rc=SELFTEST_VERDICT_RC,
            run_output="FAIL: guest rounding wrong\nfp_convert_selftest: 1/338 checks FAILED\n")
        self.assertEqual(classify_mutant_run(staged, ("FAIL: guest rounding wrong",)),
                         SEMANTIC_KILL)

    def test_host_killed_process_with_expected_diagnostic_is_a_kill(self):
        """A host FP exception after the wrong-result rows is still evidence.

        The diagnostic rows are printed before the process dies, so the guest
        semantic violation was observed; the abnormal status only means the
        harness could not finish its accounting.
        """
        staged = self._staged(run_rc=0xC0000093,
                              run_output="FAIL: guest rounding wrong\n")
        self.assertEqual(classify_mutant_run(staged, ("FAIL: guest rounding wrong",)),
                         SEMANTIC_KILL)

    def test_every_named_verdict_is_distinct(self):
        verdicts = {
            SEMANTIC_KILL, SURVIVED, INVALID_BUILD_TIMEOUT,
            INVALID_BUILD_LAUNCH_FAILED, INVALID_BUILD_FAILED, INVALID_RUN_TIMEOUT,
            INVALID_RUN_LAUNCH_FAILED, INVALID_NON_SEMANTIC_FAILURE,
            INVALID_NO_SELFTEST_ACCOUNTING,
        }
        self.assertEqual(len(verdicts), 9)

    def test_mocked_compiler_failure_never_reaches_execution(self):
        """A mocked compiler error must be staged as INVALID_BUILD_FAILED.

        Drives the real _build_mutated_selftest, so this is the defect the old
        single-returncode helper hid: it returned 1 and the caller read that as
        a behavioral kill without ever executing anything.
        """
        completed = subprocess.CompletedProcess(args=["gcc"], returncode=1,
                                                stdout="", stderr="error: undeclared")
        with mock.patch.object(sys.modules[__name__].subprocess, "run",
                               return_value=completed) as runner:
            staged = _build_mutated_selftest("/* mutated */\n")
        self.assertEqual(runner.call_count, 1,
                         "a failed build must not attempt execution")
        self.assertEqual(staged.build_rc, 1)
        self.assertIsNone(staged.run_rc, "no run status may be invented")
        self.assertEqual(
            classify_mutant_run(staged, ("FAIL: never printed",)),
            INVALID_BUILD_FAILED)

    def test_mocked_launch_failure_after_clean_build_is_invalid(self):
        def fake_run(command, **kwargs):
            if command and command[0] == CC:
                return subprocess.CompletedProcess(args=command, returncode=0,
                                                   stdout="", stderr="")
            raise OSError("exec format error")

        with mock.patch.object(sys.modules[__name__].subprocess, "run", side_effect=fake_run):
            staged = _build_mutated_selftest("/* mutated */\n")
        self.assertEqual(staged.build_rc, 0)
        self.assertIsNone(staged.run_rc)
        self.assertEqual(
            classify_mutant_run(staged, ("FAIL: never printed",)),
            INVALID_RUN_LAUNCH_FAILED)

    def test_mocked_run_timeout_is_invalid_not_a_kill(self):
        def fake_run(command, **kwargs):
            if command and command[0] == CC:
                return subprocess.CompletedProcess(args=command, returncode=0,
                                                   stdout="", stderr="")
            raise subprocess.TimeoutExpired(cmd=command, timeout=RUN_TIMEOUT_S)

        with mock.patch.object(sys.modules[__name__].subprocess, "run", side_effect=fake_run):
            staged = _build_mutated_selftest("/* mutated */\n")
        self.assertEqual(staged.build_rc, 0)
        self.assertTrue(staged.run_timed_out)
        self.assertEqual(
            classify_mutant_run(staged, ("FAIL: never printed",)),
            INVALID_RUN_TIMEOUT)


class FpScalarHeaderMutantTests(unittest.TestCase):
    """Each behavioral mutant must be killed by the committed selftest."""

    def _require_sse_backend(self) -> None:
        if CC is None:
            self.skipTest("TOOLCHAIN_MISSING: gcc required")
        if not _cc_compiles_sse_backend(CC):
            self.skipTest(f"{CC} does not build fp_convert.h's MXCSR backend, where these "
                          "mutant anchors live: they would be dead code there")

    def assert_killed(self, old: str, new: str, expected_diagnostics: tuple[str, ...]) -> None:
        self._require_sse_backend()
        original = HEADER.read_text(encoding="utf-8")
        self.assertIn(old, original, f"mutation anchor vanished from {HEADER.name}: {old!r}")
        staged = _build_mutated_selftest(original.replace(old, new))
        verdict = classify_mutant_run(staged, expected_diagnostics)
        self.assertEqual(
            verdict, SEMANTIC_KILL,
            f"{verdict} (anchor={old!r}, expected diagnostics="
            f"{list(expected_diagnostics)}): a mutant counts as killed only when "
            "it builds, executes and fails the guest semantic assertion it "
            "breaks\n" + staged.combined_output[-2000:])
        print(f"MUTANT_EXECUTED_AND_SEMANTIC_TEST_FAILED ({verdict}) "
              f"run_rc={staged.run_rc}")

    @unittest.skipUnless(CC, "TOOLCHAIN_MISSING: gcc required")
    def test_pristine_header_passes_the_same_harness(self):
        """Control for the staged kills below: the unmutated header must build,
        execute and pass, so the mutant diagnostics cannot be vacuous."""
        staged = _build_mutated_selftest(HEADER.read_text(encoding="utf-8"))
        self.assertEqual(staged.build_rc, 0,
                         f"PRISTINE_BUILD_FAILED\n{staged.combined_output}")
        self.assertEqual(staged.run_rc, 0,
                         f"PRISTINE_SELFTEST_FAILED: production FP semantics "
                         f"regressed\n{staged.combined_output}")
        self.assertIn(SELFTEST_PRISTINE_MARKER, staged.run_output,
                      "PRISTINE_NO_SELFTEST_ACCOUNTING\n" + staged.run_output)

    @unittest.skipUnless(CC, "TOOLCHAIN_MISSING: gcc required")
    def test_M1_ignore_rounding_mode(self):
        self.assert_killed(
            "csr |= rm_to_mxcsr[fcr31 & SR_FCR31_RM_MASK] << 13;",
            "csr |= 0u << 13;",
            ("FAIL: mul RM anchors RZ", "FAIL: div 1/3 RZ"))

    @unittest.skipUnless(CC, "TOOLCHAIN_MISSING: gcc required")
    def test_M2_force_RN(self):
        self.assert_killed(
            "{0u, 3u, 2u, 1u}", "{0u, 0u, 0u, 0u}",
            ("FAIL: mul RM anchors RZ", "FAIL: add tie RP bumps"))

    @unittest.skipUnless(CC, "TOOLCHAIN_MISSING: gcc required")
    def test_M3_ignore_FS_flush_gate(self):
        self.assert_killed(
            "csr |= 1u << 15;", "(void)fcr31;",
            ("FAIL: mul FS=1 flush",))

    @unittest.skipUnless(CC, "TOOLCHAIN_MISSING: gcc required")
    def test_M4_force_FTZ_regardless_of_guest(self):
        self.assert_killed(
            "csr &= ~(1u << 15);", "csr |= 1u << 15;",
            ("FAIL: mul FS=0 gradual", "FAIL: mul subnormal input exact"))

    @unittest.skipUnless(CC, "TOOLCHAIN_MISSING: gcc required")
    def test_M9_helper_leaks_modified_mxcsr(self):
        # The anchor ends sr_fpu_mul_s: the software FS flush (AArch64 only) sits between
        # the product and the closing barrier.
        self.assert_killed(
            "    if (fcr31 & SR_FCR31_FS) vr = sr_fpu_soft_ftz_mul(va, vb, vr);\n"
            "#endif\n"
            "    sr_fpu_scalar_barrier();\n"
            "    sr_fpu_env_restore(saved);",
            "    if (fcr31 & SR_FCR31_FS) vr = sr_fpu_soft_ftz_mul(va, vb, vr);\n"
            "#endif\n"
            "    sr_fpu_scalar_barrier();",
            ("FAIL: hostile-env restoration inexact",
             "FAIL: helper left host FP env modified"))

    @unittest.skipUnless(CC, "TOOLCHAIN_MISSING: gcc required")
    def test_M11_preserve_ambient_DAZ(self):
        self.assert_killed(
            "csr &= ~(1u << 6);                              /* DAZ off: inputs stay gradual */",
            "",
            ("FAIL: subnormal input keeps weight under hostile DAZ",))

    @unittest.skipUnless(CC, "TOOLCHAIN_MISSING: gcc required")
    def test_M12_drop_volatile_result_window(self):
        # The pre-fix mechanism: barrier-bracketed plain expression. Killed by
        # the folding/reorder guards and the hostile matrix at -O2.
        #
        # Known Windows-only detail: once the mutant lets MXCSR exception masks
        # leak, the unhandled host FP underflow exception (0xC0000093) can end
        # the process AFTER the wrong-result rows are printed. The expected
        # diagnostic requirement above is what makes that still a semantic
        # kill instead of an unexamined crash.
        self.assert_killed(
            "volatile float vr = va * vb;", "const float vr = va * vb;",
            ("FAIL: mul gradual out under FTZ", "FAIL: mul subnormal in under FTZ"))

    @unittest.skipUnless(CC, "TOOLCHAIN_MISSING: gcc required")
    def test_M13_partial_restore_preserves_stickies(self):
        """A helper that restores control fields but leaves sticky flags raised
        inside its window violates bit-for-bit caller restoration.

        Deterministic dedicated harness (ambient-state independent): establish
        a controlled caller state by clearing ONLY the sticky bits, capture
        before, execute a runtime inexact operation through sr_fpu_div_s
        (volatile operands cannot constant-fold), capture after. Production
        helper must restore exactly (after == before); the M13 partial-restore
        mutant must leak a newly-raised sticky bit (after != before).

        Staged evidence only -- MUTANT_EXECUTED_AND_SEMANTIC_TEST_FAILED with
        an explicit sticky-delta diagnostic counts; build/harness failures do
        not. Both directions are proven: pristine header PASSES the same
        harness, so the discriminator is not vacuous.
        """
        self._require_sse_backend()

        harness_c = """\
#include <stdio.h>
#include <xmmintrin.h>
#include "fp_convert.h"

int main(void) {
    const uint32_t outer = _mm_getcsr();
    /* TEST-ONLY setup: clear ONLY sticky status bits; every non-sticky field
     * (RC, masks, FTZ, DAZ) is preserved from the outer environment. */
    const uint32_t controlled = outer & ~0x3fu;
    _mm_setcsr(controlled);
    const uint32_t before = _mm_getcsr();

    volatile float one = 1.0f;
    volatile float three = 3.0f;
    float r = sr_fpu_div_s(one, three, 0u);   /* runtime inexact: raises PE */

    const uint32_t after = _mm_getcsr();
    const uint32_t delta = after ^ before;
    printf("before=%08x\\nafter=%08x\\ndelta=%08x\\n", before, after, delta);
    if (sr_float_bits(r) != 0x3eaaaaabu) {
        fprintf(stderr, "RESULT_WRONG got=%08x\\n", sr_float_bits(r));
        _mm_setcsr(outer);
        return 3;
    }
    _mm_setcsr(outer);
    if (after != before) {
        fprintf(stderr, "STICKY_LEAK before=%08x after=%08x delta=%08x\\n",
                before, after, delta);
        return 1;
    }
    printf("restoration exact\\n");
    return 0;
}
"""

        assert CC is not None

        def build_and_run(header_text: str, tag: str):
            with scratch_dir.TemporaryDirectory(prefix=f"fp_scalar_m13_{tag}_") as tmp:
                work = Path(tmp)
                (work / "fp_convert.h").write_text(header_text, encoding="utf-8", newline="\n")
                (work / "m13_harness.c").write_text(harness_c, encoding="utf-8", newline="\n")
                exe = work / "m13.exe"
                command = [CC, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                           "-I", str(work),
                           str(work / "m13_harness.c"), "-lm", "-o", str(exe)]
                compiled = _bounded_run(command, BUILD_TIMEOUT_S, INVALID_BUILD_TIMEOUT)
                if compiled.returncode != 0:
                    return None, compiled.stderr + compiled.stdout
                ran = _bounded_run([str(exe)], RUN_TIMEOUT_S, INVALID_RUN_TIMEOUT)
                return ran.returncode, ran.stderr + ran.stdout

        original = HEADER.read_text(encoding="utf-8")
        anchor = ("static inline void sr_fpu_env_restore(uint32_t saved) {\n"
                  "    _mm_setcsr(saved);\n"
                  "}")
        mutant_restore = ("static inline void sr_fpu_env_restore(uint32_t saved) {\n"
                          "    _mm_setcsr((saved & ~0x3fu) | (_mm_getcsr() & 0x3fu));\n"
                          "}")
        self.assertIn(anchor, original, "restore anchor drifted")

        # Direction 1 (discriminator validity): PRISTINE header must pass.
        pristine_rc, pristine_out = build_and_run(original, "pristine")
        self.assertEqual(pristine_rc, 0,
                         f"M13 harness rejected the PRODUCTION helper; "
                         f"production restoration defect:\n{pristine_out}")
        self.assertIn("restoration exact", pristine_out)

        # Direction 2: the M13 mutant must build, run, and fail SEMANTICALLY
        # with a newly-leaked sticky bit in the diagnostic delta.
        mutant_rc, mutant_out = build_and_run(
            original.replace(anchor, mutant_restore), "mutant")
        self.assertIsNotNone(
            mutant_rc,
            "M13_MUTANT_BUILD_FAILED: compilation failure is not a "
            "behavioral kill\n" + mutant_out)
        self.assertNotEqual(
            mutant_rc, 0,
            "M13_SURVIVED: partial-restore mutant passed the controlled "
            "sticky-leak harness\n" + mutant_out)
        self.assertIn("STICKY_LEAK", mutant_out,
                      "M13_NON_SEMANTIC_FAILURE: no sticky-leak diagnostic\n"
                      + mutant_out)
        delta_line = next((line for line in mutant_out.splitlines()
                           if line.startswith("delta=")), "")
        self.assertTrue(delta_line, f"M13 diagnostic missing delta=\n{mutant_out}")
        delta = int(delta_line.split("=")[1], 16)
        self.assertTrue(
            delta & 0x3f,
            f"M13_EXPECTED_STICKY_LEAK_OBSERVED failed: delta={delta_line} "
            "carries no sticky bit")
        print(f"M13_EXPECTED_STICKY_LEAK_OBSERVED ({delta_line.strip()})")


class FastPathRemovedStructuralTests(unittest.TestCase):
    """Disposition A: no native fast path may exist without a validated host
    invariant, and the scoped helpers must be the only scalar mechanism."""

    def test_no_fast_path_predicate_anywhere(self):
        # Anchor on definition/usage shapes so the disposition comment may
        # still NAME the removed mechanism without tripping the guard.
        header = HEADER.read_text(encoding="utf-8")
        self.assertNotIn(
            "static inline int sr_fpu_scalar_fast", header,
            "native fast-path predicate redefined; see fp_convert.h FAST PATH "
            "DISPOSITION before reintroducing it")
        codegen = CODEGEN.read_text(encoding="utf-8")
        self.assertNotIn(
            "sr_fpu_scalar_fast(", codegen,
            "emitted code consults a native fast path again; a validated "
            "host-invariant design is a prerequisite (RISK-8)")

    def test_mul_inf_zero_classifier_is_raw_bit(self):
        """Structural anchor for the DAZ-precheck repair.

        The emitted mul.s inf*0 classification must read raw register-file
        bits; a floating isinf()/==0.0f precheck is guest-sensitive FP work
        outside the scoped window and misfires under hostile ambient DAZ.
        Anchors are contiguous fragments of codegen.py's emission template.
        """
        text = CODEGEN.read_text(encoding="utf-8")
        self.assertIn(
            'const uint32_t _ab=s->fi[{fs}],_bb=s->fi[{ft}];', text,
            "mul.s must classify inf*0 from raw fi[] bits")
        self.assertIn(
            'f"if((((_ab & 0x7fffffffu) == 0x7f800000u && (_bb & 0x7fffffffu) == 0u)) || "',
            text, "raw-bit inf classification fragment drifted")
        self.assertNotIn("isinf(_a)&&_b==0.0f", text,
                         "floating inf*0 precheck reintroduced into mul.s emission")

    def test_behavioral_mul_daz_precheck_mutant_is_killed(self):
        """Behavioral guard: a cleanly rebuilt PRE-REPAIR classifier (floating
        isinf()/==0.0f comparisons, no leftover raw-bit locals) must compile
        AND run AND fail the hostile-DAZ generated case semantically:
        inf * min-subnormal misclassified as inf * zero -> qNaN.

        The mutation is applied to a TEMP COPY of codegen.py; tracked bytes are
        never touched. Only MUTANT_EXECUTED_AND_SEMANTIC_TEST_FAILED counts as
        a valid kill; generation or compilation failure is reported and
        rejected as evidence.
        """
        if CC is None:
            self.skipTest("TOOLCHAIN_MISSING: gcc required")
        original = CODEGEN.read_text(encoding="utf-8")
        original_sha = hashlib.sha256(original.encode("utf-8")).hexdigest()

        # Exact contiguous fragments of the current raw-bit emission template.
        frag_decl = 'f"{{ const uint32_t _ab=s->fi[{fs}],_bb=s->fi[{ft}]; "'
        frag_if = ('f"if((((_ab & 0x7fffffffu) == 0x7f800000u '
                   '&& (_bb & 0x7fffffffu) == 0u)) || "')
        frag_sym = ('f"(((_bb & 0x7fffffffu) == 0x7f800000u '
                    '&& (_ab & 0x7fffffffu) == 0u))) "')
        frag_store = 'f"s->fi[{fdv}]=0x7fc00000u; "'
        for frag in (frag_decl, frag_if, frag_sym, frag_store):
            self.assertIn(frag, original, f"raw-bit classifier anchor drifted: {frag!r}")

        # Rebuild the pre-repair emission shape CLEANLY: no _ab/_bb leftovers,
        # floating classifier only. Emitted shape becomes exactly:
        #   { float _a=..., _b=...;
        #     if ((isinf(_a)&&_b==0.0f)||(isinf(_b)&&_a==0.0f)) canonical qNaN;
        #     else sr_fpu_mul_s(...); }
        mutant = (original
                  .replace(frag_decl, 'f"{{ "')
                  .replace(frag_if, "")
                  .replace(frag_sym, "")
                  .replace(frag_store,
                           'f"if((isinf(_a)&&_b==0.0f)||(isinf(_b)&&_a==0.0f)) '
                           's->fi[{fdv}]=0x7fc00000u; "'))
        self.assertNotEqual(mutant, original, "mutant construction produced no change")

        with scratch_dir.TemporaryDirectory(prefix="fp_scalar_codegen_mut_") as tmp:
            tmp_codegen = Path(tmp) / "mutant_codegen.py"
            tmp_codegen.write_text(mutant, encoding="utf-8", newline="\n")

            # Drive the real fixture pipeline through the mutated COPY.
            import tools.test_codegen_fp_convert as fixture

            # Stage 1: generation must succeed through the mutated codegen.
            gen_words = [
                fixture._ctc1(8),
                fixture._fps3(1, 0, 8, 0x02), fixture._mfc1(9, 8),
                0x03E00008, 0x00000000,
            ]
            elf_bytes = fixture._synthetic_elf(gen_words)
            gen_dir = Path(tmp) / "gen"
            gen_dir.mkdir()
            elf_path = gen_dir / "m.elf"
            elf_path.write_bytes(elf_bytes)
            import os as _os
            env = dict(_os.environ)
            env["PYTHONPATH"] = str(ROOT / "tools") + _os.pathsep + env.get("PYTHONPATH", "")
            gen_proc = _bounded_run(
                [sys.executable, str(tmp_codegen), str(elf_path),
                 str(gen_dir / "m.c"), "--profile=hst"],
                BUILD_TIMEOUT_S, INVALID_BUILD_TIMEOUT, cwd=ROOT, env=env)
            self.assertEqual(
                gen_proc.returncode, 0,
                "MUTANT_GENERATION_FAILED: mutated codegen.py could not run")

            # Stage 2: generated C must COMPILE under normal test flags.
            chunks = sorted(gen_dir.glob("m_*.c"))
            (gen_dir / "recomp.h").write_text(fixture.ISOLATED_RECOMP_H, encoding="ascii")
            harness = ('#include "m_funcs.h"\n'
                       + fixture.ISOLATED_STUBS_C + fixture._CELL_INFZERO_MAIN)
            (gen_dir / "harness.c").write_text(harness, encoding="ascii")
            exe = gen_dir / "mutant_test.exe"
            command = [CC, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                       "-I", str(gen_dir), "-I", str(ROOT / "src" / "rt")]
            if os.name != "nt":
                command.extend(["-fsanitize=undefined,float-cast-overflow",
                                "-fno-sanitize-recover=all"])
            command.extend([str(gen_dir / "harness.c"), str(gen_dir / "m.c"),
                            *(str(p) for p in chunks), "-lm", "-o", str(exe)])
            compiled = _bounded_run(command, BUILD_TIMEOUT_S, INVALID_BUILD_TIMEOUT)
            self.assertEqual(
                compiled.returncode, 0,
                "MUTANT_BUILD_FAILED: mutant must compile cleanly to prove a "
                "SEMANTIC kill; build output:\n" + (compiled.stderr + compiled.stdout)[-1500:])

            # Stage 3: executable must RUN and fail SEMANTICALLY.
            ran = _bounded_run([str(exe)], RUN_TIMEOUT_S, INVALID_RUN_TIMEOUT)
            self.assertNotEqual(
                ran.returncode, 0,
                "MUTANT_SURVIVED: FP-comparison classifier passed the "
                "hostile-DAZ fixture")
            combined = ran.stderr + ran.stdout
            self.assertIn(
                "+inf * +minsub", combined,
                "MUTANT_NON_SEMANTIC_FAILURE: fixture failed without reaching "
                "the inf*min-subnormal classification row\n" + combined[-800:])
            self.assertIn(
                "got=7fc00000", combined,
                "MUTANT_NON_SEMANTIC_FAILURE: misclassification did not take "
                "the canonical-qNaN path\n" + combined[-800:])
            # Explicit verdict marker for audit trails.
            print("MUTANT_EXECUTED_AND_SEMANTIC_TEST_FAILED (valid behavioral kill)")

        # Hygiene guard: tracked codegen.py untouched for the whole test.
        final_sha = hashlib.sha256(CODEGEN.read_text(encoding="utf-8").encode("utf-8")).hexdigest()
        self.assertEqual(final_sha, original_sha,
                         "tracked tools/codegen.py bytes changed by mutation test")

    def test_codegen_routes_every_scalar_op_through_helpers(self):
        text = CODEGEN.read_text(encoding="utf-8")
        for helper in ("sr_fpu_add_s(_a,_b,s->fcr31)",
                       "sr_fpu_sub_s(_a,_b,s->fcr31)",
                       "sr_fpu_mul_s(_a,_b,s->fcr31)",
                       "sr_fpu_div_s(_a,_b,s->fcr31)"):
            self.assertIn(helper, text, f"scalar op no longer routed through {helper}")
        self.assertIn(
            "sr_fpu_cvt_s_w(sr_u32_as_s32(s->fi[{fs}]), s->fcr31)", text,
            "cvt.s.w must honor guest RM through the pinned helper")
        self.assertNotIn(
            "(float)sr_u32_as_s32(", text,
            "bare host-cast cvt.s.w reintroduced (mutation M7 shape)")

    def test_cond_compare_writes_architectural_fcc0(self):
        text = CODEGEN.read_text(encoding="utf-8")
        self.assertIn(
            "s->fcr31 = (s->fcr31 & ~0x00800000u) | (s->fpcond << 23);", text,
            "c.cond must keep FCC0 architectural in fcr31 (mutation M5 shape)")
        self.assertNotIn(
            "(s->fpcond ^ 1u) << 23", text,
            "inverted FCC0 write reintroduced (mutation M6 shape)")
        self.assertIn(
            "s->fpcond = (s->fcr31 >> 23) & 1u;", text,
            "ctc1 must resync the cached fpcond mirror from written bits")

    def test_reference_model_shares_the_helper_contract(self):
        text = REF_INTERP.read_text(encoding="utf-8")
        self.assertIn("sr_fpu_add_s(", text)
        self.assertIn("sr_fpu_mul_s(", text)
        self.assertIn("sr_fpu_div_s(", text)
        self.assertIn("sr_fpu_cvt_s_w(", text)
        self.assertIn(
            "SHARED_HELPER", text,
            "reference model must disclose shared-helper evidence class")


class Ctc1MeasuredMaskTests(unittest.TestCase):
    """ctc1 drops FCR31 bits 25..31 (PSP-FPU-CC-001, measured on a PSP-3000).

    A guest ctc1 of 0xFE800000 reads back 0x00800000 on the console. The
    generated code, the reference interpreter and the named constant must
    agree on exactly that word, and a generator COPY restored to the old
    wholesale store must be killed by the measured generated-code cell.
    """

    EMITTED = 'f"if ({fs} == 31) {{ s->fcr31 = {R(rt(w))} & ~0xfe000000u; "'
    WHOLESALE = 'f"if ({fs} == 31) {{ s->fcr31 = {R(rt(w))}; "'

    def test_every_ctc1_model_drops_exactly_the_measured_bits(self):
        self.assertIn(self.EMITTED, CODEGEN.read_text(encoding="utf-8"))
        self.assertIn("#define SR_FCR31_CTC1_UNWRITABLE 0xfe000000u",
                      HEADER.read_text(encoding="utf-8"))
        self.assertIn("s->fcr31 = s->r[ft] & ~SR_FCR31_CTC1_UNWRITABLE;",
                      REF_INTERP.read_text(encoding="utf-8"))

    def test_behavioral_wholesale_ctc1_mutant_is_killed(self):
        """The pre-measurement emission (ctc1 stores the guest word wholesale)
        must generate, compile, run and fail CELL 5 on the cfc1 read-back.

        The mutation is applied to a TEMP COPY of codegen.py; tracked bytes are
        never touched. Generation or compilation failure is not a kill.
        """
        if CC is None:
            self.skipTest("TOOLCHAIN_MISSING: gcc required")
        original = CODEGEN.read_text(encoding="utf-8")
        original_sha = hashlib.sha256(original.encode("utf-8")).hexdigest()
        self.assertEqual(original.count(self.EMITTED), 1, "ctc1 emission anchor drifted")
        mutant = original.replace(self.EMITTED, self.WHOLESALE)
        self.assertNotEqual(mutant, original, "mutant construction produced no change")

        import tools.test_codegen_fp_convert as fixture

        with scratch_dir.TemporaryDirectory(prefix="fp_ctc1_codegen_mut_") as tmp:
            work = Path(tmp)
            tmp_codegen = work / "mutant_codegen.py"
            tmp_codegen.write_text(mutant, encoding="utf-8", newline="\n")
            elf_path = work / "m.elf"
            elf_path.write_bytes(fixture._synthetic_elf(fixture._cell5_words()))
            env = dict(os.environ)
            env["PYTHONPATH"] = str(ROOT / "tools") + os.pathsep + env.get("PYTHONPATH", "")
            gen_proc = _bounded_run(
                [sys.executable, str(tmp_codegen), str(elf_path),
                 str(work / "m.c"), "--profile=hst"],
                BUILD_TIMEOUT_S, INVALID_BUILD_TIMEOUT, cwd=ROOT, env=env)
            self.assertEqual(gen_proc.returncode, 0,
                             "MUTANT_GENERATION_FAILED\n" + gen_proc.stderr[-1500:])
            emitted = "\n".join(p.read_text(encoding="ascii")
                                for p in sorted(work.glob("m*.c")))
            self.assertNotIn("0xfe000000u", emitted, "mutant still masks ctc1")

            (work / "recomp.h").write_text(fixture.ISOLATED_RECOMP_H, encoding="ascii")
            (work / "harness.c").write_text(
                '#include "m_funcs.h"\n' + fixture.ISOLATED_STUBS_C + fixture._CELL5_MAIN,
                encoding="ascii")
            exe = work / "mutant_test.exe"
            command = [CC, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                       "-I", str(work), "-I", str(ROOT / "src" / "rt"),
                       str(work / "harness.c"), str(work / "m.c"),
                       *(str(p) for p in sorted(work.glob("m_*.c"))), "-lm", "-o", str(exe)]
            compiled = _bounded_run(command, BUILD_TIMEOUT_S, INVALID_BUILD_TIMEOUT)
            self.assertEqual(
                compiled.returncode, 0,
                "MUTANT_BUILD_FAILED: a build failure is not a semantic kill\n"
                + (compiled.stderr + compiled.stdout)[-1500:])

            ran = _bounded_run([str(exe)], RUN_TIMEOUT_S, INVALID_RUN_TIMEOUT)
            self.assertNotEqual(ran.returncode, 0, "MUTANT_SURVIVED: wholesale ctc1 passed CELL 5")
            combined = ran.stderr + ran.stdout
            self.assertIn(
                "FAIL cell5 cfc1 after ctc1 0xFE800000: fe800000", combined,
                "MUTANT_NON_SEMANTIC_FAILURE: CELL 5 failed without the measured "
                "read-back diagnostic\n" + combined[-800:])
            print("MUTANT_EXECUTED_AND_SEMANTIC_TEST_FAILED (valid behavioral kill)")

        final_sha = hashlib.sha256(CODEGEN.read_text(encoding="utf-8").encode("utf-8")).hexdigest()
        self.assertEqual(final_sha, original_sha,
                         "tracked tools/codegen.py bytes changed by mutation test")


if __name__ == "__main__":
    unittest.main()
