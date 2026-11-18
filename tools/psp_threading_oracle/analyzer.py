# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""CLI analyzer for threading oracle runs. Produces human-reviewable table."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import sys

try:
    from .parser import analyze_runs, human_table
    from psp_oracle.protocol import ProtocolError
except ImportError:
    # Direct invocation
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
    from psp_threading_oracle.parser import analyze_runs, human_table  # type: ignore[no-redef]
    from psp_oracle.protocol import ProtocolError  # type: ignore[no-redef]


def _read_capture(path: pathlib.Path) -> tuple[str, str]:
    """Read exact UTF-8 capture bytes and return text plus their original digest."""

    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise ValueError(f"cannot read input ({type(exc).__name__})") from exc
    if raw.startswith(b"\xef\xbb\xbf"):
        raise ValueError("UTF-8 BOM is not supported")
    if raw.startswith((b"\xff\xfe", b"\xfe\xff")):
        raise ValueError("UTF-16 BOM is not supported")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ValueError("input is not valid UTF-8") from exc
    return text, hashlib.sha256(raw).hexdigest()


def _write_error(output: pathlib.Path | None, text: str) -> None:
    if output is None:
        sys.stderr.write(text + "\n")
    else:
        output.write_text(text + "\n", encoding="utf-8")

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Analyze PSP threading oracle captures")
    ap.add_argument("inputs", nargs="+", type=pathlib.Path, help="one or more captured result files (one per launch)")
    ap.add_argument("--out", type=pathlib.Path, help="write table to file")
    args = ap.parse_args(argv)
    texts: list[str] = []
    raw_hashes: list[str] = []
    for p in args.inputs:
        try:
            text, raw_hash = _read_capture(p)
        except ValueError as exc:
            _write_error(args.out, f"MALFORMED input {p.name}: {exc}")
            return 2
        texts.append(text)
        raw_hashes.append(raw_hash)
    # By default without explicit evidence context, parsing is UNVERIFIED_CAPTURE.
    # Callers with trusted hardware workflow should construct HardwareCaptureContext
    # out-of-band and call analyze_runs(texts, evidence_contexts=...) directly.
    try:
        analysis = analyze_runs(texts)
    except ProtocolError as exc:
        _write_error(args.out, f"MALFORMED capture: {exc}")
        return 2
    table = human_table(analysis)
    for idx, (path, label, raw_hash) in enumerate(
        zip(args.inputs, analysis["evidence_labels"], raw_hashes, strict=True)
    ):
        table += f"\n# Input {idx}: {label} ({path.name}; raw_capture_sha256={raw_hash})"
    if args.out:
        args.out.write_text(table, encoding="utf-8")
    else:
        sys.stdout.write(table)
    variable_cases = any(item["variable"] for item in analysis["case_reports"])
    return 0 if analysis["all_required_observed"] and not variable_cases else 2

if __name__ == "__main__":
    raise SystemExit(main())
