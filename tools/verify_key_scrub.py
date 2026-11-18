#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Check whether any PSP KIRK/amctrl constant is still reachable in Git history.

This contains no key material: it reads the constant *values* from the local key
file (a private local binding) and searches every reachable commit for each value in
the textual encodings the tree has ever used -- contiguous hex, and C/Python byte
arrays with assorted spacing, case, and zero-padding. `git log -S <hex>` alone is
NOT sufficient, because it never matches the `{0x12,0x46,...}` byte-array form.
Beside those exact spellings, one whitespace-tolerant regular-expression search per
constant (`history_pattern`) also matches a byte array or hex string wrapped over
several lines or spaced with tabs, mixed 0x/decimal arrays, and `\\xNN` escaped
strings, including ones split into adjacent string literals. Not searched: 16- or
32-bit word literals and escaped strings that spell printable bytes as characters.

Every `name = value` line of the key file must hold exactly 16 bytes of hex; a line
that does not parse makes the run unverifiable (exit 2), naming the entry but never
printing its value, so a clean verdict never covers a constant that was not searched.

The verifier fails closed (#377): a history search that did not *complete* is never
reported as clean. Repository preconditions (inside a Git work tree or bare
repository, not shallow, refs with reachable history) are validated once up front,
and every `git log -m` pickaxe invocation (merges diffed against each parent) must exit
0 to count as a completed search.
An empty/no-ref repository is unverifiable (exit 2), not clean.

Exit 0  -> every configured constant was searched in every encoding, every search
           completed successfully, and nothing matched (scrub verified / never present).
Exit 3  -> at least one constant is positively reachable (scrub incomplete / not yet run).
Exit 2  -> could not verify (missing/unusable key file, an unparseable entry,
           failed repository preconditions, or any failed `git` invocation) and there is no
           definitive positive exposure result. Verdict precedence is
           exposure > unverifiable > clean: a confirmed reachable constant
           exits 3 even when other searches failed, with the partial-coverage
           diagnostics printed prominently beside the finding.

Scope: the Git repository containing the current working directory; preconditions
and searches always target that same repository. Run it BEFORE a scrub to confirm
the exposure, and AFTER to confirm it is gone.

Diagnostics never contain constant values or search needles: `git` stderr is
redacted against every configured encoding before display.

Usage:
  python tools/verify_key_scrub.py            # uses keys/pgd_keys.txt or $SR_PGD_KEYS
  python tools/verify_key_scrub.py --keys PATH
"""

from __future__ import annotations

import argparse
from collections.abc import Sequence
import os
import subprocess
import sys
from dataclasses import dataclass
from enum import Enum

try:
    from .nk_core.git_isolation import isolated_git_env
except ImportError:
    from nk_core.git_isolation import isolated_git_env  # type: ignore[no-redef]

GIT_TIMEOUT_SECONDS = 300
# A regex pickaxe walk evaluates the pattern over every changed blob; on this
# repository's history it takes several times as long as a literal walk.
REGEX_GIT_TIMEOUT_SECONDS = 1800
_MAX_STDERR_CHARS = 2000


class Verdict(Enum):
    """Outcome of one encoding search. ERROR is never conflated with NOT_FOUND."""

    FOUND = "FOUND"            # a completed search matched this encoding
    NOT_FOUND = "NOT_FOUND"    # a completed search did not match
    ERROR = "ERROR"            # the search did not complete (git failed/missing/timed out)


@dataclass(frozen=True)
class SearchError:
    """Why one history search could not run. `detail` is redacted and needle-free."""

    operation: str  # command description; never contains the needle
    exit_code: int | None
    detail: str


def parse_key_file(path: str) -> tuple[dict[str, bytes], list[str]]:
    """(name -> 16 raw bytes, names of `name = value` lines that do not parse).

    A value parses only as exactly 32 hex digits (16 bytes). The second list names
    every other `name = value` line (or its line number when the name is empty) and
    never carries the value, so a caller can refuse to verify without leaking it.
    """
    values: dict[str, bytes] = {}
    unparsed: list[str] = []
    with open(path, "r", encoding="utf-8") as handle:
        for number, line in enumerate(handle, start=1):
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            name, _, value = line.partition("=")
            name, value = name.strip(), value.strip()
            raw: bytes | None = None
            if len(value) == 32:
                try:
                    raw = bytes.fromhex(value)
                except ValueError:
                    raw = None
            if raw is None or not name:
                unparsed.append(name or f"(unnamed entry on line {number})")
                continue
            values[name] = raw
    return values, unparsed


def key_values(path: str) -> dict[str, bytes]:
    """name -> 16 raw bytes, parsed from the `name = hex` key file (parseable entries only)."""
    return parse_key_file(path)[0]


def encodings(raw: bytes) -> list[str]:
    """Every textual spelling of `raw` this codebase has plausibly used."""
    forms: set[str] = set()
    forms.add(raw.hex())                      # lowercase contiguous hex
    forms.add(raw.hex().upper())              # uppercase contiguous hex
    for sep in (",", ", "):                   # C / Python byte arrays
        for pad in (True, False):             # 0x0a vs 0xa (non-padded ints)
            for up in (False, True):          # 0xab vs 0xAB
                parts = []
                for b in raw:
                    h = f"{b:02x}" if pad else f"{b:x}"
                    if up:
                        h = h.upper()
                    parts.append("0x" + h)
                forms.add(sep.join(parts))
                # Python bytes([...]) also renders small values as bare ints (1, not 0x01).
    # Bare-int Python list form (bytes([0x27, 0x74, 1, 2, ...])): decimal for <16, hex else.
    forms.add(", ".join(str(b) if b < 16 else f"0x{b:02x}" for b in raw))
    forms.add(", ".join(str(b) if b < 16 else f"0x{b:02X}" for b in raw))
    return sorted(forms)


def _hex_digit_class(digit: int) -> str:
    """One hex digit in either case as a POSIX bracket expression; a decimal digit stays bare."""
    text = f"{digit:x}"
    return f"[{text}{text.upper()}]" if text.isalpha() else text


def history_pattern(raw: bytes) -> str:
    """One whitespace-tolerant POSIX extended regex for `git log --pickaxe-regex -S`.

    It matches the spellings no single-line literal needle can: a hex string or byte
    array wrapped over several lines or spaced with tabs, an array mixing `0x` and
    decimal bytes in any padding, and `\\xNN` escaped strings, including ones split
    into adjacent literals (`b"\\x12"` then `b"\\x34"` on the next line) or continued
    with a backslash. Git compiles pickaxe patterns as POSIX extended regexes, in
    which `[[:space:]]` also matches a newline, so a match may span lines.
    """
    hex_tokens = [_hex_digit_class(b >> 4) + _hex_digit_class(b & 0xF) for b in raw]
    array_tokens = []
    for b, token in zip(raw, hex_tokens, strict=True):
        # 0x0c, 0xc, 0x00c and decimal 12 all spell the byte 0x0c.
        prefixed = "0[xX]0*" + (token if b >= 16 else _hex_digit_class(b))
        array_tokens.append(f"({prefixed}|{b})")
    forms = (
        # Hex text: contiguous, space- or tab-separated, or wrapped.
        "[[:space:]]*".join(hex_tokens),
        # C / Python byte arrays: one comma between bytes, any whitespace around it.
        "[[:space:]]*,[[:space:]]*".join(array_tokens),
        # Escaped strings; quotes, a bytes prefix and line continuations may separate the escapes.
        """[[:space:]"'bB\\]*""".join("\\\\[xX]" + token for token in hex_tokens),
    )
    return "|".join(f"({form})" for form in forms)


def _redact(text: str, redact_needles: Sequence[str]) -> str:
    """Remove every configured encoding (and so every key value) from `text`."""
    for needle in redact_needles:
        if needle:
            text = text.replace(needle, "[redacted]")
    return text


def _as_text(value: object) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", "replace")
    return str(value)


def _git(args: list[str]) -> subprocess.CompletedProcess[str] | None:
    """Run a read-only probe `git` command; None when git is missing or fails.

    Catches the same failure family as the search path so a broken probe
    becomes a documented exit-2 precondition failure, never a traceback.
    """
    try:
        return subprocess.run(
            ["git", *args],
            env=isolated_git_env(),
            capture_output=True,
            text=True,
            check=False,
            timeout=GIT_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.SubprocessError, ValueError):
        return None


def check_repository() -> str | None:
    """Validate the Git preconditions for searching this repository's history.

    Scope is the repository containing the current working directory. Returns
    None when a `git log --all` search can meaningfully cover the reachable
    history, or a diagnostic (never containing key material) naming the failed
    precondition. Every failure maps to exit 2 so an unrunnable search can
    never be read as "clean".
    """
    inside = _git(["rev-parse", "--is-inside-work-tree"])
    if inside is None or inside.returncode != 0 or inside.stdout.strip() != "true":
        bare = _git(["rev-parse", "--is-bare-repository"])
        if bare is None or bare.returncode != 0 or bare.stdout.strip() != "true":
            return "not inside a Git work tree or bare repository; run from the checkout (or mirror clone) to verify"
    git_dir = _git(["rev-parse", "--git-dir"])
    if git_dir is None or git_dir.returncode != 0:
        return "git rev-parse --git-dir failed; there is no usable Git repository here"
    shallow = _git(["rev-parse", "--is-shallow-repository"])
    if shallow is None or shallow.returncode != 0:
        return "cannot determine whether this clone is shallow (git rev-parse --is-shallow-repository failed)"
    if shallow.stdout.strip() == "true":
        return "shallow clone: reachable history is truncated here, so no clean verdict would be trustworthy"
    refs = _git(["for-each-ref", "--format=%(refname)"])
    if refs is None or refs.returncode != 0:
        return "git for-each-ref failed; cannot enumerate refs to search"
    if not refs.stdout.strip():
        return "repository has no refs; there is no reachable history to verify (empty repository?)"
    return None


def _diagnostic(text: object, redact_needles: Sequence[str]) -> str:
    """Redact every configured encoding from `text` and bound its length."""
    detail = _redact(_as_text(text).strip(), redact_needles)
    if len(detail) > _MAX_STDERR_CHARS:
        detail = detail[:_MAX_STDERR_CHARS] + "...(truncated)"
    return detail


def search_history(
    needle: str, redact_needles: Sequence[str], *, regex: bool = False,
) -> tuple[Verdict, SearchError | None]:
    """Pickaxe-search every reachable commit for one textual encoding.

    With `regex`, `needle` is a POSIX extended regex (`history_pattern`) passed
    with `--pickaxe-regex`; otherwise it is a literal substring. Returns
    (FOUND, None) or (NOT_FOUND, None) only when `git log` ran and exited 0.
    Any nonzero exit, timeout, or missing `git` returns (ERROR, SearchError);
    ERROR never reads as "clean" (#377), though a definitive exposure still
    outranks it in the final verdict.
    """
    # `-m` diffs every merge against each parent: without it pickaxe builds no
    # diff for a merge commit, so a value added only by a merge's own
    # resolution (an "evil merge") would never be found.
    mode = ["--pickaxe-regex"] if regex else []
    timeout = REGEX_GIT_TIMEOUT_SECONDS if regex else GIT_TIMEOUT_SECONDS
    operation = f"git log --all -m {'--pickaxe-regex ' if regex else ''}-S <needle redacted> --oneline --source"
    try:
        result = subprocess.run(
            ["git", "log", "--all", "-m", *mode, "-S", needle, "--oneline", "--source"],
            env=isolated_git_env(),
            capture_output=True,
            text=True,
            check=False,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return Verdict.ERROR, SearchError(operation, None, f"git command timed out after {timeout}s")
    except (OSError, subprocess.SubprocessError, ValueError) as exc:
        # Any raise out of subprocess.run (missing git, spawn failure, ...) means
        # the search did not complete; it must never surface as "clean" (#377).
        # The exception text can embed the full command including the needle,
        # so it is redacted and bounded like any other diagnostic.
        detail = _diagnostic(exc, redact_needles) or "(exception carried no detail)"
        return Verdict.ERROR, SearchError(operation, None, f"git could not be executed: {detail}")
    if result.returncode != 0:
        detail = _diagnostic(result.stderr, redact_needles) or "(git produced no stderr)"
        return Verdict.ERROR, SearchError(operation, result.returncode, detail)
    if result.stdout.strip():
        return Verdict.FOUND, None
    return Verdict.NOT_FOUND, None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--keys", default=os.environ.get("SR_PGD_KEYS") or os.path.join("keys", "pgd_keys.txt"))
    args = parser.parse_args(argv)

    if not os.path.isfile(args.keys):
        print(f"key file not found: {args.keys} (private local binding)", file=sys.stderr)
        return 2

    values, unparsed = parse_key_file(args.keys)
    if unparsed:
        # Names only: the value on a malformed line may still be key material.
        print(
            f"cannot verify history: {len(unparsed)} key-file entr{'y' if len(unparsed) == 1 else 'ies'} "
            f"in {args.keys} did not parse as 16 bytes of hex and would not be searched: "
            f"{', '.join(unparsed)}",
            file=sys.stderr,
        )
        return 2
    if not values:
        print(f"no usable constants parsed from {args.keys}", file=sys.stderr)
        return 2

    # Preconditions once, up front: a search that cannot run must never be
    # conflated with a completed search that found nothing (#377).
    repo_problem = check_repository()
    if repo_problem is not None:
        print(f"cannot verify history: {repo_problem}", file=sys.stderr)
        return 2

    # Every search a constant gets: its literal spellings, then its tolerant pattern.
    # Each is computed once; the longest needles redact first.
    searches = {
        name: [(form, False) for form in encodings(raw)] + [(history_pattern(raw), True)]
        for name, raw in values.items()
    }
    all_needles = sorted({needle for plan in searches.values() for needle, _regex in plan}, key=len, reverse=True)

    exposed: list[str] = []
    unverifiable: list[str] = []
    errors: list[tuple[str, SearchError]] = []
    for name, plan in searches.items():
        found = False
        failed = False
        for needle, regex in plan:
            verdict, error = search_history(needle, all_needles, regex=regex)
            if error is not None:
                failed = True
                errors.append((name, error))
                continue
            if verdict is Verdict.FOUND:
                found = True
        if found:
            exposed.append(name)
            print(f"  {name}: REACHABLE in history")
        elif failed:
            unverifiable.append(name)
            print(f"  {name}: UNVERIFIABLE (a history search did not complete; diagnostics on stderr)")
        else:
            print(f"  {name}: clean")

    # Verdict precedence: exposure > unverifiable > clean. A definitive
    # positive finding must never be hidden behind a later search failure,
    # but the partial-coverage diagnostics print prominently either way.
    if errors:
        print("\nHistory verification could not be completed; these searches failed:", file=sys.stderr)
        for name, error in errors:
            print(f"  {name}: {error.operation} -> {error.detail}", file=sys.stderr)
        if unverifiable:
            print(f"\n{len(unverifiable)} constant(s) could not be verified: {', '.join(unverifiable)}", file=sys.stderr)
        if exposed:
            print(f"{len(exposed)} constant(s) are still reachable in Git history: {', '.join(exposed)}", file=sys.stderr)
            print(
                "History still exposes the keys and coverage was partial; fix the failures above, "
                "re-run after scrubbing. See docs/KEY_HISTORY_SCRUB.md.",
                file=sys.stderr,
            )
        else:
            print("No clean verdict is possible until every search completes; fix the failures above and re-run.", file=sys.stderr)

    if exposed:
        print(f"\n{len(exposed)} constant(s) still reachable in Git history: {', '.join(exposed)}")
        print("History still exposes the keys. See docs/KEY_HISTORY_SCRUB.md.")
        return 3
    if errors:
        return 2
    print(
        "\nNo PSP constant is reachable in any commit. History is clean for the spellings searched: "
        "contiguous hex, C/Python byte arrays, and hex strings, byte arrays and \\xNN escaped strings "
        "with any whitespace or line breaks (16- and 32-bit word literals are not searched)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
