# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""A fork maintainer can become their fork's provenance authority (``tools/fork_init.py``)."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
from typing import Any
import tempfile
import unittest
from unittest import mock

try:
    import admit_paths
    import fork_init
    import provenance_ledger
except ModuleNotFoundError:  # Running as ``python -m unittest tools.test_fork_init``.
    from tools import admit_paths, fork_init, provenance_ledger

ROOT = Path(__file__).resolve().parents[1]

IDENTITY = {
    "GIT_AUTHOR_NAME": "Fork Maintainer",
    "GIT_AUTHOR_EMAIL": "fork@example.invalid",
    "GIT_COMMITTER_NAME": "Fork Maintainer",
    "GIT_COMMITTER_EMAIL": "fork@example.invalid",
}


def _entry(path: str, classification: str, record_id: str | None = None, **evidence: object) -> dict:
    body: dict = {"source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json"} if record_id else {"source": "x"}
    if record_id:
        body |= {"record_id": record_id, "evidence_tier": "S"}
    return {"path": path, "classification": classification, "evidence": body | evidence}


class SeedRecordsTest(unittest.TestCase):
    def test_one_record_per_id_adopting_the_published_claim(self) -> None:
        ledger = {"entries": [
            _entry("README.md", "reviewed_documentation"),
            _entry("src/a.c", "project_authored_attested", "rec-a"),
            _entry("src/b.c", "project_authored_attested", "rec-a"),
            _entry("src/c.c", "upstream_derived", "rec-c", license="GPL-2.0-or-later"),
            _entry("src/d.c", "upstream_derived", "rec-d", license="see NOTICE.md"),
            _entry("assets/e.bin", "generated_from_public_source", "rec-e"),
        ]}
        records = {record["id"]: record for record in fork_init.seed_records(ledger, "c" * 40)}
        self.assertEqual(sorted(records), ["rec-a", "rec-c", "rec-d", "rec-e"])
        self.assertEqual(records["rec-a"]["paths"], ["src/a.c", "src/b.c"])
        self.assertEqual(records["rec-a"]["classification"], "project-authored-independent")
        self.assertEqual(records["rec-c"]["classification"], "derived-translated")
        self.assertEqual(records["rec-c"]["upstream_license"], "GPL-2.0-or-later")
        self.assertNotIn("upstream_license", records["rec-d"])
        self.assertEqual(records["rec-e"]["classification"], "derived-data")
        for record in records.values():
            self.assertEqual(record["seeded_from"]["commit"], "c" * 40)
            self.assertIn("Not a new attestation", record["seeded_from"]["statement"])

    def test_each_seeded_record_maps_back_to_its_public_claim(self) -> None:
        ledger = {"entries": [
            _entry("src/a.c", "project_authored_attested", "rec-a"),
            _entry("src/c.c", "upstream_derived", "rec-c", license="MIT"),
            _entry("assets/e.bin", "generated_from_public_source", "rec-e"),
        ]}
        for record in fork_init.seed_records(ledger, "c" * 40):
            for path in record["paths"]:
                published = next(entry for entry in ledger["entries"] if entry["path"] == path)
                classification, evidence = provenance_ledger._refresh_class_for(path, record)
                self.assertEqual(classification, published["classification"])
                self.assertEqual(evidence["record_id"], record["id"])

    def test_one_id_with_two_claims_is_refused(self) -> None:
        ledger = {"entries": [
            _entry("src/a.c", "project_authored_attested", "rec-a"),
            _entry("src/b.c", "upstream_derived", "rec-a", license="MIT"),
        ]}
        with self.assertRaises(fork_init.ForkInitError) as caught:
            fork_init.seed_records(ledger, "c" * 40)
        self.assertEqual(caught.exception.code, "PUBLIC_LEDGER_INVALID")

    def test_an_unknown_record_backed_class_is_refused(self) -> None:
        with self.assertRaises(fork_init.ForkInitError):
            fork_init.seed_records({"entries": [_entry("x.c", "unresolved", "rec-x")]}, "c" * 40)


class AuthorityTest(unittest.TestCase):
    """Seed an authority from this checkout's own public ledger, outside the checkout."""

    scratch: tempfile.TemporaryDirectory[str]
    authority: Path
    result: dict[str, Any]

    @classmethod
    def setUpClass(cls) -> None:
        cls.scratch = tempfile.TemporaryDirectory(prefix="nk-fork-authority-")
        cls.authority = Path(cls.scratch.name) / "authority"
        with mock.patch.dict(os.environ, IDENTITY):
            cls.result = fork_init.init(ROOT, cls.authority)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.scratch.cleanup()

    def test_seed_reproduces_the_committed_public_ledger(self) -> None:
        self.assertGreater(self.result["records"], 0)
        head = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"], capture_output=True, text=True,
                              check=True).stdout.strip()
        self.assertEqual(self.result["commit"], head)
        # verify() compares the trusted verifier's regeneration with the committed bytes.
        self.assertEqual(fork_init.verify(ROOT, self.authority), head)

    def test_the_seed_is_committed_on_main(self) -> None:
        def git(*args: str) -> str:
            return str(subprocess.run(["git", "-C", str(self.authority), *args], capture_output=True, text=True,
                                      check=True).stdout).strip()
        self.assertEqual(git("rev-parse", "--abbrev-ref", "HEAD"), "main")
        self.assertEqual(git("status", "--porcelain"), "")
        self.assertEqual(git("ls-files").splitlines(), ["README.md", fork_init.AUTHORITY_LEDGER])

    def test_a_record_missing_from_the_authority_fails_verification(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-fork-tamper-") as scratch:
            copy = Path(scratch) / fork_init.AUTHORITY_LEDGER
            copy.parent.mkdir(parents=True)
            document = json.loads((self.authority / fork_init.AUTHORITY_LEDGER).read_text(encoding="utf-8"))
            document["records"] = document["records"][1:]
            copy.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaises(fork_init.ForkInitError) as caught:
                fork_init.verify(ROOT, Path(scratch))
            self.assertIn(caught.exception.code, {"AUTHORITY_MISMATCH", "TRUSTED_PATH_UNQUALIFIED",
                                                  "BASELINE_UNRESOLVED"})

    def test_a_new_record_is_the_one_admit_paths_needs(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-fork-record-") as scratch:
            authority = Path(scratch) / "authority"
            subprocess.run(["git", "clone", "--quiet", str(self.authority), str(authority)], check=True)
            path = "tools/fork_example_tool.py"
            with self.assertRaises(admit_paths.AdmitError) as caught:
                admit_paths.classify([path], {}, {})
            self.assertEqual(caught.exception.code, "PROVENANCE_UNRESOLVED")
            with mock.patch.dict(os.environ, IDENTITY):
                fork_init.add_record(
                    authority, record_id="fork-example-tool-2026-11-08", paths=[path],
                    classification="project-authored-independent", evidence_tier="S",
                    statement="I wrote tools/fork_example_tool.py for this fork.",
                )
            records = provenance_ledger._detailed_records(
                json.loads((authority / fork_init.AUTHORITY_LEDGER).read_text(encoding="utf-8")))
            origins = {path: {"origin_kind": "authored_from_scratch", "origin": "fork maintainer",
                              "license": "GPL-3.0-or-later"}}
            (statement,) = admit_paths.classify([path], records, origins)
            self.assertEqual(statement["classification"], "project_authored_attested")
            self.assertEqual(statement["record_id"], "fork-example-tool-2026-11-08")
            status = subprocess.run(["git", "-C", str(authority), "status", "--porcelain"], capture_output=True,
                                    text=True, check=True).stdout
            self.assertEqual(status, "")

    def _clone(self, scratch: str) -> Path:
        authority = Path(scratch) / "authority"
        subprocess.run(["git", "clone", "--quiet", str(self.authority), str(authority)], check=True)
        return authority

    def _record(self, authority: Path) -> None:
        with mock.patch.dict(os.environ, IDENTITY):
            fork_init.add_record(
                authority, record_id="fork-example-2026-11-11", paths=["tools/fork_example_tool.py"],
                classification="project-authored-independent", evidence_tier="S", statement="mine",
            )

    def test_record_refuses_an_authority_init_did_not_seed(self) -> None:
        # The upstream's own authority has the same ledger path but no fork README: the tool must not
        # re-serialise and commit its ledger.
        with tempfile.TemporaryDirectory(prefix="nk-fork-foreign-") as scratch:
            authority = self._clone(scratch)
            (authority / fork_init.AUTHORITY_README).write_text("# Provenance authority\n", encoding="utf-8")
            before = (authority / fork_init.AUTHORITY_LEDGER).read_bytes()
            with self.assertRaises(fork_init.ForkInitError) as caught:
                self._record(authority)
            self.assertEqual(caught.exception.code, "AUTHORITY_NOT_A_FORK_AUTHORITY")
            self.assertEqual((authority / fork_init.AUTHORITY_LEDGER).read_bytes(), before)

    def test_record_refuses_an_authority_off_main(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-fork-branch-") as scratch:
            authority = self._clone(scratch)
            subprocess.run(["git", "-C", str(authority), "switch", "--quiet", "-c", "side"], check=True)
            with self.assertRaises(fork_init.ForkInitError) as caught:
                self._record(authority)
            self.assertEqual(caught.exception.code, "AUTHORITY_NOT_MAIN")


class RefusalTest(unittest.TestCase):
    def test_authority_inside_the_checkout_is_refused(self) -> None:
        with self.assertRaises(fork_init.ForkInitError) as caught:
            fork_init.init(ROOT, ROOT / "build" / "fork-authority")
        self.assertEqual(caught.exception.code, "AUTHORITY_INSIDE_REPOSITORY")

    def test_non_empty_authority_is_refused(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-fork-full-") as scratch:
            (Path(scratch) / "keep.txt").write_text("x", encoding="utf-8")
            with self.assertRaises(fork_init.ForkInitError) as caught:
                fork_init.init(ROOT, Path(scratch))
            self.assertEqual(caught.exception.code, "AUTHORITY_NOT_EMPTY")

    def test_authority_inside_another_worktree_of_the_fork_is_refused(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-fork-worktree-") as scratch:
            other = Path(scratch)
            with mock.patch.object(fork_init, "_worktrees", return_value=[ROOT, other]):
                with self.assertRaises(fork_init.ForkInitError) as caught:
                    fork_init.init(ROOT, other / "authority")
            self.assertEqual(caught.exception.code, "AUTHORITY_INSIDE_REPOSITORY")
            self.assertFalse((other / "authority").exists())

    def test_this_checkout_is_one_of_its_worktrees(self) -> None:
        trees = {tree.resolve() for tree in fork_init._worktrees(ROOT)}
        self.assertIn(ROOT.resolve(), trees)

    def test_an_authority_that_is_a_file_is_refused(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-fork-file-") as scratch:
            target = Path(scratch) / "authority"
            target.write_text("x", encoding="utf-8")
            with self.assertRaises(fork_init.ForkInitError) as caught:
                fork_init.init(ROOT, target)
            self.assertEqual(caught.exception.code, "AUTHORITY_NOT_A_DIRECTORY")

    def test_a_failed_verification_leaves_the_target_as_it_was(self) -> None:
        # Before, the half-written authority stayed and the next init refused AUTHORITY_NOT_EMPTY.
        with tempfile.TemporaryDirectory(prefix="nk-fork-failed-") as scratch:
            failing = fork_init.ForkInitError("AUTHORITY_MISMATCH", "simulated")
            for existed in (False, True):
                with self.subTest(existed=existed):
                    target = Path(scratch) / f"authority-{existed}"
                    if existed:
                        target.mkdir()
                    with mock.patch.object(fork_init, "verify", side_effect=failing):
                        with self.assertRaises(fork_init.ForkInitError):
                            fork_init.init(ROOT, target)
                    self.assertEqual(target.exists(), existed)
                    if existed:
                        self.assertEqual(list(target.iterdir()), [])
            self.assertEqual(sorted(path.name for path in Path(scratch).iterdir()),
                             ["authority-True"])

    def test_record_needs_a_statement_and_exact_paths(self) -> None:
        base: dict[str, Any] = {"record_id": "r-1", "paths": ["tools/x.py"], "classification": "project-authored-independent",
                "evidence_tier": "S", "statement": "mine"}
        cases: dict[str, dict[str, Any]] = {
            "empty statement": {"statement": "  "},
            "wildcard path": {"paths": ["tools/*.py"]},
            "parent path": {"paths": ["../x.py"]},
            "bad id": {"record_id": "has space"},
            "upstream-derived without its upstream": {"classification": "derived-translated"},
            "unknown classification": {"classification": "trusted-by-me"},
        }
        for name, change in cases.items():
            with self.subTest(name), tempfile.TemporaryDirectory(prefix="nk-fork-bad-") as scratch:
                with self.assertRaises(fork_init.ForkInitError) as caught:
                    fork_init.add_record(Path(scratch), **(base | change))
                self.assertEqual(caught.exception.code, "RECORD_INVALID")


if __name__ == "__main__":
    unittest.main()
