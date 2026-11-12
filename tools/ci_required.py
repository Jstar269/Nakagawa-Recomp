# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Evaluate the stable aggregate status for the path-gated CI workflow."""

from __future__ import annotations

import os
import sys
from typing import Mapping, Sequence


# The path-gated jobs. The Markdown lint and the main smoke are steps of the
# always-required hygiene job, and the profile-zero fixture build is a step of the
# always-required public-export job (.github/workflows/ci.yml), so their results
# reach this aggregate through those two jobs.
_GATES = (
    ("python-tools", "PYTHON_RESULT", "RUN_PYTHON"),
    ("native-tools", "NATIVE_RESULT", "RUN_NATIVE"),
    # #360: the Android launcher build runs whenever the native gates do.
    ("android-build", "ANDROID_RESULT", "RUN_NATIVE"),
    ("windows-runtime", "WINDOWS_RESULT", "RUN_WINDOWS"),
    ("windows-installer", "INSTALLER_RESULT", "RUN_INSTALLER"),
)

# The run flags of the steps folded into hygiene. Their results arrive through
# HYGIENE_RESULT, but each flag must still be an explicit boolean: a missing or
# misnamed classifier output would otherwise skip the Markdown lint or the main
# smoke step silently while hygiene stays green.
_FOLDED_STEP_FLAGS = ("RUN_MARKDOWN", "RUN_MAIN_SMOKE")

_SUBSTANTIVE_GATES = frozenset(
    {"python-tools", "native-tools", "android-build", "windows-runtime", "windows-installer"}
)

#: The reason a Draft pull request's run is never green. A Draft runs only the quick
#: jobs (tools/ci_paths.py sets ``allow_substantive`` false for it); the full checks run
#: when it is marked Ready. tools/ci_trust_verdict.py reports the same code from the pull
#: request's own draft state (docs/CI.md, "Draft pull requests").
DRAFT_LIGHT_CI = "DRAFT_LIGHT_CI"

#: The jobs a Draft's run still executes, as named in this aggregate's results.
_QUICK_JOBS = (
    ("classify", "CLASSIFY_RESULT"),
    ("hygiene", "HYGIENE_RESULT"),
    ("public-export", "PUBLIC_EXPORT_RESULT"),
)

#: The two CI tiers, as tools/ci_paths.py exports them in ``tier`` (docs/CI.md, "Quick and
#: full checks"). ``light`` is what the documentation calls the quick checks. A light run
#: is never green here: it suppressed the substantive gates, so it cannot stand for the
#: head. The ``CI quick checks`` job (``--quick``) reports a light run instead.
TIER_LIGHT = "light"
TIER_FULL = "full"
#: The reason ``CI required`` refuses a light run, should the workflow ever start it there.
LIGHT_CI_ONLY = "LIGHT_CI_ONLY"
#: The one event whose run may count a suppressed substantive gate as passed: a push to
#: ``main``, after the merged pull request (or batch) already ran those gates.
_SUPPRESSION_EVENT = "push"

#: The jobs the quick aggregate requires, as (name, result variable).
_QUICK_AGGREGATE_JOBS = _QUICK_JOBS + (("quick-checks", "QUICK_CHECKS_RESULT"),)


def _parse_boolean(value: str | None) -> bool | None:
    """Parse the exact boolean contract emitted through GitHub job outputs."""

    if value is None:
        return None
    normalised = value.strip().lower()
    if normalised == "true":
        return True
    if normalised == "false":
        return False
    return None


def required_gate_passes(
    results: Mapping[str, str],
    applicable: Mapping[str, bool],
    *,
    event_name: str,
    allow_substantive: bool = True,
    draft: bool = False,
    tier: str = TIER_FULL,
) -> bool:
    """Return whether every required or applicable job completed successfully.

    ``skipped`` is accepted only for a gate that is not applicable. A failed,
    cancelled, or otherwise incomplete applicable gate fails the aggregate, and
    classifier, hygiene, or public-export failures can never be hidden by path
    gating. The public-export job always runs, so it must report success.

    A Draft pull request's run never passes (``DRAFT_LIGHT_CI``), whatever its
    jobs did: it ran only the quick jobs, so it cannot stand for the head.

    A run that suppressed the substantive gates passes only as a push
    (``event_name`` is GitHub's own ``github.event_name``): that is the main-push
    policy, which skips them after the merged pull request has already run them.
    On every other event a suppressed gate was never run for this change, so the
    aggregate fails, and so does any run whose ``tier`` is not ``full``. This is
    what keeps a light run from ever turning ``CI required`` green, whatever the
    workflow's own conditions do (docs/CI.md, "Quick and full checks").
    """

    if (
        results.get("classify") != "success"
        or results.get("hygiene") != "success"
        or results.get("public-export") != "success"
    ):
        return False

    if draft:
        return False
    if tier != TIER_FULL:
        return False
    if not allow_substantive and event_name != _SUPPRESSION_EVENT:
        return False

    for name, _result_key, _run_key in _GATES:
        should_run = applicable.get(name, False)
        if not allow_substantive and name in _SUBSTANTIVE_GATES:
            should_run = False
        if should_run and results.get(name) != "success":
            return False
    return True


def evaluate_environment(environment: Mapping[str, str]) -> bool:
    results = {
        "classify": environment.get("CLASSIFY_RESULT", ""),
        "hygiene": environment.get("HYGIENE_RESULT", ""),
        "public-export": environment.get("PUBLIC_EXPORT_RESULT", ""),
    }
    for run_key in _FOLDED_STEP_FLAGS:
        if _parse_boolean(environment.get(run_key)) is None:
            return False
    applicable: dict[str, bool] = {}
    for name, result_key, run_key in _GATES:
        results[name] = environment.get(result_key, "")
        parsed = _parse_boolean(environment.get(run_key))
        if parsed is None:
            return False
        applicable[name] = parsed
    allow_substantive = _parse_boolean(environment.get("ALLOW_SUBSTANTIVE"))
    if allow_substantive is None:
        return False
    draft = _parse_boolean(environment.get("DRAFT"))
    if draft is None:
        return False
    # Both must be stated: a missing event name or tier is a broken control state, not
    # leave to treat the run as a push or as a full one.
    event_name = (environment.get("EVENT_NAME") or "").strip()
    tier = (environment.get("TIER") or "").strip()
    if not event_name or tier not in (TIER_LIGHT, TIER_FULL):
        return False
    return required_gate_passes(
        results,
        applicable,
        event_name=event_name,
        allow_substantive=allow_substantive,
        draft=draft,
        tier=tier,
    )


def draft_light_run(environment: Mapping[str, str]) -> bool:
    """Whether the run belongs to a Draft pull request (the classifier's ``draft`` output)."""

    return _parse_boolean(environment.get("DRAFT")) is True


def light_run(environment: Mapping[str, str]) -> bool:
    """Whether the classifier called this run light (its ``tier`` output)."""

    return (environment.get("TIER") or "").strip() == TIER_LIGHT


def quick_checks_pass(environment: Mapping[str, str]) -> bool:
    """Whether a light run's quick jobs all succeeded (the ``CI quick checks`` aggregate).

    Only a light run has this aggregate: on any other tier, or with the tier unstated, it
    fails, so it can never be read as a second way to pass a full run. Passing says the
    quick checks are green and nothing more: ``CI required`` does not run on a light run,
    and the Trusted CI verdict never reports ``success`` for one.
    """

    if not light_run(environment):
        return False
    if _parse_boolean(environment.get("DRAFT")) is None:
        return False
    return all(environment.get(key) == "success" for _name, key in _QUICK_AGGREGATE_JOBS)


def quick_main() -> int:
    results = ", ".join(f"{name} {os.environ.get(key) or 'missing'}" for name, key in _QUICK_AGGREGATE_JOBS)
    if not quick_checks_pass(os.environ):
        print(
            f"A quick check failed, was cancelled or did not run, or this is not a light run ({results}; "
            f"tier {os.environ.get('TIER') or 'missing'}).",
            file=sys.stderr,
        )
        return 1
    if draft_light_run(os.environ):
        after = "This is a Draft: mark it Ready for review when it is finished. A Draft is never batched for landing."
    else:
        after = (
            "The full checks (Python, native, Windows, Android) run when this pull request is batched for "
            "landing, or on this pull request when a maintainer adds the ci-full-matrix label and re-runs CI."
        )
    lines = [
        f"The quick checks passed ({results}).",
        "CI required did not run, and the Trusted CI verdict stays pending (LIGHT_CI_GREEN): that is the normal "
        "state of a healthy pull request on a branch of this repository, not a failure.",
        after,
        'See docs/CI.md, "Quick and full checks".',
    ]
    print("\n".join(lines))
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8", newline="\n") as handle:
            handle.write("### CI quick checks: passed\n\n" + "\n\n".join(lines) + "\n")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    arguments = sys.argv[1:] if argv is None else list(argv)
    if arguments == ["--quick"]:
        return quick_main()
    if arguments:
        print(f"unknown argument(s): {' '.join(arguments)}", file=sys.stderr)
        return 2
    if evaluate_environment(os.environ):
        print("All applicable CI gates passed; skipped jobs were intentionally irrelevant or policy-suppressed.")
        return 0
    if draft_light_run(os.environ):
        quick = ", ".join(f"{name} {os.environ.get(key) or 'missing'}" for name, key in _QUICK_JOBS)
        print(
            f"::error title={DRAFT_LIGHT_CI}::Draft pull request: only the quick checks ran. "
            "Mark it Ready for review to run the full checks."
        )
        print(
            f"{DRAFT_LIGHT_CI}: this head belongs to a Draft pull request, so CI ran only the quick checks "
            f"({quick}) and skipped the Python, native and Windows gates. A Draft's run is never green. "
            "Mark the pull request Ready for review to run the full checks (docs/CI.md, \"Draft pull requests\").",
            file=sys.stderr,
        )
        return 1
    if light_run(os.environ):
        print(f"::error title={LIGHT_CI_ONLY}::Only the quick checks ran. A light run is never green for landing.")
        print(
            f"{LIGHT_CI_ONLY}: this run's tier is light, so the Python, native and Windows gates were skipped "
            "and CI required cannot pass. The workflow does not start this job on a light run; the CI quick "
            'checks job reports it (docs/CI.md, "Quick and full checks").',
            file=sys.stderr,
        )
        return 1
    print(
        "An applicable CI gate failed, was cancelled, was suppressed by policy, or the classifier, hygiene, or public-export gate did not pass.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
