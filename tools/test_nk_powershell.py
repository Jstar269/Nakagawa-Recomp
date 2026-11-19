# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""PowerShell host selection: pwsh first, Windows PowerShell 5.1 as the fallback."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import nk_powershell  # noqa: E402
import psp_readiness  # noqa: E402
import stage_install  # noqa: E402
from nk_core import report_bundle  # noqa: E402


def _which(table: dict[str, str]):
    return lambda name: table.get(name)


def _clear_override(test: unittest.TestCase) -> None:
    """Remove the override for one test only; the run's own NK_POWERSHELL comes back after."""
    patcher = mock.patch.dict(os.environ)
    patcher.start()
    test.addCleanup(patcher.stop)
    os.environ.pop(nk_powershell.ENV_OVERRIDE, None)


class OverrideIsolationTests(unittest.TestCase):
    def test_setup_leaves_the_run_override_in_place(self) -> None:
        with mock.patch.dict(os.environ, {nk_powershell.ENV_OVERRIDE: "the run's own host"}):
            for case in (HostSelectionTests("test_override_wins"), CallSiteTests("test_psp_readiness")):
                with self.subTest(case=type(case).__name__):
                    case.setUp()
                    self.assertNotIn(nk_powershell.ENV_OVERRIDE, os.environ)
                    case.doCleanups()
                    self.assertEqual(os.environ.get(nk_powershell.ENV_OVERRIDE), "the run's own host")


class HostSelectionTests(unittest.TestCase):
    def setUp(self) -> None:
        _clear_override(self)

    def test_pwsh_wins_when_installed(self) -> None:
        with mock.patch.object(nk_powershell.shutil, "which",
                               _which({"pwsh": "P7", "powershell": "P5"})):
            self.assertEqual(nk_powershell.powershell_exe(), "P7")

    def test_windows_powershell_when_pwsh_absent(self) -> None:
        with mock.patch.object(nk_powershell.shutil, "which", _which({"powershell": "P5"})):
            self.assertEqual(nk_powershell.powershell_exe(), "P5")

    def test_system32_path_when_nothing_on_path(self) -> None:
        with mock.patch.object(nk_powershell.shutil, "which", _which({})), \
             mock.patch.dict(os.environ, {"SystemRoot": "R"}):
            self.assertEqual(
                Path(nk_powershell.powershell_exe()),
                Path("R") / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe")

    @unittest.skipUnless(os.name == "nt", "only Windows searches the working directory first")
    def test_a_pwsh_in_the_working_directory_is_never_picked(self) -> None:
        with tempfile.TemporaryDirectory() as here, tempfile.TemporaryDirectory() as empty_bin:
            for name in ("pwsh.exe", "powershell.exe"):
                (Path(here) / name).write_bytes(b"MZ placeholder, never run\n")
            env = {"PATH": empty_bin, "PATHEXT": ".COM;.EXE", "SystemRoot": "R"}
            previous = os.getcwd()
            os.chdir(here)
            try:
                with mock.patch.dict(os.environ, env):
                    os.environ.pop("NoDefaultCurrentDirectoryInExePath", None)
                    picked = nk_powershell.powershell_exe()
            finally:
                os.chdir(previous)
        self.assertEqual(
            Path(picked), Path("R") / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe")

    def test_override_wins(self) -> None:
        with mock.patch.object(nk_powershell.shutil, "which", _which({"pwsh": "P7"})), \
             mock.patch.dict(os.environ, {nk_powershell.ENV_OVERRIDE: "custom"}):
            self.assertEqual(nk_powershell.powershell_exe(), "custom")


class CallSiteTests(unittest.TestCase):
    """Every call site builds argv[0] through the helper."""

    def setUp(self) -> None:
        _clear_override(self)

    def _ok(self, calls):
        def run(command, *a, **k):
            calls.append(list(command))
            return subprocess.CompletedProcess(command, 0, stdout="0", stderr="")
        return run

    def test_psp_readiness(self) -> None:
        calls: list[list[str]] = []

        def fake_run(command: list[str], *, timeout: float = 5.0) -> tuple[int, str]:
            calls.append(command)
            return 0, "1"

        with mock.patch.object(nk_powershell.shutil, "which", _which({"pwsh": "P7"})), \
             mock.patch.object(psp_readiness.sys, "platform", "win32"), \
             mock.patch.object(psp_readiness, "_run", fake_run):
            psp_readiness._psplink_device()
        self.assertEqual(calls[0][0], "P7")

    @unittest.skipUnless(os.name == "nt", "shortcut helpers are Windows only")
    def test_stage_install(self) -> None:
        calls: list[list[str]] = []
        link = Path("x.lnk")
        with mock.patch.object(nk_powershell.shutil, "which", _which({"powershell": "P5"})),              mock.patch.object(stage_install.subprocess, "run", self._ok(calls)):
            for call in (lambda: stage_install._special_folder("Desktop", "0"),
                         lambda: stage_install.read_shortcut_app_id(link),
                         lambda: stage_install.read_shortcut_target(link),
                         lambda: stage_install.write_shortcut(link, Path("d"))):
                try:
                    call()
                except Exception:  # noqa: BLE001 - only argv[0] is under test
                    pass
        self.assertEqual(len(calls), 4)
        for command in calls:
            self.assertEqual(command[0], "P5")

    def test_report_bundle_gpu_lines(self) -> None:
        calls: list[list[str]] = []
        with mock.patch.object(nk_powershell.shutil, "which", _which({"pwsh": "P7"})), \
             mock.patch.object(report_bundle.sys, "platform", "win32"), \
             mock.patch.object(report_bundle.subprocess, "run", self._ok(calls)):
            report_bundle._gpu_lines()
        self.assertEqual(calls[0][0], "P7")


if __name__ == "__main__":
    unittest.main()
