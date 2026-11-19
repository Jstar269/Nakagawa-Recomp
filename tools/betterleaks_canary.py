#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Exercise the pinned Betterleaks policy with synthetic, non-secret canaries.

The script deliberately constructs secret-shaped values from hashes at runtime,
so no credential-shaped literal is committed.  It is an explicit security gate,
not part of the ordinary Python unit-test discovery: the caller must provide the
Betterleaks binary installed from the immutable pre-commit revision.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

try:
    from .nk_core.git_isolation import run_git
except ImportError:
    from nk_core.git_isolation import run_git  # type: ignore[no-redef]


ROOT = Path(__file__).resolve().parent.parent
CONFIG = ROOT / ".betterleaks.toml"
# Betterleaks exits 1 both for "leaks found" and for a fatal error such as a config
# that does not load or a git target that is not a repository. Every scan passes
# --exit-code with this distinct value, so a positive canary passes only on a real
# detection and any other code is reported as a scanner failure.
LEAKS_FOUND_EXIT = 99

#: The `git log` options of the hosted reachable-history scan (.github/workflows/ci.yml,
#: "Scan reachable sanitized Git history with Betterleaks"), before its revision range.
#: `--diff-merges=first-parent` gives each merge commit a patch, so a value added while
#: resolving a merge is scanned; tools/test_public_ci_wiring.py keeps the workflow equal.
HISTORY_LOG_OPTIONS = "--full-history --diff-merges=first-parent"


def _material(seed: str, length: int) -> str:
    value = ""
    counter = 0
    while len(value) < length:
        value += hashlib.sha256(f"{seed}-{counter}".encode("utf-8")).hexdigest()
        counter += 1
    return value[:length]


def _scan(
    binary: str,
    mode: str,
    target: str,
    *,
    config: Path = CONFIG,
    extra: tuple[str, ...] = (),
) -> subprocess.CompletedProcess[str]:
    command = [
        binary,
        mode,
        target,
        "--config",
        str(config),
        "--redact",
        "--exit-code",
        str(LEAKS_FOUND_EXIT),
        *extra,
    ]
    return subprocess.run(
        command,
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )


def _require(label: str, result: subprocess.CompletedProcess[str], *, leaks: bool) -> None:
    expected = LEAKS_FOUND_EXIT if leaks else 0
    actual = result.returncode
    if actual != expected:
        kind = "" if actual in (0, LEAKS_FOUND_EXIT) else " (a scanner error, not a scan verdict)"
        stderr = "\n".join((result.stderr or "").strip().splitlines()[-5:])
        detail = f"\n{stderr}" if stderr else ""
        raise RuntimeError(f"{label}: expected exit {expected}, got {actual}{kind}{detail}")
    print(f"{label}: PASS")


def _write_synthetic_fixture(path: Path, content: str) -> None:
    """Write a hash-derived fixture through a disposable helper process.

    The parent never stores the synthetic credential-shaped payload through a
    normal application file-write sink; the helper receives it only on stdin
    and writes into the already-private temporary directory.  Nothing is
    logged, and the fixture is removed with the surrounding temp directory.
    """
    helper = (
        "from pathlib import Path; import sys; "
        "Path(sys.argv[1]).write_text(sys.stdin.read(), encoding='utf-8')"
    )
    subprocess.run(
        [sys.executable, "-c", helper, str(path)],
        input=content,
        text=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=True,
    )


def _build_canaries(directory: Path) -> list[tuple[str, str]]:
    classic = "gh" + "p_" + _material("classic-pat", 36)
    fine_grained = "github" + "_pat_" + _material("fine-grained-pat", 82)
    api_key = _material("generic-api-key", 48)
    aws_access = ("AK" + "IA" + _material("aws-access", 16)).upper()
    aws_secret = _material("aws-secret", 40)
    bearer = _material("bearer-token", 48)
    generic = _material("generic-credential", 64)
    private_header = "-----BEGIN " + "RSA " + "PRIVATE KEY-----"
    private_footer = "-----END " + "RSA " + "PRIVATE KEY-----"
    return [
        ("classic-github-pat", f'token = "{classic}"\n'),
        ("fine-grained-github-pat", f'token = "{fine_grained}"\n'),
        ("generic-api-key", f'api_key = "{api_key}"\n'),
        (
            "aws-access-secret",
            f'aws_access_key_id = "{aws_access}"\naws_secret_access_key = "{aws_secret}"\n',
        ),
        ("bearer-token-assignment", f'bearer_token = "{bearer}"\n'),
        ("high-entropy-credential", f'credential = "{generic}"\n'),
        (
            "synthetic-private-key",
            f"{private_header}\n{_material('private-key', 64)}\n{private_footer}\n",
        ),
    ]


def run(binary: str) -> int:
    if not CONFIG.is_file():
        raise RuntimeError(f"missing Betterleaks config: {CONFIG}")
    with tempfile.TemporaryDirectory(prefix="nakagawa-betterleaks-") as raw:
        temp_root = Path(raw)

        for label, content in _build_canaries(temp_root):
            case = temp_root / f"{label}.txt"
            _write_synthetic_fixture(case, content)
            _require(label, _scan(binary, "dir", str(case)), leaks=True)

        # The scanner must inspect encoded and archive-contained content when
        # the caller explicitly enables the documented traversal depths.
        encoded = temp_root / "encoded.txt"
        encoded.write_text(
            base64.b64encode(f'api_key = "{_material("encoded", 48)}"\n'.encode()).decode(),
            encoding="ascii",
        )
        _require(
            "base64-decoded-secret",
            _scan(binary, "dir", str(encoded), extra=("--max-decode-depth", "5")),
            leaks=True,
        )

        archive = temp_root / "archive.zip"
        with zipfile.ZipFile(archive, "w") as bundle:
            bundle.writestr("nested/secret.txt", f'api_key = "{_material("archive", 48)}"\n')
        _require(
            "archive-contained-secret",
            _scan(binary, "dir", str(archive), extra=("--max-archive-depth", "2")),
            leaks=True,
        )

        # The checked-in fixture is intentionally synthetic and must be
        # filtered; the filter is value- and path-specific, not tools/*.
        _require(
            "deliberate-history-audit-fixture",
            _scan(binary, "dir", "tools/test_history_audit.py"),
            leaks=False,
        )

        markdown = temp_root / "README.md"
        markdown.write_text(
            f"# Synthetic canary\n\napi_key = {_material('markdown', 48)}\n",
            encoding="utf-8",
        )
        _require("markdown-secret", _scan(binary, "dir", str(markdown)), leaks=True)

        unusual_path = temp_root / "path-without-a-source-extension"
        unusual_path.write_text(
            f"credential = {_material('unusual-path', 64)}\n",
            encoding="utf-8",
        )
        _require("unusual-filename", _scan(binary, "dir", str(unusual_path)), leaks=True)

        unsafe_tools = temp_root / "tools"
        unsafe_tools.mkdir()
        (unsafe_tools / "unsafe.py").write_text(
            f'api_key = "{_material("tools-unsafe", 48)}"\n', encoding="utf-8"
        )
        _require("tools-path-variation", _scan(binary, "dir", str(unsafe_tools)), leaks=True)

        mixed_case_dir = temp_root / "mixed-case"
        mixed_case_dir.mkdir()
        mixed_case_path = mixed_case_dir / "ToKeN.TXT"
        mixed_case_path.write_text(
            f'api_key = "{_material("mixed-case-path", 48)}"\n', encoding="utf-8"
        )
        _require("mixed-case-path-variation", _scan(binary, "dir", str(mixed_case_path)), leaks=True)

        moved_fixture = temp_root / "fixtures" / "history-audit-copy.py"
        moved_fixture.parent.mkdir()
        deliberate_value = "gh" + "p_" + "123456789012345678901234567890123456"
        _write_synthetic_fixture(
            moved_fixture,
            f'token = "{deliberate_value}"\n',
        )
        _require("moved-outside-fixture-path", _scan(binary, "dir", str(moved_fixture)), leaks=True)

        # The same fixture path must not suppress a different value.
        value_repo = temp_root / "value-scope-repo"
        value_fixture = value_repo / "tools" / "test_history_audit.py"
        value_fixture.parent.mkdir(parents=True)
        _write_synthetic_fixture(
            value_fixture,
            f'token = "{"gh" + "p_" + _material("different-fixture-value", 36)}"\n',
        )
        for args in (("init", "-q"),):
            run_git(args, cwd=value_repo, check=True, capture_output=True)
        run_git(
            ["add", "tools/test_history_audit.py"],
            cwd=value_repo,
            check=True,
            capture_output=True,
        )
        run_git(
            ["commit", "-qm", "different synthetic fixture"],
            cwd=value_repo,
            check=True,
        )
        _require("fixture-path-new-value", _scan(binary, "git", str(value_repo)), leaks=True)

        # A second rule matching the allowed value at the allowed path must
        # still report. This proves the project exception is rule-scoped rather
        # than a blanket exemption for that path/value pair.
        rule_config = temp_root / "rule-scope.betterleaks.toml"
        rule_config.write_text(
            CONFIG.read_text(encoding="utf-8")
            + """
[[rules]]
id = "synthetic-secondary-rule"
description = "Synthetic secondary rule for exception-scope testing"
regex = '''(ghp_[0-9]{36})'''
keywords = ["ghp_"]
""",
            encoding="utf-8",
        )
        rule_repo = temp_root / "rule-scope-repo"
        rule_fixture = rule_repo / "tools" / "test_history_audit.py"
        rule_fixture.parent.mkdir(parents=True)
        _write_synthetic_fixture(rule_fixture, f'token = "{deliberate_value}"\n')
        for args in (("init", "-q"),):
            run_git(args, cwd=rule_repo, check=True, capture_output=True)
        run_git(
            ["add", "tools/test_history_audit.py"],
            cwd=rule_repo,
            check=True,
            capture_output=True,
        )
        run_git(
            ["commit", "-qm", "exact synthetic fixture"],
            cwd=rule_repo,
            check=True,
        )
        _require(
            "fixture-secondary-rule",
            _scan(binary, "git", str(rule_repo), config=rule_config),
            leaks=True,
        )

        # A full reachable-history run protects the primary scanner from a
        # configuration change that only covers the current filesystem tree.
        _require("sanitized-history", _scan(binary, "git", "."), leaks=False)

        # Git mode intentionally covers committed history, not ignored or
        # untracked working-tree bytes.  The directory mode remains the
        # publication-critical check for that local state; record both halves
        # so a future CLI/config change cannot silently alter the contract.
        ignored_repo = temp_root / "ignored-repo"
        ignored_repo.mkdir()
        for args in (("init", "-q"),):
            run_git(args, cwd=ignored_repo, check=True, capture_output=True)
        (ignored_repo / ".gitignore").write_text("ignored.txt\n", encoding="utf-8")
        run_git(["add", ".gitignore"], cwd=ignored_repo, check=True, capture_output=True)
        run_git(["commit", "-qm", "ignore synthetic canary"], cwd=ignored_repo, check=True)
        ignored = ignored_repo / "ignored.txt"
        ignored.write_text(
            f'api_key = "{_material("ignored-untracked", 48)}"\n', encoding="utf-8"
        )
        _require("ignored-untracked-directory", _scan(binary, "dir", str(ignored)), leaks=True)
        _require("ignored-untracked-history", _scan(binary, "git", str(ignored_repo)), leaks=False)

        # Prove the history path is not merely a current-tree check: put a
        # positive synthetic value in an ancestor commit and remove it before
        # scanning the resulting repository.
        ancestor = temp_root / "ancestor-repo"
        ancestor.mkdir()
        for args in (("init", "-q"),):
            run_git(args, cwd=ancestor, check=True, capture_output=True)
        historical = ancestor / "historical.txt"
        historical.write_text(f'api_key = "{_material("ancestor", 48)}"\n', encoding="utf-8")
        run_git(["add", "historical.txt"], cwd=ancestor, check=True, capture_output=True)
        run_git(["commit", "-qm", "synthetic canary"], cwd=ancestor, check=True)
        historical.unlink()
        run_git(["commit", "-qam", "remove synthetic canary"], cwd=ancestor, check=True)
        _require("ancestor-only-secret", _scan(binary, "git", str(ancestor)), leaks=True)

        # A value typed only while resolving a merge conflict, and removed in the
        # next commit, lives in no ordinary commit's patch: `git log -p` prints none
        # for a merge unless told to. The hosted history scan passes the log options
        # below; scan the same shape of history with them, both as a pull request's
        # range from its base and as all history reachable from HEAD.
        merged = temp_root / "merge-only-repo"
        merged.mkdir()
        run_git(["init", "-q", "-b", "trunk"], cwd=merged, check=True, capture_output=True)
        conflicted = merged / "conflicted.txt"
        conflicted.write_text("base\n", encoding="utf-8")
        run_git(["add", "conflicted.txt"], cwd=merged, check=True, capture_output=True)
        run_git(["commit", "-qm", "base"], cwd=merged, check=True)
        run_git(["checkout", "-qb", "upstream"], cwd=merged, check=True, capture_output=True)
        conflicted.write_text("upstream\n", encoding="utf-8")
        run_git(["commit", "-qam", "upstream change"], cwd=merged, check=True)
        upstream = run_git(
            ["rev-parse", "HEAD"], cwd=merged, check=True, capture_output=True, text=True
        ).stdout.strip()
        run_git(["checkout", "-q", "trunk"], cwd=merged, check=True, capture_output=True)
        conflicted.write_text("topic\n", encoding="utf-8")
        run_git(["commit", "-qam", "topic change"], cwd=merged, check=True)
        conflict = run_git(["merge", "-q", "upstream"], cwd=merged, check=False, capture_output=True)
        if conflict.returncode == 0:
            raise RuntimeError("merge-only canary: the synthetic merge did not conflict")
        conflicted.write_text(
            f'resolved\napi_key = "{_material("merge-resolution", 48)}"\n', encoding="utf-8"
        )
        run_git(["add", "conflicted.txt"], cwd=merged, check=True, capture_output=True)
        run_git(["commit", "-qm", "merge upstream"], cwd=merged, check=True)
        conflicted.write_text("resolved\n", encoding="utf-8")
        run_git(["commit", "-qam", "remove synthetic canary"], cwd=merged, check=True)
        for label, revisions in (
            ("merge-only-secret-pull-request-range", f"{upstream}..HEAD"),
            ("merge-only-secret-reachable-history", "HEAD"),
        ):
            _require(
                label,
                _scan(
                    binary,
                    "git",
                    str(merged),
                    extra=(f"--log-opts={HISTORY_LOG_OPTIONS} {revisions}",),
                ),
                leaks=True,
            )
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--betterleaks",
        default=os.environ.get("BETTERLEAKS_BIN", "betterleaks"),
        help="Betterleaks executable (default: BETTERLEAKS_BIN or PATH)",
    )
    args = parser.parse_args(argv)
    try:
        return run(args.betterleaks)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"betterleaks canary: FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
