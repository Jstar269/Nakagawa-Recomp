# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Deterministic offline unit tests for tools/audit_public_issue_links.py."""

import json
import pathlib
import sys
import tempfile
import unittest
from unittest.mock import MagicMock, patch

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))

from tools.audit_public_issue_links import (
    CURRENT_FACING_DOCS,
    PRIVATE_EXCLUDED_DOCS,
    SHORTHAND_AUDIT_DOCS,
    audit_markdown_files,
    fetch_public_issues_map,
    get_tracked_markdown_files,
    main,
    status_findings,
)


class TestAuditPublicIssueLinks(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.repo_path = pathlib.Path(self.temp_dir.name)

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def test_private_exclusions_are_distinct_without_changing_audit_coverage(self) -> None:
        excluded = {
            "docs/KEY_HISTORY_SCRUB.md",
            "docs/PGD_AMCTRL_REVIEW_PACKET.md",
            "docs/PGD_KEYS.md",
            "docs/PGF_LICENSE_REVIEW_PACKET.md",
        }
        self.assertEqual(PRIVATE_EXCLUDED_DOCS, excluded)
        self.assertTrue(PRIVATE_EXCLUDED_DOCS.isdisjoint(CURRENT_FACING_DOCS))
        self.assertEqual(SHORTHAND_AUDIT_DOCS, CURRENT_FACING_DOCS | excluded)

        doc = self.repo_path / "docs" / "KEY_HISTORY_SCRUB.md"
        doc.parent.mkdir()
        doc.write_text("See unresolved issue #999\n", encoding="utf-8")
        findings = audit_markdown_files(self.repo_path, {})
        self.assertEqual(len(findings), 1)
        self.assertFalse(findings[0][4])
        self.assertIn("DEAD / UNRESOLVED SHORTHAND", findings[0][3])

    def test_live_issue_and_pr_pass(self) -> None:
        doc = self.repo_path / "README.md"
        doc.write_text(
            "See https://github.com/Jstar269/nakagawa-recomp/issues/23 and "
            "https://github.com/Jstar269/nakagawa-recomp/pull/20\n",
            encoding="utf-8",
        )
        issues_map = {
            23: {
                "number": 23,
                "is_pr": False,
                "type": "Issue",
                "state": "open",
                "merged_at": None,
                "title": "DMA Issue",
                "url": "",
            },
            20: {
                "number": 20,
                "is_pr": True,
                "type": "PR",
                "state": "closed",
                "merged_at": "2026-08-11T00:00:00Z",
                "title": "SAS PR",
                "url": "",
            },
        }
        findings = audit_markdown_files(self.repo_path, issues_map)
        self.assertEqual(len(findings), 2)
        self.assertTrue(all(finding[4] for finding in findings))

    def test_type_mismatch_fails(self) -> None:
        doc = self.repo_path / "README.md"
        doc.write_text(
            "See https://github.com/Jstar269/nakagawa-recomp/issues/20\n",
            encoding="utf-8",
        )
        issues_map = {
            20: {
                "number": 20,
                "is_pr": True,
                "type": "PR",
                "state": "closed",
                "merged_at": "2026-08-11T00:00:00Z",
                "title": "SAS PR",
                "url": "",
            }
        }
        findings = audit_markdown_files(self.repo_path, issues_map)
        self.assertEqual(len(findings), 1)
        self.assertFalse(findings[0][4])
        self.assertIn("TYPE MISMATCH", findings[0][3])

    def test_missing_number_fails(self) -> None:
        doc = self.repo_path / "README.md"
        doc.write_text(
            "See https://github.com/Jstar269/nakagawa-recomp/issues/999\n",
            encoding="utf-8",
        )
        findings = audit_markdown_files(self.repo_path, {})
        self.assertEqual(len(findings), 1)
        self.assertFalse(findings[0][4])
        self.assertIn("DEAD / UNRESOLVED PUBLIC REFERENCE #999", findings[0][3])

    def test_reused_live_number_in_historical_doc_fails(self) -> None:
        doc = self.repo_path / "docs" / "HARDWARE_ORACLE.md"
        doc.parent.mkdir()
        doc.write_text(
            "Historical PR https://github.com/Jstar269/nakagawa-recomp/pull/35\n",
            encoding="utf-8",
        )
        issues_map = {
            35: {
                "number": 35,
                "is_pr": True,
                "type": "PR",
                "state": "open",
                "merged_at": None,
                "title": "Unrelated sanitized-era scanner migration",
                "url": "",
            }
        }
        findings = audit_markdown_files(self.repo_path, issues_map)
        self.assertEqual(len(findings), 1)
        self.assertFalse(findings[0][4])
        self.assertIn("HISTORICAL NUMBER COLLISION", findings[0][3])

    def test_shorthand_dead_number_in_current_doc_fails(self) -> None:
        doc = self.repo_path / "NOTICE.md"
        doc.write_text("Blocked on #98\n", encoding="utf-8")
        findings = audit_markdown_files(self.repo_path, {})
        self.assertEqual(len(findings), 1)
        self.assertFalse(findings[0][4])
        self.assertIn("DEAD / UNRESOLVED SHORTHAND", findings[0][3])

    def test_shorthand_ignored_in_fenced_code_and_heading(self) -> None:
        doc = self.repo_path / "README.md"
        doc.write_text(
            "# 98 Example heading\n\n"
            "```text\n"
            "#98 fixture token\n"
            "```\n",
            encoding="utf-8",
        )
        findings = audit_markdown_files(self.repo_path, {})
        self.assertEqual(findings, [])

    def test_external_github_tracker_url_does_not_become_local_shorthand(self) -> None:
        doc = self.repo_path / "assets" / "README.md"
        doc.parent.mkdir()
        doc.write_text(
            "See [#16946](https://github.com/hrydgard/ppsspp/issues/16946)\n",
            encoding="utf-8",
        )
        findings = audit_markdown_files(self.repo_path, {})
        self.assertEqual(findings, [])

    def _status(self, body: str) -> None:
        doc = self.repo_path / "docs" / "STATUS.md"
        doc.parent.mkdir(exist_ok=True)
        doc.write_text("# Status\n\n" + body, encoding="utf-8")

    def test_live_work_area_citing_a_closed_issue_as_remaining_work_fails(self) -> None:
        # Regression: the status summary used to present work as open while pointing at #38
        # after its fix had merged; its live-work bullets now carry that rule.
        self._status(
            "## Live work areas\n\n"
            "- Unified PSP clocks, waits, and interrupt delivery remain source-owned\n"
            "  runtime work ([#38](https://github.com/Jstar269/nakagawa-recomp/issues/38)).\n"
            "- Exotic VFPU results remain runtime work (#40).\n"
        )
        issues_map = {38: self._tracker(38, "closed"), 40: self._tracker(40, "open")}
        stale = [f for f in audit_markdown_files(self.repo_path, issues_map) if not f[4]]
        self.assertEqual([(f[0], f[1], f[2]) for f in stale], [("docs/STATUS.md", 5, "#38")])
        self.assertIn("STALE LIVE WORK POINTER", stale[0][3])
        self.assertIn("CLOSED ISSUE", stale[0][3])

    def test_finished_work_cited_in_a_live_work_area_passes(self) -> None:
        # A bullet may cite the merged PR that did part of the work, outside a clause that
        # says work remains; and a closed item outside the section is not this rule's.
        self._status(
            "## Live work areas\n\n"
            "- The PGF reader was retired. The public reader is project-authored\n"
            "  ([#474](https://github.com/Jstar269/nakagawa-recomp/pull/474)).\n\n"
            "## Records of past closures\n\n"
            "- Timing work remains documented in #38.\n"
        )
        issues_map = {474: self._tracker(474, "closed", is_pr=True, merged=True),
                      38: self._tracker(38, "closed")}
        findings = audit_markdown_files(self.repo_path, issues_map)
        self.assertTrue(all(finding[4] for finding in findings), findings)

    def test_status_without_an_audited_section_fails_by_name(self) -> None:
        # The At-a-glance and tracker-label checks once ran on nothing after the summary's
        # layout changed, and the audit passed; a missing section is now a finding.
        self._status(
            "## At a glance\n\n"
            "| Priority | State | Public work item |\n"
            "| --- | --- | --- |\n"
            "| P1 | Open | [issue #38](https://github.com/Jstar269/nakagawa-recomp/issues/38) |\n"
        )
        findings = audit_markdown_files(self.repo_path, {38: self._tracker(38, "closed")})
        missing = [f for f in findings if not f[4] and "MISSING AUDITED SECTION" in f[3]]
        self.assertEqual([(f[0], f[2]) for f in missing], [("docs/STATUS.md", "## Live work areas")])

    def test_the_real_status_summary_has_every_audited_section(self) -> None:
        text = (ROOT / "docs" / "STATUS.md").read_text(encoding="utf-8")
        self.assertEqual([f for f in status_findings(text, {}) if not f[4]], [])

    def test_copilot_instructions_is_current_facing(self) -> None:
        # copilot-instructions.md carried pre-export shorthand numbers (#20 as an
        # "issue", #32 for the ATRAC bridge) that misresolved to unrelated public
        # objects. It is developer-facing, so its shorthand references must now be
        # liveness-audited like the other current-facing docs.
        doc = self.repo_path / ".github" / "copilot-instructions.md"
        doc.parent.mkdir()
        doc.write_text("See GitHub issue #999\n", encoding="utf-8")
        findings = audit_markdown_files(self.repo_path, {})
        self.assertEqual(len(findings), 1)
        self.assertFalse(findings[0][4])
        self.assertIn("DEAD / UNRESOLVED SHORTHAND", findings[0][3])

    def test_dated_records_are_historical_evidence(self) -> None:
        # Dated audit records keep their pre-export tracker numbers as plain
        # historical evidence; a 404 on the public tracker must be reported as
        # preserved historical evidence, not as a dead current reference.
        cases = (
            "docs/archive/ISSUE196_DIRECT_XB.md",
            "docs/provenance/MODIFIED_FILE_NOTICES.md",
        )
        for index, rel in enumerate(cases):
            with self.subTest(rel=rel):
                root = self.repo_path / f"case{index}"
                doc = root / rel
                doc.parent.mkdir(parents=True, exist_ok=True)
                doc.write_text(
                    "See https://github.com/Jstar269/nakagawa-recomp/issues/999\n",
                    encoding="utf-8",
                )
                findings = audit_markdown_files(root, {})
                self.assertEqual(len(findings), 1)
                self.assertTrue(findings[0][4])
                self.assertIn("HISTORICAL EVIDENCE REFERENCE", findings[0][3])

    @staticmethod
    def _tracker(number: int, state: str, *, is_pr: bool = False, merged: bool = False) -> dict:
        return {
            "number": number,
            "is_pr": is_pr,
            "type": "PR" if is_pr else "Issue",
            "state": state,
            "merged_at": "2026-01-01T00:00:00Z" if merged else None,
            "title": f"synthetic {number}",
            "url": "",
        }

    def _stale_in_works(self, findings: list) -> list:
        return [f for f in findings if not f[4] and "STALE IN-THE-WORKS POINTER" in f[3]]

    def test_user_guide_is_current_facing(self) -> None:
        self.assertIn("docs/YOUR_OWN_GAMES.md", CURRENT_FACING_DOCS)

    def test_in_the_works_clause_citing_closed_issue_fails(self) -> None:
        # Regression: the user guide said second-title verification was "in the
        # works" and pointed at the issue that had closed when it shipped.
        doc = self.repo_path / "docs" / "YOUR_OWN_GAMES.md"
        doc.parent.mkdir()
        doc.write_text(
            "- **Game not in the list:** imported as Experimental. Second-title work is in the works\n"
            "  ([#85](https://github.com/Jstar269/nakagawa-recomp/issues/85)) and intake too (#86).\n",
            encoding="utf-8",
        )
        issues_map = {85: self._tracker(85, "closed"), 86: self._tracker(86, "open")}
        stale = self._stale_in_works(audit_markdown_files(self.repo_path, issues_map))
        self.assertEqual([(f[0], f[1], f[2]) for f in stale], [("docs/YOUR_OWN_GAMES.md", 1, "#85")])
        self.assertIn("CLOSED ISSUE", stale[0][3])

    def test_merged_pr_in_an_in_the_works_clause_fails(self) -> None:
        (self.repo_path / "README.md").write_text(
            "Linux support is **in the works** (#90).\n", encoding="utf-8"
        )
        issues_map = {90: self._tracker(90, "closed", is_pr=True, merged=True)}
        stale = self._stale_in_works(audit_markdown_files(self.repo_path, issues_map))
        self.assertEqual(len(stale), 1)
        self.assertIn("MERGED PR", stale[0][3])

    def test_finished_work_cited_outside_the_in_the_works_clause_passes(self) -> None:
        (self.repo_path / "README.md").write_text(
            "- Fonts import from your own PSP\n"
            "  ([#30](https://github.com/Jstar269/nakagawa-recomp/issues/30)); a converter is\n"
            "  in the works ([#31](https://github.com/Jstar269/nakagawa-recomp/issues/31)).\n"
            "- A second title plays a stage (#32). Full games are in the works (#31).\n"
            "\n"
            "```text\n"
            "in the works (#30)\n"
            "```\n",
            encoding="utf-8",
        )
        issues_map = {
            30: self._tracker(30, "closed"),
            31: self._tracker(31, "open"),
            32: self._tracker(32, "closed", is_pr=True, merged=True),
        }
        findings = audit_markdown_files(self.repo_path, issues_map)
        self.assertTrue(all(finding[4] for finding in findings), findings)

    def test_tracking_column_citing_closed_issue_fails(self) -> None:
        (self.repo_path / "README.md").write_text(
            "## What is still in the works\n\n"
            "| Area | Today | Tracking |\n"
            "| --- | --- | --- |\n"
            "| Fonts | Import works (PR #40). | [#41](https://github.com/Jstar269/nakagawa-recomp/issues/41) |\n"
            "| Titles | Experimental import. | [#42](https://github.com/Jstar269/nakagawa-recomp/issues/42) |\n",
            encoding="utf-8",
        )
        issues_map = {
            40: self._tracker(40, "closed", is_pr=True, merged=True),
            41: self._tracker(41, "closed"),
            42: self._tracker(42, "open"),
        }
        stale = self._stale_in_works(audit_markdown_files(self.repo_path, issues_map))
        self.assertEqual([(f[1], f[2]) for f in stale], [(5, "#41")])
        self.assertIn("Tracking column", stale[0][3])

    def test_in_the_works_rule_skips_documents_that_are_not_current_facing(self) -> None:
        doc = self.repo_path / "docs" / "PORTING_NOTES.md"
        doc.parent.mkdir()
        doc.write_text("Root binding is in the works (#50).\n", encoding="utf-8")
        findings = audit_markdown_files(self.repo_path, {50: self._tracker(50, "closed")})
        self.assertEqual(self._stale_in_works(findings), [])

    def test_pagination_beyond_100(self) -> None:
        page1 = [
            {
                "number": i,
                "title": f"Issue {i}",
                "state": "open",
                "html_url": f"https://github.com/issues/{i}",
            }
            for i in range(1, 101)
        ]
        page2 = [
            {
                "number": 101,
                "title": "Issue 101",
                "state": "open",
                "html_url": "https://github.com/issues/101",
            }
        ]

        resp1 = MagicMock()
        resp1.read.return_value = json.dumps(page1).encode("utf-8")
        resp1.__enter__.return_value = resp1
        resp2 = MagicMock()
        resp2.read.return_value = json.dumps(page2).encode("utf-8")
        resp2.__enter__.return_value = resp2

        with patch("urllib.request.urlopen", side_effect=[resp1, resp2]):
            issues_map = fetch_public_issues_map()
        self.assertIsNotNone(issues_map)
        assert issues_map is not None
        self.assertEqual(len(issues_map), 101)
        self.assertIn(101, issues_map)

    def test_fetch_follows_the_link_header_cursor(self) -> None:
        # GitHub answers page=11 and beyond with HTTP 422 for unauthenticated requests, so the
        # fetch must follow the rel="next" cursor instead of counting pages.
        cursor = "https://api.github.com/repositories/1/issues?state=all&per_page=100&after=CUR"
        first = MagicMock()
        first.read.return_value = json.dumps(
            [{"number": n, "title": f"Issue {n}", "state": "open", "html_url": f"https://x/{n}"}
             for n in range(1, 101)]
        ).encode("utf-8")
        first.headers = {"Link": f'<{cursor}>; rel="next", <https://api.github.com/x?page=1>; rel="first"'}
        first.__enter__.return_value = first
        last = MagicMock()
        last.read.return_value = json.dumps(
            [{"number": 101, "title": "Issue 101", "state": "closed", "html_url": "https://x/101"}]
        ).encode("utf-8")
        last.headers = {}
        last.__enter__.return_value = last

        with patch("urllib.request.urlopen", side_effect=[first, last]) as opener:
            issues_map = fetch_public_issues_map()
        assert issues_map is not None
        self.assertEqual(len(issues_map), 101)
        self.assertEqual(opener.call_args_list[1].args[0].full_url, cursor)

    def test_recursive_fallback_includes_markdown_outside_docs(self) -> None:
        nested = self.repo_path / "interface" / "README.md"
        nested.parent.mkdir()
        nested.write_text("# Interface\n", encoding="utf-8")
        with patch("tools.audit_public_issue_links.subprocess.run", side_effect=OSError("git missing")):
            files = get_tracked_markdown_files(self.repo_path)
        self.assertIn(nested, files)

    def test_network_failure_optional_vs_strict(self) -> None:
        with patch("tools.audit_public_issue_links.fetch_public_issues_map", return_value=None):
            with patch(
                "sys.argv",
                ["audit_public_issue_links.py", "--repo-root", str(self.repo_path)],
            ):
                self.assertEqual(main(), 0)
            with patch(
                "sys.argv",
                [
                    "audit_public_issue_links.py",
                    "--repo-root",
                    str(self.repo_path),
                    "--strict",
                ],
            ):
                self.assertEqual(main(), 1)

    def test_same_line_external_tracker_link_does_not_hide_dead_shorthand(self) -> None:
        # Regression: an external tracker link on the same line suppressed our own bare
        # #N shorthand by number, so a dead current-facing reference passed silently.
        (self.repo_path / "README.md").write_text(
            "Ours #999 is blocked; upstream [#999](https://github.com/hrydgard/ppsspp/issues/999).\n",
            encoding="utf-8",
        )
        findings = audit_markdown_files(self.repo_path, {})
        dead = [f for f in findings if not f[4] and "DEAD / UNRESOLVED SHORTHAND" in f[3]]
        self.assertEqual([(f[1], f[2]) for f in dead], [(1, "#999")])

    def test_in_the_works_pointer_beside_external_link_of_same_number_fails(self) -> None:
        (self.repo_path / "README.md").write_text(
            "The fix is in the works (#38), upstream [#38](https://github.com/hrydgard/ppsspp/issues/38).\n",
            encoding="utf-8",
        )
        stale = self._stale_in_works(
            audit_markdown_files(self.repo_path, {38: self._tracker(38, "closed")})
        )
        self.assertEqual([(f[1], f[2]) for f in stale], [(1, "#38")])

    def test_malformed_tracker_url_tails_fail_instead_of_resolving_to_the_number(self) -> None:
        # GitHub redirects issues/N-suffix to an unrelated filtered list and returns 404 for
        # issues/N/suffix, so neither tail is a live reference to issue or PR N.
        repo = "https://github.com/Jstar269/nakagawa-recomp"
        for text in (
            f"See {repo}/issues/23-extra\n",
            f"See {repo}/issues/23/extra\n",
            f"See {repo}/pull/23-extra\n",
        ):
            with self.subTest(text=text):
                (self.repo_path / "README.md").write_text(text, encoding="utf-8")
                findings = audit_markdown_files(self.repo_path, {23: self._tracker(23, "open")})
                self.assertTrue(findings)
                self.assertTrue(all(not f[4] for f in findings), findings)
                self.assertTrue(any("MALFORMED PUBLIC TRACKER URL" in f[3] for f in findings), findings)

    def test_repository_links_are_audited_in_any_letter_case(self) -> None:
        # GitHub resolves the owner and repository name in any letter case, so a link that spells the repository
        # Jstar269/Nakagawa-Recomp names this tracker as surely as the lowercase spelling and is audited the same way.
        for repo in ("https://github.com/Jstar269/Nakagawa-Recomp", "https://github.com/JSTAR269/nakagawa-RECOMP"):
            with self.subTest(repo=repo):
                (self.repo_path / "README.md").write_text(
                    f"See {repo}/issues/23, {repo}/issues/999 and {repo}/issues/23-extra\n", encoding="utf-8"
                )
                findings = audit_markdown_files(self.repo_path, {23: self._tracker(23, "open")})
                self.assertEqual([f[2] for f in findings if f[4]], [f"{repo}/issues/23"], findings)
                failed = sorted(f[3].split(":")[0] for f in findings if not f[4])
                self.assertEqual(
                    failed, ["DEAD / UNRESOLVED PUBLIC REFERENCE #999", "MALFORMED PUBLIC TRACKER URL"], findings
                )
        # Only the letter case is free: another repository or owner is still not this tracker.
        (self.repo_path / "README.md").write_text(
            "See https://github.com/Jstar269/Nakagawa-Recomp-fork/issues/999 and "
            "https://github.com/someone-else/Nakagawa-Recomp/issues/999\n",
            encoding="utf-8",
        )
        self.assertEqual(audit_markdown_files(self.repo_path, {}), [])

    def test_canonical_query_fragment_punctuation_and_pull_subroutes_stay_live(self) -> None:
        repo = "https://github.com/Jstar269/nakagawa-recomp"
        (self.repo_path / "README.md").write_text(
            f"See {repo}/issues/23 and {repo}/issues/23?utm=x and {repo}/issues/23#issuecomment-1.\n"
            f"Also ({repo}/issues/24), {repo}/pull/20/files, and {repo}/pull/20/\n"
            "Other trackers: https://github.com/Jstar269/nakagawa-recomp-fork/issues/999 "
            "and https://github.com/hrydgard/ppsspp/issues/999.\n",
            encoding="utf-8",
        )
        issues_map = {
            23: self._tracker(23, "open"),
            24: self._tracker(24, "closed"),
            20: self._tracker(20, "closed", is_pr=True, merged=True),
        }
        findings = audit_markdown_files(self.repo_path, issues_map)
        self.assertEqual(len(findings), 6, findings)
        self.assertTrue(all(f[4] for f in findings), findings)


if __name__ == "__main__":
    unittest.main()
