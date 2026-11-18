# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Visual-oracle runner guarantees.

Two layers:

* the behavioral half runs ``tools/test_visual_oracle.ps1``, which exercises the real
  helpers against real processes and directories (Windows only -- skipped elsewhere);
* the static half pins the contract in ``tools/nk_manager.ps1`` that those helpers exist to
  enforce, so the manager cannot quietly go back to sleeping a whole deadline or writing
  a second run on top of a first.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hle_sources  # noqa: E402


ROOT = Path(__file__).resolve().parents[1]
MANAGER = ROOT / "tools" / "nk_manager.ps1"
SUPPORT = ROOT / "tools" / "nk_safety.ps1"
PS_TESTS = ROOT / "tools" / "test_visual_oracle.ps1"


def _powershell() -> str | None:
    return shutil.which("pwsh")


def _paren_list(text: str, open_at: int) -> str:
    """Return the contents of the (...) whose opening paren is at `open_at`."""
    depth = 0
    i = open_at
    quote = ""
    while i < len(text):
        c = text[i]
        if quote:
            if c == quote:
                quote = ""
        elif c in "\"'":
            quote = c
        elif c == "#":
            i = text.find("\n", i)
            if i < 0:
                break
            continue
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return text[open_at + 1 : i]
        i += 1
    return ""


def _declared_parameters(source: str) -> dict[str, set[str]]:
    """Map every PowerShell function in `source` to the parameters its param() block declares."""
    declared: dict[str, set[str]] = {}
    for match in re.finditer(r"^function\s+([A-Za-z][\w-]*)", source, re.M):
        name = match.group(1)
        end = source.find("\n}\n", match.end())
        body = source[match.end() : end if end > 0 else len(source)]
        open_at = re.search(r"(?m)^[ \t]*param[ \t]*\(", body)
        if not open_at:
            declared[name] = set()
            continue
        params: set[str] = set()
        for argument in _paren_list(body, body.index("(", open_at.start())).split(","):
            names = re.findall(r"\$(\w+)", re.sub(r"\[[^\]]*\]", "", argument))
            params.update(names)
        declared[name] = params
    return declared


def _call_segments(source: str, names: set[str]) -> list[tuple[str, str]]:
    """Yield (helper name, argument text) for every `Helper -Argument` call in `source`."""
    segments: list[tuple[str, str]] = []
    for match in re.finditer(r"\b(" + "|".join(sorted(map(re.escape, names))) + r")\b", source):
        text = match.end()
        while True:
            newline = source.find("\n", text)
            line = source[text : newline if newline >= 0 else len(source)]
            segments.append((match.group(1), line))
            if not line.rstrip().endswith("`") or newline < 0:
                break
            text = newline + 1
    return segments


def _named_arguments(segment: str) -> set[str]:
    """Return the -Named arguments in one call segment, ignoring strings and comments."""
    line = re.sub(r"#.*$", "", segment, flags=re.M)
    line = re.sub(r"\"[^\"]*\"|'[^']*'", " ", line)
    return set(re.findall(r"(?<![\w-])-([A-Za-z]\w*)\s*:?\s", line))


class VisualOracleBehaviorTests(unittest.TestCase):
    """Run the PowerShell helper tests, which need real processes to be meaningful."""

    @unittest.skipUnless(sys.platform == "win32", "PowerShell helpers are Windows-only")
    def test_safety_and_run_support_helpers(self) -> None:
        shell = _powershell()
        if shell is None:
            self.skipTest("no PowerShell interpreter on PATH")
        proc = subprocess.run(
            [shell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(PS_TESTS)],
            capture_output=True,
            text=True,
            timeout=600,
        )
        self.assertEqual(
            proc.returncode,
            0,
            f"tools/test_visual_oracle.ps1 failed:\n{proc.stdout}\n{proc.stderr}",
        )


# Defines the named functions of tools/nk_manager.ps1 at the probe's script scope. A
# name ending in "?" is defined when the manager has it and skipped otherwise.
_EXTRACT_MANAGER_FUNCTIONS = r"""
$ErrorActionPreference = "Stop"
$ast = [System.Management.Automation.Language.Parser]::ParseFile($NkManagerPath, [ref]$null, [ref]$null)
foreach ($entry in $NkManagerFunctions.Split(",")) {
    $name = $entry.TrimEnd("?")
    $fn = $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name }, $true) | Select-Object -First 1
    if (-not $fn) {
        if ($entry.EndsWith("?")) { continue }
        throw "function $name not found in the manager"
    }
    . ([scriptblock]::Create($fn.Extent.Text))
}
"""


def _manager_probe_script(functions: str, body: str) -> str:
    """A PowerShell script that loads the named manager functions, then runs `body`."""
    return (f"$NkManagerPath = '{MANAGER.as_posix()}'\n$NkSafetyPath = '{SUPPORT.as_posix()}'\n"
            f"$NkManagerFunctions = '{functions}'\n" + _EXTRACT_MANAGER_FUNCTIONS + body)


def _run_manager_functions(test: unittest.TestCase, functions: str, body: str) -> subprocess.CompletedProcess:
    """Run the real manager functions with their collaborators stubbed by `body`."""
    shell = _powershell()
    if shell is None:
        test.skipTest("no PowerShell interpreter on PATH")
    with tempfile.TemporaryDirectory(prefix="nk-manager-fn-") as tmp:
        path = Path(tmp) / "probe.ps1"
        path.write_text(_manager_probe_script(functions, body), encoding="utf-8")
        return subprocess.run(
            [shell, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", str(path)],
            capture_output=True, text=True, timeout=300, check=False,
        )


# A scratch workspace for the launch probes: an executable and an image that exist,
# an oracle trace and the comparison tool. The probe runs with its location there,
# inside a try that ManagerLaunchGuardTests closes by removing the workspace.
_LAUNCH_WORKSPACE = r"""
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("nk-manager-launch-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path (Join-Path $tmp "tools") -Force | Out-Null
try {
Set-Location -LiteralPath $tmp
$script:RepoRoot = $tmp
$script:LogDir = $tmp
$LogDir = $tmp
$ExePath = Join-Path $tmp "title.exe"; Set-Content -LiteralPath $ExePath -Value "stub"
$ImagePath = Join-Path $tmp "title_image.bin"; Set-Content -LiteralPath $ImagePath -Value "stub"
$oracle = Join-Path $tmp "oracle.trace"; Set-Content -LiteralPath $oracle -Value "stub"
Set-Content -LiteralPath (Join-Path $tmp "tools\funcdiff_cmp.py") -Value "stub"
$script:IsRetail = $false
$script:TitleDataRoot = $null
$TitleManifest = $null
$GameIsoPath = $null
function Assert-TitleManagerPrivateBindings { param([switch]$Runtime) }
function Get-NkRunBase { "0" }
function Get-NkRunEntry { "0x08804000" }
"""


class ManagerLaunchGuardTests(unittest.TestCase):
    """Run, VisualOracle and DiffFunc refuse or fail visibly instead of acting on a
    launch that cannot happen or a result that was not produced. The real manager
    functions run with their collaborators stubbed."""

    def _run(self, functions: str, body: str) -> subprocess.CompletedProcess:
        # _LAUNCH_WORKSPACE opens the try that this closes.
        return _run_manager_functions(self, functions, body + r"""
} finally {
    Set-Location -LiteralPath ([System.IO.Path]::GetTempPath())
    Remove-Item -LiteralPath $tmp -Recurse -Force
}
""")

    def test_oracle_refuses_a_running_title_before_touching_the_saves(self) -> None:
        # Sync-SaveBase swaps the live save folder for the baseline. When the title is
        # already running, Run-NkEngine refuses to launch, so the swap must not happen.
        body = _LAUNCH_WORKSPACE + r"""
$route = Join-Path $tmp "route_probe.pad"; Set-Content -LiteralPath $route -Value "stub"
$script:Synced = 0
$script:ArchiveReset = 0
function Test-SafeComponentName { param($Name) $true }
function Get-WorkspaceTargetProcesses { @([pscustomobject]@{ pid = 4242 }) }
function Reset-OracleArchive { param($Path, $AllowedRoot, [switch]$Overwrite) $script:ArchiveReset++; New-Item -ItemType Directory -Path $Path -Force | Out-Null; $true }
function Sync-SaveBase { param($BasePath, $SaveRoot, $ApprovedRoot, $RouteContext, $BuildContext) $script:Synced++; [pscustomobject]@{ Action = "restored" } }
function Read-RouteOutcome { param($StderrPath) [pscustomobject]@{ Kind = "none"; Checkpoints = @(); FailReason = $null } }
function Get-OracleVerdict { param($ReachedExit, $TimedOut, $ExitCode, $CaptureCount, $RequestedVblank, $ObservedVblank, $RouteKind, $RouteFailReason) [pscustomobject]@{ Complete = $true; Reasons = @() } }
$ok = Invoke-VisualOracle -Route $route -ExitAtVblank 5 -SaveBase (Join-Path $tmp "base") -OracleName probe
Write-Output "OK=$ok"
Write-Output "SYNCED=$script:Synced"
Write-Output "ARCHIVE_RESET=$script:ArchiveReset"
"""
        proc = self._run("Invoke-VisualOracle,Run-NkEngine,Test-NkRunPreconditions?", body)
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertIn("already running", proc.stdout, blob)
        self.assertIn("SYNCED=0", proc.stdout, blob)
        self.assertIn("ARCHIVE_RESET=0", proc.stdout, blob)
        self.assertIn("OK=False", proc.stdout, blob)

    def test_a_launch_without_a_disc_image_does_not_inherit_one(self) -> None:
        # PSP_ISO left by an earlier retail run (or exported by the shell) would make
        # the runtime mount another title's disc. Run and DiffFunc both clear it.
        body = _LAUNCH_WORKSPACE + r"""
$script:Seen = @()
function Get-WorkspaceTargetProcesses { @() }
function Start-Process {
    param($FilePath, $ArgumentList, [switch]$PassThru, [switch]$NoNewWindow, [switch]$Wait,
          $RedirectStandardOutput, $RedirectStandardError, $WindowStyle)
    $script:Seen += "[$env:PSP_ISO]"
    [pscustomobject]@{ ExitCode = 1; Id = 0 }
}
function Wait-ProcessOrKill { param($Process, $TimeoutSeconds) [pscustomobject]@{ TimedOut = $false; ExitCode = 1; ElapsedSeconds = 0 } }
function Analyze-RunLogs { param($LogPath) }
$env:PSP_ISO = "another-title.iso"
Run-NkEngine -Profile Standard -NoGui
$env:PSP_ISO = "another-title.iso"
$null = Invoke-DiffFunc -Target f_1234 -Oracle $oracle
$GameIsoPath = "this-title.iso"
$null = Invoke-DiffFunc -Target f_1234 -Oracle $oracle
Write-Output ("SEEN=" + ($script:Seen -join "|"))
"""
        proc = self._run("Run-NkEngine,Invoke-DiffFunc,Set-NkRuntimeFilesystemEnvironment,Test-NkRunPreconditions?", body)
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertIn("SEEN=[]|[]|[this-title.iso]", proc.stdout, blob)

    _DIFF_STUBS = r"""
function Set-NkRuntimeFilesystemEnvironment { param($ResolvedDataRootPath) }
# The runtime stub writes a fresh trace (argument 4) only when told to.
function Start-Process {
    param($FilePath, $ArgumentList, [switch]$PassThru, [switch]$NoNewWindow, [switch]$Wait,
          $RedirectStandardOutput, $RedirectStandardError, $WindowStyle)
    if ($script:WritesTrace) { Set-Content -LiteralPath $ArgumentList[4] -Value "fresh trace" }
    [pscustomobject]@{ ExitCode = $script:RuntimeExit }
}
# funcdiff_cmp.py stand-in: prints $script:CmpLines lines, then exits $script:CmpExit.
function python {
    foreach ($i in 1..$script:CmpLines) { "funcdiff line $i" }
    $global:LASTEXITCODE = $script:CmpExit
}
function Invoke-Case {
    param([string]$Name)
    $global:LASTEXITCODE = 0   # the title planner's exit, which DiffFunc ran after
    $result = Invoke-DiffFunc -Target f_1234 -Oracle $oracle
    Write-Output "$Name=[$result]"
}
"""

    def test_difffunc_never_compares_a_stale_trace(self) -> None:
        # diff_<addr>.trace from an earlier run must not stand in for a runtime that
        # failed or wrote nothing; the comparison would report that old result.
        body = _LAUNCH_WORKSPACE + self._DIFF_STUBS + r"""
$script:CmpLines = 1; $script:CmpExit = 0
Set-Content -LiteralPath "diff_1234.trace" -Value "stale trace from an earlier run"
$script:WritesTrace = $false; $script:RuntimeExit = 2
Invoke-Case -Name RUNTIME_FAILED
Set-Content -LiteralPath "diff_1234.trace" -Value "stale trace from an earlier run"
$script:WritesTrace = $false; $script:RuntimeExit = 0
Invoke-Case -Name NO_TRACE
"""
        proc = self._run("Invoke-DiffFunc", body)
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertNotIn("[PASS]", proc.stdout, blob)
        self.assertIn("RUNTIME_FAILED=[False]", proc.stdout, blob)
        self.assertIn("NO_TRACE=[False]", proc.stdout, blob)

    def test_difffunc_reads_the_comparison_status_after_long_output(self) -> None:
        # Cutting python's output at 20 lines stopped it before it set its exit
        # status, so a failing comparison with a long traceback printed [PASS].
        body = _LAUNCH_WORKSPACE + self._DIFF_STUBS + r"""
$script:WritesTrace = $true; $script:RuntimeExit = 0
$script:CmpLines = 40; $script:CmpExit = 1
Invoke-Case -Name LONG_FAILURE
$script:CmpLines = 3; $script:CmpExit = 0
Invoke-Case -Name MATCH
"""
        proc = self._run("Invoke-DiffFunc", body)
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertIn("[FAIL] funcdiff comparison failed (exit 1)", proc.stdout, blob)
        self.assertIn("LONG_FAILURE=[False]", proc.stdout, blob)
        self.assertEqual(proc.stdout.count("[PASS] funcdiff comparison matched"), 1, blob)
        self.assertIn("MATCH=[True]", proc.stdout, blob)


class ManagerBuildProcessTests(unittest.TestCase):
    """The build-PID ledger and the background make a full build starts."""

    def test_concurrent_managers_keep_every_build_pid_record(self) -> None:
        # One manager registers builds while another rewrites the ledger. Without a
        # lock, a record appended between the rewrite's read and its write is lost,
        # and the build it names can no longer be reaped by identity.
        shell = _powershell()
        if shell is None:
            self.skipTest("no PowerShell interpreter on PATH")
        count = 400
        body = r"""
. $NkSafetyPath
$script:BuildPidFile = $env:NK_TEST_LEDGER
function Get-ProcessIdentityRecord { param([int]$Id) [pscustomobject]@{ pid = $Id; creation_ticks = 1; exe = "make.exe"; cmd = "make all" } }
Set-Content -LiteralPath $env:NK_TEST_READY -Value ready
while (-not (Test-Path -LiteralPath $env:NK_TEST_GO)) { Start-Sleep -Milliseconds 5 }
$failures = 0
for ($i = 0; $i -lt [int]$env:NK_TEST_COUNT; $i++) {
    try {
        if ($env:NK_TEST_MODE -eq "register") { Register-BuildProcess (100000 + $i) } else { Unregister-BuildProcess 99 }
    } catch {
        $failures++
    }
}
Write-Output "LEDGER_FAILURES=$failures"
"""
        with tempfile.TemporaryDirectory(prefix="nk-build-ledger-") as tmp:
            root = Path(tmp)
            script = root / "worker.ps1"
            script.write_text(
                _manager_probe_script("Register-BuildProcess,Unregister-BuildProcess,Invoke-WithBuildPidLock?", body),
                encoding="utf-8",
            )
            ledger = root / ".build_pids"
            go = root / "go"
            workers = []
            for mode in ("register", "rewrite"):
                env = dict(os.environ, NK_TEST_LEDGER=str(ledger), NK_TEST_READY=str(root / f"ready-{mode}"),
                           NK_TEST_GO=str(go), NK_TEST_MODE=mode, NK_TEST_COUNT=str(count))
                workers.append(subprocess.Popen(
                    [shell, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", str(script)],
                    env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                ))
            try:
                deadline = time.monotonic() + 120
                while not all((root / f"ready-{mode}").exists() for mode in ("register", "rewrite")):
                    if time.monotonic() > deadline or any(w.poll() is not None for w in workers):
                        break
                    time.sleep(0.05)
                go.write_text("go", encoding="ascii")
                outputs = [w.communicate(timeout=300)[0] for w in workers]
            finally:
                for worker in workers:
                    if worker.poll() is None:
                        worker.kill()
            for worker, output in zip(workers, outputs, strict=True):
                self.assertEqual(worker.returncode, 0, output)
            records = [json.loads(line) for line in ledger.read_text(encoding="utf-8-sig").splitlines() if line.strip()]
            pids = sorted(int(record["pid"]) for record in records)
            self.assertEqual(len(pids), count, f"{count - len(pids)} build-PID record(s) lost")
            self.assertEqual(pids, list(range(100000, 100000 + count)))
            for output in outputs:
                # A sharing violation between the two writers is the same race.
                self.assertIn("LEDGER_FAILURES=0", output, output)

    def test_an_interrupted_full_build_monitor_stops_its_make(self) -> None:
        # make runs hidden in its own console, so Ctrl+C ends only the monitor loop.
        # The loop's interruption (a throw here, a pipeline stop on Ctrl+C) must stop
        # the build it started instead of leaving it writing the build folder unseen.
        body = r"""
. $NkSafetyPath
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("nk-build-monitor-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tmp | Out-Null
Set-Location -LiteralPath $tmp
$BuildDir = Join-Path $tmp "build"
$ExePath = Join-Path $BuildDir "title.exe"
$LogDir = $tmp
$script:IsRetail = $false
$script:ActiveGameName = "title"
$script:EffectiveJobs = 1
$script:Make = $null
function Assert-TitleManagerPrivateBindings { param([switch]$Runtime) }
function Stop-BuildProcesses { }
function Get-NkMakeBaseArgs { @() }
function Find-MakeExecutable { "make" }
function Write-BuildError { param($Message, $LogFile, $TailLines) }
function Start-Process { param($FilePath, $ArgumentList, [switch]$PassThru, [switch]$NoNewWindow, [switch]$Wait) [pscustomobject]@{ ExitCode = 0 } }
function Start-ScopedMake {
    param([hashtable]$StartProcess)
    $info = [System.Diagnostics.ProcessStartInfo]::new((Get-Process -Id $PID).Path)
    foreach ($a in @("-NoProfile", "-NonInteractive", "-Command", "Start-Sleep -Seconds 120")) { $info.ArgumentList.Add($a) }
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $script:Make = [System.Diagnostics.Process]::Start($info)
    $script:Make
}
function Register-BuildProcess { param([int]$ProcessId) }
function Unregister-BuildProcess { param([int]$ProcessId) $script:Unregistered = $ProcessId }
function Start-Sleep { param([int]$Seconds, [int]$Milliseconds) throw "monitor interrupted" }
try {
    try { $null = Invoke-NkBuild -Mode Full } catch { Write-Output "INTERRUPTED=$($_.Exception.Message)" }
    Write-Output "MAKE_STOPPED=$($script:Make.WaitForExit(15000))"
    Write-Output "UNREGISTERED=$($script:Unregistered -eq $script:Make.Id)"
} finally {
    if ($script:Make -and -not $script:Make.HasExited) { $script:Make.Kill() }
    Set-Location -LiteralPath ([System.IO.Path]::GetTempPath())
    Remove-Item -LiteralPath $tmp -Recurse -Force
}
"""
        proc = _run_manager_functions(self, "Invoke-NkBuild", body)
        blob = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, blob)
        self.assertIn("INTERRUPTED=monitor interrupted", proc.stdout, blob)
        self.assertIn("MAKE_STOPPED=True", proc.stdout, blob)
        self.assertIn("UNREGISTERED=True", proc.stdout, blob)


class VisualOracleContractTests(unittest.TestCase):
    """Static guards on the manager wiring the behavioral tests cannot reach."""

    def setUp(self) -> None:
        self.manager = MANAGER.read_text(encoding="utf-8-sig")
        self.support = SUPPORT.read_text(encoding="utf-8-sig")

    def test_runner_waits_on_the_process_not_the_clock(self) -> None:
        # The original defect: Start-Sleep -Seconds $RunDuration ran to completion even
        # after hst.exe had exited, adding ~50 idle minutes to a long oracle run.
        self.assertNotIn(
            "Start-Sleep -Seconds $RunDuration",
            self.manager,
            "the run deadline must not be an unconditional sleep",
        )
        self.assertIn("Wait-ProcessOrKill -Process $proc", self.manager)

    def test_run_support_is_owned_by_the_canonical_safety_module(self) -> None:
        self.assertIn("nk_safety.ps1", self.manager)
        for fn in ("Wait-ProcessOrKill", "Reset-OracleArchive", "Sync-SaveBase", "Get-OracleVerdict"):
            self.assertIn(f"function {fn}", self.support, f"{fn} must live in the support file")

    def test_oracle_archive_is_reset_before_a_run(self) -> None:
        self.assertIn("Reset-OracleArchive -Path $outDir", self.manager)
        self.assertIn("OverwriteOracle", self.manager)

    def test_oracle_records_a_provenance_manifest(self) -> None:
        # An evidence set that cannot be traced to a build, a route and a verdict is not
        # evidence. Each of these was requested explicitly after #141 review.
        for field in (
            "git_head",
            "exe_sha256",
            "route_sha256",
            "exit_code",
            "capture_count",
            "wall_seconds",
            "requested_vblank",
            "observed_vblank",
            "complete",
        ):
            self.assertIn(field, self.manager, f"manifest must record {field}")
        self.assertIn("oracle_manifest.json", self.manager)

    def test_oracle_supports_profiles_without_a_second_runner(self) -> None:
        # #33 baselines need the Benchmark profile; they must not fork the runner.
        self.assertIn("-RunProfile", self.manager)
        oracle = self.manager[self.manager.index("function Invoke-VisualOracle") :]
        oracle = oracle[: oracle.index("function Invoke-DiffFunc")]
        self.assertIn("Run-NkEngine -Profile $RunProfile", oracle)
        self.assertNotIn(
            "Start-Process",
            oracle,
            "the oracle must go through the engine runner, not launch its own runner",
        )

    def test_manager_binds_only_declared_helper_parameters(self) -> None:
        # A helper called with a parameter it does not declare is a runtime parameter-binding
        # error, not a validation failure: PowerShell aborts the whole script, so the action
        # dies on its first call with a message that names neither the caller nor the action.
        # -Action VisualOracle shipped that way since #196, which left the only scripted-input
        # route in the product unusable.
        declared = _declared_parameters(self.support)
        self.assertIn("Test-SafeComponentName", declared)
        unknown = [
            f"{name} -{param}"
            for name, segment in _call_segments(self.manager, set(declared))
            for param in _named_arguments(segment)
            if param not in declared[name]
        ]
        self.assertEqual(
            [],
            unknown,
            "tools/nk_manager.ps1 binds parameters its tools/nk_safety.ps1 helpers do not declare: "
            + ", ".join(sorted(set(unknown))),
        )

    def test_save_state_can_be_held_still_across_runs(self) -> None:
        # Deterministic inputs are not enough: the game persists a save, so without this two
        # replays of one route are two different experiments.
        self.assertIn("-SaveBase", self.manager)
        self.assertIn("Sync-SaveBase -BasePath $SaveBase", self.manager)
        self.assertIn("save_base_action", self.manager)
        self.assertIn("GAMEDATA", self.support, "the install must be explicitly excluded")

    def test_snap_windows_do_not_overwrite_each_other(self) -> None:
        # The whole point of a two-window capture is comparing both ends of a transition
        # from the same run. The rotating 8-slot name would let the late window overwrite
        # the early one, silently destroying the reference the comparison depends on.
        hle = hle_sources.text()
        self.assertIn('getenv("SR_FBSNAP_WINDOWS")', hle)
        self.assertIn('"snap_v%u.ppm"', hle)
        self.assertIn("SnapWindows", self.manager)
        self.assertIn("SR_FBSNAP_WINDOWS", self.manager)

    def test_exit_at_vblank_is_the_last_statement_of_the_tick(self) -> None:
        # The stop point must be a fully completed vblank tick: the controller sample for
        # vblank V has to be latched before the process exits, or a route's final press
        # can be dropped. Assert placement structurally, not by comment.
        hle = hle_sources.text()
        start = hle.index("void sr_vblank_tick(void) {")
        # The function ends at the first line-start closing brace after it.
        end = hle.index("\n}\n", start)
        body = hle[start:end]
        self.assertIn("SR_EXIT_AT_VBLANK", body, "the exit control must live in the tick")
        self.assertLess(
            body.index("sr_ctrl_sample()"),
            body.index("SR_EXIT_AT_VBLANK"),
            "the controller sample for this vblank must be latched before the exit check",
        )
        tail = body[body.index('getenv("SR_EXIT_AT_VBLANK")'):]
        self.assertNotIn(
            "sr_ctrl_sample",
            tail,
            "no tick work may follow the exit check",
        )
        # Nothing but the closing of the exit block may follow it.
        self.assertIsNotNone(
            re.search(r"_Exit\(0\);\s*\}\s*\}\s*$", body),
            "the exit check must be the final statement of sr_vblank_tick",
        )


if __name__ == "__main__":
    unittest.main()
