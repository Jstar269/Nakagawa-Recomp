# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Unit tests for the private<->public drift classifier."""

from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import sync_drift_check as drift


EXCLUDED = frozenset({"src/rt/pgd.c", "src/rt/pgf.c"})
GLOBS = ["font/*.pgf"]


def _write(root: Path, rel: str, text: str) -> None:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


class ClassifyTests(unittest.TestCase):
    def _classify(self, export: dict[str, str], public: dict[str, str]) -> dict[str, str]:
        findings = drift.classify(export, public, EXCLUDED, GLOBS)
        return {f["path"]: f["category"] for f in findings}

    def test_identical_paths_are_not_reported(self):
        """Matching content yields no finding for that path (exclusion rows are separate)."""
        result = self._classify({"Makefile": "aa"}, {"Makefile": "aa"})
        self.assertNotIn("Makefile", result)
        self.assertEqual({c for p, c in result.items() if p not in EXCLUDED}, set())

    def test_differing_generic_file_is_drift(self):
        result = self._classify({"Makefile": "aa"}, {"Makefile": "bb"})
        self.assertEqual(result["Makefile"], "GENERIC_DRIFT")

    def test_public_only_allowlisted_doc_is_expected(self):
        path = "docs/PUBLICATION_READINESS.md"
        self.assertIn(path, drift.PUBLIC_ONLY_PATHS)
        result = self._classify({path: "aa"}, {path: "bb"})
        self.assertEqual(result[path], "EXPECTED_PUBLIC_ONLY")

    def test_file_on_public_but_missing_from_export_is_drift(self):
        """Exporting today would delete a file public main has — a regression."""
        result = self._classify({}, {"tools/new_public_fix.py": "bb"})
        self.assertEqual(result["tools/new_public_fix.py"], "GENERIC_DRIFT")

    def test_unclassified_export_only_path_is_unknown(self):
        result = self._classify({"src/rt/surprise.c": "aa"}, {})
        self.assertEqual(result["src/rt/surprise.c"], "UNKNOWN")

    def test_excluded_path_absent_from_both_is_positively_confirmed(self):
        """Absence must be asserted, not merely unobserved, or the leak check is vacuous."""
        findings = drift.classify({}, {}, EXCLUDED, GLOBS)
        categories = {f["path"]: f["category"] for f in findings}
        self.assertEqual(set(categories), EXCLUDED)
        for path in EXCLUDED:
            self.assertEqual(categories[path], "EXPECTED_PRIVATE_EXCLUSION")

    def test_excluded_absence_alone_does_not_fail(self):
        findings = drift.classify({}, {}, EXCLUDED, GLOBS)
        _, failed = drift.render(findings)
        self.assertFalse(failed)

    def test_excluded_path_present_in_export_is_unknown_not_silent(self):
        """The leak case: excluded private material must never pass quietly."""
        result = self._classify({"src/rt/pgd.c": "aa"}, {})
        self.assertEqual(result["src/rt/pgd.c"], "UNKNOWN")

    def test_excluded_glob_present_in_public_is_unknown(self):
        result = self._classify({}, {"font/ltn0.pgf": "aa"})
        self.assertEqual(result["font/ltn0.pgf"], "UNKNOWN")


class LineEndingTests(unittest.TestCase):
    """The exporter writes host line endings; a git clone applies eol=lf."""

    def test_crlf_and_lf_text_hash_identically(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "crlf.txt").write_bytes(b"alpha\r\nbeta\r\n")
            (root / "lf.txt").write_bytes(b"alpha\nbeta\n")
            self.assertEqual(drift.sha256_file(root / "crlf.txt"), drift.sha256_file(root / "lf.txt"))

    def test_binary_files_are_hashed_byte_exactly(self):
        """A pinned LUT differing only by those bytes must never be equated."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "a.dat").write_bytes(b"\x00\r\n\x01")
            (root / "b.dat").write_bytes(b"\x00\n\x01")
            self.assertNotEqual(drift.sha256_file(root / "a.dat"), drift.sha256_file(root / "b.dat"))

    def test_real_text_difference_still_detected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "a.txt").write_bytes(b"alpha\n")
            (root / "b.txt").write_bytes(b"beta\n")
            self.assertNotEqual(drift.sha256_file(root / "a.txt"), drift.sha256_file(root / "b.txt"))


class RenderTests(unittest.TestCase):
    def test_unknown_findings_fail_closed(self):
        _, failed = drift.render([{"path": "x", "category": "UNKNOWN", "detail": "d"}])
        self.assertTrue(failed)

    def test_generic_drift_fails(self):
        _, failed = drift.render([{"path": "x", "category": "GENERIC_DRIFT", "detail": "d"}])
        self.assertTrue(failed)

    def test_expected_categories_pass(self):
        findings = [
            {"path": "a", "category": "EXPECTED_PUBLIC_ONLY", "detail": "d"},
            {"path": "b", "category": "EXPECTED_PRIVATE_EXCLUSION", "detail": "d"},
        ]
        text, failed = drift.render(findings)
        self.assertFalse(failed)
        self.assertIn("RESULT: OK", text)



class ExcludedPathLoadingTests(unittest.TestCase):
    def test_export_manifest_is_preferred(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write(root, drift.EXPORT_MANIFEST,
                   json.dumps({"excluded_paths": ["src/rt/pgd.c"], "excluded_globs": []}))
            exact, globs = drift.load_excluded_paths(root)
            self.assertIn("src/rt/pgd.c", exact)
            self.assertEqual(globs, [])

    def test_export_manifest_globs_are_applied(self):
        """PUBLIC_EXPORT.json records excluded_globs beside excluded_paths; both apply."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write(root, drift.EXPORT_MANIFEST, json.dumps(
                {"excluded_paths": ["src/rt/pgd.c"], "excluded_globs": ["font/*.pgf"]}))
            exact, globs = drift.load_excluded_paths(root)
            self.assertIn("src/rt/pgd.c", exact)
            self.assertTrue(drift.is_excluded("font/x.pgf", exact, globs))

    def test_older_manifest_without_globs_takes_the_profile_globs_and_prefixes(self):
        with tempfile.TemporaryDirectory() as tmp:
            export, source = Path(tmp) / "export", Path(tmp) / "source"
            _write(export, drift.EXPORT_MANIFEST, json.dumps({"excluded_paths": ["src/rt/pgd.c"]}))
            _write(source, drift.PROFILE_PATH, json.dumps(
                {"exclude_paths": [], "exclude_globs": ["font/*.pgf"], "exclude_prefixes": ["private[1]/"]}))
            exact, globs = drift.load_excluded_paths(export, source_root=source)
            self.assertTrue(drift.is_excluded("font/x.pgf", exact, globs))
            self.assertTrue(drift.is_excluded("private[1]/notes.txt", exact, globs))
            self.assertFalse(drift.is_excluded("private1/notes.txt", exact, globs))

    def test_globs_match_the_whole_path_as_the_publication_policy_does(self):
        """Policy.resolve uses fnmatchcase over the whole path, not right-anchored matching."""
        self.assertTrue(drift.is_excluded("font/ltn0.pgf", frozenset(), GLOBS))
        self.assertFalse(drift.is_excluded("tools/font/ltn0.pgf", frozenset(), GLOBS))

    def test_missing_manifest_and_profile_raises(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises((FileNotFoundError, OSError)):
                drift.load_excluded_paths(Path(tmp), source_root=Path(tmp))

    def test_real_profile_lists_the_pgd_and_pgf_backends(self):
        exact, globs = drift.load_excluded_paths(Path(tempfile.gettempdir()) / "nonexistent-export", drift.ROOT)
        self.assertIn("src/rt/pgd.c", exact)
        self.assertIn("src/rt/pgf.c", exact)
        self.assertIn("font/*.pgf", globs)


class EndToEndTests(unittest.TestCase):
    """main() over real trees: the leak cases must fail the run, never print RESULT: OK."""

    def _run(self, export: dict[str, str], public: dict[str, str]) -> tuple[int, dict[str, str]]:
        with tempfile.TemporaryDirectory() as tmp:
            export_root, public_root = Path(tmp) / "export", Path(tmp) / "public"
            manifest = json.dumps({"excluded_paths": sorted(EXCLUDED), "excluded_globs": GLOBS})
            for root, files in ((export_root, export), (public_root, public)):
                _write(root, drift.EXPORT_MANIFEST, manifest)
                for rel, text in files.items():
                    _write(root, rel, text)
            out = io.StringIO()
            with redirect_stdout(out):
                code = drift.main(["--export-dir", str(export_root), "--public-dir", str(public_root), "--json"])
        report = json.loads(out.getvalue())
        return code, {f["path"]: f["category"] for f in report["findings"]}

    def test_glob_excluded_font_identical_on_both_sides_is_a_leak(self):
        code, result = self._run({"font/x.pgf": "pgf"}, {"font/x.pgf": "pgf"})
        self.assertEqual(result.get("font/x.pgf"), "UNKNOWN")
        self.assertEqual(code, 1)

    def test_build_output_in_the_export_is_a_leak(self):
        """A local build/ in the public checkout is ignored; one in the export is reported."""
        leaked = "build/demo/demo_recomp_0.c"
        code, result = self._run({leaked: "int x;"}, {leaked: "int x;"})
        self.assertEqual(result.get(leaked), "UNKNOWN")
        self.assertEqual(code, 1)

    def test_local_build_in_the_public_checkout_alone_is_clean(self):
        code, result = self._run({"Makefile": "m"}, {"Makefile": "m", "build/demo/demo.exe": "bin"})
        self.assertNotIn("build/demo/demo.exe", result)
        self.assertEqual(code, 0)


if __name__ == "__main__":
    unittest.main()
