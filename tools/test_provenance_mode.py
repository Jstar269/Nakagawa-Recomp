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
        kind = match.lastindex
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


def _context(repository: str, repository_id: str) -> dict[str, str]:
    return {
        "github.event_name": "pull_request_target",
        "github.event.pull_request.base.ref": "main",
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
    # A run that could not see the ID still matches by name.
    ("Jstar269/Nakagawa-Recomp", ""),
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
        for repository, repository_id in UPSTREAM_RUNS:
            with self.subTest(repository=repository, repository_id=repository_id):
                context = _context(repository, repository_id)
                self.assertTrue(evaluate(attest, context))
                self.assertFalse(evaluate(fork, context))
        for repository, repository_id in FORK_RUNS:
            with self.subTest(repository=repository):
                context = _context(repository, repository_id)
                self.assertFalse(evaluate(attest, context))
                self.assertTrue(evaluate(fork, context))

    def test_neither_job_runs_for_another_base_branch(self) -> None:
        for repository, repository_id in UPSTREAM_RUNS + FORK_RUNS:
            context = _context(repository, repository_id) | {"github.event.pull_request.base.ref": "evil-base"}
            self.assertFalse(evaluate(_job_condition(self.attest), context))
            self.assertFalse(evaluate(_job_condition(self.fork), context))

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
        self.assertNotIn("publish_audit", block)

    def test_fork_job_reads_no_secret_and_never_contacts_the_authority(self) -> None:
        block = _code(self.fork)
        self.assertNotIn("secrets.", block)
        self.assertNotIn("history-private", block)
        self.assertNotIn("TRUSTED_REPO", block)
        self.assertNotIn("gh api", block)
        self.assertNotIn("continue-on-error", block)
        self.assertIn("ref: ${{ github.event.repository.default_branch }}", block)
        self.assertIn("python tools/provenance_mode.py --require fork", block)
        self.assertIn("python tools/provenance_mode.py fork-check", block)
        # The guard runs before the candidate is fetched or checked.
        self.assertLess(block.index("--require fork"), block.index("refs/candidate/head"))

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
        stub = self.repo.parent / f"{self.repo.name}-audit-{code}.py"
        stub.write_text(f"import sys\nprint('stub audit')\nsys.exit({code})\n", encoding="utf-8")
        self.addCleanup(stub.unlink, missing_ok=True)
        return stub

    def test_lists_profile_changes_and_passes_on_a_clean_audit(self) -> None:
        (self.repo / "NEW.md").write_text("new\n", encoding="utf-8")
        self._write_profile(["NEW.md", "README.md"], exclude_paths=["x"])
        head = self._commit()
        code, lines = pm.fork_check(self.repo, self.base, head, audit_tool=self._stub_audit(0))
        self.assertEqual(code, 0, lines)
        self.assertIn("  added NEW.md", lines)
        self.assertTrue(any("rules or exclusions changed" in line for line in lines))
        self.assertEqual(lines[-1], f"{pm.FORK_CONTEXT}: PASS")

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

    def test_the_gate_workflow_itself_may_name_both_checks(self) -> None:
        workflow = self.repo / pm.WORKFLOW
        workflow.parent.mkdir(parents=True, exist_ok=True)
        workflow.write_text(f"name: {pm.UPSTREAM_CONTEXT}\nname: {pm.FORK_CONTEXT}\n", encoding="utf-8")
        head = self._commit()
        code, lines = pm.fork_check(self.repo, self.base, head, audit_tool=self._stub_audit(0))
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
        self.assertEqual(command[2:], ["--repo-root", str(self.repo), "--committed-tree", self.base,
                                       "--public-scope", "--provenance-self-consistency"])


if __name__ == "__main__":
    unittest.main()
