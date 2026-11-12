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
A run whose ID is missing, empty or not numeric also gets the strict path: fork mode needs a
known ID that differs from the upstream's.
No variable, secret, input or file in a pull request takes part in the decision, so fork mode
cannot be switched on for the upstream at all.

The fork check reads the candidate as data from the trusted default branch's checkout, as the
upstream job does, and never contacts the private authority. What comes from where:

* the tools are the trusted checkout's: this module and ``tools/publish_audit.py``;
* the tree, the publication profile and the public ledger are the candidate's, read from its
  commit's objects. The profile a pull request carries is the one its own tree is judged by,
  so an admission inside the pull request is honoured and a head that lags the default branch
  is still judged by its own list of paths;
* the default branch is the trusted checkout's ``HEAD`` (``--tip``), not the event's base, which can
  lag it; the base must be an ancestor of it;
* the pull request and the default branch have one merge base (``MULTIPLE_MERGE_BASES``), so the
  tree the check compares with is the one a merge applies the pull request to;
* the release manifest (``assets/release_manifest.json``) is held like the profile's rules: it may not
  change (``MANIFEST_CHANGED``) and must equal the tip's (``MANIFEST_BEHIND``). The bytes of the paths
  it marks approved (``HELD_PATH_DISPOSITIONS``) and of the tables under ``assets/vfpu/`` may not
  change (``HELD_BYTES_CHANGED``), and no file the pull request adds or changes may be binary as
  the trusted audit decides it (``BINARY_CHANGED``). These change by the fork maintainer's own push;
* every path the candidate adds to ``include_paths`` is named as ``SELF_ADMITTED``, in the summary's
  title and list and as a warning annotation: the check cannot tell the fork maintainer's admission
  from a contributor's, and passes both. An added path must name a file of the candidate
  (``PROFILE_PATH_MISSING``), and at most ``SELF_ADMITTED_MAX_PATHS`` may be added
  (``SELF_ADMITTED_LIMIT``), so that every one is named on the check;
* the candidate may change the profile's ``include_paths`` only. Each added or removed path is
  listed, because on a fork the fork maintainer's admission and review are the provenance
  authority. Any other change to the profile, such as an exclusion or the default disposition,
  fails the check (``PROFILE_RULES_CHANGED``): the rules a tree is judged by are not the pull
  request's to change. The change is measured from the merge base of the base and the candidate;
* the candidate's rules, every profile key but ``include_paths``, must also equal the tip's
  (``PROFILE_RULES_BEHIND``), so a rule tightened on the default branch after the pull request
  branched is not escaped by a head that lags it. The fix is to merge the default branch in.

The check itself:

* the candidate may not change the gate's own workflow (``CI_GATE_WORKFLOW_CHANGED``), and its
  other workflows may not spell either gate's check name (``CI_CONTEXT_COLLISION``). The second
  is a byte match, a tripwire: a name built by an expression escapes it. What a fork's ruleset
  should require is **Trusted CI verdict**, which binds the check to the trusted run, never the
  check's name alone (docs/FORKING.md);
* the trusted checkout's publication audit runs on the candidate's exact tree with
  ``--public-scope --provenance-self-consistency``: game data, keys, private paths, user
  paths, unclassified paths and an inconsistent ledger are refused exactly as on the upstream.

Usage::

    python tools/provenance_mode.py --require fork          # exit 1 unless this run is a fork's
    python tools/provenance_mode.py --require upstream      # exit 1 unless it is the upstream's
    python tools/provenance_mode.py fork-check --base <sha> --candidate <sha> [--summary <file>]

``--repository`` and ``--repository-id`` default to ``GITHUB_REPOSITORY`` and
``GITHUB_REPOSITORY_ID``, which GitHub Actions sets for every run.
"""

from __future__ import annotations

import argparse
from collections.abc import Callable
import functools
import importlib.util
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
import tempfile
import traceback

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
MANIFEST = "assets/release_manifest.json"
LEDGER = "assets/public_provenance_ledger.json"
#: Release-manifest dispositions whose paths a fork pull request may not change, byte for byte.
HELD_PATH_DISPOSITIONS = frozenset(("approved_binary", "optional_excluded_if_unverified", "approved_submodule"))
#: Paths under this prefix are held byte for byte too.
HELD_PREFIX = "assets/vfpu/"
SELF_ADMITTED = "SELF_ADMITTED"
#: At most this many paths, and this many characters of their names, may a pull request admit itself: each
#: is named in the check's title, its warnings and its summary, which GitHub caps (ten warnings per step,
#: 1 MiB of summary), so more could push a path out of sight. More reach the default branch by a push.
SELF_ADMITTED_MAX_PATHS = 50
SELF_ADMITTED_MAX_CHARS = 20_000
#: Annotations GitHub shows per step; the last is kept for "and N more".
WARNING_LIMIT = 10
#: The summary's size limit, with room to spare under GitHub's 1 MiB.
SUMMARY_LIMIT = 900_000

#: The job conditions in ``WORKFLOW``. GitHub's ``==`` ignores letter case for strings, as
#: ``is_upstream`` does. They are exact complements, so exactly one of the two jobs runs, and an
#: empty ID selects the upstream's strict job. The fork job's first step repeats the check in
#: Python, which also refuses a non-numeric ID.
UPSTREAM_CONDITION = (
    f"(github.repository_id == '' || github.repository_id == '{UPSTREAM_REPOSITORY_ID}' || "
    f"github.repository == '{UPSTREAM_REPOSITORY}')"
)
FORK_CONDITION = (
    f"(github.repository_id != '' && github.repository_id != '{UPSTREAM_REPOSITORY_ID}' && "
    f"github.repository != '{UPSTREAM_REPOSITORY}')"
)


def is_upstream(repository: str | None, repository_id: str | int | None) -> bool:
    """Whether a run gets the upstream's strict path: its ID or its name matches, or its ID is unknown.

    A fork must prove it is one with a numeric repository ID that differs from the upstream's; a
    missing, empty or malformed ID fails closed to the strict path, which needs the private
    authority. The name comparison folds ASCII case only, as GitHub does. A name with any
    non-ASCII character is never the upstream's name; GitHub names are ASCII, and Unicode
    folding would let a lookalike match.
    """
    identity = "" if repository_id is None else str(repository_id).strip()
    if not (identity.isascii() and identity.isdecimal()) or int(identity) == int(UPSTREAM_REPOSITORY_ID):
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


def context_findings(workflows: dict[str, bytes], trusted_gate: bytes | None) -> list[str]:
    """A changed gate workflow, and candidate workflows that spell either gate's check name.

    A second job under the check's name would give a candidate-controlled result under the
    context a fork maintainer reads, whatever the real check concluded. The gate's own workflow
    names both checks, so it is held to the copy the candidate branched from (``trusted_gate``)
    instead: a pull request that edits it could add a job under either name. The name match is
    a byte search, a tripwire; the verdict, which binds each check to the trusted run, is what a
    ruleset should require.
    """
    findings = []
    if workflows.get(WORKFLOW) != trusted_gate:
        findings.append(
            f"CI_GATE_WORKFLOW_CHANGED: {WORKFLOW}: the pull request changes the workflow that runs this check; "
            "a change to it reaches the default branch by the fork maintainer's own push, after review")
    for path, raw in sorted(workflows.items()):
        if path == WORKFLOW:
            continue
        for context in (UPSTREAM_CONTEXT, FORK_CONTEXT):
            if context.encode("utf-8") in raw:
                findings.append(f"CI_CONTEXT_COLLISION: {shown_path(path)}: names the reserved check context {context!r}")
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


def profile_changes(repo: Path, base: str, candidate: str) -> tuple[list[str], list[str], list[str]]:
    """Added and removed ``include_paths``, and the name of every other profile key that differs."""
    before = _profile_paths(repo, base)
    after = _profile_paths(repo, candidate)
    base_document = json.loads(_git(repo, "cat-file", "blob", f"{base}:{POLICY}"))
    candidate_document = json.loads(_git(repo, "cat-file", "blob", f"{candidate}:{POLICY}"))
    if not isinstance(base_document, dict) or not isinstance(candidate_document, dict):
        raise CheckError(f"{POLICY} is not a JSON object")
    changed = sorted(
        key for key in set(base_document) | set(candidate_document)
        if key != "include_paths" and (
            key not in base_document or key not in candidate_document
            or base_document[key] != candidate_document[key])
    )
    return sorted(after - before), sorted(before - after), changed


def _merge_bases(repo: Path, base: str, candidate: str) -> list[str]:
    """Every best common ancestor of the two commits, in Git's order (the first is ``merge-base``'s)."""
    try:
        shas = _git(repo, "merge-base", "--all", base, candidate).decode("ascii").split()
    except CheckError as error:
        raise CheckError(f"{candidate} and {base} have no common ancestor") from error
    if not shas or any(len(sha) != 40 for sha in shas):
        raise CheckError(f"{candidate} and {base} have no common ancestor")
    return shas


def _merge_base(repo: Path, base: str, candidate: str) -> str:
    """The commit the candidate branched from, so a head that lags the base shows only its own changes."""
    return _merge_bases(repo, base, candidate)[0]


def _changed_blobs(repo: Path, older: str, newer: str) -> list[tuple[str, str]]:
    """``(path, blob)`` for each file ``newer`` adds or changes against ``older``, renames not followed."""
    raw = _git(repo, "diff-tree", "-r", "-z", "--no-renames", "--diff-filter=AMT", older, newer)
    fields = raw.decode("utf-8", "surrogateescape").split("\0")
    changed = []
    for index in range(0, len(fields) - 1, 2):
        meta, path = fields[index], fields[index + 1]
        if not meta.startswith(":"):
            raise CheckError(f"unexpected diff-tree record {meta!r}")
        mode, blob = meta.split(" ")[1], meta.split(" ")[3]
        if mode != "160000":
            changed.append((path, blob))
    return changed


@functools.lru_cache(maxsize=None)
def _audit_binary_test(tool: Path) -> Callable[[bytes], bool]:
    """The trusted audit's own test of whether bytes are binary, so the check and the audit cannot differ.

    Always the trusted checkout's audit, never a stand-in given for the run. The module is registered
    while it executes, as an import would, since its dataclasses look themselves up there."""
    name = "nakagawa_trusted_publish_audit"
    spec = importlib.util.spec_from_file_location(name, tool)
    if spec is None or spec.loader is None:
        raise CheckError(f"cannot load {tool}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    try:
        spec.loader.exec_module(module)
    except BaseException:
        del sys.modules[name]
        raise
    test: Callable[[bytes], bool] = module._is_binary_bytes
    return test


def _entry(repo: Path, rev: str, path: str) -> str | None:
    """``"<mode> <type> <object>"`` of ``path`` at ``rev``, or ``None`` when the commit has no such entry."""
    raw = _git(repo, "ls-tree", "-z", rev, "--", path).decode("utf-8", "surrogateescape")
    for record in raw.split("\0"):
        meta, _, name = record.partition("\t")
        if name == path:
            return meta
    return None


def _ledger_claims(repo: Path, rev: str) -> dict[str, str]:
    """Each public-ledger entry at ``rev``, by path, in a canonical form; empty when there is none to read."""
    raw = _blob(repo, rev, LEDGER)
    try:
        entries = json.loads(raw)["entries"] if raw is not None else []
    except (ValueError, KeyError, TypeError):
        return {}
    claims = {}
    for entry in entries if isinstance(entries, list) else []:
        if isinstance(entry, dict) and isinstance(entry.get("path"), str):
            claims[entry["path"]] = json.dumps(entry, sort_keys=True)
    return claims


def _blob(repo: Path, rev: str, path: str) -> bytes | None:
    """The bytes of ``path`` at ``rev``, or ``None`` when the commit has no such file."""
    try:
        return _git(repo, "cat-file", "blob", f"{rev}:{path}")
    except CheckError:
        return None


def held_paths(manifest: bytes | None) -> list[str]:
    """The paths the release manifest gives one of ``HELD_PATH_DISPOSITIONS``."""
    if manifest is None:
        return []
    try:
        document = json.loads(manifest)
    except json.JSONDecodeError as error:
        raise CheckError(f"{MANIFEST} is unreadable: {error}") from error
    components = document.get("components") if isinstance(document, dict) else None
    if not isinstance(components, list):
        raise CheckError(f"{MANIFEST} has no components list")
    paths = set()
    for component in components:
        if isinstance(component, dict) and component.get("disposition") in HELD_PATH_DISPOSITIONS:
            path = component.get("source_path") or component.get("provenance_path")
            if isinstance(path, str):
                paths.add(path)
    return sorted(paths)


def _tree_files(repo: Path, rev: str) -> set[str]:
    """The regular files and links of ``rev``'s tree."""
    listing = _git(repo, "ls-tree", "-r", "-z", "--full-tree", rev).decode("utf-8", "surrogateescape")
    files = set()
    for record in listing.split("\0"):
        if record:
            meta, _, path = record.partition("\t")
            if meta.split(" ")[1] == "blob":
                files.add(path)
    return files


def _is_ancestor(repo: Path, older: str, newer: str) -> bool:
    result = subprocess.run(["git", "-C", str(repo), "merge-base", "--is-ancestor", older, newer],
                            capture_output=True, check=False)
    return result.returncode == 0


def fork_check(repo: Path, base: str, candidate: str, *, tip: str | None = None,
               audit_tool: Path | None = None) -> tuple[int, list[str]]:
    """Run the fork provenance check. Returns the exit code and the report lines.

    ``base`` is the pull request's base as the event names it; ``tip`` is the default branch the
    trusted checkout holds (``HEAD`` from the command line), which a base can lag behind. Every
    comparison with "the default branch" is with the tip. Without a tip the base stands in for it.
    """
    base_sha = _commit(repo, base)
    tip_sha = _commit(repo, tip) if tip is not None else base_sha
    if not _is_ancestor(repo, base_sha, tip_sha):
        raise CheckError(f"base {base_sha} is not an ancestor of the default branch tip {tip_sha}")
    candidate_sha = _commit(repo, candidate)
    merge_bases = _merge_bases(repo, tip_sha, candidate_sha)
    branch_point = merge_bases[0]
    lines = [f"base {base_sha}", f"default branch tip {tip_sha}", f"candidate {candidate_sha}",
             f"merge base {branch_point}"]
    failed = False
    if len(merge_bases) > 1:
        # Every comparison below is with one tree; a merge applies the pull request to a tree built from
        # all of them. With the default branch merged in, the tip is the only merge base.
        lines.append(
            f"MULTIPLE_MERGE_BASES: the pull request and the default branch have {len(merge_bases)} merge bases "
            f"({', '.join(merge_bases)}); merge the default branch into the pull request")
        failed = True

    collisions = context_findings(_workflow_blobs(repo, candidate_sha),
                                  _workflow_blobs(repo, branch_point).get(WORKFLOW))
    lines.extend(collisions)
    failed |= bool(collisions)

    added, removed, rule_keys = profile_changes(repo, branch_point, candidate_sha)
    lines.append(f"profile: {len(added)} path(s) added, {len(removed)} removed")
    lines.extend(f"  added {shown_path(path)}" for path in added)
    lines.extend(f"  removed {shown_path(path)}" for path in removed)
    if rule_keys:
        lines.append(
            f"PROFILE_RULES_CHANGED: {POLICY}: the candidate changes {', '.join(map(shown_path, rule_keys))}; a pull request "
            "may change include_paths only. A change to the publication rules reaches the default branch by "
            "the fork maintainer's own push, after review")
        failed = True
    else:
        # The audit below judges the candidate by its own profile, so its rules must be the default
        # branch's as the trusted checkout holds them now: a rule tightened there after the pull
        # request branched would otherwise never apply to it, and a fork has no landing batch to
        # judge the merged result. The event's base can lag the tip, so it is not the measure.
        behind = profile_changes(repo, tip_sha, candidate_sha)[2]
        if behind:
            lines.append(
                f"PROFILE_RULES_BEHIND: {POLICY}: the default branch changed {', '.join(map(shown_path, behind))} after this "
                "pull request branched; merge the default branch into it so its tree is judged by the "
                "current rules")
            failed = True

    # The release manifest is held like the profile's rules, and so are the bytes of the paths it marks
    # approved, the tables under HELD_PREFIX, and every binary: each changes by the fork maintainer's
    # own push, after review.
    manifest = _blob(repo, candidate_sha, MANIFEST)
    if manifest != _blob(repo, branch_point, MANIFEST):
        lines.append(
            f"MANIFEST_CHANGED: {MANIFEST}: the pull request changes the release manifest, which is held like "
            "the profile's rules; it reaches the default branch by the fork maintainer's own push")
        failed = True
    elif manifest != _blob(repo, tip_sha, MANIFEST):
        lines.append(
            f"MANIFEST_BEHIND: {MANIFEST}: the default branch changed the release manifest after this pull "
            "request branched; merge the default branch into it")
        failed = True
    tool = audit_tool or (ROOT / "tools" / "publish_audit.py")
    held = set(held_paths(_blob(repo, branch_point, MANIFEST)))
    changed = _changed_blobs(repo, branch_point, candidate_sha)
    held |= {path for path in _tree_files(repo, branch_point) if path.startswith(HELD_PREFIX)}
    held |= {path for path, _ in changed if path.startswith(HELD_PREFIX)}
    for path in sorted(held):
        if _entry(repo, candidate_sha, path) != _entry(repo, branch_point, path):
            lines.append(
                f"HELD_BYTES_CHANGED: {shown_path(path)}: the pull request adds, changes or removes this path, "
                "whose bytes and mode only the fork maintainer's own push changes")
            failed = True
    is_binary = _audit_binary_test(ROOT / "tools" / "publish_audit.py")
    for path, blob in changed:
        if path not in held and is_binary(_git(repo, "cat-file", "blob", blob)):
            lines.append(
                f"BINARY_CHANGED: {shown_path(path)}: the pull request adds or changes this file, and its new "
                "bytes are binary; a binary reaches the default branch by the fork maintainer's own push")
            failed = True

    # Paths the pull request admits itself. The check cannot tell the fork maintainer's admission from
    # anyone else's, so each is named, here, in the summary's title and as a warning annotation. An entry
    # must name a file of the candidate, or a later pull request could add the file with nothing named.
    files = _tree_files(repo, candidate_sha)
    missing = [path for path in added if path not in files]
    lines.extend(f"PROFILE_PATH_MISSING: {shown_path(path)}: the pull request adds this path to include_paths, "
                 "and its tree has no such file; admit a path together with its file" for path in missing)
    failed |= bool(missing)
    # A changed ledger claim for a path already admitted is an admission too: it is named the same way.
    before, after = _ledger_claims(repo, branch_point), _ledger_claims(repo, candidate_sha)
    reclaimed = sorted(path for path in before.keys() & after.keys() if before[path] != after[path]
                       and path not in added)
    named = [*added, *reclaimed]
    if len(named) > SELF_ADMITTED_MAX_PATHS or sum(len(shown_path(path)) for path in named) > SELF_ADMITTED_MAX_CHARS:
        lines.append(
            f"SELF_ADMITTED_LIMIT: the pull request admits or re-claims {len(named)} path(s) itself; at most "
            f"{SELF_ADMITTED_MAX_PATHS} paths and {SELF_ADMITTED_MAX_CHARS} characters of names are named in "
            "full on the check, so more reach the default branch by the fork maintainer's own push")
        failed = True
    lines.extend(f"{SELF_ADMITTED}: {shown_path(path)}: admitted by this pull request's own profile and ledger, not on "
                 "the default branch; review it as an admission" for path in added)
    lines.extend(f"{SELF_ADMITTED}: {shown_path(path)}: this pull request changes its claim in the public ledger; "
                 "review it as an admission" for path in reclaimed)

    # The audit takes its policy from the candidate's commit, not from the checkout it runs in. The
    # checkout is the default branch, whose profile lacks what this pull request admits and may list
    # what a lagging head does not have yet. Its rules equal the tip's, checked above.
    with tempfile.TemporaryDirectory(prefix="nakagawa-fork-policy-") as scratch:
        policy = Path(scratch) / "public_source_profile.json"
        policy.write_bytes(_git(repo, "cat-file", "blob", f"{candidate_sha}:{POLICY}"))
        audit = subprocess.run(
            [sys.executable, str(tool), "--repo-root", str(repo), "--committed-tree", candidate_sha,
             "--policy", str(policy), "--public-scope", "--provenance-self-consistency"],
            capture_output=True, text=True, encoding="utf-8", errors="replace", check=False,
        )
    output = (audit.stdout + audit.stderr).strip().splitlines()
    lines.append(f"publication audit (trusted tools, candidate tree and profile): exit {audit.returncode}")
    lines.extend("  | " + line for line in output[-60:])
    failed |= audit.returncode != 0

    lines.append(f"{FORK_CONTEXT}: {'FAIL' if failed else 'PASS'}")
    return (1 if failed else 0), lines


_PLAIN_PATH = re.compile(r"[A-Za-z0-9._/+@-]+")


def shown_path(path: str) -> str:
    """A path as the report shows it. A path from the candidate's profile is the pull request's data: one
    with anything beyond plain path characters (a line break, a colon, a backtick, non-ASCII) is shown as
    an ASCII JSON string, so it cannot start a line, end a field or a code span, or pose as another path."""
    if _PLAIN_PATH.fullmatch(path):
        return path
    return json.dumps(path, ensure_ascii=True).replace("`", "\\u0060")


def self_admitted(lines: list[str]) -> list[str]:
    """The paths, as shown, that a fork check's report names as admitted by the pull request itself."""
    prefix = f"{SELF_ADMITTED}: "
    paths = []
    for line in lines:
        if not line.startswith(prefix):
            continue
        rest = line[len(prefix):]
        if rest.startswith('"'):
            paths.append(json.dumps(json.JSONDecoder().raw_decode(rest)[0], ensure_ascii=True).replace("`", "\\u0060"))
        else:
            paths.append(rest.split(": ", 1)[0])
    return paths


def _fence(lines: list[str]) -> str:
    """A code fence longer than any backtick run in ``lines`` (the audit's output names candidate paths)."""
    longest = max((len(run) for line in lines for run in re.findall(r"`+", line)), default=0)
    return "`" * max(3, longest + 1)


def _summary(lines: list[str], code: int) -> str:
    verdict = "passed" if code == 0 else "failed"
    admitted = self_admitted(lines)
    title = f"### {FORK_CONTEXT}: {verdict}"
    if admitted:
        shown = ", ".join(f"`{path}`" for path in admitted[:10])
        more = f" and {len(admitted) - 10} more" if len(admitted) > 10 else ""
        title += f"; this pull request admits {len(admitted)} path(s) itself: {shown}{more}"
    listed = [f"- `{path}`" for path in admitted[:SELF_ADMITTED_MAX_PATHS]]
    if len(admitted) > SELF_ADMITTED_MAX_PATHS:
        listed.append(f"- and {len(admitted) - SELF_ADMITTED_MAX_PATHS} more (over the limit; the job log has all)")
    head = [title, "", *(["Paths this pull request admits itself:", "", *listed, ""] if listed else [])]
    budget = SUMMARY_LIMIT - 2_000 - sum(len(line.encode("utf-8")) + 1 for line in head)
    report = []
    for line in lines:
        budget -= len(line.encode("utf-8")) + 1
        if budget < 0:
            report.append("(report cut to fit the summary; the job log has all of it)")
            break
        report.append(line)
    return "\n".join([
        *head,
        "This repository is not the upstream, so it has no access to the upstream's private trusted",
        "ledger. This check ran the trusted default branch's publication audit on the candidate's exact",
        "tree, under the candidate's own publication profile, whose rules and release manifest must",
        "equal the default branch's, and checked the public ledger's self-consistency. A pull request",
        "may change the profile's include_paths only, and not the release manifest, the paths it",
        "approves, the VFPU tables, any binary, or this check's own workflow. A pass does not mean the",
        "fork maintainer admitted anything: the paths a pull request admits or re-claims itself are named",
        "above, for review. The",
        "fork maintainer's admission (`tools/fork_init.py`, `tools/admit_paths.py`) and review are this",
        "fork's provenance authority; see docs/FORKING.md.",
        "",
        _fence(report) + "text",
        *report,
        _fence(report),
        "",
    ])


def warning_annotations(lines: list[str]) -> list[str]:
    """GitHub workflow commands that put each self-admitted path on the check as a warning."""
    # Workflow-command data escapes %, CR and LF; the path is already shown_path's single ASCII line.
    admitted = self_admitted(lines)
    shown = admitted if len(admitted) <= WARNING_LIMIT else admitted[:WARNING_LIMIT - 1]
    commands = [f"::warning title={FORK_CONTEXT} - self-admitted path::"
                f"{path.replace('%', '%25').replace(chr(13), '%0D').replace(chr(10), '%0A')} is admitted by this "
                "pull request itself, not by the fork maintainer on the default branch" for path in shown]
    if len(admitted) > len(shown):
        commands.append(f"::warning title={FORK_CONTEXT} - self-admitted paths::and {len(admitted) - len(shown)} "
                        "more paths this pull request admits itself; the check's summary lists them all")
    return commands


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
    parser.add_argument("--tip", default="HEAD",
                        help="fork-check: the default branch's tip, which --base must be an ancestor of "
                             "(default: HEAD of --repo, the trusted checkout)")
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
    # The report carries the candidate's paths and the audit's output about them, so the runner stops
    # reading workflow commands while it prints; only the warnings below are commands.
    token = secrets.token_hex(16)
    print(f"::stop-commands::{token}", flush=True)
    try:
        try:
            code, lines = fork_check(args.repo.resolve(), args.base, args.candidate, tip=args.tip)
        except CheckError as error:
            print(f"{FORK_CONTEXT}: input error: {error}", flush=True)
            return 2
        except Exception:  # noqa: BLE001 - reported here, while workflow commands are still stopped
            traceback.print_exc(file=sys.stdout)
            print(f"{FORK_CONTEXT}: internal error", flush=True)
            return 2
        print("\n".join(lines), flush=True)
    finally:
        print(f"::{token}::", flush=True)
    for command in warning_annotations(lines):
        print(command)
    if args.summary:
        with args.summary.open("a", encoding="utf-8") as handle:
            handle.write(_summary(lines, code))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
