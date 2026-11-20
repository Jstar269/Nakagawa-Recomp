#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Generate a `git filter-repo --replace-text` spec that purges the PSP KIRK/amctrl
constants from Git history.

This script contains no key material. It reads the constant values from the local
key file (a private local binding, outside the public candidate) and emits one replacement line per textual encoding
the tree has ever used, so filter-repo rewrites every historical occurrence to a
fixed placeholder regardless of hex/byte-array spelling, case, or padding.

The OUTPUT contains key material and must never be committed. It is written to a
path you choose (use a temp/ignored location) and deleted after the scrub. This
script refuses to write inside the working tree's tracked area by default.

Usage:
  python tools/gen_key_scrub_spec.py --out "$TMP/pgd-key-replacements.txt"
  git filter-repo --replace-text "$TMP/pgd-key-replacements.txt"   # on a mirror clone

See docs/KEY_HISTORY_SCRUB.md for the full procedure.
"""

from __future__ import annotations

import argparse
import os
import sys

# Reuse the exact same encoding set the verifier checks, so "generate" and "verify"
# can never drift apart.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_key_scrub import encodings, key_values  # noqa: E402

PLACEHOLDER = "PSP_CONSTANT_REDACTED"


def build_spec(values: dict[str, bytes]) -> list[str]:
    lines: list[str] = []
    seen: set[str] = set()
    for raw in values.values():
        for form in encodings(raw):
            if form in seen:
                continue
            seen.add(form)
            # literal: prefix disables regex interpretation; exact-match replacement.
            lines.append(f"literal:{form}==>{PLACEHOLDER}")
    return lines


def _is_inside(path: str, root: str) -> bool:
    """Whether ``path`` lies under ``root`` once links and case are resolved.

    Both sides go through realpath (a symlink or junction into the checkout
    counts as inside) and normcase (Windows compares paths case-insensitively,
    while ntpath.commonpath returns the prefix in the first path's case). A
    path on another drive cannot be inside the root.
    """
    path_key = os.path.normcase(os.path.realpath(path))
    root_key = os.path.normcase(os.path.realpath(root))
    try:
        return os.path.commonpath([path_key, root_key]) == root_key
    except ValueError:  # different drives
        return False


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--keys", default=os.environ.get("SR_PGD_KEYS") or os.path.join("keys", "pgd_keys.txt"))
    parser.add_argument("--out", required=True, help="output spec path (use a temp/ignored location)")
    parser.add_argument("--force", action="store_true", help="allow writing inside the repo tree")
    args = parser.parse_args(argv)

    if not os.path.isfile(args.keys):
        print(f"key file not found: {args.keys} (private local binding)", file=sys.stderr)
        return 2

    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_abs = os.path.abspath(args.out)
    if not args.force and _is_inside(out_abs, repo_root):
        print(
            f"refusing to write the spec inside the repo ({out_abs}); it contains key\n"
            "material. Choose a temp path, or pass --force if you have gitignored it.",
            file=sys.stderr,
        )
        return 2

    values = key_values(args.keys)
    if not values:
        print(f"no usable constants parsed from {args.keys}", file=sys.stderr)
        return 2

    lines = build_spec(values)
    try:
        # Owner-only from creation: the spec holds every constant. An existing
        # file keeps its old mode under O_CREAT, so it is tightened as well.
        fd = os.open(out_abs, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
            if hasattr(os, "fchmod"):
                os.fchmod(handle.fileno(), 0o600)
            handle.write("\n".join(lines) + "\n")
    except OSError as exc:
        print(f"cannot write the spec to {out_abs}: {exc.strerror or exc}", file=sys.stderr)
        return 2
    print(f"wrote {len(lines)} replacement rules for {len(values)} constants -> {out_abs}")
    print("This file contains key material. Delete it after the scrub; never commit it.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
