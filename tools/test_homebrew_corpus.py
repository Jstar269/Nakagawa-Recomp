# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The public homebrew corpus must stay pinned, licence-compatible and honest (#2144).

These tests build nothing and use no network. They check ``fixtures/homebrew_corpus/corpus.json``
against the PSPDEV lock, against PROVENANCE.md and CREDITS.md, and against the unit rules in
``tools/homebrew_corpus.py``. Where a PSPDEV is installed (``PSPDEV`` or ``/usr/local/pspdev``)
they also recompute every pinned file's Git blob ID from its samples, so a toolchain that
carries other sources than the pin fails here instead of being used silently.
"""

from __future__ import annotations

import copy
import os
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import homebrew_corpus  # noqa: E402

PROVENANCE = ROOT / "fixtures" / "homebrew_corpus" / "PROVENANCE.md"
CREDITS = ROOT / "CREDITS.md"
COMPATIBILITY = ROOT / "docs" / "COMPATIBILITY.md"
REQUIRED_ENV = "NK_HOMEBREW_CORPUS_REQUIRED"


def installed_samples_root() -> Path | None:
    for prefix in (os.environ.get("PSPDEV"), "/usr/local/pspdev"):
        if prefix:
            candidate = Path(prefix) / str(homebrew_corpus.load_corpus()["source"]["delivery"]["samples_root"])
            if candidate.is_dir():
                return candidate
    return None


class CorpusPinTests(unittest.TestCase):
    def setUp(self) -> None:
        self.corpus = homebrew_corpus.load_corpus()

    def test_checked_in_corpus_is_valid(self) -> None:
        homebrew_corpus.validate(self.corpus)
        self.assertGreaterEqual(len(self.corpus["programs"]), 3)

    def test_unknown_ids_blob_ids_and_unpinned_revisions_are_refused(self) -> None:
        def refused(mutate, message: str) -> None:
            broken = copy.deepcopy(self.corpus)
            mutate(broken)
            with self.assertRaisesRegex(homebrew_corpus.CorpusError, message):
                homebrew_corpus.validate(broken)

        refused(lambda c: c["source"].update(commit="master"), "exact 40-hex revision")
        refused(lambda c: c["source"].update(commit="0" * 40), "PSPDEV lock pins")
        refused(lambda c: c["source"].update(license_expression="GPL-2.0-only"), "compatible")
        refused(lambda c: c["source"]["delivery"].update(archive_sha256="0" * 64), "SHA-256 the PSPDEV lock")
        refused(lambda c: c["programs"][0]["files"][0].update(git_blob_sha1="abc"), "40-hex git_blob_sha1")
        refused(lambda c: c["programs"][0]["files"][0].update(path="../escape.c"), "inside the samples root")
        refused(lambda c: c["programs"].__delitem__(slice(2, None)), "at least 3 programs")
        refused(lambda c: c["programs"][1].update(id=c["programs"][0]["id"]), "malformed or repeated")
        refused(lambda c: c["programs"][0].update(smoke_checks=["nonblank_frame"]), "first_frame")
        refused(lambda c: c["programs"][0]["expected"].update(status="PASSED"), "expected.status")
        refused(lambda c: c["programs"][0]["files"].append(dict(c["programs"][0]["files"][0])), "listed twice")
        refused(lambda c: c["programs"][0]["files"].__delitem__(0), "Makefile is not pinned")
        refused(lambda c: c["programs"][0].pop("sample_dir"), "sample_dir is required")
        refused(lambda c: c["programs"][0].update(smoke_checks=["first_frame", "bogus"]), "only the vocabulary")
        refused(lambda c: c["programs"][0]["files"][0].update(upstream_path="elsewhere/x.c"), "under src/samples/")

    def test_a_result_without_backing_evidence_is_refused(self) -> None:
        def refused(**expected: object) -> None:
            broken = copy.deepcopy(self.corpus)
            broken["programs"][0]["expected"].update(expected)
            with self.assertRaises(homebrew_corpus.CorpusError):
                homebrew_corpus.validate(broken)

        refused(status="PASS")
        refused(status="PASS", evidence="")
        refused(status="PASS", evidence="reports/smoke/gu-cube.log")
        refused(evidence="reports/smoke/gu-cube.log")
        for issue in (True, "308", 0, -1, 3.5):
            refused(status="KNOWN_FAIL", issue=issue)

    def test_known_fail_must_name_its_issue(self) -> None:
        broken = copy.deepcopy(self.corpus)
        broken["programs"][0]["expected"].update(status="KNOWN_FAIL", issue=None)
        with self.assertRaisesRegex(homebrew_corpus.CorpusError, "KNOWN_FAIL needs"):
            homebrew_corpus.validate(broken)
        broken["programs"][0]["expected"]["issue"] = 308
        homebrew_corpus.validate(broken)

    def test_git_blob_id_matches_git(self) -> None:
        self.assertEqual(homebrew_corpus.git_blob_sha1(b"hello world"),
                         "95d09f2b10159347eece71399a7e2e907ea3df4f")
        self.assertEqual(homebrew_corpus.git_blob_sha1(b""),
                         "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391")

    def test_installed_pspdev_carries_exactly_the_pinned_sources(self) -> None:
        root = installed_samples_root()
        if root is None:
            if os.environ.get(REQUIRED_ENV) == "1":
                self.fail(f"{REQUIRED_ENV}=1, but no PSPDEV samples were found")
            self.skipTest("NOT_RUN: PSPDEV samples are not installed on this host")
        self.assertEqual(homebrew_corpus.verify_sources(self.corpus, root), [])

    def test_verify_sources_reports_a_changed_and_a_missing_file(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory(prefix="nk-corpus-") as tmp:
            root = Path(tmp)
            first = self.corpus["programs"][0]["files"][0]
            target = root / first["path"]
            target.parent.mkdir(parents=True)
            target.write_bytes(b"not the pinned bytes")
            differences = homebrew_corpus.verify_sources(self.corpus, root)
        self.assertTrue(any(first["path"] in line and "pinned" in line for line in differences))
        self.assertTrue(any("missing" in line for line in differences))


class CorpusRecordsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.corpus = homebrew_corpus.load_corpus()
        self.provenance = PROVENANCE.read_text(encoding="utf-8")

    def test_provenance_lists_the_pin_and_every_program_file(self) -> None:
        source = self.corpus["source"]
        for pin in (source["commit"], source["delivery"]["archive_sha256"], source["delivery"]["release"],
                    source["license_blob_sha1"], source["license_expression"]):
            self.assertIn(pin, self.provenance)
        for program in self.corpus["programs"]:
            for entry in program["files"] + program["shared_files"]:
                self.assertIn(entry["git_blob_sha1"], self.provenance, entry["path"])
                self.assertIn(entry["upstream_path"], self.provenance, entry["path"])

    def test_credits_name_pspsdk_samples_and_the_corpus_page_lists_every_program(self) -> None:
        credits = CREDITS.read_text(encoding="utf-8")
        self.assertIn("fixtures/homebrew_corpus/PROVENANCE.md", credits)
        page = COMPATIBILITY.read_text(encoding="utf-8")
        self.assertIn("## Homebrew corpus", page)
        for program in self.corpus["programs"]:
            row = next((line for line in page.splitlines() if line.startswith(f"| `{program['id']}` |")), None)
            self.assertIsNotNone(row, program["id"])
            assert row is not None
            self.assertTrue(row.rstrip(" |").endswith(f"| {program['expected']['status'].lower()}"), row)

    def test_every_copyright_holder_is_credited(self) -> None:
        for program in self.corpus["programs"]:
            for holder in program["copyright"]:
                self.assertIn(holder, self.provenance, program["id"])


if __name__ == "__main__":
    unittest.main()
