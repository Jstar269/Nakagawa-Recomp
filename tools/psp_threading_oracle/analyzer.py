# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""CLI analyzer for threading oracle runs. Produces human-reviewable table."""

from __future__ import annotations

import argparse
import pathlib
import sys

try:
    from .parser import analyze_runs, human_table
except ImportError:
    # Direct invocation
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
    from psp_threading_oracle.parser import analyze_runs, human_table  # type: ignore[no-redef]

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Analyze PSP threading oracle captures")
    ap.add_argument("inputs", nargs="+", type=pathlib.Path, help="one or more captured result files (one per launch)")
    ap.add_argument("--out", type=pathlib.Path, help="write table to file")
    ap.add_argument("--provenance", type=str, help="label provenance: synthetic vs hardware (auto per-stream)")
    args = ap.parse_args(argv)
    # Exact file bytes (no newline translation), so a raw_capture_sha256 binds them.
    captures = [p.read_bytes() for p in args.inputs]
    # By default without explicit evidence context, parsing is UNVERIFIED_CAPTURE.
    # Callers with trusted hardware workflow should construct HardwareCaptureContext
    # out-of-band and call analyze_runs(captures, evidence_contexts=...) directly.
    analysis = analyze_runs(captures)
    table = human_table(analysis)
    # Provenance per input: no out-of-band context via the CLI, so every input
    # is UNVERIFIED_CAPTURE (never self-promotes to HARDWARE).
    for idx, label in enumerate(analysis["evidence_labels"]):
        table += f"\n# Input {idx}: {label} ({args.inputs[idx]})"
    if args.out:
        args.out.write_text(table, encoding="utf-8")
    else:
        sys.stdout.write(table)
    # Exit 1 when a launch mixes launch groups or repetitions are uneven;
    # missing cases and variable results are reported for the harness to judge.
    if analysis["launch_group_issues"] or analysis["repetition_issues"]:
        return 1
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
