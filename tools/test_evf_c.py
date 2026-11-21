# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Regression coverage for PSP EventFlag semantics (issue #4).

Two layers:

1. Compile and run ``tests/native/evf_selftest.c`` against the pure helpers in
   ``src/rt/evf.h`` — keep-mask clear, AND/OR matching, WAITCLEAR/WAITCLEARALL
   consumption, and PSP-style wait/poll argument validation errors.
2. Source-level wiring checks that the EventFlag handlers in
   ``src/rt/hle_kernel_sync.c`` (found through ``hle_sources``) route through
   those helpers instead of re-deriving the semantics inline. These are source
   checks, not a production syscall proof.
"""

import os
from pathlib import Path
import re
import shutil
import subprocess
import unittest
from unittest import mock
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hle_sources  # noqa: E402
import scratch_dir  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SELFTEST_C = ROOT / "tests" / "native" / "evf_selftest.c"
HLE_SOURCE = hle_sources.text()
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")


def function_body(name):
    match = re.search(rf"\b{name}\s*\([^)]*\)\s*\{{", HLE_SOURCE)
    if not match:
        raise AssertionError(f"{name} not found in src/rt/hle.c")

    start = match.end() - 1
    depth = 0
    for pos in range(start, len(HLE_SOURCE)):
        char = HLE_SOURCE[pos]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return HLE_SOURCE[start : pos + 1]
    raise AssertionError(f"{name} has no closing brace")


def poll_outbits_order_problem(body):
    """Why a PollEventFlag body would report a post-consume outBits, or "" when it cannot.

    On success the PSP writes outBits with the pattern before WAITCLEAR/WAITCLEARALL consumes
    it, so the success-path write must come before the consume and none may follow it.
    """
    consume = body.find("sr_evf_consume(")
    if consume < 0:
        return "the success path does not consume through sr_evf_consume"
    writes = [match.start() for match in re.finditer(r"MEM_W32\(outp, m->pattern\)", body)]
    if not any(start < consume for start in writes):
        return "no outBits write precedes the consume"
    if any(start > consume for start in writes):
        return "an outBits write comes after the consume, so it reports the post-consume pattern"
    return ""


@unittest.skipUnless(CC, "TOOLCHAIN_MISSING: no C compiler on PATH")
class TestEvfSelftestC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert CC is not None
        # A class cleanup runs even when setUpClass raises, as a failed compile does below;
        # tearDownClass would not, and the scratch directory would be left behind.
        scratch = scratch_dir.TemporaryDirectory(prefix="evfc_")
        cls.addClassCleanup(scratch.cleanup)
        cls.tmp = scratch.name
        cls.exe = os.path.join(cls.tmp, "evf_selftest.exe")
        result = subprocess.run(
            [
                CC,
                "-std=c11",
                "-O0",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'src' / 'rt'}",
                "-o",
                cls.exe,
                str(SELFTEST_C),
            ],
            capture_output=True,
            text=True,
            timeout=120,
        )
        if result.returncode != 0:
            raise AssertionError("evf_selftest.c did not compile:\n" + result.stderr)

    def test_pure_helper_invariants_hold(self):
        result = subprocess.run([self.exe], capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("evf selftest: OK", result.stdout)


@unittest.skipUnless(CC, "TOOLCHAIN_MISSING: no C compiler on PATH")
class TestEvfSelftestScratch(unittest.TestCase):
    def test_a_failed_compile_leaves_no_scratch_directory_and_is_bounded(self):
        failed = subprocess.CompletedProcess([], 1, "", "simulated compile failure")
        result = unittest.TestResult()
        with mock.patch.object(subprocess, "run", return_value=failed) as runner:
            unittest.defaultTestLoader.loadTestsFromTestCase(TestEvfSelftestC).run(result)
        self.assertTrue(result.errors, "the failed compile must fail the class setup")
        self.assertFalse(os.path.exists(TestEvfSelftestC.tmp),
                         "setUpClass left its scratch directory after the compile failed")
        self.assertEqual(runner.call_count, 1)
        self.assertIn("timeout", runner.call_args.kwargs, "a hung compiler must not stall the suite")


class TestHleEventFlagWiring(unittest.TestCase):
    def test_clear_uses_keep_mask_helper(self):
        body = function_body("h_ClearEventFlag")
        self.assertIn("sr_evf_clear_pattern(m->pattern, A1)", body)
        self.assertNotIn("~A1", body, "clear must keep A1 bits, not remove them")

    def test_wait_validates_args_and_consumes_via_helpers(self):
        body = function_body("h_WaitEventFlag")
        self.assertIn("sr_evf_check_wait_args(bits, mode)", body)
        self.assertIn("sr_evf_matches(m->pattern, bits, mode)", body)
        self.assertIn("sr_evf_consume(m->pattern, bits, mode)", body)
        self.assertNotRegex(body, r"mode\s*&\s*0x[12]0", "no inline clear-bit handling")

    def test_poll_validates_consumes_and_returns_evf_cond(self):
        body = function_body("h_PollEventFlag")
        self.assertIn("sr_evf_check_poll_args(bits, mode)", body)
        self.assertIn("sr_evf_matches(m->pattern, bits, mode)", body)
        self.assertIn("sr_evf_consume(m->pattern, bits, mode)", body)
        self.assertIn("SR_EVF_ERR_COND", body)
        self.assertNotIn("0x80020021", body, "poll condition failure must be EVF_COND")

    def test_poll_writes_outbits_on_both_outcomes(self):
        body = function_body("h_PollEventFlag")
        self.assertEqual(
            len(re.findall(r"MEM_W32\(outp, m->pattern\)", body)),
            2,
            "poll must report the current pattern on failure and the "
            "pre-consume pattern on success",
        )
        self.assertEqual(poll_outbits_order_problem(body), "")

    def test_the_order_check_catches_outbits_written_after_the_consume(self):
        swapped = (
            "{\n"
            "    if (!sr_evf_matches(m->pattern, bits, mode)) {\n"
            "        if (outp) MEM_W32(outp, m->pattern);\n"
            "        return SR_EVF_ERR_COND;\n"
            "    }\n"
            "    m->pattern = sr_evf_consume(m->pattern, bits, mode);\n"
            "    if (outp) MEM_W32(outp, m->pattern);\n"
            "    return 0;\n"
            "}\n"
        )
        self.assertIn("after", poll_outbits_order_problem(swapped))


if __name__ == "__main__":
    unittest.main()
