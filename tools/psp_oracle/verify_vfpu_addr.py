#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify HQ-1 (#296) measured VFPU register sets against the production decode.

Raw-first: parse the hardware lanes payloads and independently derive the
destination register permutation per (width, field) case, then compare with
tools/codegen.py's vreg_indices (the PPSSPP-cited decode that IND-2 targets).
No expected values from PPSSPP are seeded into the measurement itself.

The probe zeroes all 128 regs, loads pattern values 0x3f800001..4 (unique per
lane) at phys regs {0,1,2,3} (S000..S003), executes one raw vmov.<w> with dst
field E and src field 0x00, then reads all 128 regs.  A physical register p is
the destination of lane i if p holds pattern value i+1 and p is not the source
register for that lane (phys i).  When a lane's value disappears entirely its
source register was overwritten and is itself a destination (identity/rotation
cases resolve through the values that DID move).
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))
from codegen import vreg_indices  # noqa: E402

PAT = [0x3F800001, 0x3F800002, 0x3F800003, 0x3F800004]


def scalar_map_field_to_phys(field: int) -> int:
    """Independent Stage-A formula confirmed by the 128-lane measurement."""
    row = (field >> 5) & 3
    mtx = (field >> 2) & 7
    col = field & 3
    return mtx * 16 + col * 4 + row


def parse_lanes(lanes: str) -> dict[int, int]:
    """phys -> raw word from the probe's lanes= payload."""
    out = {}
    for tok in lanes.split(","):
        tok = tok.strip()
        if not tok:
            continue
        p, _, v = tok.partition(":")
        phys, value = int(p, 16), int(v, 16)
        if not 0 <= phys < 128 or phys in out:
            raise ValueError(f"invalid or duplicate physical register {p!r}")
        out[phys] = value
    if set(out) != set(range(128)):
        raise ValueError(f"lanes payload has {len(out)}/128 physical registers")
    return out


def derive_perm(width: str, lanes: dict[int, int]) -> list[int]:
    """Lane i's destination phys, derived only from measured lanes."""
    n = {"s": 1, "p": 2, "t": 3, "q": 4}[width]
    perm = []
    for i in range(n):
        val = PAT[i]
        holders = sorted(p for p, v in lanes.items() if v == val)
        non_src = [p for p in holders if p != i]
        if non_src:
            perm.append(non_src[0])
        elif i in holders:
            perm.append(i)  # identity copy: dst == src
        else:
            perm.append(-1)  # value overwritten: dst took its place
    return perm


def _scalar_map(raw: str) -> dict[int, int]:
    lines = [
        line for line in raw.splitlines()
        if line.startswith("NAKAGAWA_PSP_TEST") and "case_id=S:map" in line
    ]
    if len(lines) != 1:
        raise ValueError(f"expected exactly one S:map record, found {len(lines)}")
    match = re.search(r"(?:^|\s)map=(\S+)", lines[0])
    if match is None:
        raise ValueError("S:map record has no map= field")
    pairs: dict[int, int] = {}
    for token in match.group(1).split(","):
        field_text, separator, phys_text = token.partition(":")
        if not separator:
            raise ValueError(f"malformed S:map pair {token!r}")
        field, physical = int(field_text, 16), int(phys_text, 16)
        if not 0 <= field < 128 or not 0 <= physical < 128:
            raise ValueError(f"S:map pair is outside the 128-register file: {token!r}")
        if field in pairs:
            raise ValueError(f"duplicate S:map field {field:02x}")
        pairs[field] = physical
    if set(pairs) != set(range(128)):
        missing = sorted(set(range(128)) - set(pairs))
        raise ValueError(f"S:map coverage is incomplete: {len(pairs)}/128 fields; missing {missing[:8]}")
    return pairs


def _wide_cases(raw: str) -> dict[tuple[str, int], dict[int, int]]:
    cases: dict[tuple[str, int], dict[int, int]] = {}
    for line in raw.splitlines():
        if not line.startswith("NAKAGAWA_PSP_TEST"):
            continue
        match = re.search(r"case_id=([sptq]):0x([0-9a-f]+)", line)
        if match is None:
            continue
        key = (match.group(1), int(match.group(2), 16))
        if key in cases:
            raise ValueError(f"duplicate wide case {key[0]}:0x{key[1]:02x}")
        lane_match = re.search(r"(?:^|\s)lanes=(\S+)", line)
        if lane_match is None:
            raise ValueError(f"wide case {key[0]}:0x{key[1]:02x} has no lanes= payload")
        cases[key] = parse_lanes(lane_match.group(1))
    return cases


def _expected_wide_cases(fixture_path: Path) -> set[tuple[str, int]]:
    fixture = json.loads(fixture_path.read_text(encoding="utf-8"))
    width_names = {1: "s", 2: "p", 3: "t", 4: "q"}
    expected = {
        (width_names[case["width"]], int(case["encoding"], 16))
        for case in fixture["wide_cases"]
    }
    measured_count = fixture["measurement_boundary"]["wide_encodings_measured"]
    if len(expected) != measured_count:
        raise ValueError("published fixture has duplicate or malformed wide-case expectations")
    return expected


def verify_payloads(payloads: list[str], expected_wide_cases: set[tuple[str, int]]) -> int:
    """Verify two payloads have complete, identical coverage and match codegen."""

    if len(payloads) != 2:
        print(f"verify_vfpu_addr: expected two raw payloads, found {len(payloads)}")
        return 1
    scalar_maps: list[dict[int, int]] = []
    wide_runs: list[dict[tuple[str, int], dict[int, int]]] = []
    try:
        for run_index, raw in enumerate(payloads, 1):
            scalar_maps.append(_scalar_map(raw))
            wide = _wide_cases(raw)
            if set(wide) != expected_wide_cases:
                missing = sorted(expected_wide_cases - set(wide))
                extra = sorted(set(wide) - expected_wide_cases)
                print(
                    f"verify_vfpu_addr: run{run_index} wide-case coverage is "
                    f"{len(wide)}/{len(expected_wide_cases)}; missing={missing}, extra={extra}"
                )
                return 1
            wide_runs.append(wide)
    except (ValueError, KeyError, TypeError) as exc:
        print(f"verify_vfpu_addr: malformed measurement payload ({type(exc).__name__}: {exc})")
        return 1

    if scalar_maps[0] != scalar_maps[1]:
        print("verify_vfpu_addr: Stage A S:map differs between runs")
        return 1
    if wide_runs[0] != wide_runs[1]:
        print("verify_vfpu_addr: Stage B wide-case lane payloads differ between runs")
        return 1

    bad = 0
    for field, physical in sorted(scalar_maps[0].items()):
        expected = scalar_map_field_to_phys(field)
        if physical != expected:
            bad += 1
            print(f"SCALAR MISMATCH field={field:02x} measured={physical:02x} formula={expected:02x}")

    failures = 0
    print(f"{'case':8} {'measured':22} {'codegen':22} {'match'}")
    width_sizes = {"s": 1, "p": 2, "t": 3, "q": 4}
    for width, field in sorted(expected_wide_cases):
        lanes = wide_runs[0][(width, field)]
        measured = derive_perm(width, lanes)
        predicted = vreg_indices(field, width_sizes[width])
        ok = measured == predicted
        if not ok:
            failures += 1
        print(f"{width}:0x{field:02x}  "
              f"[{','.join(f'{p:02x}' if p >= 0 else '--' for p in measured)}] "
              f"[{','.join(f'{p:02x}' for p in predicted)}]  {'OK' if ok else '*** MISMATCH ***'}")
    print(f"\nStage A: {len(scalar_maps[0])}/128 single-encoding pairs checked, {bad} formula mismatches")
    print(f"Stage B: {len(wide_runs[0])}/{len(expected_wide_cases)} wide cases, {failures} decode mismatches")
    return 1 if bad or failures else 0


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    results_dir = root / "oracle" / "hardware-results"
    fixture_path = root / "tests" / "fixtures" / "vfpu_addressing" / "hardware_vfpu_addr_001.json"
    payloads: list[str] = []
    try:
        for index in (1, 2):
            path = results_dir / f"vfpu-addr-run{index}.json"
            document = json.loads(path.read_text(encoding="utf-8"))
            runs = document.get("runs") if isinstance(document, dict) else None
            raw = runs[0].get("raw") if isinstance(runs, list) and runs and isinstance(runs[0], dict) else None
            if not isinstance(raw, str):
                raise ValueError(f"run{index} has no runs[0].raw string")
            payloads.append(raw)
        expected = _expected_wide_cases(fixture_path)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        print(f"verify_vfpu_addr: could not load complete inputs ({type(exc).__name__})")
        return 1
    return verify_payloads(payloads, expected)


if __name__ == "__main__":
    sys.exit(main())
