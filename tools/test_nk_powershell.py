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

import nk_cli  # noqa: E402
import nk_powershell  # noqa: E402
import psp_readiness  # noqa: E402
import stage_install  # noqa: E402
from nk_core import report_bundle  # noqa: E402


def _which(table: dict[str, str]):
    return lambda name: table.get(name)


class HostSelectionTests(unittest.TestCase):
    def setUp(self) -> None:
        os.environ.pop(nk_powershell.ENV_OVERRIDE, None)

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
        os.environ.pop(nk_powershell.ENV_OVERRIDE, None)

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


class ModuleAnalysisCacheIsolationTests(unittest.TestCase):
    """Windows PowerShell 5.1 must never write Microsoft/Windows/PowerShell/ModuleAnalysisCache in cwd."""

    # Exercises Windows PowerShell 5.1's real AnalysisCacheData..cctor and Serialize()
    # without waiting out QueueSerialization()'s 13-second background Task.Delay.
    _FLUSH_CACHE_SCRIPT = (
        "$t = [System.Management.Automation.PSObject].Assembly.GetType("
        "'System.Management.Automation.AnalysisCacheData'); "
        "$cd = [System.Management.Automation.PSObject].Assembly.GetType("
        "'System.Management.Automation.AnalysisCache')"
        ".GetField('cacheData', [Reflection.BindingFlags]'Static,NonPublic').GetValue($null); "
        "$loc = $t.GetField('cacheStoreLocation', [Reflection.BindingFlags]'Static,NonPublic').GetValue($null); "
        "$t.GetMethod('Serialize', [Reflection.BindingFlags]'Instance,NonPublic')"
        ".Invoke($cd, @($loc)); "
        "Write-Output $loc"
    )

    @staticmethod
    def _strip_ps_cache_and_module_path(env: dict[str, str]) -> dict[str, str]:
        return {
            k: v for k, v in env.items()
            if k.casefold() not in ("psmoduleanalysiscachepath", "psmodulepath")
        }

    @unittest.skipUnless(os.name == "nt", "Windows PowerShell 5.1 module analysis cache is Windows-only")
    def test_runtime_build_environment_anchors_powershell_cache_outside_cwd(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-ps-cache-build-") as tmp:
            root = Path(tmp)
            stand_in_root = root / "repo"
            stand_in_root.mkdir()
            local_appdata = root / "localappdata"
            local_appdata.mkdir()
            # Same isolated profile shape as test_player_package_route: LOCALAPPDATA exists,
            # USERPROFILE does not exist yet (so SHGetFolderPathW(CSIDL_LOCAL_APPDATA) returns "").
            caller_env = self._strip_ps_cache_and_module_path(dict(os.environ))
            caller_env["LOCALAPPDATA"] = str(local_appdata)
            caller_env["APPDATA"] = str(root / "appdata")
            caller_env["USERPROFILE"] = str(root / "userprofile")
            with mock.patch.dict(os.environ, caller_env, clear=True):
                build_env = nk_cli._runtime_build_environment()
            proc = subprocess.run(
                [nk_powershell.windows_powershell_exe(), "-NoProfile", "-NonInteractive",
                 "-Command", self._FLUSH_CACHE_SCRIPT],
                cwd=stand_in_root,
                env=build_env,
                capture_output=True,
                text=True,
                timeout=60,
                check=False,
            )
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertFalse(
                (stand_in_root / "Microsoft").exists(),
                f"PowerShell wrote Microsoft/ inside cwd; cacheStoreLocation={proc.stdout.strip()!r}",
            )
            cache_path = Path(proc.stdout.strip())
            self.assertTrue(cache_path.is_absolute(), proc.stdout)
            self.assertTrue(cache_path.is_relative_to(local_appdata), proc.stdout)

    @unittest.skipUnless(os.name == "nt", "Windows PowerShell 5.1 module analysis cache is Windows-only")
    def test_stage_install_run_powershell_anchors_cache_outside_cwd(self) -> None:
        for label, make_extra in (
            ("isolated_localappdata", lambda r, lad: {"LOCALAPPDATA": str(lad), "USERPROFILE": str(r / "missing-up")}),
            ("missing_localappdata", lambda _r, _lad: {"LOCALAPPDATA": "", "USERPROFILE": "."}),
        ):
            with self.subTest(case=label), tempfile.TemporaryDirectory(prefix="nk-ps-cache-stage-") as tmp:
                root = Path(tmp)
                stand_in_root = root / "repo"
                stand_in_root.mkdir()
                local_appdata = root / "localappdata"
                local_appdata.mkdir()
                env = self._strip_ps_cache_and_module_path(dict(os.environ))
                env.update(make_extra(root, local_appdata))
                env[nk_powershell.ENV_OVERRIDE] = nk_powershell.windows_powershell_exe()
                previous = os.getcwd()
                os.chdir(stand_in_root)
                try:
                    with mock.patch.dict(os.environ, {nk_powershell.ENV_OVERRIDE: nk_powershell.windows_powershell_exe()}):
                        proc = stage_install._run_powershell(
                            self._FLUSH_CACHE_SCRIPT, "TEST_PS_CACHE", env=env
                        )
                finally:
                    os.chdir(previous)
                self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                self.assertFalse(
                    (stand_in_root / "Microsoft").exists(),
                    f"stage_install._run_powershell created Microsoft/ in cwd ({label}): "
                    f"cacheStoreLocation={proc.stdout.strip()!r}",
                )
                self.assertTrue(Path(proc.stdout.strip()).is_absolute(), proc.stdout)

    @unittest.skipUnless(os.name == "nt", "Windows PowerShell 5.1 module analysis cache is Windows-only")
    def test_powershell_env_unit_cases(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-ps-env-unit-") as tmp:
            root = Path(tmp)
            lad = root / "lad"
            temp_dir = root / "temp"
            explicit = root / "explicit"

            from_lad = nk_powershell.powershell_env({"LOCALAPPDATA": str(lad)})
            self.assertEqual(
                Path(from_lad[nk_powershell.CACHE_PATH_ENV]),
                lad / "Microsoft" / "Windows" / "PowerShell" / "ModuleAnalysisCache",
            )

            from_empty = nk_powershell.powershell_env({"LOCALAPPDATA": "", "TEMP": str(temp_dir)})
            self.assertEqual(
                Path(from_empty[nk_powershell.CACHE_PATH_ENV]),
                temp_dir / "nk-powershell-cache" / "ModuleAnalysisCache",
            )

            from_relative_cache = nk_powershell.powershell_env(
                {"LOCALAPPDATA": str(lad), "psmoduleanalysiscachepath": r"Microsoft\Windows\PowerShell\ModuleAnalysisCache"}
            )
            self.assertEqual(
                Path(from_relative_cache[nk_powershell.CACHE_PATH_ENV]),
                lad / "Microsoft" / "Windows" / "PowerShell" / "ModuleAnalysisCache",
            )
            self.assertNotIn("psmoduleanalysiscachepath", from_relative_cache)

            existing_abs = str(explicit / "ExistingCache")
            kept = nk_powershell.powershell_env(
                {"LOCALAPPDATA": str(lad), nk_powershell.CACHE_PATH_ENV: existing_abs}
            )
            self.assertEqual(kept[nk_powershell.CACHE_PATH_ENV], existing_abs)

            from_cache_dir = nk_powershell.powershell_env({"LOCALAPPDATA": str(lad)}, cache_dir=explicit)
            self.assertEqual(
                Path(from_cache_dir[nk_powershell.CACHE_PATH_ENV]),
                explicit / "ModuleAnalysisCache",
            )

    @unittest.skipUnless(os.name == "nt", "Windows PowerShell 5.1 module analysis cache is Windows-only")
    def test_report_bundle_gpu_lines_anchors_powershell_cache(self) -> None:
        captured: dict[str, object] = {}

        def fake_run(command, *args, **kwargs):
            captured["env"] = kwargs.get("env")
            return subprocess.CompletedProcess(command, 0, stdout="Test GPU | driver 1.0\n", stderr="")

        with tempfile.TemporaryDirectory(prefix="nk-ps-cache-report-") as tmp:
            local_appdata = Path(tmp) / "localappdata"
            local_appdata.mkdir()
            caller_env = self._strip_ps_cache_and_module_path(dict(os.environ))
            caller_env["LOCALAPPDATA"] = str(local_appdata)
            caller_env["USERPROFILE"] = str(Path(tmp) / "missing-userprofile")
            with mock.patch.dict(os.environ, caller_env, clear=True), \
                 mock.patch.object(report_bundle.sys, "platform", "win32"), \
                 mock.patch.object(report_bundle.subprocess, "run", fake_run):
                report_bundle._gpu_lines()
        child_env = captured.get("env")
        self.assertIsInstance(child_env, dict, "report_bundle._gpu_lines must pass an anchored env")
        assert isinstance(child_env, dict)
        cache_value = next(
            (v for k, v in child_env.items() if k.casefold() == "psmoduleanalysiscachepath"),
            "",
        )
        self.assertTrue(cache_value and os.path.isabs(cache_value), f"PSModuleAnalysisCachePath={cache_value!r}")


if __name__ == "__main__":
    unittest.main()
