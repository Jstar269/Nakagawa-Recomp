# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Host-side tests for the one-command PSP oracle matrix (tools/psp_oracle/oracle_matrix.py, issue #352).

A fake device stands in for the build shell, the PSPLink soft reset, the resident agent's recover and probe
commands and the hardware lock, so every decision is exercised without a PSP: one launch per fresh boot,
identity-bound acceptance, stale-result rejection, crash containment with a bounded budget, and an explicit
intervention state for a wedge.
"""

from __future__ import annotations

from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import time
import unittest
from typing import Any, cast
from unittest.mock import patch

_TOOLS_DIR = str(Path(__file__).resolve().parent)
if _TOOLS_DIR not in sys.path:
    sys.path.insert(0, _TOOLS_DIR)

from psp_oracle import oracle_matrix as om  # noqa: E402

COMMIT = "1234567890abcdef1234567890abcdef12345678"
SESSION = "matrix-test"


def _utc(moment: datetime) -> str:
    return moment.strftime("%Y-%m-%dT%H:%M:%SZ")


class FakeDevice:
    """The PSP, its resident agent and the build shell, driven by argv markers."""

    def __init__(self, root: Path, lock_path: Path, behaviours: dict[str, str]) -> None:
        self.root = root
        self.lock_path = lock_path
        self.behaviours = behaviours
        self.boot = 70
        self.pending_reset = False
        self.reset_works = True
        self.recover_exit = 0
        self.recover_failures_after_reset = False
        self.calls: list[str] = []
        self.launched: list[tuple[str, int]] = []
        self.attest()

    def attest(self) -> None:
        lock = json.loads(self.lock_path.read_text(encoding="utf-8"))
        lock["device_state_attestation"] = {
            "schema": 1,
            "boot_number": self.boot,
            "qualification_result": "QUALIFIED",
            "qualified_at_utc": _utc(datetime.now(timezone.utc)),
            "session_id": SESSION,
            "watchdog_state": {"boot_number": self.boot, "healthy": True, "armed": False, "remaining_s": 0},
        }
        self.lock_path.write_text(json.dumps(lock), encoding="utf-8")

    def __call__(self, argv: list[str], timeout: float, stdin: str | None) -> tuple[int | None, str, str]:
        kind = argv[0]
        self.calls.append(kind)
        if kind == "fake-build":
            return self.build(stdin or "")
        if kind == "fake-reset":
            if self.reset_works:
                self.pending_reset = True
            return None, "", ""                      # the reset drops USB: its command times out
        if kind == "fake-recover":
            if self.pending_reset:
                self.pending_reset = False
                self.boot += 1
                if self.recover_failures_after_reset:
                    return 3, "", "needs-person"
            if self.recover_exit != 0:
                return self.recover_exit, "", "needs-person"
            self.attest()
            return 0, "{}", ""
        if kind == "fake-probe":
            return self.probe(argv)
        raise AssertionError(f"unexpected command {argv}")

    def build(self, script: str) -> tuple[int | None, str, str]:
        assert "git checkout -q --detach " + COMMIT in script
        assert "git status --porcelain --untracked-files=normal" in script
        out = []
        for line in script.splitlines():
            if line.startswith("build_case "):
                _, case_id, staged = shlex.split(line)
                data = f"prx for {case_id} at {COMMIT}".encode()
                Path(staged).write_bytes(data)
                out.append(f"NK_MATRIX_BUILT {case_id} {hashlib.sha256(data).hexdigest()}")
        out.append("NK_MATRIX_BUILD_DONE")
        return 0, "\n".join(out) + "\n", ""

    def probe(self, argv: list[str]) -> tuple[int | None, str, str]:
        out = Path(argv[argv.index("--out") + 1])
        cases = [argv[i + 1].split("=", 1) for i, word in enumerate(argv) if word == "--campaign-case"]
        assert cases[0][0] == "transport-write"
        case_id = cases[1][0]
        boot = self.boot
        self.launched.append((case_id, boot))
        behaviour = self.behaviours.get(case_id, "ok")
        if behaviour == "wedge":
            return 3, json.dumps({"verdict": "needs-person", "boots": [boot], "detail": "no channel answers"}), ""
        if behaviour == "crash":
            self.boot += 1                           # the resident agent reset the PSP during the launch
            self.attest()
            return 4, json.dumps({"verdict": "recovered", "boots": [boot, self.boot]}), ""
        if behaviour == "refused":
            return 2, json.dumps({"verdict": "refused", "boots": []}), ""
        boots = [boot]
        omit_boots = behaviour == "no-boots"
        if behaviour == "reboot-no-boots":
            self.boot += 1                           # a reboot that the probe command fails to report
            self.attest()
            omit_boots = True
        if behaviour == "reboot-ok":
            self.boot += 1                           # the runner's own L2 reset rebooted the PSP mid-launch
            self.attest()
            boots.append(self.boot)
        now = time.time_ns()
        envelopes = []
        for name, prx in cases:
            sha = hashlib.sha256(Path(prx).read_bytes()).hexdigest()
            envelopes.append({
                "CASE_ID": name,
                "SOURCE_COMMIT": COMMIT,
                "BINARY_SHA256": "0" * 64 if behaviour == "wrong-sha" and name == case_id else sha,
                "FW": "6.6.1",
                "HOST0_RUN_STARTED_NS": now - (3600 * 10**9 if behaviour == "stale" else 0),
                "ACCEPTANCE_ELIGIBLE": behaviour != "ineligible" or name != case_id,
                "ACCEPTANCE_BLOCKERS": ["one or more scalar result records did not pass"]
                if behaviour == "ineligible" and name == case_id else [],
            })
        if behaviour == "wrong-commit":
            envelopes[1]["SOURCE_COMMIT"] = "f" * 40
        out.write_text(json.dumps({"envelopes": envelopes, "terminal_reason": None}), encoding="utf-8")
        command_report: dict[str, Any] = {"verdict": "ok"}
        if not omit_boots:
            command_report["boots"] = boots
        return 0, json.dumps(command_report), ""


class MatrixTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        base = Path(self._tmp.name)
        self.repo = base / "repo"
        (self.repo / "tests" / "fixtures" / "psp_oracle").mkdir(parents=True)
        self.private = base / "private"
        self.private.mkdir()
        self.lock_path = base / "HARDWARE_LOCK.json"
        acquired = datetime.now(timezone.utc) - timedelta(minutes=1)
        self.lock_path.write_text(json.dumps({
            "state": "HELD", "holder_session": SESSION, "acquired_utc": _utc(acquired), "probes_run": [],
        }), encoding="utf-8")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def plan_dict(self, cases: list[dict[str, Any]], **extra: Any) -> dict[str, Any]:
        return {
            "schema": 1,
            "matrix_id": "m1",
            "session_id": SESSION,
            "source_commit": COMMIT,
            "console_model": "PSP-3000",
            "expected_firmware": "6.6.1",
            "host0_root": "build/host0-m1",
            "evidence_dir": "evidence",
            "cases": cases,
            "commands": {
                "build_shell_argv": ["fake-build"],
                "reset_argv": ["fake-reset"],
                "recover_argv": ["fake-recover", "--session", "{session_id}", "--host0-root", "{host0_root}"],
                "probe_argv": ["fake-probe", "--deadline", "{deadline}", "--log", "{runner_log}", "--"],
                "pspsh_argv": ["pspsh", "-e", "{remote_command}"],
                "usbhostfs_argv": ["usbhostfs_pc", "{host0_root_wsl}"],
            },
            **extra,
        }

    def write_plan(self, value: dict[str, Any]) -> Path:
        path = self.private / "plan.json"
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def load(self, cases: list[dict[str, Any]], **extra: Any) -> om.MatrixPlan:
        return om.load_plan(self.write_plan(self.plan_dict(cases, **extra)), repo_root=self.repo)

    def runner(self, plan: om.MatrixPlan, device: FakeDevice) -> om.MatrixRunner:
        return om.MatrixRunner(plan, command_runner=device, lock_path=self.lock_path,
                               source_tree_problem=lambda commit: None,
                               git_common_dir=lambda root: root / ".git")

    def device(self, **behaviours: str) -> FakeDevice:
        return FakeDevice(self.repo, self.lock_path, {k.replace("_", "-"): v for k, v in behaviours.items()})

    def lock(self) -> dict[str, Any]:
        return cast(dict[str, Any], json.loads(self.lock_path.read_text(encoding="utf-8")))


class LockHelperTests(MatrixTestCase):
    def test_note_launch_removes_temporary_lock_after_replace_failure(self) -> None:
        temporary = self.lock_path.with_name(f".{self.lock_path.name}.{os.getpid()}.tmp")
        with patch.object(om.os, "replace", side_effect=PermissionError("lock file is busy")):
            with self.assertRaisesRegex(PermissionError, "busy"):
                om.note_launch(self.lock_path, SESSION, "test note")
        self.assertFalse(temporary.exists())

    def test_attested_boot_uses_shared_runner_lock_reader_for_custom_path(self) -> None:
        self.device()
        boot, status = om.attested_boot(self.lock_path, SESSION)
        self.assertEqual(boot, 70)
        self.assertEqual(status, "HELD_AND_QUALIFIED")

    def test_note_launch_uses_the_runner_atomic_json_writer(self) -> None:
        self.device()
        writer = om._runner._write_campaign_json
        with patch.object(om._runner, "_write_campaign_json", wraps=writer) as shared_writer:
            om.note_launch(self.lock_path, SESSION, "shared writer test")
        shared_writer.assert_called_once()
        self.assertEqual(shared_writer.call_args.args[0], self.lock_path)

    def test_timeout_stops_descendants_that_inherit_the_capture_pipes(self) -> None:
        fixture_build = om.ROOT / "tests" / "fixtures" / "psp_oracle" / "build"
        fixture_build.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="matrix-timeout-tree-", dir=fixture_build) as name:
            # The command starts a descendant that inherits its stdout and stderr. The descendant
            # reports that it runs, and six seconds later would write a second file. The three-second
            # bound gives both interpreters time to start, so the timeout is taken with the descendant
            # alive and holding the capture pipes.
            running = Path(name) / "descendant-running.txt"
            finished = Path(name) / "descendant-finished.txt"
            child = (
                "import pathlib,sys,time; pathlib.Path(sys.argv[1]).write_text('running'); "
                "time.sleep(6); pathlib.Path(sys.argv[2]).write_text('finished')"
            )
            parent = (
                "import subprocess,sys,time; "
                "subprocess.Popen([sys.executable,'-c',sys.argv[1],sys.argv[2],sys.argv[3]], "
                "stdout=sys.stdout,stderr=sys.stderr); "
                "time.sleep(30)"
            )
            started = time.monotonic()
            code, _stdout, _stderr = om.run_command(
                [sys.executable, "-c", parent, child, str(running), str(finished)], 3.0
            )
            elapsed = time.monotonic() - started
            was_running = running.exists()
            # Outlive the descendant's own six seconds: a survivor writes its second file.
            while time.monotonic() - started < 7.5 and not finished.exists():
                time.sleep(0.1)
            survived = finished.exists()
        self.assertIsNone(code)
        self.assertTrue(was_running, "the descendant had not started when the command timed out")
        # Without the tree stop the capture pipes stay open until the descendant exits.
        self.assertLess(elapsed, 5.5)
        self.assertFalse(survived, "the descendant outlived the timed-out command")

    def test_probe_command_report_uses_the_last_trailing_json_object(self) -> None:
        report = om._probe_command_report(
            'wrapper {not-json}\n{"verdict":"old","boots":[70]}\n'
            '{"verdict":"last","boots":[71,72]}\n'
        )
        self.assertEqual(report, {"verdict": "last", "boots": [71, 72]})

    def test_probe_command_report_without_a_valid_boot_list_is_unknown(self) -> None:
        self.assertIsNone(om._boots({"verdict": "ok"}))
        self.assertIsNone(om._boots({"verdict": "ok", "boots": [70, "bad"]}))


# A second process that writes the hardware lock. It reports "ready", waits for "go" on stdin so both
# writers start together, then makes its updates: "note" appends probes_run lines through note_launch,
# "count" increments its own field through the shared writer, "read" reads the lock without the writer lock
# until that field reaches ``count`` and fails on any read error, and "hold" takes the writer lock and keeps it
# until stdin closes (or the process is killed). A writer pauses up to 2 ms between its updates, as real
# writers do (the writer lock is not fair: a process updating back to back can hold off the other until it
# stops with HARDWARE_LOCK_WRITER_TIMEOUT); the updates of the two processes still overlap throughout.
_LOCK_WRITER_PROCESS = """
import random, sys, time
from pathlib import Path
sys.path.insert(0, sys.argv[1])
from psp_oracle import oracle_matrix as om
path, mode, count, label = Path(sys.argv[2]), sys.argv[3], int(sys.argv[4]), sys.argv[5]
def bump(lock):
    lock[label] = lock.get(label, 0) + 1
if mode == "hold":
    with om._runner.hardware_lock_writer(path, timeout_seconds=30):
        print("ready", flush=True)
        sys.stdin.read()
    raise SystemExit(0)
print("ready", flush=True)
sys.stdin.readline()
if mode == "read":
    reads, errors, deadline = 0, [], time.monotonic() + 60
    while time.monotonic() < deadline:
        try:
            held, status, _state = om._runner._read_hardware_lock(sys.argv[6], lock_path=path)
            seen = om._read_lock(path).get(label, 0)
        except Exception as exc:
            errors.append(type(exc).__name__)
            continue
        reads += 1
        if status.startswith("HARDWARE_LOCK_UNAVAILABLE"):
            errors.append(status)
        if seen == count:
            break
    print(f"{reads} reads, {len(errors)} failed: {sorted(set(errors))}", file=sys.stderr)
    raise SystemExit(1 if errors or seen != count else 0)
for index in range(count):
    if mode == "note":
        om.note_launch(path, sys.argv[6], f"{label} {index}")
    else:
        om._runner.update_hardware_lock(path, bump)
    time.sleep(random.uniform(0, 0.002))
"""


class LockWriterProcessTests(MatrixTestCase):
    """Writers in separate processes share the lock file without losing each other's updates."""

    UPDATES = 150

    def start_writer(self, mode: str, label: str, count: int = 0) -> subprocess.Popen[str]:
        process = subprocess.Popen(
            [sys.executable, "-c", _LOCK_WRITER_PROCESS, _TOOLS_DIR, str(self.lock_path), mode,
             str(count), label, SESSION],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        self.addCleanup(self._stop_writer, process)
        assert process.stdout is not None
        self.assertEqual(process.stdout.readline().strip(), "ready", process.stderr)
        return process

    @staticmethod
    def _stop_writer(process: subprocess.Popen[str]) -> None:
        if process.poll() is None:
            process.kill()
        process.communicate(timeout=30)

    def run_together(self, *writers: subprocess.Popen[str]) -> None:
        for writer in writers:
            assert writer.stdin is not None
            writer.stdin.write("go\n")
            writer.stdin.flush()
        for writer in writers:
            _stdout, stderr = writer.communicate(timeout=120)
            self.assertEqual(writer.returncode, 0, stderr)

    def test_two_processes_noting_launches_lose_no_line(self) -> None:
        first = self.start_writer("note", "first", self.UPDATES)
        second = self.start_writer("note", "second", self.UPDATES)
        self.run_together(first, second)
        lines = [line.split(": ", 1)[1] for line in self.lock()["probes_run"]]
        expected = {f"{label} {index}" for label in ("first", "second") for index in range(self.UPDATES)}
        self.assertEqual(len(expected - set(lines)), 0, f"{len(expected - set(lines))} probes_run lines lost")
        self.assertEqual(len(lines), 2 * self.UPDATES)

    def test_note_launch_keeps_the_field_another_writer_process_changes(self) -> None:
        noter = self.start_writer("note", "noted", self.UPDATES)
        counter = self.start_writer("count", "lease_renewals", self.UPDATES)
        self.run_together(noter, counter)
        lock = self.lock()
        self.assertEqual(self.UPDATES - lock.get("lease_renewals", 0), 0, "lease_renewals updates lost")
        self.assertEqual(len(lock["probes_run"]), self.UPDATES, "probes_run lines lost")
        self.assertEqual(lock["holder_session"], SESSION)

    def test_readers_without_the_writer_lock_never_fail_while_a_writer_replaces_the_file(self) -> None:
        reader = self.start_writer("read", "lease_renewals", self.UPDATES)
        counter = self.start_writer("count", "lease_renewals", self.UPDATES)
        self.run_together(reader, counter)
        self.assertEqual(self.lock()["lease_renewals"], self.UPDATES)

    def test_writer_that_cannot_take_the_lock_stops_with_a_named_error(self) -> None:
        holder = self.start_writer("hold", "holder")
        before = self.lock_path.read_bytes()
        started = time.monotonic()
        with patch.object(om._runner, "HARDWARE_LOCK_WRITER_TIMEOUT_SECONDS", 0.5):
            with self.assertRaisesRegex(om.MatrixError, "^HARDWARE_LOCK_WRITER_TIMEOUT$"):
                om.note_launch(self.lock_path, SESSION, "blocked note")
        elapsed = time.monotonic() - started
        self.assertGreaterEqual(elapsed, 0.5)
        self.assertLess(elapsed, 5.0)
        self.assertEqual(self.lock_path.read_bytes(), before)
        with self.assertRaises(om._runner.HardwareLockError) as raised:
            om._runner.update_hardware_lock(self.lock_path, lambda lock: None, timeout_seconds=0)
        self.assertEqual(raised.exception.status, "HARDWARE_LOCK_WRITER_TIMEOUT")
        assert holder.stdin is not None
        holder.stdin.close()
        self.assertEqual(holder.wait(timeout=30), 0)
        om.note_launch(self.lock_path, SESSION, "after release")
        self.assertTrue(self.lock()["probes_run"][-1].endswith(": after release"))

    def test_a_holder_that_dies_does_not_block_the_next_writer(self) -> None:
        holder = self.start_writer("hold", "holder")
        holder.kill()
        holder.wait(timeout=30)
        sidecar = om._runner.hardware_lock_writer_path(self.lock_path)
        self.assertTrue(sidecar.exists(), "the dead holder's sidecar file stays behind")
        with patch.object(om._runner, "HARDWARE_LOCK_WRITER_TIMEOUT_SECONDS", 5.0):
            om.note_launch(self.lock_path, SESSION, "after the holder died")
        self.assertTrue(self.lock()["probes_run"][-1].endswith(": after the holder died"))

    def test_a_failed_mutation_writes_nothing_and_releases_the_writer_lock(self) -> None:
        before = self.lock_path.read_bytes()
        with self.assertRaisesRegex(om.MatrixError, "HARDWARE_LOCK_SESSION_MISMATCH"):
            om.note_launch(self.lock_path, "another-session", "refused note")
        self.assertEqual(self.lock_path.read_bytes(), before)
        om._runner.update_hardware_lock(self.lock_path, lambda lock: lock.update(lease="renewed"),
                                        timeout_seconds=0)
        self.assertEqual(self.lock()["lease"], "renewed")


class PlanValidationTests(MatrixTestCase):
    def refuses(self, pattern: str, cases=None, **extra) -> None:
        with self.assertRaisesRegex(om.MatrixError, pattern):
            self.load(cases if cases is not None else [{"case_id": "delay-zero", "timeout_seconds": 60}], **extra)

    def test_valid_plan_resolves_private_and_scratch_paths(self) -> None:
        plan = self.load([{"case_id": "delay-zero", "timeout_seconds": 60}])
        self.assertEqual(plan.host0_root, (self.repo / "tests/fixtures/psp_oracle/build/host0-m1").resolve())
        self.assertEqual(plan.evidence_dir, (self.private / "evidence").resolve())
        self.assertEqual(plan.max_contained_failures, 1)
        self.assertEqual(plan.cases[0].staged_name, "delay_zero.prx")

    def test_matrix_case_uses_the_shared_staged_name_rule(self) -> None:
        shared = om._staged_name
        self.assertEqual(shared("transport-write"), "transport_write.prx")
        self.assertEqual(om.MatrixCase("delay-zero", 60).staged_name, shared("delay-zero"))

    def test_refuses_listing_the_preflight(self) -> None:
        self.refuses("transport-write runs before every case", [{"case_id": "transport-write", "timeout_seconds": 60}])

    def test_refuses_duplicates_retired_and_short_timeouts(self) -> None:
        case = {"case_id": "delay-zero", "timeout_seconds": 60}
        self.refuses("duplicate case id", [case, case])
        self.refuses("retired", [{"case_id": "hle-null-out-pointers", "timeout_seconds": 60}])
        self.refuses("needs timeout_seconds 180", [{"case_id": "ge-timing", "timeout_seconds": 60}])
        self.refuses("timeout_seconds must be", [{"case_id": "delay-zero", "timeout_seconds": 0}])
        self.refuses("timeout_seconds must be", [{"case_id": "delay-zero", "timeout_seconds": 7200}])
        self.refuses("at most 64", [{"case_id": f"c{i}", "timeout_seconds": 60} for i in range(65)])

    def test_refuses_case_ids_that_collide_after_staging_name_normalization(self) -> None:
        with self.assertRaisesRegex(om.MatrixError, "stage to the same PRX name"):
            self.load([
                {"case_id": "a-b", "timeout_seconds": 60},
                {"case_id": "a_b", "timeout_seconds": 60},
            ])
        with self.assertRaisesRegex(om.MatrixError, "transport-write.*transport_write"):
            self.load([{"case_id": "transport_write", "timeout_seconds": 60}])

    def test_refuses_host0_outside_the_fixture_scratch_and_evidence_outside_the_plan(self) -> None:
        self.refuses("host0_root must stay under", host0_root=str(self.private))
        plan = self.plan_dict([{"case_id": "delay-zero", "timeout_seconds": 60}])
        plan["evidence_dir"] = str(self.repo)
        with self.assertRaisesRegex(om.MatrixError, "evidence_dir must stay under"):
            om.load_plan(self.write_plan(plan), repo_root=self.repo)

    def test_refuses_host0_outside_build_and_visible_evidence_inside_repo(self) -> None:
        self.refuses("host0_root must stay under.*build", host0_root="host0")
        plan_dir = self.repo / "plans"
        plan_dir.mkdir()
        plan_path = plan_dir / "visible-plan.json"
        plan = self.plan_dict(
            [{"case_id": "delay-zero", "timeout_seconds": 60}],
            evidence_dir="evidence",
        )
        plan_path.write_text(json.dumps(plan), encoding="utf-8")
        with self.assertRaisesRegex(om.MatrixError, "evidence_dir.*ignored"):
            om.load_plan(plan_path, repo_root=self.repo)

    def test_prebuilt_prx_needs_a_pinned_digest_inside_the_plan_directory(self) -> None:
        self.refuses("needs its lowercase sha256 pinned", [{"case_id": "nk-hang", "timeout_seconds": 60,
                                                             "prebuilt_prx": "nk_test_hang.prx"}])
        self.refuses("must stay under", [{"case_id": "nk-hang", "timeout_seconds": 60,
                                          "prebuilt_prx": str(self.repo / "x.prx"), "sha256": "a" * 64}])
        self.refuses("sha256 pins a prebuilt_prx only", [{"case_id": "delay-zero", "timeout_seconds": 60,
                                                         "sha256": "a" * 64}])

    def test_refuses_unknown_placeholders_commands_and_bad_bounds(self) -> None:
        plan = self.plan_dict([{"case_id": "delay-zero", "timeout_seconds": 60}])
        plan["commands"]["reset_argv"] = ["fake-reset", "{deadline}"]
        with self.assertRaisesRegex(om.MatrixError, r"unknown placeholder \{deadline\}"):
            om.load_plan(self.write_plan(plan), repo_root=self.repo)
        plan = self.plan_dict([{"case_id": "delay-zero", "timeout_seconds": 60}])
        plan["commands"]["shell"] = ["sh"]
        with self.assertRaisesRegex(om.MatrixError, "unknown commands: shell"):
            om.load_plan(self.write_plan(plan), repo_root=self.repo)
        self.refuses("watchdog_deadline_seconds", watchdog_deadline_seconds=5)
        self.refuses("max_contained_failures", max_contained_failures=9)
        self.refuses("expected_firmware", expected_firmware="6.61")

    def test_runner_argv_templates_reject_unknown_placeholders(self) -> None:
        for command, valid, invalid in (
            ("pspsh_argv", "{remote_command}", "{remote_comand}"),
            ("usbhostfs_argv", "{host0_root_wsl}", "{host0_root_wsl_path}"),
        ):
            with self.subTest(command=command):
                plan = self.plan_dict([{"case_id": "delay-zero", "timeout_seconds": 60}])
                plan["commands"][command] = ["tool", valid]
                self.assertEqual(
                    om.load_plan(self.write_plan(plan), repo_root=self.repo).commands[command],
                    ["tool", valid],
                )
                plan["commands"][command] = ["tool", invalid]
                with self.assertRaisesRegex(om.MatrixError, "unknown placeholder"):
                    om.load_plan(self.write_plan(plan), repo_root=self.repo)

    def test_null_expected_firmware_is_refused_during_plan_load(self) -> None:
        with self.assertRaisesRegex(om.MatrixError, "expected_firmware"):
            self.load(
                [{"case_id": "delay-zero", "timeout_seconds": 60}],
                expected_firmware=None,
            )

    def test_render_refuses_a_placeholder_without_a_value(self) -> None:
        self.assertEqual(om.render_argv(["a{x}b"], {"x": "1"}), ["a1b"])
        with self.assertRaisesRegex(om.MatrixError, r"\{y\}"):
            om.render_argv(["{y}"], {"x": "1"})


class BuildTests(MatrixTestCase):
    def test_script_builds_each_case_from_a_clean_clone_of_the_commit(self) -> None:
        plan = self.load([{"case_id": "delay-zero", "timeout_seconds": 60}])
        script = om.build_script(plan, "/repo/.git", [om.MatrixCase("transport-write", 0.0), plan.cases[0]])
        self.assertIn("git clone -q --no-checkout --no-hardlinks /repo/.git", script)
        self.assertIn(f"git checkout -q --detach {COMMIT}", script)
        self.assertIn('[ -z "$(git status --porcelain --untracked-files=normal)" ] || fail dirty', script)
        self.assertIn('BUILD_DIR="build/$1" EBOOT.PBP', script)
        staged = re.findall(r"^build_case (\S+) (\S+)$", script, re.MULTILINE)
        self.assertEqual([name for name, _ in staged], ["transport-write", "delay-zero"])
        self.assertTrue(staged[1][1].endswith("host0-m1/delay_zero.prx"))

    @unittest.skipUnless(sys.platform == "win32", "WSL drive paths come from a Windows host; POSIX paths have no drive")
    def test_wsl_path_style_converts_staged_paths(self) -> None:
        plan = self.load([{"case_id": "delay-zero", "timeout_seconds": 60}])
        wsl = om.MatrixPlan(**{**plan.__dict__, "path_style": "wsl",
                               "host0_root": Path("X:/repo/tests/fixtures/psp_oracle/build/h")})
        self.assertIn("/mnt/x/repo/tests/fixtures/psp_oracle/build/h/delay_zero.prx",
                      om.build_script(wsl, "/mnt/x/repo/.git", [plan.cases[0]]))

    def test_build_script_reaches_the_shell_with_lf_line_ends(self) -> None:
        # Measured on Windows 2026-10-29: text-mode stdin turned every LF into CRLF and bash refused the script.
        code, out, _err = om.run_command(
            [sys.executable, "-c", "import sys; sys.stdout.write(sys.stdin.buffer.read().hex())"], 60,
            "set -u\nbuild_case() {\n}\n")
        self.assertEqual(code, 0)
        self.assertEqual(bytes.fromhex(out), b"set -u\nbuild_case() {\n}\n")

    def test_incomplete_or_failed_build_output_is_a_refusal(self) -> None:
        good = f"NK_MATRIX_BUILT a {'1' * 64}\nNK_MATRIX_BUILD_DONE\n"
        self.assertEqual(om.parse_build_output(good, ["a"]), {"a": "1" * 64})
        with self.assertRaisesRegex(om.MatrixError, "did not report every case"):
            om.parse_build_output(f"NK_MATRIX_BUILT a {'1' * 64}\n", ["a"])
        with self.assertRaisesRegex(om.MatrixError, "did not report every case"):
            om.parse_build_output(good, ["a", "b"])
        with self.assertRaisesRegex(om.MatrixError, "build failed: make b"):
            om.parse_build_output("NK_MATRIX_BUILD_FAILED make b\n", ["a", "b"])

    def test_staged_digest_must_match_the_reported_one(self) -> None:
        plan = self.load([{"case_id": "delay-zero", "timeout_seconds": 60}])
        device = self.device()
        real_build = device.build

        def tampering_build(script: str):
            code, out, err = real_build(script)
            (plan.host0_root / "delay_zero.prx").write_bytes(b"swapped after the build")
            return code, out, err

        device.build = tampering_build  # type: ignore[method-assign]
        with self.assertRaisesRegex(om.MatrixError, "differs from the one the build reported"):
            self.runner(plan, device).build()

    def test_prebuilt_prx_is_staged_only_with_its_pinned_digest(self) -> None:
        data = b"resident-agent containment test probe"
        (self.private / "nk_test_hang.prx").write_bytes(data)
        good = hashlib.sha256(data).hexdigest()
        plan = self.load([{"case_id": "nk-hang", "timeout_seconds": 60, "prebuilt_prx": "nk_test_hang.prx",
                           "sha256": good}])
        staged = self.runner(plan, self.device()).build()
        self.assertEqual(staged["nk-hang"], good)
        self.assertEqual(list(staged), ["transport-write", "nk-hang"])
        bad = self.load([{"case_id": "nk-hang", "timeout_seconds": 60, "prebuilt_prx": "nk_test_hang.prx",
                          "sha256": "b" * 64}])
        with self.assertRaisesRegex(om.MatrixError, "not the pinned one"):
            self.runner(bad, self.device()).build()

    def test_companion_prx_is_built_staged_hashed_and_verified(self) -> None:
        plan = self.load([{
            "case_id": "hle-self-stop-unload",
            "timeout_seconds": om.MAX_CASE_TIMEOUT_SECONDS,
        }])
        runner = self.runner(plan, self.device())
        staged = runner.build()
        companion = plan.host0_root / "hle_self_stop_helper.prx"
        self.assertTrue(companion.is_file())
        self.assertEqual(staged["hle-self-stop-helper"], hashlib.sha256(companion.read_bytes()).hexdigest())
        runner.verify_staged(staged)
        companion.write_bytes(b"stale companion from another matrix")
        with self.assertRaisesRegex(om.MatrixError, "hle-self-stop-helper: staged PRX changed"):
            runner.verify_staged(staged)

    def test_build_uses_the_shared_runner_binary_hasher(self) -> None:
        plan = self.load([{"case_id": "delay-zero", "timeout_seconds": 60}])
        runner = self.runner(plan, self.device())
        with patch.object(om._runner, "_host_binary_sha256", wraps=om._runner._host_binary_sha256) as shared:
            staged = runner.build()
        self.assertEqual(list(staged), ["transport-write", "delay-zero"])
        self.assertGreaterEqual(shared.call_count, 2)

    def test_build_wraps_staged_prx_read_errors_as_matrix_errors(self) -> None:
        plan = self.load([{"case_id": "delay-zero", "timeout_seconds": 60}])
        runner = self.runner(plan, self.device())

        def missing_preflight(path):
            if path.name == "transport_write.prx":
                raise FileNotFoundError("staged PRX disappeared")
            return om._runner._host_binary_sha256(path)

        with patch.object(om._runner, "_host_binary_sha256", side_effect=missing_preflight):
            with self.assertRaisesRegex(om.MatrixError, "transport-write: staged PRX is unavailable"):
                runner.build()


class MatrixExecutionTests(MatrixTestCase):
    CASES = [{"case_id": "delay-zero", "timeout_seconds": 60}, {"case_id": "ge-nan", "timeout_seconds": 60},
             {"case_id": "dma-cells", "timeout_seconds": 60}]

    def test_healthy_matrix_runs_each_case_on_its_own_fresh_boot(self) -> None:
        plan = self.load(self.CASES)
        device = self.device()
        code, report = self.runner(plan, device).execute()
        self.assertEqual(code, 0, report)
        self.assertEqual(report["accepted"], ["delay-zero", "ge-nan", "dma-cells"])
        self.assertEqual(device.launched, [("delay-zero", 71), ("ge-nan", 72), ("dma-cells", 73)])
        self.assertEqual(device.calls.count("fake-reset"), 3)
        self.assertEqual(report["state"], "COMPLETE")
        notes = self.lock()["probes_run"]
        self.assertEqual(len(notes), 6)
        self.assertIn("boot 71, launching transport-write + delay-zero", notes[0])
        self.assertIn("delay-zero on boot 71: ACCEPTED", notes[1])
        saved = json.loads(plan.report_path.read_text(encoding="utf-8"))
        self.assertEqual(saved["accepted"], report["accepted"])

    def test_changed_staged_prx_is_refused_before_the_next_reset(self) -> None:
        plan = self.load(self.CASES[:2])
        device = self.device()
        runner = self.runner(plan, device)
        original_launch = runner.launch

        def launch_then_swap(case, boot, index, staged):
            outcome = original_launch(case, boot, index, staged)
            if case.case_id == "delay-zero":
                (plan.host0_root / "ge_nan.prx").write_bytes(b"replacement after first case")
            return outcome

        runner.launch = launch_then_swap  # type: ignore[method-assign]
        code, report = runner.execute()
        self.assertEqual(code, 2)
        self.assertEqual(report["state"], om.REJECTED)
        self.assertEqual(report["accepted"], ["delay-zero"])
        self.assertEqual([item["case_id"] for item in report["not_run"]], ["ge-nan"])
        self.assertEqual(device.launched, [("delay-zero", 71)])
        self.assertEqual(device.calls.count("fake-reset"), 1)

    def test_build_only_on_completed_checkpoint_preserves_its_terminal_state(self) -> None:
        plan = self.load(self.CASES[:1])
        runner = self.runner(plan, self.device())
        self.assertEqual(runner.execute()[0], 0)
        code, report = runner.execute(build_only=True)
        self.assertEqual(code, 0)
        self.assertEqual(report["state"], "COMPLETE")
        saved = json.loads(plan.report_path.read_text(encoding="utf-8"))
        self.assertEqual(saved["state"], "COMPLETE")

    def test_last_contained_case_over_budget_reports_exhausted_state(self) -> None:
        plan = self.load(self.CASES[:1], max_contained_failures=0)
        code, report = self.runner(plan, self.device(delay_zero="crash")).execute()
        self.assertEqual(code, 2)
        self.assertEqual(report["outcomes"][0]["status"], om.CONTAINED)
        self.assertEqual(report["state"], om.CONTAINED_BUDGET_EXHAUSTED)

    def test_lock_lost_mid_matrix_writes_a_report_of_prior_hardware_results(self) -> None:
        plan = self.load(self.CASES[:2])
        device = self.device()
        runner = self.runner(plan, device)
        original_launch = runner.launch

        def release_lock_after_launch(case, boot, index, staged):
            outcome = original_launch(case, boot, index, staged)
            lock = self.lock()
            lock["state"] = "RELEASED"
            self.lock_path.write_text(json.dumps(lock), encoding="utf-8")
            return outcome

        runner.launch = release_lock_after_launch  # type: ignore[method-assign]
        plan.evidence_dir.mkdir(parents=True, exist_ok=True)
        plan.report_path.write_text(json.dumps({"state": "STALE_REPORT"}), encoding="utf-8")
        code, report = runner.execute()
        self.assertEqual(code, 2)
        self.assertEqual(report["state"], om.LOCK_LOST)
        self.assertEqual(report["accepted"], ["delay-zero"])
        self.assertEqual([item["case_id"] for item in report["not_run"]], ["ge-nan"])
        saved = json.loads(plan.report_path.read_text(encoding="utf-8"))
        self.assertEqual(saved["accepted"], ["delay-zero"])
        self.assertIn("HARDWARE_LOCK_NOT_HELD", " ".join(saved["events"]))

    def test_post_launch_probe_log_oserror_keeps_outcome_checkpointed(self) -> None:
        plan = self.load(self.CASES[:1])
        device = self.device()
        runner = self.runner(plan, device)
        original_write = Path.write_text

        def fail_probe_log(path, data, *args, **kwargs):
            if path.name.endswith(".probe-command.txt"):
                raise OSError("evidence volume is unavailable")
            return original_write(path, data, *args, **kwargs)

        with patch.object(Path, "write_text", autospec=True, side_effect=fail_probe_log):
            code, report = runner.execute()
        self.assertEqual(code, 0, report)
        self.assertEqual(report["accepted"], ["delay-zero"])
        self.assertTrue(any("probe-command evidence" in event for event in report["events"]))
        saved = json.loads(plan.checkpoint_path.read_text(encoding="utf-8"))
        self.assertEqual([item["case_id"] for item in saved["outcomes"]], ["delay-zero"])
        runner.execute()
        self.assertEqual(len(device.launched), 1)

    def test_post_launch_lock_note_oserror_keeps_outcome_checkpointed(self) -> None:
        plan = self.load(self.CASES[:1])
        device = self.device()
        runner = self.runner(plan, device)
        real_note = om.note_launch

        def fail_closing_note(path, session_id, text):
            if " on boot " in text and text.endswith("ACCEPTED"):
                raise OSError("lock note volume is unavailable")
            return real_note(path, session_id, text)

        with patch.object(om, "note_launch", side_effect=fail_closing_note):
            code, report = runner.execute()
        self.assertEqual(code, 0, report)
        self.assertEqual(report["accepted"], ["delay-zero"])
        self.assertTrue(any("probes_run note after delay-zero not written" in event
                            for event in report["events"]))
        saved = json.loads(plan.checkpoint_path.read_text(encoding="utf-8"))
        self.assertEqual([item["case_id"] for item in saved["outcomes"]], ["delay-zero"])
        runner.execute()
        self.assertEqual(len(device.launched), 1)

    def test_checkpoint_identity_binds_measurement_and_execution_parameters(self) -> None:
        data = b"synthetic prebuilt PRX for checkpoint identity"
        (self.private / "nk_hang.prx").write_bytes(data)
        case = {
            "case_id": "nk-hang",
            "timeout_seconds": 600,
            "prebuilt_prx": "nk_hang.prx",
            "sha256": hashlib.sha256(data).hexdigest(),
        }
        base = self.plan_dict([case])
        plan = om.load_plan(self.write_plan(base), repo_root=self.repo)
        device = self.device(nk_hang="wedge")
        self.assertEqual(self.runner(plan, device).execute()[0], 3)

        alternate = self.private / "alternate.prx"
        alternate.write_bytes(data)
        mutations = (
            ("console_model", lambda raw: raw.update(console_model="PSP-2000")),
            ("expected_firmware", lambda raw: raw.update(expected_firmware="6.6.0")),
            ("host0_root", lambda raw: raw.update(host0_root="build/host0-other")),
            ("watchdog_deadline", lambda raw: raw.update(
                watchdog_deadline_seconds=om.DEFAULT_WATCHDOG_DEADLINE + 1
            )),
            ("max_contained_failures", lambda raw: raw.update(max_contained_failures=2)),
            ("case_timeout", lambda raw: raw["cases"][0].update(timeout_seconds=601)),
            ("prebuilt_path", lambda raw: raw["cases"][0].update(prebuilt_prx="alternate.prx")),
            ("prebuilt_pin", lambda raw: raw["cases"][0].update(sha256="f" * 64)),
        )
        for name, mutate in mutations:
            with self.subTest(name=name):
                changed = json.loads(json.dumps(base))
                mutate(changed)
                candidate = om.load_plan(self.write_plan(changed), repo_root=self.repo)
                resume_device = self.device()
                with self.assertRaisesRegex(om.MatrixError, "different matrix"):
                    self.runner(candidate, resume_device).execute()
                self.assertEqual(resume_device.calls, [])

    def test_resume_refuses_checkpoint_prebuilt_digest_that_disagrees_with_plan_pin(self) -> None:
        data = b"synthetic pinned PRX"
        (self.private / "nk_hang.prx").write_bytes(data)
        pin = hashlib.sha256(data).hexdigest()
        plan = self.load([{
            "case_id": "nk-hang", "timeout_seconds": 600,
            "prebuilt_prx": "nk_hang.prx", "sha256": pin,
        }])
        runner = self.runner(plan, self.device(nk_hang="wedge"))
        self.assertEqual(runner.execute()[0], 3)
        checkpoint = json.loads(plan.checkpoint_path.read_text(encoding="utf-8"))
        tampered = b"different checkpointed PRX"
        checkpointed_digest = hashlib.sha256(tampered).hexdigest()
        (plan.host0_root / "nk_hang.prx").write_bytes(tampered)
        checkpoint["staged"]["nk-hang"] = checkpointed_digest
        plan.checkpoint_path.write_text(json.dumps(checkpoint), encoding="utf-8")
        with self.assertRaisesRegex(om.MatrixError, "checkpoint staged digest.*pinned"):
            runner.execute()

    def test_runner_without_boot_report_cannot_accept_a_launch(self) -> None:
        plan = self.load(self.CASES[:1], matrix_id="no-boots")
        device = self.device(**{"delay-zero": "no-boots"})
        code, report = self.runner(plan, device).execute()
        self.assertEqual(code, 2)
        self.assertEqual(report["outcomes"][0]["status"], om.NOT_ELIGIBLE)
        self.assertNotIn("delay-zero", report["accepted"])

    def test_attested_reboot_during_a_bootless_probe_report_is_contained(self) -> None:
        plan = self.load(self.CASES[:1], matrix_id="reboot-no-boots")
        device = self.device(**{"delay-zero": "reboot-no-boots"})
        code, report = self.runner(plan, device).execute()
        self.assertEqual(code, 2)
        outcome = report["outcomes"][0]
        self.assertEqual(outcome["status"], om.CONTAINED)
        self.assertIn("rebooted during the launch", " ".join(outcome["reasons"]))
        self.assertNotIn("delay-zero", report["accepted"])

    def test_missing_boot_list_is_not_accepted_and_attested_reboot_is_contained(self) -> None:
        plan = self.load(self.CASES[:1], matrix_id="missing-boots")
        device = self.device(**{"delay-zero": "reboot-no-boots"})
        code, report = self.runner(plan, device).execute()
        self.assertEqual(code, 2)
        outcome = report["outcomes"][0]
        self.assertEqual(outcome["status"], om.CONTAINED)
        self.assertIn("rebooted during the launch", " ".join(outcome["reasons"]))
        self.assertNotIn("delay-zero", report["accepted"])

        plan = self.load(self.CASES[:1], matrix_id="no-boots")
        device = self.device(**{"delay-zero": "no-boots"})
        code, report = self.runner(plan, device).execute()
        self.assertEqual(code, 2)
        self.assertEqual(report["outcomes"][0]["status"], om.NOT_ELIGIBLE)

    def test_crashing_case_is_contained_and_never_accepted(self) -> None:
        plan = self.load(self.CASES)
        device = self.device(ge_nan="crash")
        code, report = self.runner(plan, device).execute()
        self.assertEqual(code, 2)
        statuses = [item["status"] for item in report["outcomes"]]
        self.assertEqual(statuses, [om.ACCEPTED, om.CONTAINED, om.ACCEPTED])
        # The crash rebooted the PSP to boot 73; the next case still gets its own newer boot.
        self.assertEqual(device.launched, [("delay-zero", 71), ("ge-nan", 72), ("dma-cells", 74)])
        self.assertIn(73, report["used_boots"])
        self.assertIn("rebooted during the launch (boot 72 -> 73)", " ".join(report["outcomes"][1]["reasons"]))
        kept = (plan.evidence_dir / "m1-01-ge-nan-boot72.probe-command.txt").read_text(encoding="utf-8")
        self.assertTrue(kept.startswith("exit 4\n"))
        self.assertIn('"verdict": "recovered"', kept)

    def test_reboot_during_an_otherwise_eligible_launch_is_contained(self) -> None:
        plan = self.load(self.CASES[:2])
        device = self.device(delay_zero="reboot-ok")
        code, report = self.runner(plan, device).execute()
        self.assertEqual(code, 2)
        self.assertEqual([item["status"] for item in report["outcomes"]], [om.CONTAINED, om.ACCEPTED])
        self.assertEqual(report["outcomes"][0]["probe_exit"], 0)
        self.assertEqual(device.launched, [("delay-zero", 71), ("ge-nan", 73)])

    def test_contained_failure_budget_stops_the_matrix(self) -> None:
        plan = self.load(self.CASES, max_contained_failures=0)
        device = self.device(delay_zero="crash")
        code, report = self.runner(plan, device).execute()
        self.assertEqual(code, 2)
        self.assertEqual(report["state"], "CONTAINED_FAILURE_BUDGET_EXHAUSTED")
        self.assertEqual([item["case_id"] for item in report["not_run"]], ["ge-nan", "dma-cells"])
        self.assertEqual(len(device.launched), 1)

    def test_wedge_ends_in_an_explicit_intervention_state(self) -> None:
        plan = self.load(self.CASES)
        device = self.device(ge_nan="wedge")
        code, report = self.runner(plan, device).execute()
        self.assertEqual(code, 3)
        self.assertEqual(report["state"], om.INTERVENTION)
        self.assertEqual(report["outcomes"][1]["status"], om.INTERVENTION)
        self.assertEqual([item["case_id"] for item in report["not_run"]], ["dma-cells"])
        self.assertEqual(len(device.launched), 2)

    def test_failed_recovery_after_the_reset_stops_before_any_launch(self) -> None:
        plan = self.load(self.CASES)
        device = self.device()
        device.recover_failures_after_reset = True
        code, report = self.runner(plan, device).execute()
        self.assertEqual(code, 3)
        self.assertEqual(device.launched, [])
        self.assertEqual(report["state"], om.INTERVENTION)
        self.assertEqual(report["outcomes"], [])
        self.assertIn("recover command exited 3", report["stopped_before"]["reason"])
        self.assertEqual(len(report["not_run"]), 3)
        # Nothing was launched, so a resumed matrix starts again at the same case.
        device.recover_failures_after_reset = False
        code, report = self.runner(plan, device).execute()
        self.assertEqual(code, 0, report)
        self.assertEqual([case for case, _ in device.launched], ["delay-zero", "ge-nan", "dma-cells"])
        self.assertIsNone(report["stopped_before"])

    def test_refused_recovery_stops_without_claiming_an_intervention(self) -> None:
        plan = self.load(self.CASES)
        device = self.device()
        device.recover_exit = 2
        code, report = self.runner(plan, device).execute()
        self.assertEqual(code, 2)
        self.assertEqual(report["state"], om.RECOVERY_REFUSED)
        self.assertEqual(device.launched, [])

    def test_reset_that_yields_no_newer_boot_is_not_a_fresh_epoch(self) -> None:
        plan = self.load(self.CASES)
        device = self.device()
        device.reset_works = False
        code, report = self.runner(plan, device).execute()
        self.assertEqual(code, 2)
        self.assertEqual(report["state"], om.NO_FRESH_BOOT)
        self.assertEqual(device.launched, [])
        self.assertIn("did not produce a newer qualified boot", report["stopped_before"]["reason"])

    def test_stale_wrong_fixture_and_wrong_runner_results_are_rejected(self) -> None:
        for behaviour, reason in (("stale", "does not belong to this launch"),
                                  ("wrong-sha", "not the staged fixture's"),
                                  ("wrong-commit", "wrong runner")):
            with self.subTest(behaviour=behaviour):
                self.tearDown()
                self.setUp()
                plan = self.load(self.CASES[:1])
                code, report = self.runner(plan, self.device(delay_zero=behaviour)).execute()
                self.assertEqual(code, 2)
                outcome = report["outcomes"][0]
                self.assertEqual(outcome["status"], om.REJECTED)
                self.assertIn(reason, " ".join(outcome["reasons"]))
                self.assertEqual(report["accepted"], [])

    def test_ineligible_and_refused_launches_are_reported_not_accepted(self) -> None:
        plan = self.load(self.CASES[:2])
        code, report = self.runner(plan, self.device(delay_zero="ineligible", ge_nan="refused")).execute()
        self.assertEqual(code, 2)
        first, second = report["outcomes"]
        self.assertEqual(first["status"], om.NOT_ELIGIBLE)
        self.assertIn("did not pass", " ".join(first["reasons"]))
        self.assertEqual(second["status"], om.NOT_ELIGIBLE)
        self.assertIn("no valid boots list", " ".join(second["reasons"]))

    def test_lock_held_by_another_session_refuses_before_touching_the_device(self) -> None:
        plan = self.load(self.CASES)
        device = self.device()
        lock = self.lock()
        lock["holder_session"] = "someone-else"
        self.lock_path.write_text(json.dumps(lock), encoding="utf-8")
        with self.assertRaisesRegex(om.MatrixError, "HARDWARE_LOCK_SESSION_MISMATCH"):
            self.runner(plan, device).execute()
        self.assertNotIn("fake-reset", device.calls)
        self.assertNotIn("fake-recover", device.calls)
        self.assertEqual(device.launched, [])

    def test_resume_continues_after_an_intervention_without_retrying_the_case(self) -> None:
        plan = self.load(self.CASES)
        device = self.device(ge_nan="wedge")
        self.assertEqual(self.runner(plan, device).execute()[0], 3)
        device.behaviours["ge-nan"] = "ok"
        code, report = self.runner(plan, device).execute()
        self.assertEqual(code, 2)                      # ge-nan stays an intervention, not a pass
        self.assertEqual([item["status"] for item in report["outcomes"]],
                         [om.ACCEPTED, om.INTERVENTION, om.ACCEPTED])
        self.assertEqual([case for case, _ in device.launched], ["delay-zero", "ge-nan", "dma-cells"])
        self.assertEqual(device.calls.count("fake-build"), 1)

    def test_resume_refuses_a_changed_fixture_or_plan(self) -> None:
        plan = self.load(self.CASES)
        device = self.device(ge_nan="wedge")
        self.runner(plan, device).execute()
        (plan.host0_root / "dma_cells.prx").write_bytes(b"rebuilt behind the checkpoint")
        with self.assertRaisesRegex(om.MatrixError, "changed since it was built"):
            self.runner(plan, device).execute()
        other = self.load(self.CASES[:2])
        with self.assertRaisesRegex(om.MatrixError, "different matrix"):
            self.runner(other, device).execute()

    def test_build_only_touches_no_device(self) -> None:
        plan = self.load(self.CASES)
        device = self.device()
        code, report = self.runner(plan, device).execute(build_only=True)
        self.assertEqual(code, 0)
        self.assertEqual(report["state"], "BUILT")
        self.assertEqual(device.calls, ["fake-build"])
        self.assertEqual(sorted(report["staged"]), sorted(["transport-write", "delay-zero", "ge-nan", "dma-cells"]))

    def test_dirty_source_checkout_is_refused_before_the_build(self) -> None:
        plan = self.load(self.CASES)
        device = self.device()
        runner = om.MatrixRunner(plan, command_runner=device, lock_path=self.lock_path,
                                 source_tree_problem=lambda commit: "source worktree is dirty",
                                 git_common_dir=lambda root: root / ".git")
        with self.assertRaisesRegex(om.MatrixError, "dirty"):
            runner.execute()
        self.assertEqual(device.calls, [])


class ClassifyLaunchTests(MatrixTestCase):
    def classify(self, path: Path, **overrides):
        values: dict[str, Any] = dict(case=om.MatrixCase("delay-zero", 60.0), epoch_boot=80, probe_exit=0,
                      probe_stdout='{"boots": [80]}', runner_report_path=path, launch_started_ns=time.time_ns(),
                      staged={"transport-write": "a" * 64, "delay-zero": "b" * 64}, source_commit=COMMIT,
                      expected_firmware="6.6.1")
        values.update(overrides)
        return om.classify_launch(**values)

    def test_report_file_older_than_the_launch_is_stale(self) -> None:
        path = self.private / "out.json"
        path.write_text(json.dumps({"envelopes": []}), encoding="utf-8")
        old = time.time() - 3600
        os.utime(path, (old, old))
        outcome = self.classify(path)
        self.assertEqual(outcome.status, om.REJECTED)
        self.assertIn("predates this launch", outcome.reasons[0])

    def test_runner_stopping_after_the_preflight_is_not_eligible(self) -> None:
        path = self.private / "out.json"
        path.write_text(json.dumps({"terminal_reason": "HOST0_ROUNDTRIP_FAILED", "envelopes": [{
            "CASE_ID": "transport-write", "SOURCE_COMMIT": COMMIT, "BINARY_SHA256": "a" * 64, "FW": "6.6.1",
            "HOST0_RUN_STARTED_NS": time.time_ns(), "ACCEPTANCE_ELIGIBLE": False}]}), encoding="utf-8")
        outcome = self.classify(path)
        self.assertEqual(outcome.status, om.NOT_ELIGIBLE)
        self.assertIn("HOST0_ROUNDTRIP_FAILED", outcome.reasons[0])

    def test_unexpected_case_order_is_rejected(self) -> None:
        path = self.private / "out.json"
        path.write_text(json.dumps({"envelopes": [{"CASE_ID": "delay-zero"}]}), encoding="utf-8")
        self.assertEqual(self.classify(path).status, om.REJECTED)

    def test_probe_command_timeout_is_an_intervention(self) -> None:
        outcome = self.classify(self.private / "missing.json", probe_exit=None, probe_stdout="")
        self.assertEqual(outcome.status, om.INTERVENTION)


class ImportIsolationTests(unittest.TestCase):
    def test_imports_psp_oracle_as_top_level_package(self) -> None:
        self.assertNotIn("tools.psp_oracle", sys.modules)
        self.assertNotIn("tools.psp_oracle.oracle_matrix", sys.modules)
        self.assertIs(sys.modules.get("psp_oracle.oracle_matrix"), om)
        self.assertNotIn("tools.psp_oracle", _LOCK_WRITER_PROCESS)


if __name__ == "__main__":
    unittest.main()
