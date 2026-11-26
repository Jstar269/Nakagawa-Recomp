#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Generate the authoritative HLE-registration manifest from src/rt/hle.c.

This replaces ad hoc regular-expression scraping (tools/nid_auditor.py) with a
fail-closed extraction: every textual occurrence of an HLE registration call in
hle.c must be accounted for as either (a) a function definition, (b) a
fully-parsed literal `sr_hle_register(0x…, "name", handler);`, (c) an explicit
`sr_hle_register_unsupported(0x…, "name", error);`, or (d) the one known dynamic form (the sas_ok[] loop, whose NID array is itself
parsed). Any occurrence that matches none of these is a hard error, so a new
registration style can never be silently dropped from the manifest. Extracted
handler names are cross-checked against handler definitions in the file, and
curated metadata (tools/hle_registry_meta.py) is validated against the
extraction in both directions.

The manifest is deterministic (sorted, ASCII, LF) and carries per-NID
classification so downstream consumers never re-derive policy:

    {"nid": "0x1b4217bc", "name": …, "handler": …, "origin": "static",
     "owner_file": "src/rt/hle.c", "subsystem": "kernel_memory",
     "classification": "fake_success", "status": "stub"}

`owner_file` is the runtime source file whose registration call registers the
NID (one of the runtime HLE sources; see runtime_hle_sources). `subsystem` is the
runtime subsystem that owns the NID for the HLE split (issue #307), from
tools/hle_registry_meta.py SUBSYSTEM_BY_PREFIX; see subsystem_for.

CLI:
    python tools/hle_manifest.py [--out build/hle_manifest.json]
                                 [--write-baseline tools/import_audit_baseline.json]
                                 [--imports <manifest or PSP module> [--check-profile-zero-imports]]
"""

from __future__ import annotations

import argparse
import bisect
from collections.abc import Collection, Mapping
from itertools import pairwise
import json
from pathlib import Path, PurePosixPath
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hle_registry_meta as meta  # noqa: E402
from makefile_text import read_makefile  # noqa: E402
import psp_import_table  # noqa: E402
import publication_policy  # noqa: E402
import hle_sources  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BASELINE = ROOT / "tools" / "import_audit_baseline.json"

MANIFEST_SCHEMA = 1
# What build_manifest() reads: every runtime HLE source, listed in tools/hle_sources.py.
MANIFEST_SOURCE = "runtime HLE sources (tools/hle_sources.py)"

_REGISTRATION_CALL_RE = re.compile(r"\bsr_hle_register(?:_unsupported|_entry)?\s*\(")
_LITERAL_RE = re.compile(
    r"sr_hle_register\(\s*0x([0-9a-fA-F]{1,8})[uU]?\s*,\s*\"([^\"\\]+)\"\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*;"
)
_UNSUPPORTED_LITERAL_RE = re.compile(
    r"sr_hle_register_unsupported\(\s*0x([0-9a-fA-F]{1,8})[uU]?\s*,\s*\"([^\"\\]+)\"\s*,\s*0x([0-9a-fA-F]{1,8})[uU]?\s*\)\s*;"
)
_DEFINITION_RE = re.compile(r"\bvoid\s+(sr_hle_register(?:_unsupported|_entry)?)\(")
_WRAPPER_ENTRY_CALL_RE = re.compile(
    r"\bsr_hle_register_entry\(\s*nid\s*,\s*name\s*,\s*(?:fn\s*,\s*0u|h_ControlledUnsupported\s*,\s*error)\s*\)\s*;"
)
#: The registration wrappers whose bodies forward to sr_hle_register_entry (hle.c).
_WRAPPER_FUNCTIONS = frozenset({"sr_hle_register", "sr_hle_register_unsupported"})
_SAS_LOOP_RE = re.compile(r"sr_hle_register\(\s*sas_ok\[i\]\s*,\s*\"([^\"]+)\"\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*;")
_SAS_ARRAY_RE = re.compile(r"\bsas_ok\[\]\s*=\s*\{([^}]*)\}\s*;", re.DOTALL)
_HANDLER_DEF_RE = re.compile(r"\b(?:static\s+)?uint32_t\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*CpuState\s*\*")
# The one handler-defining macro in hle.c: UTILITY_DIALOG_HANDLERS(h_X, slot)
# expands h_X{Init,Update,Status,Shutdown}.
_DIALOG_MACRO_RE = re.compile(r"^UTILITY_DIALOG_HANDLERS\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,", re.MULTILINE)


class ManifestError(Exception):
    """hle.c contains a registration the extractor cannot prove it captured."""


def _strip_comments(source: str) -> str:
    """Blank out /*...*/ and //... comments, preserving offsets and newlines.

    String and character literals are copied verbatim (escape-aware), so a
    comment opener inside a literal does not start a comment and registration
    name strings survive untouched. Comment bytes become spaces, keeping every
    line number and character offset identical to the input.
    """
    out = list(source)
    i, n = 0, len(source)
    while i < n:
        c = source[i]
        if c in ('"', "'"):
            quote = c
            i += 1
            while i < n and source[i] != quote:
                i += 2 if source[i] == "\\" else 1
            i += 1
        elif c == "/" and i + 1 < n and source[i + 1] == "*":
            end = source.find("*/", i + 2)
            end = n if end < 0 else end + 2
            for j in range(i, end):
                if source[j] != "\n":
                    out[j] = " "
            i = end
        elif c == "/" and i + 1 < n and source[i + 1] == "/":
            end = source.find("\n", i)
            end = n if end < 0 else end
            for j in range(i, end):
                out[j] = " "
            i = end
        else:
            i += 1
    return "".join(out)


_PP_IF_RE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b\s*(.*?)\s*$")


def _blank_inactive_blocks(source: str) -> str:
    """Blank lines inside `#if 0` blocks so disabled registrations stay dead.

    Line-based and deliberately conservative: only a literal `#if 0` opens an
    inactive region (its outermost `#else`/`#elif` branches are treated as
    active, since a possibly-compiled registration must never be omitted);
    every other conditional is treated as active, matching how hle.c is
    actually built. Nested
    conditionals inside an inactive region are tracked so an inner `#endif`
    cannot re-enable it early. Offsets and line numbers are preserved.
    """
    lines = source.split("\n")
    out = []
    # Stack entries: True if the current region is inactive.
    stack: list[bool] = []
    depth_inactive = 0  # nesting depth *inside* the outermost inactive block
    for line in lines:
        m = _PP_IF_RE.match(line)
        inactive = bool(stack and stack[-1])
        if m:
            kind, cond = m.group(1), m.group(2)
            if inactive:
                if kind in ("if", "ifdef", "ifndef"):
                    depth_inactive += 1
                elif kind == "endif":
                    if depth_inactive:
                        depth_inactive -= 1
                    else:
                        stack.pop()
                elif kind in ("else", "elif") and not depth_inactive:
                    # The #else of #if 0 is active; an outermost #elif is
                    # conservatively treated as active too (we cannot evaluate
                    # its condition, and omitting a compiled registration is
                    # the failure mode this scanner must never have).
                    stack[-1] = False
            else:
                if kind in ("if", "ifdef", "ifndef"):
                    stack.append(kind == "if" and cond == "0")
                elif kind == "endif" and stack:
                    stack.pop()
                # #else/#elif of a conditional we cannot evaluate stays
                # active: over-including is fail-closed here, because a
                # phantom entry immediately diverges from the runtime table
                # and trips the baseline/duplicate checks.
            out.append(" " * len(line))  # directives themselves never carry code
            continue
        out.append(" " * len(line) if inactive else line)
    return "\n".join(out)


def active_source(source: str) -> str:
    """Comment-free, `#if 0`-free view of hle.c with identical offsets."""
    return _blank_inactive_blocks(_strip_comments(source))


def classify(handler: str, mechanical_stubs: frozenset[str] | set[str] = frozenset()) -> tuple[str, str]:
    """Return (classification, status) for a handler name, from curated policy.

    `h_ok` keeps its existing generic-success classification unchanged; handlers
    whose bodies do nothing but ``(void)s`` / logging / ``return 0`` (see
    :func:`mechanical_stub_handlers`) are classified identically without
    hand-listing their names, so new stubs cannot slip through.
    """
    status: str = meta.HANDLER_STATUS.get(handler, "unreviewed")
    if handler in meta.GENERIC_SUCCESS_HANDLERS or status == "stub" or handler in mechanical_stubs:
        return "fake_success", "stub"
    if status == "controlled_unsupported":
        return "controlled_unsupported", status
    return "dedicated", status


# ---------------------------------------------------------------------------
# Mechanical zero-returning stub detection
# ---------------------------------------------------------------------------
# Curated GENERIC_SUCCESS_HANDLERS only names h_ok, so dedicated one-line
# `return 0` stubs and log-then-return-0 stubs must also be found mechanically.
# This pass recognises them by body shape instead of by name:
# a handler that returns 0 on its success path while performing no
# guest/host-observable work besides `(void)s` silencing and diagnostic
# logging. Anything that writes guest memory (MEM_W*), writes runtime state
# (s_*/g_*/s->*/p-> assignments), or calls an implementation/scheduler/sync
# entry point is real work and is never flagged, so a new stub cannot slip
# through while a real handler cannot be misclassified by adding a log line.
#
# Allowed (pure/diagnostic) calls: diagnostic logging itself (fprintf/fflush/
# fputs/fputc/puts/printf), log-gate queries (getenv, hle/ge/atrac/sas_log_on),
# read-only guest introspection used to format a log line or validate an
# argument (guest_cstr, sr_inrange, sr_guest_span_*, sr_size_*), the
# read-only thread identity used as a log prefix (sched_current_uid),
# read-only guest loads (MEM_R*), and pure string/size queries
# (strstr/strlen/strcmp/strncmp/_strnicmp/memcmp/sizeof). Every other call --
# including any h_* delegation, scheduler/sync/dialog/audio/mpeg/atrac/psmf/
# sas/ge implementation entry, and host allocation or capture helpers -- marks
# the handler as doing work.

_MECHANICAL_ALLOWED_CALLS = frozenset(
    {
        "fprintf",
        "fflush",
        "fputs",
        "fputc",
        "puts",
        "printf",
        "snprintf",
        "vsnprintf",
        "getenv",
        "hle_log_on",
        "ge_log_on",
        "atrac_log_on",
        "sas_log_on",
        "guest_cstr",
        "sr_inrange",
        "sr_guest_span_readable",
        "sr_guest_span_writable",
        "sr_size_add_ok",
        "sr_size_mul_ok",
        "sched_current_uid",
        "MEM_R32",
        "MEM_R8",
        "MEM_R16",
        "MEM_R64",
        "strstr",
        "strlen",
        "strcmp",
        "strncmp",
        "_strnicmp",
        "memcmp",
        "sizeof",
    }
)

_MECHANICAL_CONTROL_WORDS = frozenset(
    {"if", "for", "while", "switch", "return", "sizeof", "static", "extern"}
)

# A single `=` that is not part of `==`, `!=`, `<=`, `>=`, `+=`, `-=`, etc.
_MECHANICAL_SINGLE_EQ = r"(?<![=!<>+\-*/%&|^])=(?!=)"

_MECHANICAL_RETURN_ZERO_RE = re.compile(
    r"\breturn\s+(?:\(uint32_t\))?\s*0(?:[xX]0+)?[uU]?\s*;"
)
_MECHANICAL_CALL_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
_MECHANICAL_HANDLER_DEF_RE = re.compile(
    r"\b(?:static\s+)?uint32_t\s+(h_\w+)\s*\(\s*CpuState\s*\*\s*\w+\s*\)\s*\{"
)


def _strip_string_literals(text: str) -> str:
    """Blank string/char literal contents so log text cannot fake code.

    Quotes and newlines are preserved; every other byte inside a literal
    becomes a space. Escape-aware, so an escaped quote does not end the
    literal early.
    """
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            quote = c
            j = i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            for k in range(i, j):
                if text[k] != "\n":
                    out[k] = " " if text[k] not in ('"', "'") else text[k]
            i = j
        else:
            i += 1
    return "".join(out)


def handler_spans(active_src: str) -> list[tuple[str, int, int, int]]:
    """(name, definition offset, opening brace offset, closing brace offset) per handler.

    The one handler-definition grammar shared with tools/verification_ledger.py. Runs on
    the active view (comments and `#if 0` regions already blanked) and brace-matches with
    string and character literals skipped. An unterminated body ends at len(active_src).
    """
    spans: list[tuple[str, int, int, int]] = []
    for m in _MECHANICAL_HANDLER_DEF_RE.finditer(active_src):
        name = m.group(1)
        depth = 0
        i = m.end() - 1  # at the opening brace
        j = i
        n = len(active_src)
        in_str: str | None = None
        while j < n:
            c = active_src[j]
            if in_str is not None:
                if c == "\\":
                    j += 2
                    continue
                if c == in_str:
                    in_str = None
            elif c == '"' or c == "'":
                in_str = c
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        spans.append((name, m.start(), i, j))
    return spans


def _handler_bodies(active_src: str) -> dict[str, str]:
    """Map handler name -> inner body text, brace-matched, strings skipped.

    Runs on the active view (comments and `#if 0` regions already blanked),
    so disabled handlers never enter the census. Macro-defined handlers
    (UTILITY_DIALOG_HANDLERS) have no `uint32_t` definition and are absent;
    they implement real dialog state machines and correctly stay dedicated.
    """
    return {name: active_src[opening + 1 : closing] for name, _, opening, closing in handler_spans(active_src)}


def _has_observable_write(body_inner: str, nos: str | None = None) -> bool:
    """True when the body writes guest memory or runtime state.

    Reads (`s->pc` in a log line, `s_ge_lists[i].uid == uid`, `>=`
    comparisons) are not writes. Only a single `=` assignment (or compound
    `+=`/`|=`/`++`/`--`) whose target reaches guest memory (`MEM_W*`),
    runtime globals (`s_*`/`g_*`), CpuState (`s->`), or a pointed-to object
    (`p->field`) counts. Assignments to function-local temporaries
    (`uint32_t uid = ...`, `char dev[64]`, `const char *path = NULL`) never
    match because their left-hand side names no such target.
    """
    if nos is None:
        nos = _strip_string_literals(body_inner)
    if "MEM_W" in nos:
        return True
    pats = (
        rf"\b[sg]_\w+[^\n;]*?{_MECHANICAL_SINGLE_EQ}",
        rf"\bs\s*->[^\n;]*?{_MECHANICAL_SINGLE_EQ}",
        rf"->\s*\w+[^\n;]*?{_MECHANICAL_SINGLE_EQ}",
        r"\b[sg]_\w+[^\n;]*?(?:\+=|-=|\*=|/=|>>=|<<=|\|=|&=)",
        r"->\s*\w+[^\n;]*?(?:\+=|-=|\*=|/=|>>=|<<=|\|=|&=)",
        r"\bs\s*->[^\n;]*?(?:\+\+|--)",
    )
    for pat in pats:
        if re.search(pat, nos):
            return True
    if re.search(r"\b[sg]_\w+\s*(?:\+\+|--)", nos):
        return True
    return False


def is_trivial_success_stub(body_inner: str) -> bool:
    """True when a handler body does nothing but silence/log/return-0.

    `body_inner` is the text between the handler's outer braces (comments
    already blanked). The check is positive, not a denylist of names: the
    body must return literal 0 on its success path, perform no observable
    write (see :func:`_has_observable_write`), and call nothing beyond pure
    diagnostic helpers. Early error returns (`return 0x8001...`) for argument
    validation are allowed; they do not make a stub real.
    """
    if not _MECHANICAL_RETURN_ZERO_RE.search(body_inner):
        return False
    nos = _strip_string_literals(body_inner)
    if _has_observable_write(body_inner, nos):
        return False
    for call in _MECHANICAL_CALL_RE.findall(nos):
        if call in _MECHANICAL_ALLOWED_CALLS or call in _MECHANICAL_CONTROL_WORDS:
            continue
        # Any other call -- including any h_* delegation to another HLE
        # handler -- is real work.
        return False
    return True


def mechanical_stub_handlers(source: str) -> set[str]:
    """Handlers in `source` whose bodies are trivial zero-returning stubs.

    Takes the same combined `hle.c` + `hle_power.c` text :func:`build_manifest`
    consumes (or any synthetic C snippet for tests) and returns the stub
    handler names. `h_ok` is included when present; callers keep its curated
    generic-success classification unchanged and union this set with it.
    """
    bodies = _handler_bodies(active_source(source))
    return {name for name, inner in bodies.items() if is_trivial_success_stub(inner)}


def _owner_lookup(raw_source: str, sources: list[tuple[str, str]] | None):
    """Return a function mapping an offset in `raw_source` to its source unit's path.

    `sources` must be the (path, text) units that `raw_source` was joined from
    with newlines. Without them the text is unattributed and every offset maps
    to None.
    """
    if sources is None:
        return lambda offset: None
    if not sources or "\n".join(text for _, text in sources) != raw_source:
        raise ValueError("sources must be the (path, text) units that raw_source is joined from")
    starts: list[int] = []
    paths: list[str] = []
    pos = 0
    for path, text in sources:
        starts.append(pos)
        paths.append(path)
        pos += len(text) + 1  # the joining newline
    return lambda offset: paths[bisect.bisect_right(starts, offset) - 1]


def extract_registrations(raw_source: str, sources: list[tuple[str, str]] | None = None) -> list[dict]:
    """Parse hle.c text into registration dicts, failing closed on anything unaccounted.

    Runs on the active view of the source (comments and `#if 0` regions
    blanked, offsets preserved), so a commented-out or preprocessed-away
    registration can never become a live manifest entry and hex constants
    inside comments can never leak into the sas_ok[] NID set.

    `sources` is the (path, text) list `raw_source` was joined from (see
    runtime_hle_sources). Each registration then carries `owner_file`, the path
    of the unit its call sits in. Without `sources` owner_file is None.
    """
    source = active_source(raw_source)
    owner_at = _owner_lookup(raw_source, sources)
    accounted: set[int] = set()

    regs: list[dict] = []
    for m in _LITERAL_RE.finditer(source):
        accounted.add(m.start())
        nid = int(m.group(1), 16)
        regs.append(
            {"nid": nid, "name": m.group(2), "handler": m.group(3), "origin": "static",
             "owner_file": owner_at(m.start())}
        )

    for m in _UNSUPPORTED_LITERAL_RE.finditer(source):
        accounted.add(m.start())
        regs.append(
            {
                "nid": int(m.group(1), 16),
                "name": m.group(2),
                "handler": "h_ControlledUnsupported",
                "origin": "static_unsupported",
                "refusal_error": int(m.group(3), 16),
                "owner_file": owner_at(m.start()),
            }
        )

    for m in _DEFINITION_RE.finditer(source):
        accounted.add(m.start(1))
    # The one forwarding call in each wrapper body. The same text in any other
    # function is a registration the census cannot attribute, so it stays unaccounted.
    wrapper_matches = list(_WRAPPER_ENTRY_CALL_RE.finditer(source))
    if wrapper_matches:
        line_starts = _line_starts(source)
        owners = enclosing_functions(source, line_starts)
        for m in wrapper_matches:
            if owners[bisect.bisect_right(line_starts, m.start()) - 1] in _WRAPPER_FUNCTIONS:
                accounted.add(m.start())

    sas_loops = list(_SAS_LOOP_RE.finditer(source))
    if sas_loops:
        arrays = list(_SAS_ARRAY_RE.finditer(source))
        if len(sas_loops) != 1 or len(arrays) != 1:
            raise ManifestError(
                f"expected exactly one sas_ok[] array and loop; found {len(arrays)} arrays, {len(sas_loops)} loops"
            )
        accounted.add(sas_loops[0].start())
        sas_name, sas_handler = sas_loops[0].group(1), sas_loops[0].group(2)
        sas_owner = owner_at(sas_loops[0].start())
        sas_nids = [int(h, 16) for h in re.findall(r"0x[0-9a-fA-F]{1,8}", arrays[0].group(1))]
        if not sas_nids:
            raise ManifestError("sas_ok[] array parsed empty")
        for nid in sas_nids:
            regs.append(
                {"nid": nid, "name": sas_name, "handler": sas_handler, "origin": "sas_ok_loop",
                 "owner_file": sas_owner}
            )

    unaccounted = []
    for call in _REGISTRATION_CALL_RE.finditer(source):
        idx = call.start()
        # Occurrences inside a C string literal are diagnostics (the
        # unimplemented-NID hint template), not registrations: an odd number
        # of unescaped double quotes precedes the token on its line.
        line_start = source.rfind("\n", 0, idx) + 1
        in_string = len(re.findall(r'(?<!\\)"', source[line_start:idx])) % 2 == 1
        if idx not in accounted and not in_string:
            line = source.count("\n", 0, idx) + 1
            unaccounted.append(f"line {line}: {source[idx: source.find(chr(10), idx)].strip()!r}")
    if unaccounted:
        raise ManifestError(
            "HLE registration occurrences the extractor cannot account for "
            "(teach tools/hle_manifest.py the new form before merging):\n  " + "\n  ".join(unaccounted)
        )

    handlers_defined = set(_HANDLER_DEF_RE.findall(source))
    for m in _DIALOG_MACRO_RE.finditer(source):
        for suffix in ("Init", "Update", "Status", "Shutdown"):
            handlers_defined.add(m.group(1) + suffix)
    missing = sorted({r["handler"] for r in regs} - handlers_defined)
    if missing:
        raise ManifestError(f"registered handlers with no `uint32_t <h>(CpuState *)` definition: {missing}")

    dupes: dict[int, list[str]] = {}
    seen: dict[int, dict] = {}
    for r in regs:
        if r["nid"] in seen:
            dupes.setdefault(r["nid"], [seen[r["nid"]]["name"]]).append(r["name"])
        else:
            seen[r["nid"]] = r
    if dupes:
        detail = ", ".join(f"0x{nid:08x} ({' / '.join(names)})" for nid, names in sorted(dupes.items()))
        raise ManifestError(f"duplicate NID registrations (runtime would reject the later one): {detail}")

    return sorted(regs, key=lambda r: r["nid"])


_PSP_EVIDENCE_ID = re.compile(r"PSP-[A-Z0-9]+(?:-[A-Z0-9]+)*-\d{3}")


def validate_status_evidence_ratchet() -> None:
    """A partial or compatibility status must cite evidence, and cited PSP ids must exist (#341).

    Statuses that predate the rule sit on the shrink-only
    `PARTIAL_WITHOUT_EVIDENCE` list; everything else must carry at least one entry in
    `HANDLER_EVIDENCE`. A cited `PSP-...-NNN` oracle id has to appear in the hardware
    oracle documentation or manifest, so a typo cannot pass as a measurement.
    """
    exempt = meta.PARTIAL_WITHOUT_EVIDENCE
    for handler, status in meta.HANDLER_STATUS.items():
        if status not in ("partial", "compatibility"):
            continue
        has_evidence = bool(meta.HANDLER_EVIDENCE.get(handler))
        if not has_evidence and handler not in exempt:
            raise ManifestError(
                f"handler {handler!r} is marked {status!r} but cites no evidence; a status "
                "claim must name a PSP oracle cell, a dispatch-entering self-test or a public "
                "signature it was read against (#341)"
            )
        if has_evidence and handler in exempt:
            raise ManifestError(
                f"PARTIAL_WITHOUT_EVIDENCE lists {handler!r}, which now cites evidence; "
                "remove it from the list (#341)"
            )
    for handler in sorted(exempt):
        if meta.HANDLER_STATUS.get(handler) not in ("partial", "compatibility"):
            raise ManifestError(
                f"PARTIAL_WITHOUT_EVIDENCE lists {handler!r}, which is no longer a partial or "
                "compatibility handler (stale); remove it (#341)"
            )
    known = _read_optional(ROOT / "docs" / "HARDWARE_ORACLE.md") + _read_optional(
        ROOT / "tools" / "psp_oracle" / "manifest.json"
    )
    for handler, evidence in meta.HANDLER_EVIDENCE.items():
        for entry in evidence:
            for oracle_id in _PSP_EVIDENCE_ID.findall(entry):
                if oracle_id not in known:
                    raise ManifestError(
                        f"handler {handler!r} cites oracle id {oracle_id!r}, which neither "
                        "docs/HARDWARE_ORACLE.md nor tools/psp_oracle/manifest.json records"
                    )



def validate_meta(regs: list[dict]) -> None:
    """Both-direction cross-check between curated metadata and the extraction."""
    handlers: set[str] = {r["handler"] for r in regs}
    nids: set[int] = {r["nid"] for r in regs}
    stale = sorted(set(meta.HANDLER_STATUS) - handlers)
    if stale:
        raise ManifestError(f"HANDLER_STATUS names handlers hle.c no longer registers: {stale}")
    bad_status = sorted(v for v in meta.HANDLER_STATUS.values() if v not in meta.HANDLER_STATUSES)
    if bad_status:
        raise ManifestError(f"HANDLER_STATUS uses unknown status values: {bad_status}")
    for rule in meta.ALIAS_RULES:
        if rule["required_handler"] not in handlers:
            raise ManifestError(f"alias rule requires unknown handler {rule['required_handler']!r}")
    for nid in meta.KNOWN_NID_NAMES:
        if nid not in nids:
            raise ManifestError(f"KNOWN_NID_NAMES lists 0x{nid:08x}, which hle.c does not register")
    for w in meta.WAIVERS:
        if w["nid"] not in nids:
            raise ManifestError(f"waiver for 0x{w['nid']:08x} names an unregistered NID (stale)")
        if not w.get("issue"):
            raise ManifestError(f"waiver for 0x{w['nid']:08x} has no issue link")
    for nid, info in meta.FLOAT_RETURN_NIDS.items():
        if nid not in nids:
            raise ManifestError(f"FLOAT_RETURN_NIDS lists 0x{nid:08x}, which hle.c does not register")
        if not info.get("issue"):
            raise ManifestError(f"FLOAT_RETURN_NIDS entry 0x{nid:08x} has no issue link")
    by_nid = {r["nid"]: r for r in regs}
    for nid, entry in meta.PROFILE_ZERO_UNREVIEWED_IMPORTS.items():
        where = f"PROFILE_ZERO_UNREVIEWED_IMPORTS entry 0x{nid:08x}"
        if entry.get("issue") != meta.PROFILE_ZERO_ISSUE:
            raise ManifestError(f"{where} does not name its tracking issue {meta.PROFILE_ZERO_ISSUE}")
        reg = by_nid.get(nid)
        if reg is None:
            raise ManifestError(f"{where} names a NID hle.c does not register (stale)")
        if reg["name"] != entry.get("name"):
            raise ManifestError(f"{where} names {entry.get('name')!r}, but hle.c registers {reg['name']!r}")
        status = meta.HANDLER_STATUS.get(reg["handler"], "unreviewed")
        if reg["handler"] in meta.GENERIC_SUCCESS_HANDLERS or status != "unreviewed":
            raise ManifestError(
                f"{where} ({reg['name']}) is stale: {reg['handler']} is no longer a dedicated "
                f"unreviewed handler (status {status!r}); remove the entry (#341)"
            )
    stale_float_handlers = sorted(meta.FLOAT_RETURN_HANDLERS - handlers)
    if stale_float_handlers:
        raise ManifestError(
            f"FLOAT_RETURN_HANDLERS names handlers hle.c no longer registers: {stale_float_handlers}"
        )
    hle_defs = hle_sources.definitions()
    for handler, status in meta.HANDLER_STATUS.items():
        evidence = meta.HANDLER_EVIDENCE.get(handler)
        if status == "complete":
            if not evidence or not isinstance(evidence, list):
                raise ManifestError(
                    f"handler {handler!r} is marked 'complete' but has no evidence metadata; "
                    "a complete handler must cite at least one verifiable evidence entry "
                    "(public ABI/doc, source-owned test name, hardware oracle id, or guest-module takeover)"
                )
        has_executable_evidence = False
        if evidence is not None:
            if not isinstance(evidence, list) or not evidence:
                raise ManifestError(f"handler {handler!r} has empty evidence metadata")
            for ev in evidence:
                if not isinstance(ev, str) or not ev.strip():
                    raise ManifestError(f"handler {handler!r} has empty evidence entry")
                candidate_path, _, anchor = (part.strip() for part in ev.partition(":"))
                if not (
                    candidate_path == "Makefile"
                    or candidate_path.startswith(("src/", "tools/", "tests/"))
                ):
                    continue
                has_executable_evidence = True
                if not (ROOT / candidate_path).exists():
                    raise ManifestError(
                        f"handler {handler!r} cites non-existent evidence file {candidate_path!r}"
                    )
                if anchor and not _evidence_anchor_present(ROOT / candidate_path, anchor, hle_defs):
                    raise ManifestError(
                        f"handler {handler!r} cites evidence anchor {anchor!r} that "
                        f"{candidate_path!r} does not define"
                    )
        if status == "complete" and not has_executable_evidence:
            raise ManifestError(
                f"handler {handler!r} is marked 'complete' but has no named executable evidence; "
                "a complete handler must cite at least one executable evidence file in the repository "
                "(under src/, tools/, tests/, or Makefile)"
            )
        # Independent of evidence: a partial or compatibility handler must always
        # name its limitation, whether or not it also cites evidence.
        if status in ("partial", "compatibility"):
            limitation = meta.HANDLER_LIMITATIONS.get(handler)
            if not limitation or not isinstance(limitation, str) or not limitation.strip():
                raise ManifestError(
                    f"handler {handler!r} is marked {status!r} but has no named limitation; "
                    "every partial and compatibility entry must carry a non-empty limitation "
                    "(state a factual limitation from code or 'limitation not yet reviewed (#341)')"
                )
    validate_status_evidence_ratchet()
    validate_subsystems(regs)


_EVIDENCE_FILE_CACHE: dict[Path, str] = {}


def _evidence_anchor_present(path: Path, anchor: str, defs: dict[str, list[str]] | None = None) -> bool:
    """Whether ``path`` defines the symbol an evidence entry cites after ``:``.

    A Makefile anchor must be a target rule. An anchor in a runtime HLE source must
    be a function or object that file defines: the sources split one subsystem per
    file (#307), so a name that only appears there (a call, a comment) would let a
    citation outlive the move of the code it cites. Any other anchor is a dotted
    name (``Class.method`` or ``function``) whose every component must appear as a
    whole word, so a renamed target, test or function fails the citation.
    """
    resolved = path.resolve()
    relative = resolved.relative_to(ROOT).as_posix() if resolved.is_relative_to(ROOT) else ""
    if relative in hle_sources.RUNTIME_HLE_SOURCES:
        if defs is None:
            defs = hle_sources.definitions()
        return relative in defs.get(anchor, [])
    text = _EVIDENCE_FILE_CACHE.get(resolved)
    if text is None:
        if path.name == "Makefile":
            text = read_makefile(path.parent, errors="replace")
        else:
            text = path.read_text(encoding="utf-8", errors="replace")
        _EVIDENCE_FILE_CACHE[resolved] = text
    if path.name == "Makefile":
        return re.search(rf"^{re.escape(anchor)}\s*:", text, re.MULTILINE) is not None
    return all(
        part in text and re.search(rf"\b{re.escape(part)}\b", text) is not None
        for part in anchor.split(".")
        if part
    )


def compute_findings(regs: list[dict]) -> list[dict]:
    """Alias-consistency and mislabel findings, before waivers are applied."""
    findings: list[dict] = []
    for r in regs:
        for rule in meta.ALIAS_RULES:
            if r["name"].startswith(rule["name_prefix"]) and r["handler"] != rule["required_handler"]:
                findings.append(
                    {
                        "finding": "alias_mismatch",
                        "nid": r["nid"],
                        "name": r["name"],
                        "handler": r["handler"],
                        "expected_handler": rule["required_handler"],
                        "why": rule["why"],
                    }
                )
        canonical = meta.KNOWN_NID_NAMES.get(r["nid"])
        if canonical is not None and r["name"] != canonical:
            findings.append(
                {
                    "finding": "mislabeled_nid",
                    "nid": r["nid"],
                    "name": r["name"],
                    "handler": r["handler"],
                    "canonical_name": canonical,
                    "why": "registered label disagrees with the canonical NID name",
                }
            )
        float_info = meta.FLOAT_RETURN_NIDS.get(r["nid"])
        if float_info is not None and r["handler"] not in meta.FLOAT_RETURN_HANDLERS:
            findings.append(
                {
                    "finding": "float_return_handler_mismatch",
                    "nid": r["nid"],
                    "name": r["name"],
                    "handler": r["handler"],
                    "why": (
                        f"{float_info['name']} returns single-precision in $f0; "
                        "an integer-return handler leaves $f0 stale for the guest"
                    ),
                }
            )
    return sorted(findings, key=lambda f: (f["nid"], f["finding"]))


def unwaived_and_stale(findings: list[dict]) -> tuple[list[dict], list[dict]]:
    """Split findings/waivers into (unwaived findings, stale waivers)."""
    fkeys = {(f["nid"], f["finding"], f["handler"]) for f in findings}
    wkeys = {(w["nid"], w["finding"], w["handler"]) for w in meta.WAIVERS}
    unwaived = [f for f in findings if (f["nid"], f["finding"], f["handler"]) not in wkeys]
    stale = [dict(w) for w in meta.WAIVERS if (w["nid"], w["finding"], w["handler"]) not in fkeys]
    return unwaived, stale


def subsystem_for(name: str) -> str | None:
    """The runtime subsystem that owns a registered name, or None when no prefix claims it.

    The longest `meta.SUBSYSTEM_BY_PREFIX` key that prefixes the name (leading
    underscores stripped) decides. Distinct keys of equal length cannot both
    prefix one name, so the answer is unique whenever it exists.
    """
    prefix = subsystem_prefix(name)
    return None if prefix is None else meta.SUBSYSTEM_BY_PREFIX[prefix]


def subsystem_prefix(name: str) -> str | None:
    """The `meta.SUBSYSTEM_BY_PREFIX` key that decides `name`'s subsystem (see subsystem_for)."""
    bare = name.lstrip("_")
    matches = [p for p in meta.SUBSYSTEM_BY_PREFIX if bare.startswith(p)]
    return max(matches, key=len) if matches else None


def validate_subsystems(regs: list[dict]) -> None:
    """Fail closed unless every registration has one subsystem and every prefix still decides one.

    An unclaimed name is a NID with no owner for the runtime split (#307); a
    prefix that decides no registration is stale; a value outside
    `meta.SUBSYSTEMS` is a typo that would mint a new subsystem.
    """
    unknown = sorted({v for v in meta.SUBSYSTEM_BY_PREFIX.values() if v not in meta.SUBSYSTEMS})
    if unknown:
        raise ManifestError(f"SUBSYSTEM_BY_PREFIX uses subsystems missing from SUBSYSTEMS: {unknown}")
    unowned = sorted({r["name"] for r in regs if subsystem_for(r["name"]) is None})
    if unowned:
        raise ManifestError(
            f"registrations no SUBSYSTEM_BY_PREFIX key claims (add an owner in tools/hle_registry_meta.py): {unowned}"
        )
    deciding = {subsystem_prefix(r["name"]) for r in regs}
    stale = sorted(set(meta.SUBSYSTEM_BY_PREFIX) - deciding)
    if stale:
        raise ManifestError(f"SUBSYSTEM_BY_PREFIX keys that decide no registration (stale): {stale}")


def runtime_hle_sources() -> list[tuple[str, str]]:
    """The runtime HLE units the manifest is extracted from, as (path, text) in join order.

    Every runtime HLE source (tools/hle_sources.py). A handler may be defined in one
    unit and registered from another, so the handler-definition cross-check in
    extract_registrations needs them all. Every registration is attributed to the
    unit its call sits in (owner_file).
    """
    sources: list[tuple[str, str]] = hle_sources.sources()
    return sources


def runtime_hle_source() -> str:
    """The runtime HLE source the manifest is extracted from: the joined runtime_hle_sources()."""
    return "\n".join(text for _, text in runtime_hle_sources())


def tracked_src_c_files() -> list[Path]:
    """Return tracked `src/**/*.c` paths so local untracked files do not pollute checks."""
    import subprocess
    res = subprocess.run(
        ["git", "ls-files", "-z", "src"],
        cwd=ROOT,
        capture_output=True,
        check=True,
    )
    return [
        ROOT / rel
        for rel in res.stdout.decode("utf-8").split("\0")
        if rel.endswith(".c") and (ROOT / rel).exists()
    ]


def registration_calls_outside(files: dict[str, str], composed: Collection[str]) -> list[str]:
    """`path:line` of each registration call in a file the manifest does not read.

    `files` maps repo-relative paths to their text and `composed` names the
    runtime sources extract_registrations attributes. A registration call in any
    other file is a NID the manifest cannot see, so it is reported, not skipped.
    """
    stray: list[str] = []
    for path in sorted(files):
        if path in composed:
            continue
        active = active_source(files[path])
        for m in _REGISTRATION_CALL_RE.finditer(active):
            line = active.count("\n", 0, m.start()) + 1
            stray.append(f"{path}:{line}")
    return stray


def registered_nids(source: str | None = None) -> frozenset[int]:
    """Every function NID the runtime registers, from the fail-closed extraction.

    Both registration forms count: a dedicated or generic handler
    (sr_hle_register, including the sas_ok[] loop) and a controlled refusal
    (sr_hle_register_unsupported). Either way the runtime answers the NID
    itself. Errors propagate unchanged: OSError when the runtime source cannot
    be read, ManifestError when a registration cannot be proven captured.
    """
    regs = extract_registrations(runtime_hle_source() if source is None else source)
    return frozenset(r["nid"] for r in regs)


def build_manifest(source: str | None = None) -> dict:
    sources = None
    if source is None:
        sources = runtime_hle_sources()
        source = "\n".join(text for _, text in sources)
    regs = extract_registrations(source, sources)
    validate_meta(regs)
    mechanical = mechanical_stub_handlers(source)
    entries = []
    for r in regs:
        classification, status = classify(r["handler"], mechanical)
        entries.append(
            {
                "nid": f"0x{r['nid']:08x}",
                "name": r["name"],
                "handler": r["handler"],
                "origin": r["origin"],
                "owner_file": r["owner_file"],
                "subsystem": subsystem_for(r["name"]),
                "classification": classification,
                "status": status,
                **({"refusal_error": f"0x{r['refusal_error']:08x}"}
                   if "refusal_error" in r else {}),
            }
        )
    findings = compute_findings(regs)
    return {
        "schema": MANIFEST_SCHEMA,
        "source": MANIFEST_SOURCE,
        "registrations": entries,
        "findings": [
            {**f, "nid": f"0x{f['nid']:08x}"} for f in findings
        ],
    }


def manifest_to_baseline(manifest: dict) -> dict:
    baseline = {}
    for r in manifest["registrations"]:
        entry = {
            "name": r["name"],
            "handler": r["handler"],
            "classification": r["classification"],
            "status": r["status"],
        }
        if "refusal_error" in r:
            entry["refusal_error"] = r["refusal_error"]
        baseline[r["nid"]] = entry
    return baseline


def dump_json(obj: dict, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="ascii", newline="\n") as f:
        json.dump(obj, f, indent=2, sort_keys=True)
        f.write("\n")




# ---------------------------------------------------------------------------
# Evidence chain
# ---------------------------------------------------------------------------
# The chain re-derives nothing another tool already owns.  It joins, per NID:
#
#   canonical name -> independently derived NID   tools/nid_name_proof.py
#   imported NID                                  --imports (private retail
#                                                 manifests stay opt-in)
#   registered handler + classification           this module's extraction
#   production dispatch reachability              this module's scope analysis
#   conformance cell / exercised                  src/rt/intr_conformance.h,
#                                                 tests/native/hle_thread_selftest.c
#   hardware exercise                             tools/psp_oracle/manifest.json
#
# Each consumed fact keeps the tier of the tool that produced it.  The chain
# invents no tier of its own, and a link it cannot establish is recorded as
# absent rather than assumed.

CHAIN_SCHEMA = 1

#: The closed vocabulary of `tools/psp_oracle/manifest.json`'s per-test
#: `hardware_evidence`.  `status` states whether the probe source exists;
#: only this field states what a hardware run established, and only `MEASURED`
#: — with a resolvable committed citation — may grant HARDWARE_MEASURED.
HARDWARE_EVIDENCE_VALUES = frozenset({"NOT_RUN", "CAPTURED", "MEASURED"})

INTR_CONFORMANCE_H = ROOT / "src" / "rt" / "intr_conformance.h"
SELFTEST_C = ROOT / "tests" / "native" / "hle_thread_selftest.c"
PSP_ORACLE_MANIFEST = ROOT / "tools" / "psp_oracle" / "manifest.json"
PUBLIC_SOURCE_PROFILE = ROOT / "assets" / "public_source_profile.json"

SELFTEST_MACRO = "SR_HLE_THREAD_SELFTEST"

EVIDENCE_TIERS = (
    "HARDWARE_MEASURED",
    "HOST_TESTED",
    "STATICALLY_SUPPORTED",
    "NOT_EVIDENCE",
)

#: The leading fields of one ``IcProbe`` row: `{ "api", 0xnid, "scenario", ...`
_IC_PROBE_RE = re.compile(
    r'\{\s*"(?P<api>[A-Za-z0-9_]+)"\s*,\s*0x(?P<nid>[0-9a-fA-F]{1,8})[uU]?\s*,\s*"(?P<scenario>[^"]*)"'
)
#: `#define NID_SCE_AUDIO_CH_RESERVE 0x5ec81c55u`
_SELFTEST_NID_DEFINE_RE = re.compile(
    r"^#define[ \t]+(?P<sym>NID_[A-Za-z0-9_]+)[ \t]+0x(?P<nid>[0-9a-fA-F]{1,8})[uU]?[ \t]*$",
    re.MULTILINE,
)
#: `sr_syscall(&cpu, 0x05572a5fu)` -- a NID dispatched as a bare literal.
_SELFTEST_LITERAL_DISPATCH_RE = re.compile(
    r"sr_syscall\s*\([^;)]*,\s*0x(?P<nid>[0-9a-fA-F]{1,8})[uU]?\s*\)"
)
_FUNC_HEAD_RE = re.compile(
    r"^(?:static[ \t]+)?(?:void|int|uint32_t)[ \t]+(?P<name>[A-Za-z_][A-Za-z0-9_]*)[ \t]*\([^;{]*\)[ \t]*\{",
    re.MULTILINE,
)
_HELPER_CALL_RE = re.compile(r"\b(?P<name>[A-Za-z_][A-Za-z0-9_]*)[ \t]*\([ \t]*\)[ \t]*;")


def _read_optional(path: Path) -> str:
    return path.read_text(encoding="utf-8") if path.exists() else ""


def _line_starts(source: str) -> list[int]:
    starts = [0]
    pos = source.find("\n")
    while pos >= 0:
        starts.append(pos + 1)
        pos = source.find("\n", pos + 1)
    return starts


def selftest_scope_map(source: str) -> list[str]:
    """Per-line ``SR_HLE_THREAD_SELFTEST`` scope: ``both``/``selftest``/``production``.

    Only a conditional that actually tests the selftest macro narrows the scope.
    Every other conditional leaves it alone, because a registration guarded by an
    unrelated ``#if`` is still compiled into the shipping runtime, and treating
    it as selftest-only would understate production reachability.
    """
    scopes: list[str] = []
    outer: list[str] = []      # scope in effect before each open conditional
    opening: list[str] = []    # scope of the opening #if/#ifdef/#ifndef branch
    tests_macro: list[bool] = []
    current = "both"
    for line in source.split("\n"):
        m = _PP_IF_RE.match(line)
        if m:
            kind, cond = m.group(1), m.group(2)
            if kind in ("if", "ifdef", "ifndef"):
                macro = SELFTEST_MACRO in cond
                tests_macro.append(macro)
                outer.append(current)
                if macro and current == "both":
                    current = "production" if kind == "ifndef" or cond.lstrip().startswith("!") else "selftest"
                opening.append(current)
            elif kind in ("else", "elif") and tests_macro:
                if tests_macro[-1] and outer[-1] == "both":
                    current = "production" if opening[-1] == "selftest" else "selftest"
            elif kind == "endif" and tests_macro:
                tests_macro.pop()
                opening.pop()
                current = outer.pop()
        scopes.append(current)
    return scopes


def enclosing_functions(source: str, line_starts: list[int] | None = None) -> list[str | None]:
    """Per-line name of the enclosing top-level function, or ``None``."""
    lines = source.split("\n")
    if line_starts is None:
        line_starts = _line_starts(source)
    owner: list[str | None] = [None] * len(lines)
    for m in _FUNC_HEAD_RE.finditer(source):
        first = bisect.bisect_right(line_starts, m.start()) - 1
        depth = 0
        for i in range(first, len(lines)):
            owner[i] = m.group("name")
            depth += lines[i].count("{") - lines[i].count("}")
            if depth <= 0:
                break
    return owner


def registration_scopes(raw_source: str) -> dict[int, dict]:
    """Where each registered NID is reachable from: production, selftest, or both.

    A registration inside a helper inherits the scopes of that helper's call
    sites, which is what turns a shared ``hle_register_*_handlers()`` helper into
    evidence that ``sr_hle_init()``'s production branch really reaches it,
    instead of merely evidence that the text exists in the file. Helpers call
    helpers (a subsystem's registration run called from a shared helper, #307),
    so a call site counts only as far as its own function is reachable: the
    scopes are resolved through the whole chain, not one level.
    """
    source = active_source(raw_source)
    # active_source() blanks preprocessor directives, so the conditional scope
    # has to be read from the comment-stripped source, which keeps them. Both
    # views preserve every offset, so line indices agree between them.
    line_starts = _line_starts(source)
    scopes = selftest_scope_map(_strip_comments(raw_source))
    owners = enclosing_functions(source, line_starts)
    lines = source.split("\n")

    call_sites: dict[str | None, list[tuple[str | None, str]]] = {}
    for idx, line in enumerate(lines):
        for m in _HELPER_CALL_RE.finditer(line):
            call_sites.setdefault(m.group("name"), []).append((owners[idx], scopes[idx]))

    def expand(scope: str) -> set[str]:
        return {"production", "selftest"} if scope == "both" else {scope}

    reach_of: dict[str | None, set[str]] = {}

    def reach(function: str | None, active: frozenset[str | None] = frozenset()) -> set[str]:
        """The scopes ``function`` runs in: sr_hle_init runs in both; uncalled helpers run in neither."""
        if function in reach_of:
            return reach_of[function]
        if function in (None, "sr_hle_init"):
            found = {"production", "selftest"}
        else:
            sites = call_sites.get(function, [])
            live = [(caller, scope) for caller, scope in sites if caller not in active]
            if not live:
                found = set()
            else:
                found = set().union(*(expand(scope) & reach(caller, active | {function}) for caller, scope in live))
        if not active:
            reach_of[function] = found
        return found

    out: dict[int, dict] = {}
    registration_matches = [
        *_LITERAL_RE.finditer(source),
        *_UNSUPPORTED_LITERAL_RE.finditer(source),
    ]
    for m in registration_matches:
        idx = bisect.bisect_right(line_starts, m.start()) - 1
        owner, own_scope = owners[idx], scopes[idx]
        if own_scope != "both" or owner in (None, "sr_hle_init"):
            reachable = expand(own_scope)
        else:
            reachable = reach(owner)
        out[int(m.group(1), 16)] = {
            "declared_in": owner,
            "reachable_from": sorted(reachable),
        }
    return out


def conformance_cells(header_text: str) -> dict[int, list[dict]]:
    """``kIcMatrix`` probe rows grouped by the NID each row dispatches."""
    cells: dict[int, list[dict]] = {}
    parts = header_text.split("static const IcProbe kIcMatrix[]", 1)
    if len(parts) != 2:
        return cells
    for m in _IC_PROBE_RE.finditer(parts[1]):
        cells.setdefault(int(m.group("nid"), 16), []).append(
            {"api": m.group("api"), "scenario": m.group("scenario")}
        )
    return cells


def _blank_literals(stripped: str) -> str:
    """Blank string and character literal contents, keeping quotes, offsets and newlines.

    Run on comment-free text. A name inside a string (an ``expect`` message, a log
    line) is then not a use, and a brace inside one cannot unbalance a function body.
    """
    blanked: str = hle_sources.blank_comments_and_literals(stripped)
    return blanked


#: A top-level function definition: a column-0 head up to its opening brace.
_SELFTEST_FUNC_HEAD_RE = re.compile(
    r"^(?:[A-Za-z_][A-Za-z0-9_]*[ \t\*]+)+(?P<name>[A-Za-z_][A-Za-z0-9_]*)[ \t]*"
    r"\((?P<params>[^;{}()]*)\)[ \t\r\n]*\{",
    re.MULTILINE,
)
_C_NOT_A_FUNCTION = frozenset({"if", "for", "while", "switch", "return", "sizeof", "else", "do"})
#: `enum { A = 0x1u, B = 0x2u }` -- the body of one enum.
_SELFTEST_ENUM_RE = re.compile(r"\benum\b[^;{}]*\{(?P<body>[^{}]*)\}")
_SELFTEST_ENUM_MEMBER_RE = re.compile(
    r"(?:^|[{,])\s*(?P<sym>[A-Za-z_][A-Za-z0-9_]*)\s*=\s*0x(?P<nid>[0-9a-fA-F]{1,8})[uU]?\s*(?=,|$)"
)
#: `static const uint32_t nid_x = 0x1u;`
_SELFTEST_CONST_RE = re.compile(
    r"\bconst\s+(?:uint32_t|unsigned(?:\s+int)?|int|u32)\s+(?P<sym>[A-Za-z_][A-Za-z0-9_]*)\s*=\s*"
    r"0x(?P<nid>[0-9a-fA-F]{1,8})[uU]?\s*;"
)
_SELFTEST_ANY_DEFINE_RE = re.compile(
    r"^#define[ \t]+(?P<sym>[A-Za-z_][A-Za-z0-9_]*)[ \t]+0x(?P<nid>[0-9a-fA-F]{1,8})[uU]?[ \t]*$",
    re.MULTILINE,
)
_C_IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_C_LITERAL_NID_RE = re.compile(r"0x(?P<nid>[0-9a-fA-F]{1,8})[uU]?")
_C_LEADING_CAST_RE = re.compile(r"^\(\s*(?:const\s+)?(?:uint32_t|unsigned(?:\s+int)?|u32)\s*\)\s*")
#: `name = value` (not `==`, `.name =` or `->name =`).
_C_ASSIGNMENT_RE = re.compile(r"(?<![\w.>])(?P<sym>[A-Za-z_][A-Za-z0-9_]*)[ \t]*=(?!=)")
#: `name[...] = {` -- an array declaration with an initializer.
_C_TABLE_DECL_RE = re.compile(r"\b(?P<sym>[A-Za-z_][A-Za-z0-9_]*)[ \t]*\[[^\]\[;{}]*\][ \t\r\n]*=[ \t\r\n]*\{")
#: `table[i]` or `table[i].nid` -- one element (or one member of one row) of an initializer table.
_SELFTEST_TABLE_ELEMENT_RE = re.compile(
    r"(?P<table>[A-Za-z_][A-Za-z0-9_]*)\s*\[[^\[\]]+\](?:\s*\.\s*(?P<field>[A-Za-z_][A-Za-z0-9_]*))?"
)


def _split_call_args(text: str, open_paren: int) -> list[str]:
    """The top-level arguments of the call whose ``(`` is at ``open_paren``."""
    args: list[str] = []
    depth, start = 0, open_paren + 1
    for i in range(open_paren, len(text)):
        c = text[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                tail = text[start:i].strip()
                if tail or args:
                    args.append(tail)
                return args
        elif c == "," and depth == 1:
            args.append(text[start:i].strip())
            start = i + 1
    return args


def _strip_outer_parens(expr: str) -> str:
    """``expr`` without parentheses that wrap all of it (and any cast just inside them)."""
    while expr.startswith("(") and expr.endswith(")"):
        depth, close = 0, -1
        for i, c in enumerate(expr):
            depth += c == "("
            depth -= c == ")"
            if depth == 0:
                close = i
                break
        if close != len(expr) - 1:
            break
        expr = _C_LEADING_CAST_RE.sub("", expr[1:-1].strip()).strip()
    return expr


def _split_ternary(expr: str) -> tuple[str, str] | None:
    """The two arms of a top-level ``cond ? a : b``, or ``None``."""
    depth, question, nested = 0, -1, 0
    for i, c in enumerate(expr):
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif depth == 0 and c == "?":
            if question < 0:
                question = i
            else:
                nested += 1
        elif depth == 0 and c == ":" and question >= 0:
            if nested:
                nested -= 1
            else:
                return expr[question + 1 : i], expr[i + 1 :]
    return None


def _selftest_parameter_names(params: str) -> list[str]:
    return [idents[-1] if (idents := _C_IDENT_RE.findall(param)) else "" for param in params.split(",")]


def _coalesce_selftest_heads(lines: list[str], boundaries: list[int]) -> None:
    """Keep one brace for equivalent conditional function heads sharing a body."""
    alternatives: list[tuple[int, int]] = []
    signature: tuple[str, list[str]] | None = None
    for start, end in pairwise(boundaries):
        branch = "\n".join(lines[start + 1 : end]).rstrip()
        if not branch:
            continue
        branch = re.sub(r"\A(?:[ \t\r]*\n)+", "", branch)
        head = _SELFTEST_FUNC_HEAD_RE.fullmatch(branch)
        if not head:
            return
        current = head["name"], _selftest_parameter_names(head["params"])
        if signature is not None and signature != current:
            return
        signature = current
        alternatives.append((start + 1, end))
    for start, end in alternatives[1:]:
        lines[start:end] = [" " * len(line) for line in lines[start:end]]


def _selftest_source(source: str) -> str:
    """Blank impossible literal branches and fold equivalent function heads.

    Unknown preprocessor conditions retain their possible variants. Literal
    0 and 1 cannot contribute dead code, including nested branches and elif.
    Only branches consisting of equivalent standalone function heads can
    share a brace. Keep source offsets and preserve other directive text.
    """
    lines = hle_sources.blank_comments_and_literals(source).split("\n")
    directives: dict[int, str] = {}
    # Each frame keeps parent activity, whether another arm is reachable, and arm boundaries.
    groups: list[tuple[bool, bool, list[int]]] = []
    active = True
    for index, line in enumerate(lines):
        directive = _PP_IF_RE.match(line)
        if not directive:
            if not active:
                lines[index] = " " * len(line)
            continue
        directives[index] = line
        lines[index] = " " * len(line)
        kind, expr = directive.groups()
        if kind in ("if", "ifdef", "ifndef"):
            literal = expr if kind == "if" else ""
            groups.append((active, literal != "1", [index]))
            active = active and literal != "0"
        elif groups and kind in ("else", "elif"):
            parent, more, boundaries = groups[-1]
            literal = expr if kind == "elif" else "1"
            active = parent and more and literal != "0"
            boundaries.append(index)
            groups[-1] = (parent, more and literal != "1", boundaries)
        elif groups and kind == "endif":
            active, _, boundaries = groups.pop()
            if len(boundaries) > 1:
                _coalesce_selftest_heads(lines, boundaries + [index])
    for index, line in directives.items():
        lines[index] = line
    return "\n".join(lines)


def selftest_dispatched_nids(selftest_text: str) -> set[int]:
    """NIDs the executable HLE selftest enters through ``sr_syscall``.

    A NID counts when one of these holds:

    * a literal NID handed to ``sr_syscall`` or to a dispatch helper;
    * a named constant (``#define``, ``enum`` member or ``const`` integer) handed
      to ``sr_syscall`` or to a dispatch helper, resolved in the enclosing function
      first and the file second;
    * an entry of an initializer table that the dispatching function iterates:
      ``sr_syscall(&cpu, nids[i])`` credits every element of ``nids[] = {...}``, and
      ``sr_syscall(&cpu, rows[i].nid)`` credits the ``nid`` member of every row of a
      table whose inline ``struct { ... }`` names that member. The table is the one
      declared in the dispatching function before the call, else at file scope;
      each entry resolves as a literal or a named constant, as above.

    A dispatch helper is a function that passes one of its own parameters to
    ``sr_syscall`` or to another dispatch helper as the NID, found to a fixed
    point. Naming a constant anywhere else (``sr_hle_test_is_registered``, a
    comment, a string, a table no dispatch reads) is not a dispatch and credits
    nothing.
    """
    nids: set[int] = set()
    text = _selftest_source(selftest_text)
    for m in _SELFTEST_LITERAL_DISPATCH_RE.finditer(text):
        nids.add(int(m.group("nid"), 16))

    # Function spans, so an enum or const local to one test resolves only inside it.
    spans: list[tuple[int, int, str, list[str]]] = []
    pos = 0
    while True:
        head = _SELFTEST_FUNC_HEAD_RE.search(text, pos)
        if not head:
            break
        if head.group("name") in _C_NOT_A_FUNCTION:
            pos = head.end()
            continue
        depth, end = 0, len(text)
        for i in range(head.end() - 1, len(text)):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    end = i + 1
                    break
        spans.append((head.start(), end, head["name"], _selftest_parameter_names(head["params"])))
        pos = end
    starts = [s[0] for s in spans]

    def owner(offset: int) -> int | None:
        i = bisect.bisect_right(starts, offset) - 1
        return i if i >= 0 and offset < spans[i][1] else None

    consts: dict[int | None, dict[str, set[int]]] = {}

    def add_const(offset: int, sym: str, value: str) -> None:
        consts.setdefault(owner(offset), {}).setdefault(sym, set()).add(int(value, 16))

    for m in _SELFTEST_ANY_DEFINE_RE.finditer(text):
        add_const(m.start(), m.group("sym"), m.group("nid"))
    for m in _SELFTEST_CONST_RE.finditer(text):
        add_const(m.start(), m.group("sym"), m.group("nid"))
    for m in _SELFTEST_ENUM_RE.finditer(text):
        for member in _SELFTEST_ENUM_MEMBER_RE.finditer(m.group("body")):
            add_const(m.start(), member.group("sym"), member.group("nid"))

    # Dispatch helpers: function name -> indexes of the parameters it forwards as the NID.
    dispatch: dict[str, set[int]] = {"sr_syscall": {1}}

    def find_calls() -> list[tuple[int, str, list[str]]]:
        pattern = re.compile(r"\b(" + "|".join(sorted(map(re.escape, dispatch))) + r")[ \t]*\(")
        return [(m.start(), m.group(1), _split_call_args(text, m.end() - 1)) for m in pattern.finditer(text)]

    changed = True
    while changed:
        changed = False
        for offset, name, args in find_calls():
            fn = owner(offset)
            if fn is None:
                continue
            fn_name, params = spans[fn][2], spans[fn][3]
            for index in dispatch[name]:
                arg = args[index] if index < len(args) else ""
                if arg and arg in params and params.index(arg) not in dispatch.get(fn_name, set()):
                    dispatch.setdefault(fn_name, set()).add(params.index(arg))
                    changed = True

    # One pass each: every `name = ...` and every `name[...] = {` in the file, by name.
    assignments: dict[str, list[re.Match[str]]] = {}
    for m in _C_ASSIGNMENT_RE.finditer(text):
        assignments.setdefault(m.group("sym"), []).append(m)
    tables: dict[str, list[re.Match[str]]] = {}
    for m in _C_TABLE_DECL_RE.finditer(text):
        tables.setdefault(m.group("sym"), []).append(m)

    def assigned_values(scope: int | None, ident: str) -> list[tuple[int | None, str]]:
        """``(scope, value)`` of each ``ident = value`` in ``scope``, or anywhere for a file-scope name."""
        found: list[tuple[int | None, str]] = []
        for m in assignments.get(ident, []):
            depth, end = 0, len(text)
            for i in range(m.end(), len(text)):
                c = text[i]
                if c in "([{":
                    depth += 1
                elif c in ")]}":
                    depth -= 1
                    if depth < 0:
                        end = i
                        break
                elif c in ";," and depth == 0:
                    end = i
                    break
            found.append((owner(m.start()), text[m.end() : end]))
        if any(where is None for where, _ in found):
            return found
        return [(where, value) for where, value in found if where == scope]

    def resolve(scope: int | None, value: str, seen: frozenset[tuple[int | None, str]] = frozenset()) -> set[int]:
        """The NIDs ``value`` can hold where ``scope`` dispatches it.

        A literal; a named constant, resolved in ``scope`` first and the file second; both
        arms of a ``cond ? a : b``; or a variable, through every value assigned to it in
        ``scope`` (in any function, when the variable is declared at file scope).
        """
        value = _strip_outer_parens(_C_LEADING_CAST_RE.sub("", value.strip()).strip())
        literal = _C_LITERAL_NID_RE.fullmatch(value)
        if literal:
            return {int(literal.group("nid"), 16)}
        arms = _split_ternary(value)
        if arms is not None:
            return resolve(scope, arms[0], seen) | resolve(scope, arms[1], seen)
        if not _C_IDENT_RE.fullmatch(value):
            return set()
        named = consts.get(scope, {}).get(value) or consts.get(None, {}).get(value)
        if named:
            return set(named)
        if (scope, value) in seen:
            return set()
        out: set[int] = set()
        for where, assigned in assigned_values(scope, value):
            out |= resolve(where, assigned, seen | {(scope, value)})
        return out

    def table_column(fn: int | None, offset: int, table: str, field: str | None) -> set[int]:
        """The NIDs one column of the initializer table ``table`` holds.

        The table is the array declaration with an initializer in the dispatching
        function (the last one before the call) or, failing that, at file scope. A
        ``table[i].field`` dispatch reads the one member the table's inline struct
        names; a declaration this cannot resolve exactly credits nothing.
        """
        found = None
        for m in tables.get(table, []):
            where = owner(m.start())
            if where == fn and fn is not None and m.start() < offset:
                found = m
            elif where is None and found is None:
                found = m
        if found is None:
            return set()
        rows = _split_call_args(text, found.end() - 1)
        scope = owner(found.start())
        if field is None:
            return set().union(*(resolve(scope, row) for row in rows)) if rows else set()
        head = text[max(0, found.start() - 2048) : found.start()]
        inline = re.search(r"\bstruct[ \t\r\n]*\{(?P<body>[^{}]*)\}[ \t\r\n]*$", head)
        if inline is not None:
            body = inline.group("body")
        else:
            # `static const Row rows[] = ...` or `struct Row rows[] = ...`: the member list
            # of the one `typedef struct [Tag] { ... } Row;` or `struct Row { ... };`.
            named = re.search(r"\b(?P<type>[A-Za-z_][A-Za-z0-9_]*)[ \t\r\n]*$", head)
            if named is None:
                return set()
            kind = re.escape(named.group("type"))
            bodies = [
                m.group("body") if m.group("body") is not None else m.group("tagbody")
                for m in re.finditer(
                    r"\btypedef[ \t\r\n]+struct[ \t\r\n]*(?:[A-Za-z_][A-Za-z0-9_]*[ \t\r\n]*)?"
                    r"\{(?P<body>[^{}]*)\}[ \t\r\n]*" + kind + r"[ \t\r\n]*;"
                    r"|\bstruct[ \t\r\n]+" + kind + r"[ \t\r\n]*\{(?P<tagbody>[^{}]*)\}[ \t\r\n]*;",
                    text,
                )
            ]
            if len(bodies) != 1:
                return set()
            body = bodies[0]
        members: list[str] = []
        for decl in body.split(";"):
            for k, declarator in enumerate(decl.split(",")):
                idents = _C_IDENT_RE.findall(declarator.split("[", 1)[0])
                if idents:
                    members.append(idents[-1] if k == 0 else idents[0])
        if field not in members:
            return set()
        column = members.index(field)
        out: set[int] = set()
        for row in rows:
            if not row.startswith("{"):
                continue
            cells = _split_call_args(row, 0)
            designated = [c for c in cells if c.startswith(".")]
            if designated:
                for cell in designated:
                    name, _, value = cell[1:].partition("=")
                    if name.strip() == field:
                        out |= resolve(scope, value)
            elif column < len(cells):
                out |= resolve(scope, cells[column])
        return out

    for offset, name, args in find_calls():
        fn = owner(offset)
        for index in dispatch[name]:
            if index >= len(args):
                continue
            arg = _C_LEADING_CAST_RE.sub("", args[index]).strip()
            subscript = _SELFTEST_TABLE_ELEMENT_RE.fullmatch(arg)
            if subscript:
                nids |= table_column(fn, offset, subscript.group("table"), subscript.group("field"))
            else:
                nids |= resolve(fn, arg)
    return nids


_HEADING_RE = re.compile(r"^#{1,6}\s+(.*?)\s*#*\s*$")
_FENCE_RE = re.compile(r"^\s{0,3}(`{3,}|~{3,})")


def _heading_slug(heading: str) -> str:
    """GitHub-style anchor slug for one Markdown ATX heading."""
    return re.sub(r"\s+", "-", re.sub(r"[^\w\s-]", "", heading.strip().lower())).strip("-")


def _cited_section(ref: str, test_id: str) -> str:
    """The text of the ``path#heading-slug`` section an evidence_ref cites.

    Fails closed on everything a citation could lean on without being checked:
    a malformed reference, a path the canonical publication policy does not
    classify as publishable (an excluded or unclassified record is not public
    evidence), a missing file, or a heading the file does not contain.
    """
    path_text, _, slug = ref.partition("#")
    pure = PurePosixPath(path_text)
    if not path_text or not slug or pure.is_absolute() or ".." in pure.parts:
        raise ManifestError(
            f"test {test_id!r} evidence_ref {ref!r} must be a repo-relative "
            "'path#heading-slug' reference"
        )
    resolution = publication_policy.load_policy(PUBLIC_SOURCE_PROFILE).resolve(path_text)
    if resolution.disposition != publication_policy.INCLUDED:
        raise ManifestError(
            f"test {test_id!r} evidence_ref cites {path_text!r}, which the publication "
            f"policy resolves as {resolution.disposition} ({resolution.rule}); hardware "
            "evidence must cite committed, publication-eligible documentation"
        )
    section: str | None = None
    body: list[str] = []
    try:
        lines = (ROOT / pure).read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise ManifestError(
            f"test {test_id!r} evidence_ref cites {path_text!r}, which is not readable "
            f"in this tree ({error.strerror})"
        ) from error
    # Headings are recognised the way GitHub renders them: a `#` line inside a
    # fenced code block is not a heading, and a repeated heading gets the
    # `-1`, `-2` ... anchor suffix, so a citation resolves to the section a
    # reader sees or fails closed.
    fence: str | None = None
    seen: dict[str, int] = {}
    for line in lines:
        marker = _FENCE_RE.match(line)
        if marker:
            token = marker.group(1)
            if fence is None:
                fence = token
            elif token[0] == fence[0] and len(token) >= len(fence):
                fence = None
        heading = None if fence is not None or marker else _HEADING_RE.match(line)
        if heading:
            if section == slug:
                return "\n".join(body)
            base = _heading_slug(heading.group(1))
            count = seen.get(base, 0)
            seen[base] = count + 1
            section, body = (base if count == 0 else f"{base}-{count}"), []
            continue
        if section == slug:
            body.append(line)
    if section == slug:
        return "\n".join(body)
    raise ManifestError(
        f"test {test_id!r} evidence_ref {ref!r} names no heading in {path_text!r}"
    )


def measured_evidence_section(test: dict) -> str:
    """Check one `MEASURED` oracle row's citation and return the cited text.

    A MEASURED row must name the cases the citation covers in `evidence_cases`,
    every one of them must be one of the row's own `case_ids`/`apis`, and the
    cited section must contain each of them verbatim.  Every one of the row's
    `case_ids` must be either cited that way or listed in `uncited_cases`, so a
    partial citation is explicit rather than silently standing for the whole
    row.  Only the `apis` named in `evidence_cases` are granted the tier (see
    `oracle_exercised_apis`).
    """
    test_id = test.get("id", "?")
    ref = test.get("evidence_ref")
    cases = test.get("evidence_cases")
    if not isinstance(ref, str) or not ref:
        raise ManifestError(
            f"test {test_id!r} claims hardware_evidence MEASURED with no evidence_ref"
        )
    if (
        not isinstance(cases, list)
        or not cases
        or not all(isinstance(case, str) and case for case in cases)
    ):
        raise ManifestError(
            f"test {test_id!r} claims hardware_evidence MEASURED with no evidence_cases"
        )
    declared = set(test.get("case_ids", [])) | set(test.get("apis", []))
    undeclared = sorted({case for case in cases if case not in declared})
    if undeclared:
        raise ManifestError(
            f"test {test_id!r} evidence_cases {undeclared} are neither its case_ids nor its apis"
        )
    uncited = test.get("uncited_cases", [])
    if not isinstance(uncited, list) or not all(isinstance(c, str) and c for c in uncited):
        raise ManifestError(f"test {test_id!r} uncited_cases must be a list of case ids")
    stray = sorted(set(uncited) - set(test.get("case_ids", [])))
    if stray:
        raise ManifestError(f"test {test_id!r} uncited_cases {stray} are not its case_ids")
    both = sorted(set(uncited) & set(cases))
    if both:
        raise ManifestError(
            f"test {test_id!r} lists {both} in both evidence_cases and uncited_cases"
        )
    unaccounted = sorted(
        set(test.get("case_ids", [])) - set(cases) - set(uncited)
    )
    if unaccounted:
        raise ManifestError(
            f"test {test_id!r} case_ids {unaccounted} are neither cited in evidence_cases "
            "nor listed in uncited_cases; a partial citation must say what it omits"
        )
    section = _cited_section(ref, test_id)
    unnamed = sorted(
        {
            case
            for case in cases
            if re.search(rf"(?<![A-Za-z0-9_-]){re.escape(case)}(?![A-Za-z0-9_-])", section) is None
        }
    )
    if unnamed:
        raise ManifestError(
            f"test {test_id!r} evidence_ref {ref!r} does not name {unnamed}; a MEASURED "
            "row must cite evidence of the exact cases it claims"
        )
    return section


def oracle_exercised_apis(manifest: dict) -> dict[str, list[str]]:
    """API name -> ids of measured source-owned PSP probes that call it.

    The strongest evidence tier is granted from the oracle manifest's explicit
    `hardware_evidence` field alone.  `status: implemented` is hand-maintained
    probe-source prose and never grants evidence; `MEASURED` additionally has to
    cite committed, publication-eligible documentation that names every case the
    row claims (see `measured_evidence_section`).
    """
    out: dict[str, list[str]] = {}
    for test in manifest.get("tests", []):
        test_id = test.get("id", "?")
        hardware_evidence = test.get("hardware_evidence", "NOT_RUN")
        if (
            not isinstance(hardware_evidence, str)
            or hardware_evidence not in HARDWARE_EVIDENCE_VALUES
        ):
            raise ManifestError(
                f"test {test_id!r} has unknown hardware_evidence "
                f"{hardware_evidence!r}; expected one of {sorted(HARDWARE_EVIDENCE_VALUES)}"
            )
        if hardware_evidence != "MEASURED":
            continue
        measured_evidence_section(test)
        # The tier follows the citation, not the row: an API the cited section
        # does not name is not measured by it, whatever else the row lists.
        cited = set(test.get("evidence_cases", []))
        for api in test.get("apis", []):
            if api in cited:
                out.setdefault(api, []).append(test.get("id", "?"))
        # `case_apis` cells (see `oracle_measured_cells`) are annotation only and never reach
        # this tier: a row's own assertion cannot grant itself the strongest tier.
    return out


_CASE_API_CALL_RE = re.compile(r"(\w+)\(.*\)", re.DOTALL)


def _case_api_cells(test: dict) -> list[dict]:
    """The validated `case_apis` cells of one `MEASURED` oracle row.

    `case_apis` maps a cited case id to the API calls that case's own source makes and the
    exact cell each call measured: `{"call": "sceX(0)", "cell": "mode 0 from a user thread"}`.
    A cell annotates the exact call measured and never grants a tier: whole-function
    promotion stays with the citation rule of `oracle_exercised_apis`.
    """
    test_id = test.get("id", "?")
    raw = test.get("case_apis")
    if raw is None:
        return []
    if test.get("hardware_evidence") != "MEASURED":
        raise ManifestError(f"test {test_id!r} has case_apis but is not a MEASURED row")
    if not isinstance(raw, dict) or not raw:
        raise ManifestError(f"test {test_id!r} case_apis must map case ids to call lists")
    cited = set(test.get("evidence_cases", []))
    apis = set(test.get("apis", []))
    cells: list[dict] = []
    for case, entries in raw.items():
        if case not in cited or case not in test.get("case_ids", []):
            raise ManifestError(
                f"test {test_id!r} case_apis case {case!r} is not one of its cited case_ids"
            )
        if not isinstance(entries, list) or not entries:
            raise ManifestError(f"test {test_id!r} case_apis {case!r} must be a non-empty list")
        for entry in entries:
            call = entry.get("call") if isinstance(entry, dict) else None
            cell = entry.get("cell") if isinstance(entry, dict) else None
            match = _CASE_API_CALL_RE.fullmatch(call) if isinstance(call, str) else None
            if (
                match is None
                or not isinstance(cell, str)
                or not cell
                or set(entry) - {"call", "cell"}
            ):
                raise ManifestError(
                    f"test {test_id!r} case_apis {case!r} entry {entry!r} must be "
                    '{"call": "api(args)", "cell": "<what was measured>"} and no other key'
                )
            if match.group(1) not in apis:
                raise ManifestError(
                    f"test {test_id!r} case_apis {case!r} calls {match.group(1)!r}, "
                    "which is not one of its apis"
                )
            cells.append({
                "oracle_id": test_id, "case": case, "api": match.group(1),
                "call": call, "cell": cell,
            })
    return cells


def _oracle_case_source(source: str, test_id: str, case: str) -> list[str]:
    """The probe source that runs before each record of `case`, back to the previous record of
    the same test (or the start of the enclosing function)."""
    spans = []
    for hit in re.finditer(rf'"{re.escape(test_id)}"\s*,\s*"{re.escape(case)}"', source):
        start = max(source.rfind("\nstatic ", 0, hit.start()), source.rfind(f'"{test_id}"', 0, hit.start()), 0)
        spans.append(source[start:hit.start()])
    return spans


def oracle_measured_cells(manifest: dict, sources: dict[str, str] | None = None) -> dict[str, list[dict]]:
    """API name -> the exact cells measured PSP-side, from each case's own probe source.

    Every `case_apis` call must appear verbatim in the source that runs before the case's
    record, so a cell names an argument the probe really passed.  `sources` maps a row's
    `source` path to its text (the repository file by default).
    """
    out: dict[str, list[dict]] = {}
    for test in manifest.get("tests", []):
        cells = _case_api_cells(test)
        if not cells:
            continue
        test_id = test.get("id", "?")
        path = test.get("source", "")
        text = sources[path] if sources is not None and path in sources else _read_optional(ROOT / path)
        for cell in cells:
            if not any(cell["call"] in span for span in _oracle_case_source(text, test_id, cell["case"])):
                raise ManifestError(
                    f"test {test_id!r} case_apis {cell['case']!r} call {cell['call']!r} is not made "
                    f"in the source that records that case ({path!r})"
                )
            out.setdefault(cell["api"], []).append(cell)
    return out


def evidence_tier(entry: dict) -> tuple[str, str]:
    """The strongest tier this NID's own links justify, plus the reason.

    Deliberately conservative.  Hardware truth transcribed into the conformance
    matrix describes the PSP, not this runtime, so a conformance cell alone never
    promotes a registration past HOST_TESTED.  Only a source-owned probe that
    actually ran on hardware carries HARDWARE_MEASURED, and that tier describes
    the API exercise, not the correctness of this handler.
    """
    reach = entry["registration"]["reachable_from"]
    ex = entry["exercised"]
    if ex["psp_oracle_probes"]:
        if entry["registration"]["classification"] == "fake_success":
            return "NOT_EVIDENCE", (
                "a source-owned PSP probe calls this API on hardware, but the registered "
                "handler is a generic success stub, so the exercise asserts nothing about the API"
            )
        return "HARDWARE_MEASURED", "a source-owned PSP probe calls this API on hardware"
    if ex["conformance_cells"] or ex["hle_selftest_dispatch"]:
        if entry["registration"]["classification"] == "fake_success":
            return "NOT_EVIDENCE", (
                "an executable test enters this NID, but the registered handler is a "
                "generic success stub, so the exercise asserts nothing about the API"
            )
        if "production" in reach:
            return "HOST_TESTED", "an executable test enters the production registration"
        return "HOST_TESTED", "an executable test enters it, but only the selftest build registers it"
    if entry["registration"]["classification"] == "fake_success":
        return "NOT_EVIDENCE", "generic success handler with no executable coverage"
    return "STATICALLY_SUPPORTED", "dedicated handler registered; no executable coverage"


def load_imports(path: Path | None) -> dict[int, dict[str, str]]:
    """Optional imported-NID side of the chain.

    The retail import manifest is a private input, so this is opt-in and absent
    by default; the chain then records the imported link as unknown rather than
    claiming the NID is unimported.

    A text manifest names each NID: ``{"name": ...}``. A PSP module (an ELF or
    PRX, such as the public profile-zero guest) is read through its import
    table with tools/psp_import_table.py, which carries NIDs but no function
    names; the link is ``{"source": "psp module import table"}`` and no name is
    invented. No library is claimed either: the parser reports stub-pairing
    findings for some linker layouts, so only the NID itself is relied on.
    """
    if path is None:
        return {}
    data = path.read_bytes()
    if data[:4] == b"\x7fELF":
        try:
            table = psp_import_table.parse_import_table(data)
        except psp_import_table.ImportTableError as e:
            raise ManifestError(f"{path}: unreadable PSP import table: {e}") from e
        return {f.nid: {"source": "psp module import table"} for f in table.funcs}
    text = data.decode("utf-8")
    found: dict[int, dict[str, str]] = {}
    for m in re.finditer(r'nid\s*=\s*0x([0-9a-fA-F]{1,8}).*?name\s*=\s*"([^"]+)"', text, re.DOTALL):
        found[int(m.group(1), 16)] = {"name": m.group(2)}
    if not found:
        for m in _LITERAL_RE.finditer(text):
            found[int(m.group(1), 16)] = {"name": m.group(2)}
    return found


def is_dedicated_unreviewed(reg: dict) -> bool:
    """A manifest registration whose dedicated handler nobody has reviewed (#341)."""
    returned: bool = reg["classification"] == "dedicated" and reg["status"] == "unreviewed"
    return returned


def unreviewed_import_gate(
    manifest: dict,
    imported: Collection[int],
    allowlist: Mapping[int, dict] | None = None,
) -> dict:
    """Check one module's imported NIDs against HLE review status (#341).

    An imported NID that routes to a dedicated `unreviewed` handler must be on
    `allowlist` (by default hle_registry_meta.PROFILE_ZERO_UNREVIEWED_IMPORTS),
    and every allowlist entry must still be both imported and unreviewed, so
    the list can only shrink. An empty import set cannot pass: it would prove
    nothing about the module.
    """
    if allowlist is None:
        allowlist = meta.PROFILE_ZERO_UNREVIEWED_IMPORTS
    imported = set(imported)
    if not imported:
        raise ManifestError("no imported NIDs were read; the import gate cannot pass on an empty import set")
    regs = {int(r["nid"], 16): r for r in manifest["registrations"]}

    def row(nid: int) -> dict:
        reg = regs[nid]
        return {"nid": f"0x{nid:08x}", "name": reg["name"], "handler": reg["handler"]}

    imported_unreviewed = sorted(n for n in imported if n in regs and is_dedicated_unreviewed(regs[n]))
    stale = []
    for nid, entry in sorted(allowlist.items()):
        if nid not in imported:
            reason = "not imported by the module"
        elif nid not in regs:
            reason = "not registered by hle.c"
        elif not is_dedicated_unreviewed(regs[nid]):
            reg = regs[nid]
            reason = f"no longer unreviewed ({reg['classification']}, status {reg['status']})"
        else:
            continue
        stale.append({"nid": f"0x{nid:08x}", "name": entry.get("name"), "reason": reason})
    return {
        "imported": len(imported),
        "imported_unreviewed": [row(n) for n in imported_unreviewed],
        "unallowed": [row(n) for n in imported_unreviewed if n not in allowlist],
        "stale": stale,
    }



# ---------------------------------------------------------------------------
# Triage
# ---------------------------------------------------------------------------
# Reincorporated from the census concept in PR #76, minus its curated
# per-module weight table. That table is unsourced editorial judgement, and an
# unattested judgement is exactly what a provenance-blocked tool should not
# smuggle in. What survives is the part the chain can already measure: which
# registrations have no executable coverage, how large their module family is,
# and which public tests already name them.

#: `sceKernelFoo` / `__sceSasBar` / `_sceKernelBaz` -> the owning PSP module bucket.
_MODULE_RE = re.compile(r"^(?:_+)?(sce[A-Z][a-z0-9]*)")

#: A NID or registered name mentioned by a public test.
PUBLIC_TEST_GLOBS = ("tools/test_*.py", "src/rt/*_selftest.c")


#: Names whose library is not the leading camel-case word: the sceMpegbase library exports
#: sceMpegBaseCscAvc and its siblings (and sceMpegbase_0530BE4E), not sceMpeg.
_MODULE_PREFIX_LIBRARIES = (("sceMpegBase", "sceMpegbase"),)


def derive_module(name: str) -> str:
    """Best-effort PSP module bucket. Grouping only -- never a support claim."""
    bare = name.lstrip("_")
    for prefix, library in _MODULE_PREFIX_LIBRARIES:
        if bare.startswith(prefix):
            return library
    m = _MODULE_RE.match(name)
    if m:
        return m.group(1)
    if "newlib" in name:
        return "newlib"
    return "other"


_PUBLIC_TEST_TOKEN_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*|0x[0-9a-fA-F]+")


def public_test_references(names: set[str], nids: set[str]) -> dict[str, list[str]]:
    """Public test files that mention each registered name or NID literal.

    A mention is a reference, not a test of the API. It is used to rank
    attention, never to claim coverage -- the chain's exercise links are what
    carry that, and they are computed separately.
    """
    keys = names | nids
    refs: dict[str, list[str]] = {}
    for pattern in PUBLIC_TEST_GLOBS:
        for path in sorted(ROOT.glob(pattern)):
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            rel = path.relative_to(ROOT).as_posix()
            tokens = {
                f"0x{int(t, 16):08x}" if t.startswith(("0x", "0X")) else t
                for t in _PUBLIC_TEST_TOKEN_RE.findall(text)
            }
            for key in keys & tokens:
                refs.setdefault(key, []).append(rel)
    return refs


def triage_candidates(entries: list[dict], top: int) -> list[dict]:
    """Rank registrations that carry no executable evidence.

    Every component is derived from something already in the chain, and each
    one is emitted alongside the score, so a ranking can be argued with rather
    than merely accepted.
    """
    module_counts: dict[str, int] = {}
    for e in entries:
        module_counts[e["module"]] = module_counts.get(e["module"], 0) + 1

    ranked = []
    for e in entries:
        if e["evidence_tier"] not in ("STATICALLY_SUPPORTED", "NOT_EVIDENCE"):
            continue
        family = module_counts.get(e["module"], 0)
        family_norm = min(family / 20.0, 1.0)
        mentions = min(len(e["public_test_references"]), 3)
        # A stub that nothing exercises is worth more attention than a
        # dedicated handler that nothing exercises: it silently reports success.
        stub = 1.0 if e["registration"]["classification"] == "fake_success" else 0.0
        score = family_norm + 0.5 * mentions + stub
        ranked.append({
            "nid": e["nid"],
            "name": e["canonical_name"],
            "handler": e["registration"]["handler"],
            "module": e["module"],
            "evidence_tier": e["evidence_tier"],
            "score": round(score, 3),
            "score_components": {
                "module_family_size": family,
                "family_norm": round(family_norm, 3),
                "public_test_mentions": mentions,
                "unexercised_stub": stub,
            },
            "public_test_references": e["public_test_references"],
        })
    ranked.sort(key=lambda t: (-t["score"], t["nid"]))
    for i, t in enumerate(ranked[:top], start=1):
        t["rank"] = i
    return ranked[:top]


def build_evidence_chain(manifest: dict | None = None, imports_path: Path | None = None,
                         top: int = 30) -> dict:
    """Join every registration to the evidence that does, or does not, back it."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import nid_name_proof

    if manifest is None:
        manifest = build_manifest()
    # hle_power.c owns scePower/sceKernelPower registrations (issue #307); read it with hle.c.
    scopes = registration_scopes(runtime_hle_source())
    cells = conformance_cells(_read_optional(INTR_CONFORMANCE_H))
    dispatched = selftest_dispatched_nids(_read_optional(SELFTEST_C))
    raw_oracle = _read_optional(PSP_ORACLE_MANIFEST)
    try:
        oracle_doc = json.loads(raw_oracle) if raw_oracle else {}
    except json.JSONDecodeError as e:
        raise ManifestError(f"{PSP_ORACLE_MANIFEST} is not valid JSON: {e.msg} at line {e.lineno}") from e
    oracle = oracle_exercised_apis(oracle_doc)
    oracle_cells = oracle_measured_cells(oracle_doc)
    imports = load_imports(imports_path)
    test_refs = public_test_references(
        {r["name"] for r in manifest["registrations"]},
        {r["nid"] for r in manifest["registrations"]},
    )
    verified_names = {
        r["name"] for r in manifest["registrations"]
        if nid_name_proof.nid_of(r["name"]) == int(r["nid"], 16)
    }

    entries = []
    for reg in manifest["registrations"]:
        nid = int(reg["nid"], 16)
        derived = nid_name_proof.nid_of(reg["name"])
        scope = scopes.get(nid, {"declared_in": None, "reachable_from": []})
        entry = {
            "nid": reg["nid"],
            "canonical_name": reg["name"],
            "module": derive_module(reg["name"]),
            "public_test_references": sorted(
                set(test_refs.get(reg["name"], [])) | set(test_refs.get(reg["nid"], []))
            ),
            "nid_derivation": {
                "independently_derived": derived == nid,
                "method": "nid == sha1(name)[0:4] little-endian",
                "derived_nid": f"0x{derived:08x}",
                # For a label whose NID is not reproducible, defer to
                # nid_name_proof's own shape classification instead of merely
                # flagging it: most are editorial aliases or firmware-suffixed
                # composites, which is a different fact from "wrong".
                "shape": (
                    None if derived == nid
                    else nid_name_proof.classify(nid, reg["name"], verified_names)._asdict()
                ),
            },
            "imported": (
                imports[nid] if nid in imports
                else (None if imports else "unknown: no import manifest supplied")
            ),
            "registration": {
                "handler": reg["handler"],
                "origin": reg["origin"],
                "classification": reg["classification"],
                "status": reg["status"],
                "declared_in": scope["declared_in"],
                "reachable_from": scope["reachable_from"],
            },
            "exercised": {
                "conformance_cells": cells.get(nid, []),
                "hle_selftest_dispatch": nid in dispatched,
                "psp_oracle_probes": oracle.get(reg["name"], []),
                "psp_oracle_cells": oracle_cells.get(reg["name"], []),
            },
        }
        entry["exercised_stub"] = bool(
            (
                entry["exercised"]["conformance_cells"]
                or entry["exercised"]["hle_selftest_dispatch"]
                or entry["exercised"]["psp_oracle_probes"]
            )
            and reg["classification"] == "fake_success"
        )
        tier, why = evidence_tier(entry)
        entry["evidence_tier"] = tier
        entry["evidence_rationale"] = why
        entries.append(entry)

    return {
        "schema": CHAIN_SCHEMA,
        "source": manifest["source"],
        "registrations": len(entries),
        "summary": {
            "by_tier": {t: sum(1 for e in entries if e["evidence_tier"] == t) for t in EVIDENCE_TIERS},
            "nid_not_independently_derived": [
                e["nid"] for e in entries if not e["nid_derivation"]["independently_derived"]
            ],
            "by_module": {
                m: sum(1 for e in entries if e["module"] == m)
                for m in sorted({e["module"] for e in entries})
            },
            "not_reachable_from_production": [
                e["nid"] for e in entries if "production" not in e["registration"]["reachable_from"]
            ],
            "exercised_stubs": [
                {"nid": e["nid"], "name": e["canonical_name"], "handler": e["registration"]["handler"]}
                for e in entries if e["exercised_stub"]
            ],
            # #341: dedicated handlers nobody has reviewed that the supplied module imports.
            "imported_unreviewed": (
                [
                    {"nid": e["nid"], "name": e["canonical_name"], "handler": e["registration"]["handler"]}
                    for e in entries
                    if isinstance(e["imported"], dict) and is_dedicated_unreviewed(e["registration"])
                ]
                if imports else "unknown: no import manifest supplied"
            ),
        },
        "triage": triage_candidates(entries, top),
        "entries": entries,
    }


# ---------------------------------------------------------------------------
# Semantic status census (#341)
# ---------------------------------------------------------------------------
# Deterministic census of every dedicated handler by semantic status
# (complete / partial / compatibility / controlled_unsupported / unreviewed),
# grouped by API family (library name). Generic success handlers (h_ok)
# and refusal markers (h_ControlledUnsupported) are excluded.

CENSUS_SCHEMA = 2
CENSUS_STATUSES = (
    "complete",
    "partial",
    "compatibility",
    "controlled_unsupported",
    "unreviewed",
)


def build_census(manifest: dict | None = None) -> dict:
    """Generate a deterministic census of dedicated handlers grouped by API family.

    Each dedicated handler is classified into one of the semantic statuses:
    complete / partial / compatibility / controlled_unsupported / unreviewed.
    Generic stubs (h_ok family, mechanical stubs) and generic refusal markers
    (h_ControlledUnsupported) are excluded so the census reflects dedicated
    handler maturity.
    """
    if manifest is None:
        manifest = build_manifest()

    regs = manifest["registrations"]
    generic = set(meta.GENERIC_SUCCESS_HANDLERS) | {"h_ControlledUnsupported"}

    by_handler: dict[str, list[dict]] = {}
    for r in regs:
        h = r["handler"]
        if h in generic or r["classification"] == "fake_success":
            continue
        by_handler.setdefault(h, []).append(r)

    handler_entries: list[dict] = []
    for h, h_regs in sorted(by_handler.items()):
        status = meta.HANDLER_STATUS.get(h, "unreviewed")
        modules = [derive_module(r["name"]) for r in h_regs]
        non_other = [m for m in modules if m != "other"]
        family = non_other[0] if non_other else (modules[0] if modules else "other")

        apis = sorted(
            [{"nid": r["nid"], "name": r["name"]} for r in h_regs],
            key=lambda a: (int(a["nid"], 16), a["name"]),
        )
        entry: dict = {
            "handler": h,
            "api_family": family,
            "status": status,
            "registrations": apis,
        }
        evidence = meta.HANDLER_EVIDENCE.get(h, [])
        if evidence:
            entry["evidence"] = evidence
        if status in ("partial", "compatibility"):
            entry["limitation"] = meta.HANDLER_LIMITATIONS.get(h, "")
        handler_entries.append(entry)

    handler_entries.sort(key=lambda e: (e["api_family"], e["handler"]))

    families: dict[str, dict] = {}
    for entry in handler_entries:
        fam = entry["api_family"]
        if fam not in families:
            families[fam] = {
                "api_family": fam,
                "summary": {s: 0 for s in CENSUS_STATUSES},
                "total_handlers": 0,
                "total_registrations": 0,
                "handlers": [],
            }
        fam_dict = families[fam]
        fam_dict["handlers"].append(entry)
        fam_dict["total_handlers"] += 1
        fam_dict["total_registrations"] += len(entry["registrations"])
        fam_dict["summary"][entry["status"]] = fam_dict["summary"].get(entry["status"], 0) + 1

    total_by_status = {s: 0 for s in CENSUS_STATUSES}
    status_registrations = {s: 0 for s in CENSUS_STATUSES}
    for e in handler_entries:
        total_by_status[e["status"]] = total_by_status.get(e["status"], 0) + 1
        status_registrations[e["status"]] = (
            status_registrations.get(e["status"], 0) + len(e["registrations"])
        )

    total_registrations = sum(len(e["registrations"]) for e in handler_entries)
    registration_classification: dict[str, int] = {}
    for reg in regs:
        classification = reg["classification"]
        registration_classification[classification] = (
            registration_classification.get(classification, 0) + 1
        )

    by_family_summary = {
        fam: {
            **data["summary"],
            "total_handlers": data["total_handlers"],
            "total_registrations": data["total_registrations"],
        }
        for fam, data in sorted(families.items())
    }

    return {
        "schema": CENSUS_SCHEMA,
        "source": manifest.get("source", MANIFEST_SOURCE),
        "summary": {
            "total_registrations": len(regs),
            "registration_classification": dict(sorted(registration_classification.items())),
            "total_dedicated_handlers": len(handler_entries),
            "total_dedicated_registrations": total_registrations,
            "by_status": total_by_status,
            "by_status_registrations": status_registrations,
            "by_family": by_family_summary,
        },
        "families": {fam: families[fam] for fam in sorted(families)},
        "handlers": handler_entries,
    }


def format_census_summary(census: dict) -> str:
    """One line carrying every count a reader would otherwise retype into prose.

    The census is the only current machine source for registration classes
    (including the fake-success and controlled-unsupported rows) and for the
    per-status handler counts, so the CLI prints all of them together instead of
    leaving a consumer to recompute the split from the JSON.
    """
    s = census["summary"]
    classes = s["registration_classification"]
    return (
        f"{s['total_registrations']} registrations "
        f"(dedicated {classes.get('dedicated', 0)}, "
        f"fake_success {classes.get('fake_success', 0)}, "
        f"controlled_unsupported {classes.get('controlled_unsupported', 0)}); "
        f"{s['total_dedicated_handlers']} handlers across {len(s['by_family'])} families ("
        + ", ".join(f"{status} {s['by_status'][status]}" for status in CENSUS_STATUSES)
        + ")"
    )


def render_census_markdown(census: dict, *, heading_level: int = 1) -> str:
    """Render a GitHub-flavored Markdown summary table and review details."""
    if not 1 <= heading_level <= 5:
        raise ValueError("heading_level must leave room for the detail headings")
    heading = "#" * heading_level
    detail_heading = "#" * (heading_level + 1)
    out = []
    out.append(f"{heading} HLE Semantic Status Census")
    out.append("")
    s = census["summary"]
    out.append(
        f"Registered NIDs: **{s['total_registrations']}** "
        f"(dedicated **{s['registration_classification'].get('dedicated', 0)}**, "
        f"fake_success **{s['registration_classification'].get('fake_success', 0)}**, "
        f"controlled_unsupported **{s['registration_classification'].get('controlled_unsupported', 0)}**)."
    )
    out.append(
        f"Semantic handler census: **{s['total_dedicated_handlers']}** handlers "
        f"across **{len(s['by_family'])}** API families, covering "
        f"**{s['total_dedicated_registrations']}** handler-associated NID registrations."
    )
    out.append("")
    out.append("| Semantic Status | Handlers | NID Registrations |")
    out.append("| :--- | :---: | :---: |")
    for status in CENSUS_STATUSES:
        out.append(
            f"| `{status}` | {s['by_status'][status]} | "
            f"{s['by_status_registrations'][status]} |"
        )
    out.append("")
    out.append(
        "| API Family | Complete | Partial | Compatibility | Controlled Unsupported | "
        "Unreviewed | Total Handlers | NID Registrations |"
    )
    out.append("| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |")

    tot = s["by_status"]
    for fam, stats in sorted(s["by_family"].items()):
        out.append(
            f"| `{fam}` | {stats['complete']} | {stats['partial']} | {stats['compatibility']} | "
            f"{stats['controlled_unsupported']} | {stats['unreviewed']} | "
            f"{stats['total_handlers']} | {stats['total_registrations']} |"
        )
    out.append(
        f"| **Total** | **{tot['complete']}** | **{tot['partial']}** | **{tot['compatibility']}** | "
        f"**{tot['controlled_unsupported']}** | **{tot['unreviewed']}** | "
        f"**{s['total_dedicated_handlers']}** | **{s['total_dedicated_registrations']}** |"
    )
    out.append("")

    handlers = census["handlers"]
    complete_handlers = [h for h in handlers if h["status"] == "complete"]
    partial_handlers = [h for h in handlers if h["status"] == "partial"]
    compat_handlers = [h for h in handlers if h["status"] == "compatibility"]
    controlled_handlers = [h for h in handlers if h["status"] == "controlled_unsupported"]

    if complete_handlers:
        out.append(f"{detail_heading} Complete Handlers (Evidence-Backed)")
        out.append("")
        for h in complete_handlers:
            api_names = ", ".join(f"`{a['name']}` ({a['nid']})" for a in h["registrations"])
            out.append(f"- **`{h['handler']}`** (`{h['api_family']}`): {api_names}")
            for ev in h.get("evidence", []):
                out.append(f"  - Evidence: {ev}")
        out.append("")

    if partial_handlers:
        out.append(f"{detail_heading} Partial Handlers (Named Limitations)")
        out.append("")
        for h in partial_handlers:
            api_names = ", ".join(f"`{a['name']}` ({a['nid']})" for a in h["registrations"])
            out.append(f"- **`{h['handler']}`** (`{h['api_family']}`): {api_names}")
            out.append(f"  - Limitation: {h.get('limitation', '')}")
            for ev in h.get("evidence", []):
                out.append(f"  - Evidence: {ev}")
        out.append("")

    if compat_handlers:
        out.append(f"{detail_heading} Compatibility Handlers (Named Limitations)")
        out.append("")
        for h in compat_handlers:
            api_names = ", ".join(f"`{a['name']}` ({a['nid']})" for a in h["registrations"])
            out.append(f"- **`{h['handler']}`** (`{h['api_family']}`): {api_names}")
            out.append(f"  - Limitation: {h.get('limitation', '')}")
            for ev in h.get("evidence", []):
                out.append(f"  - Evidence: {ev}")
        out.append("")

    if controlled_handlers:
        out.append(f"{detail_heading} Controlled Unsupported Dedicated Handlers")
        out.append("")
        for h in controlled_handlers:
            api_names = ", ".join(f"`{a['name']}` ({a['nid']})" for a in h["registrations"])
            out.append(f"- **`{h['handler']}`** (`{h['api_family']}`): {api_names}")
        out.append("")

    return "\n".join(out)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "hle_manifest.json")
    ap.add_argument(
        "--write-baseline",
        nargs="?",
        const=DEFAULT_BASELINE,
        type=Path,
        default=None,
        help="refresh the committed classification baseline (review the diff for downgrades)",
    )
    ap.add_argument(
        "--evidence-chain",
        nargs="?",
        const=ROOT / "build" / "hle_evidence_chain.json",
        type=Path,
        default=None,
        help="also emit the per-NID evidence chain (name -> NID -> registration -> "
             "production reachability -> exercise -> tier)",
    )
    ap.add_argument(
        "--census",
        "--census-json",
        dest="census",
        nargs="?",
        const=ROOT / "build" / "hle_census.json",
        type=Path,
        default=None,
        help="emit the machine-readable semantic census artifact: the single current "
             "source for registration classes (dedicated / fake_success / "
             "controlled_unsupported) and for the per-status handler counts "
             "(complete / partial / compatibility / controlled_unsupported / "
             "unreviewed) by API family; --census-json is an alias",
    )
    ap.add_argument(
        "--census-markdown",
        nargs="?",
        const=ROOT / "build" / "hle_census.md",
        type=Path,
        default=None,
        help="emit the Markdown rendering of the same census for the generated block in "
             "docs/HLE_AND_WORKAROUND_INVENTORY.md",
    )
    ap.add_argument(
        "--census-heading-level",
        type=int,
        choices=range(1, 6),
        default=1,
        help="top heading level for --census-markdown (the inventory doc embeds level 3)",
    )
    ap.add_argument(
        "--triage-top",
        type=int,
        default=30,
        help="how many unexercised registrations to rank in the chain triage list",
    )
    ap.add_argument(
        "--imports",
        type=Path,
        default=None,
        help="optional import manifest, or a PSP module (ELF/PRX) whose import table is "
             "read, supplying the imported-NID link; retail manifests are private "
             "inputs, so the link is unknown without it",
    )
    ap.add_argument(
        "--check-profile-zero-imports",
        action="store_true",
        help="fail when a NID the --imports module (the profile-zero guest) imports routes "
             "to a dedicated unreviewed handler missing from "
             "hle_registry_meta.PROFILE_ZERO_UNREVIEWED_IMPORTS, or when an entry there is "
             "stale (#341)",
    )
    args = ap.parse_args(argv)
    if args.check_profile_zero_imports and args.imports is None:
        ap.error("--check-profile-zero-imports needs --imports <profile-zero guest module>")
    try:
        manifest = build_manifest()
        chain = (
            build_evidence_chain(manifest, args.imports, args.triage_top)
            if args.evidence_chain is not None
            else None
        )
        gate = (
            unreviewed_import_gate(manifest, load_imports(args.imports))
            if args.check_profile_zero_imports
            else None
        )
    except (ManifestError, OSError, UnicodeDecodeError) as e:
        print(f"hle_manifest: {e}", file=sys.stderr)
        return 1
    dump_json(manifest, args.out)
    print(f"hle_manifest: {len(manifest['registrations'])} registrations -> {args.out}")
    if chain is not None:
        dump_json(chain, args.evidence_chain)
        tiers = ", ".join(f"{k}={v}" for k, v in chain["summary"]["by_tier"].items())
        print(f"hle_manifest: evidence chain -> {args.evidence_chain} ({tiers})")
    if args.census is not None or args.census_markdown is not None:
        census = build_census(manifest)
        summary = format_census_summary(census)
        if args.census is not None:
            dump_json(census, args.census)
            print(f"hle_manifest: census -> {args.census} ({summary})")
        if args.census_markdown is not None:
            md_text = render_census_markdown(census, heading_level=args.census_heading_level)
            args.census_markdown.parent.mkdir(parents=True, exist_ok=True)
            args.census_markdown.write_text(md_text, encoding="utf-8")
            print(f"hle_manifest: census markdown -> {args.census_markdown} ({summary})")
    if args.write_baseline is not None:
        dump_json(manifest_to_baseline(manifest), args.write_baseline)
        print(f"hle_manifest: baseline -> {args.write_baseline}")
    if gate is not None:
        for row in gate["unallowed"]:
            print(
                f"hle_manifest: {args.imports} imports {row['nid']} {row['name']}, routed to the "
                f"unreviewed handler {row['handler']} and not on PROFILE_ZERO_UNREVIEWED_IMPORTS; "
                "review the handler (#341)",
                file=sys.stderr,
            )
        for row in gate["stale"]:
            print(
                f"hle_manifest: stale PROFILE_ZERO_UNREVIEWED_IMPORTS entry {row['nid']} "
                f"{row['name']}: {row['reason']}; remove it (#341)",
                file=sys.stderr,
            )
        print(
            f"hle_manifest: profile-zero import gate: {gate['imported']} imported NIDs, "
            f"{len(gate['imported_unreviewed'])} routed to unreviewed handlers (#341), "
            f"{len(gate['unallowed'])} not allowlisted, {len(gate['stale'])} stale allowlist entries"
        )
        if gate["unallowed"] or gate["stale"]:
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
