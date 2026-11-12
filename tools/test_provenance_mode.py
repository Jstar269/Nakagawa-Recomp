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
        # The ID is compared as a number: a zero-padded or integer upstream ID is the upstream.
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
        # The job's permissions are read-only, and the check's exit code decides the job.
        self.assertIn("    permissions:\n      contents: read\n      pull-requests: read\n    steps:\n", self.fork)
        step = self.fork[self.fork.index("      - name: Check the candidate with the trusted publication audit\n"):]
        self.assertIn("          set -euo pipefail\n", step)
        # The default branch is the trusted checkout's HEAD, not the event's base.
        self.assertIn('          python tools/provenance_mode.py fork-check --repo "${GITHUB_WORKSPACE}" \\\n'
                      '            --base "${BASE_SHA}" --tip HEAD --candidate "${HEAD_SHA}" '
                      '--summary "${GITHUB_STEP_SUMMARY}"\n',
                      step)
        for tolerant in ("|| true", "|| :", "continue-on-error", "if: always()", "; exit 0"):
            self.assertNotIn(tolerant, step)

    def test_check_names_are_unique_in_the_workflow(self) -> None:
        self.assertEqual(self.text.count(f"    name: {pm.UPSTREAM_CONTEXT}\n"), 1)
        self.assertEqual(self.text.count(f"    name: {pm.FORK_CONTEXT}\n"), 1)

    def test_the_fork_job_reads_the_candidate_as_data_only(self) -> None:
        # The one checkout is the default branch without stored
        # credentials, the fetched head must be the named one, and the candidate is never checked out.
        code = _code(self.fork)
        self.assertEqual(code.count("uses: actions/checkout@"), 1)
        checkout = code[code.index("      - name: Check out the trusted default branch\n"):
                        code.index("      - name: Set up Python\n")]
        self.assertIn("          ref: ${{ github.event.repository.default_branch }}\n", checkout)
        self.assertIn("          persist-credentials: false\n", checkout)
        self.assertEqual(code.count("persist-credentials:"), 1)
        fetch = code[code.index("      - name: Fetch the candidate objects without checking them out\n"):
                     code.index("      - name: Check the candidate with the trusted publication audit\n")]
        self.assertIn('          fetched="$(git rev-parse refs/candidate/head)"\n'
                      '          if [ "${fetched}" != "${HEAD_SHA}" ]; then\n'
                      '            echo "candidate head moved during this run: event named ${HEAD_SHA}, fetched '
                      '${fetched}" >&2\n'
                      '            exit 1\n'
                      '          fi\n', fetch)
        self.assertEqual(code.count("refs/candidate/head"), 2)
        for verb in ("checkout", "switch", "worktree", "restore", "reset", "read-tree", "stash", "merge ", "pull"):
            self.assertNotRegex(code, rf"\bgit {verb}", verb)


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
        # A pass that covers a pull request's own admission says so, by path.
        self.assertEqual(pm.self_admitted(lines), ["NEW.md"])
        self.assertTrue(pm._summary(lines, code).startswith(
            f"### {pm.FORK_CONTEXT}: passed for head `{head}` against default branch tip `{self.base}`; this pull "
            "request admits 1 path(s) itself: `NEW.md`\n"))
        self.assertEqual(len(pm.warning_annotations(lines)), 1)
        self.assertTrue(pm.warning_annotations(lines)[0].startswith("::warning title="))
        self.assertIn("::NEW.md is admitted by this pull request itself", pm.warning_annotations(lines)[0])
        # A pull request that admits nothing gets a plain title and no warning.
        code, lines = pm.fork_check(self.repo, self.base, self.base, audit_tool=self._stub_audit(0))
        self.assertEqual(pm.self_admitted(lines), [])
        self.assertTrue(pm._summary(lines, code).startswith(
            f"### {pm.FORK_CONTEXT}: passed for head `{self.base}` against default branch tip `{self.base}`\n"))
        self.assertEqual(pm.warning_annotations(lines), [])

    def test_the_audit_judges_the_candidate_by_the_candidates_own_profile(self) -> None:
        # The audit judges by the candidate's profile, not by the checkout it runs in. On hosted CI that
        # checkout is the default branch, whose profile lacks the path a pull request admits.
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

    def test_a_head_without_the_tip_is_refused(self) -> None:
        # A head must contain the tip before anything else is judged; this one only adds a path, and the tip
        # only admits another and adds an exclusion.
        _git(self.repo, "checkout", "--quiet", "-b", "topic", self.base)
        (self.repo / "TOPIC.md").write_text("topic\n", encoding="utf-8")
        self._write_profile(["README.md", "TOPIC.md"])
        head = self._commit()
        _git(self.repo, "checkout", "--quiet", "main")
        (self.repo / "LATER.md").write_text("later\n", encoding="utf-8")
        self._write_profile(["LATER.md", "README.md"], exclude_paths=["x"])
        tip = self._commit()
        code, lines = pm.fork_check(self.repo, tip, head, audit_tool=self._stub_audit(0))
        # Without the tip the head fails, whatever else it changes.
        self.assertEqual(code, 1, lines)
        self.assertEqual(lines[3:], [f"NOT_UP_TO_DATE: the pull request does not contain the default branch's tip "
                                     f"{tip}; merge the default branch into it", f"{pm.FORK_CONTEXT}: FAIL"])
        # Merged with the tip, the head carries the tip's rules and passes.
        _git(self.repo, "checkout", "--quiet", "topic")
        _git(self.repo, "merge", "--quiet", "--no-edit", "-X", "theirs", "main")
        self._write_profile(["LATER.md", "README.md", "TOPIC.md"], exclude_paths=["x"])
        merged = self._commit()
        code, lines = pm.fork_check(self.repo, tip, merged, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 0, lines)
        self.assertIn("profile: 1 path(s) added, 0 removed", lines)
        self.assertIn("  added TOPIC.md", lines)
        self.assertEqual(self._policy_seen(lines)["include_paths"], ["LATER.md", "README.md", "TOPIC.md"])

    def _lagging_head_and_stricter_tip(self) -> tuple[str, str]:
        _git(self.repo, "checkout", "--quiet", "-b", "topic", self.base)
        (self.repo / "TOPIC.md").write_text("topic\n", encoding="utf-8")
        self._write_profile(["README.md", "TOPIC.md"])
        head = self._commit()
        _git(self.repo, "checkout", "--quiet", "main")
        self._write_profile(["README.md"], exclude_paths=["x"])
        return head, self._commit()

    def test_a_stale_event_base_does_not_hide_a_rule_tightened_on_the_tip(self) -> None:
        # The event's base can lag the default branch and equal the merge base. The tip is the measure, so
        # a rule tightened on the tip is compared with the head.
        head, tip = self._lagging_head_and_stricter_tip()
        code, lines = pm.fork_check(self.repo, self.base, head, tip=tip, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 1, lines)
        self.assertIn(f"default branch tip {tip}", lines)
        self.assertTrue(any(line.startswith(f"NOT_UP_TO_DATE: the pull request does not contain the default branch's "
                                            f"tip {tip}") for line in lines), lines)
        # Given the stale base alone, the check still measures against the tip and fails.
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
        # Workflow commands resume even when the check ends early.
        out = io.StringIO()
        with mock.patch("sys.stdout", out):
            code = pm.main(["fork-check", "--repository", "example-user/Nakagawa-Recomp", "--repository-id", "42",
                            "--repo", str(self.repo), "--base", "0" * 40, "--candidate", self.base])
        self.assertEqual(code, 2)
        printed = out.getvalue().splitlines()
        stop = re.fullmatch(r"::stop-commands::([0-9a-f]{32})", printed[0])
        assert stop is not None
        self.assertEqual(printed[-1], f"::{stop.group(1)}::")
        # The token differs on every run, and an unexpected error is printed before the resume line.
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
        # An entry added to include_paths names a file of the pull request, so a later pull request cannot add
        # the file unnamed.
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
        # GitHub shows ten warnings per step and drops a summary over 1 MiB, so the number of paths a pull
        # request admits itself is limited and every one stays in view.
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
        pair = ["base x", f"default branch tip {'a' * 40}", f"candidate {'b' * 40}"]
        many = [*pair, *(f"{pm.SELF_ADMITTED}: \u00e9{index}: admitted" for index in range(5000))]
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
        # The release manifest and the bytes of the paths it marks approved are held in a fork pull request.
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
        # A change to the manifest's text alone, with the same components and nothing else changed, fails the
        # check on its own: the manifest is held byte for byte.
        _git(self.repo, "reset", "--quiet", "--hard", base)
        (self.repo / pm.MANIFEST).write_text(json.dumps({"components": [approved]}, indent=2), encoding="utf-8")
        head = self._commit()
        code, lines = pm.fork_check(self.repo, base, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 1, lines)
        self.assertEqual([line.split(": ", 1)[0] for line in lines if re.match(r"[A-Z][A-Z_]+: ", line)],
                         ["MANIFEST_CHANGED"], lines)
        self.assertEqual(lines[-1], f"{pm.FORK_CONTEXT}: FAIL")
        # An edit elsewhere with the manifest and the approved bytes untouched passes.
        _git(self.repo, "reset", "--quiet", "--hard", base)
        (self.repo / "README.md").write_text("edited\n", encoding="utf-8")
        head = self._commit()
        code, lines = pm.fork_check(self.repo, base, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 0, lines)

    def test_no_file_the_pull_request_adds_or_changes_may_be_binary(self) -> None:
        # No file a fork pull request adds or changes may be binary, as the trusted audit's own test decides
        # it; text with the odd control character passes.
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
        # A table under assets/vfpu/ may not change, as text or as bytes, and none may be added.
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
        # Each held disposition, and a file added at a path the manifest holds that the tip does not carry yet.
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
        # The executable bit on a held path is held.
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
        # A changed ledger claim for a path already admitted is named like an admission.
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
        # A claim the default branch did not have, for a path it admits, is named the same way.
        (self.repo / pm.LEDGER).write_text(json.dumps({"entries": []}), encoding="utf-8")
        bare = self._commit()
        ledger("project_authored_attested")
        claimed = self._commit()
        code, lines = pm.fork_check(self.repo, bare, claimed, audit_tool=self._stub_audit(0))
        self.assertEqual((code, pm.self_admitted(lines)), (0, ["README.md"]), lines)
        # A path the pull request removes, with its claim, is listed as removed and not named.
        (self.repo / "OTHER.md").write_text("other\n", encoding="utf-8")
        self._write_profile(["OTHER.md", "README.md"])
        (self.repo / pm.LEDGER).write_text(json.dumps({"entries": [
            {"path": "OTHER.md", "classification": "upstream_derived", "evidence": {"record_id": "o"}}]}),
            encoding="utf-8")
        both = self._commit()
        self._write_profile(["README.md"])
        (self.repo / pm.LEDGER).write_text(json.dumps({"entries": []}), encoding="utf-8")
        dropped = self._commit()
        code, lines = pm.fork_check(self.repo, both, dropped, audit_tool=self._stub_audit(0))
        self.assertEqual((code, pm.self_admitted(lines)), (0, []), lines)
        self.assertIn("  removed OTHER.md", lines)

    def test_the_trusted_audits_binary_test_is_loaded_once(self) -> None:
        # The module is registered while it runs (its dataclasses need
        # that on Python 3.14) and loaded once.
        tool = pm.ROOT / "tools" / "publish_audit.py"
        first = pm._audit_binary_test(tool)
        self.assertIs(pm._audit_binary_test(tool), first)
        self.assertIn("nakagawa_trusted_publish_audit", sys.modules)
        self.assertTrue(first(b"\x00"))
        self.assertFalse(first(b"text\n"))

    def test_a_criss_cross_head_is_refused_until_it_contains_the_tip(self) -> None:
        # A merge applies the pull request to a tree built from every merge base. A criss-cross history has
        # two and lacks the tip, so it is refused until it contains the tip, which is then the only one.
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
        self.assertTrue(any(line.startswith("NOT_UP_TO_DATE: ") for line in lines), lines)
        # Merging the default branch in leaves the tip as the only merge base, and the check passes.
        _git(self.repo, "merge", "--quiet", "--no-edit", tip)
        code, lines = pm.fork_check(self.repo, self.base, _git(self.repo, "rev-parse", "HEAD"), tip=tip,
                                    audit_tool=self._stub_audit(0))
        self.assertEqual(code, 0, lines)

    def test_rule_keys_and_the_report_fence_are_shown_safely(self) -> None:
        # A hostile profile key is escaped, and the summary's fence
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

    def test_a_rule_value_of_another_json_type_is_a_change(self) -> None:
        # Values of different JSON types differ (``1``, ``1.0``, ``true``).
        for before, after in ((1, True), (1, 1.0), (0, False)):
            with self.subTest(before=before, after=after):
                _git(self.repo, "checkout", "--quiet", "--detach", self.base)
                self._write_profile(["README.md"], strict=before)
                base = self._commit()
                self._write_profile(["README.md"], strict=after)
                head = self._commit()
                code, lines = pm.fork_check(self.repo, base, head, audit_tool=self._stub_audit(0))
                self.assertEqual(code, 1, lines)
                self.assertTrue(any(line.startswith("PROFILE_RULES_CHANGED: ") and "strict" in line
                                    for line in lines), lines)

    def test_the_name_limit_counts_characters_too(self) -> None:
        # Few paths, long names.
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

    def _admit_in_the_index(self, names: list[str]) -> str:
        """A head that admits ``names`` itself, each with a file; the files go into the index only."""
        _git(self.repo, "reset", "--quiet", "--hard", self.base)
        blob = subprocess.run(["git", "-C", str(self.repo), "hash-object", "-w", "--stdin"], input=b"named\n",
                              capture_output=True, check=True).stdout.decode("ascii").strip()
        for name in names:
            _git(self.repo, "update-index", "--add", "--cacheinfo", f"100644,{blob},{name}")
        self._write_profile(["README.md", *names])
        _git(self.repo, "add", pm.POLICY)
        _git(self.repo, "commit", "--quiet", "-m", "names")
        return _git(self.repo, "rev-parse", "HEAD")

    def test_the_name_limits_are_exact(self) -> None:
        # At most SELF_ADMITTED_MAX_PATHS paths and SELF_ADMITTED_MAX_CHARS characters of names: the limit
        # itself passes, one more path or one more character fails.
        def name(index: int, length: int) -> str:
            # Plain characters, so a name is shown as it is; a separator every 200 characters.
            text = list(f"c{index:02}/" + "n" * (length - 4))
            for position in range(200, len(text) - 1, 200):
                text[position] = "/"
            return "".join(text)

        per_name = 1000
        cases = {
            "paths at the limit": ([f"p{index:03}.md" for index in range(pm.SELF_ADMITTED_MAX_PATHS)], 0),
            "one path more": ([f"p{index:03}.md" for index in range(pm.SELF_ADMITTED_MAX_PATHS + 1)], 1),
        }
        for extra, expected in ((0, 0), (1, 1)):
            count = pm.SELF_ADMITTED_MAX_CHARS // per_name
            names = [name(index, per_name) for index in range(count - 1)]
            names.append(name(count - 1, pm.SELF_ADMITTED_MAX_CHARS - per_name * (count - 1) + extra))
            self.assertEqual(sum(map(len, names)), pm.SELF_ADMITTED_MAX_CHARS + extra)
            self.assertEqual([pm.shown_path(item) for item in names], names)
            cases[f"characters at the limit plus {extra}"] = (names, expected)
        for case, (names, expected) in cases.items():
            with self.subTest(case=case):
                head = self._admit_in_the_index(names)
                code, lines = pm.fork_check(self.repo, self.base, head, audit_tool=self._stub_audit(0))
                self.assertEqual(code, expected, lines)
                self.assertEqual(any(line.startswith("SELF_ADMITTED_LIMIT: ") for line in lines), bool(expected),
                                 lines)
                self.assertEqual(len(pm.self_admitted(lines)), len(names))

    def test_re_claimed_paths_count_towards_the_name_limit(self) -> None:
        # A changed ledger claim is named like an admission, and counts towards the same limit.
        names = [f"r{index:03}.md" for index in range(pm.SELF_ADMITTED_MAX_PATHS + 1)]
        for name in names:
            (self.repo / name).write_text("claimed\n", encoding="utf-8")
        self._write_profile(["README.md", *names])

        def ledger(classification: str) -> None:
            (self.repo / pm.LEDGER).write_text(json.dumps({"entries": [
                {"path": name, "classification": classification, "evidence": {"record_id": name}}
                for name in names]}), encoding="utf-8")

        ledger("upstream_derived")
        base = self._commit()
        ledger("project_authored_attested")
        head = self._commit()
        code, lines = pm.fork_check(self.repo, base, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 1, lines)
        self.assertTrue(any(line.startswith("SELF_ADMITTED_LIMIT: ") for line in lines), lines)
        self.assertEqual(pm.self_admitted(lines), names)

    def test_a_profile_without_an_include_paths_list_is_an_input_error(self) -> None:
        for paths in ("README.md", ["README.md", 1]):
            with self.subTest(paths=paths):
                _git(self.repo, "reset", "--quiet", "--hard", self.base)
                (self.repo / pm.POLICY).write_text(json.dumps({"name": "public-safe-v1", "include_paths": paths}),
                                                   encoding="utf-8")
                head = self._commit()
                with self.assertRaisesRegex(pm.CheckError, "has no include_paths list"):
                    pm.fork_check(self.repo, self.base, head, audit_tool=self._stub_audit(0))

    def test_the_summary_names_only_the_pair_its_report_opens_with(self) -> None:
        tip, head = "a" * 40, "b" * 40
        good = ["base x", f"default branch tip {tip}", f"candidate {head}", f"{pm.FORK_CONTEXT}: PASS"]
        self.assertIn(f"for head `{head}` against default branch tip `{tip}`", pm._summary(good, 0))
        # Each line below has a SHA where the tip's or the head's would be, under another label, or the right
        # label with something that is not a full commit SHA.
        cases = {
            "another label for the tip": [good[0], f"default branch was {tip}", good[2]],
            "another label for the head": [good[0], good[1], f"head was: {head}"],
            "too short": [good[0], good[1]],
            "a name, not a SHA": [good[0], "default branch tip HEAD", good[2]],
            "upper case": [good[0], good[1], f"candidate {head.upper()}"],
            "39 digits": [good[0], f"default branch tip {tip[:39]}", good[2]],
        }
        for case, lines in cases.items():
            with self.subTest(case=case), self.assertRaises(pm.CheckError):
                pm._summary([*lines, f"{pm.FORK_CONTEXT}: PASS"], 0)

    def test_the_summarys_title_counts_the_paths_it_does_not_show(self) -> None:
        lines = ["base x", f"default branch tip {'a' * 40}", f"candidate {'b' * 40}",
                 *(f"{pm.SELF_ADMITTED}: p{index:02}.md: admitted" for index in range(11)), f"{pm.FORK_CONTEXT}: PASS"]
        title = pm._summary(lines, 0).splitlines()[0]
        self.assertIn("admits 11 path(s) itself: ", title)
        self.assertIn("`p09.md` and 1 more", title)
        self.assertNotIn("p10.md", title)

    def test_a_default_branch_manifest_that_cannot_be_read_is_an_input_error(self) -> None:
        for text, message in (("{", "is unreadable"), ('{"components": {}}', "has no components list")):
            with self.subTest(text=text):
                _git(self.repo, "reset", "--quiet", "--hard", self.base)
                (self.repo / pm.MANIFEST).parent.mkdir(parents=True, exist_ok=True)
                (self.repo / pm.MANIFEST).write_text(text, encoding="utf-8")
                tip = self._commit()
                (self.repo / "README.md").write_text("edited\n", encoding="utf-8")
                head = self._commit()
                with self.assertRaisesRegex(pm.CheckError, message):
                    pm.fork_check(self.repo, tip, head, audit_tool=self._stub_audit(0))

    def test_a_link_replaced_by_a_binary_file_is_a_changed_binary(self) -> None:
        # A change of type (a link that becomes a file) is a change like any other.
        def blob(data: bytes) -> str:
            return subprocess.run(["git", "-C", str(self.repo), "hash-object", "-w", "--stdin"], input=data,
                                  capture_output=True, check=True).stdout.decode("ascii").strip()

        _git(self.repo, "update-index", "--add", "--cacheinfo", f"120000,{blob(b'README.md')},link")
        _git(self.repo, "commit", "--quiet", "-m", "link")
        tip = _git(self.repo, "rev-parse", "HEAD")
        _git(self.repo, "update-index", "--cacheinfo", f"100644,{blob(bytes(range(32)))},link")
        _git(self.repo, "commit", "--quiet", "-m", "binary")
        head = _git(self.repo, "rev-parse", "HEAD")
        self.assertEqual(_git(self.repo, "diff-tree", "--name-status", "-r", tip, head), "T\tlink")
        code, lines = pm.fork_check(self.repo, tip, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 1, lines)
        self.assertTrue(any(line.startswith("BINARY_CHANGED: link: ") for line in lines), lines)

    def test_a_commit_that_is_not_a_sha1_commit_is_an_input_error(self) -> None:
        # The check and its summary name commits by 40-digit SHA-1; a repository with longer IDs is refused.
        with tempfile.TemporaryDirectory(prefix="nk-fork-sha256-") as scratch:
            repo = Path(scratch)
            _git(repo, "init", "--quiet", "--initial-branch=main", "--object-format=sha256")
            (repo / "README.md").write_text("fork\n", encoding="utf-8")
            _git(repo, "add", "-A")
            _git(repo, "commit", "--quiet", "-m", "base")
            with self.assertRaisesRegex(pm.CheckError, "is not a commit"):
                pm.fork_check(repo, "HEAD", "HEAD", audit_tool=self._stub_audit(0))

    def test_unrelated_histories_are_not_up_to_date(self) -> None:
        _git(self.repo, "checkout", "--quiet", "--orphan", "island")
        head = self._commit()
        code, lines = pm.fork_check(self.repo, self.base, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 1, lines)
        self.assertTrue(any(line.startswith("NOT_UP_TO_DATE: ") for line in lines), lines)

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
        # The gate workflow is held to the tip's copy: a pull request may not add a job under the check's
        # own name to it.
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
        self.assertTrue(any(line.startswith("NOT_UP_TO_DATE: ") for line in lines), lines)
        # With the tip merged in, the head carries the tip's workflow and changes nothing of it.
        _git(self.repo, "checkout", "--quiet", "topic")
        _git(self.repo, "merge", "--quiet", "--no-edit", "main")
        code, lines = pm.fork_check(self.repo, tip, _git(self.repo, "rev-parse", "HEAD"),
                                    audit_tool=self._stub_audit(0))
        self.assertEqual(code, 0, lines)

    def test_the_real_audit_runs_against_the_candidate_tree(self) -> None:
        # The default audit tool is the trusted checkout's publish_audit, given the candidate commit.
        # The candidate differs from the base and the tip, so the two cannot be confused.
        (self.repo / "README.md").write_text("fork, edited\n", encoding="utf-8")
        head = self._commit()
        self.assertNotEqual(head, self.base)
        calls = []
        real_run = subprocess.run

        def fake_run(command, **kwargs):
            calls.append(command)
            return subprocess.CompletedProcess(command, 0, "publication audit: OK\n", "")

        with mock.patch.object(pm.subprocess, "run", side_effect=lambda cmd, **kw: (
                fake_run(cmd, **kw) if cmd[0] == sys.executable else real_run(cmd, **kw))):
            code, _ = pm.fork_check(self.repo, self.base, head, tip=self.base)
        self.assertEqual(code, 0)
        (command,) = calls
        self.assertEqual(Path(command[1]), ROOT / "tools" / "publish_audit.py")
        self.assertEqual(command[2:6], ["--repo-root", str(self.repo), "--committed-tree", head])
        self.assertEqual(command[6], "--policy")
        self.assertEqual(command[8:], ["--public-scope", "--provenance-self-consistency"])

    # The pair the check judges: one head against one tip.

    def _gate_workflow(self, text: str) -> None:
        workflow = self.repo / pm.WORKFLOW
        workflow.parent.mkdir(parents=True, exist_ok=True)
        workflow.write_text(text, encoding="utf-8")

    def test_the_pair_limit_is_documented_not_closed_a_pass_names_its_tip_only(self) -> None:
        # The pair limit, a stated limit (docs/FORKING.md, "What a pass covers"): the same
        # head passes against the tip it was judged at and fails against a later tip it already contains. GitHub
        # keeps the first result on the head; the summary names the tip, and the fork maintainer runs the check
        # again after their own push.
        self._gate_workflow("name: gate v1\n")
        t1 = self._commit()
        self._gate_workflow("name: gate v2\n")
        self._write_profile(["README.md"], exclude_paths=["x"])
        later = self._commit()
        self._gate_workflow("name: gate v1\n")
        self._write_profile(["README.md"])
        (self.repo / "README.md").write_text("fork, one line more\n", encoding="utf-8")
        head = self._commit()
        self.assertEqual(_git(self.repo, "merge-base", "--is-ancestor", later, head), "")
        code, lines = pm.fork_check(self.repo, t1, head, tip=t1, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 0, lines)
        self.assertTrue(pm._summary(lines, code).startswith(
            f"### {pm.FORK_CONTEXT}: passed for head `{head}` against default branch tip `{t1}`\n"))
        code, lines = pm.fork_check(self.repo, t1, head, tip=later, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 1, lines)
        self.assertTrue(any(line.startswith(f"CI_GATE_WORKFLOW_CHANGED: {pm.WORKFLOW}") for line in lines), lines)
        self.assertTrue(any(line.startswith("PROFILE_RULES_CHANGED: ") for line in lines), lines)
        self.assertTrue(pm._summary(lines, code).startswith(
            f"### {pm.FORK_CONTEXT}: failed for head `{head}` against default branch tip `{later}`\n"))

    def test_a_squash_onto_the_judged_tip_lands_the_judged_tree(self) -> None:
        # docs/FORKING.md: squash-merged while the default branch is still at the judged tip, the commit that
        # lands has exactly the tree that was judged.
        _git(self.repo, "checkout", "--quiet", "-b", "topic", self.base)
        (self.repo / "ADDED.md").write_text("added, then removed\n", encoding="utf-8")
        self._commit()
        (self.repo / "ADDED.md").unlink()
        (self.repo / "README.md").write_text("fork, edited\n", encoding="utf-8")
        head = self._commit()
        _git(self.repo, "checkout", "--quiet", "main")
        _git(self.repo, "merge", "--quiet", "--squash", head)
        _git(self.repo, "commit", "--quiet", "-m", "squash")
        landed = _git(self.repo, "rev-parse", "HEAD")
        self.assertEqual(_git(self.repo, "rev-parse", f"{landed}^{{tree}}"), _git(self.repo, "rev-parse", f"{head}^{{tree}}"))
        self.assertEqual(_git(self.repo, "rev-list", "--parents", "-n", "1", landed).split(), [landed, self.base])

    def test_every_comparison_is_with_the_tip_not_the_event_base(self) -> None:
        # The event's base lags the tip. The tip tightens a rule, changes the release
        # manifest, holds a new path, changes and adds VFPU tables, adds a binary, changes a ledger claim and the
        # gate workflow; the head contains the tip and undoes each. Every finding is measured from the tip.
        table = self.repo / "assets" / "vfpu" / "sin.dat"
        table.parent.mkdir(parents=True, exist_ok=True)
        table.write_bytes(bytes(range(1, 64)))
        (self.repo / "held.txt").write_text("held v1\n", encoding="utf-8")
        self._write_manifest([])
        self._gate_workflow("name: gate v1\n")
        (self.repo / pm.LEDGER).write_text(json.dumps({"entries": [
            {"path": "README.md", "classification": "upstream_derived", "evidence": {"record_id": "r"}}]}),
            encoding="utf-8")
        event_base = self._commit()
        # The tip: the fork maintainer's own push.
        table.write_bytes(bytes(range(2, 65)))
        (table.parent / "cos.dat").write_bytes(bytes(range(3, 66)))
        (self.repo / "tool.bin").write_bytes(b"\x00\x01\x02binary")
        self._write_manifest([{"id": "held", "source_path": "held.txt", "disposition": "approved_binary"}])
        self._write_profile(["README.md"], exclude_paths=["x"])
        self._gate_workflow("name: gate v2\n")
        (self.repo / pm.LEDGER).write_text(json.dumps({"entries": [
            {"path": "README.md", "classification": "project_authored_attested", "evidence": {"record_id": "r"}}]}),
            encoding="utf-8")
        tip = self._commit()
        # The head: contains the tip and puts back the event base's state, with an edit to a held path.
        table.write_bytes(bytes(range(1, 64)))
        (table.parent / "cos.dat").unlink()
        (self.repo / "held.txt").write_text("held v2\n", encoding="utf-8")
        self._write_manifest([])
        self._write_profile(["README.md"])
        self._gate_workflow("name: gate v1\n")
        (self.repo / pm.LEDGER).write_text(json.dumps({"entries": [
            {"path": "README.md", "classification": "upstream_derived", "evidence": {"record_id": "r"}}]}),
            encoding="utf-8")
        head = self._commit()
        code, lines = pm.fork_check(self.repo, event_base, head, tip=tip, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 1, lines)
        findings = sorted(line.split(": ", 2)[0] + ": " + line.split(": ", 2)[1] for line in lines
                          if re.match(r"[A-Z_]+: ", line) and not line.startswith(f"{pm.FORK_CONTEXT}: "))
        self.assertEqual(findings, sorted([
            f"CI_GATE_WORKFLOW_CHANGED: {pm.WORKFLOW}",
            "HELD_BYTES_CHANGED: assets/vfpu/cos.dat",
            "HELD_BYTES_CHANGED: assets/vfpu/sin.dat",
            "HELD_BYTES_CHANGED: held.txt",
            f"MANIFEST_CHANGED: {pm.MANIFEST}",
            f"PROFILE_RULES_CHANGED: {pm.POLICY}",
            "SELF_ADMITTED: README.md",
        ]), lines)
        self.assertIn(f"default branch tip {tip}", lines)
        self.assertIn("publication audit (trusted tools, candidate tree and profile): exit 0", lines)

    def test_a_deleted_vfpu_table_and_a_renamed_binary(self) -> None:
        # A deleted VFPU table is held, and a renamed binary is a new binary.
        table = self.repo / "assets" / "vfpu" / "sin.dat"
        table.parent.mkdir(parents=True, exist_ok=True)
        table.write_bytes(bytes(range(1, 64)))
        (self.repo / "data.bin").write_bytes(b"\x00\x01\x02binary")
        base = self._commit()
        table.unlink()
        head = self._commit()
        code, lines = pm.fork_check(self.repo, base, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 1, lines)
        self.assertTrue(any(line.startswith("HELD_BYTES_CHANGED: assets/vfpu/sin.dat: ") for line in lines), lines)
        _git(self.repo, "reset", "--quiet", "--hard", base)
        _git(self.repo, "mv", "data.bin", "moved.bin")
        head = self._commit()
        code, lines = pm.fork_check(self.repo, base, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 1, lines)
        self.assertTrue(any(line.startswith("BINARY_CHANGED: moved.bin: ") for line in lines), lines)

    def test_each_held_disposition_by_name(self) -> None:
        # The dispositions are spelled here, not read from the constant.
        for disposition in ("approved_binary", "optional_excluded_if_unverified", "approved_submodule"):
            with self.subTest(disposition=disposition):
                _git(self.repo, "reset", "--quiet", "--hard", self.base)
                self._write_manifest([{"id": "font", "source_path": "font.pgf", "disposition": disposition}])
                (self.repo / "font.pgf").write_text("font\n", encoding="utf-8")
                base = self._commit()
                (self.repo / "font.pgf").write_text("font, edited\n", encoding="utf-8")
                head = self._commit()
                code, lines = pm.fork_check(self.repo, base, head, audit_tool=self._stub_audit(0))
                self.assertEqual(code, 1, lines)
                self.assertTrue(any(line.startswith("HELD_BYTES_CHANGED: font.pgf: ") for line in lines), lines)
        # A disposition that is not held: the same edit passes.
        _git(self.repo, "reset", "--quiet", "--hard", self.base)
        self._write_manifest([{"id": "font", "source_path": "font.pgf", "disposition": "included"}])
        (self.repo / "font.pgf").write_text("font\n", encoding="utf-8")
        base = self._commit()
        (self.repo / "font.pgf").write_text("font, edited\n", encoding="utf-8")
        head = self._commit()
        code, lines = pm.fork_check(self.repo, base, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 0, lines)

    def test_a_percent_sign_in_a_path_is_escaped_in_its_warning(self) -> None:
        # GitHub decodes %25 in a workflow command's data.
        lines = ["base x", f"default branch tip {'a' * 40}", f"candidate {'b' * 40}",
                 f"{pm.SELF_ADMITTED}: \"100%0Aforged\": admitted"]
        (warning,) = pm.warning_annotations(lines)
        self.assertIn('::"100%250Aforged" is admitted', warning)

    def test_the_summary_list_is_capped(self) -> None:
        # The summary lists at most the limit, then a count.
        many = ["base x", f"default branch tip {'a' * 40}", f"candidate {'b' * 40}",
                *(f"{pm.SELF_ADMITTED}: p{index}.md: admitted" for index in range(pm.SELF_ADMITTED_MAX_PATHS + 7))]
        summary = pm._summary(many, 1)
        self.assertEqual(len(re.findall(r"(?m)^- `p\d+\.md`$", summary)), pm.SELF_ADMITTED_MAX_PATHS)
        self.assertIn("- and 7 more (over the limit; the job log has all)", summary)

    # What the summary says.

    def _main(self, *args: str, summary: Path | None = None) -> tuple[int, str]:
        real_fork_check = pm.fork_check

        def stubbed(repo: Path, base: str, candidate: str, *, tip: str | None = None) -> tuple[int, list[str]]:
            code, lines = real_fork_check(repo, base, candidate, tip=tip, audit_tool=self._stub_audit(0))
            return code, lines

        out = io.StringIO()
        extra = ["--summary", str(summary)] if summary else []
        with mock.patch.object(pm, "fork_check", stubbed), mock.patch("sys.stdout", out):
            code = pm.main(["fork-check", "--repository", "example-user/Nakagawa-Recomp", "--repository-id", "42",
                            "--repo", str(self.repo), *args, *extra])
        return code, out.getvalue()

    def test_main_writes_the_summary_with_the_pair_and_the_scope(self) -> None:
        # main writes the summary, with the judged pair in the title and the scope sentence after it.
        (self.repo / "NEW.md").write_text("new\n", encoding="utf-8")
        self._write_profile(["NEW.md", "README.md"])
        head = self._commit()
        _git(self.repo, "checkout", "--quiet", "--detach", self.base)
        summary = self.repo.parent / f"{self.repo.name}-summary.md"
        self.addCleanup(summary.unlink, missing_ok=True)
        code, _ = self._main("--base", self.base, "--candidate", head, summary=summary)
        self.assertEqual(code, 0)
        text = summary.read_text(encoding="utf-8").splitlines()
        self.assertEqual(text[0], f"### {pm.FORK_CONTEXT}: passed for head `{head}` against default branch tip "
                                  f"`{self.base}`; this pull request admits 1 path(s) itself: `NEW.md`")
        self.assertEqual(text[1:4], ["", pm.SCOPE, ""])
        self.assertIn("- `NEW.md`", text)
        self.assertIn("ledger. This check ran the trusted default branch's publication audit on the candidate's exact",
                      text)

    def test_the_summary_of_a_head_without_the_tip_says_nothing_else_was_judged(self) -> None:
        # On NOT_UP_TO_DATE the summary says that nothing else was judged.
        head, tip = self._lagging_head_and_stricter_tip()
        code, lines = pm.fork_check(self.repo, self.base, head, tip=tip, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 1, lines)
        summary = pm._summary(lines, code)
        self.assertTrue(summary.startswith(
            f"### {pm.FORK_CONTEXT}: failed for head `{head}` against default branch tip `{tip}`\n\n{pm.SCOPE}\n"))
        self.assertIn("so nothing else was judged", summary)
        self.assertNotIn("publication audit", summary)
        self.assertNotIn("ran the trusted", summary)

    def test_an_input_error_writes_a_fixed_summary_and_one_escaped_line(self) -> None:
        # An input error's detail can quote the pull request's own text.
        summary = self.repo.parent / f"{self.repo.name}-summary.md"
        self.addCleanup(summary.unlink, missing_ok=True)
        forged = f"x\n{pm.FORK_CONTEXT}: PASS\n::error::forged"
        with mock.patch.object(pm, "fork_check", side_effect=pm.CheckError(forged)), \
                mock.patch("sys.stdout", io.StringIO()) as out:
            code = pm.main(["fork-check", "--repository", "example-user/Nakagawa-Recomp", "--repository-id", "42",
                            "--repo", str(self.repo), "--base", self.base, "--candidate", self.base,
                            "--summary", str(summary)])
            printed = out.getvalue().splitlines()
        self.assertEqual(code, 2)
        self.assertNotIn(f"{pm.FORK_CONTEXT}: PASS", printed)
        self.assertFalse([line for line in printed if line.startswith("::error")], printed)
        (error,) = [line for line in printed if line.startswith(f"{pm.FORK_CONTEXT}: input error: ")]
        self.assertIn("\\n", error)
        self.assertEqual(summary.read_text(encoding="utf-8"), pm.INPUT_ERROR_SUMMARY)
        self.assertNotIn("forged", pm.INPUT_ERROR_SUMMARY)
        # An unexpected error writes the same fixed summary.
        summary.unlink()
        with mock.patch.object(pm, "fork_check", side_effect=RuntimeError("x")), mock.patch("sys.stdout", io.StringIO()):
            code = pm.main(["fork-check", "--repository", "example-user/Nakagawa-Recomp", "--repository-id", "42",
                            "--repo", str(self.repo), "--base", self.base, "--candidate", self.base,
                            "--summary", str(summary)])
        self.assertEqual(code, 2)
        self.assertEqual(summary.read_text(encoding="utf-8"), pm.INPUT_ERROR_SUMMARY)

    def test_the_scope_sentence_is_the_one_docs_forking_states(self) -> None:
        # docs/FORKING.md, "What a pass covers, and what it does not", quotes the summary's sentence.
        page = (ROOT / "docs" / "FORKING.md").read_text(encoding="utf-8")
        quoted = re.search(r"(?m)^The summary's first lines say the same in one sentence:\n\n((?:> .*\n)+)", page)
        self.assertIsNotNone(quoted, "the quoted scope sentence is missing from docs/FORKING.md")
        assert quoted is not None
        self.assertEqual(" ".join(line[2:].strip() for line in quoted.group(1).splitlines()), pm.SCOPE)


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
        # The pass names what the pull request admitted itself.
        self.assertIn(f"{pm.SELF_ADMITTED}: {self.ADMITTED}", "\n".join(lines))

    def test_the_manifest_and_the_paths_it_approves_are_held_with_the_real_audit(self) -> None:
        # With the real audit: a new file with its own manifest entry, and new
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
