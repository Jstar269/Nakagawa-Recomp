#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Decide the ``Trusted CI verdict`` for one pull-request head (#373, #364).

Why this exists
---------------

The ``main`` ruleset requires ``CI required``, ``Hygiene and security``,
``OSV Vulnerability Scan`` and ``dependency-review``. They come from ordinary
``pull_request`` workflows, whose definitions the pull request controls, and
GitHub matches a required check by its context name plus the GitHub Actions
integration. A pull request can therefore edit or add a workflow, keep a
required job's name and make it ``exit 0``: the context turns green without the
intended gate having run. ``integration_id=15368`` does not tell the intended
job from a candidate's job, because both are GitHub Actions.

This tool is the consumer side of the fix. It runs only from the default
branch (``.github/workflows/ci-trust-verdict.yml``, a ``pull_request_target``,
``workflow_run`` and ``workflow_dispatch`` workflow), treats every candidate
byte as data, and posts one commit status, ``Trusted CI verdict``, bound to the
exact head SHA. The ruleset then requires that status (and CodeQL, which has
its own producer) instead of the candidate-controlled contexts.

GitHub blocks ``pull_request_target`` by default in public repositories from
2026-12-02. Nothing a verdict needs may therefore depend on that event alone:
the provenance attestation and the approval record each have a second route
through ``workflow_run`` and ``workflow_dispatch`` (rules (a) and (b) below).

Trust boundary
--------------

Trusted: this file and its workflow, as checked out from the default branch;
GitHub's API answers about runs, jobs, check runs, statuses, labels and
permissions. Untrusted data: every blob of the candidate, read with
``git ls-tree``/``git cat-file`` and never checked out or executed.

Rules (finding codes)
---------------------

(a) Protected definitions. A protected path is anything under
``.github/workflows/`` or ``.github/actions/``, the CI tools named in
``PROTECTED_EXTRA``, and every ``tools/`` file a default-branch workflow names
outside a comment. A head whose copy differs from the default branch's is

``CI_CHANGE_UNAPPROVED``
    when the candidate changed it and the PR does not carry a current
    approval: the ``ci-change-approved`` label, applied by a user with write
    access, and an approval record for this head's protected content written
    after the label (see "Approval records" below);
``CI_DEFINITION_BEHIND``
    when only the default branch changed it. The integrator merges ``main`` when
    it lands the pull request, not the author, and no label waives it.
``CI_CONTEXT_COLLISION``
    a candidate workflow other than the verdict workflow names the verdict
    context, or any check run is named like it (the verdict is a status).

Approval records
----------------

The label says that a maintainer approves this pull request's CI change; it
does not say which bytes were reviewed. A record does: a SHA-256 digest of
every protected path's mode and bytes in the candidate tree
(``approval_digest``: paths from ``protected_paths``, so added, removed and
renamed paths and the classification code itself change it). A head is approved
when the label is valid and a record for this head's digest was written under
the label's latest ``labeled`` event (``under_label``): by a run created at or
after that event, in a job that completed after it. Removing the label
withdraws every earlier record. A label alone approves nothing, and neither
does the time a head first ran: a pull request can be pushed back to a commit
that ran before the label.

A record is the *name of a job* of the verdict workflow, which only a workflow
definition from the default branch can produce:

* ``CI approval record v2 pr<N> <head> <digest>``, in a ``workflow_dispatch``
  run from the default branch that ran the default branch's current copy of the
  workflow and whose actor has write access
  (``dispatched_records``). The dispatch names the pull request and the head
  that was reviewed; ``--approval dispatch`` refuses unless the label is valid,
  that head still is the pull request's, and the label was applied after that
  head arrived (``label_covers_head``: a label names no commit, and nothing
  removes it on a push once ``pull_request_target`` is refused). This route needs no pull request
  event. The record counts for any head with the same digest, so a push that
  changes no protected content keeps the approval.
* ``CI approval record v1 pr<N> <digest>``, in a ``pull_request_target`` run at
  the approved head (``head_records``). ``--approval record`` writes it on the
  label event; ``--approval carry`` writes it on a push whose digest the
  current label already approved (a record at the previous head, or a
  dispatched one), and the workflow removes the label on any other push. It is
  read only at the head being judged.

The verdict re-derives the digest itself. Every other outcome (no record, an API
failure, a mismatch) leaves the head unapproved.

(b) Intended producers. For each context in ``PRODUCERS`` the latest
non-cancelled run of the intended workflow path and event at this head must
contain exactly one job of that name, completed with ``success``.

The provenance attestation has a second route (``producer_runs``): a
``workflow_run`` or ``workflow_dispatch`` run of its workflow. GitHub lists such
a run under the default branch, so it is bound to the head by its run name,
which carries the head SHA, and it counts only from the default branch and
only when it ran the default branch's current copy of the workflow
(``default_definition``). The newest run of either route decides, newest by
when its latest attempt started (``run_order``), so re-running a run makes it
the newest. A run that has left GitHub's run list is not read, and no older
result stands in for it (``named_runs``). A
``pull_request_target`` run that ended ``startup_failure`` (GitHub refused the
event) is ignored when the second route has a run.

``FOREIGN_PRODUCER``
    a check run or commit status under an intended context that does not come
    from a GitHub Actions run of the intended workflow path and event at this
    head: a same-name job from an edited or added workflow, another app, or a
    status.
``JOB_MISSING`` / ``JOB_FAILED`` / ``JOB_CANCELLED``
    the intended run has no such job, the job did not succeed, or every run
    was cancelled.
``DUPLICATE_CONTEXT``
    the intended run has more than one job answering to the context.

(c) Freshness.

``STALE_HEAD``
    the judged SHA is not the pull request's current head, or the head moved
    while the candidate was being fetched.
``RUN_HEAD_MISMATCH``
    a run, job or check run read for this head names another SHA (a replayed
    result).
``RUN_FOR_OTHER_PR`` / ``AMBIGUOUS_HEAD``
    the intended run belongs to a different pull request, or another open pull
    request has the same head (its base, and so its merge-ref definitions,
    may differ).
``PR_NOT_OPEN`` / ``BASE_NOT_DEFAULT``
    nothing to judge for merge into the default branch.
``DRAFT_LIGHT_CI``
    the pull request is a Draft, or its draft state cannot be read. CI runs only
    the quick checks on a Draft (``tools/ci_paths.py``), so its head is never
    judged green, and no Draft can join a landing batch. Marking it Ready starts
    a CI run, whose completion judges the head again.

(d) Tiers (docs/CI.md, "Quick and full checks"). CI runs a pull request light
(the quick checks) or full. On a light run ``CI required`` does not run: GitHub
lists it as ``skipped``, and the run's own aggregate is the ``CI quick checks``
job. Rule (b) is unchanged and is the whole guarantee: only a ``CI required``
job that concluded ``success`` can make the verdict ``success``, so a head whose
newest CI run is light never is. What a light run changes is the report:

``LIGHT_CI_GREEN``
    not a failure and not ``success``: the newest CI run at this head skipped
    ``CI required`` and its one ``CI quick checks`` job succeeded. The state is
    ``pending`` and the description starts with this code, so the landing desk
    can tell a head that is ready for a batch from one whose checks are still
    running. The batch's own head gets the full checks and its own verdict.

Every other shape of a ``CI required`` that did not succeed stays ``JOB_FAILED``,
as before, naming ``CI quick checks`` when that is the job that failed. Nothing
here reads a label, a branch name or the tier switch: which heads CI runs full
is a cost decision the classifier makes (``tools/ci_paths.py``), and this tool
judges only what ran.

A run that is still going gives ``pending`` rather than a finding. Any fatal
finding makes the verdict ``failure``; ``pending`` holds only when nothing is
fatal; otherwise ``success``.

What it does not do: decide whether candidate tests are honest (that is code
review), or vouch for a status posted under the verdict context by another
GitHub Actions token. The second needs a distinct producer identity (a GitHub
App pinned in the ruleset); see docs/CI.md, "Trusted CI verdict".
"""

from __future__ import annotations
from typing import Any

import argparse
from dataclasses import dataclass, field
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parent))
import provenance_mode  # noqa: E402

VERDICT_CONTEXT = "Trusted CI verdict"
VERDICT_WORKFLOW = ".github/workflows/ci-trust-verdict.yml"
# The aggregate of a full CI run, and of a light one (.github/workflows/ci.yml). Only the first can make a
# verdict success; the second is read to report LIGHT_CI_GREEN (rule (d) above).
FULL_AGGREGATE = "CI required"
QUICK_AGGREGATE = "CI quick checks"
LIGHT_CI_GREEN = "LIGHT_CI_GREEN"
# What a light-green head waits for, as the pending list names it.
LIGHT_PENDING = "the full checks (landing batch)"
LIGHT_DESCRIPTION = (f"{LIGHT_CI_GREEN}: quick checks passed at this head; the full checks run when it is "
                     "batched for landing")
APPROVAL_LABEL = "ci-change-approved"
GITHUB_ACTIONS_APP_ID = 15368
WRITE_PERMISSIONS = frozenset({"admin", "maintain", "write"})
API_ROOT = "https://api.github.com"
_SHA = re.compile(r"[0-9a-f]{40}")
DIGEST_VERSION = "nk-ci-approval-digest-v1"
# The approval record: the name of a successful job of the verdict workflow. v1 is written in a pull_request_target
# run at the approved head; v2 by a dispatch from the default branch that names the pull request and the head.
APPROVAL_RECORD_PREFIX = "CI approval record v1 pr"
_APPROVAL_RECORD = re.compile(re.escape(APPROVAL_RECORD_PREFIX) + r"([1-9][0-9]*) ([0-9a-f]{64})")
APPROVAL_RECORD_V2_PREFIX = "CI approval record v2 pr"
_APPROVAL_RECORD_V2 = re.compile(re.escape(APPROVAL_RECORD_V2_PREFIX) + r"([1-9][0-9]*) ([0-9a-f]{40}) ([0-9a-f]{64})")
# The run name of that dispatch (the workflow's run-name). It only selects the runs whose jobs are read.
APPROVAL_RUN_TITLE = "CI-change approval of #{number} at {head}"
_APPROVAL_RUN_TITLE = re.compile(r"CI-change approval of #([1-9][0-9]*) at ([0-9a-f]{40})")


@dataclass(frozen=True)
class Producer:
    """A required context and the only workflow path and events allowed to produce it.

    ``events`` are the events whose run GitHub attaches to the pull request head. ``named_events`` always run the
    workflow file of the ref they start from and are listed under that ref, never under the pull request head.
    Such a run answers for a head only from the default branch and through its run name: ``run_title`` with the
    head's SHA, which the workflow sets as its ``run-name`` (``producer_runs``).
    """

    context: str
    workflow: str
    events: tuple[str, ...]
    named_events: tuple[str, ...] = ()
    run_title: str = ""


# The required contexts this verdict replaces, plus the provenance attestation
# (#364): its definition is always the default branch's, so here it only needs
# its producer checked. CodeQL is not listed: GitHub Advanced Security
# (integration 57789) produces it, and the ruleset keeps requiring it directly.
PRODUCERS: tuple[Producer, ...] = (
    Producer("CI required", ".github/workflows/ci.yml", ("pull_request",)),
    Producer("Hygiene and security", ".github/workflows/ci.yml", ("pull_request",)),
    Producer("OSV Vulnerability Scan", ".github/workflows/osv-scanner.yml", ("pull_request",)),
    Producer("dependency-review", ".github/workflows/dependency-review.yml", ("pull_request",)),
    Producer(
        "Trusted provenance attestation",
        ".github/workflows/provenance-attestation.yml",
        ("pull_request_target",),
        named_events=("workflow_run", "workflow_dispatch"),
        run_title="Provenance attestation of {head}",
    ),
)


#: The status a fork's passing head gets. The fork's provenance producer is the Fork provenance check, which
#: passes paths a pull request admits itself (it names them in its warnings), so a fork's green must not read
#: like the upstream's, where only the maintainer's admission adds a path.
FORK_SUCCESS_DESCRIPTION = ("Fork: intended jobs passed; provenance is the Fork provenance check, which passes "
                            "paths a PR admits itself (see its summary)")


def status_description(verdict: Verdict, producers: tuple[Producer, ...]) -> str:
    """The status description to post: a fork's success says what its provenance check covers."""
    if verdict.state == "success" and producers != PRODUCERS:
        return FORK_SUCCESS_DESCRIPTION
    return verdict.description()


def fork_run_note(snapshot: dict, producers: tuple[Producer, ...], repository: str) -> str:
    """In a fork, the link to the Fork provenance check run this verdict judged; empty for the upstream.

    That run's summary names the head and the tip it judged and the paths the pull request admits itself. On the
    routes that need no ``pull_request_target`` it is listed under the default branch, not on the pull request, so
    the verdict's report links it. The URL is built from the repository name and the run's numeric ID only.
    """
    if producers is PRODUCERS:
        return ""
    producer = next(item for item in producers if item.context == provenance_mode.FORK_CONTEXT)
    run = latest_live_run(snapshot["workflow_runs"], producer, snapshot["head_sha"], snapshot["default_branch"])
    if run is None:
        return f"; no {provenance_mode.FORK_CONTEXT} run was judged"
    return (f"; judged {provenance_mode.FORK_CONTEXT} run: "
            f"https://github.com/{repository}/actions/runs/{int(run['id'])}")


def producers_for(repository: str | None, repository_id: str | None) -> tuple[Producer, ...]:
    """The producers judged for ``repository``: ``PRODUCERS`` unchanged for the upstream.

    A fork has no access to the upstream's private provenance authority, so its provenance job is the
    Fork provenance check from the same workflow, by the same routes and under the same run title
    (``tools/provenance_mode.py``, ``docs/FORKING.md``). The upstream, matched by ID or by name, always gets
    ``PRODUCERS``.
    """
    if provenance_mode.is_upstream(repository, repository_id):
        return PRODUCERS
    return tuple(
        Producer(provenance_mode.FORK_CONTEXT, producer.workflow, producer.events, producer.named_events,
                 producer.run_title)
        if producer.context == provenance_mode.UPSTREAM_CONTEXT else producer
        for producer in PRODUCERS
    )


PROTECTED_PREFIXES = (".github/workflows/", ".github/actions/")

# Gate code that decides what runs or what passes, protected even where no
# workflow line names it: the classifier and aggregate, test sharding, the
# pre-commit hook set, the pinned tool requirements, and this verifier.
PROTECTED_EXTRA = frozenset({
    ".pre-commit-config.yaml",
    "tools/ci_paths.py",
    "tools/ci_required.py",
    "tools/ci_test_shards.py",
    "tools/ci_test_weights.json",
    "tools/ci_trust_verdict.py",
    # Decides which provenance producer this verifier judges (producers_for).
    "tools/provenance_mode.py",
    "tools/requirements-lock.txt",
}) | frozenset({
    # The publication gate's own code. Only the publication audit refuses a file that is outside the
    # publication profile and is not implementation (game data, a key, a private path). It runs twice: in
    # the attest job from the default branch's checkout, where a pull request cannot touch it, and in
    # ``pull_request`` jobs from the pull request's own checkout, through pre-commit hooks and
    # ``tools/build_public_export.py``. No workflow line names most of these files, so without this list a
    # pull request could loosen the audit for its own run, and a loosened audit that reached the default
    # branch unread would be the trusted one from then on. Listed here: every local hook pre-commit runs,
    # every local module those hooks or the trusted-checkout tools load or run as a subprocess, and
    # ``.gitattributes``, which the audit reads for its Git LFS rule. ``GateCodeTests`` in
    # tools/test_ci_trust_verdict.py computes that closure and fails when a file in it is not protected.
    # A new file in tools/ named like a standard-library module is protected too (``_shadows_stdlib``).
    ".gitattributes",
    "tools/enhancement_package.py",
    "tools/history_audit.py",
    "tools/lint_docs.py",
    "tools/modified_file_notice_audit.py",
    "tools/nk_core/__init__.py",
    "tools/nk_core/git_isolation.py",
    "tools/policy_sync.py",
    "tools/provenance_ledger.py",
    "tools/provenance_record_gap.py",
    "tools/public_candidate.py",
    "tools/public_export.py",
    "tools/publication_policy.py",
    "tools/publish_audit.py",
})

# A tools/ path standing on its own, not the tail of a longer path such as an
# installed copy (``$dir/source/tools/nk_cli.py``) or a module name.
_TOOL_REFERENCE = re.compile(r"(?<![\w./-])(?:\./)?(tools/[A-Za-z0-9_./-]+\.[A-Za-z0-9]+)")


@dataclass(frozen=True)
class Finding:
    code: str
    subject: str
    message: str
    fatal: bool = True


@dataclass
class Verdict:
    state: str
    findings: list[Finding] = field(default_factory=list)
    pending: list[str] = field(default_factory=list)

    def description(self) -> str:
        """The status description: at most 140 characters, GitHub's limit."""
        if self.state == "success":
            text = "All intended CI jobs passed at this exact head from trusted definitions"
        elif self.state == "pending" and self.pending == [LIGHT_PENDING]:
            # Nothing else is outstanding: the head is ready for a landing batch.
            text = LIGHT_DESCRIPTION
        elif self.state == "pending":
            text = "Waiting for: " + ", ".join(self.pending)
        else:
            fatal = [item for item in self.findings if item.fatal]
            codes = sorted({item.code for item in fatal})
            text = f"{len(fatal)} finding(s): " + ", ".join(codes)
        return text if len(text) <= 140 else text[:137] + "..."


# --- Snapshot: the API answers and git facts, normalised ------------------


def _pr_numbers(raw: list[dict] | None) -> list[int]:
    return [int(item["number"]) for item in raw or [] if "number" in item]


def normalise(raw: dict) -> dict:
    """Reduce raw GitHub API payloads to the fields the rules read.

    ``raw`` holds the API documents as GitHub returns them (``pull_request``,
    ``open_pull_requests``, ``label_events``, ``label_actor_permission``,
    ``workflow_runs``, ``jobs`` keyed by run id, ``check_runs``, ``statuses``)
    and the git facts the collector computed (``protected_changes``,
    ``context_collisions``, ``fetched_head``, ``trusted_sha``, ``head_digest``,
    each named run's ``default_definition``) and the approval records it read
    (``approval_records``: digest, time written and route).
    """
    pr = raw.get("pull_request")
    pull_request = None
    if pr is not None:
        head_repo = (pr.get("head") or {}).get("repo") or {}
        base_repo = (pr.get("base") or {}).get("repo") or {}
        pull_request = {
            "number": int(pr["number"]),
            "state": pr.get("state"),
            "head_sha": (pr.get("head") or {}).get("sha"),
            "base_ref": (pr.get("base") or {}).get("ref"),
            "fork": head_repo.get("full_name") != base_repo.get("full_name"),
            # GitHub's pulls API always answers a boolean; anything else is judged as a Draft.
            "draft": pr.get("draft"),
            "labels": sorted(label.get("name", "") for label in pr.get("labels") or []),
        }
    approval = None
    events = [
        event for event in raw.get("label_events") or []
        if event.get("event") in ("labeled", "unlabeled")
        and (event.get("label") or {}).get("name") == APPROVAL_LABEL
    ]
    if events:
        last = max(events, key=lambda event: (event.get("created_at") or "", event.get("id") or 0))
        approval = {
            "event": last["event"],
            "actor": (last.get("actor") or {}).get("login"),
            "created_at": last.get("created_at"),
            "actor_permission": raw.get("label_actor_permission"),
        }
    return {
        "head_sha": raw["head_sha"],
        "default_branch": raw["default_branch"],
        "trusted_sha": raw.get("trusted_sha"),
        "fetched_head": raw.get("fetched_head"),
        "pull_request": pull_request,
        "open_pull_requests": [
            {
                "number": int(item["number"]),
                "head_sha": (item.get("head") or {}).get("sha"),
                "base_ref": (item.get("base") or {}).get("ref"),
            }
            for item in raw.get("open_pull_requests") or []
        ],
        "approval": approval,
        "head_digest": raw.get("head_digest"),
        "approval_records": sorted(
            ({
                "digest": record.get("digest"),
                "recorded_at": record.get("recorded_at") or "",
                "run_created_at": record.get("run_created_at") or "",
                "route": record.get("route"),
                "head_sha": record.get("head_sha") or "",
                "actor": record.get("actor"),
            } for record in raw.get("approval_records") or [] if isinstance(record, dict)),
            key=lambda record: (record["recorded_at"], str(record["digest"]), str(record["route"])),
        ),
        "approval_list_truncated": raw.get("approval_list_truncated") is True,
        "retargeted": any(event.get("event") == "base_ref_changed" for event in raw.get("label_events") or []),
        "workflow_runs": [
            {
                "id": int(run["id"]),
                "path": run.get("path"),
                "event": run.get("event"),
                "status": run.get("status"),
                "conclusion": run.get("conclusion"),
                "head_sha": run.get("head_sha"),
                "head_branch": run.get("head_branch"),
                "display_title": run.get("display_title"),
                # A git fact, not GitHub's: only the collector's own check sets it (``named_runs``).
                "default_definition": run.get("default_definition") is True,
                # Also the collector's own finding: the run completed without starting any job.
                "jobless": run.get("jobless") is True,
                "created_at": run.get("created_at") or "",
                "run_started_at": run.get("run_started_at") or "",
                "check_suite_id": run.get("check_suite_id"),
                "pull_requests": _pr_numbers(run.get("pull_requests")),
            }
            for run in raw.get("workflow_runs") or []
        ],
        "jobs": {
            str(run_id): [
                {
                    "id": int(job["id"]),
                    "name": job.get("name"),
                    "status": job.get("status"),
                    "conclusion": job.get("conclusion"),
                    "head_sha": job.get("head_sha"),
                }
                for job in jobs
            ]
            for run_id, jobs in (raw.get("jobs") or {}).items()
        },
        "check_runs": [
            {
                "id": int(check["id"]),
                "name": check.get("name"),
                "app_id": (check.get("app") or {}).get("id"),
                "check_suite_id": (check.get("check_suite") or {}).get("id"),
                "status": check.get("status"),
                "conclusion": check.get("conclusion"),
                "head_sha": check.get("head_sha"),
            }
            for check in raw.get("check_runs") or []
        ],
        "statuses": [
            {"context": status.get("context"), "state": status.get("state")}
            for status in raw.get("statuses") or []
        ],
        "protected_changes": list(raw.get("protected_changes") or []),
        "context_collisions": sorted(raw.get("context_collisions") or []),
    }


# --- Rules -----------------------------------------------------------------


def _when(stamp: str | None) -> datetime | None:
    """A GitHub timestamp, or ``None`` for a missing or unreadable one (which then proves nothing)."""
    if not stamp:
        return None
    try:
        when = datetime.fromisoformat(stamp.replace("Z", "+00:00"))
    except ValueError:
        return None
    return when if when.tzinfo is not None else None


def under_label(record: dict, labelled: datetime | None) -> bool:
    """Whether ``record`` was written under the label event at ``labelled``.

    Its run was created at or after that event and its job completed after it. A run created before the label
    answers an earlier event: a label somebody else applied, or a push before this one. Re-running it later, or
    its starting late from a long queue, would write a record dated after a label that was applied for other
    bytes; the creation time does not change on a re-run, so such a record is refused here. A record with a
    missing or unreadable time proves nothing.
    """
    created, written = _when(record.get("run_created_at")), _when(record.get("recorded_at"))
    return (labelled is not None and created is not None and written is not None
            and created >= labelled and written > labelled)


def _approval(snapshot: dict) -> tuple[bool, str]:
    pr = snapshot["pull_request"] or {}
    if APPROVAL_LABEL not in pr.get("labels", []):
        return False, f"no {APPROVAL_LABEL!r} label"
    event = snapshot["approval"]
    if event is None or event["event"] != "labeled":
        return False, f"no current {APPROVAL_LABEL!r} label event"
    if event["actor_permission"] not in WRITE_PERMISSIONS:
        return False, (f"{APPROVAL_LABEL!r} was applied by {event['actor']!r}, "
                       f"whose permission is {event['actor_permission']!r}, not write")
    # The label names no bytes. This head's protected content, hashed here by trusted code, must equal a digest
    # that a default-branch run of the verdict workflow recorded after the label was applied. When a head first
    # ran proves nothing: a pull request can be pushed back to a commit that ran before the label.
    labelled = _when(event["created_at"])
    digest = snapshot["head_digest"]
    for record in snapshot["approval_records"] if digest else []:
        if record["digest"] != digest or not under_label(record, labelled):
            continue
        if record["route"] == "dispatch":
            return True, (f"{APPROVAL_LABEL!r} applied by {event['actor']!r}; {record['actor']!r} recorded the "
                          f"approval of this protected content (digest {digest[:12]}) at head "
                          f"{record['head_sha'][:12]}")
        # A pull_request_target run takes its workflow from the pull request's base branch, which was not the
        # default branch for all of a retargeted pull request's life.
        if record["route"] == "head" and not snapshot["retargeted"]:
            return True, (f"{APPROVAL_LABEL!r} applied by {event['actor']!r} and recorded at this head: its "
                          f"protected content is byte-identical to the approved head's (digest {digest[:12]})")
    reason = (f"{APPROVAL_LABEL!r} is on the pull request, but no approval record written after it matches "
              "this head's protected content; review this head and record the approval for it")
    if snapshot["approval_list_truncated"]:
        reason += (f" (GitHub's list of approval dispatches since the label is incomplete: it holds at most "
                   f"{RUN_LIST_LIMIT}, or came without a count, so an older record cannot be seen; dispatch the "
                   "approval again)")
    return False, reason


def _check_pull_request(snapshot: dict) -> list[Finding]:
    head = snapshot["head_sha"]
    pr = snapshot["pull_request"]
    if pr is None:
        return [Finding("STALE_HEAD", head, "no open pull request has this head")]
    findings = []
    if pr["state"] != "open":
        findings.append(Finding("PR_NOT_OPEN", f"#{pr['number']}", f"pull request is {pr['state']}"))
    if pr["base_ref"] != snapshot["default_branch"]:
        findings.append(Finding(
            "BASE_NOT_DEFAULT", f"#{pr['number']}",
            f"base is {pr['base_ref']!r}, not the default branch {snapshot['default_branch']!r}",
        ))
    if pr["head_sha"] != head:
        findings.append(Finding(
            "STALE_HEAD", head,
            f"pull request #{pr['number']} now has head {pr['head_sha']}; this result is stale",
        ))
    fetched = snapshot["fetched_head"]
    if fetched is not None and fetched != head:
        findings.append(Finding(
            "STALE_HEAD", head, f"the pull request head moved to {fetched} while it was being read",
        ))
    if pr["draft"] is True:
        findings.append(Finding(
            "DRAFT_LIGHT_CI", f"#{pr['number']}",
            "the pull request is a Draft, so CI ran only the quick checks and it cannot be batched for landing; "
            "mark it Ready for review when it is finished",
        ))
    elif pr["draft"] is not False:
        findings.append(Finding(
            "DRAFT_LIGHT_CI", f"#{pr['number']}",
            f"the pull request's draft state is {pr['draft']!r}, not false; a head that may be a Draft is not green",
        ))
    for other in snapshot["open_pull_requests"]:
        if other["head_sha"] == head and other["number"] != pr["number"]:
            findings.append(Finding(
                "AMBIGUOUS_HEAD", f"#{other['number']}",
                f"open pull request #{other['number']} (base {other['base_ref']!r}) has the same head; "
                "its runs may use other definitions",
            ))
    return findings


def _check_definitions(snapshot: dict) -> list[Finding]:
    findings = []
    approved, reason = _approval(snapshot)
    for change in snapshot["protected_changes"]:
        path = change["path"]
        if change["kind"] == "behind":
            findings.append(Finding(
                "CI_DEFINITION_BEHIND", path,
                "the default branch changed this CI definition after the branch point; the integrator"
                " merges main when it lands the pull request, not the author",
            ))
        elif approved:
            findings.append(Finding("CI_CHANGE_APPROVED", path, reason, fatal=False))
        else:
            findings.append(Finding(
                "CI_CHANGE_UNAPPROVED", path,
                f"the candidate changes a CI definition or gate tool ({change['kind']}); {reason}",
            ))
    for path in snapshot["context_collisions"]:
        findings.append(Finding(
            "CI_CONTEXT_COLLISION", path,
            f"candidate workflow names the reserved context {VERDICT_CONTEXT!r}",
        ))
    for check in snapshot["check_runs"]:
        if check["name"] == VERDICT_CONTEXT:
            findings.append(Finding(
                "CI_CONTEXT_COLLISION", f"check run {check['id']}",
                f"a check run is named {VERDICT_CONTEXT!r}; the verdict is only ever a commit status",
            ))
    return findings


def run_order(run: dict) -> tuple[str, int]:
    """Sort key for "the newest run": when its latest attempt started, then its id.

    A re-run keeps the run's id and creation time and gets a new start time. It is a fresh result for the same
    head, made at re-run time by the same definition, and only a user with write access can ask for one, so it
    is the newest evidence. Ordering by creation time instead left a head stuck: with two attestation routes the
    run a pull request's check list shows is rarely the newest-created one, and re-running it changed nothing.
    Approval records are the exception and keep the creation time (``under_label``): there the event that
    created the run is the point.
    """
    return (run.get("run_started_at") or run.get("created_at") or "", int(run["id"]))


def producer_runs(runs: list[dict], producer: Producer, head_sha: str, default_branch: str) -> list[dict]:
    """The runs that can answer for ``producer`` at ``head_sha``, from raw or normalised run records.

    Two routes. A run of one of ``producer.events`` at exactly this head. And a run of one of
    ``producer.named_events``, which GitHub lists under the ref it started from: it counts only when that ref is
    the default branch, it ran the trusted checkout's own copy of the workflow (``default_definition``, a git
    fact from ``named_runs``), and its run name is ``producer.run_title`` for this head. A run named for another
    head, started from another ref or another copy of the workflow, or of any other event is not a candidate.

    A head-attached run that ended ``startup_failure`` started no job: GitHub refused its event, as it does for
    ``pull_request_target`` during the 2026 brownouts and by default from 2026-12-02. GitHub ended some refused
    runs as ``failure`` with no job instead ("workflow file issue", 2026-11-09); the collector marks a completed
    run that has no job at all ``jobless``. Either kind attested nothing and failed nothing, and is dropped when
    the named route has a run that was not cancelled, so a refused event cannot mask an attestation that ran.
    With no such run it stays, and the head fails for want of any attestation.
    """
    mine = [run for run in runs if run.get("path") == producer.workflow]
    attached = [run for run in mine if run.get("event") in producer.events and run.get("head_sha") == head_sha]
    title = producer.run_title.format(head=head_sha) if producer.run_title else None
    named = [run for run in mine
             if title is not None and run.get("event") in producer.named_events
             and run.get("head_branch") == default_branch and run.get("default_definition") is True
             and run.get("display_title") == title]
    if any(run.get("conclusion") != "cancelled" for run in named):
        attached = [run for run in attached
                    if run.get("conclusion") != "startup_failure" and run.get("jobless") is not True]
    return attached + named


def _check_producer(snapshot: dict, producer: Producer, pending: list[str]) -> list[Finding]:
    head = snapshot["head_sha"]
    pr = snapshot["pull_request"] or {}
    runs_by_suite = {run["check_suite_id"]: run for run in snapshot["workflow_runs"]}
    findings = []

    for check in snapshot["check_runs"]:
        if check["name"] != producer.context:
            continue
        run = runs_by_suite.get(check["check_suite_id"])
        if check["head_sha"] != head:
            findings.append(Finding(
                "RUN_HEAD_MISMATCH", producer.context,
                f"check run {check['id']} is for {check['head_sha']}, not this head",
            ))
        if check["app_id"] != GITHUB_ACTIONS_APP_ID or run is None:
            findings.append(Finding(
                "FOREIGN_PRODUCER", producer.context,
                f"check run {check['id']} (app {check['app_id']}) is not from a workflow run at this head",
            ))
        elif run["path"] != producer.workflow or run["event"] not in producer.events:
            findings.append(Finding(
                "FOREIGN_PRODUCER", producer.context,
                f"check run {check['id']} comes from {run['path']} ({run['event']}), "
                f"not {producer.workflow} ({'/'.join(producer.events)})",
            ))
    for status in snapshot["statuses"]:
        if status["context"] == producer.context:
            findings.append(Finding(
                "FOREIGN_PRODUCER", producer.context,
                f"a commit status ({status['state']}) uses this context; only the job may produce it",
            ))

    runs = [
        run for run in snapshot["workflow_runs"]
        if run["path"] == producer.workflow and run["event"] in producer.events
    ]
    for run in runs:
        if run["head_sha"] != head:
            findings.append(Finding(
                "RUN_HEAD_MISMATCH", producer.context,
                f"run {run['id']} is for {run['head_sha']}, not this head",
            ))
    runs = producer_runs(snapshot["workflow_runs"], producer, head, snapshot["default_branch"])
    live = [run for run in runs if run["conclusion"] != "cancelled"]
    if not runs:
        pending.append(producer.context)
        return findings
    if not live:
        findings.append(Finding(
            "JOB_CANCELLED", producer.context, f"every {producer.workflow} run for this head was cancelled; re-run it",
        ))
        return findings
    latest = max(live, key=run_order)
    # A named run and its jobs report the default-branch commit the run started from; its run name is what binds
    # it to this head, and ``producer_runs`` matched that.
    named = latest["event"] in producer.named_events
    run_sha = latest["head_sha"] if named else head
    if not named and latest["pull_requests"] and pr.get("number") not in latest["pull_requests"]:
        findings.append(Finding(
            "RUN_FOR_OTHER_PR", producer.context,
            f"run {latest['id']} belongs to pull request(s) {latest['pull_requests']}",
        ))
    jobs = [job for job in snapshot["jobs"].get(str(latest["id"]), []) if job["name"] == producer.context]
    if len(jobs) > 1:
        findings.append(Finding(
            "DUPLICATE_CONTEXT", producer.context,
            f"run {latest['id']} has {len(jobs)} jobs named {producer.context!r}",
        ))
        return findings
    if not jobs:
        if latest["status"] == "completed":
            findings.append(Finding(
                "JOB_MISSING", producer.context,
                f"run {latest['id']} of {producer.workflow} finished without a {producer.context!r} job",
            ))
        else:
            pending.append(producer.context)
        return findings
    job = jobs[0]
    if job["head_sha"] != run_sha:
        findings.append(Finding(
            "RUN_HEAD_MISMATCH", producer.context,
            f"job {job['id']} is for {job['head_sha']}, not {'its run' if named else 'this head'}",
        ))
    elif job["status"] != "completed":
        pending.append(producer.context)
    elif job["conclusion"] != "success":
        findings += _not_success(snapshot, producer, latest, job, pending)
    return findings


def _not_success(snapshot: dict, producer: Producer, run: dict, job: dict, pending: list[str]) -> list[Finding]:
    """The findings for an intended job that completed without ``success``. Never empty-handed: it returns a
    fatal finding or adds to ``pending``, so the caller's verdict cannot become ``success`` through here.

    One shape is not a failure (rule (d)): ``CI required`` skipped, in a run whose single ``CI quick checks``
    job succeeded at this head. That is a light run with green quick checks, reported as ``LIGHT_CI_GREEN``.
    The quick job is read from this run's own job list, so a job of that name in another workflow or run
    cannot stand in for it. Every other shape is ``JOB_FAILED``.
    """
    failed = Finding("JOB_FAILED", producer.context, f"job {job['id']} concluded {job['conclusion']!r}")
    if producer.context != FULL_AGGREGATE or job["conclusion"] != "skipped":
        return [failed]
    quick = [item for item in snapshot["jobs"].get(str(run["id"]), []) if item["name"] == QUICK_AGGREGATE]
    if len(quick) != 1 or quick[0]["head_sha"] != snapshot["head_sha"]:
        return [failed]
    if quick[0]["status"] != "completed":
        pending.append(QUICK_AGGREGATE)
        return []
    if quick[0]["conclusion"] != "success":
        return [Finding(
            "JOB_FAILED", QUICK_AGGREGATE,
            f"job {quick[0]['id']} concluded {quick[0]['conclusion']!r}; {FULL_AGGREGATE!r} did not run "
            "(a light run)",
        )]
    pending.append(LIGHT_PENDING)
    return [Finding(
        LIGHT_CI_GREEN, FULL_AGGREGATE,
        f"run {run['id']} was a light run: {QUICK_AGGREGATE!r} passed and {FULL_AGGREGATE!r} did not run; "
        "the full checks run on the landing batch, or here when a maintainer adds the ci-full-matrix label "
        "and re-runs CI",
        fatal=False,
    )]


def evaluate(snapshot: dict, producers: tuple[Producer, ...] = PRODUCERS) -> Verdict:
    """Apply every rule to a normalised snapshot. Pure: no I/O."""
    findings = _check_pull_request(snapshot) + _check_definitions(snapshot)
    pending: list[str] = []
    for producer in producers:
        findings += _check_producer(snapshot, producer, pending)
    if any(item.fatal for item in findings):
        state = "failure"
    elif pending:
        state = "pending"
    else:
        state = "success"
    return Verdict(state, findings, pending)


# --- Git facts (candidate objects are data, never checked out) -------------


def _git(repo: Path, *args: str) -> bytes:
    return subprocess.run(
        ["git", "-C", str(repo), *args], check=True, capture_output=True,
    ).stdout


def tree_entries(repo: Path, rev: str) -> dict[str, str]:
    """Map every path in ``rev``'s tree to ``mode:object``."""
    entries = {}
    for record in _git(repo, "ls-tree", "-r", "-z", "--full-tree", rev).split(b"\0"):
        if not record:
            continue
        meta, _, path = record.partition(b"\t")
        mode, _kind, obj = meta.decode("ascii").split(" ")
        entries[path.decode("utf-8", "surrogateescape")] = f"{mode}:{obj}"
    return entries


def referenced_tools(workflow_text: str) -> set[str]:
    """``tools/`` paths a workflow names outside a comment line."""
    found: set[Any] = set()
    for line in workflow_text.splitlines():
        if line.lstrip().startswith("#"):
            continue
        found.update(match.group(1) for match in _TOOL_REFERENCE.finditer(line))
    return found


#: What Python imports from a directory on ``sys.path``: a module file or a package's ``__init__``.
_IMPORTABLE_SUFFIXES = (".py", ".pyc", ".pyw", ".pyd", ".so")


def _shadows_stdlib(path: str) -> bool:
    """Whether ``path`` would be imported in place of a standard-library module by a gate tool.

    The gate tools run as ``python tools/X.py``, which puts ``tools/`` first on ``sys.path``, so a
    ``tools/fnmatch.py`` replaces the ``fnmatch`` the publication audit imports. Adding such a file
    changes the gate's code without touching a listed path.
    """
    parts = path.split("/")
    if parts[0] != "tools" or len(parts) not in (2, 3):
        return False
    if len(parts) == 3:
        name, leaf = parts[1], parts[2]
        if not (leaf.startswith("__init__.") and leaf.lower().endswith(_IMPORTABLE_SUFFIXES)):
            return False
    else:
        name, leaf = parts[1].split(".", 1)[0], parts[1]
        if not leaf.lower().endswith(_IMPORTABLE_SUFFIXES):
            return False
    return name in sys.stdlib_module_names


def protected_paths(trusted: dict[str, str], candidate: dict[str, str], workflow_texts: list[str]) -> set[str]:
    """Paths whose bytes define or decide the CI gates."""
    paths = {path for path in set(trusted) | set(candidate) if path.startswith(PROTECTED_PREFIXES)}
    paths |= PROTECTED_EXTRA
    paths |= {path for path in set(trusted) | set(candidate) if _shadows_stdlib(path)}
    for text in workflow_texts:
        paths |= {path for path in referenced_tools(text) if path in trusted}
    return paths


def protected_changes(
    trusted: dict[str, str], candidate: dict[str, str], merge_base: dict[str, str], paths: set[str],
) -> list[dict]:
    """Protected paths whose candidate copy differs from the default branch's.

    ``kind`` is ``behind`` when the candidate still has the merge base's copy
    (only the default branch moved), otherwise added, deleted or modified.
    """
    changes = []
    for path in sorted(paths):
        ours, theirs, base = trusted.get(path), candidate.get(path), merge_base.get(path)
        if ours == theirs:
            continue
        if theirs == base:
            kind = "behind"
        elif ours is None:
            kind = "added"
        elif theirs is None:
            kind = "deleted"
        else:
            kind = "modified"
        changes.append({"path": path, "kind": kind})
    return changes


def context_collisions(repo: Path, candidate_rev: str, candidate: dict[str, str]) -> list[str]:
    needle = VERDICT_CONTEXT.encode("utf-8")
    hits = []
    for path, entry in sorted(candidate.items()):
        if not path.startswith(".github/workflows/") or path == VERDICT_WORKFLOW:
            continue
        if needle in _git(repo, "cat-file", "blob", entry.split(":", 1)[1]):
            hits.append(path)
    return hits


def fetch_candidate(repo: Path, repository: str, number: int, remote: str | None = None) -> str:
    """Fetch ``refs/pull/N/head`` as objects only; return the SHA it points at now."""
    ref = f"refs/nk-candidate/pr-{number}"
    _git(repo, "fetch", "--no-tags", "--no-recurse-submodules", "--quiet",
         remote or f"https://github.com/{repository}.git", f"+refs/pull/{number}/head:{ref}")
    return _git(repo, "rev-parse", ref).decode("ascii").strip()


def trusted_workflow_texts(repo: Path, trusted: dict[str, str]) -> list[str]:
    return [
        _git(repo, "cat-file", "blob", entry.split(":", 1)[1]).decode("utf-8", "replace")
        for path, entry in sorted(trusted.items())
        if path.startswith(".github/workflows/") and path.endswith((".yml", ".yaml"))
    ]


def git_facts(repo: Path, repository: str, number: int, head_sha: str, remote: str | None = None) -> dict:
    """Fetch ``refs/pull/N/head`` as objects only and compare its CI definitions."""
    fetched = fetch_candidate(repo, repository, number, remote)
    facts: dict = {"fetched_head": fetched, "trusted_sha": _git(repo, "rev-parse", "HEAD").decode("ascii").strip()}
    if fetched != head_sha:
        return facts | {"protected_changes": [], "context_collisions": []}
    trusted = tree_entries(repo, "HEAD")
    candidate = tree_entries(repo, head_sha)
    merge_base_sha = _git(repo, "merge-base", "HEAD", head_sha).decode("ascii").strip()
    merge_base = tree_entries(repo, merge_base_sha)
    paths = protected_paths(trusted, candidate, trusted_workflow_texts(repo, trusted))
    return facts | {
        "protected_changes": protected_changes(trusted, candidate, merge_base, paths),
        "context_collisions": context_collisions(repo, head_sha, candidate),
    }


def on_default_branch(repo: Path, sha: str) -> bool:
    """Whether ``sha`` is the trusted checkout's tip or one of its ancestors: a commit of the default branch.

    GitHub runs a ``workflow_dispatch`` from the ref the dispatcher picks, with that ref's workflow file, and
    reports only the ref's short name. A branch or a tag can be named anything, so the name does not prove whose
    definition ran; the commit and its copy of the file do (``default_definition``). An unknown object, as any
    git error, is not on the default branch.
    """
    if not _SHA.fullmatch(sha or ""):
        return False
    return subprocess.run(
        ["git", "-C", str(repo), "merge-base", "--is-ancestor", sha, "HEAD"], check=False, capture_output=True,
    ).returncode == 0


def default_definition(repo: Path, sha: str, workflow: str) -> bool:
    """Whether a run that started from ``sha`` ran the trusted checkout's own copy of ``workflow``.

    The commit must be on the default branch, and its copy of the workflow file must be byte-identical to the
    trusted checkout's. Being on the default branch is not enough: a rebase merge puts every commit of a pull
    request there, while only the pull request's last tree was judged, so an earlier commit can hold a copy nobody
    approved. A run of an older copy therefore does not count; a head attested under one is attested again, and
    such a head is ``CI_DEFINITION_BEHIND`` in any case.
    """
    if not on_default_branch(repo, sha):
        return False
    copies = [
        subprocess.run(["git", "-C", str(repo), "rev-parse", "--verify", "--quiet", f"{rev}:{workflow}"],
                       check=False, capture_output=True)
        for rev in (sha, "HEAD")
    ]
    blobs = {copy.stdout.strip() for copy in copies}
    return all(copy.returncode == 0 for copy in copies) and len(blobs) == 1 and blobs != {b""}


def approval_digest(repo: Path, head_sha: str) -> str:
    """SHA-256 over the mode and bytes of every protected path in ``head_sha``'s tree.

    The path set is ``protected_paths`` with the trusted checkout's workflows, exactly as the verdict's own
    classification, plus every ``tools/`` path that a workflow or action of ``head_sha`` itself names. A protected path absent from the candidate contributes nothing, so a path added to
    or removed from the candidate changes the digest, and a rename is a removal plus an addition. The candidate's
    copy of this tool is itself a protected path. The path is NUL-terminated and the rest of a record has a fixed
    shape, so no two different trees give the same preimage.
    """
    trusted = tree_entries(repo, "HEAD")
    candidate = tree_entries(repo, head_sha)
    paths = protected_paths(trusted, candidate, trusted_workflow_texts(repo, trusted))
    # The verdict's own classification names a tool only when a default-branch workflow does. A tool the
    # candidate's workflows or actions newly run (or newly stop running) is exactly what a reviewed head can
    # change later without touching a workflow, so it is hashed too, and a reference that only the candidate
    # makes cannot be missed. Absent paths contribute nothing, as above.
    for named, blob in candidate.items():
        if named.startswith(PROTECTED_PREFIXES) and named.endswith((".yml", ".yaml")):
            text = _git(repo, "cat-file", "blob", blob.split(":", 1)[1]).decode("utf-8", "replace")
            paths |= {tool for tool in referenced_tools(text) if tool in candidate}
    digest = hashlib.sha256(DIGEST_VERSION.encode("ascii") + b"\n")
    for path in sorted(paths):
        entry = candidate.get(path)
        if entry is None:
            continue
        mode, oid = entry.split(":", 1)
        body = oid.encode("ascii") if mode == "160000" else _git(repo, "cat-file", "blob", oid)
        digest.update(path.encode("utf-8", "surrogateescape") + b"\0" + mode.encode("ascii") + b"\0"
                      + hashlib.sha256(body).hexdigest().encode("ascii") + b"\n")
    return digest.hexdigest()


# --- GitHub API ------------------------------------------------------------


# GitHub's API answers a share of requests with a gateway error under load. One such answer made the verifier
# post "could not judge this head: HTTPError" on several pull requests at once (HTTP 502, 2026-10-30), and each
# needed a manual re-run. These statuses, and a dropped connection, are retried; any other answer is final.
TRANSIENT_HTTP = frozenset({500, 502, 503, 504})
RETRY_DELAYS = (5, 15)   # seconds before the second and third attempt
# Answers to the read token that send the read to GITHUB_TOKEN instead: a revoked or expired token (401), a
# missing permission or a spent budget (403, 429), and a fine-grained token's 404 for a resource it may not see.
READ_TOKEN_REFUSALS = frozenset({401, 403, 404, 429})


class GitHub:
    def __init__(self, repository: str, token: str, read_token: str | None = None) -> None:
        self.repository = repository
        self.token = token
        # The repository's GITHUB_TOKEN shares one hourly API budget across every workflow. Its use ran out on
        # 2026-10-31 and failed Trusted CodeQL four times in an hour. Reads therefore use ``read_token`` when it
        # is set: the maintainer's read-only VERDICT_READ_TOKEN, whose budget is the maintainer's own. Writes
        # (the status) always use ``token``, and a read the read token is refused (a missing permission, or
        # its own budget spent) is retried once with ``token``. Each run logs what it spent and what each
        # token's last answer said was left (``budget_note``).
        self.read_token = read_token or None
        self.requests = 0
        self.read_requests = 0
        self.read_refusals = 0
        self.rate_limit: dict[str, dict[str, str]] = {}

    def _request(self, method: str, path: str, body: dict | None = None) -> tuple[object, dict]:
        if method != "GET" or self.read_token is None:
            return self._send(method, path, body, self.token, "token")
        try:
            return self._send(method, path, body, self.read_token, "read token")
        except urllib.error.HTTPError as error:
            if error.code not in READ_TOKEN_REFUSALS:
                raise
            if not self.read_refusals:
                print(f"GET {path}: the read token was refused (HTTP {error.code}); using GITHUB_TOKEN for it",
                      file=sys.stderr)
            self.read_refusals += 1
            return self._send(method, path, body, self.token, "token")

    def _send(self, method: str, path: str, body: dict | None, token: str, label: str) -> tuple[object, dict]:
        url = path if path.startswith("https://") else f"{API_ROOT}/repos/{self.repository}/{path}"
        data = json.dumps(body).encode("utf-8") if body is not None else None
        request = urllib.request.Request(url, data=data, method=method, headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {token}",
            "X-GitHub-Api-Version": "2022-11-28",
            "User-Agent": "nakagawa-recomp-ci-trust-verdict",
        })
        for delay in (*RETRY_DELAYS, None):
            try:
                self.requests += 1
                if label == "read token":
                    self.read_requests += 1
                with urllib.request.urlopen(request, timeout=60) as response:
                    payload = response.read()
                    headers = dict(response.headers)
                    self._note_rate_limit(label, headers)
                    return (json.loads(payload) if payload else None), headers
            except urllib.error.HTTPError as error:
                self._note_rate_limit(label, dict(error.headers or {}))
                if delay is None or error.code not in TRANSIENT_HTTP:
                    raise
            except (urllib.error.URLError, TimeoutError):
                if delay is None:
                    raise
            print(f"{method} {url}: transient GitHub error; retrying in {delay} s", file=sys.stderr)
            time.sleep(delay)
        raise AssertionError("unreachable")

    def get(self, path: str) -> object:
        return self._request("GET", path)[0]

    def paginate(self, path: str, key: str | None = None) -> list:
        return self.counted(path, key)[0]

    def counted(self, path: str, key: str | None = None) -> tuple[list, int | None]:
        """Every page of a list, and the ``total_count`` its first page reports (``None`` when it reports none)."""
        items: list = []
        total: int | None = None
        url: str | None = path + ("&" if "?" in path else "?") + "per_page=100"
        while url:
            payload, headers = self._request("GET", url)
            if key and total is None and isinstance(payload, dict) and isinstance(payload.get("total_count"), int):
                total = payload["total_count"]
            items.extend(payload[key] if key else payload)
            match = re.search(r'<([^>]+)>;\s*rel="next"', headers.get("Link", ""))
            url = match.group(1) if match else None
        return items, total

    def post(self, path: str, body: dict) -> object:
        return self._request("POST", path, body)[0]

    def _note_rate_limit(self, label: str, headers: dict) -> None:
        lowered = {str(name).lower(): str(value) for name, value in headers.items()}
        fields = {field: lowered[f"x-ratelimit-{field}"]
                  for field in ("limit", "remaining", "used", "reset", "resource")
                  if f"x-ratelimit-{field}" in lowered}
        if fields:
            self.rate_limit[label] = fields

    def budget_note(self) -> str:
        """One log line: this run's requests and each token's budget as its last answer reported it."""
        note = f"GitHub API: {self.requests} request(s) this run"
        if self.read_requests:
            note += f" ({self.read_requests} with the read token, {self.read_refusals} refused)"
        for label in ("read token", "token"):
            budget = self.rate_limit.get(label, {})
            if "remaining" in budget and "limit" in budget:
                owner = "the token's" if label == "token" and not self.read_token else f"the {label}'s"
                if label == "token" and self.read_token:
                    owner = "GITHUB_TOKEN's"
                note += (f"; {owner} {budget.get('resource', 'core')} budget had {budget['remaining']} "
                         f"of {budget['limit']} left")
                if budget.get("reset", "").isdigit():
                    reset = time.strftime("%H:%M:%SZ", time.gmtime(int(budget["reset"])))
                    note += f", resetting at {reset}"
        return note


def head_records(runs: list[dict], jobs_of, head_sha: str, number: int, default_branch: str) -> list[dict]:
    """Records at ``head_sha`` for pull request ``number``: successful ``CI approval record v1 pr<N> <digest>``
    jobs of this workflow, each with its digest and when its job completed.

    Only a ``pull_request_target`` run of the verdict workflow at exactly this head counts. A pull request cannot
    choose a job name in such a run: GitHub takes its workflow definition from the pull request's base branch,
    not from the pull request. That is the default branch only when the pull request targets it, so a run that
    GitHub reports as belonging to a pull request on another base, or to another pull request, is ignored, and
    so is a job whose name carries another pull request's number (the same SHA can head two pull requests). A job
    in a ``pull_request`` run, another workflow, another head or a failed job is ignored. GitHub leaves a fork
    pull request's run without its pull requests; ``label_approver`` and ``_approval`` close that case by refusing
    a pull request that was ever retargeted. ``jobs_of(run_id)`` returns that run's jobs.
    """
    found: list[dict] = []
    for run in runs:
        if (run.get("path") != VERDICT_WORKFLOW or run.get("event") != "pull_request_target"
                or run.get("head_sha") != head_sha):
            continue
        reported = run.get("pull_requests") or []
        if any((item.get("base") or {}).get("ref") != default_branch for item in reported):
            continue
        if reported and number not in {item.get("number") for item in reported}:
            continue
        for job in jobs_of(int(run["id"])):
            match = _APPROVAL_RECORD.fullmatch(job.get("name") or "")
            if (match and int(match.group(1)) == number and job.get("head_sha") == head_sha
                    and job.get("status") == "completed" and job.get("conclusion") == "success"):
                found.append({"digest": match.group(2), "recorded_at": job.get("completed_at") or "",
                              "run_created_at": run.get("created_at") or "",
                              "route": "head", "head_sha": head_sha, "actor": None})
    return found


def _jobs_of(gh: "GitHub"):
    return lambda run_id: gh.paginate(f"actions/runs/{run_id}/jobs?filter=latest", "jobs")


def _has_write(gh: "GitHub", login: str, known: dict[str, bool]) -> bool:
    """Whether ``login`` has write, maintain or admin permission. A login GitHub cannot answer for has none."""
    if login not in known:
        try:
            answer = gh.get(f"collaborators/{urllib.parse.quote(login)}/permission")
        except urllib.error.HTTPError:
            answer = None
        known[login] = isinstance(answer, dict) and answer.get("permission") in WRITE_PERMISSIONS
    return known[login]


def dispatched_records(gh: "GitHub", repo: Path, number: int, default_branch: str,
                       since: str) -> tuple[list[dict], bool]:
    """Records for pull request ``number`` written by a dispatch of the verdict workflow created since ``since``,
    and whether GitHub's list of those dispatches came back truncated.

    A record is a successful job named ``CI approval record v2 pr<N> <head> <digest>``. It is read only from a
    ``workflow_dispatch`` run of the verdict workflow that started from the default branch and ran the trusted
    checkout's own copy of the workflow (``default_definition``): any other copy can name a job anything. Every
    actor of the run, the dispatcher and whoever re-ran it, must have write access. GitHub lets a workflow's own
    token dispatch a workflow, and its actor is a bot with no collaborator permission, so a workflow cannot
    approve on a maintainer's behalf. The run name only selects whose jobs are read; the pull request number,
    the head and the digest are taken from the job name, which the trusted tool's outputs fill in. ``since`` is
    checked here as well as sent to GitHub, so the answer to the filter is not taken on trust.

    A truncated list (``list_cut``) can only hide a record, which leaves the head unapproved; the flag lets the
    verdict say why, so the approval is dispatched again instead of searched for.
    """
    query = urllib.parse.urlencode({"event": "workflow_dispatch", "branch": default_branch, "created": f">={since}"})
    workflow = urllib.parse.quote(VERDICT_WORKFLOW.rsplit("/", 1)[1])
    found: list[dict] = []
    permissions: dict[str, bool] = {}
    runs, total = gh.counted(f"actions/workflows/{workflow}/runs?{query}", "workflow_runs")
    for run in runs:
        title = _APPROVAL_RUN_TITLE.fullmatch(run.get("display_title") or "")
        if (title is None or int(title.group(1)) != number or run.get("path") != VERDICT_WORKFLOW
                or run.get("event") != "workflow_dispatch" or run.get("head_branch") != default_branch
                or (run.get("created_at") or "") < since
                or not default_definition(repo, run.get("head_sha") or "", VERDICT_WORKFLOW)):
            continue
        actors = {(run.get(role) or {}).get("login") for role in ("actor", "triggering_actor") if run.get(role)}
        if not actors or not all(login and _has_write(gh, login, permissions) for login in actors):
            continue
        for job in _jobs_of(gh)(int(run["id"])):
            match = _APPROVAL_RECORD_V2.fullmatch(job.get("name") or "")
            if (match and int(match.group(1)) == number and match.group(2) == title.group(2)
                    and job.get("status") == "completed" and job.get("conclusion") == "success"):
                found.append({"digest": match.group(3), "recorded_at": job.get("completed_at") or "",
                              "run_created_at": run.get("created_at") or "",
                              "route": "dispatch", "head_sha": match.group(2),
                              "actor": (run.get("triggering_actor") or run.get("actor") or {}).get("login")})
    return found, list_cut(runs, total)


# GitHub answers a filtered run list with at most this many runs, its newest.
RUN_LIST_LIMIT = 1000
# The horizon of a truncated list that returned nothing to date it by: later than any run.
UNBOUNDED = "9999-12-31T23:59:59Z"


def list_cut(runs: list, total: int | None) -> bool:
    """Whether a run list may be missing runs: no count, more runs than were returned, or a full list."""
    return total is None or total > len(runs) or len(runs) >= RUN_LIST_LIMIT


def named_runs(gh: "GitHub", repo: Path, default_branch: str, head_sha: str, since: str,
               producers: tuple[Producer, ...] = PRODUCERS) -> tuple[list[dict], dict[str, str]]:
    """Default-branch runs named for ``head_sha``, for every producer with a named route, and for each context
    with a truncated list its horizon: the time before which runs are missing.

    GitHub lists a ``workflow_run`` or ``workflow_dispatch`` run under the default branch, so the head's own run
    list cannot contain it. The workflow's default-branch runs created since ``since`` are listed, one list per
    event, and matched by run name. ``since`` is when the head's earliest run was created: a run is named for a
    head only once a pull request has it. Each match gets ``default_definition``, the git fact ``producer_runs``
    requires.

    A list is truncated when GitHub reports more runs than it returned (``RUN_LIST_LIMIT``; every ``CI`` run
    starts one, so at the 2026-11 rate a ``workflow_run`` attestation leaves the list after about five days, and
    a dispatched one, in its own far shorter list, practically never). The runs it lost are the ones created
    before the oldest run it returned; that time is the list's horizon, and a context's horizon is the latest
    among its truncated lists. A list that returned nothing to date it by has the horizon ``UNBOUNDED``.
    ``collect`` judges a run for the head only when it is newer than the horizon, so nothing that a lost run
    could have superseded stands in for it, whichever list or route it came from.
    """
    found: list[dict] = []
    horizons: dict[str, str] = {}
    for producer in producers:
        title = producer.run_title.format(head=head_sha) if producer.named_events else ""
        workflow = urllib.parse.quote(producer.workflow.rsplit("/", 1)[1])
        for event in producer.named_events:
            query = urllib.parse.urlencode({"event": event, "branch": default_branch, "created": f">={since}"})
            runs, total = gh.counted(f"actions/workflows/{workflow}/runs?{query}", "workflow_runs")
            if list_cut(runs, total):
                created = [run.get("created_at") or "" for run in runs]
                horizon = min(created) if created and all(created) else UNBOUNDED
                horizons[producer.context] = max(horizons.get(producer.context, ""), horizon)
            for run in runs:
                if run.get("event") == event and run.get("display_title") == title:
                    trusted = default_definition(repo, run.get("head_sha") or "", producer.workflow)
                    found.append(run | {"default_definition": trusted})
    return found, horizons


def behind_horizon(workflow_runs: list[dict], horizons: dict[str, str], head_sha: str,
                   default_branch: str, producers: tuple[Producer, ...] = PRODUCERS) -> list[str]:
    """The contexts whose run to be judged is not newer than its truncated list's horizon, each with the remedy.

    The run must have started after the horizon. One that started in the horizon's own second is refused as
    well: a lost run of that second with a higher id would be the newer one.
    """
    lost = []
    for producer in producers:
        horizon = horizons.get(producer.context)
        latest = latest_live_run(workflow_runs, producer, head_sha, default_branch)
        if horizon is not None and (latest is None or run_order(latest)[0] <= horizon):
            lost.append(f"{producer.context} (not among its workflow's newest {RUN_LIST_LIMIT} runs; "
                        "dispatch it again)")
    return lost


def unfinished_producers(workflow_runs: list[dict], head_sha: str, default_branch: str = "main",
                         producers: tuple[Producer, ...] = PRODUCERS) -> list[str]:
    """The intended contexts whose producer has no run at this head yet, or whose newest live run is unfinished.

    While any is listed the verdict cannot be success, so ``collect`` stops after reading the workflow runs and
    the head is reported pending. The full read costs a dozen or more API calls. It ran on every CI, OSV-Scanner
    and Dependency Review completion of every pull request, and that exhausted the repository's GITHUB_TOKEN
    budget: Trusted CodeQL's upload failed on the rate limit (2026-10-30). A run that finishes re-runs the
    verdict, and the last one to finish gets the full judgement. Only a pending status is ever posted early,
    and pending never satisfies the ruleset.
    """
    waiting = []
    for producer in producers:
        runs = producer_runs(workflow_runs, producer, head_sha, default_branch)
        live = [run for run in runs if run.get("conclusion") != "cancelled"]
        if not runs:
            waiting.append(producer.context)
        elif live and max(live, key=run_order)["status"] != "completed":
            waiting.append(producer.context)
    return waiting


def collect(
    gh: GitHub, repo: Path, default_branch: str, head_sha: str, number: int | None,
    producers: tuple[Producer, ...] = PRODUCERS,
) -> dict | None:
    """Read everything the rules need. ``None`` when no pull request into the default branch has this head.

    While a producer is unfinished (``unfinished_producers``) only the workflow runs are read, and the result
    holds ``waiting_producers`` in place of the rest.
    """
    open_prs = gh.paginate("pulls?state=open")
    if number is None:
        matches = [pr for pr in open_prs if pr["head"]["sha"] == head_sha
                   and pr["base"]["ref"] == default_branch]
        if not matches:
            return None
        number = min(int(pr["number"]) for pr in matches)
    workflow_runs = gh.paginate(f"actions/runs?head_sha={head_sha}", "workflow_runs")
    waiting = unfinished_producers(workflow_runs, head_sha, default_branch, producers)
    # A named route is listed only when it can change the answer: while a producer without one is unfinished the
    # head is pending whatever the list says, and the list is one more API call (several for an old head).
    named_contexts = {producer.context for producer in producers if producer.named_events}
    if not set(waiting) <= named_contexts:
        return {"head_sha": head_sha, "waiting_producers": waiting}
    since = min((run["created_at"] for run in workflow_runs if run.get("created_at")), default=None)
    horizons: dict[str, str] = {}
    if since:
        named, horizons = named_runs(gh, repo, default_branch, head_sha, since, producers)
        workflow_runs = workflow_runs + named
        # A truncated list has lost every run created before its horizon, and one of them may be this head's
        # newest attestation and a failure. The run that would be judged must therefore be newer than the
        # horizon: started at or after it, so nothing lost could have come later. An older one, from either
        # route or from the other event's complete list, is not judged in its place; the head waits until it
        # is attested again, and that run is newer than any horizon. (A lost run that someone re-runs is the
        # one case this cannot see.) The check runs again below, once the jobs are read: a run found to be
        # jobless is dropped, and the run judged in its place has to pass the horizon too.
        lost = behind_horizon(workflow_runs, horizons, head_sha, default_branch, producers)
        if lost:
            return {"head_sha": head_sha, "waiting_producers": lost}
        waiting = unfinished_producers(workflow_runs, head_sha, default_branch, producers)
    if waiting:
        return {"head_sha": head_sha, "waiting_producers": waiting}
    pr = gh.get(f"pulls/{number}")
    assert isinstance(pr, dict)
    labelled = any(label.get("name") == APPROVAL_LABEL for label in pr.get("labels") or [])
    raw: dict = {
        "head_sha": head_sha,
        "default_branch": default_branch,
        "pull_request": pr,
        "open_pull_requests": open_prs,
        # The events only matter when the label is on the pull request: without it nothing is approved.
        "label_events": gh.paginate(f"issues/{number}/events") if labelled else [],
        "workflow_runs": workflow_runs,
        "check_runs": gh.paginate(f"commits/{head_sha}/check-runs?filter=all", "check_runs"),
        "statuses": gh.paginate(f"commits/{head_sha}/statuses"),
    }
    events = [event for event in raw["label_events"]
              if event.get("event") in ("labeled", "unlabeled")
              and (event.get("label") or {}).get("name") == APPROVAL_LABEL]
    last_label: dict = max(
        events, key=lambda event: (event.get("created_at") or "", event.get("id") or 0), default={},
    )
    if events:
        actor = (last_label.get("actor") or {}).get("login")
        if actor:
            try:
                answer = gh.get(f"collaborators/{urllib.parse.quote(actor)}/permission")
                raw["label_actor_permission"] = (answer or {}).get("permission")  # type: ignore[union-attr]
            except urllib.error.HTTPError:
                raw["label_actor_permission"] = None
    # The rules read jobs only from each producer's newest live run at this head (``_check_producer``), so only
    # those runs' jobs are fetched: one call per run, and superseded or cancelled runs used to cost one each.
    jobs: dict[str, list] = {}
    for producer in producers:
        while (run := latest_live_run(workflow_runs, producer, head_sha, default_branch)) is not None:
            key = str(run["id"])
            if key not in jobs:
                jobs[key] = gh.paginate(f"actions/runs/{run['id']}/jobs?filter=latest", "jobs")
            # A head-attached run that completed without any job never ran the gate (``producer_runs``). It
            # is marked, and when that lets a named run take its place, that run's jobs are read instead.
            if (jobs[key] or run.get("status") != "completed" or run.get("event") not in producer.events
                    or not producer.named_events or run.get("jobless") is True):
                break
            run["jobless"] = True
    lost = behind_horizon(workflow_runs, horizons, head_sha, default_branch, producers)
    if lost:
        return {"head_sha": head_sha, "waiting_producers": lost}
    raw["jobs"] = jobs
    facts = git_facts(repo, gh.repository, number, head_sha)
    raw |= facts
    # An approval is judged only when a label is on and some protected path changed.
    if labelled and any(change["kind"] != "behind" for change in facts["protected_changes"]):
        try:
            raw["head_digest"] = approval_digest(repo, head_sha)
            records = head_records(workflow_runs, _jobs_of(gh), head_sha, number, default_branch)
            # A dispatched record counts only when written after the label, so only runs since then are listed.
            if last_label.get("event") == "labeled" and last_label.get("created_at"):
                dispatched, cut = dispatched_records(gh, repo, number, default_branch, last_label["created_at"])
                records += dispatched
                raw["approval_list_truncated"] = cut
            raw["approval_records"] = records
        except (subprocess.CalledProcessError, urllib.error.URLError, KeyError, ValueError) as error:
            print(f"approval records unavailable ({type(error).__name__}); this head is not approved",
                  file=sys.stderr)
    return raw


def latest_live_run(workflow_runs: list[dict], producer: Producer, head_sha: str,
                    default_branch: str = "main") -> dict | None:
    """The run whose jobs judge ``producer``: its newest run for this head that was not cancelled, chosen as the
    rules choose it (``producer_runs``, then ``run_order``)."""
    live = [run for run in producer_runs(workflow_runs, producer, head_sha, default_branch)
            if run.get("conclusion") != "cancelled"]
    return max(live, key=run_order) if live else None


def summary_markdown(verdict: Verdict, snapshot: dict) -> str:
    pr = snapshot["pull_request"] or {}
    lines = [
        f"### {VERDICT_CONTEXT}: {verdict.state}",
        "",
        f"Head `{snapshot['head_sha']}` of #{pr.get('number', '?')}, judged with the default branch at "
        f"`{snapshot.get('trusted_sha') or 'unknown'}`.",
        "",
    ]
    if verdict.pending:
        lines += ["Waiting for: " + ", ".join(f"`{name}`" for name in verdict.pending), ""]
    if verdict.findings:
        lines += ["| Code | Subject | Detail |", "| --- | --- | --- |"]
        lines += [
            f"| {'' if item.fatal else '(info) '}`{item.code}` | `{item.subject}` | {item.message} |"
            for item in verdict.findings
        ]
    else:
        lines.append("No findings.")
    return "\n".join(lines) + "\n"


# --- Recording and carrying an approval (the label workflow's two jobs) ----


class ApprovalRefused(Exception):
    """The approval cannot be recorded or carried; the caller leaves the label to be withdrawn."""


def _open_pull_request(gh: GitHub, number: int, head_sha: str, default_branch: str) -> dict:
    pr = gh.get(f"pulls/{number}")
    assert isinstance(pr, dict)
    if pr.get("state") != "open" or (pr.get("base") or {}).get("ref") != default_branch:
        raise ApprovalRefused(f"#{number} is not an open pull request into {default_branch}")
    if (pr.get("head") or {}).get("sha") != head_sha:
        raise ApprovalRefused(f"#{number} no longer has head {head_sha}")
    return pr


def label_state(gh: GitHub, number: int, pr: dict) -> dict:
    """The label's latest event when it is a ``labeled`` by a user with write access; refuse otherwise.

    ``actor`` and ``created_at`` are that event's. ``retargeted`` says whether the pull request ever changed its
    base: a ``pull_request_target`` run takes its workflow from the base branch, so such a pull request's
    label-event records are not trusted, while a dispatched record comes from the default branch whatever the
    base was.
    """
    if APPROVAL_LABEL not in {label.get("name") for label in pr.get("labels") or []}:
        raise ApprovalRefused(f"{APPROVAL_LABEL!r} is not on #{number}")
    all_events = gh.paginate(f"issues/{number}/events")
    events = [event for event in all_events
              if event.get("event") in ("labeled", "unlabeled")
              and (event.get("label") or {}).get("name") == APPROVAL_LABEL]
    if not events:
        raise ApprovalRefused("no label event")
    last = max(events, key=lambda event: (event.get("created_at") or "", event.get("id") or 0))
    actor = (last.get("actor") or {}).get("login")
    if last["event"] != "labeled" or not actor:
        raise ApprovalRefused(f"the latest {APPROVAL_LABEL!r} event is not a labeling")
    answer = gh.get(f"collaborators/{urllib.parse.quote(actor)}/permission")
    permission = answer.get("permission") if isinstance(answer, dict) else None
    if permission not in WRITE_PERMISSIONS:
        raise ApprovalRefused(f"{actor!r} has permission {permission!r}, not write")
    return {"actor": actor, "created_at": last.get("created_at") or "",
            "retargeted": any(event.get("event") == "base_ref_changed" for event in all_events),
            "moved": [(event["event"], event.get("created_at") or "") for event in all_events
                      if event.get("event") in HEAD_MOVED_EVENTS + BASE_MOVED_EVENTS]}


# Events of a pull request after which what it proposes may differ from what a labeller saw. A force push and a
# restored branch move the head. A re-open does too: GitHub leaves a closed pull request's head where it was,
# and commits pushed meanwhile arrive when it is re-opened. A retarget changes what the head is compared with.
HEAD_MOVED_EVENTS = ("head_ref_force_pushed", "head_ref_restored", "reopened")
BASE_MOVED_EVENTS = ("base_ref_changed",)
_REPOSITORY_NAME = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")
_REFUSAL_CODE = re.compile(r"([A-Z][A-Z_]+): ")


def head_pushed_at(gh: GitHub, pr: dict, head_sha: str) -> datetime:
    """When ``head_sha`` became the tip of the pull request's head branch: the newest record of that branch in
    GitHub's repository activity, which must be a record of this commit.

    The activity list holds the updates of a branch (push, force push, creation) with the commit before and
    after and the time. It dates the *latest* arrival of a commit: a pull request can be moved back to a commit
    it had before, and then every run and every check of that commit predates the moment it became the head
    again. The earliest run at the commit, which the verifier once used, says when the commit first arrived, and
    a label applied in between passed for a head nobody had read. Nothing here is taken from the candidate: the
    branch name only selects the list, as a query value.

    The list is not complete: it can lag a push and it has been seen to lack a record. So a record of the commit
    somewhere in it is not enough. The branch's newest record must be this commit's, which is true of a complete
    list whenever the commit is the tip; when another commit's record is newer, the list does not yet show, or
    has lost, the update that made this one the tip, and its older arrival proves nothing
    (``HEAD_NOT_NEWEST_PUSH``). Records that share the newest time must all be this commit's.

    Refused as ``HEAD_TIME_UNREADABLE`` when the head repository or branch is not named (a deleted fork), the
    list cannot be read (a fork this token may not read), it has no record of the branch, or a record has no
    readable time.
    """
    head = pr.get("head") or {}
    repository = (head.get("repo") or {}).get("full_name") or ""
    branch = head.get("ref") or ""
    if not _REPOSITORY_NAME.fullmatch(repository) or not branch:
        raise ApprovalRefused("HEAD_TIME_UNREADABLE: the pull request's head repository or branch is not named")
    ref = f"refs/heads/{branch}"
    try:
        records = gh.paginate(f"{API_ROOT}/repos/{repository}/activity?" + urllib.parse.urlencode({"ref": ref}))
    except (urllib.error.URLError, TimeoutError, KeyError, TypeError, ValueError) as error:
        status = f"HTTP {error.code}" if isinstance(error, urllib.error.HTTPError) else type(error).__name__
        raise ApprovalRefused(f"HEAD_TIME_UNREADABLE: the activity of {repository} {ref} cannot be read "
                              f"({status})") from error
    dated = [(_when(record.get("timestamp")), record.get("after")) for record in records
             if isinstance(record, dict) and record.get("ref") == ref]
    if not dated or any(stamp is None for stamp, _after in dated):
        raise ApprovalRefused(f"HEAD_TIME_UNREADABLE: {repository} {ref} has no dated push record")
    newest = max(stamp for stamp, _after in dated if stamp is not None)
    if any(after != head_sha for stamp, after in dated if stamp == newest):
        raise ApprovalRefused(
            f"HEAD_NOT_NEWEST_PUSH: the newest push record of {repository} {ref} is not for {head_sha}, so the "
            "list does not show when this commit became the head; push the branch again, then label and dispatch")
    return newest


def label_covers_head(gh: GitHub, number: int, pr: dict, head_sha: str) -> None:
    """Refuse a dispatched approval unless the current label was applied after what it approves was last changed.

    The label names no commit. While every push ran the handler that removes it, "the label is on" meant "this
    head was read"; once GitHub refuses ``pull_request_target`` nothing removes it, and a label applied for one
    head is still there at the next. A dispatch for that next head would then record bytes nobody read. So the
    label's latest ``labeled`` event must be later than the push that made this commit the tip of the head
    branch (``head_pushed_at``), and later than every event of the pull request that can change its head or its
    base without such a push (``HEAD_MOVED_EVENTS``, ``BASE_MOVED_EVENTS``): the branch's log dates the branch,
    and these date the pull request. An event in the label's own second cannot be ordered and is refused. The
    remedy is always the same: remove the label, read the head, apply the label again.
    """
    state = label_state(gh, number, pr)
    labelled = _when(state["created_at"])
    pushed = head_pushed_at(gh, pr, head_sha)
    changes = [("LABEL_PREDATES_HEAD", f"{head_sha} became the head of #{number}", pushed)]
    for event, stamp in state["moved"]:
        moved = _when(stamp)
        if moved is None:
            raise ApprovalRefused(f"HEAD_TIME_UNREADABLE: the {event} event of #{number} has no readable time")
        changes.append(("LABEL_PREDATES_BASE" if event in BASE_MOVED_EVENTS else "LABEL_PREDATES_HEAD",
                        f"the {event} event of #{number}", moved))
    if labelled is None:
        raise ApprovalRefused(f"HEAD_TIME_UNREADABLE: the label event of #{number} has no readable time")
    code, what, newest = max(changes, key=lambda change: change[2])
    if labelled <= newest:
        raise ApprovalRefused(
            f"{code}: {APPROVAL_LABEL!r} was applied at {state['created_at']}, not after {what} "
            f"({newest.isoformat()}); remove the label, read this head, apply the label again, then dispatch "
            "again")


def label_approver(gh: GitHub, number: int, pr: dict, *, refuse_retargeted: bool = True) -> str:
    """The write-access user who applied the current label; refuse otherwise, and by default refuse a pull request
    that was ever retargeted (``label_state``)."""
    state = label_state(gh, number, pr)
    if refuse_retargeted and state["retargeted"]:
        raise ApprovalRefused(f"#{number} was retargeted; its earlier runs may have used another base's workflow")
    actor: str = state["actor"]
    return actor


def candidate_digest(gh: GitHub, repo: Path, number: int, head_sha: str, remote: str | None = None) -> str:
    """Digest of the protected content of ``head_sha``, which must still be what the pull request ref points at."""
    fetched = fetch_candidate(repo, gh.repository, number, remote)
    if fetched != head_sha:
        raise ApprovalRefused(f"refs/pull/{number}/head is {fetched}, not {head_sha}")
    return approval_digest(repo, head_sha)


def record_approval(gh: GitHub, repo: Path, number: int, head_sha: str, default_branch: str,
                    remote: str | None = None, *, dispatched: bool = False) -> str:
    """The digest to record for ``head_sha``, or ``ApprovalRefused``.

    On a ``labeled`` event ``head_sha`` is the head the event named. For a dispatch (``dispatched``) it is the head
    the dispatcher reviewed and typed: a pull request whose head has moved since is refused, so nobody approves
    bytes they did not name, and so is a label that was applied before that head arrived
    (``label_covers_head``), so nobody approves bytes under a label that was given for others.
    """
    pr = _open_pull_request(gh, number, head_sha, default_branch)
    if dispatched:
        label_covers_head(gh, number, pr, head_sha)
    else:
        label_approver(gh, number, pr)
    digest = candidate_digest(gh, repo, number, head_sha, remote)
    # The head must not have moved while it was hashed, or the digest would describe a head nobody approved.
    label_approver(gh, number, _open_pull_request(gh, number, head_sha, default_branch),
                   refuse_retargeted=not dispatched)
    return digest


def carry_approval(gh: GitHub, repo: Path, number: int, before_sha: str, head_sha: str, default_branch: str,
                   remote: str | None = None) -> str:
    """On a push: the digest to record at the new head, only if the current label already approved it.

    That is a digest recorded at the previous head, or by a dispatch for this pull request, in both cases under
    the label's latest ``labeled`` event (``under_label``). A record from before that event is not carried: the
    label it was written for was removed, and the one on the pull request now was applied for other bytes.
    """
    if before_sha == head_sha:
        raise ApprovalRefused("the push did not move the head")
    label = label_state(gh, number, _open_pull_request(gh, number, head_sha, default_branch))
    records: list[dict] = []
    if not label["retargeted"]:
        records += head_records(gh.paginate(f"actions/runs?head_sha={before_sha}", "workflow_runs"), _jobs_of(gh),
                                before_sha, number, default_branch)
    if label["created_at"]:
        dispatched, cut = dispatched_records(gh, repo, number, default_branch, label["created_at"])
        records += dispatched
    else:
        cut = False
    recorded = {record["digest"] for record in records if under_label(record, _when(label["created_at"]))}
    if not recorded:
        raise ApprovalRefused(f"the current label has no approval record at the previous head {before_sha} "
                              "and none by dispatch"
                              + (" (GitHub's list of approval dispatches since the label is incomplete)" if cut else ""))
    digest = candidate_digest(gh, repo, number, head_sha, remote)
    if digest not in recorded:
        raise ApprovalRefused("protected CI content differs from the approved head's")
    label_state(gh, number, _open_pull_request(gh, number, head_sha, default_branch))
    return digest


def approval_main(args: argparse.Namespace, gh: GitHub) -> int:
    for name in ("head_sha", "before_sha") if args.approval == "carry" else ("head_sha",):
        if not _SHA.fullmatch(getattr(args, name) or ""):
            print(f"--{name.replace('_', '-')} must be 40 lowercase hex digits", file=sys.stderr)
            return 2
    if args.pull_request is None:
        print("--pull-request is required", file=sys.stderr)
        return 2
    done = {"record": "recorded", "carry": "carried", "dispatch": "recorded by dispatch"}[args.approval]
    try:
        if args.approval == "carry":
            digest = carry_approval(gh, args.repo_dir, args.pull_request, args.before_sha, args.head_sha,
                                    args.default_branch)
        else:
            digest = record_approval(gh, args.repo_dir, args.pull_request, args.head_sha, args.default_branch,
                                     dispatched=args.approval == "dispatch")
    except Exception as error:  # Fail closed: any error, an API failure included, records and carries nothing.
        print(f"approval not {done}: {type(error).__name__}: {error}", file=sys.stderr)
        code = _REFUSAL_CODE.match(str(error)) if isinstance(error, ApprovalRefused) else None
        if code:
            # Only the code is shown on the run's page. The text stays in the log: it can name the head branch,
            # which is the pull request author's text and has no place in a workflow command.
            print(f"::error::approval not {done}: {code.group(1)}")
        print(gh.budget_note())
        return 1
    print(f"approval {done} at {args.head_sha}: protected content digest {digest}")
    print(gh.budget_note())
    output = args.github_output or os.environ.get("GITHUB_OUTPUT")
    if output:
        with open(output, "a", encoding="utf-8") as handle:
            handle.write(f"digest={digest}\n")
            if args.approval == "dispatch":
                # The record job's whole name comes from these: the number as an integer and the SHA as checked
                # above, never the dispatch inputs as typed.
                handle.write(f"number={args.pull_request}\nhead_sha={args.head_sha}\n")
    return 0


class TargetRefused(Exception):
    """No single open pull request into the default branch has the named head."""


def attestation_target(gh: GitHub, default_branch: str, head_sha: str, number: int | None = None) -> dict:
    """The one open pull request into the default branch whose head is ``head_sha``.

    For the provenance attestation's ``workflow_run`` and dispatch routes, whose event names no pull request. A
    ``workflow_run`` gives only the head its CI run was requested for; a dispatch also names the pull request,
    which must be the match. Exactly one pull request must match: none means the head moved or the pull request
    targets another branch, and two pull requests sharing a head can have different bases. The caller fails on
    ``TargetRefused``, because a run named for a head must never end green without attesting it.
    """
    matches = [pr for pr in gh.paginate(f"pulls?state=open&base={urllib.parse.quote(default_branch, safe='')}")
               if (pr.get("head") or {}).get("sha") == head_sha
               and (pr.get("base") or {}).get("ref") == default_branch]
    if len(matches) != 1:
        raise TargetRefused(f"{len(matches)} open pull requests into {default_branch} have head {head_sha}; "
                            "exactly one is needed")
    found = int(matches[0]["number"])
    if number is not None and found != number:
        raise TargetRefused(f"head {head_sha} belongs to #{found}, not #{number}")
    base_sha = (matches[0].get("base") or {}).get("sha") or ""
    if not _SHA.fullmatch(base_sha):
        raise TargetRefused(f"#{found} has no exact base SHA")
    return {"number": found, "head_sha": head_sha, "base_sha": base_sha}


def target_main(args: argparse.Namespace, gh: GitHub) -> int:
    if not _SHA.fullmatch(args.head_sha or ""):
        print("--head-sha must be 40 lowercase hex digits", file=sys.stderr)
        return 2
    try:
        target = attestation_target(gh, args.default_branch, args.head_sha, args.pull_request)
    except Exception as error:  # Fail closed: any error, an API failure included, resolves nothing.
        print(f"no attestation target: {type(error).__name__}: {error}", file=sys.stderr)
        return 1
    print(f"attesting #{target['number']} at {target['head_sha']} (event base {target['base_sha']}; "
          "the verdict base is the head's merge base with the default branch)")
    output = args.github_output or os.environ.get("GITHUB_OUTPUT")
    if output:
        with open(output, "a", encoding="utf-8") as handle:
            handle.write("".join(f"{name}={value}\n" for name, value in target.items()))
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--repo-dir", type=Path, default=Path("."),
                        help="the trusted default-branch checkout (full history)")
    parser.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY"))
    parser.add_argument("--default-branch", default="main")
    parser.add_argument("--head-sha", help="the head to judge (workflow_run, pull_request_target)")
    parser.add_argument("--pull-request", type=int, help="the pull request (pull_request_target, dispatch)")
    parser.add_argument("--post", action="store_true", help="post the verdict as a commit status")
    parser.add_argument("--target-url", default="", help="link for the status (this run)")
    parser.add_argument("--summary", type=Path, help="append a Markdown report here")
    parser.add_argument("--snapshot", type=Path, help="evaluate a saved raw snapshot offline instead")
    parser.add_argument("--snapshot-out", type=Path, help="save the raw snapshot read from GitHub")
    parser.add_argument("--approval", choices=("record", "carry", "dispatch"),
                        help="write the digest to record for --head-sha: on a label event (record), for the head a "
                             "dispatch names (dispatch), or carried from --before-sha on a push (carry); exit 1 "
                             "records nothing")
    parser.add_argument("--before-sha", help="the previous head, for --approval carry")
    parser.add_argument("--attestation-target", action="store_true",
                        help="provenance attestation: write the number and base SHA of the one open pull request "
                             "into the default branch whose head is --head-sha (and is --pull-request, if given)")
    parser.add_argument("--github-output", type=Path,
                        help="file receiving the outputs of --approval or --attestation-target (default "
                             "$GITHUB_OUTPUT)")
    args = parser.parse_args(argv)
    if args.snapshot and args.post:
        parser.error("--post judges live GitHub state; it cannot be combined with --snapshot")
    if args.approval and args.attestation_target:
        parser.error("--approval and --attestation-target are separate commands")

    if args.snapshot:
        snapshot = normalise(json.loads(args.snapshot.read_text(encoding="utf-8")))
        verdict = evaluate(snapshot)
        _report(verdict, snapshot, args.summary)
        return 1 if verdict.state == "failure" else 0

    token = os.environ.get("GITHUB_TOKEN", "")
    if not token or not args.repository:
        parser.error("GITHUB_TOKEN and --repository are required unless --snapshot is given")
    gh = GitHub(args.repository, token, os.environ.get("VERDICT_READ_TOKEN") or None)
    if args.approval:
        return approval_main(args, gh)
    if args.attestation_target:
        return target_main(args, gh)
    head = args.head_sha
    if head is None:
        if args.pull_request is None:
            parser.error("give --head-sha or --pull-request")
        head = gh.get(f"pulls/{args.pull_request}")["head"]["sha"]  # type: ignore[index]
    if not _SHA.fullmatch(head or ""):
        parser.error("head SHA must be 40 lowercase hex digits")

    def post(state: str, description: str) -> None:
        if args.post:
            gh.post(f"statuses/{head}", {
                "state": state,
                "context": VERDICT_CONTEXT,
                "description": description,
                **({"target_url": args.target_url} if args.target_url else {}),
            })

    identity = os.environ.get("GITHUB_REPOSITORY_ID")
    judged = (f"Provenance producer set: {provenance_mode.mode(args.repository, identity)} "
              f"(repository {args.repository}, ID {identity or 'unknown'})")
    print(judged)
    try:
        producers = producers_for(args.repository, identity)
        raw = collect(gh, args.repo_dir, args.default_branch, head, args.pull_request, producers)
        if raw is None:
            print(f"no open pull request into {args.default_branch} has head {head}; no verdict")
            print(gh.budget_note())
            return 0
        if raw.get("waiting_producers"):
            description = Verdict("pending", pending=raw["waiting_producers"]).description()
            print(f"{description}; the full judgement runs when the last of them finishes")
            post("pending", description)
            print(gh.budget_note())
            return 0
        if args.snapshot_out:
            args.snapshot_out.write_text(json.dumps(raw, indent=1, sort_keys=True), encoding="utf-8")
        snapshot = normalise(raw)
        verdict = evaluate(snapshot, producers)
    except Exception as error:
        # An earlier verdict for this head may still say success. Replace it:
        # a head that cannot be judged now is not a passing head.
        post("error", f"verifier could not judge this head: {type(error).__name__}")
        print(gh.budget_note())
        raise
    _report(verdict, snapshot, args.summary, judged + fork_run_note(snapshot, producers, args.repository))
    post(verdict.state, status_description(verdict, producers))
    print(gh.budget_note())
    return 1 if verdict.state == "failure" else 0


def _report(verdict: Verdict, snapshot: dict, summary: Path | None, judged: str | None = None) -> None:
    report = summary_markdown(verdict, snapshot)
    if judged:
        report += f"\n{judged}\n"
    print(report)
    if summary:
        with summary.open("a", encoding="utf-8") as handle:
            handle.write(report)


if __name__ == "__main__":
    sys.exit(main())
