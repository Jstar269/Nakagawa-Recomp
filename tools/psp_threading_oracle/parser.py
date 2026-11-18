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
         probe not measurable). FAIL records must still carry canary/attemp
         and any partial fields but are distinguished from raw API errors.
  SKIP = NOT_MEASURABLE_WITH_CURRENT_HARNESS: bounded scalar-only or
         orientation not established; no unsafe memory read performed.
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
CANARY_RE = re.compile(r"^0x[0-9a-fA-F]{8}$")
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

def _sha256_hex(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()

def evidence_label(parsed: Any, evidence_context: Any | None = None, raw_text: str | None = None) -> str:
    """Return evidence provenance label for a parsed stream.

    Without an explicit out-of-band context, a stream is never
    HARDWARE_MEASURED even if its metadata looks plausible. This prevents
    arbitrary text from self-promoting via declarative fields.

    - evidence_context is None -> UNVERIFIED_CAPTURE (default for raw parse)
    - SyntheticCaptureContext -> SYNTHETIC_TEST_ONLY
    - HardwareCaptureContext with valid binding, matching raw hash and run_id,
      source=psp, and no stream metadata value that contradicts the context
      -> HARDWARE_MEASURED; else UNVERIFIED_CAPTURE
    The stream's own metadata never selects a label: placeholder metadata
    without a context is UNVERIFIED_CAPTURE like any other capture.
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
        # Also require metadata run_id matches context run_id
        if meta.get("run_id", "").lower() != evidence_context.run_id.lower():
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

def parse_threading_output(text: str, *, require_metadata: bool = True) -> Any:
    """Parse and validate a threading oracle stream.

    Extends protocol.parse_output with campaign checks, strict per-case
    schemas, and evidence separation (no self-promotion).
    """
    parsed = parse_output(text, require_metadata=require_metadata)
    meta = dict(parsed.metadata)
    if require_metadata:
        cv = meta.get("campaign_version")
        if cv is None:
            raise ProtocolError("metadata missing campaign_version")
        if cv != CAMPAIGN_VERSION:
            raise ProtocolError(f"unsupported campaign_version {cv!r}, expected {CAMPAIGN_VERSION!r}")
        run_id = meta.get("run_id")
        if run_id is None or not _HEX_RE.fullmatch(run_id):
            raise ProtocolError("run_id must be hexadecimal 0x...")
        # run_id syntactic validity already checked, but also must be within 32-bit? allow 1..8 hex digits after 0x
        # Schema version check vs matrix schema
        matrix = load_matrix()
        # Enforce campaign_version matches matrix
        if matrix.get("campaign_version") != cv:
            raise ProtocolError(f"campaign_version {cv!r} does not match matrix {matrix.get('campaign_version')!r}")
        # Validate metadata fields not duplicated already done by protocol
        # Per-case validation
        required_map = _build_required_map(matrix)
        # Check for unknown case_ids? Allow but later analyzer will mark extra
        # But per strict schema, unknown case should be rejected? We reject if case not in matrix
        # Actually we want to reject unexpected case that is not defined – but analyzer's extra_cases
        # would handle? The spec says parser must reject missing required field/case etc.
        # Unknown case is an unexpected case -> we will let analyzer flag, but parser should
        # reject if record's case_id not in matrix? For strict fail-closed, we reject unknown case_ids
        # unless they are explicitly allowed as extension. We'll reject unknown case_ids as unexpected.
        for r in parsed.results:
            vals = dict(r.values)
            # Global required fields for every record
            # result must be present (raw API return code) – not optional
            if "result" not in vals:
                raise ProtocolError(f"test {r.case_id!r} missing required field 'result' (raw return code)")
            if not _HEX_RE.fullmatch(vals["result"]):
                raise ProtocolError(f"test {r.case_id!r} field result must be hexadecimal")
            canary = vals.get("canary")
            if canary is None:
                raise ProtocolError(f"test {r.case_id!r} missing canary")
            if not CANARY_RE.fullmatch(canary) or canary.lower() != CANARY_EXPECTED:
                raise ProtocolError(f"test {r.case_id!r} has invalid canary {canary!r} expected {CANARY_EXPECTED}")
            if r.test_id != EXPECTED_TEST_ID:
                raise ProtocolError(f"unexpected test_id {r.test_id!r} expected {EXPECTED_TEST_ID!r}")
            # attempt is required per observation (Stage 6)
            attempt = vals.get("attempt")
            if attempt is None:
                raise ProtocolError(f"test {r.case_id!r} missing required field 'attempt' (run/attempt identity)")
            if not ATTEMPT_RE.fullmatch(attempt):
                raise ProtocolError(f"test {r.case_id!r} field attempt must be hex 0x...")
            # Validate attempt sane (0..255 etc)
            try:
                att_val = int(attempt, 16)
                if att_val > 255:
                    raise ProtocolError(f"test {r.case_id!r} attempt out of sane range {attempt!r}")
            except ValueError as exc:
                raise ProtocolError(f"test {r.case_id!r} field attempt must be hex") from exc
            # Validate per-case required_out_fields
            if r.case_id not in required_map:
                raise ProtocolError(f"test {r.case_id!r} is not defined in matrix.json (unexpected case)")
            req = required_map[r.case_id]["required"]
            opt = required_map[r.case_id]["optional"]
            # Check missing required out fields
            missing = sorted(req - set(vals.keys()))
            if missing:
                raise ProtocolError(f"test {r.case_id!r} missing required fields: {', '.join(missing)}")
            # Check for unexpected fields: allowed = required | optional | {result,canary,attempt} plus probe length fields for oversize tests
            allowed_probe = {"probe_len", "stack_bytes", "window_bytes", "arg_bytes"}
            allowed = set(req) | set(opt) | {"result", "canary", "attempt"} | allowed_probe
            # Also allow future_* optional namespace under the exact prefix
            for k, v in vals.items():
                if k in allowed:
                    # probe_len etc will be checked for oversize later, but still allowed
                    if OPTIONAL_FIELD_RE.fullmatch(k):
                        # future_* already handled but keep hex check
                        if not _HEX_RE.fullmatch(v):
                            raise ProtocolError(f"test {r.case_id!r} optional field {k} must be hex")
                    continue
                # Check near-miss extension prefix: bare future, future_, future__invalid (unless intentionally valid)
                if k == "future" or k == "future_" or k.startswith("future__"):
                    raise ProtocolError(f"test {r.case_id!r} extension prefix {k!r} rejected; valid extensions are under future_<name> namespace")
                if OPTIONAL_FIELD_RE.fullmatch(k):
                    if not _HEX_RE.fullmatch(v):
                        raise ProtocolError(f"test {r.case_id!r} optional field {k} must be hex")
                    continue
                # Any other field is unexpected – fail closed even if hex
                raise ProtocolError(f"test {r.case_id!r} has unexpected field {k!r} value {v!r} (possible misspelling, unknown hex field rejected)")
            # Check for oversized fields: any probe length > STACK_PROBE_MAX_BYTES
            for k, v in vals.items():
                if k in {"probe_len", "stack_bytes", "window_bytes", "arg_bytes"}:
                    try:
                        n = int(v, 16)
                    except ValueError as exc:
                        raise ProtocolError(f"test {r.case_id!r} field {k} must be hex") from exc
                    if n > STACK_PROBE_MAX_BYTES:
                        raise ProtocolError(f"test {r.case_id!r} field {k} oversized {n} > {STACK_PROBE_MAX_BYTES}")
                # For cases that involve stack probe, bound any out that might encode length
                # ST-SA02 expects 16, ST-SA03 expects 64 – but generic out* bounds:
                # If case is ST-SA02/ST-SA03 and any out encodes arg size, bound it
                # We treat out* values that are >64 and not sentinel as oversized for those cases
            # Stack probe bound for ST-SA* / CT stack cases via explicit checks on required outs
            # Additional oversized check: if case_id in stack probe cases and any out hex >64 where it should be length
            # Use matrix's measurement_fields to detect stack probe? Simpler: for ST-SA02, require out* hex values that represent lengths are bounded.
            # We will check that for ST-SA02, out fields that are checksums etc are not length; skip.
            # For CT-D00, ensure any field that claims stack size not oversized – but we already cover named fields.
            # Also enforce that no out field is oversized beyond 64 if it is declared as length in future
            # Generic oversize for known probe cases: if case_id.startswith(("CT-A12","CT-D00","ST-STACK")) and any k.startswith("out") with value > 0x40 where it should be canary? Not.
            # Keep minimal: named probe_len check suffices; extra generic oversize test is handled via strict unknown rejection.
            # Validate hex values for all out/result/canary/attempt are hex already
            for k, v in vals.items():
                if k.startswith("out") or k in {"result","canary","attempt"} or OPTIONAL_FIELD_RE.fullmatch(k):
                    if not _HEX_RE.fullmatch(v):
                        raise ProtocolError(f"test {r.case_id!r} field {k} must be hexadecimal")
                elif k in {"error","detail"}:
                    # those may be non-hex detail? but we already rejected unknown; detail allowed non-hex?
                    pass
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


def analyze_runs(texts: list[str], evidence_contexts: list[Any] | None = None,
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

    evidence_contexts, if provided, must align 1:1 with texts and are used
    to label each run's evidence; default is UNVERIFIED_CAPTURE per run.
    """
    matrix = load_matrix()
    required = expected_case_ids(matrix, scope)
    gating = expected_case_ids(matrix, GATING_SCOPE)
    matrix_cases = expected_case_ids(matrix, FULL_SCOPE)
    required_map = _build_required_map(matrix)
    runs: list[Any] = []
    run_ids: list[str] = []
    for idx, text in enumerate(texts):
        parsed = parse_threading_output(text)
        meta = dict(parsed.metadata)
        run_id = meta.get("run_id")
        if run_id is None or not _HEX_RE.fullmatch(run_id):
            raise ProtocolError(f"run {idx} has invalid run_id {run_id!r}")
        runs.append(parsed)
        run_ids.append(run_id.lower())
    # Each launch must be distinct: a duplicate run_id is impossible.
    if len(set(run_ids)) != len(run_ids):
        raise ProtocolError(f"duplicate run_id detected: {run_ids}")
    # Observations keyed by (run_id, attempt, case_id).
    from collections import defaultdict
    per_case: dict[str, list[dict[str, Any]]] = defaultdict(list)
    seen_triples: set[tuple[str, str, str]] = set()
    for run_idx, parsed in enumerate(runs):
        run_id = run_ids[run_idx]
        for r in parsed.results:
            vals = dict(r.values)
            attempt = vals.get("attempt", "0x00000000").lower()
            triple = (run_id, attempt, r.case_id)
            if triple in seen_triples:
                raise ProtocolError(f"duplicate run/case/attempt triple {triple!r}")
            seen_triples.add(triple)
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
            case_reports.append({"case_id": case_id, "observed": False, "stable": False, "runs": [], "variable": False, "run_count": 0, "expected_launch": expected_launch, "required": True})
            continue
        # Divergent observations are preserved; status is part of the comparison.
        stable = len({_stability_key(o["result"]) for o in obs}) == 1
        case_reports.append({
            "case_id": case_id,
            "observed": True,
            "stable": stable,
            "variable": not stable,
            "observations": obs,
            "run_count": len(obs),
            "expected_launch": expected_launch,
            "required": case_id in required,
        })
    missing = sorted(required - observed)
    extra = sorted(observed - matrix_cases)
    launch_group_issues = _launch_group_issues(runs, run_ids, required_map, gating)
    counts = {cid: len(per_case.get(cid, [])) for cid in reported}
    repetition_issues = _repetition_issues(counts)
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
        "extra_cases": extra,
        "optional_cases_observed": optional_observed,
        "all_required_observed": not missing,
        "launch_group_issues": launch_group_issues,
        "repetition_issues": repetition_issues,
        "repetitions": min(counts[cid] for cid in required),
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
    matrix = load_matrix()
    by_id = {c["case_id"]: c for c in matrix["cases"]}
    for rep in analysis["case_reports"]:
        cid = rep["case_id"]
        info = by_id.get(cid, {})
        inputs = str(info.get("inputs", ""))[:20]
        status = "MISSING" if not rep["observed"] else ("STABLE" if rep["stable"] else "VARIABLE")
        statuses = {o["result"]["status"] for o in rep.get("observations", [])}
        result = "/".join(sorted(statuses))
        lines.append(f"{cid:12} {inputs:20} {result:12} {status:8} {_measured_column(statuses)}")
    if analysis["missing_cases"]:
        lines.append(f"Missing: {', '.join(analysis['missing_cases'])}")
    if analysis["extra_cases"]:
        lines.append(f"Extra: {', '.join(analysis['extra_cases'])}")
    for issue in analysis.get("launch_group_issues", []) + analysis.get("repetition_issues", []):
        lines.append(f"Invalid: {issue}")
    if "evidence_labels" in analysis:
        lines.append(f"Evidence: {', '.join(analysis['evidence_labels'])}")
    return "\n".join(lines) + "\n"
