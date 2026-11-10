# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Regression tests for the stable CI aggregate status."""

from __future__ import annotations

import contextlib
import io
import itertools
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

try:
    import ci_required
    from ci_required import evaluate_environment, required_gate_passes
except ModuleNotFoundError:  # Running as ``python -m unittest tools.test_ci_required``.
    from tools import ci_required
    from tools.ci_required import evaluate_environment, required_gate_passes


def _results(**overrides: str) -> dict[str, str]:
    values = {
        "classify": "success",
        "hygiene": "success",
        "public-export": "success",
        "python-tools": "skipped",
        "native-tools": "skipped",
        "android-build": "skipped",
        "windows-runtime": "skipped",
        "windows-installer": "skipped",
    }
    values.update(overrides)
    return values


class CiRequiredTests(unittest.TestCase):
    def test_irrelevant_skips_pass(self) -> None:
        self.assertTrue(required_gate_passes(_results(), {}, allow_substantive=True, event_name="pull_request"))

    def test_applicable_success_passes(self) -> None:
        results = _results(**{"python-tools": "success", "native-tools": "success"})
        self.assertTrue(
            required_gate_passes(
                results,
                {"python-tools": True, "native-tools": True},
                allow_substantive=True,
                event_name="pull_request",
            )
        )

    def test_applicable_failure_or_cancel_fails(self) -> None:
        for outcome in ("failure", "cancelled", "skipped"):
            with self.subTest(outcome=outcome):
                results = _results(**{"python-tools": outcome})
                self.assertFalse(required_gate_passes(results, {"python-tools": True}, allow_substantive=True, event_name="pull_request"))

    def test_applicable_windows_installer_failure_or_skip_fails(self) -> None:
        for outcome in ("failure", "cancelled", "skipped"):
            with self.subTest(outcome=outcome):
                results = _results(**{"windows-installer": outcome})
                self.assertFalse(
                    required_gate_passes(results, {"windows-installer": True}, allow_substantive=True, event_name="pull_request")
                )

    def test_applicable_native_player_gate_failure_or_skip_fails(self) -> None:
        for outcome in ("failure", "cancelled", "skipped"):
            with self.subTest(outcome=outcome):
                results = _results(**{"native-tools": outcome})
                self.assertFalse(
                    required_gate_passes(results, {"native-tools": True}, allow_substantive=True, event_name="pull_request")
                )

    def test_android_build_failure_fails_whenever_the_native_gates_apply(self) -> None:
        """#360: the Android launcher build shares RUN_NATIVE, so a broken APK build fails
        CI required even when every native gate passed."""
        base = {
            "CLASSIFY_RESULT": "success",
            "HYGIENE_RESULT": "success",
            "PUBLIC_EXPORT_RESULT": "success",
            "MARKDOWN_RESULT": "skipped",
            "PYTHON_RESULT": "skipped",
            "NATIVE_RESULT": "success",
            "WINDOWS_RESULT": "success",
            "INSTALLER_RESULT": "success",
            "MAIN_SMOKE_RESULT": "skipped",
            "RUN_MARKDOWN": "false",
            "RUN_PYTHON": "false",
            "RUN_NATIVE": "true",
            "RUN_WINDOWS": "true",
            "RUN_INSTALLER": "false",
            "RUN_MAIN_SMOKE": "false",
            "ALLOW_SUBSTANTIVE": "true",
            "DRAFT": "false",
            "EVENT_NAME": "pull_request",
            "TIER": "full",
        }
        self.assertTrue(evaluate_environment(dict(base, ANDROID_RESULT="success")))
        for outcome in ("failure", "cancelled", "skipped", ""):
            with self.subTest(outcome=outcome):
                self.assertFalse(evaluate_environment(dict(base, ANDROID_RESULT=outcome)))
        # When the native gates do not apply, a skipped Android build is expected.
        quiet = dict(base, NATIVE_RESULT="skipped", WINDOWS_RESULT="skipped",
                     RUN_NATIVE="false", RUN_WINDOWS="false", ANDROID_RESULT="skipped")
        self.assertTrue(evaluate_environment(quiet))

    def test_classifier_and_hygiene_failures_cannot_be_hidden(self) -> None:
        for name in ("classify", "hygiene", "public-export"):
            with self.subTest(name=name):
                self.assertFalse(required_gate_passes(_results(**{name: "failure"}), {}, event_name="pull_request"))

    def test_draft_substantive_skips_fail_closed(self) -> None:
        self.assertFalse(
            required_gate_passes(
                _results(),
                {"python-tools": True, "native-tools": True, "windows-runtime": True},
                allow_substantive=False,
                draft=True,
                event_name="pull_request",
            )
        )

    def test_main_push_substantive_suppression_without_draft_remains_allowed(self) -> None:
        self.assertTrue(
            required_gate_passes(
                _results(),
                {"python-tools": True, "native-tools": True},
                allow_substantive=False,
                draft=False,
                event_name="push",
            )
        )

    def test_draft_to_ready_transition_requires_a_new_green_run(self) -> None:
        draft_env = {
            "CLASSIFY_RESULT": "success",
            "HYGIENE_RESULT": "success",
            "PUBLIC_EXPORT_RESULT": "success",
            "PYTHON_RESULT": "skipped",
            "NATIVE_RESULT": "skipped",
            "ANDROID_RESULT": "skipped",
            "WINDOWS_RESULT": "skipped",
            "INSTALLER_RESULT": "skipped",
            "RUN_MARKDOWN": "false",
            "RUN_MAIN_SMOKE": "false",
            "RUN_PYTHON": "true",
            "RUN_NATIVE": "true",
            "RUN_WINDOWS": "true",
            "RUN_INSTALLER": "true",
            "ALLOW_SUBSTANTIVE": "false",
            "DRAFT": "true",
            "EVENT_NAME": "pull_request",
            "TIER": "light",
        }
        self.assertFalse(evaluate_environment(draft_env))

        ready_env = dict(draft_env)
        ready_env.update(
            {
                "PYTHON_RESULT": "success",
                "NATIVE_RESULT": "success",
                "ANDROID_RESULT": "success",
                "WINDOWS_RESULT": "success",
                "INSTALLER_RESULT": "success",
                "ALLOW_SUBSTANTIVE": "true",
                "DRAFT": "false",
                "EVENT_NAME": "pull_request",
                "TIER": "full",
            }
        )
        self.assertTrue(evaluate_environment(ready_env))

    def test_workflow_triggers_on_new_heads_and_on_draft_transitions(self) -> None:
        """CI starts for a new head and for each Draft/Ready transition.

        A Draft runs only the quick jobs, so marking it Ready must start the full
        matrix on the same head (ready_for_review), and converting it back to a Draft
        must start a light run whose shared concurrency group cancels a full run in
        progress (converted_to_draft). Every head-changing event stays listed, so
        nothing that changes code can go unvalidated.
        """
        workflow = (Path(__file__).resolve().parents[1] / ".github" / "workflows" / "ci.yml").read_text(
            encoding="utf-8"
        )
        # Match the declared list, not the file text: the rationale comment
        # above it names the events that must NOT trigger.
        match = re.search(
            r"^\s*pull_request:\s*\n\s*types:\s*\[([^\]]*)\]", workflow, re.MULTILINE
        )
        self.assertIsNotNone(match, "ci.yml must list the pull_request event types explicitly")
        assert match is not None
        declared = {item.strip() for item in match.group(1).split(",")}
        self.assertEqual(
            declared, {"opened", "synchronize", "reopened", "ready_for_review", "converted_to_draft"}
        )
        self.assertIn("cancel-in-progress: true", workflow)
        self.assertIn("DRAFT: ${{ needs.classify.outputs.draft }}", workflow)

    def test_main_smoke_remains_required_when_substantive_jobs_are_suppressed(self) -> None:
        # The main smoke is a step of the hygiene job, so its failure is hygiene's.
        results = _results(hygiene="failure")
        self.assertFalse(
            required_gate_passes(
                results,
                {"python-tools": True},
                allow_substantive=False,
                event_name="push",
            )
        )

    def test_folded_jobs_are_no_separate_inputs(self) -> None:
        """Markdown, main smoke and the profile-zero fixture are steps of required jobs.

        They reach the aggregate through hygiene and public-export, which must succeed on
        every event; the workflow no longer passes a result or run flag of their own.
        """
        workflow = (Path(__file__).resolve().parents[1] / ".github" / "workflows" / "ci.yml").read_text(
            encoding="utf-8"
        )
        for variable in ("MARKDOWN_RESULT", "MAIN_SMOKE_RESULT"):
            with self.subTest(variable=variable):
                self.assertNotIn(variable, workflow)
        # Their run flags are still passed, so a missing classifier output fails closed.
        self.assertIn("RUN_MARKDOWN: ${{ needs.classify.outputs.run_markdown }}", workflow)
        self.assertIn("RUN_MAIN_SMOKE: ${{ needs.classify.outputs.run_main_smoke }}", workflow)
        for name in ("markdown", "main-smoke"):
            with self.subTest(name=name):
                self.assertFalse(required_gate_passes(_results(hygiene="failure", **{name: "success"}), {}, event_name="pull_request"))

    def test_environment_mapping_matches_workflow_contract(self) -> None:
        env = {
            "CLASSIFY_RESULT": "success",
            "HYGIENE_RESULT": "success",
            "PUBLIC_EXPORT_RESULT": "success",
            "PYTHON_RESULT": "success",
            "NATIVE_RESULT": "skipped",
            "ANDROID_RESULT": "skipped",
            "WINDOWS_RESULT": "skipped",
            "INSTALLER_RESULT": "skipped",
            "RUN_MARKDOWN": "false",
            "RUN_MAIN_SMOKE": "false",
            "RUN_PYTHON": "true",
            "RUN_NATIVE": "false",
            "RUN_WINDOWS": "false",
            "RUN_INSTALLER": "false",
            "ALLOW_SUBSTANTIVE": "true",
            "DRAFT": "false",
            "EVENT_NAME": "pull_request",
            "TIER": "full",
        }
        self.assertTrue(evaluate_environment(env))
        env["PYTHON_RESULT"] = "cancelled"
        self.assertFalse(evaluate_environment(env))


def _draft_env(**overrides: str) -> dict[str, str]:
    """A Draft pull request's light run: the quick jobs passed, the substantive ones skipped."""

    env = {
        "CLASSIFY_RESULT": "success",
        "HYGIENE_RESULT": "success",
        "PUBLIC_EXPORT_RESULT": "success",
        "PYTHON_RESULT": "skipped",
        "NATIVE_RESULT": "skipped",
        "WINDOWS_RESULT": "skipped",
        "INSTALLER_RESULT": "skipped",
        "RUN_MARKDOWN": "true",
        "RUN_MAIN_SMOKE": "false",
        "RUN_PYTHON": "false",
        "RUN_NATIVE": "false",
        "RUN_WINDOWS": "false",
        "RUN_INSTALLER": "false",
        "ALLOW_SUBSTANTIVE": "false",
        "DRAFT": "true",
        "EVENT_NAME": "pull_request",
        "TIER": "light",
    }
    env.update(overrides)
    return env


class DraftLightRunTests(unittest.TestCase):
    """A Draft pull request's run is never green, and says why (DRAFT_LIGHT_CI)."""

    def test_a_docs_only_draft_with_every_quick_job_green_is_not_green(self) -> None:
        # No substantive gate applies, so only the Draft rule can turn this red.
        self.assertFalse(evaluate_environment(_draft_env()))
        self.assertFalse(required_gate_passes(_results(), {}, allow_substantive=False, draft=True, event_name="pull_request"))

    def test_a_draft_is_not_green_even_if_every_substantive_job_passed(self) -> None:
        env = _draft_env(
            PYTHON_RESULT="success", NATIVE_RESULT="success", ANDROID_RESULT="success",
            WINDOWS_RESULT="success", INSTALLER_RESULT="success", RUN_PYTHON="true", RUN_NATIVE="true",
            RUN_WINDOWS="true", RUN_INSTALLER="true", ALLOW_SUBSTANTIVE="true",
        )
        self.assertFalse(evaluate_environment(env))
        results = _results(**{"python-tools": "success", "native-tools": "success", "android-build": "success",
                              "windows-runtime": "success", "windows-installer": "success"})
        applicable = {"python-tools": True, "native-tools": True, "android-build": True, "windows-runtime": True,
                      "windows-installer": True}
        self.assertFalse(required_gate_passes(results, applicable, allow_substantive=True, draft=True, event_name="pull_request"))
        # The same results for a pull request that is not a Draft pass, exactly as before.
        self.assertTrue(required_gate_passes(results, applicable, allow_substantive=True, draft=False, event_name="pull_request"))
        self.assertTrue(evaluate_environment(dict(env, DRAFT="false", TIER="full")))

    def test_main_names_the_reason_and_fails(self) -> None:
        stdout, stderr = io.StringIO(), io.StringIO()
        with (
            mock.patch.dict(os.environ, _draft_env(), clear=True),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            code = ci_required.main([])
        self.assertEqual(code, 1)
        self.assertEqual(ci_required.DRAFT_LIGHT_CI, "DRAFT_LIGHT_CI")
        self.assertIn("::error title=DRAFT_LIGHT_CI::", stdout.getvalue())
        self.assertIn("DRAFT_LIGHT_CI: this head belongs to a Draft pull request", stderr.getvalue())
        self.assertIn("classify success, hygiene success, public-export success", stderr.getvalue())
        self.assertIn("Ready for review", stderr.getvalue())

    def test_a_failed_quick_job_in_a_draft_is_reported_with_its_result(self) -> None:
        stderr = io.StringIO()
        with (
            mock.patch.dict(os.environ, _draft_env(HYGIENE_RESULT="failure"), clear=True),
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(stderr),
        ):
            self.assertEqual(ci_required.main([]), 1)
        self.assertIn("hygiene failure", stderr.getvalue())

    def test_a_non_draft_run_keeps_the_former_messages(self) -> None:
        ready = _draft_env(DRAFT="false", ALLOW_SUBSTANTIVE="true", TIER="full")
        stdout = io.StringIO()
        with mock.patch.dict(os.environ, ready, clear=True), contextlib.redirect_stdout(stdout):
            self.assertEqual(ci_required.main([]), 0)
        self.assertIn("All applicable CI gates passed", stdout.getvalue())
        stderr = io.StringIO()
        failed = dict(ready, RUN_PYTHON="true", PYTHON_RESULT="failure")
        with (
            mock.patch.dict(os.environ, failed, clear=True),
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(stderr),
        ):
            self.assertEqual(ci_required.main([]), 1)
        self.assertNotIn("DRAFT_LIGHT_CI", stderr.getvalue())
        self.assertIn("An applicable CI gate failed", stderr.getvalue())


class AllowSubstantiveParsingTests(unittest.TestCase):
    """Every control output must be an explicit, well-formed boolean.

    A malformed classifier output is a broken control state, not an
    instruction to skip a gate.
    """

    def _env(self, allow_substantive: str | None) -> dict[str, str]:
        env = {
            "CLASSIFY_RESULT": "success",
            "HYGIENE_RESULT": "success",
            "PUBLIC_EXPORT_RESULT": "success",
            "PYTHON_RESULT": "skipped",
            "NATIVE_RESULT": "skipped",
            "ANDROID_RESULT": "skipped",
            "WINDOWS_RESULT": "skipped",
            "INSTALLER_RESULT": "skipped",
            "RUN_MARKDOWN": "false",
            "RUN_MAIN_SMOKE": "false",
            "RUN_PYTHON": "false",
            # Applicable, but the job was skipped -- this must fail unless
            # suppression was explicitly requested.
            "RUN_NATIVE": "true",
            "RUN_WINDOWS": "false",
            "RUN_INSTALLER": "false",
            # This fixture models the intentional main-push suppression, not a
            # draft PR. Draft-specific suppression is tested separately below.
            "DRAFT": "false",
            "EVENT_NAME": "push",
            "TIER": "full",
        }
        if allow_substantive is not None:
            env["ALLOW_SUBSTANTIVE"] = allow_substantive
        return env

    def test_explicit_false_suppresses(self) -> None:
        for value in ("false", "False", "FALSE", " false "):
            with self.subTest(value=value):
                self.assertTrue(evaluate_environment(self._env(value)))

    def test_draft_suppression_is_not_green(self) -> None:
        env = self._env("false")
        env["DRAFT"] = "true"
        self.assertFalse(evaluate_environment(env))

    def test_absent_empty_or_malformed_keeps_gates_required(self) -> None:
        for value in (None, "", "   ", "xyz", "0", "no", "null", "true"):
            with self.subTest(value=value):
                self.assertFalse(
                    evaluate_environment(self._env(value)),
                    f"ALLOW_SUBSTANTIVE={value!r} must not silently suppress an applicable gate",
                )


class ApplicabilityParsingTests(unittest.TestCase):
    def _env(self) -> dict[str, str]:
        return {
            "CLASSIFY_RESULT": "success",
            "HYGIENE_RESULT": "success",
            "PUBLIC_EXPORT_RESULT": "success",
            "PYTHON_RESULT": "success",
            "NATIVE_RESULT": "success",
            "ANDROID_RESULT": "success",
            "WINDOWS_RESULT": "success",
            "INSTALLER_RESULT": "success",
            "RUN_MARKDOWN": "false",
            "RUN_MAIN_SMOKE": "false",
            "RUN_PYTHON": "false",
            "RUN_NATIVE": "false",
            "RUN_WINDOWS": "false",
            "RUN_INSTALLER": "false",
            "ALLOW_SUBSTANTIVE": "true",
            "DRAFT": "false",
            "EVENT_NAME": "pull_request",
            "TIER": "full",
        }

    def test_windows_installer_result_is_required_when_applicable(self) -> None:
        env = self._env()
        env.update(RUN_INSTALLER="true", INSTALLER_RESULT="success")
        self.assertTrue(evaluate_environment(env))
        for outcome in ("failure", "cancelled", "skipped", ""):
            with self.subTest(outcome=outcome):
                env["INSTALLER_RESULT"] = outcome
                self.assertFalse(evaluate_environment(env))

    def test_every_run_variable_requires_a_boolean(self) -> None:
        for run_key in (
            "RUN_MARKDOWN",
            "RUN_PYTHON",
            "RUN_NATIVE",
            "RUN_WINDOWS",
            "RUN_INSTALLER",
            "RUN_MAIN_SMOKE",
        ):
            for value in (None, "", "   ", "garbage", "0", "no"):
                with self.subTest(run_key=run_key, value=value):
                    env = self._env()
                    if value is None:
                        env.pop(run_key)
                    else:
                        env[run_key] = value
                    self.assertFalse(evaluate_environment(env))

    def test_run_variables_accept_case_and_surrounding_whitespace(self) -> None:
        for run_key in (
            "RUN_MARKDOWN",
            "RUN_PYTHON",
            "RUN_NATIVE",
            "RUN_WINDOWS",
            "RUN_INSTALLER",
            "RUN_MAIN_SMOKE",
        ):
            with self.subTest(run_key=run_key):
                env = self._env()
                env[run_key] = " TrUe "
                self.assertTrue(evaluate_environment(env))
                env[run_key] = " FaLsE "
                self.assertTrue(evaluate_environment(env))

    def test_missing_folded_step_flag_fails_closed(self) -> None:
        """Markdown and main smoke run as hygiene steps gated on these flags.

        A dropped or misnamed classifier output would skip the step and leave hygiene
        green, so the aggregate must fail when either flag is absent, even with every
        job successful.
        """
        for run_key in ("RUN_MARKDOWN", "RUN_MAIN_SMOKE"):
            with self.subTest(run_key=run_key):
                env = self._env()
                self.assertTrue(evaluate_environment(env))
                del env[run_key]
                self.assertFalse(evaluate_environment(env))

    def test_malformed_applicability_cannot_be_masked_by_successful_jobs(self) -> None:
        env = self._env()
        env["RUN_NATIVE"] = "unexpected"
        self.assertTrue(all(value == "success" for key, value in env.items() if key.endswith("_RESULT")))
        self.assertFalse(evaluate_environment(env))

    def test_allow_substantive_requires_a_boolean(self) -> None:
        for value in (None, "", "   ", "garbage", "0", "no", "null"):
            with self.subTest(value=value):
                env = self._env()
                if value is None:
                    env.pop("ALLOW_SUBSTANTIVE")
                else:
                    env["ALLOW_SUBSTANTIVE"] = value
                self.assertFalse(evaluate_environment(env))

    def test_draft_requires_a_boolean(self) -> None:
        for value in (None, "", "   ", "garbage", "0", "no", "null"):
            with self.subTest(value=value):
                env = self._env()
                if value is None:
                    env.pop("DRAFT")
                else:
                    env["DRAFT"] = value
                self.assertFalse(evaluate_environment(env))

    def test_public_export_result_is_required_on_every_event(self) -> None:
        """The export audit is its own required result (#735 item 5).

        It runs ungated on every event, so an unset or non-success result is never green,
        even when no substantive gate applies (the docs-only and main-push shapes).
        """
        for value in ("failure", "cancelled", "skipped", ""):
            with self.subTest(value=value):
                env = self._env()
                env["PUBLIC_EXPORT_RESULT"] = value
                self.assertFalse(evaluate_environment(env))
        env = self._env()
        del env["PUBLIC_EXPORT_RESULT"]
        self.assertFalse(evaluate_environment(env))
        env = self._env()
        env["PUBLIC_EXPORT_RESULT"] = "success"
        self.assertTrue(evaluate_environment(env))

    def test_classifier_and_hygiene_non_success_remain_unconditionally_red(self) -> None:
        for key in ("CLASSIFY_RESULT", "HYGIENE_RESULT", "PUBLIC_EXPORT_RESULT"):
            for value in ("failure", "cancelled", "skipped", "", "garbage"):
                with self.subTest(key=key, value=value):
                    env = self._env()
                    env[key] = value
                    self.assertFalse(evaluate_environment(env))


_SUBSTANTIVE = (
    ("PYTHON_RESULT", "RUN_PYTHON"),
    ("NATIVE_RESULT", "RUN_NATIVE"),
    ("WINDOWS_RESULT", "RUN_WINDOWS"),
)


def _tier_env(**overrides: str) -> dict[str, str]:
    """A full pull-request run in which every substantive gate was routed and passed."""

    env = {
        "CLASSIFY_RESULT": "success",
        "HYGIENE_RESULT": "success",
        "PUBLIC_EXPORT_RESULT": "success",
        "PYTHON_RESULT": "success",
        "NATIVE_RESULT": "success",
        "ANDROID_RESULT": "success",
        "WINDOWS_RESULT": "success",
        "INSTALLER_RESULT": "success",
        "RUN_MARKDOWN": "false",
        "RUN_MAIN_SMOKE": "false",
        "RUN_PYTHON": "true",
        "RUN_NATIVE": "true",
        "RUN_WINDOWS": "true",
        "RUN_INSTALLER": "true",
        "ALLOW_SUBSTANTIVE": "true",
        "DRAFT": "false",
        "EVENT_NAME": "pull_request",
        "TIER": "full",
    }
    env.update(overrides)
    return env


class TierTests(unittest.TestCase):
    """A run that suppressed the substantive gates never turns CI required green, except a push to main.

    This is the second of the layers that keep a light run from standing for a head
    (docs/CI.md, "Quick and full checks"): the workflow does not start CI required on a
    light run, and if it did, this aggregate would fail it. The event name is GitHub's
    own, so the rule does not rest on the classifier's ``tier`` alone.
    """

    OUTCOMES = ("success", "skipped", "failure", "cancelled", "")
    NOT_PUSH = ("pull_request", "pull_request_target", "merge_group", "schedule", "workflow_dispatch")

    def test_a_pull_request_run_with_suppressed_gates_is_never_green(self) -> None:
        checked = 0
        for event in self.NOT_PUSH:
            for tier, allow in (("light", "false"), ("light", "true"), ("full", "false")):
                for outcomes in itertools.product(self.OUTCOMES, repeat=len(_SUBSTANTIVE)):
                    for flags in itertools.product(("true", "false"), repeat=len(_SUBSTANTIVE)):
                        env = _tier_env(EVENT_NAME=event, TIER=tier, ALLOW_SUBSTANTIVE=allow)
                        for (result_key, run_key), outcome, flag in zip(_SUBSTANTIVE, outcomes, flags, strict=True):
                            env[result_key] = outcome
                            env[run_key] = flag
                        self.assertFalse(
                            evaluate_environment(env),
                            f"{event} tier={tier} allow_substantive={allow} {outcomes} {flags} must not be green",
                        )
                        checked += 1
        self.assertEqual(checked, len(self.NOT_PUSH) * 3 * 5 ** 3 * 2 ** 3)

    def test_a_docs_only_light_run_is_not_green_either(self) -> None:
        # No substantive gate applies, so only the tier rules can turn this red.
        quiet = {key: "skipped" for key, _run in _SUBSTANTIVE} | {run: "false" for _key, run in _SUBSTANTIVE}
        quiet |= {"ANDROID_RESULT": "skipped", "INSTALLER_RESULT": "skipped", "RUN_INSTALLER": "false"}
        self.assertTrue(evaluate_environment(_tier_env(**quiet)))
        self.assertFalse(evaluate_environment(_tier_env(**quiet, TIER="light", ALLOW_SUBSTANTIVE="false")))
        self.assertFalse(evaluate_environment(_tier_env(**quiet, TIER="light")))
        self.assertFalse(evaluate_environment(_tier_env(**quiet, ALLOW_SUBSTANTIVE="false")))

    def test_only_a_push_may_count_a_suppressed_gate_as_passed(self) -> None:
        skipped = {key: "skipped" for key, _run in _SUBSTANTIVE}
        skipped |= {"ANDROID_RESULT": "skipped", "INSTALLER_RESULT": "skipped"}
        push = _tier_env(**skipped, EVENT_NAME="push", ALLOW_SUBSTANTIVE="false")
        self.assertTrue(evaluate_environment(push), "the main-push policy is unchanged")
        self.assertFalse(evaluate_environment(dict(push, TIER="light")), "no event is green on a light tier")
        for event in self.NOT_PUSH:
            with self.subTest(event=event):
                self.assertFalse(evaluate_environment(dict(push, EVENT_NAME=event)))
        results = _results()
        applicable = {"python-tools": True, "native-tools": True}
        self.assertTrue(required_gate_passes(results, applicable, event_name="push", allow_substantive=False))
        self.assertFalse(
            required_gate_passes(results, applicable, event_name="pull_request", allow_substantive=False)
        )
        self.assertFalse(required_gate_passes(results, {}, event_name="push", allow_substantive=False, tier="light"))

    def test_the_event_name_and_the_tier_must_be_stated(self) -> None:
        self.assertTrue(evaluate_environment(_tier_env()))
        for key in ("EVENT_NAME", "TIER"):
            for value in (None, "", "   "):
                with self.subTest(key=key, value=value):
                    env = _tier_env()
                    if value is None:
                        del env[key]
                    else:
                        env[key] = value
                    self.assertFalse(evaluate_environment(env))
        for value in ("Full", "FULL", "quick", "true", "full,light", "light "):
            with self.subTest(tier=value):
                self.assertFalse(evaluate_environment(_tier_env(TIER=value)))
        with self.assertRaises(TypeError):
            required_gate_passes(_results(), {})  # type: ignore[call-arg]

    def test_a_full_pull_request_run_is_judged_as_before(self) -> None:
        self.assertTrue(evaluate_environment(_tier_env()))
        for result_key, _run_key in _SUBSTANTIVE:
            for outcome in ("skipped", "failure", "cancelled", ""):
                with self.subTest(result=result_key, outcome=outcome):
                    self.assertFalse(evaluate_environment(_tier_env(**{result_key: outcome})))

    def test_main_refuses_a_light_run_and_names_the_reason(self) -> None:
        light = _tier_env(TIER="light", ALLOW_SUBSTANTIVE="false", PYTHON_RESULT="skipped",
                          NATIVE_RESULT="skipped", WINDOWS_RESULT="skipped", ANDROID_RESULT="skipped",
                          INSTALLER_RESULT="skipped")
        stdout, stderr = io.StringIO(), io.StringIO()
        with (
            mock.patch.dict(os.environ, light, clear=True),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            self.assertEqual(ci_required.main([]), 1)
        self.assertEqual(ci_required.LIGHT_CI_ONLY, "LIGHT_CI_ONLY")
        self.assertIn("::error title=LIGHT_CI_ONLY::", stdout.getvalue())
        self.assertIn("CI quick checks", stderr.getvalue())

    def test_an_unknown_argument_is_refused(self) -> None:
        with (
            mock.patch.dict(os.environ, _tier_env(), clear=True),
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            self.assertEqual(ci_required.main(["--quickly"]), 2)
            self.assertEqual(ci_required.main(["--quick", "extra"]), 2)
            self.assertEqual(ci_required.main([]), 0)


def _quick_env(**overrides: str) -> dict[str, str]:
    """A light run whose quick jobs all passed."""

    env = {
        "CLASSIFY_RESULT": "success",
        "HYGIENE_RESULT": "success",
        "PUBLIC_EXPORT_RESULT": "success",
        "QUICK_CHECKS_RESULT": "success",
        "TIER": "light",
        "DRAFT": "false",
    }
    env.update(overrides)
    return env


class QuickAggregateTests(unittest.TestCase):
    """``--quick`` is the aggregate of a light run (the CI quick checks job), and of nothing else."""

    RESULTS = ("CLASSIFY_RESULT", "HYGIENE_RESULT", "PUBLIC_EXPORT_RESULT", "QUICK_CHECKS_RESULT")

    def test_green_only_when_every_quick_job_succeeded(self) -> None:
        self.assertTrue(ci_required.quick_checks_pass(_quick_env()))
        self.assertTrue(ci_required.quick_checks_pass(_quick_env(DRAFT="true")))
        for key in self.RESULTS:
            for value in ("failure", "cancelled", "skipped", "", "garbage"):
                with self.subTest(key=key, value=value):
                    self.assertFalse(ci_required.quick_checks_pass(_quick_env(**{key: value})))
            env = _quick_env()
            del env[key]
            self.assertFalse(ci_required.quick_checks_pass(env))

    def test_never_green_on_a_full_or_unstated_tier(self) -> None:
        for value in ("full", "", "Light", "quick"):
            with self.subTest(tier=value):
                self.assertFalse(ci_required.quick_checks_pass(_quick_env(TIER=value)))
        env = _quick_env()
        del env["TIER"]
        self.assertFalse(ci_required.quick_checks_pass(env))
        for value in ("", "garbage"):
            self.assertFalse(ci_required.quick_checks_pass(_quick_env(DRAFT=value)))

    def test_a_green_quick_run_is_not_a_green_ci_required(self) -> None:
        """The two aggregates never agree on a light run: this one passes, CI required fails."""
        light = _tier_env(TIER="light", ALLOW_SUBSTANTIVE="false", QUICK_CHECKS_RESULT="success")
        self.assertTrue(ci_required.quick_checks_pass(light))
        self.assertFalse(evaluate_environment(light))
        full = _tier_env(QUICK_CHECKS_RESULT="skipped")
        self.assertFalse(ci_required.quick_checks_pass(full))
        self.assertTrue(evaluate_environment(full))

    def _quick_main(self, env: dict[str, str]) -> tuple[int, str, str]:
        stdout, stderr = io.StringIO(), io.StringIO()
        with (
            mock.patch.dict(os.environ, env, clear=True),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            code = ci_required.main(["--quick"])
        return code, stdout.getvalue(), stderr.getvalue()

    def test_main_says_what_happens_next(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            summary = Path(scratch) / "summary.md"
            code, stdout, _stderr = self._quick_main(_quick_env(GITHUB_STEP_SUMMARY=str(summary)))
            self.assertEqual(code, 0)
            for text in ("The quick checks passed", "LIGHT_CI_GREEN", "batched for landing", "ci-full-matrix",
                         "not a failure"):
                self.assertIn(text, stdout)
            self.assertIn("### CI quick checks: passed", summary.read_text(encoding="utf-8"))
        code, stdout, _stderr = self._quick_main(_quick_env(DRAFT="true"))
        self.assertEqual(code, 0)
        self.assertIn("This is a Draft", stdout)
        self.assertNotIn("ci-full-matrix", stdout)

    def test_main_names_the_failed_quick_job(self) -> None:
        code, _stdout, stderr = self._quick_main(_quick_env(QUICK_CHECKS_RESULT="failure"))
        self.assertEqual(code, 1)
        self.assertIn("quick-checks failure", stderr)
        code, _stdout, stderr = self._quick_main(_quick_env(TIER="full"))
        self.assertEqual(code, 1)
        self.assertIn("tier full", stderr)


class ProcessExitTests(unittest.TestCase):
    """The script's exit status is the job's result: run it as the workflow step does.

    The other tests call the functions in-process, so they would still pass if the script
    stopped passing ``main()``'s value to ``sys.exit``. Then every aggregate step would exit 0,
    and a run with a failed gate would report CI required green.
    """

    SCRIPT = Path(__file__).resolve().with_name("ci_required.py")

    def _exit_status(self, env: dict[str, str], *args: str) -> int:
        inherited = {key: os.environ[key] for key in ("PATH", "SYSTEMROOT") if key in os.environ}
        completed = subprocess.run(
            [sys.executable, str(self.SCRIPT), *args],
            env={**inherited, **env, "PYTHONIOENCODING": "utf-8"},
            capture_output=True,
            check=False,
            timeout=60,
        )
        return completed.returncode

    def test_the_full_aggregate_exits_non_zero_unless_the_run_is_green(self) -> None:
        self.assertEqual(self._exit_status(_tier_env()), 0)
        for name, env in (
            ("a failed gate", _tier_env(NATIVE_RESULT="failure")),
            ("a light run", _tier_env(TIER="light", ALLOW_SUBSTANTIVE="false")),
            ("a suppressed gate on a pull request", _tier_env(ALLOW_SUBSTANTIVE="false")),
            ("a Draft", _tier_env(DRAFT="true")),
        ):
            with self.subTest(name=name):
                self.assertEqual(self._exit_status(env), 1)

    def test_the_quick_aggregate_exits_non_zero_unless_the_quick_run_is_green(self) -> None:
        self.assertEqual(self._exit_status(_quick_env(), "--quick"), 0)
        for name, env in (
            ("a failed quick job", _quick_env(QUICK_CHECKS_RESULT="failure")),
            ("a full run", _quick_env(TIER="full")),
        ):
            with self.subTest(name=name):
                self.assertEqual(self._exit_status(env, "--quick"), 1)
        self.assertEqual(self._exit_status(_quick_env(), "--unknown"), 2)


if __name__ == "__main__":
    unittest.main()
