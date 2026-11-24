# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""tools/compat_table.py: the tier rules, and that no identifier of a private disc leaves it."""

from __future__ import annotations

import contextlib
import io
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import compat_table  # noqa: E402
from nk_core import disc_verify  # noqa: E402

ROOT = TOOLS.parent
COMMIT = "0123456789abcdef0123456789abcdef01234567"


def record(disc_id: str, title: str, outcome: str, *, frames: int = 0, failure: str | None = None,
           tier: str | None = None, sha1: str = "ab" * 20) -> dict:
    """One manifest record shaped as tools/library_build.py writes it, with identifying text."""
    source = f"{title} [fan patch]/{title} (Japan).iso"
    built = outcome == "BUILT"
    row = {
        "source_key": source.casefold(), "source": source, "size_bytes": 1, "mtime_ns": 1,
        "outcome": outcome, "disc_id": disc_id, "title": title, "disc_version": "1.00",
        "identify_error": None, "work_dir": f"G:\\Library\\work\\{disc_id}-0123456789",
        "failure_class": failure if not built else "NONE",
        "run_settings": {"title_timeout_seconds": 2400, "launch_timeout_seconds": 30,
                         "source_commit": COMMIT},
    }
    if built:
        row["launch"] = {"presentation": "FRAME_SUBMITTED" if frames else "NO_FRAME_SUBMISSIONS",
                         "frame_submissions": frames, "failure_class": failure}
    if tier is not None:
        row["verification"] = {"sha1": sha1, "crc32": "cafe0123", "size_bytes": 1, "tier": tier,
                               "redump_name": f"{title} (Japan)"}
    return row


def manifest(titles: list[dict]) -> dict:
    return {"format": "nakagawa-library-build", "schema_version": 1,
            "updated_at": "2026-10-31T00:00:00Z", "disc_root": "G:\\Private Games",
            "user_data_root": "G:\\Private Data",
            "datafile": {"name": "Sony - PlayStation Portable", "version": "2026-06-13",
                         "discs": 3500, "checked_at": "2026-10-31T00:00:00Z"},
            "titles": titles}


SAMPLE = manifest([
    record("TEST71001", "Silver Lantern", "BUILT", frames=12, failure="LAUNCH_TIMEOUT",
           tier=disc_verify.VERIFIED_RETAIL),
    record("TEST71001", "Silver Lantern", "BUILT", frames=0, failure="UNSUPPORTED_IMPORT",
           tier=disc_verify.MODIFIED, sha1="cd" * 20),
    record("TEST71002", "Quiet Harbor", "BUILD_FAILED", failure="COMPILE_FAILED",
           tier=disc_verify.UNKNOWN),
    record("TEST71003", "Lantern Road", "TIMED_OUT", failure="TITLE_TIME_BUDGET"),
    record("TEST71004", "Glass Orchard", "BUILT", frames=3, failure="A_PRIVATE_FREE_TEXT_CAUSE",
           tier=disc_verify.VERIFIED_RETAIL),
    record("TEST71005", "Copper Tide", "PENDING"),
    record("TEST71006", "Copper Tide Again", "DUPLICATE"),
    record("TEST71007", "Replaced Variant", "REPLACED"),
])


class TierRuleTests(unittest.TestCase):
    def test_each_outcome_lands_in_its_tier(self) -> None:
        self.assertEqual(compat_table.tier_of(record("A", "B", "BUILD_FAILED")),
                         compat_table.NOT_BUILT)
        self.assertEqual(compat_table.tier_of(record("A", "B", "TIMED_OUT")), compat_table.NOT_BUILT)
        self.assertEqual(compat_table.tier_of(record("A", "B", "NO_REPORT")), compat_table.NOT_BUILT)
        self.assertEqual(compat_table.tier_of(record("A", "B", "BUILT")), compat_table.NO_FRAME)
        self.assertEqual(compat_table.tier_of(record("A", "B", "BUILT", frames=1)),
                         compat_table.DRAWS_FRAMES)
        self.assertIsNone(compat_table.tier_of(record("A", "B", "PENDING")))
        self.assertIsNone(compat_table.tier_of(record("A", "B", "RUNNING")))
        self.assertIsNone(compat_table.tier_of(record("A", "B", "DUPLICATE")))
        self.assertIsNone(compat_table.tier_of(record("A", "B", "REPLACED")))

    def test_counts_by_tier_verification_and_cause(self) -> None:
        summary = compat_table.summarize(SAMPLE)
        self.assertEqual(summary["counted"], 5)
        tiers = summary["tiers"]
        self.assertEqual(tiers[compat_table.DRAWS_FRAMES][disc_verify.VERIFIED_RETAIL], 2)
        self.assertEqual(tiers[compat_table.NO_FRAME][disc_verify.MODIFIED], 1)
        self.assertEqual(tiers[compat_table.NOT_BUILT][disc_verify.UNKNOWN], 1)
        self.assertEqual(tiers[compat_table.NOT_BUILT][disc_verify.NOT_CHECKED], 1)
        self.assertEqual(summary["left_out"], {"not_built_yet": 2, "copies": 1})
        causes = {(row["tier"], row["cause"]): (row["discs"], row["issue"])
                  for row in summary["causes"]}
        self.assertEqual(causes[(compat_table.NOT_BUILT, "COMPILE_FAILED")], (1, 1730))
        self.assertEqual(causes[(compat_table.NO_FRAME, "UNSUPPORTED_IMPORT")], (1, 308))
        # A cause outside the failure-code taxonomy is never printed by name.
        self.assertEqual(causes[(compat_table.DRAWS_FRAMES, compat_table.OTHER_CAUSE)], (1, 308))
        self.assertEqual(summary["settings"], [{"title_timeout_seconds": 2400,
                                                "launch_timeout_seconds": 30,
                                                "source_commit": COMMIT[:12], "decrypt": None,
                                                "discs": 5}])

    def test_higher_tiers_say_not_measured_and_settings_are_named(self) -> None:
        block = compat_table.render_markdown(compat_table.summarize(SAMPLE), "2026-10-31")
        self.assertIn("| Reaches menus | not measured |", block)
        self.assertIn("| Playable start to end | not measured |", block)
        self.assertIn("commit `0123456789ab`, with a 2400 s time budget per disc and a 30 s "
                      "first launch", block)
        self.assertIn("3,500 discs, version 2026-06-13", block)
        self.assertIn(
            "2 without a current package in this run (not built yet or replaced by a later "
            "same-ID build), and 1 copy of a disc already counted.",
            block,
        )
        # The list in SAMPLE records no serials, so the summary says Unknown is not Modified.
        self.assertIn("A lone fan translation counts as **Unknown**", block)
        with_serials = json.loads(json.dumps(SAMPLE))
        with_serials["datafile"]["serials"] = 12
        self.assertNotIn("lone fan translation",
                         compat_table.render_markdown(compat_table.summarize(with_serials),
                                                      "2026-10-31"))
        mixed = json.loads(json.dumps(SAMPLE))
        mixed["titles"][0]["run_settings"]["title_timeout_seconds"] = 5400
        self.assertIn("not all built with the same settings",
                      compat_table.render_markdown(compat_table.summarize(mixed), "2026-10-31"))
        # Decrypting or not is a setting of its own: the runs are not compared as one.
        decrypting = json.loads(json.dumps(SAMPLE))
        for index, title in enumerate(decrypting["titles"]):
            if isinstance(title.get("run_settings"), dict):
                title["run_settings"]["decrypt"] = index == 0
        block = compat_table.render_markdown(compat_table.summarize(decrypting), "2026-10-31")
        self.assertIn("decrypting each disc's program", block)
        self.assertIn("without decrypting", block)
        self.assertIn("not all built with the same settings", block)


class PrivacyTests(unittest.TestCase):
    def test_no_identifier_of_the_input_reaches_the_summary(self) -> None:
        block = compat_table.render_markdown(compat_table.summarize(SAMPLE), "2026-10-31")
        payload = json.dumps(compat_table.summarize(SAMPLE))
        compat_table.check_public(block, SAMPLE)
        compat_table.check_public(payload, SAMPLE)
        for text in (block, payload):
            for row in SAMPLE["titles"]:
                for value in (row["disc_id"], row["title"], row["source"], row["work_dir"]):
                    self.assertNotIn(value.casefold(), text.casefold())
                verification = row.get("verification") or {}
                for value in (verification.get("sha1"), verification.get("redump_name")):
                    if value:
                        self.assertNotIn(value.casefold(), text.casefold())
            self.assertNotIn("A_PRIVATE_FREE_TEXT_CAUSE", text)
            self.assertIsNone(re.search(r"[A-Z]{4}-?[0-9]{5}", text))
            self.assertNotIn("Private", text)

    def test_the_guard_refuses_a_summary_that_names_a_disc(self) -> None:
        block = compat_table.render_markdown(compat_table.summarize(SAMPLE), "2026-10-31")
        for leak in ("Quiet Harbor", "quiet harbor", "TEST71001", "TEST-71001", "ab" * 20,
                     "G:\\Private Games", "Lantern Road (Japan)"):
            with self.subTest(leak=leak), self.assertRaises(compat_table.PrivacyError):
                compat_table.check_public(block + f"\n{leak}\n", SAMPLE)
        # Folders named only with the summary's own words don't stop it.
        plain = manifest([record("TEST71008", "Quiet Field", "BUILT", frames=1)])
        plain["titles"][0]["source"] = "Library/Modified/Redump/Quiet Field (Japan).iso"
        compat_table.check_public(
            compat_table.render_markdown(compat_table.summarize(plain), "2026-10-31"), plain)
        # Free text in the datafile's version field is never published.
        noted = json.loads(json.dumps(SAMPLE))
        noted["datafile"]["version"] = "my private note"
        self.assertIsNone(compat_table.summarize(noted)["datafile"]["version"])
        self.assertNotIn("private note", compat_table.render_markdown(
            compat_table.summarize(noted), "2026-10-31"))
        # Settings of another shape count as unrecorded, never as text or a crash.
        odd = json.loads(json.dumps(SAMPLE))
        odd["titles"][0]["run_settings"] = {"title_timeout_seconds": "2400 private",
                                            "launch_timeout_seconds": [30],
                                            "source_commit": "x"}
        rows = compat_table.summarize(odd)["settings"]
        self.assertIn({"title_timeout_seconds": None, "launch_timeout_seconds": None,
                       "source_commit": None, "decrypt": None, "discs": 1}, rows)
        # A disc named like part of a cause name is not a leak by itself.
        named_exit = manifest([record("TEST71007", "EXIT", "BUILT", failure="EXITED_ZERO_BEFORE_HLE")])
        compat_table.check_public(
            compat_table.render_markdown(compat_table.summarize(named_exit), "2026-10-31"),
            named_exit)

    def test_cli_writes_the_document_block_and_json(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "library-build.json"
            source.write_text(json.dumps(SAMPLE), encoding="utf-8")
            doc = root / "COMPATIBILITY.md"
            doc.write_text(f"# Compatibility\n\nBefore.\n\n{compat_table.DOC_BEGIN}\nold\n"
                           f"{compat_table.DOC_END}\n\nAfter.\n", encoding="utf-8")
            self.assertEqual(compat_table.main([str(source), "--write-doc", str(doc),
                                                "--json", str(root / "summary.json")]), 0)
            text = doc.read_text(encoding="utf-8")
            self.assertTrue(text.startswith("# Compatibility\n\nBefore.\n\n"))
            self.assertTrue(text.endswith(f"{compat_table.DOC_END}\n\nAfter.\n"))
            self.assertNotIn("\nold\n", text)
            self.assertIn("Whole library, 2026-10-31: 5 disc images counted.", text)
            self.assertEqual(json.loads((root / "summary.json").read_text(encoding="utf-8"))
                             ["counted"], 5)

    def test_the_cli_never_writes_over_its_manifest_or_one_output_twice(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "library-build.json"
            source.write_text(json.dumps(SAMPLE), encoding="utf-8")
            before = source.read_bytes()
            for extra in (["--json", str(source)],
                          ["--output", str(root / "same.md"), "--json", str(root / "same.md")]):
                with self.subTest(extra=extra):
                    with contextlib.redirect_stderr(io.StringIO()):
                        with self.assertRaises(SystemExit):
                            compat_table.main([str(source), *extra])
                    self.assertEqual(source.read_bytes(), before)
                    self.assertFalse((root / "same.md").exists())
            # A hard link to the manifest is the manifest.
            linked = root / "linked.json"
            try:
                os.link(source, linked)
            except OSError:
                self.skipTest("this filesystem makes no hard links")
            with contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    compat_table.main([str(source), "--json", str(linked)])
            self.assertEqual(source.read_bytes(), before)

    def test_the_compatibility_page_has_one_generated_block(self) -> None:
        text = (ROOT / "docs" / "COMPATIBILITY.md").read_text(encoding="utf-8")
        self.assertEqual(text.count(compat_table.DOC_BEGIN), 1)
        self.assertEqual(text.count(compat_table.DOC_END), 1)
        start = text.index(compat_table.DOC_BEGIN)
        block = text[start:text.index(compat_table.DOC_END) + len(compat_table.DOC_END)]
        for shape in compat_table._IDENTIFIER_SHAPES:
            self.assertIsNone(shape.search(block))


class ReleaseValidationTests(unittest.TestCase):
    def setUp(self) -> None:
        tree = tempfile.TemporaryDirectory()
        self.addCleanup(tree.cleanup)
        root = Path(tree.name)
        self.source = root / "library-build.json"
        self.plan_path = root / "release-plan.json"
        self.doc = root / "COMPATIBILITY.md"
        self.output = root / "summary.json"
        self.settings = {
            "source_commit": COMMIT, "title_timeout_seconds": 2400,
            "launch_timeout_seconds": 30, "decrypt": False,
        }
        self.data = manifest([
            record("TEST72001", "Release Fixture One", "BUILT", frames=2,
                   tier=disc_verify.VERIFIED_RETAIL),
            record("TEST72002", "Release Fixture Two", "BUILD_FAILED", failure="COMPILE_FAILED",
                   tier=disc_verify.UNKNOWN, sha1="ef" * 20),
        ])
        self.data["settings"] = dict(self.settings, jobs=1, priority="normal")
        for row in self.data["titles"]:
            row["run_settings"] = dict(self.settings)
        self.plan = {
            "format": "nakagawa-compat-release-plan", "schema_version": 1,
            "settings": dict(self.settings),
            "sources": [
                {"source": row["source"], "size_bytes": row["size_bytes"],
                 "sha1": row["verification"]["sha1"]}
                for row in self.data["titles"]
            ],
        }
        self.doc.write_text(f"Before\n{compat_table.DOC_BEGIN}\nold\n"
                            f"{compat_table.DOC_END}\nAfter\n", encoding="utf-8")
        self.output.write_bytes(b"old summary\n")

    def run_export(self, *, release: bool = True) -> int:
        self.source.write_text(json.dumps(self.data), encoding="utf-8")
        self.plan_path.write_text(json.dumps(self.plan), encoding="utf-8")
        args = [str(self.source), "--write-doc", str(self.doc), "--json", str(self.output)]
        if release:
            args.extend(("--release-plan", str(self.plan_path)))
        return compat_table.main(args)

    def assert_refuses(self, reason: str) -> None:
        before = (self.doc.read_bytes(), self.output.read_bytes())
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit) as error:
            self.run_export()
        self.assertEqual(error.exception.code, 2)
        self.assertIn(reason, stderr.getvalue())
        self.assertNotIn("Release Fixture", stderr.getvalue())
        self.assertEqual((self.doc.read_bytes(), self.output.read_bytes()), before)

    def test_matching_candidate_publishes_aggregate_without_corpus_fields(self) -> None:
        self.assertEqual(self.run_export(), 0)
        summary = json.loads(self.output.read_text(encoding="utf-8"))
        self.assertEqual(summary["counted"], 2)
        self.assertEqual(summary["settings"], [dict(self.settings, source_commit=COMMIT[:12], discs=2)])
        for path in (self.output, self.doc):
            for secret in ("TEST72001", "Release Fixture One", "ab" * 20):
                self.assertNotIn(secret, path.read_text(encoding="utf-8"))

    def test_stale_top_level_source_is_refused(self) -> None:
        self.data["settings"]["source_commit"] = "a" * 40
        self.assert_refuses("release manifest settings differ from the plan")

    def test_stale_or_missing_attempt_settings_are_refused(self) -> None:
        for changed in (dict(self.settings, source_commit="a" * 40),
                        dict(self.settings, launch_timeout_seconds=31),
                        dict(self.settings, decrypt=True), None):
            with self.subTest(changed=changed):
                self.data["titles"][1]["run_settings"] = changed
                self.assert_refuses("release row settings differ from the plan")

    def test_missing_substituted_or_repeated_source_is_refused(self) -> None:
        rows = self.data["titles"]
        replacement = dict(rows[1], source="Synthetic replacement.iso")
        for changed in ([rows[0]], [rows[0], replacement], [rows[0], rows[0]]):
            with self.subTest(changed=changed):
                self.data["titles"] = changed
                self.assert_refuses("release corpus membership differs from the plan")

    def test_same_source_with_different_identity_is_refused(self) -> None:
        row = self.data["titles"][1]
        row["verification"]["sha1"] = "ba" * 20
        self.assert_refuses("release corpus identity differs from the plan")
        row["verification"]["sha1"] = self.plan["sources"][1]["sha1"]
        row["verification"]["size_bytes"] = row["size_bytes"] + 1
        self.assert_refuses("release corpus identity is unverified")

    def test_unattempted_source_is_refused(self) -> None:
        self.data["titles"][1]["outcome"] = "PENDING"
        self.assert_refuses("release corpus has unfinished or unknown outcomes")

    def test_copy_of_replaced_package_counts_as_a_copy(self) -> None:
        previous, current = self.data["titles"]
        previous.update(outcome="REPLACED", replaced_by=current["source"])
        current.update(outcome="BUILT", disc_id=previous["disc_id"])
        copy = dict(previous, source="Synthetic identical copy.iso", outcome="DUPLICATE",
                    duplicate_of=previous["source"])
        copy.pop("run_settings")
        self.data["titles"].append(copy)
        self.plan["sources"].append(dict(self.plan["sources"][0], source=copy["source"]))
        self.assertEqual(self.run_export(), 0)
        summary = json.loads(self.output.read_text(encoding="utf-8"))
        self.assertEqual(summary["counted"], 1)
        self.assertEqual(summary["left_out"], {"not_built_yet": 1, "copies": 1})

        copy["duplicate_of"] = current["source"]
        self.assert_refuses("release corpus copy does not match its counted source")
        copy["duplicate_of"] = previous["source"]
        current.update(outcome="REPLACED", replaced_by=previous["source"])
        self.assert_refuses("release corpus replacement references form a cycle")

    def test_private_release_plan_cannot_be_an_output(self) -> None:
        self.plan_path.write_text(json.dumps(self.plan), encoding="utf-8")
        before = self.plan_path.read_bytes()
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            compat_table.main([str(self.source), "--release-plan", str(self.plan_path),
                               "--json", str(self.plan_path)])
        self.assertEqual(error.exception.code, 2)
        self.assertEqual(self.plan_path.read_bytes(), before)
        self.assertFalse(self.source.exists())

    def test_diagnostic_export_can_read_historical_rows(self) -> None:
        self.data["titles"][1].pop("run_settings")
        self.data.pop("settings")
        self.assertEqual(self.run_export(release=False), 0)
        summary = json.loads(self.output.read_text(encoding="utf-8"))
        self.assertEqual(summary["counted"], 2)
        self.assertTrue(any(row["source_commit"] is None for row in summary["settings"]))


if __name__ == "__main__":
    unittest.main()
