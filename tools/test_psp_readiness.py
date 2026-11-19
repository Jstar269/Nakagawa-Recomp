# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

import os
from pathlib import Path
import sys
import subprocess
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import psp_readiness  # noqa: E402


class ReadinessAggregationTests(unittest.TestCase):
    def collect_on_synthetic_root(self, *, platform: str, tools: dict[str, str], with_hst: bool) -> dict:
        with tempfile.TemporaryDirectory(prefix="psp-readiness-") as scratch:
            root = Path(scratch)
            (root / "tools" / "psp_oracle").mkdir(parents=True)
            (root / "tests" / "fixtures" / "psp_oracle").mkdir(parents=True)
            (root / "tools" / "psp_oracle" / "manifest.json").write_text("{}", encoding="utf-8")
            probe_root = root / "tests" / "fixtures" / "psp_oracle"
            (probe_root / "probe.c").write_text("/* synthetic */\n", encoding="utf-8")
            (probe_root / "Makefile").write_text("# synthetic\n", encoding="utf-8")
            if with_hst:
                hst = root / "build" / "hst"
                hst.mkdir(parents=True)
                (hst / "hst.exe").write_bytes(b"synthetic executable")
                (hst / "hst_recomp_0.c").write_text("/* synthetic */\n", encoding="utf-8")

            lock_module = types.ModuleType("pspdev_lock")
            lock_module.load_lock = lambda _path: ({}, False)  # type: ignore[attr-defined]

            def fake_run(command, *, timeout=5.0):
                if len(command) > 1 and command[1].startswith("-print-file-name="):
                    return 0, f"/synthetic/{command[1].split('=', 1)[1]}"
                return 1, "not installed"

            with mock.patch.object(psp_readiness, "ROOT", root), \
                 mock.patch.object(psp_readiness.sys, "platform", platform), \
                 mock.patch.object(psp_readiness.sys, "path", list(sys.path)), \
                 mock.patch.object(psp_readiness.shutil, "which", side_effect=lambda name: tools.get(name)), \
                 mock.patch.object(psp_readiness, "_git_dirty", return_value=False), \
                 mock.patch.object(psp_readiness, "_run", side_effect=fake_run), \
                 mock.patch.object(psp_readiness, "_wsl_pspdev_tools", return_value=set()), \
                 mock.patch.object(psp_readiness, "_client_dir_tools", return_value=set()), \
                 mock.patch.object(psp_readiness, "_ppsspp_headless", return_value=None), \
                 mock.patch.object(psp_readiness, "_psplink_device", return_value=(False, "offline")), \
                 mock.patch.dict(os.environ, {
                     psp_readiness.ENV_CLIENT_DIR: "",
                     psp_readiness.ENV_PPSSPP_HEADLESS: "",
                 }), \
                 mock.patch.dict(sys.modules, {"pspdev_lock": lock_module}):
                report: dict = psp_readiness.collect()
                return report

    def test_linux_host_tools_use_current_python_and_make(self) -> None:
        report = self.collect_on_synthetic_root(
            platform="linux",
            tools={"git": "/usr/bin/git", "gcc": "/usr/bin/gcc", "make": "/usr/bin/make"},
            with_hst=True,
        )
        checks = {check["name"]: check for check in report["checks"]}
        self.assertTrue(report["software_ready"], report["software_failures"])
        self.assertEqual(checks["host_tool:python"]["status"], "PASS")
        self.assertEqual(checks["host_tool:make"]["status"], "PASS")
        self.assertNotIn("host_tool:mingw32-make", checks)

    def test_missing_private_hst_build_is_optional_and_not_a_software_failure(self) -> None:
        report = self.collect_on_synthetic_root(
            platform="win32",
            tools={
                "python": "python.exe",
                "git": "git.exe",
                "gcc": "gcc.exe",
                "mingw32-make": "mingw32-make.exe",
            },
            with_hst=False,
        )
        checks = {check["name"]: check for check in report["checks"]}
        self.assertEqual(checks["hst_buildfull_artifact"]["status"], "OPTIONAL_MISSING")
        self.assertTrue(report["software_ready"], report["software_failures"])
        self.assertNotIn("hst_buildfull_artifact", report["software_failures"])


class ClientDirDiscoveryTests(unittest.TestCase):
    """PSPLINK PC clients are found via an operator-configured directory."""

    def test_unset_environment_finds_nothing(self) -> None:
        with mock.patch.dict(os.environ, {psp_readiness.ENV_CLIENT_DIR: ""}, clear=False):
            self.assertEqual(psp_readiness._client_dir_tools(), set())

    def test_missing_directory_is_not_an_error(self) -> None:
        with mock.patch.dict(
            os.environ, {psp_readiness.ENV_CLIENT_DIR: str(Path(tempfile.gettempdir()) / "no-such-dir-xyz")}
        ):
            self.assertEqual(psp_readiness._client_dir_tools(), set())

    def test_windows_and_bare_client_names_are_both_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            (directory / "usbhostfs_pc.exe").write_bytes(b"")
            (directory / "pspsh").write_bytes(b"")
            with mock.patch.dict(os.environ, {psp_readiness.ENV_CLIENT_DIR: raw}):
                self.assertEqual(psp_readiness._client_dir_tools(), {"usbhostfs_pc", "pspsh"})


class PpssppHeadlessDiscoveryTests(unittest.TestCase):
    def test_explicit_override_must_exist(self) -> None:
        with mock.patch.dict(os.environ, {psp_readiness.ENV_PPSSPP_HEADLESS: "/definitely/not/here.exe"}):
            self.assertIsNone(psp_readiness._ppsspp_headless())

    def test_explicit_override_is_returned_when_present(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            candidate = Path(raw) / "PPSSPPHeadless.exe"
            candidate.write_bytes(b"")
            with mock.patch.dict(os.environ, {psp_readiness.ENV_PPSSPP_HEADLESS: str(candidate)}):
                self.assertEqual(psp_readiness._ppsspp_headless(), candidate)


class PsplinkDeviceDetectionTests(unittest.TestCase):
    """Tool presence must never be mistaken for a reachable device."""

    def setUp(self) -> None:
        # The tests fake sys.platform as win32; the real host lookup would take
        # shutil.which's Windows path on this machine, so pin the PowerShell host.
        patcher = mock.patch.object(psp_readiness, "powershell_exe", return_value="pwsh-default")
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_no_endpoint_reports_not_connected(self) -> None:
        with mock.patch.object(psp_readiness.sys, "platform", "win32"), \
             mock.patch.object(psp_readiness, "_run", return_value=(0, "0")):
            connected, detail = psp_readiness._psplink_device()
        self.assertFalse(connected)
        self.assertIn("mass-storage mode does not count", detail)

    def test_present_endpoint_reports_connected(self) -> None:
        with mock.patch.object(psp_readiness.sys, "platform", "win32"), \
             mock.patch.object(psp_readiness, "_run", return_value=(0, "1")):
            connected, _detail = psp_readiness._psplink_device()
        self.assertTrue(connected)

    def test_query_failure_is_treated_as_not_connected(self) -> None:
        with mock.patch.object(psp_readiness.sys, "platform", "win32"), \
             mock.patch.object(psp_readiness, "_run", return_value=(1, "")):
            connected, _detail = psp_readiness._psplink_device()
        self.assertFalse(connected)

    def test_non_windows_does_not_assume_a_device(self) -> None:
        with mock.patch.object(psp_readiness.sys, "platform", "linux"):
            connected, detail = psp_readiness._psplink_device()
        self.assertFalse(connected)
        self.assertIn("host0", detail)

    def test_query_uses_an_array_subexpression(self) -> None:
        """Regression: the probe silently reported "not connected" forever.

        `psp_readiness` shells out to `powershell.exe`, i.e. Windows PowerShell
        5.1, where a single pipeline object has no `.Count` property and the
        expression evaluates to $null. The original query lacked `@(...)`, so
        both the zero-match and the one-match case produced empty output and the
        check could never report a connected device -- a false negative that
        looks correct because the common case really is disconnected. Mocks
        cannot catch this, so the command text itself is asserted.
        """

        captured: dict[str, list[str]] = {}

        def fake_run(command, *, timeout=5.0):
            captured["command"] = command
            return 0, "1"

        with mock.patch.object(psp_readiness.sys, "platform", "win32"), \
             mock.patch.object(psp_readiness, "_run", fake_run):
            psp_readiness._psplink_device()

        script = captured["command"][-1]
        self.assertIn("@(", script, "query must force an array before reading .Count")
        self.assertIn("VID_054C&PID_01C9", script)
        self.assertNotIn("PID_02D2", script, "mass-storage mode must not count as a link")

    def test_wsl_usbip_attach_is_detected_after_windows_pnp_disappears(self) -> None:
        calls: list[list[str]] = []

        def fake_run(command, *, timeout=5.0):
            calls.append(command)
            if command[0] == "pwsh-test":
                return 0, "0"
            return 0, "Bus 001 Device 004: ID 054c:01c9 Sony Corp. PSP Type B"

        with mock.patch.object(psp_readiness.sys, "platform", "win32"), \
             mock.patch.object(psp_readiness, "_run", fake_run), \
             mock.patch.object(psp_readiness, "powershell_exe", return_value="pwsh-test"), \
             mock.patch.object(psp_readiness.shutil, "which", return_value="wsl.exe"):
            connected, detail = psp_readiness._psplink_device()

        self.assertTrue(connected)
        self.assertIn("via WSL lsusb", detail)
        self.assertTrue(any(command[-1] == "lsusb 2>/dev/null" for command in calls))

    def test_early_psplink_line_is_found_in_long_lsusb_output(self) -> None:
        output = "Bus 001 Device 004: ID 054c:01c9 Sony Corp. PSP Type B\n" + "x" * 2100
        completed = subprocess.CompletedProcess(["wsl"], 0, output, "")
        which = lambda name: "wsl.exe" if name == "wsl.exe" else None
        with mock.patch.object(psp_readiness.shutil, "which", side_effect=which), \
             mock.patch.object(psp_readiness.subprocess, "run", return_value=completed):
            connected, detail = psp_readiness._psplink_wsl_device()
        self.assertTrue(connected, detail)


if __name__ == "__main__":
    unittest.main()
