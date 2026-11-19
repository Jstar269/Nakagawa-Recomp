# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The record-gap inventory must agree with the gate that produces the finding.

``tools/provenance_record_gap.py`` exists so a maintainer can admit every
unrecorded path in one batch instead of one pull request at a time.  If it
disagreed with ``_exact_record_finding_for_change`` -- the function that decides
whether a change fails with ``TRUSTED_PATH_MISSING`` -- the batch would clear
some paths and not others, and the inventory would be worse than useless.

So the inventory is tested against that function directly, and against the
committed ledger, rather than against a list frozen into the test.
"""

from __future__ import annotations

import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import provenance_attest_verify as verifier  # noqa: E402
import provenance_ledger  # noqa: E402
import provenance_record_gap as gap  # noqa: E402

LEDGER = ROOT / "assets" / "public_provenance_ledger.json"

#: A record that the gate accepts for any path: it classifies as implementation-grade.
QUALIFYING_RECORD = {"id": "PROV-TEST", "classification": "project-authored-independent",
                     "evidence_tier": "S"}


def constructed_gap() -> tuple[set[str], set[str]]:
    """The committed coverage with a sample of the paths the gate asks about removed.

    The live repository may have no gaps at all once every path is admitted, and a
    comparison of two empty lists proves nothing, so the agreement tests run on this
    constructed gap instead.  Returns ``(covered, victims)``; neither is empty.
    """
    covered = gap.exact_paths_from_public_ledger(LEDGER)
    policy = gap.load_policy(ROOT / gap.POLICY_PATH)
    asked = sorted(
        path for path in covered
        if policy.resolve(path).disposition == "included"
        and provenance_ledger.admission_requires_implementation(path)
    )
    victims = set(asked[:: max(1, len(asked) // 10)])
    assert victims, "the gate asks about no covered path, which means it stopped asking"
    return covered - victims, victims


class TestInventoryShape(unittest.TestCase):
    def test_the_ledger_is_the_source_of_the_default_answer(self) -> None:
        covered = gap.exact_paths_from_public_ledger(LEDGER)
        self.assertTrue(covered, "the committed public ledger names records")
        document = json.loads(LEDGER.read_text(encoding="utf-8"))
        for entry in document["entries"]:
            classification, record_id = verifier._claim(entry)
            listed = entry["path"] in covered
            expected = classification in verifier.IMPLEMENTATION_CLASSES and bool(record_id)
            self.assertEqual(listed, expected, entry["path"])

    def test_the_inventory_is_deterministic_and_sorted(self) -> None:
        covered, victims = constructed_gap()
        first = gap.record_gaps(covered=covered)
        second = gap.record_gaps(covered=covered)
        self.assertEqual(first, second)
        paths = [item["path"] for item in first]
        self.assertLessEqual(victims, set(paths))
        self.assertEqual(paths, sorted(paths))
        self.assertEqual(len(paths), len(set(paths)))

    def test_a_covered_path_is_never_reported(self) -> None:
        covered = gap.exact_paths_from_public_ledger(LEDGER)
        reported = {item["path"] for item in gap.record_gaps()}
        self.assertEqual(reported & covered, set())

    def test_documentation_and_configuration_that_need_no_record_are_not_reported(self) -> None:
        """A path the gate never asks about must not become a maintainer chore.

        With nothing covered every path the gate asks about is reported, so these
        paths stay out only because the classifier exempts them, which is pinned
        here on its own and not through the inventory's filter."""
        reported = {item["path"] for item in gap.record_gaps(covered=set())}
        self.assertTrue(reported)
        for path in ("README.md", "LICENSE", "docs/CI.md", "docs/SETUP.md"):
            with self.subTest(path=path):
                self.assertFalse(provenance_ledger._admission_requires_implementation(path))
                self.assertIsNone(verifier._exact_record_finding_for_change(
                    path, base_blobs={path: b"old"}, candidate_blobs={path: b"new"},
                    exact_records={},
                ))
                self.assertNotIn(path, reported)

    def test_every_uncovered_path_the_gate_asks_about_is_reported(self) -> None:
        """The complement: nothing the gate would ask about is left out.

        The expectation is the gate's own answer for an edit to each public path,
        given a record for exactly the covered paths, not a copy of the inventory's
        filter."""
        covered, victims = constructed_gap()
        reported = {item["path"] for item in gap.record_gaps(covered=covered)}
        policy = gap.load_policy(ROOT / gap.POLICY_PATH)
        records = {path: dict(QUALIFYING_RECORD, paths=[path]) for path in covered}
        expected = {
            path for path in gap.tracked_paths(ROOT)
            if policy.resolve(path).disposition == "included"
            and verifier._exact_record_finding_for_change(
                path, base_blobs={path: b"old"}, candidate_blobs={path: b"new"},
                exact_records=records,
            ) == "TRUSTED_PATH_MISSING"
        }
        self.assertLessEqual(victims, expected)
        self.assertEqual(reported, expected)

    def test_a_path_the_gate_asks_about_is_reported_once_uncovered(self) -> None:
        """Remove one covered implementation path: the inventory must name it.

        The live repository may have no gaps at all once every path is admitted,
        so the gate's question is proved on a constructed gap instead."""
        covered = gap.exact_paths_from_public_ledger(LEDGER)
        policy = gap.load_policy(ROOT / gap.POLICY_PATH)
        asked = sorted(
            path for path in covered
            if policy.resolve(path).disposition == "included"
            and provenance_ledger.admission_requires_implementation(path)
        )
        self.assertTrue(asked, "the gate asks about no covered path, which means it stopped asking")
        victim = asked[0]
        reported = {item["path"] for item in gap.record_gaps(covered=covered - {victim})}
        self.assertIn(victim, reported)

    def test_upstream_production_gap_requires_campaign_disposition(self) -> None:
        """A derived production gap without a roadmap owner fails closed."""
        covered = gap.exact_paths_from_public_ledger(LEDGER)
        victim = "src/core/nk_psp_aes.c"
        self.assertIn(victim, covered)
        item = next(
            item for item in gap.record_gaps(covered=covered - {victim})
            if item["path"] == victim
        )
        self.assertEqual(item["deterministic_class"], "upstream_derived")
        self.assertEqual(item["independence"]["status"], "missing")
        self.assertIn("owner or disposition", item["independence"]["reason"])

    def test_upstream_production_gap_accepts_explicit_campaign_disposition(self) -> None:
        """A group named in the campaign carries its owner and disposition."""
        covered = gap.exact_paths_from_public_ledger(LEDGER)
        victim = "src/rt/sched.c"
        self.assertIn(victim, covered)
        item = next(
            item for item in gap.record_gaps(covered=covered - {victim})
            if item["path"] == victim
        )
        self.assertEqual(item["independence"], {
            "status": "mapped",
            "owner": "G1",
            "disposition": "clean-room-rewrite",
            "source": gap.INDEPENDENCE_PLAN_PATH,
        })

    def test_bounded_campaign_disposition_is_explicit(self) -> None:
        metadata = gap.independence_metadata("src/rt/ge.c")
        self.assertIsNotNone(metadata)
        self.assertEqual(metadata["owner"], "G10")
        self.assertEqual(metadata["disposition"], "bounded-exclusion")

    def test_a_group_verdict_is_read_from_its_own_heading(self) -> None:
        """A ``(c)`` or ``(a)`` in passing, or the prose after the list, never sets a verdict."""
        plan = "\n".join([
            "- **G1 loader → (a), late.** Rewritten from the format; the vendored",
            "  header keeps its Copyright (c) notice, unlike the (c) groups.",
            "- **G2 HLE core → (b), staged.** The host-boundary handlers",
            "  get an (a) rewrite.",
            "- **G3 interim → (a), late.** Interim disposition is bounded-(c):",
            "  the comparison path only.",
            "- **G4 PGD → (c) keep excluded.** No code work.",
            "- **G5 media → split.** `a.c` → (b); `b.c` → (a) rewrite.",
            "- **G6 transport → evidence, not rewrite.** Validate with its selftest.",
            "",
            "Later prose: entries carrying a bounded-(c) disposition (G3, G4) and (a) rewrites.",
        ])
        self.assertEqual(gap._campaign_targets(plan), {
            "G1": "clean-room-rewrite",
            "G2": "LLE-replacement",
            "G3": "bounded-exclusion",
            "G4": "bounded-exclusion",
            "G5": "clean-room-rewrite",
            "G6": "evidence-hardening",
        })
        live = gap._campaign_targets((ROOT / gap.INDEPENDENCE_PLAN_PATH).read_text(encoding="utf-8"))
        self.assertEqual(live["G9"], "LLE-replacement", "G9's heading names (b)")
        self.assertEqual(live["G23"], "evidence-hardening", "the last item does not run to the end of the file")

    def test_an_indented_list_item_ends_the_item_before_it(self) -> None:
        """A group's item never runs into the next list item, however that item is indented.

        An indented item used to count as a continuation of the one before it, so a
        group whose own item names no verdict took the next group's.
        """
        plan = "\n".join([
            "  - **G1 scheduler.** The scheduler stays a host boundary.",
            "  - **G2 ISO → (a).** Rewritten from the format.",
        ])
        self.assertEqual(gap._campaign_targets(plan), {"G2": "clean-room-rewrite"})
        nested = "\n".join([
            "- **G1 scheduler.** The scheduler stays a host boundary.",
            "  - a sub-item naming (a) is its own item",
            "  1. and so is a numbered one naming (b)",
            "\t* or a tab-indented one naming bounded-(c)",
            "- **G2 media → split.** `a.c` is kept as it is;",
            "  `b.c` → (b), which this continuation line still gives it.",
            "    - **G3 ISO → (a).** Rewritten from the format.",
        ])
        self.assertEqual(gap._campaign_targets(nested), {
            "G2": "LLE-replacement",
            "G3": "clean-room-rewrite",
        })

    def test_every_live_campaign_item_keeps_a_disposition(self) -> None:
        """Ending an item at the next list item costs no group of the campaign its target."""
        text = (ROOT / gap.INDEPENDENCE_PLAN_PATH).read_text(encoding="utf-8")
        items = [
            match.group(1) for line in text.splitlines()
            if (match := gap._CAMPAIGN_TARGET.match(line)) is not None
        ]
        self.assertEqual(len(items), len(set(items)), "a group has two target items")
        self.assertGreaterEqual(len(items), 23)
        self.assertEqual(sorted(gap._campaign_targets(text)), sorted(items))

    def test_the_campaign_is_read_once_per_inventory(self) -> None:
        from unittest import mock

        with mock.patch.object(gap, "_campaign_targets", wraps=gap._campaign_targets) as targets:
            gap.record_gaps(covered=set())
            gap.disposition_gaps(repo=ROOT)
        self.assertEqual(targets.call_count, 2)

    def test_trusted_classification_names_the_inventory_class(self) -> None:
        """With ``--trusted-ledger`` the inventory reports the authority's class, not the public one."""
        import io
        from contextlib import redirect_stdout
        from unittest import mock

        covered, victims = constructed_gap()
        victim = sorted(victims)[0]
        public_class = gap.public_classifications(LEDGER)[victim]
        # A gate class the public ledger does not give the victim, so the authority's wins visibly.
        trusted_class = "unresolved" if public_class != "unresolved" else "upstream_derived"
        buffer = io.StringIO()
        with mock.patch.object(gap, "exact_paths_from_trusted_ledger", return_value=covered), \
                mock.patch.object(gap, "classifications_from_trusted_ledger",
                                  return_value={victim: trusted_class}), \
                redirect_stdout(buffer):
            gap.main(["--repo", str(ROOT), "--json", "--trusted-ledger", str(LEDGER)])
        reported = {item["path"]: item for item in json.loads(buffer.getvalue())["paths"]}
        self.assertEqual(reported[victim]["deterministic_class"], trusted_class)


class TestAgreementWithTheGate(unittest.TestCase):
    """The decisive property: the inventory predicts the gate exactly."""

    def test_every_reported_path_would_fail_with_a_missing_record(self) -> None:
        covered, victims = constructed_gap()
        reported = gap.record_gaps(covered=covered)
        self.assertLessEqual(victims, {item["path"] for item in reported})
        for item in reported:
            path = item["path"]
            self.assertTrue(
                provenance_ledger._admission_requires_implementation(path),
                f"{path} is in the inventory but needs no record",
            )
            finding = verifier._exact_record_finding_for_change(
                path,
                base_blobs={},
                candidate_blobs={path: b"changed"},
                exact_records={},
            )
            self.assertEqual(
                finding, "TRUSTED_PATH_MISSING",
                f"{path} is in the inventory but the gate would not report it",
            )

    def test_a_path_the_gate_accepts_is_not_in_the_inventory(self) -> None:
        covered = gap.exact_paths_from_public_ledger(LEDGER)
        reported = {item["path"] for item in gap.record_gaps(covered=covered)}
        sample = sorted(covered)[:: max(1, len(covered) // 20)][:20]
        for path in sample:
            if provenance_ledger._admission_requires_implementation(path):
                self.assertNotIn(path, reported)

    def test_the_gate_would_not_report_a_deterministic_documentation_path(self) -> None:
        finding = verifier._exact_record_finding_for_change(
            "docs/CI.md",
            base_blobs={"docs/CI.md": b"old"},
            candidate_blobs={"docs/CI.md": b"new"},
            exact_records={},
        )
        self.assertIsNone(finding)


class TestOutput(unittest.TestCase):
    def _check(self, **patches: object) -> tuple[int, str]:
        import io
        from contextlib import redirect_stderr, redirect_stdout
        from unittest import mock

        with mock.patch.multiple(gap, **patches):
            stderr = io.StringIO()
            with redirect_stderr(stderr), redirect_stdout(io.StringIO()):
                rc = gap.main(["--repo", str(ROOT), "--check"])
        return rc, stderr.getvalue()

    def test_check_mode_passes_on_the_repository_with_the_reviewed_baseline(self) -> None:
        rc, err = self._check(KNOWN_DISPOSITION_GAPS=dict(gap.KNOWN_DISPOSITION_GAPS))
        self.assertEqual(rc, 0, err)

    def test_check_mode_passes_when_no_gap_exists(self) -> None:
        rc, _ = self._check(disposition_gaps=lambda **_kwargs: [], KNOWN_DISPOSITION_GAPS={})
        self.assertEqual(rc, 0)

    def test_check_mode_flags_a_group_whose_item_names_no_verdict(self) -> None:
        """An indented item that follows gives the group before it no disposition.

        The campaign here is the live one with G1's and G2's items replaced by two
        indented one-line items: G1 names no verdict, G2 names ``(a)``.  ``--check``
        must ask about G1's paths; when G1 took G2's verdict it passed.
        """
        lines = (ROOT / gap.INDEPENDENCE_PLAN_PATH).read_text(encoding="utf-8").splitlines()
        starts = {
            match.group(1): index for index, line in enumerate(lines)
            if (match := gap._CAMPAIGN_TARGET.match(line)) is not None
        }
        self.assertLess(starts["G1"], starts["G2"])
        self.assertLess(starts["G2"], starts["G3"])
        lines[starts["G1"]:starts["G3"]] = [
            "  - **G1 scheduler.** The scheduler stays a host boundary.",
            "  - **G2 ISO/VFS → (a).** Rewritten from the format.",
        ]
        plan = "\n".join(lines)
        groups = gap._campaign_groups(plan)
        self.assertIn("src/rt/sched.c", groups["G1"])

        rc, err = self._check(
            _campaign_index=lambda _path: (groups, gap._campaign_targets(plan)),
        )

        self.assertEqual(rc, 1, err)
        flagged = {
            line.split()[0] for line in err.splitlines() if "(status=missing;" in line
        }
        self.assertIn("src/rt/sched.c", flagged)
        self.assertLessEqual(flagged, set(groups["G1"]))
        self.assertNotIn("stale", err)

    def test_a_trusted_record_does_not_hide_a_missing_disposition(self) -> None:
        """A covered upstream-derived path with no disposition still fails the gate.

        FAILING_BEFORE: the gate only looked at paths without an exact trusted
        record, and every tracked path has one, so it could never fail.
        """
        victim = "src/core/nk_psp_aes.c"
        self.assertIn(victim, gap.exact_paths_from_public_ledger(LEDGER))
        self.assertIn(victim, gap.KNOWN_DISPOSITION_GAPS)
        baseline = {path: entry for path, entry in gap.KNOWN_DISPOSITION_GAPS.items()
                    if path != victim}
        rc, err = self._check(KNOWN_DISPOSITION_GAPS=baseline)
        self.assertEqual(rc, 1)
        self.assertIn(victim, err)
        self.assertIn("status=missing", err)

    def test_every_baseline_entry_is_a_live_gap(self) -> None:
        live = {str(item["path"]) for item in gap.disposition_gaps(repo=ROOT)}
        self.assertEqual(set(gap.KNOWN_DISPOSITION_GAPS), live)
        gap.validate_baseline(gap.KNOWN_DISPOSITION_GAPS)

    def test_a_mapped_path_is_not_a_disposition_gap(self) -> None:
        live = {str(item["path"]) for item in gap.disposition_gaps(repo=ROOT)}
        self.assertIsNotNone(gap.independence_metadata("src/rt/hle.c"))
        self.assertNotIn("src/rt/hle.c", live)

    def test_check_mode_fails_closed_on_stale_baseline_entry(self) -> None:
        """A baseline entry whose gap is resolved must be removed (stale entries fail)."""
        stale_path = "src/rt/resolved_gap.c"
        rc, err = self._check(
            disposition_gaps=lambda **_kwargs: [],
            KNOWN_DISPOSITION_GAPS={stale_path: {"issue": "#355", "reason": "resolved since"}},
        )
        self.assertEqual(rc, 1)
        self.assertIn(stale_path, err)
        self.assertIn("stale", err)

    def test_baseline_validation_enforces_owner_and_reason(self) -> None:
        """Every baseline entry must carry an issue owner (#N) and a non-empty reason."""
        gap.validate_baseline({"src/a.c": {"issue": "#355", "reason": "unresolved"}})
        for bad in (
            {"src/a.c": {"reason": "missing owner"}},
            {"src/a.c": {"issue": "ISSUES.md", "reason": "not a live issue"}},
            {"src/a.c": {"issue": "#355", "reason": ""}},
            {"src/a.c": "not-a-dict"},
        ):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                gap.validate_baseline(bad)  # type: ignore[arg-type]

    def test_check_records_mode_fails_when_any_unrecorded_gap_remains(self) -> None:
        from unittest import mock

        covered = gap.exact_paths_from_public_ledger(LEDGER)
        policy = gap.load_policy(ROOT / gap.POLICY_PATH)
        victim = min(
            path for path in covered
            if policy.resolve(path).disposition == "included"
            and provenance_ledger.admission_requires_implementation(path)
        )
        with mock.patch.object(gap, "exact_paths_from_public_ledger",
                               return_value=covered - {victim}):
            self.assertEqual(gap.main(["--repo", str(ROOT), "--check-records"]), 1)
        with mock.patch.object(gap, "record_gaps", return_value=[]):
            self.assertEqual(gap.main(["--repo", str(ROOT), "--check-records"]), 0)

    def test_notice_and_format_documents_are_not_production_code(self) -> None:
        """Licences, provenance notes and format docs never enter the ratchet."""
        for path in ("src/rt/atrac3p/LICENSE.LGPLv2.1.txt", "src/rt/atrac3p/PROVENANCE.md",
                     "tools/TRACE_FORMAT.md"):
            with self.subTest(path=path):
                self.assertFalse(gap._is_production_code(path))
                self.assertNotIn(path, gap.KNOWN_DISPOSITION_GAPS)
                self.assertNotIn(path, {str(item["path"]) for item in gap.disposition_gaps(repo=ROOT)})
        self.assertTrue(gap._is_production_code("src/core/nk_psp_aes.c"))

    def test_trusted_classification_wins_over_the_public_projection(self) -> None:
        """A path the authority calls upstream-derived is a gap even if the public ledger disagrees."""
        from unittest import mock

        victim = "src/core/nk_psp_aes.c"
        self.assertIn(victim, gap.KNOWN_DISPOSITION_GAPS)
        live_public = {str(item["path"]) for item in gap.disposition_gaps(repo=ROOT)}
        self.assertIn(victim, live_public)
        # Reclassify the victim away from upstream-derived in a copy of the public
        # ledger view; only the authority's answer may put it back.
        reclassified = {victim: "project_authored"}
        with mock.patch.object(gap, "public_classifications",
                                        return_value=reclassified):
            silenced = {str(item["path"]) for item in gap.disposition_gaps(repo=ROOT)}
            restored = {
                str(item["path"])
                for item in gap.disposition_gaps(
                    repo=ROOT, trusted_classifications={victim: "upstream_derived"})
            }
        self.assertNotIn(victim, silenced)
        self.assertIn(victim, restored)

    def test_check_with_trusted_ledger_passes_the_authority_classification(self) -> None:
        import io
        from contextlib import redirect_stderr, redirect_stdout
        from unittest import mock

        seen: dict[str, object] = {}

        def fake_gaps(**kwargs: object) -> list[dict[str, object]]:
            seen.update(kwargs)
            return []

        with mock.patch.object(gap, "exact_paths_from_trusted_ledger", return_value=set()), \
                mock.patch.object(gap, "classifications_from_trusted_ledger",
                                  return_value={"src/a.c": "upstream_derived"}), \
                mock.patch.object(gap, "record_gaps", return_value=[]), \
                mock.patch.object(gap, "disposition_gaps", side_effect=fake_gaps), \
                mock.patch.object(gap, "KNOWN_DISPOSITION_GAPS", {}), \
                redirect_stderr(io.StringIO()), redirect_stdout(io.StringIO()):
            rc = gap.main(["--repo", str(ROOT), "--check", "--trusted-ledger", str(LEDGER)])
        self.assertEqual(rc, 0)
        self.assertEqual(seen.get("trusted_classifications"), {"src/a.c": "upstream_derived"})

    def test_documented_flag_behaviour_is_described(self) -> None:
        doc = gap.__doc__ or ""
        self.assertIn("--check-records", doc)
        self.assertIn("disposition ratchet", doc)
        self.assertNotIn("``--check`` fails closed when they do not", doc)
        readme = (ROOT / "tools" / "README.md").read_text(encoding="utf-8")
        rows = [line for line in readme.splitlines()
                if line.startswith("| `provenance_record_gap.py` |")]
        self.assertEqual(len(rows), 1, "tools/README.md needs exactly one index row for the tool")
        self.assertIn("disposition", rows[0])
        self.assertIn("--check-records", rows[0])

    def test_combining_the_two_gates_is_rejected(self) -> None:
        import io
        from contextlib import redirect_stderr, redirect_stdout
        err = io.StringIO()
        with redirect_stderr(err), redirect_stdout(io.StringIO()):
            rc = gap.main(["--repo", str(ROOT), "--check", "--check-records"])
        self.assertEqual(rc, 2)
        self.assertIn("separate gates", err.getvalue())

    def test_the_inventory_mode_is_not_a_gate(self) -> None:
        self.assertEqual(gap.main(["--repo", str(ROOT)]), 0)

    def test_json_output_names_every_gap(self) -> None:
        import io
        from contextlib import redirect_stdout

        from unittest import mock

        covered, victims = constructed_gap()
        buffer = io.StringIO()
        with mock.patch.object(gap, "exact_paths_from_public_ledger", return_value=covered), \
                redirect_stdout(buffer):
            gap.main(["--repo", str(ROOT), "--json"])
        document = json.loads(buffer.getvalue())
        self.assertEqual(document["gap_count"], len(document["paths"]))
        self.assertLessEqual(victims, {item["path"] for item in document["paths"]})
        self.assertEqual(
            [item["path"] for item in document["paths"]],
            [item["path"] for item in gap.record_gaps(covered=covered)],
        )



class TestInventoryPrintsOnlyItsFields(unittest.TestCase):
    """The inventory prints a path, its gate class and the fixed status strings, nothing more.

    A trusted ledger's records and a ledger's classification field may hold anything; only a
    class name the gate defines reaches the output (CodeQL py/clear-text-logging-sensitive-data
    on the record_gaps flow, #2720).
    """

    ROW_KEYS = {"path", "reason", "deterministic_class", "independence"}
    INDEPENDENCE_KEYS = {"status", "owner", "disposition", "reason", "source"}
    # Marker strings that must never appear in the output. The extra fields that carry them have
    # plain names on purpose: the tool projects rows by its own key list and never looks at a
    # field's name, and names that look like credentials only make a scanner treat this
    # fixture as stored sensitive data.
    MARKERS = ("marker-RECORD-EXTRA", "marker-RECORD-NOTE", "marker-RECORD-CLASS", "marker-PUBLIC-CLASS")

    def _run(self, *mode: str) -> str:
        import io
        import tempfile
        from contextlib import redirect_stdout
        from unittest import mock

        covered, victims = constructed_gap()
        self.assertGreaterEqual(len(victims), 3)
        first, second, third = sorted(victims)[:3]
        records = [
            {"id": "PROV-EXTRA-1", "paths": [first], "classification": f"not-a-class={self.MARKERS[2]}",
             "extra_field": self.MARKERS[0], "extra_note": self.MARKERS[1]},
            {"id": "PROV-EXTRA-2", "paths": [second], "classification": "unresolved",
             "extra_field": self.MARKERS[0], "extra_nested": {"inner": self.MARKERS[1]}},
        ]
        public = dict(gap.public_classifications(LEDGER))
        # No trusted record speaks for the third path, so its class comes from this field.
        public[third] = f"not-a-class={self.MARKERS[3]}"
        with tempfile.TemporaryDirectory(prefix="nakagawa-record-gap-test-") as tmp:
            ledger = Path(tmp) / "trusted.json"
            ledger.write_text(json.dumps({"records": records}), encoding="utf-8")
            buffer = io.StringIO()
            with mock.patch.object(gap, "exact_paths_from_trusted_ledger", return_value=covered), \
                    mock.patch.object(gap, "public_classifications", return_value=public), \
                    redirect_stdout(buffer):
                self.assertEqual(gap.main(["--repo", str(ROOT), *mode, "--trusted-ledger", str(ledger)]), 0)
        return buffer.getvalue()

    def test_json_rows_hold_only_the_printed_fields(self) -> None:
        out = self._run("--json")
        for marker in self.MARKERS:
            self.assertNotIn(marker, out)
        document = json.loads(out)
        self.assertEqual(set(document), {"source", "gap_count", "paths"})
        self.assertEqual(document["source"], "trusted authority (--trusted-ledger)")
        allowed_classes = set(provenance_ledger.ALLOWED_CLASSES)
        for row in document["paths"]:
            self.assertLessEqual(set(row), self.ROW_KEYS, row)
            self.assertEqual(row["reason"], "no exact trusted record")
            self.assertIn(row["deterministic_class"], allowed_classes, row)
            if "independence" in row:
                self.assertLessEqual(set(row["independence"]), self.INDEPENDENCE_KEYS, row)
                self.assertTrue(all(isinstance(value, str) for value in row["independence"].values()))

    def test_text_rows_hold_only_the_path_and_class(self) -> None:
        out = self._run()
        for marker in self.MARKERS:
            self.assertNotIn(marker, out)
        allowed_classes = set(provenance_ledger.ALLOWED_CLASSES)
        for line in out.splitlines():
            if line.startswith("  ") and not line.startswith("    "):
                path, _, bracket = line.strip().partition("  [")
                self.assertTrue(path and bracket.endswith("]"), line)
                self.assertIn(bracket[:-1], allowed_classes, line)


if __name__ == "__main__":
    unittest.main()
