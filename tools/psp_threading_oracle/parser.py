# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Host-side parser for the PSP threading oracle.

Reuses the strict scalar protocol from tools/psp_oracle/protocol.py and adds
campaign-specific validation: campaign_version, run_id, attempt, expected case
coverage, per-case required fields, unknown-field rejection, and evidence
separation.

Status semantics (Stage 3):
  PASS = MEASURED: experiment completed structurally and all required
         measurement fields were captured. The measured PSP API return code
         remains raw data in `result` / out* ; an API returning an error can
         still be PASS if the experiment was measuring that outcome.
  FAIL = ERROR: the harness itself could not produce a valid measurement
         (e.g., thread creation failed before measurement, allocation failed,
         probe not measurable). FAIL records must still carry result,
         canary and attempt, plus any partial out fields and optional
         error/detail; they are distinguished from raw API errors.
  SKIP = NOT_MEASURABLE_WITH_CURRENT_HARNESS: bounded scalar-only or
         orientation not established; no unsafe memory read performed.
         Like FAIL, it carries only the outs it filled, plus error/detail.
  Only a PASS record must carry every required out field and may not carry
  error/detail.
  Unknown PSP semantics are never encoded as expected PASS/FAIL; they are
  preserved as raw data (UNKNOWN_RESULTS_PRESERVED).

Evidence model (Stage 4):
  RAW CAPTURE TEXT MUST NEVER LABEL ITSELF HARDWARE_MEASURED.
  Metadata inside the captured stream is untrusted/declarative.
  parse_threading_output(text) -> PARSED_CAPTURE (UNVERIFIED_CAPTURE by
  default).  An explicit out-of-band evidence context is required to promote
  to SYNTHETIC_TEST_ONLY or HARDWARE_MEASURED.

  - Default without context: UNVERIFIED_CAPTURE
  - Synthetic unit fixtures: SYNTHETIC_TEST_ONLY via SyntheticCaptureContext
  - Hardware: HARDWARE_MEASURED via HardwareCaptureContext that binds at
    minimum raw_capture_sha256, binary_sha256, source_commit, model,
    firmware, capture_timestamp/run_id, runner/tool identity.
  A hand-edited file with plausible metadata remains UNVERIFIED_CAPTURE.
"""

from __future__ import annotations

import hashlib
import json
import pathlib
import re
from collections.abc import Sequence
from dataclasses import dataclass
from typing import Any

import sys

try:
    import psp_oracle.protocol  # noqa: F401  (tools/ already importable)
except ImportError:
    # Direct use without tools/ on sys.path: append, never shadow the stdlib.
    sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))
from psp_oracle.protocol import (  # noqa: E402
    _ALL_ZERO_RE,
    ProtocolError,
    _HEX_RE,
    is_unmeasured,
    parse_output,
)

CAMPAIGN_VERSION = "psp-threading-v1"
EXPECTED_TEST_ID = "PSP-THREAD-001"
CANARY_EXPECTED = "0xa5a5a5a5"
ATTEMPT_RE = re.compile(r"^0x[0-9a-fA-F]{1,8}$")

MATRIX_PATH = pathlib.Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "psp_threading_oracle" / "matrix.json"

def load_matrix(path: pathlib.Path | None = None) -> dict[str, Any]:
    p = path or MATRIX_PATH
    returned: dict[str, Any] = json.loads(p.read_text(encoding="utf-8"))
    return returned

GATING_SCOPE = "gating"
FULL_SCOPE = "full"
FULL_ONLY_CLASSIFICATION = "FULL_ONLY_HYPOTHESIS"


def expected_case_ids(matrix: dict[str, Any] | None = None, scope: str = GATING_SCOPE) -> set[str]:
    """Cases a campaign of ``scope`` must observe.

    The gating minimum leaves out the full-campaign-only cases, which the PSP
    side emits only when built with ``PSP_THREADING_FULL``.
    """
    m = matrix or load_matrix()
    if scope == FULL_SCOPE:
        return {c["case_id"] for c in m["cases"]}
    if scope != GATING_SCOPE:
        raise ValueError(f"unknown campaign scope {scope!r}; expected {GATING_SCOPE!r} or {FULL_SCOPE!r}")
    gating = {
        c["case_id"] for c in m["cases"]
        if c.get("expected_classification") != FULL_ONLY_CLASSIFICATION
    }
    planned = m["launch_plan"]["gating"]["unique_cases"]
    if len(gating) != planned:
        raise ProtocolError(f"matrix has {len(gating)} gating cases but launch_plan.gating.unique_cases is {planned}")
    return gating

STACK_PROBE_MAX_BYTES = 64
# Optional extension namespace: identifier-style future_ fields only.
# Grammar: future_[A-Za-z0-9][A-Za-z0-9_]*
# Bare "future", "future_" and "future__invalid" are NOT accepted.
OPTIONAL_FIELD_RE = re.compile(r"^future_[A-Za-z0-9][A-Za-z0-9_]*$")
OPTIONAL_FIELD_PREFIX = "future_"

UNVERIFIED_CAPTURE = "UNVERIFIED_CAPTURE"
SYNTHETIC_TEST_ONLY = "SYNTHETIC_TEST_ONLY"
HARDWARE_MEASURED = "HARDWARE_MEASURED"

@dataclass(frozen=True)
class SyntheticCaptureContext:
    """Explicit synthetic fixture context. Any capture parsed with this
    context is labelled SYNTHETIC_TEST_ONLY regardless of metadata.
    """
    kind: str = "synthetic"

@dataclass(frozen=True)
class HardwareCaptureContext:
    """Out-of-band hardware provenance binding.
    The raw capture text never self-asserts this; the runner constructs it
    from trusted local workflow record and binds the raw bytes hash.
    """
    raw_capture_sha256: str  # hex sha256 of exact raw capture bytes
    binary_sha256: str  # lowercase 64 hex
    source_commit: str  # 40-64 hex
    model: str
    firmware: str
    capture_timestamp: str  # run identifier or timestamp, non-empty
    run_id: str  # hex run_id that must match metadata run_id
    runner: str | None = None

    def is_bound(self) -> bool:
        if not re.fullmatch(r"[0-9a-f]{64}", self.raw_capture_sha256):
            return False
        if not re.fullmatch(r"[0-9a-f]{64}", self.binary_sha256):
            return False
        if not re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", self.source_commit):
            return False
        if not self.model or is_unmeasured(self.model):
            return False
        if not self.firmware or is_unmeasured(self.firmware):
            return False
        if not self.capture_timestamp:
            return False
        if not self.runner:
            return False
        if not _HEX_RE.fullmatch(self.run_id):
            return False
        # binary all-zero is placeholder, not hardware
        if self.binary_sha256 == "0"*64:
            return False
        if self.source_commit == "0"*40 or self.source_commit == "0"*64:
            return False
        return True

def _sha256_hex(raw: str | bytes) -> str:
    data = raw if isinstance(raw, bytes) else raw.encode("utf-8")
    return hashlib.sha256(data).hexdigest()


def _hex_value(value: str) -> int | None:
    """The integer a protocol hex scalar names, or None when it is not one."""
    return int(value, 16) if _HEX_RE.fullmatch(value) else None


def _canonical_hex(value: str) -> str:
    """One spelling per value (the harness's ``0x%08x``) for identity keys."""
    return f"0x{int(value, 16):08x}"


def evidence_label(parsed: Any, evidence_context: Any | None = None,
                   raw_text: str | bytes | None = None) -> str:
    """Return evidence provenance label for a parsed stream.

    Without an explicit out-of-band context, a stream is never
    HARDWARE_MEASURED even if its metadata looks plausible. This prevents
    arbitrary text from self-promoting via declarative fields.

    - evidence_context is None -> UNVERIFIED_CAPTURE (default for raw parse)
    - SyntheticCaptureContext -> SYNTHETIC_TEST_ONLY
    - HardwareCaptureContext with valid binding, matching raw hash and run_id
      (compared by value), source=psp, and no stream metadata value that
      contradicts the context -> HARDWARE_MEASURED; else UNVERIFIED_CAPTURE
    ``raw_text`` should be the capture's exact bytes; a str is hashed as its
    UTF-8 encoding, so it matches only text decoded without newline
    translation. The stream's own metadata never selects a label: placeholder
    metadata without a context is UNVERIFIED_CAPTURE like any other capture.
    """
    meta = dict(parsed.metadata)

    # Explicit synthetic context always -> synthetic
    if isinstance(evidence_context, SyntheticCaptureContext):
        return SYNTHETIC_TEST_ONLY
    if isinstance(evidence_context, HardwareCaptureContext):
        if raw_text is None:
            return UNVERIFIED_CAPTURE
        computed = _sha256_hex(raw_text)
        if computed != evidence_context.raw_capture_sha256.lower():
            return UNVERIFIED_CAPTURE
        if not evidence_context.is_bound():
            return UNVERIFIED_CAPTURE
        # The metadata run_id must name the context's run_id (is_bound checked its form).
        stream_run_id = _hex_value(meta.get("run_id", ""))
        if stream_run_id is None or stream_run_id != int(evidence_context.run_id, 16):
            return UNVERIFIED_CAPTURE
        if meta.get("source") != "psp":
            return UNVERIFIED_CAPTURE
        if _metadata_contradicts(meta, evidence_context):
            return UNVERIFIED_CAPTURE
        return HARDWARE_MEASURED

    # No explicit context: never self-promote, whatever the metadata says.
    return UNVERIFIED_CAPTURE


def _metadata_contradicts(meta: dict[str, str], context: HardwareCaptureContext) -> bool:
    """True when the stream names a build or device other than the context's.

    The PSP emitter writes placeholders (``unknown``, all-zero digests) that the
    runner's context supplies; a measured value in the stream must equal it.
    """
    for field in ("binary_sha256", "source_commit"):
        value = meta.get(field)
        if value is not None and not _ALL_ZERO_RE.fullmatch(value):
            if value.lower() != getattr(context, field).lower():
                return True
    for field in ("model", "firmware"):
        value = meta.get(field)
        if value is not None and not is_unmeasured(value):
            if value != getattr(context, field):
                return True
    return False

def _build_required_map(matrix: dict[str, Any]) -> dict[str, dict[str, Any]]:
    out: dict[str, dict[str, Any]] = {}
    for c in matrix["cases"]:
        cid = c["case_id"]
        # required_out_fields may be absent in older matrix – treat as empty
        req = c.get("required_out_fields", c.get("required_fields", []))
        # also handle measurement_fields legacy – not used for strict enforcement
        opt = c.get("optional_out_fields", c.get("optional_fields", []))
        # launch group validation
        out[cid] = {
            "required": set(req),
            "optional": set(opt),
            "launch": c.get("launch"),
            "phase": c.get("phase"),
            "control_group": c.get("control_group"),
        }
    return out

_RECORD_IDENTITY_FIELDS = frozenset({"result", "canary", "attempt"})
# Harness-error annotations (protocol.parse_output passes them unchecked); only a
# record that is not a measurement (FAIL/SKIP) may carry them.
_ERROR_FIELDS = frozenset({"error", "detail"})
# Probe-length fields: bounded by STACK_PROBE_MAX_BYTES, and accepted only on a
# case whose matrix entry lists them in required_out_fields/optional_out_fields.
_PROBE_LENGTH_FIELDS = frozenset({"probe_len", "stack_bytes", "window_bytes", "arg_bytes"})


def _check_record(r: Any, spec: dict[str, Any]) -> None:
    """Validate one record against its matrix case ``spec`` (one pass per field)."""
    vals = dict(r.values)
    case = r.case_id
    if r.test_id != EXPECTED_TEST_ID:
        raise ProtocolError(f"unexpected test_id {r.test_id!r} expected {EXPECTED_TEST_ID!r}")
    # result (raw API return code) is mandatory; parse_output already checked it is hex.
    if "result" not in vals:
        raise ProtocolError(f"test {case!r} missing required field 'result' (raw return code)")
    canary = vals.get("canary")
    if canary is None:
        raise ProtocolError(f"test {case!r} missing canary")
    if canary.lower() != CANARY_EXPECTED:
        raise ProtocolError(f"test {case!r} has invalid canary {canary!r} expected {CANARY_EXPECTED}")
    attempt = vals.get("attempt")
    if attempt is None:
        raise ProtocolError(f"test {case!r} missing required field 'attempt' (run/attempt identity)")
    if not ATTEMPT_RE.fullmatch(attempt):
        raise ProtocolError(f"test {case!r} field attempt must be hex 0x...")
    if int(attempt, 16) > 255:
        raise ProtocolError(f"test {case!r} attempt out of sane range {attempt!r}")
    measured = r.status == "PASS"
    declared = spec["required"] | spec["optional"]
    # Only a measurement must carry every required out; a FAIL/SKIP record
    # carries the fields it could fill (module docstring, status semantics).
    if measured:
        missing = sorted(spec["required"] - vals.keys())
        if missing:
            raise ProtocolError(f"test {case!r} missing required fields: {', '.join(missing)}")
    for k, v in vals.items():
        if k in _RECORD_IDENTITY_FIELDS:
            continue
        if k in _ERROR_FIELDS and not measured:
            continue
        if k in declared:
            # Every matrix-declared field is a protocol hex scalar.
            if not _HEX_RE.fullmatch(v):
                raise ProtocolError(f"test {case!r} field {k} must be hexadecimal 0x...")
            if k in _PROBE_LENGTH_FIELDS:
                n = int(v, 16)
                if n > STACK_PROBE_MAX_BYTES:
                    raise ProtocolError(f"test {case!r} field {k} oversized {n} > {STACK_PROBE_MAX_BYTES}")
            continue
        # Near-miss extension prefixes: bare future, future_, future__invalid.
        if k in {"future", OPTIONAL_FIELD_PREFIX} or k.startswith("future__"):
            raise ProtocolError(f"test {case!r} extension prefix {k!r} rejected; valid extensions are under future_<name> namespace")
        if OPTIONAL_FIELD_RE.fullmatch(k):
            if not _HEX_RE.fullmatch(v):
                raise ProtocolError(f"test {case!r} optional field {k} must be hex")
            continue
        # Any other field is unexpected: fail closed even if hex.
        raise ProtocolError(f"test {case!r} has unexpected field {k!r} value {v!r} (possible misspelling, unknown hex field rejected)")


def parse_threading_output(text: str, *, require_metadata: bool = True,
                           matrix: dict[str, Any] | None = None) -> Any:
    """Parse and validate a threading oracle stream.

    Extends protocol.parse_output with campaign checks, strict per-case
    schemas, and evidence separation (no self-promotion).
    ``require_metadata=False`` only tolerates a stream without campaign
    metadata; every record is still checked against the matrix. ``matrix``
    lets a caller that already loaded matrix.json pass it in.
    """
    parsed = parse_output(text, require_metadata=require_metadata)
    meta = dict(parsed.metadata)
    m = matrix if matrix is not None else load_matrix()
    if require_metadata:
        cv = meta.get("campaign_version")
        if cv is None:
            raise ProtocolError("metadata missing campaign_version")
        if cv != CAMPAIGN_VERSION:
            raise ProtocolError(f"unsupported campaign_version {cv!r}, expected {CAMPAIGN_VERSION!r}")
        if m.get("campaign_version") != cv:
            raise ProtocolError(f"campaign_version {cv!r} does not match matrix {m.get('campaign_version')!r}")
        run_id = meta.get("run_id")
        if run_id is None or not _HEX_RE.fullmatch(run_id):
            raise ProtocolError("run_id must be hexadecimal 0x...")
    required_map = _build_required_map(m)
    for r in parsed.results:
        # A case the matrix does not define is unexpected: fail closed.
        if r.case_id not in required_map:
            raise ProtocolError(f"test {r.case_id!r} is not defined in matrix.json (unexpected case)")
        _check_record(r, required_map[r.case_id])
    return parsed

# Per-record identity fields: they say which launch/attempt produced a record,
# not what the PSP did, so they never enter the stability comparison.
_IDENTITY_FIELDS = frozenset({"attempt"})


def _stability_key(result: dict[str, Any]) -> str:
    values = {k: v for k, v in result["values"].items() if k not in _IDENTITY_FIELDS}
    return json.dumps({"status": result["status"], "values": values}, sort_keys=True)


def _launch_group_issues(runs: list[Any], run_ids: list[str],
                         required_map: dict[str, dict[str, Any]], gating: set[str]) -> list[str]:
    """A launch is one phase build (one launch group) or a CASE=all build that
    carries every gating case; anything else mixes launch groups."""
    issues: list[str] = []
    for idx, parsed in enumerate(runs):
        cases = {r.case_id for r in parsed.results}
        groups = sorted({str(required_map[c]["launch"]) for c in cases})
        if len(groups) > 1 and not gating <= cases:
            issues.append(
                f"run {idx} ({run_ids[idx]}) mixes launch groups {', '.join(groups)} "
                "without carrying every gating case (CASE=all)"
            )
    return issues


def _repetition_issues(counts: dict[str, int]) -> list[str]:
    observed = {cid: n for cid, n in counts.items() if n}
    if len(set(observed.values())) <= 1:
        return []
    most = max(observed.values())
    short = sorted(f"{cid} x{n}" for cid, n in observed.items() if n != most)
    return [f"uneven repetitions: most cases observed {most} times, but {', '.join(short)}"]


def analyze_runs(texts: Sequence[str | bytes], evidence_contexts: list[Any] | None = None,
                 scope: str = GATING_SCOPE) -> dict[str, Any]:
    """Analyze one or more runs (for repetition handling).

    Each text is one launch's output. The analyzer validates run/attempt
    identity explicitly via metadata run_id and per-record attempt, not list
    position. Divergent observations are retained.

    ``scope`` selects the required case set: ``"gating"`` (the 28-case
    minimum) or ``"full"`` (every matrix case, PSP_THREADING_FULL builds).
    Each launch must be one launch group or a CASE=all build, and every
    observed case must be observed equally often; violations are listed in
    ``launch_group_issues`` / ``repetition_issues`` and clear ``campaign_valid``.
    ``repetitions`` is the fewest observations of any required case;
    ``repetitions_complete`` says whether that reaches ``expected_repetitions``,
    the repeat plan's observations per gating case, so a one-launch campaign
    reports its repetition shortfall instead of passing for a repeated one.

    evidence_contexts, if provided, must align 1:1 with texts and are used
    to label each run's evidence; default is UNVERIFIED_CAPTURE per run.
    Pass each capture's exact bytes so a context's raw_capture_sha256 can bind
    it (bytes are decoded as UTF-8 for parsing); a str is hashed as UTF-8.
    run_id and attempt are identities by value: ``0x1`` and ``0x00000001`` are
    the same launch, reported in the harness's ``0x%08x`` form.
    """
    matrix = load_matrix()
    required = expected_case_ids(matrix, scope)
    gating = expected_case_ids(matrix, GATING_SCOPE)
    matrix_cases = expected_case_ids(matrix, FULL_SCOPE)
    required_map = _build_required_map(matrix)
    inputs_by_case = {c["case_id"]: str(c.get("inputs", "")) for c in matrix["cases"]}
    runs: list[Any] = []
    run_ids: list[str] = []
    for text in texts:
        decoded = text.decode("utf-8") if isinstance(text, bytes) else text
        # parse_threading_output requires metadata and a hex run_id.
        parsed = parse_threading_output(decoded, matrix=matrix)
        runs.append(parsed)
        run_ids.append(_canonical_hex(dict(parsed.metadata)["run_id"]))
    # Each launch must be distinct: a duplicate run_id is impossible.
    if len(set(run_ids)) != len(run_ids):
        raise ProtocolError(f"duplicate run_id detected: {run_ids}")
    # A (run_id, attempt, case_id) observation cannot repeat: run_ids are unique
    # here, parse_output rejects a repeated case within one stream, and
    # parse_threading_output rejects a case outside the matrix.
    from collections import defaultdict
    per_case: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for run_idx, parsed in enumerate(runs):
        run_id = run_ids[run_idx]
        for r in parsed.results:
            attempt = _canonical_hex(dict(r.values)["attempt"])
            per_case[r.case_id].append({
                "run": run_idx,
                "run_id": run_id,
                "attempt": attempt,
                "result": r.as_dict(),
            })
    observed = set(per_case.keys())
    optional_observed = sorted((observed & matrix_cases) - required)
    reported = required | set(optional_observed)
    case_reports: list[dict[str, Any]] = []
    for case_id in sorted(reported):
        obs = per_case.get(case_id, [])
        expected_launch = required_map.get(case_id, {}).get("launch")
        if not obs:
            case_reports.append({"case_id": case_id, "observed": False, "stable": False, "runs": [], "variable": False, "run_count": 0, "expected_launch": expected_launch, "required": True, "inputs": inputs_by_case.get(case_id, "")})
            continue
        # Divergent observations are preserved; status is part of the comparison.
        # One observation is not a repeat: stable is None (SINGLE), not True.
        distinct = len({_stability_key(o["result"]) for o in obs})
        stable: bool | None = None if len(obs) < 2 else distinct == 1
        case_reports.append({
            "case_id": case_id,
            "observed": True,
            "stable": stable,
            "variable": distinct > 1,
            "observations": obs,
            "run_count": len(obs),
            "expected_launch": expected_launch,
            "required": case_id in required,
            "inputs": inputs_by_case.get(case_id, ""),
        })
    missing = sorted(required - observed)
    launch_group_issues = _launch_group_issues(runs, run_ids, required_map, gating)
    counts = {cid: len(per_case.get(cid, [])) for cid in reported}
    repetition_issues = _repetition_issues(counts)
    repetitions = min(counts[cid] for cid in required)
    repeat_plan = matrix["launch_plan"]["gating"]["repeat"]
    expected_repetitions, remainder = divmod(repeat_plan["records"],
                                             matrix["launch_plan"]["gating"]["unique_cases"])
    if remainder or expected_repetitions < 1:
        raise ProtocolError(f"launch_plan.gating.repeat.records {repeat_plan['records']} is not a "
                            "whole number of observations per gating case")
    evidence_labels: list[str] = []
    if evidence_contexts is not None:
        if len(evidence_contexts) != len(texts):
            raise ProtocolError("evidence_contexts length must match texts")
        for text, ctx, parsed in zip(texts, evidence_contexts, runs, strict=True):
            evidence_labels.append(evidence_label(parsed, evidence_context=ctx, raw_text=text))
    else:
        for text, parsed in zip(texts, runs, strict=True):
            evidence_labels.append(evidence_label(parsed, evidence_context=None, raw_text=text))

    return {
        "campaign_version": CAMPAIGN_VERSION,
        "scope": scope,
        "required_cases": sorted(required),
        "case_reports": case_reports,
        "missing_cases": missing,
        "optional_cases_observed": optional_observed,
        "all_required_observed": not missing,
        "launch_group_issues": launch_group_issues,
        "repetition_issues": repetition_issues,
        "repetitions": repetitions,
        "expected_repetitions": expected_repetitions,
        "repetitions_complete": repetitions >= expected_repetitions,
        "campaign_valid": not missing and not launch_group_issues and not repetition_issues,
        "launch_count": len(runs),
        "run_ids": run_ids,
        "expected_launches_gating": matrix["launch_plan"]["gating"]["launches"],
        "expected_launches_repeat": matrix["launch_plan"]["gating"]["repeat"]["launches"],
        "record_count": sum(len(r.results) for r in runs),
        "evidence_labels": evidence_labels,
        "required_map": {k: {"required": sorted(v["required"]), "launch": v["launch"]} for k,v in required_map.items()},
    }

# Status semantics (module docstring): only PASS is a measurement.
_STATUS_COLUMN = {"PASS": "MEASURED", "FAIL": "ERROR", "SKIP": "NOT_MEASURABLE"}


def _measured_column(statuses: set[str]) -> str:
    if not statuses:
        return "NOT_MEASURED"
    if len(statuses) > 1:
        return "MIXED"
    (status,) = statuses
    return _STATUS_COLUMN.get(status, status)


def human_table(analysis: dict[str, Any]) -> str:
    lines: list[str] = []
    lines.append(f"Campaign {analysis['campaign_version']}  launches={analysis['launch_count']}  required={len(analysis['required_cases'])}  records={analysis['record_count']}")
    lines.append(f"{'CASE_ID':12} {'INPUT':20} {'RESULT':12} {'STABLE':8} {'MEASURED'}")
    lines.append("-"*80)
    for rep in analysis["case_reports"]:
        cid = rep["case_id"]
        inputs = rep.get("inputs", "")[:20]
        if not rep["observed"]:
            status = "MISSING"
        elif rep["stable"] is None:
            status = "SINGLE"
        else:
            status = "STABLE" if rep["stable"] else "VARIABLE"
        statuses = {o["result"]["status"] for o in rep.get("observations", [])}
        result = "/".join(sorted(statuses))
        lines.append(f"{cid:12} {inputs:20} {result:12} {status:8} {_measured_column(statuses)}")
    if analysis["missing_cases"]:
        lines.append(f"Missing: {', '.join(analysis['missing_cases'])}")
    for issue in analysis.get("launch_group_issues", []) + analysis.get("repetition_issues", []):
        lines.append(f"Invalid: {issue}")
    if "evidence_labels" in analysis:
        lines.append(f"Evidence: {', '.join(analysis['evidence_labels'])}")
    return "\n".join(lines) + "\n"
