# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
# Synthetic test fixture — tools/test_ppmdiff_coverage.py

"""
Fail-closed coverage tests for tools/ppmdiff.py.

Every test creates a temporary directory tree with synthetic 2×1 PPM frames
(6 pixel bytes) and asserts the expected pass/fail/exception behaviour.

Run: python -m unittest tools.test_ppmdiff_coverage -v
"""
import contextlib
import io
import json
import os
import struct
import subprocess
import tempfile
import unittest

# Allow running from the repository root or via `python -m unittest discover`
import importlib
import sys

_TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
if _TOOLS_DIR not in sys.path:
    sys.path.insert(0, _TOOLS_DIR)

ppmdiff = importlib.import_module("ppmdiff")

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_ppm(w, h, pixels):
    """Return raw P6 bytes for a *w*×*h* image with *pixels* as RGB tuples."""
    header = f"P6\n{w} {h}\n255\n".encode()
    body = b"".join(struct.pack("3B", r, g, b) for r, g, b in pixels)
    return header + body


def _write(directory, name, data):
    path = os.path.join(directory, name)
    with open(path, "wb") as f:
        f.write(data)
    return path


# Canonical 2×1 red frame: two red pixels
RED_2x1 = _make_ppm(2, 1, [(255, 0, 0), (255, 0, 0)])

# Canonical 2×1 green frame: two green pixels (big delta from red)
GREEN_2x1 = _make_ppm(2, 1, [(0, 255, 0), (0, 255, 0)])

# Same as RED_2x1 — used for exact-match assertions
RED_2x1_COPY = RED_2x1


class TestPPMDiffCoverage(unittest.TestCase):
    """Fail-closed coverage contract for ppmdiff.run_diff."""

    # ---- 1. Exact match PASS ----
    def test_exact_match_pass(self):
        with tempfile.TemporaryDirectory() as td:
            a, b = os.path.join(td, "a"), os.path.join(td, "b")
            os.makedirs(a)
            os.makedirs(b)
            _write(a, "frame0000.ppm", RED_2x1)
            _write(b, "frame0000.ppm", RED_2x1_COPY)

            report, ok = ppmdiff.run_diff(a, b, threshold=3)
            self.assertTrue(ok)
            self.assertEqual(report["summary"]["total_frames"], 1)
            self.assertEqual(report["summary"]["failed_frames"], 0)
            self.assertEqual(report["frames"][0]["status"], "pass")

    # ---- 2. Pixel mismatch FAIL ----
    def test_pixel_mismatch_fail(self):
        with tempfile.TemporaryDirectory() as td:
            a, b = os.path.join(td, "a"), os.path.join(td, "b")
            os.makedirs(a)
            os.makedirs(b)
            _write(a, "frame0000.ppm", RED_2x1)
            _write(b, "frame0000.ppm", GREEN_2x1)

            report, ok = ppmdiff.run_diff(a, b, threshold=3)
            self.assertFalse(ok)
            self.assertGreater(report["summary"]["failed_frames"], 0)
            self.assertEqual(report["frames"][0]["status"], "fail")

    # ---- 3. Missing directory FAIL ----
    def test_missing_dir_fail(self):
        with tempfile.TemporaryDirectory() as td:
            a = os.path.join(td, "does_not_exist")
            b = os.path.join(td, "also_missing")
            with self.assertRaises(ppmdiff.CoverageError):
                ppmdiff.run_diff(a, b, threshold=3)

    # ---- 4. Empty directories FAIL ----
    def test_empty_dirs_fail(self):
        with tempfile.TemporaryDirectory() as td:
            a, b = os.path.join(td, "a"), os.path.join(td, "b")
            os.makedirs(a)
            os.makedirs(b)
            with self.assertRaises(ppmdiff.CoverageError):
                ppmdiff.run_diff(a, b, threshold=3)

    # ---- 5. Disjoint sets FAIL ----
    def test_disjoint_sets_fail(self):
        with tempfile.TemporaryDirectory() as td:
            a, b = os.path.join(td, "a"), os.path.join(td, "b")
            os.makedirs(a)
            os.makedirs(b)
            _write(a, "frame0000.ppm", RED_2x1)
            _write(b, "frame0001.ppm", RED_2x1)

            with self.assertRaises(ppmdiff.CoverageError):
                ppmdiff.run_diff(a, b, threshold=3)

    # ---- 6. A-only / B-only FAIL ----
    def test_a_only_b_only_fail(self):
        with tempfile.TemporaryDirectory() as td:
            a, b = os.path.join(td, "a"), os.path.join(td, "b")
            os.makedirs(a)
            os.makedirs(b)
            _write(a, "frame0000.ppm", RED_2x1)
            _write(a, "frame0001.ppm", RED_2x1)
            _write(b, "frame0000.ppm", RED_2x1)
            _write(b, "frame0002.ppm", RED_2x1)

            report, ok = ppmdiff.run_diff(a, b, threshold=3)
            self.assertFalse(ok)
            # frame0001 is A-only, frame0002 is B-only
            statuses = {f["filename"]: f["status"] for f in report["frames"]}
            self.assertEqual(statuses.get("frame0001.ppm"), "fail")
            self.assertEqual(statuses.get("frame0002.ppm"), "fail")

    # ---- 7. Truncated PPM FAIL ----
    def test_truncated_ppm_fail(self):
        with tempfile.TemporaryDirectory() as td:
            a, b = os.path.join(td, "a"), os.path.join(td, "b")
            os.makedirs(a)
            os.makedirs(b)
            _write(a, "frame0000.ppm", RED_2x1)
            # Write a PPM with correct header but truncated payload
            truncated = b"P6\n2 1\n255\n\xff"  # needs 6 bytes, only has 1
            _write(b, "frame0000.ppm", truncated)

            with self.assertRaises(ppmdiff.CoverageError):
                ppmdiff.run_diff(a, b, threshold=3)

    # ---- 8. Dimension mismatch FAIL ----
    def test_dimension_mismatch_fail(self):
        with tempfile.TemporaryDirectory() as td:
            a, b = os.path.join(td, "a"), os.path.join(td, "b")
            os.makedirs(a)
            os.makedirs(b)
            _write(a, "frame0000.ppm", RED_2x1)
            # 1×2 instead of 2×1
            frame_1x2 = _make_ppm(1, 2, [(255, 0, 0), (255, 0, 0)])
            _write(b, "frame0000.ppm", frame_1x2)

            with self.assertRaises(ppmdiff.CoverageError):
                ppmdiff.run_diff(a, b, threshold=3)



    # ---- 9. Header comment non-match ----
    def test_ppm_comment_in_header_diff(self):
        with tempfile.TemporaryDirectory() as td:
            a, b = os.path.join(td, "a"), os.path.join(td, "b")
            os.makedirs(a)
            os.makedirs(b)
            _write(a, "frame0000.ppm", RED_2x1)
            # Image B with unusual header comment and different pixel data (green)
            green_comment = b"P6\n# unusual header comment\n2 1\n255\n\x00\xff\x00\x00\xff\x00"
            _write(b, "frame0000.ppm", green_comment)

            report, ok = ppmdiff.run_diff(a, b, threshold=3)
            self.assertFalse(ok)
            self.assertEqual(report["summary"]["failed_frames"], 1)
            self.assertEqual(report["frames"][0]["status"], "fail")

    # ---- 10. Header comment match ----
    def test_ppm_comment_in_header_pass(self):
        with tempfile.TemporaryDirectory() as td:
            a, b = os.path.join(td, "a"), os.path.join(td, "b")
            os.makedirs(a)
            os.makedirs(b)
            _write(a, "frame0000.ppm", RED_2x1)
            red_comment = b"P6\n# header comment\n2 1\n# another comment\n255\n\xff\x00\x00\xff\x00\x00"
            _write(b, "frame0000.ppm", red_comment)

            report, ok = ppmdiff.run_diff(a, b, threshold=3)
            self.assertTrue(ok)
            self.assertEqual(report["summary"]["passed_frames"], 1)
            self.assertEqual(report["frames"][0]["status"], "pass")

    # ---- 11. Extra payload bytes FAIL ----
    def test_extra_payload_bytes_fail(self):
        with tempfile.TemporaryDirectory() as td:
            a, b = os.path.join(td, "a"), os.path.join(td, "b")
            os.makedirs(a)
            os.makedirs(b)
            _write(a, "frame0000.ppm", RED_2x1)
            # 12 bytes payload for a 2x1 image (expected 6)
            extra_payload = b"P6\n2 1\n255\n\xff\x00\x00\xff\x00\x00\x00\x00\x00\x00\x00\x00"
            _write(b, "frame0000.ppm", extra_payload)

            with self.assertRaises(ppmdiff.CoverageError):
                ppmdiff.run_diff(a, b, threshold=3)

    # ---- 12. Unsupported maxval FAIL ----
    def test_unsupported_maxval_fail(self):
        with tempfile.TemporaryDirectory() as td:
            a, b = os.path.join(td, "a"), os.path.join(td, "b")
            os.makedirs(a)
            os.makedirs(b)
            _write(a, "frame0000.ppm", RED_2x1)
            bad_maxval = b"P6\n2 1\n256\n\xff\x00\x00\xff\x00\x00"
            _write(b, "frame0000.ppm", bad_maxval)

            with self.assertRaises(ppmdiff.CoverageError):
                ppmdiff.run_diff(a, b, threshold=3)


class TestPPMDiffModes(unittest.TestCase):
    """One-shot and watch mode: how the tool runs and what report it leaves."""

    def test_an_inherited_sr_fbsnap_keeps_the_one_shot_run(self):
        # SR_FBSNAP=<N> is the runtime's capture setting; a shell that exported it for a capture
        # must still get the one-shot exit code, not an endless watch loop.
        with tempfile.TemporaryDirectory() as td:
            a, b = os.path.join(td, "a"), os.path.join(td, "b")
            os.makedirs(a)
            os.makedirs(b)
            _write(a, "frame0000.ppm", RED_2x1)
            _write(b, "frame0000.ppm", RED_2x1_COPY)
            env = dict(os.environ, SR_FBSNAP="1")
            result = subprocess.run(
                [sys.executable, os.path.join(_TOOLS_DIR, "ppmdiff.py"), a, b],
                cwd=td, env=env, capture_output=True, text=True, timeout=60,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn("Watching", result.stdout)
            self.assertTrue(os.path.isfile(os.path.join(td, "visual_regression_report.json")))

    def test_watch_mode_removes_the_report_when_a_later_comparison_fails_coverage(self):
        with tempfile.TemporaryDirectory() as td:
            a, b = os.path.join(td, "a"), os.path.join(td, "b")
            os.makedirs(a)
            os.makedirs(b)
            _write(a, "frame0000.ppm", RED_2x1)
            _write(b, "frame0000.ppm", RED_2x1_COPY)
            report_path = os.path.join(td, "visual_regression_report.json")
            self.assertTrue(ppmdiff.regenerate_watch_report(a, b, 3, report_path))
            with open(report_path, encoding="utf-8") as f:
                self.assertTrue(json.load(f)["summary"]["coverage_ok"])
            _write(b, "frame0000.ppm", RED_2x1[:-2])  # a truncated capture
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertFalse(ppmdiff.regenerate_watch_report(a, b, 3, report_path))
            self.assertFalse(os.path.exists(report_path))
            self.assertIn("Coverage problem: truncated payload", err.getvalue())


if __name__ == "__main__":
    unittest.main()
