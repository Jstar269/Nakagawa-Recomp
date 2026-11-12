# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The upstream keeps the trusted attestation; only a fork runs the Fork provenance check.

``tools/provenance_mode.py`` decides the mode and ``.github/workflows/provenance-attestation.yml``
gates its two jobs on the same constants. These tests hold the two together and prove that the
upstream path still requires the private authority and that nothing switches fork mode on for
``Jstar269/Nakagawa-Recomp``.
"""

from __future__ import annotations

import contextlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

try:
    import provenance_mode as pm
except ModuleNotFoundError:  # Running as ``python -m unittest tools.test_provenance_mode``.
    from tools import provenance_mode as pm

ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / pm.WORKFLOW

UPSTREAM_ID = pm.UPSTREAM_REPOSITORY_ID


def _job_block(text: str, job: str) -> str:
    match = re.search(rf"(?ms)^  {re.escape(job)}:\n(.*?)(?=^  [A-Za-z0-9_-]+:\n|\Z)", text)
    if match is None:
        raise AssertionError(f"job {job!r} not found in {WORKFLOW}")
    return match.group(1)


def _code(block: str) -> str:
    """A job block without its comment lines."""
    return "\n".join(line for line in block.splitlines() if not line.lstrip().startswith("#")) + "\n"


def _job_condition(block: str) -> str:
    match = re.search(r"(?m)^    if: >-\n((?:^      .*\n)+)", block)
    if match is None:
        raise AssertionError("job has no folded if: condition")
    expression = " ".join(line.strip() for line in match.group(1).splitlines())
    expression = expression.strip()
    if not (expression.startswith("${{") and expression.endswith("}}")):
        raise AssertionError(f"condition is not a single expression: {expression!r}")
    return expression[3:-2].strip()


_TOKEN = re.compile(r"\s*(?:(\()|(\))|(&&)|(\|\|)|(==)|(!=)|'([^']*)'|([A-Za-z_][A-Za-z0-9_.\-]*))")


def evaluate(expression: str, context: dict[str, str]) -> bool:
    """Evaluate the subset of GitHub's expression language the two job conditions use.

    Identifiers are dotted context names, literals are single-quoted strings, and ``==``/``!=``
    compare strings ignoring case, as GitHub does. ``&&`` binds tighter than ``||``.
    """
    tokens = []
    position = 0
    while position < len(expression):
        if expression[position:].strip() == "":
            break
        match = _TOKEN.match(expression, position)
        if match is None:
            raise AssertionError(f"cannot parse {expression[position:]!r}")
        position = match.end()
        kind = match.lastindex or 0
        value = match.group(kind)
        tokens.append((kind, value))
    index = 0

    def peek():
        return tokens[index] if index < len(tokens) else (None, None)

    def take():
        nonlocal index
        index += 1
        return tokens[index - 1]

    def operand() -> object:
        kind, value = take()
        if kind == 1:
            result = disjunction()
            if take()[0] != 2:
                raise AssertionError("unbalanced parentheses")
            return result
        if kind == 7:
            return value
        if kind == 8:
            if value not in context:
                raise AssertionError(f"context {value!r} is not modelled")
            return context[value]
        raise AssertionError(f"unexpected token {value!r}")

    def comparison() -> object:
        left = operand()
        kind, _ = peek()
        if kind in (5, 6):
            take()
            right = operand()
            equal = str(left).lower() == str(right).lower()
            return equal if kind == 5 else not equal
        return left

    def conjunction() -> bool:
        result = bool(comparison())
        while peek()[0] == 3:
            take()
            result = bool(comparison()) and result
        return result

    def disjunction() -> bool:
        result = conjunction()
        while peek()[0] == 4:
            take()
            result = conjunction() or result
        return result

    value = disjunction()
    if index != len(tokens):
        raise AssertionError("trailing tokens in expression")
    return value


ROUTES = ("pull_request_target", "workflow_run", "workflow_dispatch")


def _context(repository: str, repository_id: str, route: str = "pull_request_target") -> dict[str, str]:
    """The context of a run that judges a pull request, by one of the three routes (docs/CI.md)."""
    return {
        "github.event_name": route,
        # GitHub reads a missing context value as the empty string.
        "github.event.pull_request.base.ref": "main" if route == "pull_request_target" else "",
        "github.event.workflow_run.event": "pull_request" if route == "workflow_run" else "",
        "inputs.pull_request_number": "7" if route == "workflow_dispatch" else "",
        "github.event.repository.default_branch": "main",
        "github.repository": repository,
        "github.repository_id": repository_id,
    }


UPSTREAM_RUNS = (
    ("Jstar269/Nakagawa-Recomp", UPSTREAM_ID),
    ("jstar269/nakagawa-recomp", UPSTREAM_ID),
    # A rename or a transfer to an organisation keeps the ID.
    ("Nakagawa-Project/Nakagawa-Recomp", UPSTREAM_ID),
    ("Jstar269/Nakagawa", UPSTREAM_ID),
    # A run that could not see the ID still matches by name, in any letter case.
    ("Jstar269/Nakagawa-Recomp", ""),
    ("jstar269/nakagawa-recomp", ""),
    ("JSTAR269/NAKAGAWA-RECOMP", ""),
    # The name alone decides when the ID is a different number: compared without regard to case.
    ("jstar269/nakagawa-recomp", "987654321"),
    ("JSTAR269/Nakagawa-RECOMP", "987654321"),
)
#: Runs whose ID is unknown take the strict path whatever their name says.
UNKNOWN_ID_RUNS = (
    ("example-user/Nakagawa-Recomp", ""),
    ("example-org/nakagawa-recomp-fork", ""),
)
FORK_RUNS = (
    ("example-user/Nakagawa-Recomp", "987654321"),
    ("example-org/nakagawa-recomp-fork", "123"),
)


class ModeDecisionTest(unittest.TestCase):
    def test_upstream_by_id_or_name(self) -> None:
        for repository, repository_id in UPSTREAM_RUNS:
            with self.subTest(repository=repository, repository_id=repository_id):
                self.assertEqual(pm.mode(repository, repository_id or None), "upstream")

    def test_the_id_is_compared_as_a_number(self) -> None:
        # Review 2309-4e69563b9228 N6: a zero-padded upstream ID was a fork by string comparison.
        self.assertTrue(pm.is_upstream("example-user/fork", "0" + pm.UPSTREAM_REPOSITORY_ID))
        self.assertTrue(pm.is_upstream("example-user/fork", int(pm.UPSTREAM_REPOSITORY_ID)))
        self.assertFalse(pm.is_upstream("example-user/fork", "1" + pm.UPSTREAM_REPOSITORY_ID))

    def test_an_unknown_id_fails_closed_to_upstream(self) -> None:
        for repository_id in (None, "", "  ", "abc", "12a", "-5", "١٢"):
            for repository in (None, "", "example-user/Nakagawa-Recomp"):
                with self.subTest(repository=repository, repository_id=repository_id):
                    self.assertEqual(pm.mode(repository, repository_id), "upstream")
        self.enterContext(contextlib.redirect_stdout(io.StringIO()))
        with mock.patch.dict(os.environ, {"GITHUB_REPOSITORY": "example-user/Nakagawa-Recomp"}, clear=True):
            self.assertEqual(pm.main(["--require", "fork"]), 1)
        self.assertEqual(pm.main(["--repository", "example-user/Nakagawa-Recomp", "--repository-id", "abc",
                                  "--require", "fork"]), 1)

    def test_fork_needs_a_different_name_and_id(self) -> None:
        for repository, repository_id in FORK_RUNS:
            with self.subTest(repository=repository):
                self.assertEqual(pm.mode(repository, repository_id), "fork")

    def test_lookalike_names_are_not_folded(self) -> None:
        # Unicode folding would make these equal; GitHub names are ASCII, so they are other repositories.
        self.assertFalse(pm.is_upstream("J\u017ftar269/Nakagawa-Recomp", "1"))
        self.assertFalse(pm.is_upstream("Jstar269/Na\u212aagawa-Recomp", "1"))

    def test_require_refuses_the_other_mode(self) -> None:
        self.enterContext(contextlib.redirect_stdout(io.StringIO()))
        self.assertEqual(pm.main(["--repository", "Jstar269/Nakagawa-Recomp", "--repository-id", UPSTREAM_ID,
                                  "--require", "fork"]), 1)
        self.assertEqual(pm.main(["--repository", "x/y", "--repository-id", "1", "--require", "upstream"]), 1)
        self.assertEqual(pm.main(["--repository", "x/y", "--repository-id", "1", "--require", "fork"]), 0)

    def test_fork_check_never_runs_for_the_upstream(self) -> None:
        for repository, repository_id in UPSTREAM_RUNS:
            with self.subTest(repository=repository):
                code = pm.main(["fork-check", "--repository", repository, "--repository-id", repository_id,
                                "--base", "HEAD", "--candidate", "HEAD"])
                self.assertEqual(code, 1)

    def test_environment_cannot_switch_the_upstream(self) -> None:
        # Only GITHUB_REPOSITORY and GITHUB_REPOSITORY_ID are read; no other variable takes part.
        environment = {
            "GITHUB_REPOSITORY": "Jstar269/Nakagawa-Recomp",
            "GITHUB_REPOSITORY_ID": UPSTREAM_ID,
            "NK_PROVENANCE_MODE": "fork",
            "NK_PROVENANCE_AUTHORITY": "example/authority",
        }
        self.enterContext(contextlib.redirect_stdout(io.StringIO()))
        with mock.patch.dict(os.environ, environment):
            self.assertEqual(pm.main(["--require", "fork"]), 1)
            self.assertEqual(pm.main(["--require", "upstream"]), 0)
        source = (ROOT / "tools" / "provenance_mode.py").read_text(encoding="utf-8")
        self.assertEqual(sorted(set(re.findall(r"os\.environ\.get\(\"([A-Z_]+)\"", source))),
                         ["GITHUB_REPOSITORY", "GITHUB_REPOSITORY_ID"])


class WorkflowConditionTest(unittest.TestCase):
    text: str
    attest: str
    fork: str

    @classmethod
    def setUpClass(cls) -> None:
        cls.text = WORKFLOW.read_text(encoding="utf-8")
        cls.attest = _job_block(cls.text, "attest")
        cls.fork = _job_block(cls.text, "fork_check")

    def test_conditions_carry_the_module_constants(self) -> None:
        self.assertIn(pm.UPSTREAM_CONDITION, _job_condition(self.attest))
        self.assertIn(pm.FORK_CONDITION, _job_condition(self.fork))
        self.assertIn(f"name: {pm.UPSTREAM_CONTEXT}\n", self.attest)
        self.assertIn(f"name: {pm.FORK_CONTEXT}\n", self.fork)

    def test_exactly_one_job_runs_and_the_upstream_always_runs_the_attestation(self) -> None:
        attest = _job_condition(self.attest)
        fork = _job_condition(self.fork)
        # On every route: pull_request_target, a requested CI run, and a dispatch naming a pull request.
        for route in ROUTES:
            for repository, repository_id in UPSTREAM_RUNS:
                with self.subTest(route=route, repository=repository, repository_id=repository_id):
                    context = _context(repository, repository_id, route)
                    self.assertTrue(evaluate(attest, context))
                    self.assertFalse(evaluate(fork, context))
            for repository, repository_id in FORK_RUNS:
                with self.subTest(route=route, repository=repository):
                    context = _context(repository, repository_id, route)
                    self.assertFalse(evaluate(attest, context))
                    self.assertTrue(evaluate(fork, context))
            for repository, repository_id in UNKNOWN_ID_RUNS:
                with self.subTest(route=route, repository=repository, repository_id="(empty)"):
                    context = _context(repository, repository_id, route)
                    self.assertTrue(evaluate(attest, context))
                    self.assertFalse(evaluate(fork, context))

    def test_neither_job_runs_for_another_base_branch(self) -> None:
        for repository, repository_id in UPSTREAM_RUNS + FORK_RUNS:
            context = _context(repository, repository_id) | {"github.event.pull_request.base.ref": "evil-base"}
            self.assertFalse(evaluate(_job_condition(self.attest), context))
            self.assertFalse(evaluate(_job_condition(self.fork), context))

    def test_neither_job_runs_when_no_pull_request_is_named(self) -> None:
        # CI's own push, schedule and merge-group runs arrive as workflow_run events, and a dispatch without a
        # pull request number builds candidate binaries: neither is a pull request to judge.
        for repository, repository_id in UPSTREAM_RUNS + FORK_RUNS + UNKNOWN_ID_RUNS:
            for change in ({"github.event_name": "workflow_run", "github.event.workflow_run.event": "push"},
                           {"github.event_name": "workflow_run", "github.event.workflow_run.event": "merge_group"},
                           {"github.event_name": "workflow_dispatch", "inputs.pull_request_number": ""},
                           {"github.event_name": "push"}):
                with self.subTest(repository=repository, change=change):
                    context = _context(repository, repository_id, "workflow_dispatch") | {
                        "inputs.pull_request_number": "", "github.event.workflow_run.event": ""} | change
                    self.assertFalse(evaluate(_job_condition(self.attest), context))
                    self.assertFalse(evaluate(_job_condition(self.fork), context))

    def test_the_fork_job_resolves_its_target_exactly_as_the_attestation_does(self) -> None:
        # The step that turns an event, a requested CI run or a dispatch into one pull request, head and base
        # is the same text in both jobs, and every later step of the fork job reads its outputs.
        def resolver(block: str) -> str:
            start = block.index("      - name: Resolve the pull request at the named head\n")
            return block[start:block.index("\n\n", start)]

        self.assertEqual(resolver(self.fork), resolver(self.attest))
        self.assertIn("--attestation-target)", resolver(self.fork))
        code = _code(self.fork)
        after = code[code.index("refs/candidate/head") - 600:]
        self.assertEqual(code.count("github.event.pull_request."), 4)   # the condition and the resolver's env
        self.assertEqual(code.count("${{ steps.target.outputs.head_sha }}"), 2)
        self.assertEqual(code.count("${{ steps.target.outputs.base_sha }}"), 2)
        self.assertEqual(code.count("${{ steps.target.outputs.number }}"), 1)
        self.assertNotIn("github.event.", after)
        self.assertIn("      pull-requests: read\n", code)

    def test_upstream_job_still_requires_the_private_authority(self) -> None:
        block = _code(self.attest)
        self.assertIn("GH_TOKEN: ${{ secrets.PROVENANCE_AUTHORITY_TOKEN }}", block)
        self.assertIn("TRUSTED_REPO: Jstar269/nakagawa-recomp-history-private", block)
        self.assertIn("TRUSTED_LEDGER_PATH: docs/provenance/IMPLEMENTATION_PROVENANCE.json", block)
        # A missing token fails the job; it never falls back to a weaker check.
        self.assertRegex(block, r'if \[ -z "\$\{GH_TOKEN:-\}" \]; then\n(?:.*\n){1,4}?\s+exit 1')
        self.assertIn('--trusted-ledger "${RUNNER_TEMP}/trusted/IMPLEMENTATION_PROVENANCE.json"', block)
        self.assertIn("--ephemeral", block)
        self.assertIn("--require-immutable-revisions", block)
        self.assertNotIn("continue-on-error", block)
        self.assertNotIn("provenance_mode", block)
        # Its one audit run is of the candidate's committed tree, from the trusted checkout (#2387); the fork
        # path adds no audit of its own here.
        self.assertEqual(block.count("tools/publish_audit.py"), 1)
        self.assertIn('--committed-tree "${HEAD_SHA}"', block)

    def test_fork_job_reads_no_secret_and_never_contacts_the_authority(self) -> None:
        block = _code(self.fork)
        # Every way an expression reaches the secrets context: secrets.X, secrets['X'], toJSON(secrets).
        self.assertIsNone(re.search(r"\bsecrets\b", block), "the fork job names the secrets context")
        self.assertNotIn("history-private", block)
        self.assertNotIn("TRUSTED_REPO", block)
        self.assertNotIn("gh api", block)
        self.assertNotIn("continue-on-error", block)
        self.assertIn("ref: ${{ github.event.repository.default_branch }}", block)
        self.assertIn("python tools/provenance_mode.py --require fork", block)
        self.assertIn("python tools/provenance_mode.py fork-check", block)
        # The guard runs before the pull request is looked up and before the candidate is fetched or checked.
        self.assertLess(block.index("--require fork"), block.index("--attestation-target"))
        self.assertLess(block.index("--require fork"), block.index("refs/candidate/head"))
        # The one token is the job's own read-only one, for the lookup.
        self.assertEqual(re.findall(r"\$\{\{ *(github\.token|secrets\.[A-Z_]+) *\}\}", block), ["github.token"])

    def test_fork_job_only_reads_and_its_check_decides_the_job(self) -> None:
        # Review 2309-4e69563b9228 N9 (W9, W10): neither the job's permissions nor the check's exit code
        # deciding the job was pinned.
        self.assertIn("    permissions:\n      contents: read\n      pull-requests: read\n    steps:\n", self.fork)
        step = self.fork[self.fork.index("      - name: Check the candidate with the trusted publication audit\n"):]
        self.assertIn("          set -euo pipefail\n", step)
        # Review 2309-36f775933693 S1: the default branch is the trusted checkout's HEAD, not the event's base.
        self.assertIn('          python tools/provenance_mode.py fork-check --repo "${GITHUB_WORKSPACE}" \\\n'
                      '            --base "${BASE_SHA}" --tip HEAD --candidate "${HEAD_SHA}" '
                      '--summary "${GITHUB_STEP_SUMMARY}"\n',
                      step)
        for tolerant in ("|| true", "|| :", "continue-on-error", "if: always()", "; exit 0"):
            self.assertNotIn(tolerant, step)

    def test_check_names_are_unique_in_the_workflow(self) -> None:
        self.assertEqual(self.text.count(f"    name: {pm.UPSTREAM_CONTEXT}\n"), 1)
        self.assertEqual(self.text.count(f"    name: {pm.FORK_CONTEXT}\n"), 1)


def _git(repo: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(repo), "-c", "user.name=test", "-c", "user.email=test@example.invalid",
         "-c", "commit.gpgsign=false", *args],
        check=True, capture_output=True, text=True,
    ).stdout.strip()


class ForkCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        self.scratch = tempfile.TemporaryDirectory(prefix="nk-fork-check-")
        self.repo = Path(self.scratch.name)
        _git(self.repo, "init", "--quiet", "--initial-branch=main")
        self._write_profile(["README.md"])
        (self.repo / "README.md").write_text("fork\n", encoding="utf-8")
        _git(self.repo, "add", "-A")
        _git(self.repo, "commit", "--quiet", "-m", "base")
        self.base = _git(self.repo, "rev-parse", "HEAD")

    def tearDown(self) -> None:
        self.scratch.cleanup()

    def _write_profile(self, paths: list[str], **extra: object) -> None:
        path = self.repo / pm.POLICY
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({"name": "public-safe-v1", "include_paths": paths, **extra}), encoding="utf-8")

    def _commit(self) -> str:
        _git(self.repo, "add", "-A")
        _git(self.repo, "commit", "--quiet", "-m", "candidate")
        return _git(self.repo, "rev-parse", "HEAD")

    def _stub_audit(self, code: int) -> Path:
        # Prints the policy it was handed, so a test can see whose profile the audit judged by.
        stub = self.repo.parent / f"{self.repo.name}-audit-{code}.py"
        stub.write_text(
            "import sys\nfrom pathlib import Path\nprint('stub audit')\n"
            "print('POLICY ' + Path(sys.argv[sys.argv.index('--policy') + 1]).read_text(encoding='utf-8'))\n"
            f"sys.exit({code})\n", encoding="utf-8")
        self.addCleanup(stub.unlink, missing_ok=True)
        return stub

    @staticmethod
    def _policy_seen(lines: list[str]) -> dict[str, object]:
        (line,) = [line for line in lines if line.startswith("  | POLICY ")]
        seen = json.loads(line[len("  | POLICY "):])
        assert isinstance(seen, dict)
        return seen

    def test_lists_profile_changes_and_passes_on_a_clean_audit(self) -> None:
        (self.repo / "NEW.md").write_text("new\n", encoding="utf-8")
        self._write_profile(["NEW.md", "README.md"])
        head = self._commit()
        code, lines = pm.fork_check(self.repo, self.base, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 0, lines)
        self.assertIn("  added NEW.md", lines)
        self.assertFalse([line for line in lines if line.startswith("PROFILE_RULES_CHANGED")])
        self.assertEqual(lines[-1], f"{pm.FORK_CONTEXT}: PASS")
        # Review 2309-36f775933693 S3: a pass that covers a pull request's own admission says so, by path.
        self.assertEqual(pm.self_admitted(lines), ["NEW.md"])
        self.assertTrue(pm._summary(lines, code).startswith(
            f"### {pm.FORK_CONTEXT}: passed; this pull request admits 1 path(s) itself: `NEW.md`\n"))
        self.assertEqual(len(pm.warning_annotations(lines)), 1)
        self.assertTrue(pm.warning_annotations(lines)[0].startswith("::warning title="))
        self.assertIn("::NEW.md is admitted by this pull request itself", pm.warning_annotations(lines)[0])
        # A pull request that admits nothing gets a plain title and no warning.
        code, lines = pm.fork_check(self.repo, self.base, self.base, audit_tool=self._stub_audit(0))
        self.assertEqual(pm.self_admitted(lines), [])
        self.assertTrue(pm._summary(lines, code).startswith(f"### {pm.FORK_CONTEXT}: passed\n"))
        self.assertEqual(pm.warning_annotations(lines), [])

    def test_the_audit_judges_the_candidate_by_the_candidates_own_profile(self) -> None:
        # Failing-before: the audit took the profile from the checkout it ran in. On hosted CI that is the
        # default branch, so the path a pull request admits was refused as unclassified.
        (self.repo / "NEW.md").write_text("new\n", encoding="utf-8")
        self._write_profile(["NEW.md", "README.md"])
        head = self._commit()
        # The hosted layout: the checkout the tools run in holds the base's files.
        _git(self.repo, "checkout", "--quiet", "--detach", self.base)
        self.assertEqual(json.loads((self.repo / pm.POLICY).read_text(encoding="utf-8"))["include_paths"],
                         ["README.md"])
        code, lines = pm.fork_check(self.repo, self.base, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 0, lines)
        self.assertEqual(self._policy_seen(lines)["include_paths"], ["NEW.md", "README.md"])

    def test_a_change_to_the_profiles_rules_fails_the_check(self) -> None:
        for extra in ({"exclude_paths": ["x"]}, {"default_disposition": "ALLOW"}, {"name": "other"}):
            with self.subTest(extra=extra):
                self._write_profile(["README.md"], **extra)
                head = self._commit()
                code, lines = pm.fork_check(self.repo, self.base, head, audit_tool=self._stub_audit(0))
                self.assertEqual(code, 1, lines)
                (finding,) = [line for line in lines if line.startswith("PROFILE_RULES_CHANGED")]
                self.assertIn(f"the candidate changes {next(iter(extra))};", finding)
                self.assertEqual(lines[-1], f"{pm.FORK_CONTEXT}: FAIL")
                _git(self.repo, "reset", "--quiet", "--hard", self.base)

    def test_a_removed_rule_is_a_rules_change(self) -> None:
        self._write_profile(["README.md"], exclude_paths=["x"])
        base = self._commit()
        self._write_profile(["README.md"])
        head = self._commit()
        code, lines = pm.fork_check(self.repo, base, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 1, lines)
        self.assertTrue(any(line.startswith("PROFILE_RULES_CHANGED") and "exclude_paths" in line for line in lines))

    def test_a_head_that_lags_the_base_shows_only_its_own_changes(self) -> None:
        # The default branch admits a path after the pull request branched. Measured against the base's
        # tip that looks like a removal by the pull request; measured from the merge base it is nothing.
        _git(self.repo, "checkout", "--quiet", "-b", "topic", self.base)
        (self.repo / "TOPIC.md").write_text("topic\n", encoding="utf-8")
        self._write_profile(["README.md", "TOPIC.md"])
        head = self._commit()
        _git(self.repo, "checkout", "--quiet", "main")
        (self.repo / "LATER.md").write_text("later\n", encoding="utf-8")
        self._write_profile(["LATER.md", "README.md"], exclude_paths=["x"])
        tip = self._commit()
        code, lines = pm.fork_check(self.repo, tip, head, audit_tool=self._stub_audit(0))
        self.assertIn(f"merge base {self.base}", lines)
        self.assertIn("profile: 1 path(s) added, 0 removed", lines)
        self.assertIn("  added TOPIC.md", lines)
        self.assertFalse([line for line in lines if line.startswith("PROFILE_RULES_CHANGED")], lines)
        self.assertEqual(self._policy_seen(lines)["include_paths"], ["README.md", "TOPIC.md"])
        # Failing-before: the audit judged the lagging head by its branch point's rules, so the
        # exclusion added on the tip after it branched never applied to it, and the check passed.
        self.assertEqual(code, 1, lines)
        (finding,) = [line for line in lines if line.startswith("PROFILE_RULES_BEHIND")]
        self.assertIn("the default branch changed exclude_paths after", finding)
        # Merged with the tip, the head carries the tip's rules and passes.
        _git(self.repo, "checkout", "--quiet", "topic")
        _git(self.repo, "merge", "--quiet", "--no-edit", "-X", "theirs", "main")
        self._write_profile(["LATER.md", "README.md", "TOPIC.md"], exclude_paths=["x"])
        merged = self._commit()
        code, lines = pm.fork_check(self.repo, tip, merged, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 0, lines)

    def _lagging_head_and_stricter_tip(self) -> tuple[str, str]:
        _git(self.repo, "checkout", "--quiet", "-b", "topic", self.base)
        (self.repo / "TOPIC.md").write_text("topic\n", encoding="utf-8")
        self._write_profile(["README.md", "TOPIC.md"])
        head = self._commit()
        _git(self.repo, "checkout", "--quiet", "main")
        self._write_profile(["README.md"], exclude_paths=["x"])
        return head, self._commit()

    def test_a_stale_event_base_does_not_hide_a_rule_tightened_on_the_tip(self) -> None:
        # Review 2309-36f775933693 S1: the event's base can lag the default branch and equal the merge
        # base, so a rule tightened on the tip was compared with nothing. The tip is the measure.
        head, tip = self._lagging_head_and_stricter_tip()
        code, lines = pm.fork_check(self.repo, self.base, head, tip=tip, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 1, lines)
        self.assertIn(f"default branch tip {tip}", lines)
        (finding,) = [line for line in lines if line.startswith("PROFILE_RULES_BEHIND")]
        self.assertIn("changed exclude_paths after", finding)
        # Failing-before: measured against the stale base alone, the check passed.
        code, lines = pm.fork_check(self.repo, self.base, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 0, lines)

    def test_a_path_from_the_pull_request_cannot_issue_workflow_commands(self) -> None:
        # Security review of 913c7747: the candidate's include_paths reached the log, the warning
        # annotations and the summary verbatim, so a path with a line break could issue a workflow command
        # and one with a backtick could end the summary's code span.
        hostile = "x\n::error title=Fork provenance check::forged\n`b`"
        _git(self.repo, "checkout", "--quiet", "-b", "hostile", self.base)
        self._write_profile(["README.md", hostile, "a: b"])
        head = self._commit()
        _git(self.repo, "checkout", "--quiet", "main")
        code, lines = pm.fork_check(self.repo, self.base, head, audit_tool=self._stub_audit(0))
        # Neither entry names a file (a name with a line break cannot be made on every platform).
        self.assertEqual(code, 1, lines)
        self.assertEqual(len([line for line in lines if line.startswith("PROFILE_PATH_MISSING: ")]), 2, lines)
        self.assertFalse([line for line in lines if "\n" in line or "\r" in line], lines)
        self.assertIn('  added "x\\n::error title=Fork provenance check::forged\\n\\u0060b\\u0060"', lines)
        self.assertEqual(pm.self_admitted(lines), ['"a: b"', '"x\\n::error title=Fork provenance check::forged'
                                                   '\\n\\u0060b\\u0060"'])
        self.assertNotIn("`b`", pm._summary(lines, code).splitlines()[0])
        self.assertEqual(pm._fence(["a ```` b"]), "`````")
        real_fork_check = pm.fork_check

        def stubbed(repo: Path, base: str, candidate: str, *, tip: str | None = None) -> tuple[int, list[str]]:
            code, lines = real_fork_check(repo, base, candidate, tip=tip, audit_tool=self._stub_audit(0))
            return code, lines

        out = io.StringIO()
        with mock.patch.object(pm, "fork_check", stubbed), mock.patch("sys.stdout", out):
            pm.main(["fork-check", "--repository", "example-user/Nakagawa-Recomp", "--repository-id", "42",
                     "--repo", str(self.repo), "--base", self.base, "--candidate", head])
        printed = out.getvalue().splitlines()
        # The report is printed between ::stop-commands:: and its resume token; after it come only warnings.
        stop = re.fullmatch(r"::stop-commands::([0-9a-f]{32})", printed[0])
        self.assertIsNotNone(stop, printed[:2])
        assert stop is not None
        resume = printed.index(f"::{stop.group(1)}::")
        self.assertEqual([line for line in printed[1:resume] if line.startswith("::")], [])
        warnings = printed[resume + 1:]
        self.assertEqual(len(warnings), 2, warnings)
        prefix = "::warning title=Fork provenance check - self-admitted path::"
        suffix = " is admitted by this pull request itself, not by the fork maintainer on the default branch"
        self.assertEqual(warnings, [prefix + '"a: b"' + suffix,
                                    prefix + '"x\\n::error title=Fork provenance check::forged\\n\\u0060b\\u0060"'
                                    + suffix])

    def test_the_resume_line_follows_an_input_error(self) -> None:
        # Review 2309-913c7747 N2: commands stayed stopped when the check ended early.
        out = io.StringIO()
        with mock.patch("sys.stdout", out):
            code = pm.main(["fork-check", "--repository", "example-user/Nakagawa-Recomp", "--repository-id", "42",
                            "--repo", str(self.repo), "--base", "0" * 40, "--candidate", self.base])
        self.assertEqual(code, 2)
        printed = out.getvalue().splitlines()
        stop = re.fullmatch(r"::stop-commands::([0-9a-f]{32})", printed[0])
        assert stop is not None
        self.assertEqual(printed[-1], f"::{stop.group(1)}::")
        # Review 2309-66fce005c252 answer 6 (M2, M3): the token differs on every run, and an unexpected error
        # is printed before the resume line.
        out = io.StringIO()
        with mock.patch.object(pm, "fork_check", side_effect=RuntimeError("x\n::error::forged")), \
                mock.patch("sys.stdout", out):
            code = pm.main(["fork-check", "--repository", "example-user/Nakagawa-Recomp", "--repository-id", "42",
                            "--repo", str(self.repo), "--base", self.base, "--candidate", self.base])
        self.assertEqual(code, 2)
        again = out.getvalue().splitlines()
        second = re.fullmatch(r"::stop-commands::([0-9a-f]{32})", again[0])
        assert second is not None
        self.assertNotEqual(second.group(1), stop.group(1))
        resume = again.index(f"::{second.group(1)}::")
        self.assertEqual(resume, len(again) - 1)
        self.assertIn("::error::forged", "\n".join(again[1:resume]))
        self.assertIn(f"{pm.FORK_CONTEXT}: internal error", again[1:resume])

    def test_a_profile_entry_names_a_file_of_the_pull_request(self) -> None:
        # Review 2309-913c7747 S3: an entry with no file passed, and a later pull request could then add the
        # file with nothing named.
        _git(self.repo, "checkout", "--quiet", "-b", "entry", self.base)
        self._write_profile(["README.md", "LATER.md"])
        head = self._commit()
        code, lines = pm.fork_check(self.repo, self.base, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 1, lines)
        self.assertIn("PROFILE_PATH_MISSING: LATER.md: the pull request adds this path to include_paths, and its "
                      "tree has no such file; admit a path together with its file", lines)
        (self.repo / "LATER.md").write_text("later\n", encoding="utf-8")
        head = self._commit()
        code, lines = pm.fork_check(self.repo, self.base, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 0, lines)
        self.assertEqual(pm.self_admitted(lines), ["LATER.md"])

    def test_many_self_admitted_paths_cannot_hide_one(self) -> None:
        # Review 2309-913c7747 S2: the title showed ten, GitHub shows ten warnings per step and drops a summary
        # over 1 MiB, so decoys could push a path out of sight.
        _git(self.repo, "checkout", "--quiet", "-b", "many", self.base)
        names = [f"d{index:03}.md" for index in range(pm.SELF_ADMITTED_MAX_PATHS)]
        for name in names:
            (self.repo / name).write_text("decoy\n", encoding="utf-8")
        self._write_profile(["README.md", *names])
        head = self._commit()
        code, lines = pm.fork_check(self.repo, self.base, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 0, lines)
        summary = pm._summary(lines, code)
        for name in names:
            self.assertIn(f"- `{name}`", summary)
        warnings = pm.warning_annotations(lines)
        self.assertEqual(len(warnings), pm.WARNING_LIMIT)
        self.assertIn(f"and {len(names) - pm.WARNING_LIMIT + 1} more paths", warnings[-1])
        # One more is refused.
        (self.repo / "zzz.md").write_text("decoy\n", encoding="utf-8")
        self._write_profile(["README.md", *names, "zzz.md"])
        head = self._commit()
        code, lines = pm.fork_check(self.repo, self.base, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 1, lines)
        self.assertTrue(any(line.startswith("SELF_ADMITTED_LIMIT: ") for line in lines), lines)
        # A report too long for GitHub's summary is cut, never the list of admitted paths.
        long_report = lines + ["  " + "x" * 1000] * 2000
        summary = pm._summary(long_report, code)
        self.assertLess(len(summary.encode("utf-8")), 1024 * 1024)
        self.assertIn("- and 1 more (over the limit; the job log has all)", summary)
        self.assertIn("(report cut to fit the summary; the job log has all of it)", summary)
        # Over the limit, the list itself is bounded too, and the cut is measured in bytes.
        many = [f"{pm.SELF_ADMITTED}: \u00e9{index}: admitted" for index in range(5000)]
        summary = pm._summary(many + ["  " + "\u00e9" * 1000] * 2000, 1)
        self.assertLess(len(summary.encode("utf-8")), pm.SUMMARY_LIMIT)
        self.assertIn(f"- and {5000 - pm.SELF_ADMITTED_MAX_PATHS} more (over the limit", summary)

    def test_the_command_line_measures_against_the_trusted_checkouts_head(self) -> None:
        head, tip = self._lagging_head_and_stricter_tip()
        self.assertEqual(_git(self.repo, "rev-parse", "HEAD"), tip)
        calls = []
        real_fork_check = pm.fork_check

        def spy(repo: Path, base: str, candidate: str, *, tip: str | None = None) -> tuple[int, list[str]]:
            calls.append((base, candidate, tip))
            code, lines = real_fork_check(repo, base, candidate, tip=tip, audit_tool=self._stub_audit(0))
            return code, lines

        with mock.patch.object(pm, "fork_check", spy), mock.patch("sys.stdout", io.StringIO()):
            code = pm.main(["fork-check", "--repository", "example-user/Nakagawa-Recomp", "--repository-id", "42",
                            "--repo", str(self.repo), "--base", self.base, "--candidate", head])
        self.assertEqual(calls, [(self.base, head, "HEAD")])
        self.assertEqual(code, 1)

    def test_a_base_that_is_not_on_the_default_branch_is_an_input_error(self) -> None:
        head, tip = self._lagging_head_and_stricter_tip()
        with self.assertRaisesRegex(pm.CheckError, "not an ancestor of the default branch tip"):
            pm.fork_check(self.repo, head, head, tip=tip, audit_tool=self._stub_audit(0))

    def _write_manifest(self, components: list[dict[str, object]]) -> None:
        path = self.repo / pm.MANIFEST
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({"components": components}), encoding="utf-8")

    def test_the_release_manifest_and_the_paths_it_approves_are_held(self) -> None:
        # Review 2309-36f775933693 S2: the release manifest and the bytes of the paths it marks approved are
        # held in a fork pull request.
        approved: dict[str, object] = {"id": "image", "source_path": "image.gif", "disposition": "approved_binary"}
        self._write_manifest([approved])
        (self.repo / "image.gif").write_bytes(b"GIF89a" + bytes(8))
        self._write_profile(["README.md", "image.gif"])
        base = self._commit()
        pbp = b"\x00PBP\x00\x00\x01\x00" + bytes(32)
        cases: dict[str, tuple[list[dict[str, object]], dict[str, bytes], str]] = {
            "self-approval": ([approved, {"id": "x", "source_path": "x.bin", "disposition": "approved_binary"}],
                              {"x.bin": pbp}, "MANIFEST_CHANGED"),
            "approved bytes replaced": ([approved], {"image.gif": b"GIF89a" + bytes(9)},
                                        "HELD_BYTES_CHANGED: image.gif"),
        }
        for case, (components, files, code_text) in cases.items():
            with self.subTest(case=case):
                _git(self.repo, "reset", "--quiet", "--hard", base)
                self._write_manifest(components)
                for name, data in files.items():
                    (self.repo / name).write_bytes(data)
                head = self._commit()
                code, lines = pm.fork_check(self.repo, base, head, audit_tool=self._stub_audit(0))
                self.assertEqual(code, 1, lines)
                self.assertTrue(any(line.startswith(code_text) for line in lines), lines)
        # The default branch drops the approval after a pull request branched: the head must merge it.
        _git(self.repo, "reset", "--quiet", "--hard", base)
        _git(self.repo, "checkout", "--quiet", "-b", "lagging", base)
        (self.repo / "README.md").write_text("edited\n", encoding="utf-8")
        head = self._commit()
        _git(self.repo, "checkout", "--quiet", "main")
        self._write_manifest([])
        tip = self._commit()
        code, lines = pm.fork_check(self.repo, base, head, tip=tip, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 1, lines)
        self.assertTrue(any(line.startswith("MANIFEST_BEHIND") for line in lines), lines)
        # An edit elsewhere with the manifest and the approved bytes untouched passes.
        code, lines = pm.fork_check(self.repo, base, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 0, lines)

    def test_no_file_the_pull_request_adds_or_changes_may_be_binary(self) -> None:
        # Reviews 2309-913c7747 S1 and 2309-66fce005c252 S2: no file a fork pull request adds or changes may
        # be binary, as the trusted audit's own test decides it; text with the odd control character passes.
        self._write_manifest([{"id": "stb", "source_path": "LICENSE.txt", "disposition": "included"}])
        (self.repo / "LICENSE.txt").write_text("licence\n", encoding="utf-8")
        self._write_profile(["README.md", "LICENSE.txt"])
        base = self._commit()
        long_text = b"a licence with a page break\n" * 20
        for name, data, expected in (
                ("LICENSE.txt", b"licence\nmore text\n", 0), ("LICENSE.txt", long_text + b"\x0c\n" + long_text, 0),
                ("LICENSE.txt", b"tab\tand\r\n", 0), ("LICENSE.txt", b"licence\x00tail\n", 1),
                ("LICENSE.txt", b"\xff\xfe", 1), ("LICENSE.txt", b"licence" + b"\x0c" * 167 + b"\n", 1),
                ("README.md", b"readme\x00\n", 1), ("NEW.bin", b"\x00\x01\x02", 1)):
            with self.subTest(name=name, data=data[:24]):
                _git(self.repo, "reset", "--quiet", "--hard", base)
                (self.repo / name).write_bytes(data)
                head = self._commit()
                code, lines = pm.fork_check(self.repo, base, head, audit_tool=self._stub_audit(0))
                self.assertEqual(code, expected, lines)
                self.assertEqual(any(line.startswith(f"BINARY_CHANGED: {name}") for line in lines),
                                 bool(expected), lines)

    def test_the_vfpu_tables_are_held(self) -> None:
        # Review 2309-66fce005c252 S2: a table under assets/vfpu/ may not change, as text or as bytes, and none
        # may be added.
        table = self.repo / "assets" / "vfpu" / "sin.dat"
        table.parent.mkdir(parents=True, exist_ok=True)
        table.write_bytes(bytes(range(1, 64)))
        base = self._commit()
        for name, data in (("sin.dat", bytes(range(2, 65))), ("sin.dat", b"plain text now\n"),
                           ("cos.dat", b"new table text\n")):
            with self.subTest(name=name, data=data[:16]):
                _git(self.repo, "reset", "--quiet", "--hard", base)
                (table.parent / name).write_bytes(data)
                head = self._commit()
                code, lines = pm.fork_check(self.repo, base, head, audit_tool=self._stub_audit(0))
                self.assertEqual(code, 1, lines)
                self.assertIn(f"HELD_BYTES_CHANGED: assets/vfpu/{name}: the pull request adds, changes or removes "
                              "this path, whose bytes and mode only the fork maintainer's own push changes", lines)

    def test_every_held_disposition_and_a_first_file_at_a_held_path(self) -> None:
        # Review 2309-66fce005c252 answer 6 (M4, M13): each held disposition, and a file added at a path the
        # manifest holds that the merge base does not carry yet.
        for disposition in sorted(pm.HELD_PATH_DISPOSITIONS):
            with self.subTest(disposition=disposition):
                _git(self.repo, "reset", "--quiet", "--hard", self.base)
                self._write_manifest([{"id": "font", "source_path": "font.pgf", "disposition": disposition},
                                      {"id": "later", "provenance_path": "later.txt", "disposition": disposition}])
                (self.repo / "font.pgf").write_text("font\n", encoding="utf-8")
                base = self._commit()
                (self.repo / "font.pgf").write_text("font, edited\n", encoding="utf-8")
                (self.repo / "later.txt").write_text("first bytes\n", encoding="utf-8")
                head = self._commit()
                code, lines = pm.fork_check(self.repo, base, head, audit_tool=self._stub_audit(0))
                self.assertEqual(code, 1, lines)
                for path in ("font.pgf", "later.txt"):
                    self.assertTrue(any(line.startswith(f"HELD_BYTES_CHANGED: {path}: ") for line in lines), lines)

    def test_a_held_paths_mode_is_held(self) -> None:
        # Review 2309-82a49cb3d6ca N1: the executable bit on a held path.
        self._write_manifest([{"id": "image", "source_path": "image.gif", "disposition": "approved_binary"}])
        (self.repo / "image.gif").write_bytes(b"GIF89a" + bytes(8))
        base = self._commit()
        _git(self.repo, "update-index", "--chmod=+x", "image.gif")
        _git(self.repo, "commit", "--quiet", "-m", "mode")
        head = _git(self.repo, "rev-parse", "HEAD")
        code, lines = pm.fork_check(self.repo, base, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 1, lines)
        self.assertTrue(any(line.startswith("HELD_BYTES_CHANGED: image.gif: ") for line in lines), lines)

    def test_a_changed_ledger_claim_is_named_like_an_admission(self) -> None:
        # Review 2309-82a49cb3d6ca S3: rewriting the claim of a path already admitted passed with nothing named.
        def ledger(classification: str) -> None:
            (self.repo / pm.LEDGER).write_text(json.dumps({"entries": [
                {"path": "README.md", "classification": classification, "evidence": {"record_id": "r"}}]}),
                encoding="utf-8")

        ledger("upstream_derived")
        base = self._commit()
        ledger("project_authored_attested")
        head = self._commit()
        code, lines = pm.fork_check(self.repo, base, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 0, lines)
        self.assertEqual(pm.self_admitted(lines), ["README.md"])
        self.assertIn("README.md", pm._summary(lines, code).splitlines()[0])
        self.assertEqual(len(pm.warning_annotations(lines)), 1)

    def test_the_trusted_audits_binary_test_is_loaded_once(self) -> None:
        # Review 2309-82a49cb3d6ca S4 and N5: the module is registered while it runs (its dataclasses need
        # that on Python 3.14) and loaded once.
        tool = pm.ROOT / "tools" / "publish_audit.py"
        first = pm._audit_binary_test(tool)
        self.assertIs(pm._audit_binary_test(tool), first)
        self.assertIn("nakagawa_trusted_publish_audit", sys.modules)
        self.assertTrue(first(b"\x00"))
        self.assertFalse(first(b"text\n"))

    def test_a_pull_request_with_two_merge_bases_is_refused(self) -> None:
        # Review 2309-66fce005c252 S1: the check compared with one merge base, while a merge applies the pull
        # request to a tree built from all of them. A criss-cross history has two.
        def branch(name: str, file: str) -> str:
            _git(self.repo, "checkout", "--quiet", "-b", name, self.base)
            (self.repo / file).write_text(f"{name}\n", encoding="utf-8")
            return self._commit()

        first, second = branch("p", "P.md"), branch("q", "Q.md")
        _git(self.repo, "checkout", "--quiet", "main")
        _git(self.repo, "merge", "--quiet", "--no-ff", "--no-edit", "p", "q")
        tip = _git(self.repo, "rev-parse", "HEAD")
        _git(self.repo, "checkout", "--quiet", "-b", "criss", first)
        _git(self.repo, "merge", "--quiet", "--no-ff", "--no-edit", "q")
        head = _git(self.repo, "rev-parse", "HEAD")
        self.assertEqual(sorted(_git(self.repo, "merge-base", "--all", tip, head).split()), sorted([first, second]))
        code, lines = pm.fork_check(self.repo, self.base, head, tip=tip, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 1, lines)
        (finding,) = [line for line in lines if line.startswith("MULTIPLE_MERGE_BASES: ")]
        self.assertIn("have 2 merge bases", finding)
        # Merging the default branch in leaves the tip as the only merge base, and the check passes.
        _git(self.repo, "merge", "--quiet", "--no-edit", tip)
        code, lines = pm.fork_check(self.repo, self.base, _git(self.repo, "rev-parse", "HEAD"), tip=tip,
                                    audit_tool=self._stub_audit(0))
        self.assertEqual(code, 0, lines)

    def test_rule_keys_and_the_report_fence_are_shown_safely(self) -> None:
        # Review 2309-66fce005c252 answer 6 (M5, M6): a hostile profile key is escaped, and the summary's fence
        # outruns the report's backticks.
        _git(self.repo, "checkout", "--quiet", "-b", "keys", self.base)
        self._write_profile(["README.md"], **{"x\n::error::k`": 1})
        head = self._commit()
        code, lines = pm.fork_check(self.repo, self.base, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 1, lines)
        (finding,) = [line for line in lines if line.startswith("PROFILE_RULES_CHANGED: ")]
        self.assertIn('"x\\n::error::k\\u0060"', finding)
        summary = pm._summary([*lines, "  | ````"], code)
        self.assertIn("\n`````text\n", summary)
        self.assertTrue(summary.rstrip("\n").endswith("`````"))

    def test_the_name_limit_counts_characters_too(self) -> None:
        # Review 2309-66fce005c252 answer 6 (M1): few paths, long names.
        _git(self.repo, "checkout", "--quiet", "-b", "long", self.base)
        # Names this long cannot sit in every file system, so they go into the index only.
        names = [f"{index}/" + "/".join(["n" * 200] * 5) + ".md"
                 for index in range(pm.SELF_ADMITTED_MAX_CHARS // 1000 + 1)]
        self.assertLess(len(names), pm.SELF_ADMITTED_MAX_PATHS)
        blob = subprocess.run(["git", "-C", str(self.repo), "hash-object", "-w", "--stdin"], input=b"long\n",
                              capture_output=True, check=True).stdout.decode("ascii").strip()
        for name in names:
            _git(self.repo, "update-index", "--add", "--cacheinfo", f"100644,{blob},{name}")
        self._write_profile(["README.md", *names])
        _git(self.repo, "add", pm.POLICY)
        _git(self.repo, "commit", "--quiet", "-m", "long names")
        head = _git(self.repo, "rev-parse", "HEAD")
        code, lines = pm.fork_check(self.repo, self.base, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 1, lines)
        self.assertTrue(any(line.startswith("SELF_ADMITTED_LIMIT: ") for line in lines), lines)

    def test_a_head_that_lags_only_the_bases_paths_passes(self) -> None:
        _git(self.repo, "checkout", "--quiet", "-b", "topic", self.base)
        (self.repo / "TOPIC.md").write_text("topic\n", encoding="utf-8")
        self._write_profile(["README.md", "TOPIC.md"])
        head = self._commit()
        _git(self.repo, "checkout", "--quiet", "main")
        (self.repo / "LATER.md").write_text("later\n", encoding="utf-8")
        self._write_profile(["LATER.md", "README.md"])
        tip = self._commit()
        code, lines = pm.fork_check(self.repo, tip, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 0, lines)

    def test_unrelated_histories_are_an_input_error(self) -> None:
        _git(self.repo, "checkout", "--quiet", "--orphan", "island")
        head = self._commit()
        with self.assertRaisesRegex(pm.CheckError, "no common ancestor"):
            pm.fork_check(self.repo, self.base, head, audit_tool=self._stub_audit(0))

    def test_a_failing_publication_audit_fails_the_check(self) -> None:
        code, lines = pm.fork_check(self.repo, self.base, self.base, audit_tool=self._stub_audit(1))
        self.assertEqual(code, 1)
        self.assertEqual(lines[-1], f"{pm.FORK_CONTEXT}: FAIL")

    def test_a_candidate_workflow_answering_to_a_check_name_fails(self) -> None:
        for context in (pm.UPSTREAM_CONTEXT, pm.FORK_CONTEXT):
            with self.subTest(context=context):
                workflow = self.repo / ".github" / "workflows" / "spoof.yml"
                workflow.parent.mkdir(parents=True, exist_ok=True)
                workflow.write_text(f"jobs:\n  a:\n    name: {context}\n", encoding="utf-8")
                head = self._commit()
                code, lines = pm.fork_check(self.repo, self.base, head, audit_tool=self._stub_audit(0))
                self.assertEqual(code, 1)
                self.assertTrue(any(line.startswith("CI_CONTEXT_COLLISION: .github/workflows/spoof.yml")
                                    for line in lines), lines)

    def test_the_gate_workflow_may_name_both_checks_but_not_be_changed(self) -> None:
        workflow = self.repo / pm.WORKFLOW
        workflow.parent.mkdir(parents=True, exist_ok=True)
        workflow.write_bytes(f"name: {pm.UPSTREAM_CONTEXT}\nname: {pm.FORK_CONTEXT}\n".encode())
        base = self._commit()
        # Unchanged since the branch point: the trusted copy, which names both checks.
        (self.repo / "README.md").write_text("fork, edited\n", encoding="utf-8")
        head = self._commit()
        code, lines = pm.fork_check(self.repo, base, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 0, lines)
        # Failing-before: the gate workflow was skipped, so a pull request could add a job under
        # the check's own name to it and post a green result beside the real one.
        cases = {
            "edited": f"name: {pm.UPSTREAM_CONTEXT}\nname: {pm.FORK_CONTEXT}\nname: {pm.FORK_CONTEXT}\n".encode(),
            "deleted": None,
        }
        for case, content in cases.items():
            with self.subTest(case=case):
                _git(self.repo, "reset", "--quiet", "--hard", base)
                if content is None:
                    workflow.unlink()
                else:
                    workflow.write_bytes(content)
                head = self._commit()
                code, lines = pm.fork_check(self.repo, base, head, audit_tool=self._stub_audit(0))
                self.assertEqual(code, 1, lines)
                self.assertTrue(any(line.startswith(f"CI_GATE_WORKFLOW_CHANGED: {pm.WORKFLOW}")
                                    for line in lines), lines)
                self.assertEqual(lines[-1], f"{pm.FORK_CONTEXT}: FAIL")

    def test_a_gate_workflow_changed_only_on_the_base_is_not_the_candidates(self) -> None:
        workflow = self.repo / pm.WORKFLOW
        workflow.parent.mkdir(parents=True, exist_ok=True)
        workflow.write_bytes(b"name: gate v1\n")
        branch_point = self._commit()
        _git(self.repo, "checkout", "--quiet", "-b", "topic", branch_point)
        (self.repo / "README.md").write_text("topic\n", encoding="utf-8")
        head = self._commit()
        _git(self.repo, "checkout", "--quiet", "main")
        workflow.write_bytes(b"name: gate v2\n")
        tip = self._commit()
        code, lines = pm.fork_check(self.repo, tip, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 0, lines)

    def test_the_real_audit_runs_against_the_candidate_tree(self) -> None:
        # The default audit tool is the trusted checkout's publish_audit, given the candidate commit.
        calls = []
        real_run = subprocess.run

        def fake_run(command, **kwargs):
            calls.append(command)
            return subprocess.CompletedProcess(command, 0, "publication audit: OK\n", "")

        with mock.patch.object(pm.subprocess, "run", side_effect=lambda cmd, **kw: (
                fake_run(cmd, **kw) if cmd[0] == sys.executable else real_run(cmd, **kw))):
            code, _ = pm.fork_check(self.repo, self.base, self.base)
        self.assertEqual(code, 0)
        (command,) = calls
        self.assertEqual(Path(command[1]), ROOT / "tools" / "publish_audit.py")
        self.assertEqual(command[2:6], ["--repo-root", str(self.repo), "--committed-tree", self.base])
        self.assertEqual(command[6], "--policy")
        self.assertEqual(command[8:], ["--public-scope", "--provenance-self-consistency"])


class ForkCheckRealAuditTest(unittest.TestCase):
    """The fork check with the real publication audit, in the layout hosted CI gives it.

    Every test above stubs the audit. Here the candidate is this checkout's own committed tree and the
    trusted tools are this checkout's. A scratch repository borrows the objects, so nothing is checked out
    and nothing here is written to.

    * An admission inside the pull request passes: the candidate is judged by its own profile.
    * One ``.gitattributes`` line does not change which files are judged.

    Each case runs the whole audit once, over every tracked file.
    """

    ADMITTED = "tools/provenance_mode.py"
    repo: Path
    base: str
    head_tree: str

    @classmethod
    def setUpClass(cls) -> None:
        try:
            head = _git(ROOT, "rev-parse", "--verify", "--quiet", "HEAD^{commit}")
            objects = Path(_git(ROOT, "rev-parse", "--path-format=absolute", "--git-path", "objects"))
        except subprocess.CalledProcessError as error:
            raise unittest.SkipTest("needs this checkout's Git objects") from error
        scratch = tempfile.TemporaryDirectory(prefix="nk-fork-real-")
        cls.addClassCleanup(scratch.cleanup)
        cls.repo = Path(scratch.name) / "fork"
        cls.repo.mkdir()
        _git(cls.repo, "init", "--quiet", "--initial-branch=main")
        alternates = cls.repo / ".git" / "objects" / "info" / "alternates"
        # Bytes, not text: on Windows write_text ends the line with CRLF, and Git then reads a path
        # ending in CR and borrows nothing.
        alternates.write_bytes((objects.as_posix() + "\n").encode("utf-8"))
        cls.head_tree = _git(cls.repo, "rev-parse", f"{head}^{{tree}}")
        profile = json.loads(_git(cls.repo, "cat-file", "blob", f"{cls.head_tree}:{pm.POLICY}"))
        assert cls.ADMITTED in profile["include_paths"]
        profile["include_paths"] = [path for path in profile["include_paths"] if path != cls.ADMITTED]
        base_profile = json.dumps(profile, indent=2) + "\n"
        # The base: the default branch before the admission. Only its profile is read from it.
        cls.base = cls._commit_tree(cls._tree_with({pm.POLICY: base_profile.encode("utf-8")}))
        # The hosted layout: the checkout the job runs in is the default branch, so the profile on disk
        # is the base's, without the admitted path.
        on_disk = cls.repo / pm.POLICY
        on_disk.parent.mkdir(parents=True)
        on_disk.write_text(base_profile, encoding="utf-8")

    @classmethod
    def _tree_with(cls, files: dict[str, bytes]) -> str:
        """This checkout's committed tree with ``files`` written over it, as a tree object."""
        index = cls.repo / ".git" / "probe-index"
        index.unlink(missing_ok=True)
        env = os.environ | {"GIT_INDEX_FILE": str(index)}

        def git(*args: str, data: bytes | None = None) -> str:
            return subprocess.run(["git", "-C", str(cls.repo), *args], check=True, capture_output=True,
                                  input=data, env=env).stdout.decode("utf-8").strip()

        git("read-tree", cls.head_tree)
        for path, data in files.items():
            blob = git("hash-object", "-w", "--stdin", data=data)
            git("update-index", "--add", "--cacheinfo", f"100644,{blob},{path}")
        return git("write-tree")

    @classmethod
    def _commit_tree(cls, tree: str, parent: str | None = None) -> str:
        env = os.environ | {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid",
                            "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.invalid"}
        parents = ["-p", parent] if parent else []
        return subprocess.run(["git", "-C", str(cls.repo), "commit-tree", tree, *parents, "-m", "probe"],
                              check=True, capture_output=True, text=True, env=env).stdout.strip()

    def test_an_admission_inside_the_pull_request_passes(self) -> None:
        candidate = self._commit_tree(self.head_tree, self.base)
        code, lines = pm.fork_check(self.repo, self.base, candidate)
        self.assertEqual(code, 0, "\n".join(lines))
        self.assertIn("profile: 1 path(s) added, 0 removed", lines)
        self.assertIn(f"  added {self.ADMITTED}", lines)
        self.assertEqual(lines[-1], f"{pm.FORK_CONTEXT}: PASS")
        # Review 2309-36f775933693 S3: the pass names what the pull request admitted itself.
        self.assertIn(f"{pm.SELF_ADMITTED}: {self.ADMITTED}", "\n".join(lines))

    def test_the_manifest_and_the_paths_it_approves_are_held_with_the_real_audit(self) -> None:
        # Review 2309-36f775933693 S2, with the real audit: a new file with its own manifest entry, and new
        # bytes at a path the manifest approves, each fail in a fork pull request.
        manifest = json.loads(_git(self.repo, "cat-file", "blob", f"{self.head_tree}:{pm.MANIFEST}"))
        approved = pm.held_paths(json.dumps(manifest).encode("utf-8"))
        image = next(path for path in approved if path.endswith(".gif"))
        manifest["components"].append({"id": "eboot", "name": "EBOOT", "type": "asset", "source_path": "EBOOT.PBP",
                                       "license": "CC0-1.0", "disposition": "approved_binary",
                                       "public_scope_included": True})
        profile = json.loads(_git(self.repo, "cat-file", "blob", f"{self.head_tree}:{pm.POLICY}"))
        profile["include_paths"] = sorted([*profile["include_paths"], "EBOOT.PBP"])
        pbp = b"\x00PBP\x00\x00\x01\x00" + bytes(64)
        cases = {
            "self-approved EBOOT.PBP": ({"EBOOT.PBP": pbp,
                                         pm.MANIFEST: (json.dumps(manifest, indent=2) + "\n").encode("utf-8"),
                                         pm.POLICY: (json.dumps(profile, indent=2) + "\n").encode("utf-8")},
                                        "MANIFEST_CHANGED"),
            "approved image edited": ({image: b"GIF89a" + bytes(16)}, f"HELD_BYTES_CHANGED: {image}"),
        }
        for case, (files, expected) in cases.items():
            with self.subTest(case=case):
                code, lines = pm.fork_check(self.repo, self.base, self._commit_tree(self._tree_with(files), self.base))
                self.assertEqual(code, 1, "\n".join(lines))
                self.assertTrue(any(line.startswith(expected) for line in lines), "\n".join(lines[-30:]))
                self.assertEqual(lines[-1], f"{pm.FORK_CONTEXT}: FAIL")

    def test_an_export_ignore_line_does_not_hide_a_forbidden_file(self) -> None:
        attributes = _git(self.repo, "cat-file", "blob", f"{self.head_tree}:.gitattributes")
        tree = self._tree_with({
            "EBOOT.PBP": b"\x00PBP\x00\x00\x01\x00" + bytes(64),
            ".gitattributes": (attributes + "\nEBOOT.PBP export-ignore\n").encode("utf-8"),
        })
        code, lines = pm.fork_check(self.repo, self.base, self._commit_tree(tree, self.base))
        self.assertEqual(code, 1, "\n".join(lines))
        self.assertTrue(any("MAGIC: EBOOT.PBP" in line for line in lines), "\n".join(lines[-20:]))
        self.assertEqual(lines[-1], f"{pm.FORK_CONTEXT}: FAIL")


if __name__ == "__main__":
    unittest.main()
