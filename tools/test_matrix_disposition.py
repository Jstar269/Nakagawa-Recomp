# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Guard the evidence-matrix `disposition` field against becoming uninformative.

`disposition` once restated `source_shape_classification`: every classified case
was emitted as "UNKNOWN". That inverted the intended reading, because the
"UNKNOWN" set was exactly the set of cases that had been categorised. These
tests keep the field independent and keep the deletion boundary conservative.
"""

from __future__ import annotations

from pathlib import Path
import sys
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import test_matrix_report  # noqa: E402

class DispositionMappingTests(unittest.TestCase):
    def test_every_shape_category_has_a_disposition(self) -> None:
        expected = {
            "NOT_APPLICABLE",
            "A_LEGITIMATE_STRUCTURAL_INVARIANT",
            "B_HISTORICAL_BEHAVIORAL_PROXY",
            "C_REDUNDANT_BEHAVIORAL_PROXY",
            "D_ONLY_AVAILABLE_EVIDENCE",
            "E_OBSOLETE_INVARIANT",
        }
        self.assertEqual(set(test_matrix_report._DISPOSITION_BY_SHAPE), expected)

    def test_no_category_maps_to_unknown(self) -> None:
        self.assertNotIn("UNKNOWN", set(test_matrix_report._DISPOSITION_BY_SHAPE.values()))

    def test_only_the_deletion_boundary_yields_delete_candidate(self) -> None:
        mapping = test_matrix_report._DISPOSITION_BY_SHAPE
        deletable = {shape for shape, verdict in mapping.items() if verdict == "DELETE_CANDIDATE"}
        self.assertEqual(deletable, {"C_REDUNDANT_BEHAVIORAL_PROXY", "E_OBSOLETE_INVARIANT"})

    def test_reviewed_structural_invariants_are_kept(self) -> None:
        mapping = test_matrix_report._DISPOSITION_BY_SHAPE
        self.assertEqual(mapping["A_LEGITIMATE_STRUCTURAL_INVARIANT"], "KEEP")
        self.assertEqual(mapping["NOT_APPLICABLE"], "KEEP")


_DELETION_SHAPES = {"C_REDUNDANT_BEHAVIORAL_PROXY", "E_OBSOLETE_INVARIANT"}


class SyntheticMatrixTests(unittest.TestCase):
    """Exercise the generator without requiring excluded historical matrix data."""

    def test_the_classifier_never_generates_a_deletion_category(self) -> None:
        # Categories C and E are the deletion boundary: a delete verdict requires
        # manual review, so the generated classification must never reach them.
        modules = [
            "test_not_in_any_list",
            *sorted(test_matrix_report._PROXY_MODULES),
            *sorted(test_matrix_report._ONLY_AVAILABLE_MODULES),
            *sorted(test_matrix_report._STRUCTURAL_MODULES),
            *sorted(test_matrix_report._MIXED_SOURCE_SHAPE_MODULE_CATEGORIES),
        ]
        evidences = (
            "security tripwire",
            "manifest/structural",
            "source-shape",
            "integration",
            "pure functional/unit",
        )
        texts = ("", "plain assertions", "SPDX provenance forbidden manifest contract")
        seen: set[str] = set()
        for module_name in modules:
            for evidence in evidences:
                for text in texts:
                    shape = test_matrix_report._source_shape_classification(module_name, text, evidence)
                    seen.add(shape)
                    with self.subTest(module=module_name, evidence=evidence, text=text):
                        self.assertIn(shape, test_matrix_report._DISPOSITION_BY_SHAPE)
                        self.assertNotIn(shape, _DELETION_SHAPES)
        # Vacuity guard: the grid reaches every non-deletion category.
        self.assertEqual(seen, set(test_matrix_report._DISPOSITION_BY_SHAPE) - _DELETION_SHAPES)

    def test_build_report_fills_disposition_from_the_mapping_and_proposes_no_deletions(self) -> None:
        # Run the real generator over a small, known suite (this module and a
        # structural one) instead of discovering the whole tree.
        loader = unittest.TestLoader()
        import test_publish_audit

        small_suite = unittest.TestSuite([
            loader.loadTestsFromModule(sys.modules[__name__]),
            loader.loadTestsFromModule(test_publish_audit),
        ])
        with mock.patch.object(unittest.TestLoader, "discover", return_value=small_suite):
            report = test_matrix_report.build_report()
        cases = report["cases"]
        self.assertGreater(len(cases), 0)
        mapping = test_matrix_report._DISPOSITION_BY_SHAPE
        for case in cases:
            with self.subTest(case=case["id"]):
                self.assertNotIn(case["source_shape_classification"], _DELETION_SHAPES)
                self.assertNotEqual(case["disposition"], "DELETE_CANDIDATE")
                self.assertNotEqual(case["disposition"], "UNKNOWN")
                self.assertEqual(case["disposition"], mapping[case["source_shape_classification"]])
        # Vacuity guard: the structural module really was classified, so the loop
        # above checked a category other than NOT_APPLICABLE.
        self.assertIn(
            "A_LEGITIMATE_STRUCTURAL_INVARIANT",
            {case["source_shape_classification"] for case in cases},
        )


if __name__ == "__main__":
    unittest.main()
