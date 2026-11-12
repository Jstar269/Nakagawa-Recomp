# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Canary tests for the Trusted CI verdict (#373, #364).

Each attack from #373 is replayed as synthetic GitHub API payloads, shaped like
the real ``actions/runs``, ``runs/{id}/jobs``, ``commits/{sha}/check-runs`` and
``issues/{n}/events`` answers, and must turn the verdict red. No request
leaves the process and no real pull request is opened.
"""

from __future__ import annotations

import ast
import copy
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import urllib.error
from typing import Any

try:
    import ci_trust_verdict as ctv
except ModuleNotFoundError:  # Running as ``python -m unittest tools.test_ci_trust_verdict``.
    from tools import ci_trust_verdict as ctv

ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS = ROOT / ".github" / "workflows"
HEAD = "a" * 40
OTHER = "b" * 40
PR = 7
CI = ".github/workflows/ci.yml"
OSV = ".github/workflows/osv-scanner.yml"
DEP = ".github/workflows/dependency-review.yml"
PROV = ".github/workflows/provenance-attestation.yml"
PUSHED = "2026-10-28T10:00:00Z"
MAIN = "c" * 40                       # the default branch's tip: what a workflow_run or dispatched run reports
D1 = "1" * 64
D2 = "2" * 64
LABELLED = "2026-10-28T10:30:00Z"     # Scenario.label's default
RECORDED = "2026-10-28T10:31:00Z"     # after it: when Scenario.record's job completed
PROVENANCE = next(producer for producer in ctv.PRODUCERS if producer.workflow == PROV)
NAMED_TITLE = "Provenance attestation of {head}"   # the attestation workflow's run-name; see WorkflowWiringTests


def _run(run_id: int, path: str, event: str = "pull_request", *, sha: str = HEAD, created: str = PUSHED,
         status: str = "completed", conclusion: str | None = "success", prs: tuple[int, ...] = (PR,),
         branch: str | None = None, title: str | None = None, actor: str | None = None,
         started: str | None = None) -> dict:
    run = {
        "id": run_id, "path": path, "event": event, "head_sha": sha, "created_at": created,
        "status": status, "conclusion": conclusion, "check_suite_id": run_id * 10,
        "pull_requests": [{"number": number, "base": {"ref": "main"}} for number in prs],
        "head_branch": branch, "display_title": title,
    }
    if actor is not None:
        run["actor"] = run["triggering_actor"] = {"login": actor}
    if started is not None:
        run["run_started_at"] = started     # when its latest attempt started: later than created for a re-run
    return run


def _job(job_id: int, name: str, *, sha: str = HEAD, status: str = "completed",
         conclusion: str | None = "success", completed: str | None = RECORDED) -> dict:
    return {"id": job_id, "name": name, "head_sha": sha, "status": status, "conclusion": conclusion,
            "completed_at": completed if status == "completed" else None}


def _check(job: dict, suite: int, *, app: int = ctv.GITHUB_ACTIONS_APP_ID) -> dict:
    return {
        "id": job["id"], "name": job["name"], "head_sha": job["head_sha"], "status": job["status"],
        "conclusion": job["conclusion"], "app": {"id": app}, "check_suite": {"id": suite},
    }


def _pull_request(*, head: str = HEAD, base: str = "main", fork: bool = False,
                  labels: tuple[str, ...] = (), draft: bool = False) -> dict:
    owner = "someone/nakagawa-recomp" if fork else "Jstar269/nakagawa-recomp"
    return {
        "number": PR, "state": "open", "draft": draft,
        "head": {"sha": head, "ref": "ci/topic", "repo": {"full_name": owner}},
        "base": {"ref": base, "repo": {"full_name": "Jstar269/nakagawa-recomp"}},
        "labels": [{"name": name} for name in labels],
    }


class Scenario:
    """A healthy pull-request head; each canary breaks one thing."""

    def __init__(self, *, fork: bool = False) -> None:
        self.raw: dict = {
            "head_sha": HEAD,
            "default_branch": "main",
            "trusted_sha": "c" * 40,
            "fetched_head": HEAD,
            "pull_request": _pull_request(fork=fork),
            "open_pull_requests": [],
            "label_events": [],
            "workflow_runs": [],
            "jobs": {},
            "check_runs": [],
            "statuses": [],
            "protected_changes": [],
            "context_collisions": [],
        }
        self.raw["open_pull_requests"].append(copy.deepcopy(self.raw["pull_request"]))
        self.add_run(1, CI, ["Classify change", "Hygiene and security", "CI required"])
        self.add_run(2, OSV, ["OSV Vulnerability Scan"])
        self.add_run(3, DEP, ["dependency-review"])
        self.add_run(4, PROV, ["Trusted provenance attestation"], event="pull_request_target")
        self.add_run(5, ctv.VERDICT_WORKFLOW, ["Compute trusted CI verdict"], event="pull_request_target")

    def add_run(self, run_id: int, path: str, job_names: list[str], **run_args) -> dict:
        run = _run(run_id, path, **run_args)
        self.raw["workflow_runs"].append(run)
        jobs = [_job(run_id * 100 + index, name, sha=run["head_sha"]) for index, name in enumerate(job_names)]
        self.raw["jobs"][str(run_id)] = jobs
        self.raw["check_runs"] += [_check(job, run["check_suite_id"]) for job in jobs]
        return run

    def job(self, run_id: int, name: str) -> dict:
        return next(job for job in self.raw["jobs"][str(run_id)] if job["name"] == name)

    def drop_runs(self, path: str) -> None:
        """Remove every run of one workflow, with its check runs: the workflow never ran at this head."""
        suites = {run["check_suite_id"] for run in self.raw["workflow_runs"] if run["path"] == path}
        self.raw["workflow_runs"] = [run for run in self.raw["workflow_runs"] if run["path"] != path]
        self.raw["check_runs"] = [check for check in self.raw["check_runs"]
                                  if check["check_suite"]["id"] not in suites]

    def add_named(self, run_id: int, *, event: str = "workflow_run", head: str = HEAD, branch: str = "main",
                  trusted: bool = True, job_names: tuple[str, ...] = (PROVENANCE.context,), **run_args) -> dict:
        """An attestation run of the second route: listed under the default branch and titled for ``head``.

        GitHub attaches its check runs to the default branch's commit, so none is added at the pull request head.
        """
        run = _run(run_id, PROV, event, sha=MAIN, prs=(), branch=branch,
                   title=NAMED_TITLE.format(head=head), **run_args)
        run["default_definition"] = trusted
        self.raw["workflow_runs"].append(run)
        self.raw["jobs"][str(run_id)] = [_job(run_id * 100 + index, name, sha=MAIN)
                                         for index, name in enumerate(job_names)]
        return run

    def change(self, path: str, kind: str = "modified") -> None:
        self.raw["protected_changes"].append({"path": path, "kind": kind})

    def record(self, digest: str = D1, *, at: str = RECORDED, route: str = "dispatch", head: str = HEAD,
               head_digest: str | None = D1) -> None:
        """An approval record as the collector reads it, and this head's own digest."""
        self.raw["head_digest"] = head_digest
        self.raw.setdefault("approval_records", []).append(
            {"digest": digest, "recorded_at": at, "run_created_at": at, "route": route, "head_sha": head,
             "actor": "maintainer"})

    def label(self, *, at: str = LABELLED, permission: str = "admin",
              event: str = "labeled", actor: str = "maintainer") -> None:
        self.raw["pull_request"]["labels"] = (
            [{"name": ctv.APPROVAL_LABEL}] if event == "labeled" else []
        )
        self.raw["label_events"].append({
            "id": len(self.raw["label_events"]) + 1, "event": event, "created_at": at,
            "actor": {"login": actor}, "label": {"name": ctv.APPROVAL_LABEL},
        })
        self.raw["label_actor_permission"] = permission

    def verdict(self) -> ctv.Verdict:
        return ctv.evaluate(ctv.normalise(self.raw))


class VerdictAssertions(unittest.TestCase):
    def assertVerdict(self, scenario: Scenario, state: str, *codes: str) -> ctv.Verdict:
        verdict = scenario.verdict()
        fatal = {item.code for item in verdict.findings if item.fatal}
        self.assertEqual(verdict.state, state, verdict.findings)
        for code in codes:
            self.assertIn(code, fatal, verdict.findings)
        if state == "success":
            self.assertFalse(fatal)
        self.assertLessEqual(len(verdict.description()), 140)
        return verdict


class HealthyHeadTests(VerdictAssertions):
    def test_every_intended_job_from_its_own_workflow_passes(self) -> None:
        self.assertVerdict(Scenario(), "success")

    def test_a_running_job_is_pending_not_failed(self) -> None:
        scenario = Scenario()
        scenario.job(1, "CI required").update(status="in_progress", conclusion=None)
        verdict = self.assertVerdict(scenario, "pending")
        self.assertEqual(verdict.pending, ["CI required"])

    def test_a_workflow_that_has_not_started_is_pending(self) -> None:
        scenario = Scenario()
        scenario.raw["workflow_runs"] = [run for run in scenario.raw["workflow_runs"] if run["path"] != OSV]
        scenario.raw["check_runs"] = [check for check in scenario.raw["check_runs"]
                                      if check["check_suite"]["id"] != 20]
        verdict = self.assertVerdict(scenario, "pending")
        self.assertIn("OSV Vulnerability Scan", verdict.description())

    def test_an_unfinished_run_without_the_job_yet_is_pending(self) -> None:
        scenario = Scenario()
        scenario.raw["workflow_runs"][0].update(status="in_progress", conclusion=None)
        scenario.raw["jobs"]["1"] = [job for job in scenario.raw["jobs"]["1"] if job["name"] != "CI required"]
        self.assertVerdict(scenario, "pending")

    def test_a_rerun_after_a_cancelled_run_counts(self) -> None:
        scenario = Scenario()
        scenario.raw["workflow_runs"][0]["created_at"] = "2026-10-28T09:00:00Z"
        scenario.add_run(9, CI, ["Hygiene and security", "CI required"],
                         created="2026-10-28T09:30:00Z", conclusion="cancelled")
        self.assertVerdict(scenario, "success")


class SameNameJobCanaryTests(VerdictAssertions):
    """Canary: a candidate adds or edits a workflow whose job is named like a required context."""

    def test_added_workflow_with_a_required_job_name_is_foreign(self) -> None:
        scenario = Scenario()
        scenario.add_run(6, ".github/workflows/evil.yml", ["CI required"])
        scenario.change(".github/workflows/evil.yml", "added")
        self.assertVerdict(scenario, "failure", "FOREIGN_PRODUCER", "CI_CHANGE_UNAPPROVED")

    def test_an_approved_workflow_change_still_cannot_produce_a_required_context(self) -> None:
        scenario = Scenario()
        scenario.add_run(6, ".github/workflows/evil.yml", ["Hygiene and security"])
        scenario.change(".github/workflows/evil.yml", "added")
        scenario.label()
        scenario.record()
        verdict = self.assertVerdict(scenario, "failure", "FOREIGN_PRODUCER")
        self.assertNotIn("CI_CHANGE_UNAPPROVED", {item.code for item in verdict.findings})

    def test_check_run_from_another_app_is_foreign(self) -> None:
        scenario = Scenario()
        spoof = _check(_job(777, "dependency-review"), 9999, app=4242)
        scenario.raw["check_runs"].append(spoof)
        self.assertVerdict(scenario, "failure", "FOREIGN_PRODUCER")

    def test_provenance_context_from_a_pull_request_run_is_foreign(self) -> None:
        # The provenance file gains a pull_request trigger: its run is then
        # candidate-defined, so the context must not count from it.
        scenario = Scenario()
        scenario.add_run(6, PROV, ["Trusted provenance attestation"], event="pull_request")
        self.assertVerdict(scenario, "failure", "FOREIGN_PRODUCER")


class DeletedJobCanaryTests(VerdictAssertions):
    """Canary: the candidate's ci.yml drops a required job."""

    def test_missing_job_fails(self) -> None:
        scenario = Scenario()
        scenario.raw["jobs"]["1"] = [job for job in scenario.raw["jobs"]["1"]
                                     if job["name"] != "Hygiene and security"]
        scenario.change(CI)
        self.assertVerdict(scenario, "failure", "JOB_MISSING", "CI_CHANGE_UNAPPROVED")

    def test_a_label_does_not_excuse_a_missing_job(self) -> None:
        scenario = Scenario()
        scenario.raw["jobs"]["1"] = [job for job in scenario.raw["jobs"]["1"] if job["name"] != "CI required"]
        scenario.change(CI)
        scenario.label()
        self.assertVerdict(scenario, "failure", "JOB_MISSING")

    def test_deleted_workflow_fails(self) -> None:
        scenario = Scenario()
        scenario.raw["workflow_runs"] = [run for run in scenario.raw["workflow_runs"] if run["path"] != DEP]
        scenario.raw["check_runs"] = [check for check in scenario.raw["check_runs"]
                                      if check["check_suite"]["id"] != 30]
        scenario.change(DEP, "deleted")
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")


class DuplicateContextCanaryTests(VerdictAssertions):
    def test_two_jobs_under_one_required_name_fail(self) -> None:
        scenario = Scenario()
        extra = _job(150, "CI required")
        scenario.raw["jobs"]["1"].append(extra)
        scenario.raw["check_runs"].append(_check(extra, 10))
        self.assertVerdict(scenario, "failure", "DUPLICATE_CONTEXT")

    def test_commit_status_under_a_required_name_fails(self) -> None:
        scenario = Scenario()
        scenario.raw["statuses"].append({"context": "CI required", "state": "success"})
        self.assertVerdict(scenario, "failure", "FOREIGN_PRODUCER")

    def test_check_run_named_like_the_verdict_fails(self) -> None:
        scenario = Scenario()
        scenario.raw["check_runs"].append(_check(_job(888, ctv.VERDICT_CONTEXT), 50))
        self.assertVerdict(scenario, "failure", "CI_CONTEXT_COLLISION")

    def test_candidate_workflow_naming_the_verdict_fails_even_when_approved(self) -> None:
        scenario = Scenario()
        scenario.raw["context_collisions"] = [".github/workflows/ci.yml"]
        scenario.change(CI)
        scenario.label()
        self.assertVerdict(scenario, "failure", "CI_CONTEXT_COLLISION")


class ExitZeroSwapCanaryTests(VerdictAssertions):
    """Canary: failing source paired with a candidate gate that returns success."""

    def test_workflow_body_swapped_for_exit_zero_fails_despite_green_jobs(self) -> None:
        scenario = Scenario()  # every job reports success, because the body is `exit 0`
        scenario.change(CI)
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")

    def test_aggregate_tool_swapped_for_exit_zero_fails(self) -> None:
        scenario = Scenario()
        scenario.change("tools/ci_required.py")
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")

    def test_honest_failure_stays_red(self) -> None:
        scenario = Scenario()
        scenario.job(1, "CI required").update(conclusion="failure")
        self.assertVerdict(scenario, "failure", "JOB_FAILED")

    def test_skipped_required_job_is_not_success(self) -> None:
        scenario = Scenario()
        scenario.job(2, "OSV Vulnerability Scan").update(conclusion="skipped")
        self.assertVerdict(scenario, "failure", "JOB_FAILED")


class StaleHeadCanaryTests(VerdictAssertions):
    def test_result_for_a_superseded_head_fails(self) -> None:
        scenario = Scenario()
        scenario.raw["pull_request"]["head"]["sha"] = OTHER
        self.assertVerdict(scenario, "failure", "STALE_HEAD")

    def test_head_moving_while_it_is_read_fails(self) -> None:
        scenario = Scenario()
        scenario.raw["fetched_head"] = OTHER
        self.assertVerdict(scenario, "failure", "STALE_HEAD")

    def test_job_for_another_sha_is_a_replay(self) -> None:
        scenario = Scenario()
        scenario.job(3, "dependency-review")["head_sha"] = OTHER
        self.assertVerdict(scenario, "failure", "RUN_HEAD_MISMATCH")

    def test_run_for_another_sha_is_a_replay(self) -> None:
        scenario = Scenario()
        scenario.add_run(6, OSV, ["OSV Vulnerability Scan"], sha=OTHER, created="2026-10-28T11:00:00Z")
        self.assertVerdict(scenario, "failure", "RUN_HEAD_MISMATCH")

    def test_older_success_cannot_mask_the_latest_failure(self) -> None:
        scenario = Scenario()
        scenario.add_run(6, CI, ["Hygiene and security", "CI required"], created="2026-10-28T11:00:00Z")
        scenario.job(6, "CI required").update(conclusion="failure")
        self.assertVerdict(scenario, "failure", "JOB_FAILED")

    def test_run_belonging_to_another_pull_request_fails(self) -> None:
        scenario = Scenario()
        scenario.add_run(6, CI, ["Hygiene and security", "CI required"],
                         created="2026-10-28T11:00:00Z", prs=(99,))
        self.assertVerdict(scenario, "failure", "RUN_FOR_OTHER_PR")

    def test_same_head_open_against_another_base_fails(self) -> None:
        scenario = Scenario()
        twin = _pull_request(base="old-branch")
        twin["number"] = 99
        scenario.raw["open_pull_requests"].append(twin)
        self.assertVerdict(scenario, "failure", "AMBIGUOUS_HEAD")

    def test_definitions_behind_main_fail_even_with_a_label(self) -> None:
        scenario = Scenario()
        scenario.change(CI, "behind")
        scenario.label()
        verdict = self.assertVerdict(scenario, "failure", "CI_DEFINITION_BEHIND")
        # The integrator merges main at landing (AGENTS.md section 10); the author is not asked to.
        (message,) = [item.message for item in verdict.findings if item.code == "CI_DEFINITION_BEHIND"]
        self.assertIn("the integrator merges main when it lands the pull request, not the author", message)
        self.assertNotRegex(message, r";\s*merge main$")

    def test_pull_request_into_another_branch_gets_no_pass(self) -> None:
        scenario = Scenario()
        scenario.raw["pull_request"]["base"]["ref"] = "release"
        self.assertVerdict(scenario, "failure", "BASE_NOT_DEFAULT")

    def test_closed_pull_request_gets_no_pass(self) -> None:
        scenario = Scenario()
        scenario.raw["pull_request"]["state"] = "closed"
        self.assertVerdict(scenario, "failure", "PR_NOT_OPEN")


class ForkCanaryTests(VerdictAssertions):
    """Canary: fork pull requests are judged from the API, the same way, with no candidate code."""

    def test_fork_head_with_honest_runs_passes(self) -> None:
        scenario = Scenario(fork=True)
        for run in scenario.raw["workflow_runs"]:
            run["pull_requests"] = []  # GitHub leaves this empty for fork runs
        verdict = self.assertVerdict(scenario, "success")
        self.assertTrue(ctv.normalise(scenario.raw)["pull_request"]["fork"])
        self.assertEqual(verdict.findings, [])

    def test_fork_workflow_edit_fails_without_a_label(self) -> None:
        scenario = Scenario(fork=True)
        scenario.change(CI)
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")

    def test_label_from_a_user_without_write_access_does_not_approve(self) -> None:
        scenario = Scenario(fork=True)
        scenario.change(CI)
        scenario.label(permission="triage", actor="helper")
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")


class CiToolApprovalCanaryTests(VerdictAssertions):
    """Canary: a CI tool edit needs the maintainer label and a record of this head's protected content."""

    def test_ci_tool_edit_without_label_fails(self) -> None:
        scenario = Scenario()
        scenario.change("tools/ci_paths.py")
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")

    def test_a_label_with_a_record_of_this_content_written_after_it_approves(self) -> None:
        scenario = Scenario()
        scenario.change("tools/ci_paths.py")
        scenario.label(at="2026-10-28T10:05:00Z")
        scenario.record(at="2026-10-28T10:06:00Z")
        verdict = self.assertVerdict(scenario, "success")
        self.assertEqual([item.code for item in verdict.findings], ["CI_CHANGE_APPROVED"])

    def test_a_label_alone_does_not_approve(self) -> None:
        """Failing-before: a label applied after the head's first run approved the head with no record at all,
        so nothing tied the approval to the bytes that were reviewed."""
        scenario = Scenario()
        scenario.change("tools/ci_paths.py")
        scenario.label(at="2026-10-28T10:05:00Z")
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")

    def test_a_label_newer_than_a_re_pushed_heads_first_run_does_not_approve(self) -> None:
        """A pull request is pushed to HEAD (its runs start at 10:00), then to another commit, which a
        maintainer reviews and labels at 10:30, and then back to HEAD at 10:40. HEAD's protected content was
        never approved.

        Failing-before: the verifier took the earliest pull-request run at HEAD as the head's arrival, found the
        label newer, and approved. The synchronize handler that would have removed the label does not run while
        GitHub blocks pull_request_target, and workflow_run and dispatch verdicts still do."""
        scenario = Scenario()
        scenario.change("tools/ci_paths.py")
        for run_id, path, jobs in ((11, CI, ["Classify change", "Hygiene and security", "CI required"]),
                                   (12, OSV, ["OSV Vulnerability Scan"]), (13, DEP, ["dependency-review"])):
            scenario.add_run(run_id, path, jobs, created="2026-10-28T10:40:00Z")   # the push back to HEAD
        scenario.label(at=LABELLED)
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")

    def test_label_from_an_earlier_head_does_not_approve(self) -> None:
        scenario = Scenario()
        scenario.change("tools/ci_paths.py")
        scenario.label(at="2026-10-28T09:59:00Z")
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")

    def test_removed_label_does_not_approve(self) -> None:
        scenario = Scenario()
        scenario.change("tools/ci_paths.py")
        scenario.label(at="2026-10-28T10:05:00Z")
        scenario.record(at="2026-10-28T10:05:30Z")
        scenario.label(at="2026-10-28T10:06:00Z", event="unlabeled")
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")

    def test_label_name_alone_without_an_event_does_not_approve(self) -> None:
        scenario = Scenario()
        scenario.change(".pre-commit-config.yaml")
        scenario.raw["pull_request"]["labels"] = [{"name": ctv.APPROVAL_LABEL}]
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")


LIGHT_JOBS = ["Classify change", "Hygiene and security", "CI required"]
DRAFT_AT = "2026-10-28T10:20:00Z"
READY_AT = "2026-10-28T10:40:00Z"


class DraftLightCiTests(VerdictAssertions):
    """A Draft runs only the quick CI jobs, so its head is never green (DRAFT_LIGHT_CI).

    The verifier reads the draft state from the pull request itself, so the rule holds
    whatever the candidate's CI reported. A pull request that is not a Draft is judged
    exactly as before.
    """

    @staticmethod
    def _draft() -> Scenario:
        scenario = Scenario()
        scenario.raw["pull_request"]["draft"] = True
        return scenario

    def test_a_draft_with_every_intended_job_green_is_a_failure_naming_the_reason(self) -> None:
        verdict = self.assertVerdict(self._draft(), "failure", "DRAFT_LIGHT_CI")
        self.assertEqual(verdict.description(), "1 finding(s): DRAFT_LIGHT_CI")
        finding = next(item for item in verdict.findings if item.code == "DRAFT_LIGHT_CI")
        self.assertEqual(finding.subject, f"#{PR}")
        self.assertIn("Ready for review", finding.message)

    def test_a_pull_request_that_is_not_a_draft_is_judged_as_before(self) -> None:
        verdict = self.assertVerdict(Scenario(), "success")
        self.assertEqual(verdict.findings, [])

    def test_an_unreadable_draft_state_is_judged_as_a_draft(self) -> None:
        for value in (None, "false", 0, "true"):
            with self.subTest(value=value):
                scenario = Scenario()
                scenario.raw["pull_request"]["draft"] = value
                self.assertVerdict(scenario, "failure", "DRAFT_LIGHT_CI")
        scenario = Scenario()
        del scenario.raw["pull_request"]["draft"]
        self.assertVerdict(scenario, "failure", "DRAFT_LIGHT_CI")

    def test_a_draft_behind_main_is_not_behind_only(self) -> None:
        """The batch tool lands a verdict whose one code is CI_DEFINITION_BEHIND; a Draft's never is."""
        behind = Scenario()
        behind.change(CI, kind="behind")
        self.assertEqual(self.assertVerdict(behind, "failure").description(), "1 finding(s): CI_DEFINITION_BEHIND")
        draft = self._draft()
        draft.change(CI, kind="behind")
        verdict = self.assertVerdict(draft, "failure", "DRAFT_LIGHT_CI", "CI_DEFINITION_BEHIND")
        self.assertEqual(verdict.description(), "2 finding(s): CI_DEFINITION_BEHIND, DRAFT_LIGHT_CI")
        self.assertNotEqual(verdict.description().split(": ", 1)[-1], "CI_DEFINITION_BEHIND")

    def test_an_approved_ci_change_does_not_waive_the_draft_rule(self) -> None:
        scenario = self._draft()
        scenario.change("tools/ci_paths.py")
        scenario.label(at="2026-10-28T10:05:00Z")
        scenario.record(at="2026-10-28T10:06:00Z")
        verdict = self.assertVerdict(scenario, "failure", "DRAFT_LIGHT_CI")
        self.assertEqual(sorted({item.code for item in verdict.findings if item.fatal}), ["DRAFT_LIGHT_CI"])

    def test_ready_for_review_is_judged_on_the_full_run_it_starts(self) -> None:
        # The Draft's light run failed CI required (DRAFT_LIGHT_CI); marking it Ready started the full run.
        scenario = Scenario()
        scenario.raw["workflow_runs"][0]["created_at"] = "2026-10-28T10:00:00Z"
        scenario.job(1, "CI required").update(conclusion="failure")
        full = scenario.add_run(6, CI, LIGHT_JOBS, created=READY_AT, status="in_progress", conclusion=None)
        scenario.job(6, "CI required").update(status="in_progress", conclusion=None)
        verdict = self.assertVerdict(scenario, "pending")
        self.assertEqual(verdict.pending, ["CI required"])
        full.update(status="completed", conclusion="success")
        scenario.job(6, "CI required").update(status="completed", conclusion="success")
        self.assertVerdict(scenario, "success")

    def test_ready_for_review_cancelling_the_light_run_is_judged_on_the_full_run(self) -> None:
        scenario = Scenario()
        scenario.raw["workflow_runs"][0].update(conclusion="cancelled")
        scenario.add_run(6, CI, LIGHT_JOBS, created=READY_AT)
        self.assertVerdict(scenario, "success")

    def test_converted_to_draft_turns_a_green_head_red(self) -> None:
        # The full run passed while it was Ready; converting it back to a Draft started a light run.
        scenario = self._draft()
        scenario.add_run(6, CI, LIGHT_JOBS, created=DRAFT_AT)
        scenario.job(6, "CI required").update(conclusion="failure")
        self.assertVerdict(scenario, "failure", "DRAFT_LIGHT_CI", "JOB_FAILED")

    def test_a_ready_head_whose_newest_ci_run_is_light_is_not_green(self) -> None:
        """Defence in depth: the light run's own CI required fails, so it can never stand for a Ready head."""
        scenario = Scenario()
        scenario.add_run(6, CI, LIGHT_JOBS, created=DRAFT_AT)
        scenario.job(6, "CI required").update(conclusion="failure")
        self.assertVerdict(scenario, "failure", "JOB_FAILED")

    def test_main_posts_the_draft_failure(self) -> None:
        scenario = self._draft()
        gh = FakeGitHub(scenario)
        with mock.patch.object(ctv, "git_facts", return_value=_facts()), \
                mock.patch.object(ctv, "GitHub", return_value=gh), \
                mock.patch.dict(os.environ, {"GITHUB_TOKEN": "token"}), mock.patch("sys.stdout"):
            code = ctv.main(["--repository", gh.repository, "--post", "--pull-request", str(PR)])
        self.assertEqual(code, 1)
        self.assertEqual(gh.posts, [(f"statuses/{HEAD}", {
            "state": "failure", "context": ctv.VERDICT_CONTEXT, "description": "1 finding(s): DRAFT_LIGHT_CI",
        })])


#: Every conclusion GitHub gives a completed job, and two shapes it should never give.
JOB_CONCLUSIONS = ("success", "failure", "cancelled", "skipped", "neutral", "timed_out", "action_required",
                   "stale", "startup_failure", None, "")


class LightTierVerdictTests(VerdictAssertions):
    """A head whose newest CI run was light (the quick checks) is never ``success`` (docs/CI.md,
    "Quick and full checks").

    On a light run ``CI required`` is skipped and ``CI quick checks`` is the run's aggregate. Only a
    ``CI required`` job that concluded ``success`` makes a verdict ``success``; a light run with green quick
    checks is ``pending`` with ``LIGHT_CI_GREEN``, which is what the landing desk selects on.
    """

    @staticmethod
    def _light(quick: str | None = "success", required: str | None = "skipped") -> Scenario:
        """The healthy scenario with its CI run turned into a light one. ``quick`` None omits the quick job."""
        scenario = Scenario()
        scenario.job(1, ctv.FULL_AGGREGATE).update(conclusion=required)
        if quick is not None:
            job = _job(199, ctv.QUICK_AGGREGATE, conclusion=quick)
            scenario.raw["jobs"]["1"].append(job)
            scenario.raw["check_runs"].append(_check(job, 10))
        return scenario

    def test_a_quick_green_head_is_never_success(self) -> None:
        verdict = self.assertVerdict(self._light(), "pending")
        self.assertNotEqual(verdict.state, "success")
        self.assertEqual(verdict.pending, [ctv.LIGHT_PENDING])
        self.assertEqual(verdict.description(), ctv.LIGHT_DESCRIPTION)
        self.assertTrue(verdict.description().startswith("LIGHT_CI_GREEN: "))
        self.assertEqual([(item.code, item.fatal) for item in verdict.findings], [("LIGHT_CI_GREEN", False)])
        self.assertIn("ci-full-matrix", verdict.findings[0].message)

    def test_success_requires_ci_required_success(self) -> None:
        """The invariant, at the verdict: whatever else a CI run holds, no conclusion of ``CI required``
        but ``success`` gives ``success``."""
        for required in JOB_CONCLUSIONS:
            for quick in (None, *JOB_CONCLUSIONS):
                with self.subTest(required=required, quick=quick):
                    state = self._light(quick=quick, required=required).verdict().state
                    self.assertEqual(state == "success", required == "success")
        absent = Scenario()
        absent.raw["jobs"]["1"] = [job for job in absent.raw["jobs"]["1"] if job["name"] != ctv.FULL_AGGREGATE]
        absent.raw["jobs"]["1"].append(_job(199, ctv.QUICK_AGGREGATE))
        self.assertVerdict(absent, "failure", "JOB_MISSING")
        running = self._light()
        running.job(1, ctv.FULL_AGGREGATE).update(status="in_progress", conclusion=None)
        self.assertEqual(self.assertVerdict(running, "pending").pending, [ctv.FULL_AGGREGATE])

    def test_only_the_recognised_light_shape_is_light(self) -> None:
        """Default deny: any other way for ``CI required`` not to succeed stays a failure."""
        for required in JOB_CONCLUSIONS:
            if required in ("success", "skipped"):
                continue
            with self.subTest(required=required):
                verdict = self.assertVerdict(self._light(required=required), "failure", "JOB_FAILED")
                self.assertNotIn("LIGHT_CI_GREEN", verdict.description())
        # Skipped, but without exactly one quick aggregate of this head in the same run.
        self.assertVerdict(self._light(quick=None), "failure", "JOB_FAILED")
        twice = self._light()
        twice.raw["jobs"]["1"].append(_job(198, ctv.QUICK_AGGREGATE))
        self.assertVerdict(twice, "failure", "JOB_FAILED")
        other_head = self._light()
        other_head.job(1, ctv.QUICK_AGGREGATE).update(head_sha=OTHER)
        self.assertVerdict(other_head, "failure", "JOB_FAILED")

    def test_a_failed_quick_aggregate_is_a_failure_naming_it(self) -> None:
        for quick in ("failure", "cancelled", "skipped", "timed_out", ""):
            with self.subTest(quick=quick):
                verdict = self.assertVerdict(self._light(quick=quick), "failure", "JOB_FAILED")
                failed = [item for item in verdict.findings if item.code == "JOB_FAILED"]
                self.assertEqual([item.subject for item in failed], [ctv.QUICK_AGGREGATE])
                self.assertEqual(verdict.description(), "1 finding(s): JOB_FAILED")

    def test_a_running_quick_aggregate_is_pending_under_its_own_name(self) -> None:
        scenario = self._light()
        scenario.job(1, ctv.QUICK_AGGREGATE).update(status="in_progress", conclusion=None)
        verdict = self.assertVerdict(scenario, "pending")
        self.assertEqual(verdict.pending, [ctv.QUICK_AGGREGATE])
        self.assertEqual(verdict.description(), f"Waiting for: {ctv.QUICK_AGGREGATE}")

    def test_a_quick_job_outside_the_judged_run_does_not_count(self) -> None:
        # A job of that name in another workflow's run, or in an older CI run, is not this run's aggregate.
        scenario = self._light(quick=None)
        foreign = scenario.add_run(8, ".github/workflows/extra.yml", [ctv.QUICK_AGGREGATE])
        self.assertEqual(foreign["head_sha"], HEAD)
        self.assertVerdict(scenario, "failure", "JOB_FAILED")
        older = self._light(quick=None)
        older.add_run(8, CI, [ctv.QUICK_AGGREGATE], created="2026-10-28T09:00:00Z")
        self.assertVerdict(older, "failure", "JOB_FAILED")

    def test_the_other_producers_still_decide(self) -> None:
        failed = self._light()
        failed.job(2, "OSV Vulnerability Scan").update(conclusion="failure")
        verdict = self.assertVerdict(failed, "failure", "JOB_FAILED")
        self.assertEqual(verdict.description(), "1 finding(s): JOB_FAILED")
        waiting = self._light()
        waiting.job(2, "OSV Vulnerability Scan").update(status="in_progress", conclusion=None)
        verdict = self.assertVerdict(waiting, "pending")
        self.assertEqual(verdict.pending, [ctv.LIGHT_PENDING, "OSV Vulnerability Scan"])
        self.assertFalse(verdict.description().startswith("LIGHT_CI_GREEN"),
                         "the code leads the description only when nothing else is outstanding")
        hygiene = self._light()
        hygiene.job(1, "Hygiene and security").update(conclusion="skipped")
        self.assertVerdict(hygiene, "failure", "JOB_FAILED")

    def test_a_light_draft_is_a_draft_failure(self) -> None:
        scenario = self._light()
        scenario.raw["pull_request"]["draft"] = True
        verdict = self.assertVerdict(scenario, "failure", "DRAFT_LIGHT_CI")
        self.assertEqual(verdict.description(), "1 finding(s): DRAFT_LIGHT_CI")

    def test_a_light_head_behind_main_reports_only_behind(self) -> None:
        # The desk's behind-only path then reads the check runs itself, as for a full-green head.
        scenario = self._light()
        scenario.change(CI, kind="behind")
        verdict = self.assertVerdict(scenario, "failure", "CI_DEFINITION_BEHIND")
        self.assertEqual(verdict.description(), "1 finding(s): CI_DEFINITION_BEHIND")

    def test_an_unapproved_ci_change_is_not_hidden_by_a_light_run(self) -> None:
        scenario = self._light()
        scenario.change("tools/ci_required.py")
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")

    def test_the_newest_run_decides_between_light_and_full(self) -> None:
        # Full and green first, then a light run at the same head (the label was removed and CI re-run,
        # or the pull request went to Draft and back): the newer, light run is judged.
        scenario = Scenario()
        scenario.add_run(6, CI, ["Hygiene and security", ctv.QUICK_AGGREGATE, ctv.FULL_AGGREGATE], created=DRAFT_AT)
        scenario.job(6, ctv.FULL_AGGREGATE).update(conclusion="skipped")
        self.assertEqual(self.assertVerdict(scenario, "pending").description(), ctv.LIGHT_DESCRIPTION)
        # Light first, then the full run the label started: success.
        scenario = self._light()
        scenario.add_run(6, CI, ["Hygiene and security", ctv.FULL_AGGREGATE], created=READY_AT)
        self.assertVerdict(scenario, "success")
        # A cancelled newer run does not stand in for either.
        scenario = self._light()
        scenario.add_run(6, CI, ["Hygiene and security", ctv.FULL_AGGREGATE], created=READY_AT,
                         conclusion="cancelled")
        self.assertEqual(self.assertVerdict(scenario, "pending").description(), ctv.LIGHT_DESCRIPTION)

    def test_the_verdict_reads_no_label_for_the_tier(self) -> None:
        """Which heads run full is the classifier's cost decision; the verdict judges only what ran."""
        for labels in ((), ("ci-full-matrix",), ("ci-full-matrix", ctv.APPROVAL_LABEL)):
            with self.subTest(labels=labels):
                light = self._light()
                light.raw["pull_request"]["labels"] = [{"name": name} for name in labels]
                self.assertVerdict(light, "pending")
                full = Scenario()
                full.raw["pull_request"]["labels"] = [{"name": name} for name in labels]
                self.assertVerdict(full, "success")
        source = (ROOT / "tools" / "ci_trust_verdict.py").read_text(encoding="utf-8")
        code = "\n".join(line for line in source.split('"""', 2)[2].splitlines() if not line.lstrip().startswith("#"))
        for name in ("NK_CI_TIERS", "FULL_TIER_LABEL", "batch/"):
            self.assertNotIn(name, code)

    def test_main_posts_pending_with_the_code(self) -> None:
        gh = FakeGitHub(self._light())
        with mock.patch.object(ctv, "git_facts", return_value=_facts()), \
                mock.patch.object(ctv, "GitHub", return_value=gh), \
                mock.patch.dict(os.environ, {"GITHUB_TOKEN": "token"}), mock.patch("sys.stdout"):
            code = ctv.main(["--repository", gh.repository, "--post", "--pull-request", str(PR)])
        self.assertEqual(code, 0)
        self.assertEqual(gh.posts, [(f"statuses/{HEAD}", {
            "state": "pending", "context": ctv.VERDICT_CONTEXT, "description": ctv.LIGHT_DESCRIPTION,
        })])
        self.assertEqual(ctv.LIGHT_DESCRIPTION, "LIGHT_CI_GREEN: quick checks passed at this head; the full checks "
                         "run when it is batched for landing")

    def test_the_summary_names_the_light_run(self) -> None:
        scenario = self._light()
        report = ctv.summary_markdown(scenario.verdict(), ctv.normalise(scenario.raw))
        self.assertIn("### Trusted CI verdict: pending", report)
        self.assertIn("(info) `LIGHT_CI_GREEN`", report)
        self.assertIn(f"`{ctv.LIGHT_PENDING}`", report)


class ProtectedPathTests(unittest.TestCase):
    def test_tool_references_skip_comments_and_longer_paths(self) -> None:
        text = (
            "      # tools/hle_registry_meta.py is documentation here\n"
            "        run: python tools/ci_paths.py --github-output x\n"
            '          "$py" tools/vfpu_fuzz_gen.py --synthetic\n'
            '          test -f "$install_dir/source/tools/nk_cli.py"\n'
            "          python -m unittest -v tools.test_site_mobile\n"
            "            -r tools/requirements-lock.txt\n"
        )
        self.assertEqual(
            ctv.referenced_tools(text),
            {"tools/ci_paths.py", "tools/vfpu_fuzz_gen.py", "tools/requirements-lock.txt"},
        )

    def test_change_kinds(self) -> None:
        trusted = {"a": "1", "b": "2", "c": "3"}
        candidate = {"a": "1", "b": "9", "d": "4", "c": "0"}
        merge_base = {"a": "1", "b": "2", "c": "0"}
        changes = ctv.protected_changes(trusted, candidate, merge_base, {"a", "b", "c", "d", "e"})
        self.assertEqual(changes, [
            {"path": "b", "kind": "modified"},
            {"path": "c", "kind": "behind"},
            {"path": "d", "kind": "added"},
        ])

    def test_live_workflows_protect_the_gate_tools(self) -> None:
        texts = [path.read_text(encoding="utf-8") for path in sorted(WORKFLOWS.glob("*.y*ml"))]
        tree = {path.relative_to(ROOT).as_posix(): "x" for path in (ROOT / "tools").iterdir()}
        tree |= {".pre-commit-config.yaml": "x"}
        paths = ctv.protected_paths(tree, {}, texts)
        for path in ("tools/ci_required.py", "tools/ci_paths.py", "tools/tree_lint.py",
                     "tools/provenance_attest_verify.py", "tools/ci_trust_verdict.py",
                     "tools/ci_test_shards.py", "tools/betterleaks_canary.py", ".pre-commit-config.yaml"):
            self.assertIn(path, paths)
        self.assertNotIn("tools/nk_cli.py", paths)
        self.assertNotIn("tools/hle_registry_meta.py", paths)

    def test_git_facts_on_a_real_repository(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            env = os.environ | {
                "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid",
                "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.invalid",
            }

            def git(*args: str) -> str:
                return subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True,
                                      text=True, env=env).stdout.strip()

            def write(path: str, text: str) -> None:
                target = repo / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text, encoding="utf-8", newline="\n")

            git("init", "-q", "-b", "main")
            write(".github/workflows/ci.yml", "jobs:\n  a:\n    steps:\n      - run: python tools/gate.py\n")
            write("tools/gate.py", "print('gate')\n")
            write("src/x.c", "int x;\n")
            git("add", "-A")
            git("commit", "-q", "-m", "base")
            git("checkout", "-q", "-b", "candidate")
            write("tools/gate.py", "raise SystemExit(0)\n")
            write(".github/workflows/spoof.yml", f"jobs:\n  s:\n    name: {ctv.VERDICT_CONTEXT}\n")
            write("src/x.c", "int x = 1;\n")
            git("add", "-A")
            git("commit", "-q", "-m", "candidate")
            head = git("rev-parse", "HEAD")
            git("update-ref", f"refs/pull/{PR}/head", head)
            git("checkout", "-q", "main")

            # The trusted checkout knows only main; the candidate arrives by fetch, as data.
            trusted = Path(tmp) / "trusted"
            subprocess.run(["git", "clone", "-q", "--single-branch", "-b", "main", str(repo), str(trusted)],
                           check=True, capture_output=True, env=env)
            facts = ctv.git_facts(trusted, "owner/repo", PR, head, remote=str(repo))
            self.assertEqual(facts["fetched_head"], head)
            self.assertEqual(facts["trusted_sha"], git("rev-parse", "main"))
            self.assertEqual(facts["protected_changes"], [
                {"path": ".github/workflows/spoof.yml", "kind": "added"},
                {"path": "tools/gate.py", "kind": "modified"},
            ])
            self.assertEqual(facts["context_collisions"], [".github/workflows/spoof.yml"])
            # Nothing of the candidate was checked out in the trusted tree.
            self.assertEqual((trusted / "tools/gate.py").read_text(encoding="utf-8"), "print('gate')\n")

            moved = ctv.git_facts(trusted, "owner/repo", PR, OTHER, remote=str(repo))
            self.assertEqual(moved["fetched_head"], head)
            self.assertEqual(moved["protected_changes"], [])


class WorkflowWiringTests(unittest.TestCase):
    """The verdict workflow keeps the shape its trust argument depends on."""

    def setUp(self) -> None:
        self.text = (ROOT / ctv.VERDICT_WORKFLOW).read_text(encoding="utf-8")

    @staticmethod
    def _top_name(path: Path) -> str:
        match = re.search(r"(?m)^name:[ \t]*['\"]?(.+?)['\"]?[ \t]*$", path.read_text(encoding="utf-8"))
        assert match is not None
        return match.group(1)

    def test_runs_only_trusted_definitions(self) -> None:
        on_block = self.text[self.text.index("\non:\n"):self.text.index("\nconcurrency:")]
        self.assertIn("pull_request_target:", on_block)
        self.assertIn("workflow_run:", on_block)
        self.assertNotRegex(on_block, r"(?m)^  pull_request:")
        self.assertNotIn("merge_group", on_block)
        self.assertIn("ref: ${{ github.event.repository.default_branch }}", self.text)
        self.assertIn("persist-credentials: false", self.text)
        self.assertNotIn("head.ref", self.text)
        self.assertNotIn("github.head_ref", self.text)
        self.assertNotIn("workflow_run.head_branch", self.text)
        # The two approval jobs and the verdict job, each the default branch.
        self.assertEqual(self.text.count("actions/checkout@"), 3)
        self.assertEqual(self.text.count("ref: ${{ github.event.repository.default_branch }}"), 3)

    def test_least_privilege(self) -> None:
        self.assertIn("\npermissions: {}\n", self.text)
        self.assertNotRegex(self.text, r"contents: write|actions: write|checks: write")
        self.assertEqual(self.text.count("pull-requests: write"), 1)
        self.assertEqual(self.text.count("statuses: write"), 1)
        for uses in re.findall(r"(?m)^\s*(?:- )?uses: (\S+)", self.text):
            self.assertRegex(uses, r"@[0-9a-f]{40}$")

    def test_retriggers_only_on_ci_completion(self) -> None:
        # CI finished last on all 181 heads whose three pull_request producers passed on
        # 2026-11-04/05, so a verdict started by OSV-Scanner or Dependency Review only
        # re-posted "pending" and held the head's concurrency group ahead of the run CI
        # starts. When either finishes after CI (a re-run), the verdict stays pending, never
        # success, until it is dispatched by hand, as for the provenance attestation.
        match = re.search(r"workflows: \[([^\]]*)\]", self.text)
        assert match is not None
        names = {name.strip().strip('"') for name in match.group(1).split(",")}
        ci = {self._top_name(ROOT / producer.workflow) for producer in ctv.PRODUCERS
              if producer.workflow == ".github/workflows/ci.yml"}
        self.assertEqual(ci, {"CI"})
        self.assertEqual(names, ci)

    def test_cancelled_first_attempt_starts_no_verdict(self) -> None:
        # A first CI attempt is cancelled when a newer push supersedes it; its verdict would
        # judge a stale head. The skip sits inside the workflow_run route only, so a re-run
        # (attempt 2 or later) and the pull_request_target and dispatch routes still judge.
        start = self.text.index("\n  verdict:\n")
        condition = self.text[self.text.index("    if: >-\n", start):self.text.index("    runs-on:", start)]
        skip = ("!(github.event.workflow_run.conclusion == 'cancelled' && "
                "github.event.workflow_run.run_attempt == 1)")
        self.assertEqual(condition.count(skip), 1)
        route = re.search(r"\(github\.event_name == 'workflow_run' && "
                          r"github\.event\.workflow_run\.event == 'pull_request' &&\s*"
                          + re.escape(skip) + r"\)", condition)
        self.assertIsNotNone(route, "the skip must be part of the workflow_run alternative")
        self.assertIn("github.event_name == 'workflow_dispatch'", condition)
        self.assertIn("github.event.pull_request.base.ref == github.event.repository.default_branch", condition)

    def test_does_not_name_the_provenance_context(self) -> None:
        # provenance_attest_verify.py refuses any other workflow naming its context.
        self.assertNotIn("Trusted provenance attestation", self.text)

    def test_verdict_job_is_not_named_like_the_status(self) -> None:
        self.assertNotRegex(self.text, rf"(?m)^    name: {re.escape(ctv.VERDICT_CONTEXT)}\s*$")

    def test_no_other_workflow_names_the_verdict_context(self) -> None:
        for path in sorted(WORKFLOWS.glob("*.y*ml")):
            if path.name == Path(ctv.VERDICT_WORKFLOW).name:
                continue
            with self.subTest(workflow=path.name):
                self.assertNotIn(ctv.VERDICT_CONTEXT, path.read_text(encoding="utf-8"))

    def test_the_ruleset_requires_the_verdict_not_the_contexts_it_judges(self) -> None:
        try:
            from test_public_ci_wiring import MAIN_RULESET_SNAPSHOT
        except ModuleNotFoundError:
            from tools.test_public_ci_wiring import MAIN_RULESET_SNAPSHOT
        required = MAIN_RULESET_SNAPSHOT["required_status_checks"]
        assert isinstance(required, dict)
        actions = {name for name, app in required.items() if app == ctv.GITHUB_ACTIONS_APP_ID}
        # Since 2026-10-29 the only GitHub Actions context the ruleset requires is the verdict;
        # a producer context required by name again would be spoofable by any same-named job.
        self.assertEqual(actions, {ctv.VERDICT_CONTEXT})
        self.assertNotIn(ctv.VERDICT_CONTEXT, {producer.context for producer in ctv.PRODUCERS})

    def test_each_producer_workflow_declares_its_job_and_event(self) -> None:
        for producer in ctv.PRODUCERS:
            with self.subTest(context=producer.context):
                text = (ROOT / producer.workflow).read_text(encoding="utf-8")
                self.assertRegex(text, rf"(?m)^(    name: {re.escape(producer.context)}|  {re.escape(producer.context)}:)\s*$")
                for event in producer.events + producer.named_events:
                    self.assertRegex(text, rf"(?m)^  {event}:")

    def test_the_attestation_titles_its_run_with_the_head_on_every_route(self) -> None:
        """Failing-before: the workflow set no run name, so a workflow_run or dispatched attestation could not
        be told from one for any other head."""
        self.assertEqual(PROVENANCE.run_title, NAMED_TITLE)
        text = (ROOT / PROVENANCE.workflow).read_text(encoding="utf-8")
        title = "format('" + PROVENANCE.run_title.format(head="{0}") + "', "
        heads = re.findall(re.escape(title) + r"([A-Za-z_.]+)\)", text[:text.index("\non:\n")])
        self.assertEqual(heads, ["github.event.pull_request.head.sha", "github.event.workflow_run.head_sha",
                                 "inputs.head_sha"])
        self.assertEqual(set(PROVENANCE.named_events), {"workflow_run", "workflow_dispatch"})


class FakeGitHub(ctv.GitHub):
    """Answers the collector's reads from a canned scenario and records posts."""

    def __init__(self, scenario: Scenario, permission_error: bool = False) -> None:
        super().__init__("Jstar269/nakagawa-recomp", "token")
        self.raw = scenario.raw
        self.permission_error = permission_error
        self.posts: list[tuple[str, dict]] = []
        self.reads: list[str] = []

    def get(self, path: str) -> object:
        self.reads.append(path)
        if path == f"pulls/{PR}":
            return self.raw["pull_request"]
        if path.startswith("collaborators/"):
            if self.permission_error:
                raise ctv.urllib.error.HTTPError(path, 403, "forbidden", {}, None)  # type: ignore[arg-type]
            return {"permission": self.raw.get("label_actor_permission")}
        raise AssertionError(path)

    def paginate(self, path: str, key: str | None = None) -> list:
        self.reads.append(path)
        if path == "pulls?state=open":
            returned: list[Any] = self.raw["open_pull_requests"]
            return returned
        if path == f"issues/{PR}/events":
            returned = self.raw["label_events"]
            return returned
        if path.startswith("actions/runs?head_sha="):
            # GitHub lists a workflow_run or dispatched run under the default branch's commit, never here.
            returned = [run for run in self.raw["workflow_runs"] if run["head_sha"] == HEAD]
            return returned
        if path.startswith("actions/workflows/provenance-attestation.yml/runs?"):
            event = path.split("event=", 1)[1].split("&", 1)[0]
            returned = [run for run in self.raw["workflow_runs"]
                        if run["path"] == PROV and run["head_sha"] != HEAD and run["event"] == event]
            return returned
        if path.startswith("actions/workflows/ci-trust-verdict.yml/runs?"):
            returned = self.raw.get("dispatch_runs", [])
            return returned
        if path.startswith("actions/runs/"):
            returned = self.raw["jobs"][path.split("/")[2]]
            return returned
        if path.startswith(f"commits/{HEAD}/check-runs"):
            returned = self.raw["check_runs"]
            return returned
        if path == f"commits/{HEAD}/statuses":
            returned = self.raw["statuses"]
            return returned
        raise AssertionError(path)

    def counted(self, path: str, key: str | None = None) -> tuple[list, int | None]:
        """A list with the total GitHub reports for it.

        ``listed_total`` is the total of the attestation's lists (one value for both events, or a dict by
        event) and ``dispatch_total`` that of the verdict's dispatch list; a total above what is returned makes
        the list look truncated.
        """
        items = self.paginate(path, key)
        total: int | None = len(items)
        if path.startswith("actions/workflows/provenance-attestation.yml/runs?"):
            listed = self.raw.get("listed_total", {})
            event = path.split("event=", 1)[1].split("&", 1)[0]
            total = listed.get(event, total) if isinstance(listed, dict) else listed
        elif path.startswith("actions/workflows/ci-trust-verdict.yml/runs?"):
            total = self.raw.get("dispatch_total", total)
        return items, total

    def post(self, path: str, body: dict) -> object:
        self.posts.append((path, body))
        return None


def _facts(changes: list[dict] | None = None) -> dict:
    return {"fetched_head": HEAD, "trusted_sha": "c" * 40,
            "protected_changes": changes or [], "context_collisions": []}


class CollectorTests(unittest.TestCase):
    """The live path: what the collector reads, and what main() posts."""

    def test_collect_by_head_finds_the_pull_request_and_reads_every_source(self) -> None:
        scenario = Scenario()
        scenario.label(at="2026-10-28T10:05:00Z", permission="write")
        gh = FakeGitHub(scenario)
        with mock.patch.object(ctv, "git_facts", return_value=_facts()) as facts:
            raw = ctv.collect(gh, Path("."), "main", HEAD, None)
        assert raw is not None
        facts.assert_called_once_with(Path("."), gh.repository, PR, HEAD)
        self.assertEqual(raw["label_actor_permission"], "write")
        self.assertEqual(set(raw["jobs"]), {"1", "2", "3", "4"})  # producer workflows only
        self.assertIn(f"commits/{HEAD}/check-runs?filter=all", gh.reads)
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).state, "success")

    def test_collect_reads_jobs_only_for_each_producers_newest_live_run(self) -> None:
        """The rules judge a producer from its newest live run at the head, so a superseded or cancelled run's
        jobs are not read: one API call each, and the token's hourly budget ran out on 2026-10-31.
        Failing-before: the collector read the jobs of every run of a producer workflow."""
        scenario = Scenario()
        scenario.add_run(6, CI, ["Classify change", "Hygiene and security", "CI required"],
                         created="2026-10-28T09:00:00Z")
        scenario.add_run(8, OSV, ["OSV Vulnerability Scan"], created="2026-10-28T10:10:00Z",
                         conclusion="cancelled")
        gh = FakeGitHub(scenario)
        with mock.patch.object(ctv, "git_facts", return_value=_facts()):
            raw = ctv.collect(gh, Path("."), "main", HEAD, None)
        assert raw is not None
        self.assertEqual(set(raw["jobs"]), {"1", "2", "3", "4"})
        self.assertEqual([read for read in gh.reads if read.startswith(("actions/runs/6/", "actions/runs/8/"))], [])
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).state, "success")

    def test_requests_are_counted_and_the_tokens_budget_is_logged(self) -> None:
        """Each run logs its API requests and the budget the token's last answer reported, a refusal included."""
        gh = ctv.GitHub("Jstar269/nakagawa-recomp", "token")
        response = mock.MagicMock()
        response.read.return_value = b"{}"
        response.headers = {"X-RateLimit-Limit": "1000", "X-RateLimit-Remaining": "37", "X-RateLimit-Used": "963",
                            "X-RateLimit-Reset": "1793473200", "X-RateLimit-Resource": "core"}
        response.__enter__.return_value = response
        with mock.patch.object(ctv.urllib.request, "urlopen", return_value=response):
            gh.get(f"pulls/{PR}")
            gh.get(f"pulls/{PR}")
        note = gh.budget_note()
        self.assertIn("2 request(s) this run", note)
        self.assertIn("core budget had 37 of 1000 left", note)
        self.assertRegex(note, r"resetting at \d\d:\d\d:\d\dZ$")
        refusal = urllib.error.HTTPError("https://api.github.com/x", 403, "rate limited",
                                         {"x-ratelimit-limit": "1000", "x-ratelimit-remaining": "0"},  # type: ignore[arg-type]
                                         None)
        with mock.patch.object(ctv.urllib.request, "urlopen", side_effect=refusal):
            with self.assertRaises(urllib.error.HTTPError):
                gh.get(f"pulls/{PR}")
        self.assertIn("3 request(s) this run; the token's core budget had 0 of 1000 left", gh.budget_note())

    @staticmethod
    def _answer(limit: str, remaining: str) -> mock.MagicMock:
        response = mock.MagicMock()
        response.read.return_value = b"{}"
        response.headers = {"X-RateLimit-Limit": limit, "X-RateLimit-Remaining": remaining,
                            "X-RateLimit-Resource": "core"}
        response.__enter__.return_value = response
        return response

    def test_reads_use_the_read_token_and_the_status_uses_github_token(self) -> None:
        """VERDICT_READ_TOKEN carries the reads, on the maintainer's budget; the ruleset accepts the status only
        from GitHub Actions, so it is posted with GITHUB_TOKEN."""
        gh = ctv.GitHub("Jstar269/nakagawa-recomp", "actions-token", "read-token")
        seen: list[tuple[str, str]] = []

        def urlopen(request, timeout):
            seen.append((request.get_method(), request.get_header("Authorization")))
            return self._answer("5000", "4900") if request.get_method() == "GET" else self._answer("1000", "640")

        with mock.patch.object(ctv.urllib.request, "urlopen", side_effect=urlopen):
            gh.get(f"pulls/{PR}")
            gh.paginate("pulls?state=open")
            gh.post(f"statuses/{HEAD}", {"state": "success"})
        self.assertEqual(seen, [("GET", "Bearer read-token"), ("GET", "Bearer read-token"),
                                ("POST", "Bearer actions-token")])
        note = gh.budget_note()
        self.assertIn("3 request(s) this run (2 with the read token, 0 refused)", note)
        self.assertIn("the read token's core budget had 4900 of 5000 left", note)
        self.assertIn("GITHUB_TOKEN's core budget had 640 of 1000 left", note)

    def test_a_read_the_read_token_is_refused_goes_to_github_token(self) -> None:
        """A missing permission or a spent budget on the read token costs one retry, not the verdict."""
        gh = ctv.GitHub("Jstar269/nakagawa-recomp", "actions-token", "read-token")
        refusal = urllib.error.HTTPError("https://api.github.com/x", 403, "forbidden", {}, None)  # type: ignore[arg-type]
        seen: list[str] = []

        def urlopen(request, timeout):
            seen.append(request.get_header("Authorization"))
            if request.get_header("Authorization") == "Bearer read-token":
                raise refusal
            return self._answer("1000", "500")

        with mock.patch.object(ctv.urllib.request, "urlopen", side_effect=urlopen), mock.patch("sys.stderr"):
            self.assertEqual(gh.get(f"commits/{HEAD}/check-runs"), {})
        self.assertEqual(seen, ["Bearer read-token", "Bearer actions-token"])
        self.assertEqual(gh.read_refusals, 1)
        not_found = urllib.error.HTTPError("https://api.github.com/x", 404, "missing", {}, None)  # type: ignore[arg-type]
        with mock.patch.object(ctv.urllib.request, "urlopen", side_effect=not_found), mock.patch("sys.stderr"):
            with self.assertRaises(urllib.error.HTTPError):
                gh.get(f"pulls/{PR}")   # missing for both tokens: still an error

    def test_main_passes_the_read_token_only_when_it_is_set(self) -> None:
        for value, expected in (("read-token", "read-token"), ("", None)):
            with self.subTest(read_token=value):
                gh = FakeGitHub(Scenario())
                env = {"GITHUB_TOKEN": "token", "VERDICT_READ_TOKEN": value}
                with mock.patch.object(ctv, "GitHub", return_value=gh) as client, \
                        mock.patch.dict(os.environ, env), mock.patch("sys.stdout"), \
                        mock.patch.object(ctv, "git_facts", return_value=_facts()):
                    ctv.main(["--repository", gh.repository, "--head-sha", HEAD])
                client.assert_called_once_with(gh.repository, "token", expected)

    def test_collect_finds_nothing_for_a_head_without_a_pull_request_into_main(self) -> None:
        scenario = Scenario()
        scenario.raw["open_pull_requests"][0]["base"]["ref"] = "release"
        self.assertIsNone(ctv.collect(FakeGitHub(scenario), Path("."), "main", HEAD, None))

    def test_unreadable_label_permission_does_not_approve(self) -> None:
        scenario = Scenario()
        scenario.change("tools/ci_paths.py")
        scenario.label(at="2026-10-28T10:05:00Z")
        gh = FakeGitHub(scenario, permission_error=True)
        with mock.patch.object(ctv, "git_facts", return_value=_facts(scenario.raw["protected_changes"])):
            raw = ctv.collect(gh, Path("."), "main", HEAD, PR)
        assert raw is not None
        self.assertIsNone(raw["label_actor_permission"])
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).state, "failure")

    def _main(self, gh: FakeGitHub, *args: str) -> int:
        with mock.patch.object(ctv, "GitHub", return_value=gh), \
                mock.patch.dict(os.environ, {"GITHUB_TOKEN": "token"}), \
                mock.patch("sys.stdout"):
            returned: int = ctv.main(["--repository", gh.repository, "--post", "--target-url", "https://run", *args])
            return returned

    def test_main_posts_one_status_bound_to_the_head(self) -> None:
        gh = FakeGitHub(Scenario())
        with mock.patch.object(ctv, "git_facts", return_value=_facts()):
            self.assertEqual(self._main(gh, "--head-sha", HEAD), 0)
        self.assertEqual(gh.posts, [(f"statuses/{HEAD}", {
            "state": "success", "context": ctv.VERDICT_CONTEXT,
            "description": ctv.Verdict("success").description(), "target_url": "https://run",
        })])

    def test_main_posts_failure_and_exits_nonzero(self) -> None:
        gh = FakeGitHub(Scenario())
        with mock.patch.object(ctv, "git_facts", return_value=_facts([{"path": CI, "kind": "modified"}])):
            self.assertEqual(self._main(gh, "--pull-request", str(PR)), 1)
        self.assertEqual(gh.posts[0][1]["state"], "failure")
        self.assertIn("CI_CHANGE_UNAPPROVED", gh.posts[0][1]["description"])

    def test_main_replaces_an_old_verdict_with_error_when_it_cannot_judge(self) -> None:
        gh = FakeGitHub(Scenario())
        with mock.patch.object(ctv, "git_facts", side_effect=subprocess.CalledProcessError(128, "git")), \
                self.assertRaises(subprocess.CalledProcessError):
            self._main(gh, "--head-sha", HEAD)
        self.assertEqual([body["state"] for _, body in gh.posts], ["error"])

    def test_main_posts_nothing_when_no_pull_request_has_the_head(self) -> None:
        scenario = Scenario()
        scenario.raw["open_pull_requests"] = []
        gh = FakeGitHub(scenario)
        self.assertEqual(self._main(gh, "--head-sha", HEAD), 0)
        self.assertEqual(gh.posts, [])

    def test_paginate_follows_link_headers(self) -> None:
        gh = ctv.GitHub("owner/repo", "token")
        pages = {
            "actions/runs?head_sha=x&per_page=100": ({"workflow_runs": [1, 2]},
                                                     {"Link": '<https://api.github.com/next>; rel="next"'}),
            "https://api.github.com/next": ({"workflow_runs": [3]}, {}),
        }
        with mock.patch.object(gh, "_request", side_effect=lambda method, url, body=None: pages[url]):
            self.assertEqual(gh.paginate("actions/runs?head_sha=x", "workflow_runs"), [1, 2, 3])

    def test_requests_carry_the_token_and_api_version(self) -> None:
        gh = ctv.GitHub("owner/repo", "secret")
        response = mock.MagicMock()
        response.read.return_value = b'{"ok": true}'
        response.headers = {}
        response.__enter__.return_value = response
        with mock.patch.object(ctv.urllib.request, "urlopen", return_value=response) as urlopen:
            self.assertEqual(gh.post(f"statuses/{HEAD}", {"state": "pending"}), {"ok": True})
        request = urlopen.call_args.args[0]
        self.assertEqual(request.full_url, f"https://api.github.com/repos/owner/repo/statuses/{HEAD}")
        self.assertEqual(request.get_method(), "POST")
        self.assertEqual(request.get_header("Authorization"), "Bearer secret")
        self.assertEqual(request.get_header("X-github-api-version"), "2022-11-28")

    def _response(self, body: bytes) -> mock.MagicMock:
        response = mock.MagicMock()
        response.read.return_value = body
        response.headers = {}
        response.__enter__.return_value = response
        return response

    def _http_error(self, code: int) -> urllib.error.HTTPError:
        return urllib.error.HTTPError("https://api.github.com/x", code, "error", {}, None)

    def test_a_gateway_error_is_retried_until_github_answers(self) -> None:
        # One HTTP 502 made the verifier post "could not judge this head" on several pull requests (2026-10-30).
        gh = ctv.GitHub("owner/repo", "token")
        answers = [self._http_error(502), urllib.error.URLError("reset"), self._response(b'{"ok": true}')]
        with mock.patch.object(ctv.urllib.request, "urlopen", side_effect=answers) as urlopen, \
                mock.patch.object(ctv.time, "sleep") as sleep:
            self.assertEqual(gh.get("pulls/1"), {"ok": True})
        self.assertEqual(urlopen.call_count, 3)
        self.assertEqual([call.args[0] for call in sleep.call_args_list], list(ctv.RETRY_DELAYS))

    def test_a_lasting_gateway_error_still_fails_after_the_last_attempt(self) -> None:
        gh = ctv.GitHub("owner/repo", "token")
        with mock.patch.object(ctv.urllib.request, "urlopen", side_effect=[self._http_error(503)] * 3), \
                mock.patch.object(ctv.time, "sleep"):
            with self.assertRaises(urllib.error.HTTPError) as raised:
                gh.get("pulls/1")
        self.assertEqual(raised.exception.code, 503)

    def test_a_refusal_is_not_retried(self) -> None:
        # 404 and 403 are answers, not outages: retrying would only spend the token budget.
        for code in (403, 404, 422):
            with self.subTest(code=code):
                gh = ctv.GitHub("owner/repo", "token")
                with mock.patch.object(ctv.urllib.request, "urlopen", side_effect=[self._http_error(code)]) as urlopen, \
                        mock.patch.object(ctv.time, "sleep") as sleep:
                    with self.assertRaises(urllib.error.HTTPError):
                        gh.get("pulls/1")
                self.assertEqual(urlopen.call_count, 1)
                sleep.assert_not_called()

    def test_an_unfinished_producer_is_reported_pending_from_the_runs_alone(self) -> None:
        # The full read made a dozen calls on every completion and exhausted the token budget (2026-10-30).
        scenario = Scenario()
        scenario.raw["workflow_runs"][0].update(status="in_progress", conclusion=None)
        gh = FakeGitHub(scenario)
        with mock.patch.object(ctv, "git_facts") as facts:
            self.assertEqual(self._main(gh, "--head-sha", HEAD), 0)
        facts.assert_not_called()
        self.assertEqual(gh.reads, ["pulls?state=open", f"actions/runs?head_sha={HEAD}"])
        self.assertEqual([(path, body["state"], body["description"]) for path, body in gh.posts], [
            (f"statuses/{HEAD}", "pending", "Waiting for: CI required, Hygiene and security"),
        ])

    def test_a_producer_with_no_run_yet_is_waited_for(self) -> None:
        scenario = Scenario()
        runs = [run for run in scenario.raw["workflow_runs"] if run["path"] != OSV]
        self.assertEqual(ctv.unfinished_producers(runs, HEAD), ["OSV Vulnerability Scan"])

    def test_a_producer_whose_runs_were_all_cancelled_gets_the_full_judgement(self) -> None:
        scenario = Scenario()
        scenario.raw["workflow_runs"][1].update(conclusion="cancelled")
        self.assertEqual(ctv.unfinished_producers(scenario.raw["workflow_runs"], HEAD), [])
        gh = FakeGitHub(scenario)
        with mock.patch.object(ctv, "git_facts", return_value=_facts()):
            self.assertEqual(self._main(gh, "--head-sha", HEAD), 1)
        self.assertIn("JOB_CANCELLED", gh.posts[0][1]["description"])

    def test_runs_for_other_heads_do_not_count_as_finished(self) -> None:
        scenario = Scenario()
        stale = copy.deepcopy(scenario.raw["workflow_runs"])
        for run in stale:
            run["head_sha"] = "d" * 40
        self.assertEqual(ctv.unfinished_producers(stale, HEAD),
                         [producer.context for producer in ctv.PRODUCERS])

    def test_label_events_are_read_only_when_the_label_is_on(self) -> None:
        gh = FakeGitHub(Scenario())
        with mock.patch.object(ctv, "git_facts", return_value=_facts()):
            raw = ctv.collect(gh, Path("."), "main", HEAD, PR)
        assert raw is not None
        self.assertNotIn(f"issues/{PR}/events", gh.reads)
        labelled = Scenario()
        labelled.label(at="2026-10-28T10:05:00Z", permission="write")
        gh = FakeGitHub(labelled)
        with mock.patch.object(ctv, "git_facts", return_value=_facts()):
            ctv.collect(gh, Path("."), "main", HEAD, PR)
        self.assertIn(f"issues/{PR}/events", gh.reads)

    def test_main_rejects_a_malformed_head(self) -> None:
        gh = FakeGitHub(Scenario())
        with mock.patch("sys.stderr"), self.assertRaises(SystemExit):
            self._main(gh, "--head-sha", "HEAD")


BEFORE = "d" * 40
LABELLED_AT = "2026-10-28T09:59:00Z"   # before PUSHED: the label predates the head, as after a push that was carried
VERDICT_RUN_PATH = ctv.VERDICT_WORKFLOW


def _record(run_id: int, digest: str, *, sha: str = HEAD, path: str = VERDICT_RUN_PATH,
            event: str = "pull_request_target", conclusion: str | None = "success",
            status: str = "completed", name: str | None = None, job_sha: str | None = None,
            number: int = PR, run_prs: tuple[int, ...] = (PR,), base: str = "main",
            created: str = PUSHED, completed: str = RECORDED) -> tuple[dict, dict]:
    """A run and its one job, shaped like the approval record the label workflow leaves."""
    run = _run(run_id, path, event, sha=sha, prs=run_prs, created=created)
    for item in run["pull_requests"]:
        item["base"]["ref"] = base
    job = _job(run_id * 100, name or f"{ctv.APPROVAL_RECORD_PREFIX}{number} {digest}", sha=job_sha or sha,
               status=status, conclusion=conclusion, completed=completed)
    return run, job


def _carried(digest: str | None = D1, records: tuple[str, ...] = (D1,), **label) -> Scenario:
    """A head that changes a CI tool, carrying a label applied before the head arrived, with the records the
    label workflow wrote at this head when the push was carried."""
    scenario = Scenario()
    scenario.change("tools/ci_paths.py")
    scenario.label(at=label.pop("at", LABELLED_AT), **label)
    scenario.raw["head_digest"] = digest
    scenario.raw["approval_records"] = [
        {"digest": item, "recorded_at": PUSHED, "run_created_at": PUSHED, "route": "head", "head_sha": HEAD,
         "actor": None}
        for item in records
    ]
    return scenario


class CarriedApprovalTests(VerdictAssertions):
    """A label applied before this head arrived approves it only when its protected content is unchanged."""

    def test_a_record_written_before_the_label_was_applied_does_not_approve(self) -> None:
        """Removing the label withdraws its records: a label applied again approves nothing by itself.
        Failing-before: a record at the head counted whenever it was written."""
        scenario = _carried(at="2026-10-28T10:05:00Z")   # the records were written at 10:00
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")
        record = scenario.raw["approval_records"][0]
        # A job that completed in the label's own second proves nothing about which came first.
        record.update(run_created_at="2026-10-28T10:05:00Z", recorded_at="2026-10-28T10:05:00Z")
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")
        record["recorded_at"] = "2026-10-28T10:05:01Z"
        self.assertVerdict(scenario, "success")

    def test_a_run_from_before_the_label_that_finished_after_it_does_not_approve(self) -> None:
        """A run answers the event that created it. One created before the current label (a label somebody else
        applied, or an earlier push), then re-run or started late, would write a record dated after a label that
        was applied for other bytes. Failing-before: nothing compared a record with the label at all."""
        scenario = _carried(at="2026-10-28T10:05:00Z")
        scenario.raw["approval_records"][0].update(run_created_at="2026-10-28T10:04:59Z",
                                                   recorded_at="2026-10-28T11:00:00Z")
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")

    def test_a_record_without_a_readable_time_does_not_approve(self) -> None:
        for field in ("recorded_at", "run_created_at"):
            for stamp in ("", None, "yesterday", "2026-10-28T10:00:00"):
                with self.subTest(field=field, stamp=stamp):
                    scenario = _carried()
                    scenario.raw["approval_records"][0][field] = stamp
                    self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")

    def test_unchanged_protected_content_keeps_the_approval(self) -> None:
        verdict = self.assertVerdict(_carried(), "success")
        self.assertEqual([item.code for item in verdict.findings], ["CI_CHANGE_APPROVED"])
        self.assertIn("byte-identical", verdict.findings[0].message)

    def test_changed_protected_content_drops_the_approval(self) -> None:
        self.assertVerdict(_carried(digest=D2), "failure", "CI_CHANGE_UNAPPROVED")

    def test_no_record_at_this_head_drops_the_approval(self) -> None:
        self.assertVerdict(_carried(records=()), "failure", "CI_CHANGE_UNAPPROVED")

    def test_a_record_of_another_digest_is_not_this_content(self) -> None:
        self.assertVerdict(_carried(records=(D2,)), "failure", "CI_CHANGE_UNAPPROVED")

    def test_a_digest_that_could_not_be_computed_drops_the_approval(self) -> None:
        self.assertVerdict(_carried(digest=None), "failure", "CI_CHANGE_UNAPPROVED")

    def test_a_retargeted_pull_request_does_not_carry_an_approval(self) -> None:
        scenario = _carried()
        scenario.raw["label_events"].append({"id": 9, "event": "base_ref_changed", "created_at": "2026-10-28T10:20:00Z"})
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")

    def test_a_matching_record_does_not_replace_the_label(self) -> None:
        scenario = _carried()
        scenario.raw["pull_request"]["labels"] = []
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")

    def test_a_matching_record_does_not_excuse_a_label_from_a_user_without_write(self) -> None:
        self.assertVerdict(_carried(permission="triage", actor="helper"), "failure", "CI_CHANGE_UNAPPROVED")

    def test_a_matching_record_does_not_survive_the_label_being_removed(self) -> None:
        scenario = _carried()
        scenario.label(at="2026-10-28T10:06:00Z", event="unlabeled")
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")

    def test_a_carried_approval_still_needs_every_intended_job(self) -> None:
        scenario = _carried()
        scenario.job(1, "CI required")["conclusion"] = "failure"
        self.assertVerdict(scenario, "failure", "JOB_FAILED")


class ApprovalRecordForgeryTests(unittest.TestCase):
    """Only a successful job of a pull_request_target run of the verdict workflow at this head is a record."""

    def digests(self, *pairs: tuple[dict, dict], head: str = HEAD) -> set[str]:
        runs = [run for run, _ in pairs]
        jobs = {run["id"]: [job] for run, job in pairs}
        records = ctv.head_records(runs, lambda run_id: jobs[run_id], head, PR, "main")
        return {record["digest"] for record in records}

    def test_the_genuine_record_is_read(self) -> None:
        self.assertEqual(self.digests(_record(1, D1)), {D1})

    def test_a_job_of_a_pull_request_run_is_ignored(self) -> None:
        # A candidate controls its own pull_request workflows, including a copy of this file under this path.
        self.assertEqual(self.digests(_record(1, D1, event="pull_request")), set())

    def test_a_job_of_a_workflow_run_or_dispatch_is_ignored(self) -> None:
        self.assertEqual(self.digests(_record(1, D1, event="workflow_run"), _record(2, D1, event="workflow_dispatch")),
                         set())

    def test_a_job_of_another_workflow_is_ignored(self) -> None:
        self.assertEqual(self.digests(_record(1, D1, path=".github/workflows/evil.yml")), set())

    def test_a_record_of_another_head_is_ignored(self) -> None:
        self.assertEqual(self.digests(_record(1, D1, sha=OTHER)), set())
        self.assertEqual(self.digests(_record(2, D1, job_sha=OTHER)), set())

    def test_a_failed_cancelled_or_unfinished_record_is_ignored(self) -> None:
        self.assertEqual(self.digests(_record(1, D1, conclusion="failure"), _record(2, D1, conclusion="cancelled"),
                                      _record(3, D1, conclusion=None, status="in_progress")), set())

    def test_malformed_names_are_ignored(self) -> None:
        prefix = "CI approval record v1 "
        for name in (f"{prefix}pr{PR} {D1[:-1]}", f"{prefix}pr{PR} {D1}0", f"{prefix}pr{PR} {(chr(97) * 64).upper()}",
                     f"{prefix}pr{PR}{D1}", f"{prefix}{D1}", f"{prefix}pr0{PR} {D1}", f"{prefix}pr-{PR} {D1}",
                     f"CI approval record v2 pr{PR} {D1}", f"x {prefix}pr{PR} {D1}", f"{prefix}pr{PR} {D1} ",
                     prefix, "Compute trusted CI verdict"):
            with self.subTest(name=name):
                self.assertEqual(self.digests(_record(1, D1, name=name)), set())

    def test_a_record_for_another_pull_request_at_the_same_sha_is_ignored(self) -> None:
        """Two pull requests can share a head SHA; a record names the pull request it was written for."""
        self.assertEqual(self.digests(_record(1, D1, number=PR + 1)), set())
        self.assertEqual(self.digests(_record(2, D1, number=PR)), {D1})

    def test_a_run_reported_for_another_pull_request_or_base_is_ignored(self) -> None:
        self.assertEqual(self.digests(_record(1, D1, run_prs=(PR + 1,))), set())
        self.assertEqual(self.digests(_record(2, D1, base="release")), set())
        self.assertEqual(self.digests(_record(3, D1, run_prs=())), {D1})   # a fork's run reports no pull requests

    def test_a_retargeted_pull_request_is_never_carried_from_a_label_event_record(self) -> None:
        gh = ApprovalGitHub(records=(_record(6, D1, sha=BEFORE),))
        gh.events.append({"id": 9, "event": "base_ref_changed", "created_at": "2026-10-28T10:20:00Z"})
        with mock.patch.object(ctv, "candidate_digest", return_value=D1), self.assertRaises(ctv.ApprovalRefused):
            ctv.carry_approval(gh, Path("."), PR, BEFORE, HEAD, "main")
        # A dispatched record never came from another base's workflow, so it still carries.
        gh.dispatches.append(_dispatched(8, created=PUSHED))
        with mock.patch.object(ctv, "candidate_digest", return_value=D1), \
                mock.patch.object(ctv, "default_definition", return_value=True):
            self.assertEqual(ctv.carry_approval(gh, Path("."), PR, BEFORE, HEAD, "main"), D1)

    def test_a_forged_record_does_not_approve_in_the_collector(self) -> None:
        """Failing-before: there was no record to forge, and a stale label was always withdrawn by the workflow."""
        scenario = Scenario()
        scenario.change("tools/ci_paths.py")
        scenario.label(at=LABELLED_AT)
        run, job = _record(6, D1, event="pull_request")
        scenario.raw["workflow_runs"].append(run)
        scenario.raw["jobs"]["6"] = [job]
        gh = FakeGitHub(scenario)
        with mock.patch.object(ctv, "git_facts", return_value=_facts(scenario.raw["protected_changes"])), \
                mock.patch.object(ctv, "approval_digest", return_value=D1):
            raw = ctv.collect(gh, Path("."), "main", HEAD, PR)
        assert raw is not None
        self.assertEqual(raw["approval_records"], [])
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).state, "failure")


def _dispatched(run_id: int, digest: str = D1, *, number: int = PR, head: str = HEAD, created: str = LABELLED,
                event: str = "workflow_dispatch", path: str = VERDICT_RUN_PATH, branch: str = "main",
                actor: str | None = "maintainer", title: str | None = None, name: str | None = None,
                conclusion: str | None = "success", status: str = "completed") -> tuple[dict, dict]:
    """A dispatched approval run and its record job, as ci-trust-verdict.yml leaves them on the default branch."""
    run = _run(run_id, path, event, sha=MAIN, created=created, prs=(), branch=branch, actor=actor,
               title=ctv.APPROVAL_RUN_TITLE.format(number=number, head=head) if title is None else title)
    job = _job(run_id * 100, name or f"{ctv.APPROVAL_RECORD_V2_PREFIX}{number} {head} {digest}", sha=MAIN,
               status=status, conclusion=conclusion)
    return run, job


class DispatchGitHub(ctv.GitHub):
    """The verdict workflow's dispatch runs, their jobs, and who has write access."""

    def __init__(self, *pairs: tuple[dict, dict], writers: tuple[str, ...] = ("maintainer",),
                 total: int | None | str = "all") -> None:
        super().__init__("Jstar269/nakagawa-recomp", "token")
        self.pairs = pairs
        self.writers = writers
        self.total = total
        self.reads: list[str] = []

    def counted(self, path: str, key: str | None = None) -> tuple[list, int | None]:
        items = self.paginate(path, key)
        return items, len(items) if isinstance(self.total, str) else self.total

    def get(self, path: str) -> object:
        self.reads.append(path)
        login = path.split("/")[1]
        if login.endswith("%5Bbot%5D"):
            raise urllib.error.HTTPError(path, 404, "not a user", {}, None)  # type: ignore[arg-type]
        return {"permission": "write" if login in self.writers else "read"}

    def paginate(self, path: str, key: str | None = None) -> list:
        self.reads.append(path)
        if path.startswith("actions/workflows/ci-trust-verdict.yml/runs?"):
            return [run for run, _ in self.pairs]
        if path.startswith("actions/runs/"):
            return [job for run, job in self.pairs if run["id"] == int(path.split("/")[2])]
        raise AssertionError(path)


class DispatchedRecordTests(unittest.TestCase):
    """The approval record that needs no pull request event: only a successful job of a workflow_dispatch run of
    the verdict workflow, started from a commit on the default branch by a user with write access, is one.

    Failing-before: no such record existed, so a head could be approved only through pull_request_target."""

    def records(self, *pairs: tuple[dict, dict], on_main: bool = True, **github) -> list[dict]:
        gh = DispatchGitHub(*pairs, **github)
        with mock.patch.object(ctv, "default_definition", return_value=on_main) as ancestry:
            answer: tuple[list[dict], bool] = ctv.dispatched_records(gh, Path("."), PR, "main", LABELLED)
        self.cut = answer[1]
        self.ancestry = ancestry
        self.gh = gh
        return answer[0]

    def test_the_genuine_record_is_read(self) -> None:
        self.assertEqual(self.records(_dispatched(1)), [
            {"digest": D1, "recorded_at": RECORDED, "run_created_at": LABELLED, "route": "dispatch",
             "head_sha": HEAD, "actor": "maintainer"},
        ])
        self.ancestry.assert_called_once_with(Path("."), MAIN, ctv.VERDICT_WORKFLOW)

    def test_only_runs_since_the_label_are_listed_and_only_titled_runs_have_their_jobs_read(self) -> None:
        plain = _dispatched(2, title=f"Trusted CI verdict (PR #{PR})")
        self.assertEqual(len(self.records(_dispatched(1), plain)), 1)
        listing = self.gh.reads[0]
        self.assertTrue(listing.startswith("actions/workflows/ci-trust-verdict.yml/runs?"), listing)
        self.assertIn("event=workflow_dispatch", listing)
        self.assertIn("branch=main", listing)
        self.assertIn("created=%3E%3D2026-10-28T10%3A30%3A00Z", listing)
        self.assertNotIn("actions/runs/2/jobs?filter=latest", self.gh.reads)

    def test_a_dispatch_from_another_ref_is_ignored(self) -> None:
        # Another branch runs that branch's copy of the workflow, which can name a job anything.
        self.assertEqual(self.records(_dispatched(1, branch="ci/evil")), [])
        # So does a tag named like the default branch: only the commit says whose definition ran.
        self.assertEqual(self.records(_dispatched(1), on_main=False), [])

    def test_a_run_of_another_event_or_workflow_is_ignored(self) -> None:
        for event in ("pull_request", "pull_request_target", "workflow_run", "push"):
            with self.subTest(event=event):
                self.assertEqual(self.records(_dispatched(1, event=event)), [])
        self.assertEqual(self.records(_dispatched(1, path=".github/workflows/evil.yml")), [])

    def test_a_dispatch_by_an_actor_without_write_access_is_ignored(self) -> None:
        self.assertEqual(self.records(_dispatched(1, actor="helper")), [])
        # A workflow's own token can dispatch a workflow; its actor is a bot with no collaborator permission.
        self.assertEqual(self.records(_dispatched(1, actor="github-actions[bot]")), [])
        self.assertEqual(self.records(_dispatched(1, actor=None)), [])
        # A re-run by someone without write access makes the run theirs as well.
        run, job = _dispatched(1)
        run["triggering_actor"] = {"login": "helper"}
        self.assertEqual(self.records((run, job)), [])

    def test_a_truncated_dispatch_list_is_reported(self) -> None:
        """GitHub returns the newest 1,000 dispatches since the label. A record older than that is not seen,
        which leaves the head unapproved; the flag lets the verdict say so. Failing-before: the list was read
        with no count, and such a head was CI_CHANGE_UNAPPROVED with no hint why."""
        for total, cut in (("all", False), (1, False), (2, True), (5000, True), (None, True)):
            with self.subTest(total=total):
                self.assertEqual(len(self.records(_dispatched(1), total=total)), 1)
                self.assertIs(self.cut, cut)
        self.assertTrue(ctv.list_cut([0] * ctv.RUN_LIST_LIMIT, ctv.RUN_LIST_LIMIT))    # a full list, whatever the count
        self.assertFalse(ctv.list_cut([0] * 3, 3))

    def test_the_verdict_names_a_truncated_dispatch_list(self) -> None:
        scenario = Scenario()
        scenario.change("tools/ci_paths.py")
        scenario.label(at=LABELLED, permission="write")
        hint = "dispatch the approval again"
        for total, hinted in ((5000, True), (0, False)):
            with self.subTest(total=total):
                scenario.raw["dispatch_total"] = total
                gh = FakeGitHub(scenario)
                with mock.patch.object(ctv, "git_facts", return_value=_facts(scenario.raw["protected_changes"])), \
                        mock.patch.object(ctv, "approval_digest", return_value=D1):
                    raw = ctv.collect(gh, Path("."), "main", HEAD, PR)
                assert raw is not None
                self.assertIs(raw["approval_list_truncated"], hinted)
                verdict = ctv.evaluate(ctv.normalise(raw))
                self.assertEqual(verdict.state, "failure")
                self.assertEqual(hint in verdict.findings[0].message, hinted, verdict.findings[0].message)

    def test_a_run_created_before_the_label_is_ignored_whatever_github_answered(self) -> None:
        # The filter sent to GitHub is checked again here: a run from before the label answers another approval.
        self.assertEqual(self.records(_dispatched(1, created="2026-10-28T10:29:59Z")), [])
        self.assertEqual(len(self.records(_dispatched(1, created=LABELLED))), 1)

    def test_the_dispatcher_and_whoever_re_ran_the_run_both_need_write_access(self) -> None:
        for actor, rerun_by in (("helper", "maintainer"), ("maintainer", "helper"),
                                ("github-actions[bot]", "maintainer")):
            with self.subTest(actor=actor, rerun_by=rerun_by):
                run, job = _dispatched(1, actor=actor)
                run["triggering_actor"] = {"login": rerun_by}
                self.assertEqual(self.records((run, job)), [])

    def test_a_run_titled_for_another_pull_request_is_not_read(self) -> None:
        other = _dispatched(1, title=ctv.APPROVAL_RUN_TITLE.format(number=PR + 1, head=HEAD))
        self.assertEqual(self.records(other), [])
        self.assertNotIn("actions/runs/1/jobs?filter=latest", self.gh.reads)

    def test_a_record_for_another_pull_request_is_ignored(self) -> None:
        self.assertEqual(self.records(_dispatched(1, number=PR + 1)), [])
        # The title selects the run; the job name is the record, and it must name this pull request too.
        mixed = _dispatched(1, name=f"{ctv.APPROVAL_RECORD_V2_PREFIX}{PR + 1} {HEAD} {D1}")
        self.assertEqual(self.records(mixed), [])

    def test_a_failed_cancelled_or_unfinished_record_is_ignored(self) -> None:
        self.assertEqual(self.records(_dispatched(1, conclusion="failure"), _dispatched(2, conclusion="skipped"),
                                      _dispatched(3, conclusion=None, status="in_progress")), [])

    def test_malformed_names_are_ignored(self) -> None:
        prefix = ctv.APPROVAL_RECORD_V2_PREFIX
        for name in (f"{prefix}{PR} {HEAD} {D1[:-1]}", f"{prefix}{PR} {HEAD[:-1]} {D1}", f"{prefix}{PR} {D1}",
                     f"{prefix}0{PR} {HEAD} {D1}", f"{prefix}{PR} {HEAD.upper()} {D1}", f"{prefix}{PR} {HEAD} {D1} ",
                     f"{prefix}{PR} {OTHER} {D1}",   # a head the run's title does not name
                     f"CI approval record v1 pr{PR} {D1}", f"x{prefix}{PR} {HEAD} {D1}", prefix):
            with self.subTest(name=name):
                self.assertEqual(self.records(_dispatched(1, name=name)), [])

    def test_a_dispatched_record_approves_in_the_collector_without_any_pull_request_target_run(self) -> None:
        scenario = Scenario()
        scenario.drop_runs(ctv.VERDICT_WORKFLOW)
        scenario.change("tools/ci_paths.py")
        scenario.label(at=LABELLED, permission="write")
        run, job = _dispatched(6)
        scenario.raw["dispatch_runs"] = [run]
        scenario.raw["jobs"]["6"] = [job]
        gh = FakeGitHub(scenario)
        with mock.patch.object(ctv, "git_facts", return_value=_facts(scenario.raw["protected_changes"])), \
                mock.patch.object(ctv, "approval_digest", return_value=D1), \
                mock.patch.object(ctv, "default_definition", return_value=True):
            raw = ctv.collect(gh, Path("."), "main", HEAD, PR)
        assert raw is not None
        self.assertEqual([record["route"] for record in raw["approval_records"]], ["dispatch"])
        verdict = ctv.evaluate(ctv.normalise(raw))
        self.assertEqual(verdict.state, "success", verdict.findings)
        self.assertIn("recorded the approval", verdict.findings[0].message)


class DispatchedApprovalTests(VerdictAssertions):
    """What a dispatched record approves: the digest it names, on this pull request, while the label stands."""

    @staticmethod
    def _approved(**record) -> Scenario:
        scenario = Scenario()
        scenario.change("tools/ci_paths.py")
        scenario.label()
        scenario.record(**record)
        return scenario

    def test_a_record_of_this_content_approves_a_later_head_with_the_same_content(self) -> None:
        # The dispatch named an earlier head; a push that changed no protected file keeps the approval.
        self.assertVerdict(self._approved(head=OTHER), "success")

    def test_a_record_of_other_content_does_not_approve(self) -> None:
        self.assertVerdict(self._approved(digest=D2), "failure", "CI_CHANGE_UNAPPROVED")
        self.assertVerdict(self._approved(head_digest=None), "failure", "CI_CHANGE_UNAPPROVED")

    def test_a_record_written_before_the_label_does_not_approve(self) -> None:
        self.assertVerdict(self._approved(at="2026-10-28T10:29:59Z"), "failure", "CI_CHANGE_UNAPPROVED")
        self.assertVerdict(self._approved(at=LABELLED), "failure", "CI_CHANGE_UNAPPROVED")

    def test_a_record_does_not_replace_the_label_or_its_write_access(self) -> None:
        scenario = self._approved()
        scenario.raw["pull_request"]["labels"] = []
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")
        scenario = Scenario()
        scenario.change("tools/ci_paths.py")
        scenario.label(permission="triage", actor="helper")
        scenario.record()
        self.assertVerdict(scenario, "failure", "CI_CHANGE_UNAPPROVED")

    def test_a_retargeted_pull_request_is_approved_only_by_a_dispatched_record(self) -> None:
        """A pull_request_target record may come from another base's workflow; a dispatched one cannot."""
        for route, state in (("dispatch", "success"), ("head", "failure")):
            with self.subTest(route=route):
                scenario = self._approved(route=route)
                scenario.raw["label_events"].append(
                    {"id": 9, "event": "base_ref_changed", "created_at": "2026-10-28T10:20:00Z"})
                self.assertVerdict(scenario, state)

    def test_a_record_of_an_unknown_route_does_not_approve(self) -> None:
        self.assertVerdict(self._approved(route="status"), "failure", "CI_CHANGE_UNAPPROVED")


class CarriedApprovalCollectorTests(unittest.TestCase):
    def collect(self, scenario: Scenario, digest: str = D1, **errors) -> dict:
        gh = FakeGitHub(scenario)
        with mock.patch.object(ctv, "git_facts", return_value=_facts(scenario.raw["protected_changes"])), \
                mock.patch.object(ctv, "approval_digest", return_value=digest, **errors), \
                mock.patch("sys.stderr"):
            raw = ctv.collect(gh, Path("."), "main", HEAD, PR)
        assert raw is not None
        result: dict = raw
        return result

    def scenario(self) -> Scenario:
        scenario = Scenario()
        scenario.change("tools/ci_paths.py")
        scenario.label(at=LABELLED_AT)
        run, job = _record(6, D1)
        scenario.raw["workflow_runs"].append(run)
        scenario.raw["jobs"]["6"] = [job]
        return scenario

    def test_a_recorded_unchanged_head_is_approved(self) -> None:
        raw = self.collect(self.scenario())
        self.assertEqual(raw["approval_records"], [
            {"digest": D1, "recorded_at": RECORDED, "run_created_at": PUSHED, "route": "head", "head_sha": HEAD,
             "actor": None},
        ])
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).state, "success")

    def test_changed_content_is_not(self) -> None:
        self.assertEqual(ctv.evaluate(ctv.normalise(self.collect(self.scenario(), digest=D2))).state, "failure")

    def test_a_failed_digest_computation_drops_the_approval(self) -> None:
        raw = self.collect(self.scenario(), side_effect=subprocess.CalledProcessError(128, "git"))
        self.assertNotIn("head_digest", raw)
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).state, "failure")

    def test_a_failed_record_lookup_drops_the_approval(self) -> None:
        scenario = self.scenario()

        class Failing(FakeGitHub):
            def paginate(self, path: str, key: str | None = None) -> list:
                if path.startswith("actions/runs/6/jobs"):
                    raise urllib.error.URLError("connection reset")
                return super().paginate(path, key)

        with mock.patch.object(ctv, "git_facts", return_value=_facts(scenario.raw["protected_changes"])), \
                mock.patch.object(ctv, "approval_digest", return_value=D1), mock.patch("sys.stderr"):
            raw = ctv.collect(Failing(scenario), Path("."), "main", HEAD, PR)
        assert raw is not None
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).state, "failure")

    def test_nothing_is_hashed_without_a_label_or_without_a_protected_change(self) -> None:
        unlabelled = self.scenario()
        unlabelled.raw["pull_request"]["labels"] = []
        behind = self.scenario()
        behind.raw["protected_changes"] = [{"path": CI, "kind": "behind"}]
        for scenario in (unlabelled, behind):
            gh = FakeGitHub(scenario)
            with mock.patch.object(ctv, "git_facts", return_value=_facts(scenario.raw["protected_changes"])), \
                    mock.patch.object(ctv, "approval_digest") as digest:
                ctv.collect(gh, Path("."), "main", HEAD, PR)
            digest.assert_not_called()


def _push(after: str, at: str | None, *, kind: str = "push", ref: str = "refs/heads/ci/topic") -> dict:
    """One record of GitHub's repository activity: a branch was moved to ``after`` at ``at``."""
    return {"id": 1, "ref": ref, "before": "0" * 40, "after": after, "timestamp": at, "activity_type": kind}


class ApprovalGitHub(ctv.GitHub):
    """The label workflow's reads: the pull request, its label events, a user's permission and recorded runs."""

    def __init__(self, *, head: str = HEAD, labels: tuple[str, ...] = (ctv.APPROVAL_LABEL,),
                 permission: str | None = "write", events: list[dict] | None = None,
                 records: tuple[tuple[dict, dict], ...] = (), broken: str | None = None,
                 dispatches: tuple[tuple[dict, dict], ...] = (), pushes: list[dict] | None = None) -> None:
        super().__init__("Jstar269/nakagawa-recomp", "token")
        self.dispatches = list(dispatches)
        # GitHub's activity of the head branch: by default one push, an hour before the label, made HEAD the head.
        self.pushes = pushes if pushes is not None else [_push(HEAD, "2026-10-28T09:00:00Z")]
        self.pull = _pull_request(head=head, labels=labels)
        self.reads: list[str] = []
        self.permission = permission
        self.events = events if events is not None else [{
            "id": 1, "event": "labeled", "created_at": LABELLED_AT, "actor": {"login": "maintainer"},
            "label": {"name": ctv.APPROVAL_LABEL},
        }]
        self.records = list(records)
        self.broken = broken

    def _check(self, path: str) -> None:
        if self.broken and path.startswith(self.broken):
            raise urllib.error.URLError("down")

    def counted(self, path: str, key: str | None = None) -> tuple[list, int | None]:
        items = self.paginate(path, key)
        return items, len(items)

    def get(self, path: str) -> object:
        self._check(path)
        if path == f"pulls/{PR}":
            return self.pull
        if path.startswith("collaborators/"):
            return {"permission": self.permission}
        raise AssertionError(path)

    def paginate(self, path: str, key: str | None = None) -> list:
        self._check(path)
        if path == f"issues/{PR}/events":
            return self.events
        if path.startswith("actions/runs?head_sha="):
            sha = path.split("=", 1)[1]
            return [run for run, _ in self.records if run["head_sha"] == sha]
        if path.startswith("actions/workflows/ci-trust-verdict.yml/runs?"):
            return [run for run, _ in self.dispatches]
        if path.startswith("actions/runs/"):
            run_id = int(path.split("/")[2])
            return [job for run, job in self.records + self.dispatches if run["id"] == run_id]
        if path.startswith("https://api.github.com/repos/") and "/activity?" in path:
            self.reads.append(path)
            return self.pushes
        raise AssertionError(path)


class RecordAndCarryTests(unittest.TestCase):
    """The label workflow's two steps: record on a label, carry on a push. Any doubt refuses."""

    def record(self, gh: ApprovalGitHub, digest: str = D1) -> str:
        with mock.patch.object(ctv, "candidate_digest", return_value=digest):
            digest_out: str = ctv.record_approval(gh, Path("."), PR, HEAD, "main")
            return digest_out

    def carry(self, gh: ApprovalGitHub, digest: str = D1, before: str = BEFORE) -> str:
        with mock.patch.object(ctv, "candidate_digest", return_value=digest):
            digest_out: str = ctv.carry_approval(gh, Path("."), PR, before, HEAD, "main")
            return digest_out

    def test_a_label_from_a_user_with_write_records_the_digest(self) -> None:
        self.assertEqual(self.record(ApprovalGitHub()), D1)

    def test_a_label_from_a_user_without_write_records_nothing(self) -> None:
        for permission in ("triage", "read", "none", None):
            with self.subTest(permission=permission), self.assertRaises(ctv.ApprovalRefused):
                self.record(ApprovalGitHub(permission=permission))

    def test_a_removed_label_or_a_moved_head_records_nothing(self) -> None:
        with self.assertRaises(ctv.ApprovalRefused):
            self.record(ApprovalGitHub(labels=()))
        with self.assertRaises(ctv.ApprovalRefused):
            self.record(ApprovalGitHub(head=OTHER))   # the head moved after the event named HEAD
        unlabeled = [{"id": 2, "event": "unlabeled", "created_at": "2026-10-28T10:00:00Z",
                      "actor": {"login": "maintainer"}, "label": {"name": ctv.APPROVAL_LABEL}}]
        with self.assertRaises(ctv.ApprovalRefused):
            self.record(ApprovalGitHub(events=ApprovalGitHub().events + unlabeled))

    def test_the_head_moving_while_it_is_hashed_records_nothing(self) -> None:
        gh = ApprovalGitHub()

        def move(*_args, **_kwargs) -> str:
            gh.pull["head"]["sha"] = OTHER
            return D1

        with mock.patch.object(ctv, "candidate_digest", side_effect=move), self.assertRaises(ctv.ApprovalRefused):
            ctv.record_approval(gh, Path("."), PR, HEAD, "main")

    def test_an_unchanged_digest_is_carried(self) -> None:
        self.assertEqual(self.carry(ApprovalGitHub(records=(_record(6, D1, sha=BEFORE),))), D1)

    def test_a_record_written_under_an_earlier_label_is_not_carried(self) -> None:
        """A maintainer approves one head, the label is removed, and a maintainer labels other bytes. With
        pull_request_target blocked the pull request is pushed back to the first head, and the label stays
        because no handler runs. The next push of unrelated files must not carry the first head's old record
        forward under the new label. Failing-before: a carry took any record at the previous head."""
        old = _record(6, D1, sha=BEFORE, created="2026-10-28T09:00:00Z", completed="2026-10-28T09:01:00Z")
        with self.assertRaises(ctv.ApprovalRefused):
            self.carry(ApprovalGitHub(records=(old,)))          # the label is from 09:59
        # Re-running that old run after the label gives its job a later time, never its run.
        rerun = _record(6, D1, sha=BEFORE, created="2026-10-28T09:00:00Z", completed="2026-10-28T10:30:00Z")
        with self.assertRaises(ctv.ApprovalRefused):
            self.carry(ApprovalGitHub(records=(rerun,)))
        current = _record(7, D1, sha=BEFORE, created=LABELLED_AT, completed="2026-10-28T10:00:00Z")
        self.assertEqual(self.carry(ApprovalGitHub(records=(old, current))), D1)

    def test_a_dispatched_approval_is_carried_without_a_record_at_the_previous_head(self) -> None:
        """A push that changes no protected file keeps an approval that was recorded by dispatch.
        Failing-before: no dispatched record existed, and the push removed the label."""
        with mock.patch.object(ctv, "default_definition", return_value=True):
            self.assertEqual(self.carry(ApprovalGitHub(dispatches=(_dispatched(8, created=PUSHED),))), D1)
            with self.assertRaises(ctv.ApprovalRefused):        # the new head's protected content differs
                self.carry(ApprovalGitHub(dispatches=(_dispatched(8, created=PUSHED),)), digest=D2)
            with self.assertRaises(ctv.ApprovalRefused):        # dispatched before the current label
                self.carry(ApprovalGitHub(dispatches=(_dispatched(8, created="2026-10-28T09:00:00Z"),)))
        with mock.patch.object(ctv, "default_definition", return_value=False), \
                self.assertRaises(ctv.ApprovalRefused):         # not the default branch's copy of the workflow
            self.carry(ApprovalGitHub(dispatches=(_dispatched(8, created=PUSHED),)))

    def test_a_changed_digest_is_not_carried(self) -> None:
        with self.assertRaises(ctv.ApprovalRefused):
            self.carry(ApprovalGitHub(records=(_record(6, D1, sha=BEFORE),)), digest=D2)

    def test_no_record_at_the_previous_head_is_not_carried(self) -> None:
        with self.assertRaises(ctv.ApprovalRefused):
            self.carry(ApprovalGitHub())

    def test_a_forged_record_at_the_previous_head_is_not_carried(self) -> None:
        for forged in (_record(6, D1, sha=BEFORE, event="pull_request"),
                       _record(6, D1, sha=BEFORE, path=".github/workflows/evil.yml"),
                       _record(6, D1, sha=BEFORE, conclusion="failure"),
                       _record(6, D1, sha=HEAD)):   # a record at the NEW head says nothing about the old one
            with self.subTest(forged=forged[0]["path"], event=forged[0]["event"]), self.assertRaises(ctv.ApprovalRefused):
                self.carry(ApprovalGitHub(records=(forged,)))

    def test_a_record_at_an_older_head_is_not_carried_through_a_gap(self) -> None:
        with self.assertRaises(ctv.ApprovalRefused):
            self.carry(ApprovalGitHub(records=(_record(6, D1, sha=OTHER),)))

    def test_a_label_that_is_not_from_write_is_not_carried(self) -> None:
        with self.assertRaises(ctv.ApprovalRefused):
            self.carry(ApprovalGitHub(permission="triage", records=(_record(6, D1, sha=BEFORE),)))

    def test_a_push_that_did_not_move_the_head_is_refused(self) -> None:
        with self.assertRaises(ctv.ApprovalRefused):
            self.carry(ApprovalGitHub(records=(_record(6, D1, sha=HEAD),)), before=HEAD)

    def test_a_closed_pull_request_or_other_base_is_not_carried(self) -> None:
        for change in ({"state": "closed"}, {"base": {"ref": "release"}}):
            gh = ApprovalGitHub(records=(_record(6, D1, sha=BEFORE),))
            gh.pull |= change
            with self.subTest(change=change), self.assertRaises(ctv.ApprovalRefused):
                self.carry(gh)

    def test_the_candidate_ref_not_matching_the_head_refuses_the_digest(self) -> None:
        with mock.patch.object(ctv, "fetch_candidate", return_value=OTHER), self.assertRaises(ctv.ApprovalRefused):
            ctv.candidate_digest(ApprovalGitHub(), Path("."), PR, HEAD)

    def _main(self, gh: ApprovalGitHub, *args: str, output: Path) -> tuple[int, str]:
        with mock.patch.object(ctv, "GitHub", return_value=gh), mock.patch.dict(os.environ, {"GITHUB_TOKEN": "t"}), \
                mock.patch.object(ctv, "candidate_digest", return_value=D1), \
                mock.patch("sys.stdout"), mock.patch("sys.stderr"):
            code = ctv.main(["--repository", gh.repository, "--pull-request", str(PR), "--head-sha", HEAD,
                             "--github-output", str(output), *args])
        return code, output.read_text(encoding="utf-8") if output.exists() else ""

    def test_the_command_writes_the_digest_for_the_job_name_only_on_success(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "out"
            records = (_record(6, D1, sha=BEFORE),)
            self.assertEqual(self._main(ApprovalGitHub(), "--approval", "record", output=output), (0, f"digest={D1}\n"))
            output.unlink()
            self.assertEqual(self._main(ApprovalGitHub(records=records), "--approval", "carry",
                                        "--before-sha", BEFORE, output=output), (0, f"digest={D1}\n"))
            output.unlink()
            # Fail closed: no record, an API failure, or a malformed SHA leave the output empty and exit non-zero.
            self.assertEqual(self._main(ApprovalGitHub(), "--approval", "carry", "--before-sha", BEFORE,
                                        output=output), (1, ""))
            self.assertEqual(self._main(ApprovalGitHub(records=records, broken="actions/runs"), "--approval", "carry",
                                        "--before-sha", BEFORE, output=output), (1, ""))
            self.assertEqual(self._main(ApprovalGitHub(broken="issues/"), "--approval", "record", output=output),
                             (1, ""))
            self.assertEqual(self._main(ApprovalGitHub(), "--approval", "carry", "--before-sha", "nope",
                                        output=output), (2, ""))

    def test_a_dispatched_approval_is_refused_unless_the_label_and_the_named_head_hold(self) -> None:
        def dispatch(gh: ApprovalGitHub) -> str:
            with mock.patch.object(ctv, "candidate_digest", return_value=D1):
                digest: str = ctv.record_approval(gh, Path("."), PR, HEAD, "main", dispatched=True)
                return digest

        self.assertEqual(dispatch(ApprovalGitHub()), D1)
        # The dispatcher names the head they reviewed; a pull request that has moved on is not that head.
        for refused in (ApprovalGitHub(head=OTHER), ApprovalGitHub(labels=()), ApprovalGitHub(permission="triage")):
            with self.assertRaises(ctv.ApprovalRefused):
                dispatch(refused)

    def _dispatch(self, gh: ApprovalGitHub) -> str:
        with mock.patch.object(ctv, "candidate_digest", return_value=D1):
            digest: str = ctv.record_approval(gh, Path("."), PR, HEAD, "main", dispatched=True)
            return digest

    def _refused(self, gh: ApprovalGitHub, code: str) -> None:
        with self.assertRaises(ctv.ApprovalRefused) as refusal:
            self._dispatch(gh)
        self.assertTrue(str(refusal.exception).startswith(code + ": "), str(refusal.exception))

    @staticmethod
    def _label(event_id: int, event: str, at: str) -> dict:
        return {"id": event_id, "event": event, "created_at": at, "actor": {"login": "maintainer"},
                "label": {"name": ctv.APPROVAL_LABEL}}

    def test_a_label_older_than_the_head_is_refused(self) -> None:
        """A label is applied for one head and the pull request is pushed. With pull_request_target refused
        nothing removes the label, and a dispatch that names the new head finds "the label is on, and this is
        the head". Failing-before: it recorded an approval of a head nobody had read."""
        stale = ApprovalGitHub(pushes=[_push(OTHER, "2026-10-28T09:00:00Z"), _push(HEAD, "2026-10-28T10:00:00Z")])
        self._refused(stale, "LABEL_PREDATES_HEAD")          # the label is from 09:59
        self.assertEqual(stale.reads, [
            "https://api.github.com/repos/Jstar269/nakagawa-recomp/activity?ref=refs%2Fheads%2Fci%2Ftopic",
        ])
        # A label in the push's own second cannot be ordered against it.
        self._refused(ApprovalGitHub(pushes=[_push(HEAD, LABELLED_AT)]), "LABEL_PREDATES_HEAD")
        self._refused(ApprovalGitHub(pushes=[_push(HEAD, "2026-10-28T09:59:00.400Z")]), "LABEL_PREDATES_HEAD")
        self.assertEqual(self._dispatch(ApprovalGitHub(pushes=[_push(HEAD, "2026-10-28T09:58:59Z")])), D1)

    def test_a_label_applied_again_after_the_push_is_accepted(self) -> None:
        pushes = [_push(HEAD, "2026-10-28T10:00:00Z")]
        events = [self._label(1, "labeled", LABELLED_AT), self._label(2, "unlabeled", "2026-10-28T10:05:00Z"),
                  self._label(3, "labeled", "2026-10-28T10:06:00Z")]
        self.assertEqual(self._dispatch(ApprovalGitHub(pushes=pushes, events=events)), D1)
        # Removed and applied again in one second: the events are ordered by time, then id.
        same_second = [self._label(1, "labeled", LABELLED_AT), self._label(2, "unlabeled", "2026-10-28T10:06:00Z"),
                       self._label(3, "labeled", "2026-10-28T10:06:00Z")]
        self.assertEqual(self._dispatch(ApprovalGitHub(pushes=pushes, events=same_second)), D1)
        reversed_ids = [self._label(1, "labeled", LABELLED_AT), self._label(3, "unlabeled", "2026-10-28T10:06:00Z"),
                        self._label(2, "labeled", "2026-10-28T10:06:00Z")]
        with self.assertRaises(ctv.ApprovalRefused):
            self._dispatch(ApprovalGitHub(pushes=pushes, events=reversed_ids))

    def test_a_force_push_back_to_an_earlier_commit_is_dated_by_its_latest_arrival(self) -> None:
        """HEAD is pushed at 09:00, replaced at 09:30, the replacement is labelled at 09:59, and the pull
        request is force-pushed back to HEAD at 10:10. HEAD's first arrival and its earliest runs predate the
        label; its latest arrival does not."""
        pushes = [_push(HEAD, "2026-10-28T10:10:00Z", kind="force_push"), _push(OTHER, "2026-10-28T09:30:00Z"),
                  _push(HEAD, "2026-10-28T09:00:00Z")]
        self._refused(ApprovalGitHub(pushes=pushes), "LABEL_PREDATES_HEAD")
        self._refused(ApprovalGitHub(pushes=list(reversed(pushes))), "LABEL_PREDATES_HEAD")   # whatever the order
        # The pull request's own events are a second witness: a force push after the label, even where the
        # activity list shows only the first arrival.
        witnessed = ApprovalGitHub()
        witnessed.events.append({"id": 5, "event": "head_ref_force_pushed", "created_at": "2026-10-28T10:10:00Z"})
        self._refused(witnessed, "LABEL_PREDATES_HEAD")
        earlier = ApprovalGitHub()
        earlier.events.append({"id": 5, "event": "head_ref_force_pushed", "created_at": "2026-10-28T09:30:00Z"})
        self.assertEqual(self._dispatch(earlier), D1)
        undated = ApprovalGitHub()
        undated.events.append({"id": 5, "event": "head_ref_force_pushed", "created_at": None})
        self._refused(undated, "HEAD_TIME_UNREADABLE")

    def test_an_unreadable_push_time_is_refused(self) -> None:
        cases = {
            "no records at all": ApprovalGitHub(pushes=[]),
            "a record without a time": ApprovalGitHub(pushes=[_push(HEAD, None)]),
            "a record with an unreadable time": ApprovalGitHub(pushes=[_push(HEAD, "09:00 yesterday")]),
            "one dated and one undated record": ApprovalGitHub(
                pushes=[_push(HEAD, "2026-10-28T09:00:00Z"), _push(HEAD, None)]),
            "a record of another branch": ApprovalGitHub(
                pushes=[_push(HEAD, "2026-10-28T09:00:00Z", ref="refs/heads/other")]),
            "an undated record of another commit": ApprovalGitHub(
                pushes=[_push(HEAD, "2026-10-28T09:00:00Z"), _push(OTHER, None)]),
            "the list cannot be read": ApprovalGitHub(broken="https://api.github.com/repos/"),
            "an error document instead of a list": ApprovalGitHub(pushes=["message"]),   # type: ignore[list-item]
        }
        for name, gh in cases.items():
            with self.subTest(case=name):
                self._refused(gh, "HEAD_TIME_UNREADABLE")
        for head in ({"sha": HEAD, "ref": "ci/topic", "repo": None}, {"sha": HEAD, "repo": {"full_name": "a/b"}},
                     {"sha": HEAD, "ref": "ci/topic", "repo": {"full_name": "a/b/../c"}}):
            with self.subTest(head=head):
                gh = ApprovalGitHub()
                gh.pull["head"] = head
                self._refused(gh, "HEAD_TIME_UNREADABLE")

    def test_the_refusal_says_how_the_list_could_not_be_read(self) -> None:
        # 403, 404 and a server error call for different actions.
        for code in (403, 404, 503):
            gh = ApprovalGitHub()
            error = urllib.error.HTTPError("https://api.github.com/x", code, "no", {}, None)  # type: ignore[arg-type]
            with self.subTest(code=code), mock.patch.object(gh, "paginate", side_effect=error), \
                    self.assertRaises(ctv.ApprovalRefused) as refusal:
                ctv.head_pushed_at(gh, gh.pull, HEAD)
            self.assertIn(f"(HTTP {code})", str(refusal.exception))

    def test_the_newest_record_of_the_branch_must_be_the_head(self) -> None:
        """The activity list can lag a push and has been seen to lack a record. The pull request is at HEAD, the
        list shows HEAD at 09:00 and another commit at 09:30, and the label is from 09:59: a complete list
        cannot look like that while HEAD is the tip, so the update that brought HEAD back is missing, and it
        may be newer than the label (pushed, rewound, labelled, fast-forwarded again: no force-push event backs
        the list up). Failing-before: HEAD's older record dated the head and the approval was recorded."""
        behind = [_push(OTHER, "2026-10-28T09:30:00Z"), _push(HEAD, "2026-10-28T09:00:00Z")]
        self._refused(ApprovalGitHub(pushes=behind), "HEAD_NOT_NEWEST_PUSH")
        self._refused(ApprovalGitHub(pushes=list(reversed(behind))), "HEAD_NOT_NEWEST_PUSH")
        self._refused(ApprovalGitHub(pushes=[_push(OTHER, "2026-10-28T09:00:00Z")]), "HEAD_NOT_NEWEST_PUSH")
        # Two updates in one second cannot be ordered, so both have to be this commit's.
        tied = [_push(HEAD, "2026-10-28T09:30:00Z"), _push(OTHER, "2026-10-28T09:30:00Z")]
        self._refused(ApprovalGitHub(pushes=tied), "HEAD_NOT_NEWEST_PUSH")
        same = [_push(HEAD, "2026-10-28T09:30:00Z"), _push(HEAD, "2026-10-28T09:30:00Z", kind="force_push"),
                _push(OTHER, "2026-10-28T09:00:00Z")]
        self.assertEqual(self._dispatch(ApprovalGitHub(pushes=same)), D1)
        # Another branch's records do not count either way.
        other_branch = [_push(HEAD, "2026-10-28T09:00:00Z"),
                        _push(OTHER, "2026-10-28T09:30:00Z", ref="refs/heads/other")]
        self.assertEqual(self._dispatch(ApprovalGitHub(pushes=other_branch)), D1)

    def test_a_label_older_than_a_re_open_or_a_restored_branch_is_refused(self) -> None:
        """GitHub leaves a closed pull request's head where it was; commits pushed while it is closed arrive when
        it is re-opened. Closed at A, pushed to B, labelled while closed (the page shows A), re-opened: B's push
        predates the label. The branch's log dates the branch, so the pull request's own events are read too.
        Failing-before: the dispatch recorded B."""
        for event in ("reopened", "head_ref_restored", "head_ref_force_pushed"):
            with self.subTest(event=event):
                after = ApprovalGitHub()
                after.events.append({"id": 5, "event": event, "created_at": "2026-10-28T10:10:00Z"})
                self._refused(after, "LABEL_PREDATES_HEAD")
                tied = ApprovalGitHub()
                tied.events.append({"id": 5, "event": event, "created_at": LABELLED_AT})
                self._refused(tied, "LABEL_PREDATES_HEAD")
                before = ApprovalGitHub()
                before.events.append({"id": 5, "event": event, "created_at": "2026-10-28T09:30:00Z"})
                self.assertEqual(self._dispatch(before), D1)
                undated = ApprovalGitHub()
                undated.events.append({"id": 5, "event": event})
                self._refused(undated, "HEAD_TIME_UNREADABLE")

    def test_a_label_older_than_a_retarget_is_refused(self) -> None:
        """The labeller read the change against one base; a retarget afterwards changes what the head is compared
        with. Failing-before: the dispatch route ignored base_ref_changed altogether."""
        after = ApprovalGitHub()
        after.events.append({"id": 5, "event": "base_ref_changed", "created_at": "2026-10-28T10:10:00Z"})
        self._refused(after, "LABEL_PREDATES_BASE")
        # A retarget before the label is what the labeller saw, and stays allowed.
        before = ApprovalGitHub()
        before.events.append({"id": 5, "event": "base_ref_changed", "created_at": "2026-10-28T09:30:00Z"})
        self.assertEqual(self._dispatch(before), D1)
        # The newest change names the refusal.
        both = ApprovalGitHub(pushes=[_push(HEAD, "2026-10-28T10:20:00Z")])
        both.events.append({"id": 5, "event": "base_ref_changed", "created_at": "2026-10-28T10:10:00Z"})
        self._refused(both, "LABEL_PREDATES_HEAD")

    def test_a_fork_pull_requests_push_is_read_from_the_fork(self) -> None:
        gh = ApprovalGitHub(pushes=[_push(HEAD, "2026-10-28T09:00:00Z", ref="refs/heads/fix #1")])
        gh.pull["head"] = {"sha": HEAD, "ref": "fix #1", "repo": {"full_name": "someone/nakagawa-recomp"}}
        self.assertEqual(self._dispatch(gh), D1)
        # The branch name is the author's text: it reaches the request only as an encoded query value.
        self.assertEqual(gh.reads, [
            "https://api.github.com/repos/someone/nakagawa-recomp/activity?ref=refs%2Fheads%2Ffix+%231",
        ])

    def test_a_label_event_record_is_not_dated_against_the_push(self) -> None:
        # On a `labeled` event the event itself names the head; only a dispatch has to prove the label is newer.
        gh = ApprovalGitHub(pushes=[])
        with mock.patch.object(ctv, "candidate_digest", return_value=D1):
            self.assertEqual(ctv.record_approval(gh, Path("."), PR, HEAD, "main"), D1)
        self.assertEqual(gh.reads, [])

    def test_the_dispatch_command_names_a_stale_label(self) -> None:
        gh = ApprovalGitHub(pushes=[_push(HEAD, "2026-10-28T10:00:00Z")])
        errors: list[str] = []
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(ctv, "GitHub", return_value=gh), \
                mock.patch.dict(os.environ, {"GITHUB_TOKEN": "t"}), \
                mock.patch.object(ctv, "candidate_digest", return_value=D1), \
                mock.patch("sys.stdout") as stdout, mock.patch("sys.stderr") as stderr:
            output = Path(tmp) / "out"
            code = ctv.main(["--repository", gh.repository, "--pull-request", str(PR), "--head-sha", HEAD,
                             "--github-output", str(output), "--approval", "dispatch"])
            errors = [str(call.args[0]) for call in stderr.write.call_args_list if call.args]
            printed = "".join(str(call.args[0]) for call in stdout.write.call_args_list if call.args)
            self.assertFalse(output.exists())
        self.assertEqual(code, 1)
        self.assertIn("approval not recorded by dispatch: ApprovalRefused: LABEL_PREDATES_HEAD: ", "".join(errors))
        # The run's page shows the code alone; the text, which can name the author's branch, stays in the log.
        annotations = [line for line in printed.splitlines() if line.startswith("::")]
        self.assertEqual(annotations, ["::error::approval not recorded by dispatch: LABEL_PREDATES_HEAD"])

    def test_a_dispatched_approval_does_not_depend_on_the_base_a_pull_request_once_had(self) -> None:
        """A label-event record comes from the base branch's workflow, so a retargeted pull request is refused
        one. A dispatch runs the default branch's workflow whatever the base was. Failing-before: with no
        dispatch route a retargeted pull request could be approved only by the time of its label."""
        gh = ApprovalGitHub()
        gh.events.append({"id": 9, "event": "base_ref_changed", "created_at": "2026-10-28T09:00:00Z"})
        with mock.patch.object(ctv, "candidate_digest", return_value=D1):
            with self.assertRaises(ctv.ApprovalRefused):
                ctv.record_approval(gh, Path("."), PR, HEAD, "main")
            self.assertEqual(ctv.record_approval(gh, Path("."), PR, HEAD, "main", dispatched=True), D1)

    def test_the_dispatch_command_writes_every_part_of_the_record_name(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "out"
            self.assertEqual(self._main(ApprovalGitHub(), "--approval", "dispatch", output=output),
                             (0, f"digest={D1}\nnumber={PR}\nhead_sha={HEAD}\n"))
            output.unlink()
            self.assertEqual(self._main(ApprovalGitHub(labels=()), "--approval", "dispatch", output=output), (1, ""))
            self.assertEqual(self._main(ApprovalGitHub(head=OTHER), "--approval", "dispatch", output=output), (1, ""))


class ApprovalDigestTests(unittest.TestCase):
    """The digest over a real repository: what changes it, and what must not."""

    WORKFLOW = "jobs:\n  a:\n    steps:\n      - run: python tools/gate.py\n"

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.repo = Path(self._tmp.name) / "origin"
        self.repo.mkdir()
        self.env = os.environ | {
            "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid",
            "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.invalid",
        }
        self.git("init", "-q", "-b", "main")
        # Without this, ``git add -A`` in push() re-reads the working tree on Linux and resets an index-only
        # mode change (update-index --chmod) back to 0644 before it is committed.
        self.git("config", "core.fileMode", "false")
        self.write(".github/workflows/ci.yml", self.WORKFLOW)
        self.write("tools/gate.py", "print('gate')\n")
        self.write("tools/other.py", "print('not a CI tool')\n")
        self.write("tools/ci_trust_verdict.py", "# the classifier\n")
        self.write("src/x.c", "int x;\n")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "base")
        self.trusted = Path(self._tmp.name) / "trusted"
        subprocess.run(["git", "clone", "-q", "--single-branch", "-b", "main", str(self.repo), str(self.trusted)],
                       check=True, capture_output=True, env=self.env)
        self.git("checkout", "-q", "-b", "candidate")

    def git(self, *args: str) -> str:
        return subprocess.run(["git", "-C", str(self.repo), *args], check=True, capture_output=True, text=True,
                              env=self.env).stdout.strip()

    def write(self, path: str, text: str) -> None:
        target = self.repo / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8", newline="\n")

    def push(self, message: str = "push") -> str:
        """Commit the working tree as a new pull-request head, fetch it into the trusted clone, and hash it."""
        self.git("add", "-A")
        self.git("commit", "-q", "--allow-empty", "-m", message)
        head = self.git("rev-parse", "HEAD")
        self.git("update-ref", f"refs/pull/{PR}/head", head)
        self.head = head
        gh = ctv.GitHub("owner/repo", "token")
        fetched = ctv.fetch_candidate(self.trusted, gh.repository, PR, remote=str(self.repo))
        self.assertEqual(fetched, head)
        digest: str = ctv.candidate_digest(gh, self.trusted, PR, head, remote=str(self.repo))
        return digest

    def test_a_change_to_no_protected_path_keeps_the_digest(self) -> None:
        first = self.push()
        self.write("src/x.c", "int x = 1;\n")
        self.write("tools/other.py", "print('still not a CI tool')\n")
        self.write("docs/CONTRIBUTING.md", "text\n")
        self.assertEqual(self.push(), first)
        self.assertEqual(self.push("empty"), first)

    def test_a_change_to_a_protected_path_changes_the_digest(self) -> None:
        first = self.push()
        for path, text in ((".github/workflows/ci.yml", self.WORKFLOW + "      - run: true\n"),
                           ("tools/gate.py", "raise SystemExit(0)\n"),
                           ("tools/ci_trust_verdict.py", "# the classifier, changed\n")):
            with self.subTest(path=path):
                self.write(path, text)
                changed = self.push()
                self.assertNotEqual(changed, first)
                self.git("checkout", "-q", "HEAD~1", "--", path)
                self.assertEqual(self.push(), first)   # changing a file and reverting it is the same content

    def test_a_pattern_matching_path_that_is_added_changes_the_digest(self) -> None:
        first = self.push()
        for path in (".github/workflows/new.yml", ".github/actions/x/action.yml"):
            with self.subTest(path=path):
                self.write(path, "name: x\n")
                self.assertNotEqual(self.push(), first)
                self.git("rm", "-q", path)
                self.assertEqual(self.push(), first)

    def test_a_protected_path_that_is_removed_or_renamed_changes_the_digest(self) -> None:
        first = self.push()
        self.git("rm", "-q", "tools/gate.py")
        self.assertNotEqual(self.push(), first)
        self.git("checkout", "-q", "main", "--", "tools/gate.py")
        self.assertEqual(self.push(), first)
        self.git("mv", "tools/gate.py", "tools/gate2.py")   # same bytes under a name no workflow runs
        renamed = self.push()
        self.assertNotEqual(renamed, first)
        self.git("mv", "tools/gate2.py", "tools/gate.py")
        self.git("mv", ".github/workflows/ci.yml", ".github/workflows/ci2.yml")
        self.assertNotIn(self.push(), (first, renamed))

    def test_a_tool_that_only_the_candidates_workflow_names_is_hashed(self) -> None:
        """Failing-before: a head adds a gate tool through its own workflow edit and the label is applied; a later
        push turns the tool into ``exit 0`` without touching a workflow, and the digest did not change."""
        self.write(".github/workflows/ci.yml", self.WORKFLOW + "      - run: python tools/new_gate.py\n")
        self.write("tools/new_gate.py", "print('real gate')\n")
        approved = self.push()
        self.write("tools/new_gate.py", "raise SystemExit(0)\n")
        self.assertNotEqual(self.push(), approved)
        # The same holds for an existing tool that only the candidate's workflow newly runs.
        self.write(".github/workflows/ci.yml", self.WORKFLOW + "      - run: python tools/other.py\n")
        self.write("tools/other.py", "print('now a gate')\n")
        newly_run = self.push()
        self.write("tools/other.py", "raise SystemExit(0)\n")
        self.assertNotEqual(self.push(), newly_run)
        # ... and for an action's own workflow-style file.
        self.write(".github/actions/x/action.yml", "runs:\n  steps:\n    - run: python tools/third.py\n")
        self.write("tools/third.py", "print(1)\n")
        third = self.push()
        self.write("tools/third.py", "print(2)\n")
        self.assertNotEqual(self.push(), third)

    def test_a_file_mode_change_changes_the_digest(self) -> None:
        first = self.push()
        self.git("update-index", "--chmod=+x", "tools/gate.py")
        self.assertNotEqual(self.push(), first)

    def test_the_trusted_classification_not_the_candidates_picks_the_paths(self) -> None:
        """A candidate that edits a workflow to stop naming a tool still has that tool hashed: the tool set comes
        from the default branch's workflows."""
        self.write(".github/workflows/ci.yml", "jobs: {}\n")
        first = self.push()
        self.write("tools/gate.py", "raise SystemExit(0)\n")
        self.assertNotEqual(self.push(), first)

    def test_carry_on_a_real_repository(self) -> None:
        """Failing-before: every push withdrew the approval. Now a push of unrelated files carries it and a push of
        a CI file does not."""
        approved = self.push()
        before = self.head
        gh = ApprovalGitHub(records=(_record(6, approved, sha=before),))

        def carry() -> str:
            gh.pull["head"]["sha"] = self.head
            digest: str = ctv.carry_approval(gh, self.trusted, PR, before, self.head, "main", remote=str(self.repo))
            return digest

        self.write("src/x.c", "int x = 2;\n")
        self.push()
        self.assertEqual(carry(), approved)
        self.write("tools/gate.py", "raise SystemExit(0)\n")
        self.push()
        with self.assertRaises(ctv.ApprovalRefused):
            carry()


class ApprovalWorkflowWiringTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = (ROOT / ctv.VERDICT_WORKFLOW).read_text(encoding="utf-8")

    def test_the_record_job_name_is_what_the_verifier_reads(self) -> None:
        match = re.search(r"(?m)^    name: (CI approval record v1 pr\$\{\{ github\.event\.pull_request\.number \}\} "
                          r"\$\{\{ needs\.approval\.outputs\.digest \}\})$", self.text)
        assert match is not None
        name = match.group(1).replace("${{ needs.approval.outputs.digest }}", D1)
        name = name.replace("${{ github.event.pull_request.number }}", str(PR))
        self.assertRegex(name, ctv._APPROVAL_RECORD)
        self.assertTrue(name.startswith(ctv.APPROVAL_RECORD_PREFIX))

    def test_the_dispatched_record_job_name_is_what_the_verifier_reads(self) -> None:
        """Failing-before: the workflow had no dispatched record job."""
        outputs = "needs.approval_dispatch.outputs"
        match = re.search(r"(?m)^    name: (CI approval record v2 pr\$\{\{ " + re.escape(outputs) + r"\.number \}\} "
                          r"\$\{\{ " + re.escape(outputs) + r"\.head_sha \}\} "
                          r"\$\{\{ " + re.escape(outputs) + r"\.digest \}\})$", self.text)
        assert match is not None
        name = match.group(1)
        # Every part is an output of the trusted tool; a dispatch input never reaches the name as typed.
        self.assertNotIn("inputs.", name)
        self.assertNotIn("github.event", name)
        for output, value in (("number", str(PR)), ("head_sha", HEAD), ("digest", D1)):
            name = name.replace("${{ " + outputs + "." + output + " }}", value)
        self.assertRegex(name, ctv._APPROVAL_RECORD_V2)
        self.assertEqual(name, f"{ctv.APPROVAL_RECORD_V2_PREFIX}{PR} {HEAD} {D1}")

    def test_the_approval_dispatch_is_titled_as_the_verifier_looks_it_up(self) -> None:
        header = self.text[:self.text.index("\non:\n")]
        title = ctv.APPROVAL_RUN_TITLE.format(number="{0}", head="{1}")
        self.assertIn(f"format('{title}', inputs.pull_request_number, inputs.approve_head_sha)", header)
        self.assertRegex(ctv.APPROVAL_RUN_TITLE.format(number=PR, head=HEAD), ctv._APPROVAL_RUN_TITLE)

    def test_the_dispatched_approval_runs_default_branch_code_and_cannot_post_or_label(self) -> None:
        job = self.text[self.text.index("\n  approval_dispatch:\n"):self.text.index("\n  approval_record_dispatched:\n")]
        record = self.text[self.text.index("\n  approval_record_dispatched:\n"):self.text.index("\n  verdict:\n")]
        self.assertIn("if: ${{ github.event_name == 'workflow_dispatch' && inputs.approve_head_sha != '' }}", job)
        self.assertIn("ref: ${{ github.event.repository.default_branch }}", job)
        self.assertIn("persist-credentials: false", job)
        self.assertIn('--approval dispatch --pull-request "${PR_NUMBER}" --head-sha "${HEAD_SHA}"', job)
        self.assertIn('if [ "${GITHUB_REF}" != "refs/heads/${DEFAULT_BRANCH}" ]; then', job)
        self.assertNotRegex(job, r": write")
        # The dispatch inputs reach the shell only as environment values.
        body = job[job.index("        run: |"):]
        self.assertNotIn("${{", body)
        self.assertIn("permissions: {}", record)
        self.assertNotIn("checkout", record)
        self.assertNotIn("secrets.", record)
        self.assertIn("if: ${{ needs.approval_dispatch.outputs.digest != '' }}", record)

    def test_an_approval_dispatch_is_not_replaced_by_a_later_verdict_dispatch(self) -> None:
        # One pending run per concurrency group: a plain dispatch for the same pull request must not share it.
        self.assertIn("(inputs.approve_head_sha != '' && format('approval-{0}-{1}', inputs.pull_request_number, "
                      "inputs.approve_head_sha)) || inputs.pull_request_number }}", self.text)

    def test_the_verdict_waits_for_the_approval_jobs(self) -> None:
        self.assertIn("    needs: [approval, approval_record, approval_dispatch, approval_record_dispatched]\n",
                      self.text)
        self.assertNotIn("clear_approval", self.text)
        # A refused dispatch still gets a verdict: the verdict's guard names only the label jobs.
        verdict = self.text[self.text.index("\n  verdict:\n"):]
        guard = verdict[verdict.index("    if: >-\n"):verdict.index("    runs-on:")]
        self.assertNotIn("approval_dispatch", guard)

    def test_only_a_carried_push_keeps_the_label(self) -> None:
        # The label is removed on every push or reopen unless the carry step produced a digest.
        self.assertIn("github.event.action != 'labeled' && steps.carry.outputs.digest == ''", self.text)
        self.assertIn("if: ${{ github.event.action == 'synchronize' }}", self.text)   # reopened is never carried
        self.assertIn("!cancelled()", self.text)

    def test_the_approval_jobs_run_default_branch_code_with_the_least_they_need(self) -> None:
        approval = self.text[self.text.index("\n  approval:\n"):self.text.index("\n  approval_record:\n")]
        record = self.text[self.text.index("\n  approval_record:\n"):self.text.index("\n  approval_dispatch:\n")]
        self.assertIn("ref: ${{ github.event.repository.default_branch }}", approval)
        self.assertIn("python tools/ci_trust_verdict.py", approval)
        self.assertNotIn("statuses: write", approval)
        self.assertIn("permissions: {}", record)
        self.assertNotIn("checkout", record)
        self.assertNotIn("secrets.", record)


class NamedAttestationRouteTests(VerdictAssertions):
    """The attestation's second route: a workflow_run or dispatched run on the default branch, titled for the head.

    GitHub blocks pull_request_target by default in public repositories from 2026-12-02, and such a run reports
    the default branch's commit, not the pull request head. Failing-before: the verifier read the attestation
    only from a pull_request_target run at the head, so without one the verdict stayed pending for ever.
    """

    @staticmethod
    def _without_the_event() -> Scenario:
        scenario = Scenario()
        scenario.drop_runs(PROV)
        return scenario

    def test_without_either_route_the_attestation_is_awaited(self) -> None:
        verdict = self.assertVerdict(self._without_the_event(), "pending")
        self.assertEqual(verdict.pending, [PROVENANCE.context])

    def test_a_default_branch_workflow_run_titled_for_the_head_is_judged(self) -> None:
        scenario = self._without_the_event()
        scenario.add_named(6)
        verdict = self.assertVerdict(scenario, "success")
        self.assertEqual(verdict.findings, [])

    def test_a_default_branch_dispatch_titled_for_the_head_is_judged(self) -> None:
        scenario = self._without_the_event()
        scenario.add_named(6, event="workflow_dispatch")
        self.assertVerdict(scenario, "success")

    def test_a_run_titled_for_another_head_is_ignored(self) -> None:
        scenario = self._without_the_event()
        scenario.add_named(6, head=OTHER)
        self.assertVerdict(scenario, "pending")
        scenario.raw["workflow_runs"][-1]["display_title"] = "Trusted provenance attestation"
        self.assertVerdict(scenario, "pending")
        scenario.raw["workflow_runs"][-1]["display_title"] = NAMED_TITLE.format(head=HEAD) + " "
        self.assertVerdict(scenario, "pending")

    def test_a_run_dispatched_from_another_ref_is_ignored(self) -> None:
        # Another branch runs that branch's copy of the workflow: its job could be `exit 0`.
        scenario = self._without_the_event()
        scenario.add_named(6, event="workflow_dispatch", branch="ci/evil")
        self.assertVerdict(scenario, "pending")
        # A tag named like the default branch is told apart only by its commit and copy (default_definition).
        scenario = self._without_the_event()
        scenario.add_named(6, event="workflow_dispatch", trusted=False)
        self.assertVerdict(scenario, "pending")

    def test_only_the_collectors_own_check_marks_a_definition_trusted(self) -> None:
        scenario = self._without_the_event()
        run = scenario.add_named(6)
        for value in ("true", 1, None):
            with self.subTest(value=value):
                run["default_definition"] = value
                self.assertVerdict(scenario, "pending")
        del run["default_definition"]
        self.assertVerdict(scenario, "pending")

    def test_a_titled_run_of_any_other_event_is_ignored(self) -> None:
        for event in ("pull_request", "push", "schedule", "merge_group"):
            with self.subTest(event=event):
                scenario = self._without_the_event()
                scenario.add_named(6, event=event)
                self.assertVerdict(scenario, "pending")

    def test_a_titled_run_of_another_workflow_is_ignored(self) -> None:
        scenario = self._without_the_event()
        scenario.add_named(6)["path"] = ".github/workflows/evil.yml"
        self.assertVerdict(scenario, "pending")

    def test_a_failed_or_skipped_attestation_is_not_green(self) -> None:
        for conclusion in ("failure", "skipped"):
            with self.subTest(conclusion=conclusion):
                scenario = self._without_the_event()
                scenario.add_named(6)
                scenario.job(6, PROVENANCE.context)["conclusion"] = conclusion
                self.assertVerdict(scenario, "failure", "JOB_FAILED")

    def test_an_unfinished_run_is_awaited_and_a_finished_one_must_have_the_job_once(self) -> None:
        scenario = self._without_the_event()
        scenario.add_named(6, status="in_progress", conclusion=None)
        scenario.job(6, PROVENANCE.context).update(status="in_progress", conclusion=None)
        self.assertVerdict(scenario, "pending")
        scenario = self._without_the_event()
        scenario.add_named(6, job_names=())
        self.assertVerdict(scenario, "failure", "JOB_MISSING")
        scenario = self._without_the_event()
        scenario.add_named(6, job_names=(PROVENANCE.context, PROVENANCE.context))
        self.assertVerdict(scenario, "failure", "DUPLICATE_CONTEXT")

    def test_a_job_that_reports_another_commit_than_its_run_is_a_replay(self) -> None:
        scenario = self._without_the_event()
        scenario.add_named(6)
        scenario.job(6, PROVENANCE.context)["head_sha"] = OTHER
        self.assertVerdict(scenario, "failure", "RUN_HEAD_MISMATCH")

    def test_the_newest_run_of_either_route_decides(self) -> None:
        # An older success cannot mask a newer failure, whichever route each came from.
        scenario = Scenario()
        scenario.add_named(6, created="2026-10-28T10:05:00Z")
        scenario.job(6, PROVENANCE.context)["conclusion"] = "failure"
        self.assertVerdict(scenario, "failure", "JOB_FAILED")
        scenario = Scenario()
        scenario.job(4, PROVENANCE.context)["conclusion"] = "failure"
        scenario.add_named(6, created="2026-10-28T09:55:00Z")
        self.assertVerdict(scenario, "failure", "JOB_FAILED")
        # Attesting again after an admission: the dispatch is the newest run.
        scenario.add_named(7, event="workflow_dispatch", created="2026-10-28T10:20:00Z")
        self.assertVerdict(scenario, "success")

    def test_re_running_a_run_makes_it_the_newest_whichever_route_it_is(self) -> None:
        """Both routes start on a push and fail before an admission. The run a pull request's check list shows
        is the pull_request_target one, created seconds before the workflow_run one, and a maintainer re-runs
        it after the admission. Failing-before: the newest run was chosen by creation time, which a re-run
        keeps, so the re-run's success never counted and the head stayed JOB_FAILED."""
        scenario = Scenario()
        scenario.job(4, PROVENANCE.context)["conclusion"] = "failure"
        scenario.add_named(6, created="2026-10-28T10:00:05Z")
        scenario.job(6, PROVENANCE.context)["conclusion"] = "failure"
        self.assertVerdict(scenario, "failure", "JOB_FAILED")
        scenario.raw["workflow_runs"][3]["run_started_at"] = "2026-10-28T12:00:00Z"     # attempt 2 of run 4
        scenario.job(4, PROVENANCE.context)["conclusion"] = "success"
        self.assertVerdict(scenario, "success")
        # The same for the other route, and a re-run that is still going is waited for.
        scenario.raw["workflow_runs"][-1].update(run_started_at="2026-10-28T12:30:00Z", status="in_progress",
                                                 conclusion=None)
        scenario.job(6, PROVENANCE.context).update(status="in_progress", conclusion=None)
        self.assertVerdict(scenario, "pending")
        scenario.raw["workflow_runs"][-1].update(status="completed", conclusion="failure")
        scenario.job(6, PROVENANCE.context).update(status="completed", conclusion="failure")
        self.assertVerdict(scenario, "failure", "JOB_FAILED")

    def test_a_re_run_decides_for_every_producer(self) -> None:
        # Two CI runs at one head (a push, then a Draft/Ready transition); the older is re-run last.
        scenario = Scenario()
        scenario.add_run(9, CI, ["Classify change", "Hygiene and security", "CI required"],
                         created="2026-10-28T10:20:00Z")
        scenario.job(9, "CI required")["conclusion"] = "failure"
        self.assertVerdict(scenario, "failure", "JOB_FAILED")
        scenario.raw["workflow_runs"][0]["run_started_at"] = "2026-10-28T11:00:00Z"
        self.assertVerdict(scenario, "success")
        self.assertEqual(ctv.run_order({"id": 1, "created_at": "a"}), ("a", 1))
        self.assertEqual(ctv.run_order({"id": 1, "created_at": "a", "run_started_at": "b"}), ("b", 1))

    def test_a_cancelled_named_run_is_superseded_not_failed(self) -> None:
        scenario = self._without_the_event()
        scenario.add_named(6, created="2026-10-28T10:01:00Z")
        scenario.add_named(7, created="2026-10-28T10:02:00Z", conclusion="cancelled")
        self.assertVerdict(scenario, "success")
        scenario = self._without_the_event()
        scenario.add_named(7, conclusion="cancelled")
        self.assertVerdict(scenario, "failure", "JOB_CANCELLED")

    def test_a_refused_pull_request_target_event_does_not_mask_an_attestation_that_ran(self) -> None:
        """GitHub ends a refused event's run as startup_failure with no job (2026-11-09: 18 attestation runs).
        Failing-before: that run was the newest at the head, so the verdict failed with JOB_MISSING."""
        scenario = Scenario()
        scenario.raw["workflow_runs"][3].update(conclusion="startup_failure", created_at="2026-10-28T10:10:00Z")
        scenario.raw["jobs"]["4"] = []
        scenario.raw["check_runs"] = [check for check in scenario.raw["check_runs"] if check["check_suite"]["id"] != 40]
        self.assertVerdict(scenario, "failure", "JOB_MISSING")       # nothing attested this head
        scenario.add_named(6, created="2026-10-28T10:00:05Z")
        self.assertVerdict(scenario, "success")
        scenario.job(6, PROVENANCE.context)["conclusion"] = "failure"   # and its result is the one judged
        self.assertVerdict(scenario, "failure", "JOB_FAILED")

    def test_a_run_that_completed_without_any_job_does_not_mask_an_attestation_that_ran(self) -> None:
        """GitHub ended some refused pull_request_target runs as `failure` with no job at all (2026-11-09,
        "workflow file issue"). Such a run attested nothing and failed nothing. Failing-before: when it was
        the newest run the head was JOB_MISSING although the other route had attested it."""
        scenario = Scenario()
        scenario.raw["workflow_runs"][3].update(conclusion="failure", created_at="2026-10-28T10:10:00Z")
        scenario.raw["jobs"]["4"] = []
        scenario.raw["check_runs"] = [check for check in scenario.raw["check_runs"] if check["check_suite"]["id"] != 40]
        scenario.add_named(6, created="2026-10-28T10:00:05Z")
        self.assertVerdict(scenario, "failure", "JOB_MISSING")      # only the collector says a run had no job
        scenario.raw["workflow_runs"][3]["jobless"] = "yes"
        self.assertVerdict(scenario, "failure", "JOB_MISSING")
        scenario.raw["workflow_runs"][3]["jobless"] = True
        self.assertVerdict(scenario, "success")
        scenario.job(6, PROVENANCE.context)["conclusion"] = "failure"
        self.assertVerdict(scenario, "failure", "JOB_FAILED")
        # With no run of the other route, or only a cancelled one, nothing attested the head.
        scenario.raw["workflow_runs"][-1]["conclusion"] = "cancelled"
        self.assertVerdict(scenario, "failure", "JOB_MISSING")

    def test_a_refused_event_still_fails_when_the_other_route_only_has_a_cancelled_run(self) -> None:
        scenario = Scenario()
        scenario.raw["workflow_runs"][3].update(conclusion="startup_failure")
        scenario.raw["jobs"]["4"] = []
        scenario.add_named(6, conclusion="cancelled")
        self.assertVerdict(scenario, "failure", "JOB_MISSING")

    def test_a_failed_run_of_the_event_route_is_not_dropped(self) -> None:
        # Only a run GitHub refused to start is ignored; a real failure stays the newest result.
        scenario = Scenario()
        scenario.raw["workflow_runs"][3].update(created_at="2026-10-28T10:10:00Z")
        scenario.job(4, PROVENANCE.context)["conclusion"] = "failure"
        scenario.add_named(6, created="2026-10-28T10:00:05Z")
        self.assertVerdict(scenario, "failure", "JOB_FAILED")

    def test_only_the_attestation_has_a_named_route(self) -> None:
        named = [producer.context for producer in ctv.PRODUCERS if producer.named_events]
        self.assertEqual(named, [PROVENANCE.context])
        # A pull_request workflow's definition is the candidate's: a default-branch run titled for the head
        # must never answer for one.
        scenario = Scenario()
        scenario.drop_runs(OSV)
        run = scenario.add_named(6, job_names=("OSV Vulnerability Scan",))
        run["path"] = OSV
        self.assertVerdict(scenario, "pending")


class NamedRouteCollectorTests(unittest.TestCase):
    """How the collector finds the second route's runs, and what it costs."""

    def collect(self, scenario: Scenario, *, on_main: bool = True) -> tuple[dict | None, FakeGitHub]:
        gh = FakeGitHub(scenario)
        with mock.patch.object(ctv, "git_facts", return_value=_facts()), \
                mock.patch.object(ctv, "default_definition", return_value=on_main) as ancestry:
            raw = ctv.collect(gh, Path("."), "main", HEAD, PR)
        self.ancestry = ancestry
        return raw, gh

    @staticmethod
    def _listing(gh: FakeGitHub) -> list[str]:
        return [read for read in gh.reads if read.startswith("actions/workflows/")]

    def test_the_named_run_is_listed_by_branch_and_time_and_judged(self) -> None:
        """Failing-before: only the head's own runs were read, and a workflow_run attestation is not among them."""
        scenario = Scenario()
        scenario.drop_runs(PROV)
        scenario.add_named(6, created="2026-10-28T10:00:05Z")
        scenario.add_named(7, head=OTHER, created="2026-10-28T10:00:06Z")     # another head's attestation
        raw, gh = self.collect(scenario)
        assert raw is not None
        self.assertEqual(self._listing(gh), [
            "actions/workflows/provenance-attestation.yml/runs?event=workflow_run&branch=main"
            "&created=%3E%3D2026-10-28T10%3A00%3A00Z",
            "actions/workflows/provenance-attestation.yml/runs?event=workflow_dispatch&branch=main"
            "&created=%3E%3D2026-10-28T10%3A00%3A00Z",
        ])
        self.assertEqual({run["id"] for run in raw["workflow_runs"] if run["path"] == PROV}, {6})
        self.ancestry.assert_called_once_with(Path("."), MAIN, PROV)
        self.assertIn("actions/runs/6/jobs?filter=latest", gh.reads)
        self.assertNotIn("actions/runs/7/jobs?filter=latest", gh.reads)
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).state, "success")

    def test_the_listing_starts_at_the_heads_earliest_run(self) -> None:
        # A later start could leave out the attestation that ran when the head first arrived.
        scenario = Scenario()
        scenario.add_run(9, DEP, ["dependency-review"], created="2026-10-28T10:40:00Z")
        scenario.raw["workflow_runs"][1]["created_at"] = "2026-10-28T09:30:00Z"
        _raw, gh = self.collect(scenario)
        self.assertEqual([read.split("created=", 1)[1] for read in self._listing(gh)],
                         ["%3E%3D2026-10-28T09%3A30%3A00Z"] * 2)

    def test_a_run_whose_commit_is_not_on_the_default_branch_stays_untrusted(self) -> None:
        scenario = Scenario()
        scenario.drop_runs(PROV)
        scenario.add_named(6, event="workflow_dispatch")
        raw, _gh = self.collect(scenario, on_main=False)
        assert raw is not None
        self.assertEqual(raw, {"head_sha": HEAD, "waiting_producers": [PROVENANCE.context]})

    def test_nothing_is_listed_while_another_producer_is_unfinished(self) -> None:
        # The listing is an API call on every judgement; a pending CI run already decides the answer.
        scenario = Scenario()
        scenario.drop_runs(PROV)
        scenario.add_named(6)
        scenario.raw["workflow_runs"][0].update(status="in_progress", conclusion=None)
        raw, gh = self.collect(scenario)
        assert raw is not None
        self.assertEqual(raw["waiting_producers"], ["CI required", "Hygiene and security", PROVENANCE.context])
        self.assertEqual(self._listing(gh), [])

    def test_the_jobs_of_the_re_run_run_are_the_ones_read(self) -> None:
        scenario = Scenario()
        scenario.raw["workflow_runs"][3]["run_started_at"] = "2026-10-28T12:00:00Z"     # run 4, re-run
        scenario.add_named(6, created="2026-10-28T10:00:05Z")
        scenario.job(6, PROVENANCE.context)["conclusion"] = "failure"
        raw, gh = self.collect(scenario)
        assert raw is not None
        self.assertIn("actions/runs/4/jobs?filter=latest", gh.reads)
        self.assertNotIn("actions/runs/6/jobs?filter=latest", gh.reads)
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).state, "success")

    def test_a_truncated_run_list_never_lets_an_older_result_stand_in(self) -> None:
        """GitHub returns the newest 1,000 runs of a filtered list. If this head's named run has left the list
        it may have been a failure, so the pull_request_target success at the head must not be judged in its
        place. Failing-before: truncation was not detected and the older run decided."""
        note = f"{PROVENANCE.context} (not among its workflow's newest 1000 runs; dispatch it again)"
        for total in (5000, None):
            with self.subTest(total=total):
                scenario = Scenario()                     # a green pull_request_target attestation at the head
                scenario.raw["listed_total"] = total
                raw, gh = self.collect(scenario)
                self.assertEqual(raw, {"head_sha": HEAD, "waiting_producers": [note]})
                # Decided from the run lists alone, before the full read of the pull request.
                self.assertNotIn(f"pulls/{PR}", gh.reads)
        # With no run of either route the remedy is still named.
        missing = Scenario()
        missing.drop_runs(PROV)
        missing.raw["listed_total"] = 5000
        raw, _gh = self.collect(missing)
        self.assertEqual(raw, {"head_sha": HEAD, "waiting_producers": [note]})
        self.assertLessEqual(len(ctv.Verdict("pending", pending=[note]).description()), 140)
        # A cancelled named run in what was returned is not a result either.
        scenario = Scenario()
        scenario.raw["listed_total"] = 5000
        scenario.add_named(6, conclusion="cancelled")
        raw, _gh = self.collect(scenario)
        self.assertEqual(raw, {"head_sha": HEAD, "waiting_producers": [note]})

    def test_a_truncated_run_list_that_still_holds_the_heads_run_is_judged(self) -> None:
        # Attesting again puts a new run at the top of the list: that is how an old head recovers.
        for conclusion, state in (("success", "success"), ("failure", "failure")):
            with self.subTest(conclusion=conclusion):
                scenario = Scenario()
                scenario.raw["listed_total"] = {"workflow_run": 5000}
                scenario.add_named(8, head=OTHER, created="2026-10-28T10:10:00Z")   # the oldest run returned
                scenario.add_named(6, event="workflow_dispatch", created="2026-10-28T10:20:00Z")
                scenario.job(6, PROVENANCE.context)["conclusion"] = conclusion
                raw, _gh = self.collect(scenario)
                assert raw is not None
                self.assertEqual(ctv.evaluate(ctv.normalise(raw)).state, state)

    def test_a_run_older_than_a_truncated_lists_horizon_does_not_stand_in(self) -> None:
        """A dispatched attestation succeeded at 10:20; a workflow_run attestation of the same head failed at
        11:00 and has since left the truncated workflow_run list, whose oldest returned run is from 12:00. The
        dispatch list is complete and still holds the success. Failing-before: any live named run that was
        returned excused the truncation, so the older success was judged and the verdict was `success` where
        the complete list gives JOB_FAILED."""
        note = f"{PROVENANCE.context} (not among its workflow's newest 1000 runs; dispatch it again)"

        def scenario(lost: bool) -> Scenario:
            built = Scenario()
            built.add_named(6, event="workflow_dispatch", created="2026-10-28T10:20:00Z")
            if lost:
                built.raw["listed_total"] = {"workflow_run": 5000}
                built.add_named(8, head=OTHER, created="2026-10-28T12:00:00Z")
            else:
                built.add_named(7, created="2026-10-28T11:00:00Z")
                built.job(7, PROVENANCE.context)["conclusion"] = "failure"
            return built

        raw, _gh = self.collect(scenario(lost=False))
        assert raw is not None
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).description(), "1 finding(s): JOB_FAILED")
        raw, _gh = self.collect(scenario(lost=True))
        self.assertEqual(raw, {"head_sha": HEAD, "waiting_producers": [note]})
        # A run started at or after the horizon is newer than anything the list lost, whichever route it is:
        # a new dispatch, or a re-run of the run on the pull request's check list.
        recovered = scenario(lost=True)
        recovered.add_named(9, event="workflow_dispatch", created="2026-10-28T12:00:01Z")
        raw, _gh = self.collect(recovered)
        assert raw is not None
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).state, "success")
        # The horizon's own second is not after it: a lost run of that second could have the higher id.
        tied = scenario(lost=True)
        tied.add_named(9, event="workflow_dispatch", created="2026-10-28T12:00:00Z")
        raw, _gh = self.collect(tied)
        self.assertEqual(raw, {"head_sha": HEAD, "waiting_producers": [note]})
        rerun = scenario(lost=True)
        rerun.raw["workflow_runs"][3]["run_started_at"] = "2026-10-28T13:00:00Z"
        raw, _gh = self.collect(rerun)
        assert raw is not None
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).state, "success")
        # One second before the horizon is not.
        early = scenario(lost=True)
        early.add_named(9, event="workflow_dispatch", created="2026-10-28T11:59:59Z")
        raw, _gh = self.collect(early)
        self.assertEqual(raw, {"head_sha": HEAD, "waiting_producers": [note]})

    def test_a_jobless_run_newer_than_the_horizon_does_not_carry_an_older_run_past_it(self) -> None:
        """A dispatched attestation succeeded at 10:20, a workflow_run attestation failed at 11:00 and has left
        the truncated list (horizon 12:00), and a refused pull_request_target run with no job is from 13:00.
        The jobless run is newer than the horizon but judges nothing: it is dropped, and the run judged in its
        place is the 10:20 success. Failing-before: the horizon was checked before jobless runs were marked,
        so the verdict was `success` where the complete lists give JOB_FAILED."""
        note = f"{PROVENANCE.context} (not among its workflow's newest 1000 runs; dispatch it again)"

        def scenario(lost: bool, conclusion: str = "failure") -> Scenario:
            built = Scenario()
            built.raw["workflow_runs"][3].update(conclusion=conclusion, created_at="2026-10-28T13:00:00Z")
            built.raw["jobs"]["4"] = []
            built.raw["check_runs"] = [check for check in built.raw["check_runs"] if check["check_suite"]["id"] != 40]
            built.add_named(6, event="workflow_dispatch", created="2026-10-28T10:20:00Z")
            if lost:
                built.raw["listed_total"] = {"workflow_run": 5000}
                built.add_named(8, head=OTHER, created="2026-10-28T12:00:00Z")
            else:
                built.add_named(7, created="2026-10-28T11:00:00Z")
                built.job(7, PROVENANCE.context)["conclusion"] = "failure"
            return built

        raw, _gh = self.collect(scenario(lost=False))
        assert raw is not None
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).description(), "1 finding(s): JOB_FAILED")
        raw, _gh = self.collect(scenario(lost=True))
        self.assertEqual(raw, {"head_sha": HEAD, "waiting_producers": [note]})
        # The same history with the refused run ended as startup_failure was already held back.
        raw, _gh = self.collect(scenario(lost=True, conclusion="startup_failure"))
        self.assertEqual(raw, {"head_sha": HEAD, "waiting_producers": [note]})

    def test_the_horizon_of_a_context_is_the_latest_of_its_truncated_lists(self) -> None:
        title = NAMED_TITLE.format(head=OTHER)

        class Lists(ctv.GitHub):
            def counted(self, path: str, key: str | None = None) -> tuple[list, int | None]:
                if "event=workflow_run" in path:
                    return [_run(1, PROV, "workflow_run", created="2026-10-28T12:00:00Z", title=title),
                            _run(2, PROV, "workflow_run", created="2026-10-28T11:00:00Z", title=title)], 5000
                return [_run(3, PROV, "workflow_dispatch", created="2026-10-28T09:00:00Z", title=title)], 5000

        _found, horizons = ctv.named_runs(Lists("o/r", "t"), Path("."), "main", HEAD, PUSHED)
        self.assertEqual(horizons, {PROVENANCE.context: "2026-10-28T11:00:00Z"})

    def test_a_jobless_run_is_found_by_the_collector_and_the_other_routes_jobs_are_read(self) -> None:
        def scenario(named: bool) -> Scenario:
            built = Scenario()
            built.raw["workflow_runs"][3].update(conclusion="failure", created_at="2026-10-28T10:10:00Z")
            built.raw["jobs"]["4"] = []
            built.raw["check_runs"] = [check for check in built.raw["check_runs"] if check["check_suite"]["id"] != 40]
            if named:
                built.add_named(6, created="2026-10-28T10:00:05Z")
            return built

        raw, gh = self.collect(scenario(named=True))
        assert raw is not None
        self.assertIs(raw["workflow_runs"][3]["jobless"], True)
        self.assertIn("actions/runs/6/jobs?filter=latest", gh.reads)
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).state, "success")
        raw, _gh = self.collect(scenario(named=False))
        assert raw is not None
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).description(), "1 finding(s): JOB_MISSING")
        # Only a producer with a second route is treated so: a CI run without jobs is a missing job.
        ci = Scenario()
        ci.raw["jobs"]["1"] = []
        ci.raw["check_runs"] = [check for check in ci.raw["check_runs"] if check["check_suite"]["id"] != 10]
        raw, _gh = self.collect(ci)
        assert raw is not None
        self.assertNotIn("jobless", raw["workflow_runs"][0])
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).state, "failure")

    def test_a_complete_run_list_leaves_the_event_route_to_decide(self) -> None:
        raw, _gh = self.collect(Scenario())
        assert raw is not None
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).state, "success")

    def test_a_full_run_list_is_treated_as_truncated_whatever_total_github_reports(self) -> None:
        class Full(ctv.GitHub):
            def counted(self, path: str, key: str | None = None) -> tuple[list, int | None]:
                runs = [{"id": index, "event": "workflow_run", "display_title": "x"}
                        for index in range(ctv.RUN_LIST_LIMIT)]
                return runs, ctv.RUN_LIST_LIMIT

        found, horizons = ctv.named_runs(Full("o/r", "t"), Path("."), "main", HEAD, PUSHED)
        # The runs carry no time to date the list by, so nothing bounds what is missing.
        self.assertEqual((found, horizons), ([], {PROVENANCE.context: ctv.UNBOUNDED}))

    def test_a_run_is_taken_only_from_the_list_of_its_own_event(self) -> None:
        # Each event has its own list. A run GitHub returned under the other event's filter is not taken there,
        # so no run is counted twice and none enters through a list it does not belong to.
        title = NAMED_TITLE.format(head=HEAD)
        runs = [_run(6, PROV, "workflow_run", sha=MAIN, branch="main", title=title),
                _run(7, PROV, "workflow_dispatch", sha=MAIN, branch="main", title=title),
                _run(8, PROV, "pull_request_target", sha=MAIN, branch="main", title=title)]

        class Unfiltered(ctv.GitHub):
            def counted(self, path: str, key: str | None = None) -> tuple[list, int | None]:
                return runs, len(runs)

        with mock.patch.object(ctv, "default_definition", return_value=True):
            found, horizons = ctv.named_runs(Unfiltered("o/r", "t"), Path("."), "main", HEAD, PUSHED)
        self.assertEqual([run["id"] for run in found], [6, 7])
        self.assertEqual(horizons, {})

    def test_counted_reports_the_first_pages_total(self) -> None:
        gh = ctv.GitHub("owner/repo", "token")
        pages = {
            "actions/runs?x=1&per_page=100": ({"total_count": 3, "workflow_runs": [1, 2]},
                                              {"Link": '<https://api.github.com/next>; rel="next"'}),
            "https://api.github.com/next": ({"total_count": 9, "workflow_runs": [3]}, {}),
            "pulls?state=open&per_page=100": ([4], {}),
        }
        with mock.patch.object(gh, "_request", side_effect=lambda method, url, body=None: pages[url]):
            self.assertEqual(gh.counted("actions/runs?x=1", "workflow_runs"), ([1, 2, 3], 3))
            self.assertEqual(gh.counted("pulls?state=open"), ([4], None))
            self.assertEqual(gh.paginate("actions/runs?x=1", "workflow_runs"), [1, 2, 3])

    def test_an_unfinished_named_run_is_waited_for(self) -> None:
        scenario = Scenario()
        scenario.drop_runs(PROV)
        scenario.add_named(6, status="in_progress", conclusion=None)
        raw, gh = self.collect(scenario)
        assert raw is not None
        self.assertEqual(raw["waiting_producers"], [PROVENANCE.context])
        self.assertEqual(len(self._listing(gh)), 2)     # one list per event of the named route

    def test_both_routes_are_read_when_the_event_route_finished(self) -> None:
        # While pull_request_target still runs, a newer dispatch after an admission must be seen.
        scenario = Scenario()
        scenario.job(4, PROVENANCE.context)["conclusion"] = "failure"
        scenario.add_named(6, event="workflow_dispatch", created="2026-10-28T10:20:00Z")
        raw, gh = self.collect(scenario)
        assert raw is not None
        self.assertIn("actions/runs/6/jobs?filter=latest", gh.reads)
        self.assertNotIn("actions/runs/4/jobs?filter=latest", gh.reads)
        self.assertEqual(ctv.evaluate(ctv.normalise(raw)).state, "success")

    def test_on_default_branch_reads_ancestry_from_the_trusted_checkout(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)

            def git(*args: str) -> str:
                return subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True,
                                      text=True).stdout.strip()

            git("init", "-q", "-b", "main")
            git("-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit", "-q", "--allow-empty", "-m", "one")
            first = git("rev-parse", "HEAD")
            git("-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit", "-q", "--allow-empty", "-m", "two")
            tip = git("rev-parse", "HEAD")
            git("checkout", "-q", "-b", "topic")
            git("-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit", "-q", "--allow-empty", "-m", "side")
            side = git("rev-parse", "HEAD")
            git("checkout", "-q", "main")
            self.assertTrue(ctv.on_default_branch(repo, tip))
            self.assertTrue(ctv.on_default_branch(repo, first))
            self.assertFalse(ctv.on_default_branch(repo, side))        # a commit only another ref has
            self.assertFalse(ctv.on_default_branch(repo, "e" * 40))    # an object the checkout does not have
            for malformed in ("", "main", tip[:12], tip.upper(), f"{tip} --not"):
                self.assertFalse(ctv.on_default_branch(repo, malformed))

    def test_only_the_default_branchs_current_copy_of_a_workflow_is_a_trusted_definition(self) -> None:
        """A rebase merge puts every commit of a pull request on the default branch, and only its last tree was
        judged: a commit in the middle can hold a copy of the workflow that prints green. A tag named like the
        default branch can be dispatched from there. Failing-before: there was no second route to protect."""
        workflow = ".github/workflows/gate.yml"
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)

            def git(*args: str) -> str:
                return subprocess.run(["git", "-C", str(repo), "-c", "user.name=t", "-c",
                                       "user.email=t@example.invalid", *args], check=True, capture_output=True,
                                      text=True).stdout.strip()

            def commit(text: str | None, message: str) -> str:
                path = repo / workflow
                if text is None:
                    path.unlink()
                else:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(text, encoding="utf-8", newline="")
                git("add", "-A")
                git("commit", "-q", "--allow-empty", "-m", message)
                return git("rev-parse", "HEAD")

            git("init", "-q", "-b", "main")
            absent = git("commit", "-q", "--allow-empty", "-m", "no workflow yet") or git("rev-parse", "HEAD")
            honest = commit("jobs: {attest: honest}", "the gate")
            swapped = commit("jobs: {attest: exit 0}", "first commit of a rebase-merged pull request")
            restored = commit("jobs: {attest: honest}", "its second commit restores the file")
            tip = commit("jobs: {attest: honest}", "unrelated work")
            git("checkout", "-q", "-b", "topic")
            side = commit("jobs: {attest: honest}", "the same copy on a commit only a topic branch has")
            git("checkout", "-q", "main")
            for sha in (tip, restored, honest):
                self.assertTrue(ctv.default_definition(repo, sha, workflow), sha)
            self.assertFalse(ctv.default_definition(repo, swapped, workflow))   # on main, another copy
            self.assertFalse(ctv.default_definition(repo, side, workflow))      # the same copy, not on main
            self.assertFalse(ctv.default_definition(repo, absent, workflow))    # no copy at that commit
            self.assertFalse(ctv.default_definition(repo, tip, ".github/workflows/other.yml"))
            self.assertFalse(ctv.default_definition(repo, "e" * 40, workflow))


class AttestationTargetTests(unittest.TestCase):
    """The pull request a workflow_run or dispatched attestation attests: exactly one, at exactly the head."""

    class Pulls(ctv.GitHub):
        def __init__(self, *pulls: dict) -> None:
            super().__init__("Jstar269/nakagawa-recomp", "token")
            self.pulls = list(pulls)
            self.reads: list[str] = []

        def paginate(self, path: str, key: str | None = None) -> list:
            self.reads.append(path)
            return self.pulls

    @staticmethod
    def _pull(number: int = PR, **args) -> dict:
        pull = _pull_request(**args)
        pull["number"] = number
        pull["base"]["sha"] = MAIN
        return pull

    def test_the_one_open_pull_request_at_the_head_is_the_target(self) -> None:
        gh = self.Pulls(self._pull(), self._pull(8, head=OTHER))
        self.assertEqual(ctv.attestation_target(gh, "main", HEAD),
                         {"number": PR, "head_sha": HEAD, "base_sha": MAIN})
        self.assertEqual(gh.reads, ["pulls?state=open&base=main"])
        self.assertEqual(ctv.attestation_target(gh, "main", HEAD, PR)["number"], PR)

    def test_no_pull_request_at_the_head_is_refused(self) -> None:
        # The head moved, or the pull request targets another branch: nothing is attested, so nothing is green.
        for pulls in ((), (self._pull(head=OTHER),), (self._pull(base="release"),)):
            with self.subTest(pulls=len(pulls)), self.assertRaises(ctv.TargetRefused):
                ctv.attestation_target(self.Pulls(*pulls), "main", HEAD)

    def test_two_pull_requests_with_one_head_are_refused(self) -> None:
        with self.assertRaises(ctv.TargetRefused):
            ctv.attestation_target(self.Pulls(self._pull(), self._pull(8)), "main", HEAD)

    def test_a_dispatch_naming_another_pull_request_is_refused(self) -> None:
        with self.assertRaises(ctv.TargetRefused):
            ctv.attestation_target(self.Pulls(self._pull()), "main", HEAD, PR + 1)

    def test_a_base_without_an_exact_sha_is_refused(self) -> None:
        pull = self._pull()
        pull["base"]["sha"] = "main"
        with self.assertRaises(ctv.TargetRefused):
            ctv.attestation_target(self.Pulls(pull), "main", HEAD)

    def _main(self, gh: ctv.GitHub, *args: str) -> tuple[int, str]:
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "out"
            with mock.patch.object(ctv, "GitHub", return_value=gh), \
                    mock.patch.dict(os.environ, {"GITHUB_TOKEN": "t"}), \
                    mock.patch("sys.stdout"), mock.patch("sys.stderr"):
                code = ctv.main(["--repository", gh.repository, "--attestation-target", "--github-output",
                                 str(output), *args])
            return code, output.read_text(encoding="utf-8") if output.exists() else ""

    def test_the_command_writes_the_target_only_on_success(self) -> None:
        gh = self.Pulls(self._pull())
        self.assertEqual(self._main(gh, "--head-sha", HEAD),
                         (0, f"number={PR}\nhead_sha={HEAD}\nbase_sha={MAIN}\n"))
        self.assertEqual(self._main(gh, "--head-sha", HEAD, "--pull-request", str(PR))[0], 0)
        # Fail closed: no match, another pull request, a malformed head or an API failure write nothing.
        self.assertEqual(self._main(gh, "--head-sha", OTHER), (1, ""))
        self.assertEqual(self._main(gh, "--head-sha", HEAD, "--pull-request", str(PR + 1)), (1, ""))
        self.assertEqual(self._main(gh, "--head-sha", "HEAD"), (2, ""))
        with mock.patch.object(gh, "paginate", side_effect=urllib.error.URLError("down")):
            self.assertEqual(self._main(gh, "--head-sha", HEAD), (1, ""))


class CommandLineTests(unittest.TestCase):
    def _main(self, raw: dict, *extra: str) -> int:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "snapshot.json"
            path.write_text(json.dumps(raw), encoding="utf-8")
            with mock.patch("sys.stdout"):
                returned: int = ctv.main(["--snapshot", str(path), *extra])
                return returned

    def test_offline_snapshot_exit_codes(self) -> None:
        self.assertEqual(self._main(Scenario().raw), 0)
        broken = Scenario()
        broken.change(CI)
        self.assertEqual(self._main(broken.raw), 1)

    def test_snapshot_cannot_be_posted(self) -> None:
        with mock.patch("sys.stderr"), self.assertRaises(SystemExit):
            self._main(Scenario().raw, "--post")


class ForkProducerTests(unittest.TestCase):
    def test_a_forks_success_says_what_its_provenance_check_covers(self) -> None:
        # Review 2309-36f775933693 S3 (and N3): a fork's green read exactly like the upstream's, although
        # its provenance check passes paths a pull request admits itself.
        upstream = ctv.producers_for("Jstar269/Nakagawa-Recomp", "1330183186")
        fork = ctv.producers_for("example-user/Nakagawa-Recomp", "42")
        success = ctv.Verdict("success")
        self.assertEqual(ctv.status_description(success, upstream), success.description())
        self.assertEqual(ctv.status_description(success, fork), ctv.FORK_SUCCESS_DESCRIPTION)
        self.assertLessEqual(len(ctv.FORK_SUCCESS_DESCRIPTION), 140)
        self.assertIn("admits itself", ctv.FORK_SUCCESS_DESCRIPTION)
        for verdict in (ctv.Verdict("failure", [ctv.Finding("JOB_FAILED", "x", "y")]), ctv.Verdict("pending", pending=["x"])):
            self.assertEqual(ctv.status_description(verdict, fork), verdict.description())

    def _main_with_id(self, identity: str | None) -> tuple[mock.MagicMock, str]:
        """Run main() as the workflow does; return the producer choice's spy and the job log."""
        import io  # noqa: PLC0415

        scenario = Scenario()
        gh = FakeGitHub(scenario)
        environment = {"GITHUB_TOKEN": "token"}
        if identity is not None:
            environment["GITHUB_REPOSITORY_ID"] = identity
        log = io.StringIO()
        with mock.patch.object(ctv, "git_facts", return_value=_facts()), \
                mock.patch.object(ctv, "GitHub", return_value=gh), \
                mock.patch.object(ctv, "producers_for", wraps=ctv.producers_for) as spy, \
                mock.patch.dict(os.environ, environment), mock.patch("sys.stdout", log):
            if identity is None:
                os.environ.pop("GITHUB_REPOSITORY_ID", None)
            ctv.main(["--repository", gh.repository, "--pull-request", str(PR)])
        return spy, log.getvalue()

    def test_main_hands_the_runs_repository_and_id_to_the_producer_choice(self) -> None:
        # Review 2309-4e69563b9228 N9 (C4, C5): no test ran main() with a repository ID, so handing the
        # choice the wrong values survived. N2: the log names the set that was judged.
        for identity in ("42", None):
            with self.subTest(identity=identity):
                spy, log = self._main_with_id(identity)
                repository = spy.call_args.args[0]
                spy.assert_called_once_with(repository, identity)
                expected = ctv.provenance_mode.mode(repository, identity)
                self.assertIn(f"Provenance producer set: {expected} (repository {repository}, "
                              f"ID {identity or 'unknown'})", log)

    """The upstream judges the trusted attestation; a fork judges its Fork provenance check (docs/FORKING.md)."""

    def test_upstream_producers_are_unchanged(self) -> None:
        for repository, repository_id in (
            ("Jstar269/Nakagawa-Recomp", None),
            ("jstar269/nakagawa-recomp", None),
            ("jstar269/nakagawa-recomp", "987654321"),
            ("jstar269/nakagawa-recomp", "1330183186"),
            ("Some-Org/Nakagawa-Recomp", "1330183186"),
            # An unknown ID fails closed to the upstream's producers.
            ("example-user/Nakagawa-Recomp", None),
            ("example-user/Nakagawa-Recomp", ""),
        ):
            with self.subTest(repository=repository):
                producers = ctv.producers_for(repository, repository_id)
                self.assertIs(producers, ctv.PRODUCERS)
                self.assertIn("Trusted provenance attestation", [item.context for item in producers])

    def test_fork_judges_the_fork_check_from_the_same_workflow_and_event(self) -> None:
        producers = ctv.producers_for("example-user/Nakagawa-Recomp", "42")
        contexts = [item.context for item in producers]
        self.assertNotIn("Trusted provenance attestation", contexts)
        fork = next(item for item in producers if item.context == "Fork provenance check")
        upstream = next(item for item in ctv.PRODUCERS if item.context == "Trusted provenance attestation")
        self.assertEqual(fork.workflow, ".github/workflows/provenance-attestation.yml")
        self.assertEqual(fork.events, ("pull_request_target",))
        # The routes that need no pull_request_target, which GitHub blocks by default in public repositories
        # from 2026-12-02, and the run title that binds such a run to a head.
        self.assertEqual(fork.named_events, ("workflow_run", "workflow_dispatch"))
        self.assertEqual((fork.named_events, fork.run_title), (upstream.named_events, upstream.run_title))
        self.assertEqual(
            [item for item in producers if item.context != "Fork provenance check"],
            [item for item in ctv.PRODUCERS if item.context != "Trusted provenance attestation"],
        )

    def test_a_fork_finds_its_check_by_run_title_when_the_event_route_is_refused(self) -> None:
        # In a fork GitHub refuses pull_request_target without an Actions policy: the head-attached run ends
        # startup_failure with no job. The run its requested CI started on the default branch answers instead.
        head = "a" * 40
        fork = next(item for item in ctv.producers_for("example-user/Nakagawa-Recomp", "42")
                    if item.context == "Fork provenance check")
        refused = {"id": 1, "path": fork.workflow, "event": "pull_request_target", "head_sha": head,
                   "status": "completed", "conclusion": "startup_failure"}
        named = {"id": 2, "path": fork.workflow, "event": "workflow_run", "head_sha": "b" * 40,
                 "head_branch": "main", "default_definition": True, "status": "completed",
                 "conclusion": "success", "display_title": f"Provenance attestation of {head}"}
        self.assertEqual(ctv.producer_runs([refused, named], fork, head, "main"), [named])
        # A run titled for another head, or one that did not run the default branch's copy, is no answer.
        for change in ({"display_title": f"Provenance attestation of {'c' * 40}"}, {"default_definition": False},
                       {"head_branch": "topic"}):
            with self.subTest(change=change):
                self.assertEqual(ctv.producer_runs([refused, named | change], fork, head, "main"), [refused])
        self.assertEqual(ctv.unfinished_producers([refused, named], head, "main", (fork,)), [])
        self.assertEqual(ctv.unfinished_producers([], head, "main", (fork,)), ["Fork provenance check"])

    def test_the_mode_switch_is_protected_gate_code(self) -> None:
        # The verifier imports tools/provenance_mode.py to choose its producers, so a change to it
        # needs the CI-change approval even when no workflow line names it.
        self.assertIn("tools/provenance_mode.py", ctv.PROTECTED_EXTRA)
        self.assertIn("tools/provenance_mode.py", ctv.protected_paths({}, {}, []))

    def test_a_skipped_upstream_job_fails_the_upstream_verdict(self) -> None:
        # On the upstream the fork job is skipped and the attestation must still succeed: a skipped
        # attestation is a failure, never a pass.
        raw = Scenario().raw
        for jobs in raw["jobs"].values():
            for job in jobs:
                if job["name"] == "Trusted provenance attestation":
                    job["conclusion"] = "skipped"
        verdict = ctv.evaluate(ctv.normalise(raw), ctv.producers_for("Jstar269/Nakagawa-Recomp", "1330183186"))
        self.assertEqual(verdict.state, "failure")
        self.assertIn("JOB_FAILED", {item.code for item in verdict.findings})


class GateCodeTests(unittest.TestCase):
    """The code that decides what the publication gate passes is a protected path.

    Failing-before: ``tools/publish_audit.py``, ``tools/policy_sync.py`` and the modules they import were
    not protected. They ran only from the pull request's own checkout, and the trusted attestation did not
    judge a file that is outside the publication profile and is not implementation. A pull request could
    therefore loosen the audit, add such a file, and reach a green verdict with no ``ci-change-approved``.
    """

    # The tools the trusted checkout runs, with secrets or to judge a head, and the public-export job's
    # tool. Every local hook pre-commit runs is added to these in ``setUpClass``.
    TRUSTED_ROOTS = (
        "tools/ci_trust_verdict.py",
        "tools/provenance_attest_verify.py",
        "tools/publish_audit.py",
        "tools/build_public_export.py",
    )
    tree: dict[str, str]
    protected: set[str]
    hooks: set[str]
    roots: tuple[str, ...]
    # Imports that a gate never executes, each with the reason. Everything else a root can load is followed.
    NOT_LOADED_BY_A_GATE = {
        # Inside the draft and publish commands only; the audit calls the package validator, which is
        # pure Python over the package's JSON.
        ("tools/enhancement_package.py", "tools/analyze.py"),
        ("tools/enhancement_package.py", "tools/imports.py"),
        # A subprocess of the public-export job that checks the export's SBOM against the generator's
        # output. It does not decide whether a file may be published (publish_audit.py and
        # history_audit.py do), and it loads the analyzer and code generator, about 30 modules that
        # would all need the approval for every change.
        ("tools/build_public_export.py", "tools/verify_sbom.py"),
    }
    @classmethod
    def setUpClass(cls) -> None:
        texts = [path.read_text(encoding="utf-8") for path in sorted(WORKFLOWS.glob("*.y*ml"))]
        listed = subprocess.run(["git", "-C", str(ROOT), "ls-files", "-z"], check=True, capture_output=True).stdout
        cls.tree = {path: "x" for path in listed.decode("utf-8").split("\0") if path}
        cls.protected = ctv.protected_paths(cls.tree, cls.tree, texts)
        config = (ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8")
        cls.hooks = set(re.findall(r"(?m)^\s+entry: python (tools/[A-Za-z0-9_/]+\.py)\b", config))
        cls.roots = (*cls.TRUSTED_ROOTS, *sorted(cls.hooks))

    @staticmethod
    def _resolve(module: str, level: int, here: Path) -> list[str]:
        """Every tools/ file that importing ``module`` from ``here`` loads: packages' __init__ files too."""
        base = here.parent if level else ROOT / "tools"
        for _ in range(max(level - 1, 0)):
            base = base.parent
        parts = [part for part in module.split(".") if part]
        if not level and parts[:1] == ["tools"]:
            parts = parts[1:]
        found: list[Path] = []
        for part in parts:
            if (base / part / "__init__.py").is_file():
                found.append(base / part / "__init__.py")
                base = base / part
            elif (base / f"{part}.py").is_file():
                found.append(base / f"{part}.py")
                break
            else:
                break
        return [path.relative_to(ROOT).as_posix() for path in found]

    @classmethod
    def _imports(cls, path: str) -> set[str]:
        """Local modules ``path`` imports anywhere in its text, except under ``if TYPE_CHECKING:``."""
        file = ROOT / path
        parsed = ast.parse(file.read_text(encoding="utf-8"))
        typing_only = {id(node) for block in ast.walk(parsed)
                       if isinstance(block, ast.If) and getattr(block.test, "id", "") == "TYPE_CHECKING"
                       for statement in block.body for node in ast.walk(statement)}
        found: set[str] = set()
        for node in ast.walk(parsed):
            if id(node) in typing_only:
                continue
            if isinstance(node, ast.Import):
                for alias in node.names:
                    found.update(cls._resolve(alias.name, 0, file))
            elif isinstance(node, ast.ImportFrom):
                found.update(cls._resolve(node.module or "", node.level, file))
                for alias in node.names:
                    found.update(cls._resolve(f"{node.module or ''}.{alias.name}".strip("."), node.level, file))
        return found | cls._subprocess_tools(parsed)

    @staticmethod
    def _subprocess_tools(parsed: ast.AST) -> set[str]:
        """tools/ scripts named as ``<root> / "tools" / "<name>.py"``, the form the gate tools run them by."""
        found: set[str] = set()
        for node in ast.walk(parsed):
            if (isinstance(node, ast.BinOp) and isinstance(node.op, ast.Div)
                    and isinstance(node.right, ast.Constant) and isinstance(node.right.value, str)
                    and node.right.value.endswith(".py")
                    and isinstance(node.left, ast.BinOp) and isinstance(node.left.right, ast.Constant)
                    and node.left.right.value == "tools" and (ROOT / "tools" / node.right.value).is_file()):
                found.add(f"tools/{node.right.value}")
        return found

    def _closure(self) -> set[str]:
        seen: set[str] = set()
        todo = list(self.roots)
        while todo:
            path = todo.pop()
            if path in seen:
                continue
            seen.add(path)
            todo.extend(target for target in self._imports(path) if (path, target) not in self.NOT_LOADED_BY_A_GATE)
        return seen

    def test_every_module_a_gate_loads_is_protected(self) -> None:
        closure = self._closure()
        self.assertEqual(sorted(path for path in closure if path not in self.protected), [])
        # The roots are real files, and the walk found the modules this change is about.
        self.assertLessEqual(set(self.roots), set(self.tree))
        for path in ("tools/publication_policy.py", "tools/provenance_ledger.py", "tools/public_export.py",
                     "tools/nk_core/git_isolation.py", "tools/nk_core/__init__.py", "tools/enhancement_package.py",
                     "tools/history_audit.py"):
            self.assertIn(path, closure)

    def test_the_walk_follows_the_scripts_a_gate_runs(self) -> None:
        # Failing-before (review 2387-22495ab5fee4 S2): only imports were followed, so a script the
        # public-export job runs as a subprocess, such as history_audit.py, was outside the closure.
        self.assertEqual(self._imports("tools/build_public_export.py") & {
            "tools/publish_audit.py", "tools/history_audit.py", "tools/verify_sbom.py"},
            {"tools/publish_audit.py", "tools/history_audit.py", "tools/verify_sbom.py"})

    def test_a_new_tools_module_named_like_the_standard_library_is_protected(self) -> None:
        # Failing-before (review 2387-22495ab5fee4 S3): an added tools/fnmatch.py replaced the fnmatch the
        # audit imports, and no listed path changed.
        for path in ("tools/fnmatch.py", "tools/json/__init__.py", "tools/tempfile.pyc", "tools/glob.PY",
                     "tools/hashlib.cpython-315-x86_64-linux-gnu.so"):
            with self.subTest(path=path):
                protected = ctv.protected_paths({}, {path: "1"}, [])
                self.assertEqual(ctv.protected_changes({}, {path: "1"}, {}, protected),
                                 [{"path": path, "kind": "added"}])
        for path in ("tools/fnmatch_rules.py", "tools/json/data.json", "tools/nk_core/json.py", "docs/json.py"):
            with self.subTest(path=path):
                self.assertNotIn(path, ctv.protected_paths({}, {path: "1"}, []))
        # Nothing in the tree today is named so; the rule costs no approval for an existing file.
        self.assertEqual(sorted(path for path in self.tree if ctv._shadows_stdlib(path)), [])

    def test_the_exemptions_are_real_edges_inside_functions(self) -> None:
        for source, target in sorted(self.NOT_LOADED_BY_A_GATE):
            with self.subTest(source=source, target=target):
                self.assertIn(target, self._imports(source))
                parsed = ast.parse((ROOT / source).read_text(encoding="utf-8"))
                module = Path(target).stem
                top_level = [node for node in parsed.body if isinstance(node, (ast.Import, ast.ImportFrom))
                             and module in ([alias.name for alias in node.names] + [getattr(node, "module", "")])]
                self.assertEqual(top_level, [], "an exempt edge must not run when the module is loaded")
                self.assertNotIn(target, self.protected, "an exempt edge to a protected file needs no exemption")

    def test_every_local_pre_commit_hook_is_protected(self) -> None:
        # The hooks are read from the configuration, so a new one is a root the day it is added.
        for hook in ("tools/publish_audit.py", "tools/policy_sync.py", "tools/modified_file_notice_audit.py",
                     "tools/provenance_record_gap.py", "tools/lint_docs.py"):
            self.assertIn(hook, self.hooks)
        self.assertEqual(sorted(self.hooks - self.protected), [])
        # Every hook that runs a local tool is written in the one form the pattern reads.
        config = (ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8")
        local = re.findall(r"(?m)^\s+entry: (.*tools/.*)$", config)
        self.assertEqual(len(local), len(re.findall(r"(?m)^\s+entry: python tools/[A-Za-z0-9_/]+\.py\b", config)), local)

    def test_a_change_to_the_audit_needs_the_approval(self) -> None:
        for path in ("tools/publish_audit.py", "tools/policy_sync.py", "tools/publication_policy.py",
                     "tools/nk_core/git_isolation.py", ".gitattributes"):
            with self.subTest(path=path):
                trusted = {path: "1"}
                candidate = {path: "2"}
                protected = ctv.protected_paths(trusted, candidate, [])
                self.assertEqual(ctv.protected_changes(trusted, candidate, trusted, protected),
                                 [{"path": path, "kind": "modified"}])

    def test_importing_the_git_environment_helper_loads_no_other_package_module(self) -> None:
        # tools/nk_core/__init__.py resolves its exports on first use. Were it eager, every gate would run
        # the preparation engine and the code generator at import, and they would be gate code too.
        script = ("import sys; sys.path.insert(0, 'tools'); import nk_core.git_isolation; "
                  "print(sorted(name for name in sys.modules if name.split('.')[0] == 'nk_core'))")
        result = subprocess.run([sys.executable, "-c", script], cwd=ROOT, check=True, capture_output=True, text=True)
        self.assertEqual(result.stdout.strip(), "['nk_core', 'nk_core.git_isolation']")

    def test_the_package_still_exports_its_names(self) -> None:
        script = ("import sys; sys.path.insert(0, 'tools'); import nk_core; "
                  "from nk_core import TitleRegistry, inspect_iso, package_cache; "
                  "assert sorted(nk_core.__all__) == sorted(nk_core._EXPORTS); "
                  "assert all(getattr(nk_core, name) is not None for name in nk_core.__all__); "
                  "print(TitleRegistry.__module__, inspect_iso.__module__, package_cache.__name__)")
        result = subprocess.run([sys.executable, "-c", script], cwd=ROOT, check=True, capture_output=True, text=True)
        self.assertEqual(result.stdout.strip(), "nk_core.title_registry nk_core.iso_inspect nk_core.package_cache")
        missing = subprocess.run([sys.executable, "-c", "import sys; sys.path.insert(0, 'tools'); import nk_core; nk_core.nope"],
                                 cwd=ROOT, check=False, capture_output=True, text=True)
        self.assertIn("AttributeError", missing.stderr)


if __name__ == "__main__":
    unittest.main()
