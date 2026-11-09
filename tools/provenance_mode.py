#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Decide whether a provenance run is the upstream's or a fork's, and run the fork check.

The upstream repository, ``Jstar269/Nakagawa-Recomp``, keeps its **Trusted provenance
attestation** exactly as it is: every pull request is verified against the private trusted
ledger. A fork has no access to that ledger, so on a fork the same workflow runs the **Fork
provenance check** instead (``docs/FORKING.md``). This module is the one place that tells the
two apart, and the workflow's job conditions are built from the same constants
(``tools/test_provenance_mode.py`` holds them equal).

A repository is the upstream when its numeric ID is the upstream's **or** its name is the
upstream's. The ID survives a rename or a transfer to an organisation, so neither can turn
fork mode on there; the name keeps the upstream strict even if a run could not see the ID.
No variable, secret, input or file in a pull request takes part in the decision, so fork mode
cannot be switched on for the upstream at all.

The fork check reads the candidate as data from the trusted default branch's checkout, as the
upstream job does, and never contacts the private authority:

* the candidate's workflows may not answer to either gate's check name;
* the trusted checkout's publication audit runs on the candidate's exact tree with
  ``--public-scope --provenance-self-consistency``: game data, keys, private paths, user
  paths, unclassified paths and an inconsistent ledger are refused exactly as on the upstream;
* the paths the candidate adds to or removes from the publication profile are listed, because
  on a fork the fork maintainer's admission and review are the provenance authority.

Usage::

    python tools/provenance_mode.py --require fork          # exit 1 unless this run is a fork's
    python tools/provenance_mode.py --require upstream      # exit 1 unless it is the upstream's
    python tools/provenance_mode.py fork-check --base <sha> --candidate <sha> [--summary <file>]

``--repository`` and ``--repository-id`` default to ``GITHUB_REPOSITORY`` and
``GITHUB_REPOSITORY_ID``, which GitHub Actions sets for every run.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent

#: The upstream repository. GitHub compares owner and repository names without regard to case.
UPSTREAM_REPOSITORY = "Jstar269/Nakagawa-Recomp"
#: Its numeric ID (``gh api repos/Jstar269/Nakagawa-Recomp --jq .id``), stable across renames and
#: transfers.
UPSTREAM_REPOSITORY_ID = "1330183186"

UPSTREAM_CONTEXT = "Trusted provenance attestation"
FORK_CONTEXT = "Fork provenance check"
WORKFLOW = ".github/workflows/provenance-attestation.yml"
WORKFLOW_PREFIX = ".github/workflows/"
POLICY = "assets/public_source_profile.json"

#: The job conditions in ``WORKFLOW``. GitHub's ``==`` ignores letter case for strings, as
#: ``is_upstream`` does. They are exact complements, so exactly one of the two jobs runs.
UPSTREAM_CONDITION = (
    f"(github.repository_id == '{UPSTREAM_REPOSITORY_ID}' || github.repository == '{UPSTREAM_REPOSITORY}')"
)
FORK_CONDITION = (
    f"(github.repository_id != '{UPSTREAM_REPOSITORY_ID}' && github.repository != '{UPSTREAM_REPOSITORY}')"
)


def is_upstream(repository: str | None, repository_id: str | int | None) -> bool:
    """Whether a run belongs to the upstream repository: its ID or its name matches.

    The name comparison folds ASCII case only, as GitHub does. A name with any non-ASCII
    character is never the upstream's name; GitHub names are ASCII, and Unicode folding would
    let a lookalike match.
    """
    if repository_id is not None and str(repository_id).strip() == UPSTREAM_REPOSITORY_ID:
        return True
    return (
        isinstance(repository, str)
        and repository.isascii()
        and repository.strip().lower() == UPSTREAM_REPOSITORY.lower()
    )


def mode(repository: str | None, repository_id: str | int | None) -> str:
    """``"upstream"`` or ``"fork"``."""
    return "upstream" if is_upstream(repository, repository_id) else "fork"


class CheckError(RuntimeError):
    """A fork-check input could not be read."""


def _git(repo: Path, *args: str) -> bytes:
    result = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, check=False)
    if result.returncode:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise CheckError(detail or f"git {' '.join(args)} failed")
    return result.stdout


def _commit(repo: Path, rev: str) -> str:
    sha = _git(repo, "rev-parse", "--verify", "--quiet", f"{rev}^{{commit}}").decode("ascii").strip()
    if len(sha) != 40:
        raise CheckError(f"{rev} is not a commit")
    return sha


def _workflow_blobs(repo: Path, rev: str) -> dict[str, bytes]:
    names = _git(repo, "ls-tree", "-r", "-z", "--name-only", rev, "--", WORKFLOW_PREFIX).split(b"\0")
    blobs = {}
    for raw in names:
        if raw:
            path = raw.decode("utf-8", errors="surrogateescape")
            blobs[path] = _git(repo, "cat-file", "blob", f"{rev}:{path}")
    return blobs


def context_findings(workflows: dict[str, bytes]) -> list[str]:
    """Candidate workflows, other than the gate's own, that name either gate's check.

    A second job under the check's name would give a candidate-controlled result under the
    context a fork maintainer reads, whatever the real check concluded.
    """
    findings = []
    for path, raw in sorted(workflows.items()):
        if path == WORKFLOW:
            continue
        for context in (UPSTREAM_CONTEXT, FORK_CONTEXT):
            if context.encode("utf-8") in raw:
                findings.append(f"CI_CONTEXT_COLLISION: {path}: names the reserved check context {context!r}")
    return findings


def _profile_paths(repo: Path, rev: str) -> set[str]:
    try:
        document = json.loads(_git(repo, "cat-file", "blob", f"{rev}:{POLICY}"))
    except (CheckError, json.JSONDecodeError) as error:
        raise CheckError(f"{POLICY} at {rev} is unreadable: {error}") from error
    paths = document.get("include_paths")
    if not isinstance(paths, list) or not all(isinstance(path, str) for path in paths):
        raise CheckError(f"{POLICY} at {rev} has no include_paths list")
    return set(paths)


def profile_changes(repo: Path, base: str, candidate: str) -> tuple[list[str], list[str], bool]:
    """Added and removed ``include_paths``, and whether anything else in the profile changed."""
    before = _profile_paths(repo, base)
    after = _profile_paths(repo, candidate)
    base_document = json.loads(_git(repo, "cat-file", "blob", f"{base}:{POLICY}"))
    candidate_document = json.loads(_git(repo, "cat-file", "blob", f"{candidate}:{POLICY}"))
    base_document.pop("include_paths", None)
    candidate_document.pop("include_paths", None)
    return sorted(after - before), sorted(before - after), base_document != candidate_document


def fork_check(repo: Path, base: str, candidate: str, *, audit_tool: Path | None = None) -> tuple[int, list[str]]:
    """Run the fork provenance check. Returns the exit code and the report lines."""
    base_sha = _commit(repo, base)
    candidate_sha = _commit(repo, candidate)
    lines = [f"base {base_sha}", f"candidate {candidate_sha}"]
    failed = False

    collisions = context_findings(_workflow_blobs(repo, candidate_sha))
    lines.extend(collisions)
    failed |= bool(collisions)

    added, removed, rules_changed = profile_changes(repo, base_sha, candidate_sha)
    lines.append(f"profile: {len(added)} path(s) added, {len(removed)} removed")
    lines.extend(f"  added {path}" for path in added)
    lines.extend(f"  removed {path}" for path in removed)
    if rules_changed:
        lines.append("profile: rules or exclusions changed (review the diff of assets/public_source_profile.json)")

    tool = audit_tool or (ROOT / "tools" / "publish_audit.py")
    audit = subprocess.run(
        [sys.executable, str(tool), "--repo-root", str(repo), "--committed-tree", candidate_sha,
         "--public-scope", "--provenance-self-consistency"],
        capture_output=True, text=True, encoding="utf-8", errors="replace", check=False,
    )
    output = (audit.stdout + audit.stderr).strip().splitlines()
    lines.append(f"publication audit (trusted checkout, candidate tree): exit {audit.returncode}")
    lines.extend("  " + line for line in output[-60:])
    failed |= audit.returncode != 0

    lines.append(f"{FORK_CONTEXT}: {'FAIL' if failed else 'PASS'}")
    return (1 if failed else 0), lines


def _summary(lines: list[str], code: int) -> str:
    verdict = "passed" if code == 0 else "failed"
    return "\n".join([
        f"### {FORK_CONTEXT}: {verdict}",
        "",
        "This repository is not the upstream, so it has no access to the upstream's private trusted",
        "ledger. This check ran the trusted default branch's publication audit on the candidate's exact",
        "tree and checked the public ledger's self-consistency. The fork maintainer's admission",
        "(`tools/fork_init.py`, `tools/admit_paths.py`) and review are this fork's provenance authority;",
        "see docs/FORKING.md.",
        "",
        "```text",
        *lines,
        "```",
        "",
    ])


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", nargs="?", choices=("mode", "fork-check"), default="mode")
    parser.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY"))
    parser.add_argument("--repository-id", default=os.environ.get("GITHUB_REPOSITORY_ID"))
    parser.add_argument("--require", choices=("upstream", "fork"),
                        help="exit 1 unless the run is in this mode")
    parser.add_argument("--repo", type=Path, default=ROOT, help="repository holding both commits")
    parser.add_argument("--base", help="fork-check: exact base commit")
    parser.add_argument("--candidate", help="fork-check: exact candidate commit, read as data")
    parser.add_argument("--summary", type=Path, help="fork-check: append a Markdown summary here")
    args = parser.parse_args(argv)

    current = mode(args.repository, args.repository_id)
    if args.require and current != args.require:
        print(f"provenance mode is {current!r} for {args.repository!r} (id {args.repository_id!r}), "
              f"not {args.require!r}; refusing", file=sys.stderr)
        return 1
    if args.command == "mode":
        print(current)
        return 0

    if current != "fork":
        print(f"{UPSTREAM_REPOSITORY} never runs the fork check; it requires the trusted attestation",
              file=sys.stderr)
        return 1
    if not args.base or not args.candidate:
        parser.error("fork-check needs --base and --candidate")
    try:
        code, lines = fork_check(args.repo.resolve(), args.base, args.candidate)
    except CheckError as error:
        print(f"{FORK_CONTEXT}: input error: {error}", file=sys.stderr)
        return 2
    print("\n".join(lines))
    if args.summary:
        with args.summary.open("a", encoding="utf-8") as handle:
            handle.write(_summary(lines, code))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
