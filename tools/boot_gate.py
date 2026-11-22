#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Summarize machine-readable native boot milestones from a runtime log.

Exit 0 means the required boot phases occurred in causal order, no disqualifying
fault was observed, and frame content was validated as nonblank (see below). Exit 1 means
the run started but did not meet that evidence contract. Exit 2 means the log is
absent/invalid. ``--allow-present-only`` explicitly lowers only the frame-content
requirement for liveness diagnostics; the JSON output still labels that evidence
as ``present-submitted`` rather than visual success.

Frame content rule. The gate judges content over the run, not only the first frame, because
many games start on a black frame. Content is validated when either (a) the ``first_frame``
event counts at least one nonzero pixel, or (b) a ``phase=first_content`` event exists with
``brightest`` above the dark threshold (``PICTURE_DARK_MAX``, 8, the value of
``SR_PICTURE_DARK_MAX`` in src/rt/fbcap.h; a test pins the two together). The runtime emits
``first_content`` once per run, when the first picture probe (every 300 vblanks while the GUI is
on, on the CPU and the GPU presenter alike) reads a frame with a colour byte above that
threshold, with the probe's ``vblank`` and ``brightest``. Neither (a) nor (b), as in an
all-black run or one shorter than the first probe, stays ``frame-content-unvalidated``; a
``first_content`` event without a ``first_frame`` event proves nothing (the milestone order
fails). A ``first_content`` event whose ``brightest`` is missing, unparsable or at or below the
threshold cannot come from a correct runtime: it is a disqualifying fault and never content. ``frameContentSource`` in the JSON
output names which event validated the content.

A ``phase=stalled`` event carrying a known neutral observation is reported in the
``observations`` JSON list, not as a disqualifying reason: ``observation=no_new_flip``
(the no-frame watchdog fires on a stretch of vblanks with no new presented frame, which
a legitimately static scene, e.g. a save-confirmation modal waiting for input, also
produces) and ``observation=dark_picture`` (flips arrive but every probed frame of a
stretch was dark, as in a long fade). Each stalled event is judged on its own: one
without a known observation (the legacy bare ``phase=stalled``) stays disqualifying,
whatever other stalled events the log holds.

A run that ended at a named stop is not fault-free, wherever the stop falls: a
``phase=stop`` event (a semantic boundary or an unsupported import) and the runtime's
unimplemented-NID and unsupported-import stop lines are disqualifying. A
``SEMANTIC_BOUNDARY:`` line alone is not: the runtime also prints it for a modeled PSP
error it returns to the guest, and the run goes on.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys

from evidence_model import milestones_in_order


# The runtime's dark-picture threshold, SR_PICTURE_DARK_MAX in src/rt/fbcap.h (tools/test_boot_gate.py
# pins the two together): a probed frame counts as visible only when a colour byte is above it.
PICTURE_DARK_MAX = 8
EVENT_RE = re.compile(r"\bBOOT_EVENT\s+(.+)$")
PAIR_RE = re.compile(r'([A-Za-z_][A-Za-z0-9_]*)=("[^"]*"|\S+)')
# ``window_ready`` is the historical name for the host-presenter initialization
# milestone. Interactive backends use a real window; the explicit offscreen
# backend uses the same phase with ``backend=offscreen`` and makes no window claim.
EXPECTED = ("image_loaded", "runtime_registered", "window_ready", "guest_start", "display_flip", "first_frame")
# The last two are the runtime's stop at an import with no handler (src/rt/hle.c), after
# which the run ends.
FAULT_MARKERS = ("ERROR:", "MEM OOR", "Unhandled", "HLE: unimplemented nid", "UNSUPPORTED_IMPORT:")
# Neutral phase=stalled observations from the current runtime (src/rt/hle_display.c), and the
# name each is reported under in ``observations``.
NEUTRAL_STALL_OBSERVATIONS = {
    "no_new_flip": "no-frame-observation",
    "dark_picture": "dark-picture-observation",
}
# An Ace Mode run (src/rt/ace.h) says so on its first line of runtime output and in a
# phase=ace_mode boot event. Its pictures are enhanced, so it is never boot evidence, whatever
# else the log shows.
ACE_ON_RE = re.compile(r"(?:^|\s)ACE_MODE: on \(")
# A run with the presentation upscaler on (SR_PRESENT_UPSCALE, #1273) is stamped the same way: an
# "ACE_MODE: present upscale <mode>" line and a phase=present_upscale boot event. Its window is
# enhanced, so it is never boot evidence either.
PRESENT_UPSCALE_RE = re.compile(r"(?:^|\s)ACE_MODE: present upscale (\S+)")


def _parse_nonnegative_int(value: str | None) -> tuple[int, bool]:
    if value is None:
        return 0, True
    try:
        parsed = int(value, 0)
    except (TypeError, ValueError):
        return 0, False
    return (parsed, parsed >= 0) if parsed >= 0 else (0, False)


def _frame_evidence(first_frame: dict[str, str] | None, nonzero_pixels: int) -> str:
    """How much the first_frame event proves: none, content-validated, present-submitted or blank.

    A GPU-sourced frame with no counted pixels was submitted for presentation but its content
    was not validated, which is weaker than ``content-validated`` and is graded separately.
    """
    if first_frame is None:
        return "none"
    if nonzero_pixels > 0:
        return "content-validated"
    if first_frame.get("source", "cpu") == "gpu":
        return "present-submitted"
    return "blank-or-unvalidated"


def _first_content(events: list[dict[str, str]], faults: list[str]) -> tuple[dict[str, str] | None, int]:
    """The first ``first_content`` event and its ``brightest`` value (0 when absent or not content)."""
    event = next((e for e in events if e.get("phase") == "first_content"), None)
    if event is None:
        return None, 0
    brightest, valid = _parse_nonnegative_int(event.get("brightest"))
    if event.get("brightest") is None or not valid:
        faults.append(
            f"BOOT_GATE: invalid first_content brightest={event.get('brightest')!r} "
            f"at line {event.get('line', '?')}"
        )
        return event, 0
    if brightest <= PICTURE_DARK_MAX:
        faults.append(
            f"BOOT_GATE: first_content brightest={brightest} is not above the dark threshold "
            f"{PICTURE_DARK_MAX} at line {event.get('line', '?')}"
        )
        return event, 0
    return event, brightest


def parse_log(path: str, *, allow_present_only: bool = False) -> dict:
    if type(allow_present_only) is not bool:
        raise TypeError("allow_present_only must be a boolean")

    events: list[dict[str, str]] = []
    malformed_paths = 0
    faults: list[str] = []
    ace_mode = False
    present_upscale: str | None = None
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line_number, line in enumerate(handle, 1):
            match = EVENT_RE.search(line)
            if match:
                event = {key: value.strip('"') for key, value in PAIR_RE.findall(match.group(1))}
                event["line"] = str(line_number)
                events.append(event)
            if "Open(host0:" in line and ("Open(host0:)" in line or "�" in line or "\x03" in line):
                malformed_paths += 1
            if any(marker in line for marker in FAULT_MARKERS):
                faults.append(line.strip())
            if ACE_ON_RE.search(line):
                ace_mode = True
            upscale = PRESENT_UPSCALE_RE.search(line)
            if upscale and present_upscale is None:
                present_upscale = upscale.group(1)

    phases = [event.get("phase", "unknown") for event in events]
    # The structured event says it as well, for a run whose stderr was silenced.
    ace_mode = ace_mode or "ace_mode" in phases
    if present_upscale is None:
        upscale_event = next((event for event in events if event.get("phase") == "present_upscale"), None)
        if upscale_event is not None:
            present_upscale = upscale_event.get("mode") or "unknown"
    reached = {phase: phase in phases for phase in EXPECTED}
    sequence_ok = milestones_in_order(phases, EXPECTED)

    first_frame = next((event for event in events if event.get("phase") == "first_frame"), None)
    frame_source = first_frame.get("source", "cpu") if first_frame else None
    nonzero_pixels, nonzero_valid = _parse_nonnegative_int(
        first_frame.get("nonzero_pixels") if first_frame else None
    )
    if first_frame and not nonzero_valid:
        faults.append(
            f"BOOT_GATE: invalid nonzero_pixels={first_frame.get('nonzero_pixels')!r} "
            f"at line {first_frame.get('line', '?')}"
        )

    frame_evidence = _frame_evidence(first_frame, nonzero_pixels)
    content_event, content_brightest = _first_content(events, faults)
    frame_content_source: str | None = None
    if frame_evidence == "content-validated":
        frame_content_source = "first_frame"
    elif first_frame is not None and content_brightest > PICTURE_DARK_MAX:
        # The first frame was black (or, on the GPU path, uncounted), but a later probe read
        # a visible frame: the run did render content.
        frame_evidence = "content-validated"
        frame_content_source = "first_content"
    frame_ok = frame_evidence == "content-validated" or (
        allow_present_only and frame_evidence == "present-submitted"
    )
    stalled = "stalled" in phases

    # A phase=stalled event from the current runtime carries a neutral observation
    # (no_new_flip, dark_picture): a static scene or a long fade produces them too, so they
    # are reported, not a hang verdict. Each event is judged on its own: one without a
    # known observation (old runtime logs) stays disqualifying beside neutral ones.
    observations: list[str] = []
    unexplained_stall = False
    for event in events:
        if event.get("phase") != "stalled":
            continue
        name = NEUTRAL_STALL_OBSERVATIONS.get(event.get("observation", ""))
        if name is None:
            unexplained_stall = True
        elif name not in observations:
            observations.append(name)

    stops = [
        f"{event.get('reason', 'unknown')}:{event.get('boundary', 'unknown')} at line {event.get('line', '?')}"
        for event in events if event.get("phase") == "stop"
    ]

    reasons: list[str] = []
    if not sequence_ok:
        reasons.append("milestones-incomplete-or-out-of-order")
    if unexplained_stall:
        reasons.append("stalled")
    if stops:
        reasons.append("semantic-stop")
    if faults:
        reasons.append("disqualifying-fault")
    if malformed_paths:
        reasons.append("malformed-host-path")
    if not frame_ok:
        reasons.append("frame-content-unvalidated")
    if ace_mode:
        reasons.append("ace-mode-run")
    if present_upscale is not None:
        reasons.append("present-upscale-run")

    ok = not reasons
    last_phase = phases[-1] if phases else "not-started"
    return {
        "ok": ok,
        "stalled": stalled,
        "observations": observations,
        "stops": stops,
        "lastPhase": last_phase,
        "reached": reached,
        "sequenceOk": sequence_ok,
        "requiredSequence": list(EXPECTED),
        "frameSource": frame_source,
        "frameEvidence": frame_evidence,
        "requiredFrameEvidence": "present-submitted" if allow_present_only else "content-validated",
        "nonzeroPixels": nonzero_pixels,
        "frameContentSource": frame_content_source,
        "firstContentVblank": _parse_nonnegative_int(content_event.get("vblank"))[0] if content_event else None,
        "firstContentBrightest": content_brightest if content_event else None,
        "malformedHostPaths": malformed_paths,
        "faultCount": len(faults),
        "faults": faults[-20:],
        "aceMode": ace_mode,
        "presentUpscale": present_upscale,
        "disqualifyingReasons": reasons,
        "events": events,
        "logPath": os.path.abspath(path),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", nargs="?", default=os.path.join("logs", "stderr_run.log"))
    parser.add_argument("--json", action="store_true", help="emit one JSON object")
    parser.add_argument(
        "--allow-present-only",
        action="store_true",
        help="accept a causally ordered fault-free GPU present as liveness evidence, not visual success",
    )
    args = parser.parse_args(argv)
    if not os.path.isfile(args.log):
        result = {"ok": False, "error": "log-not-found", "logPath": os.path.abspath(args.log)}
        print(json.dumps(result) if args.json else f"BOOT GATE: log not found: {result['logPath']}")
        return 2
    result = parse_log(args.log, allow_present_only=args.allow_present_only)
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        mark = "PASS" if result["ok"] else "FAIL"
        print(
            f"BOOT GATE: {mark} "
            f"(last={result['lastPhase']}, frame={result['frameEvidence']}, "
            f"nonzero={result['nonzeroPixels']}, malformed_paths={result['malformedHostPaths']}, "
            f"faults={result['faultCount']})"
        )
        for phase, reached in result["reached"].items():
            print(f"  {'[x]' if reached else '[ ]'} {phase}")
        for reason in result["disqualifyingReasons"]:
            print(f"  [!] {reason}")
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
