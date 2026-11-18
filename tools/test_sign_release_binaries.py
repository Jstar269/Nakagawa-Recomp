# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Checks on the Authenticode signing script and the pages that describe it (#1284).

A real signature needs a certificate, so the paths only a signed file reaches are read, not run. The paths
that stop before a signature exists (a refused timestamp, a failing signtool sign or verify, and the restore
of the stage after it) run the script against a throwaway stage with a stub signtool.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tools" / "sign_release_binaries.ps1"
SIGNING_DOC = ROOT / "docs" / "RELEASE_SIGNING.md"
RELEASE_DOC = ROOT / "docs" / "PREVIEW_RELEASE.md"


class SignReleaseBinariesTests(unittest.TestCase):
    def setUp(self):
        self.script = SCRIPT.read_text(encoding="utf-8")

    @unittest.skipUnless(shutil.which("pwsh"), "PowerShell 7 is not installed")
    def test_script_parses(self):
        # The path travels in the environment, not in the command text: a checkout path holding an
        # apostrophe would end a quoted PowerShell literal early.
        command = ("$e = $null; [System.Management.Automation.Language.Parser]::ParseFile("
                   "$env:NK_PARSE_TARGET, [ref]$null, [ref]$e) | Out-Null; "
                   "if ($e.Count) { $e | ForEach-Object { $_.Message }; exit 1 }")
        result = subprocess.run(["pwsh", "-NoProfile", "-Command", command],
                                env={**os.environ, "NK_PARSE_TARGET": str(SCRIPT)},
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_only_the_two_project_executables_are_signed(self):
        match = re.search(r"\$SignedFiles = @\(([^)]*)\)", self.script)
        self.assertIsNotNone(match)
        names = re.findall(r"'([^']+)'", match.group(1))
        self.assertEqual(names, ["bin/nakagawa_player.exe", "bin/production_smoke.exe"])
        # A demo's completion-manifest.json lists its executable's SHA-256, so a signed demo would be hidden.
        self.assertNotIn("demos", match.group(1))

    def test_no_secret_reaches_a_command_line(self):
        # signtool's /p (password) and /f (key file) put a secret in the process list; the script has neither.
        self.assertIsNone(re.search(r"'/(p|f|csp|kc)'", self.script))
        self.assertNotIn("Read-Host", self.script)
        self.assertNotIn("-AsSecureString", self.script)

    def test_the_paths_only_a_signed_file_reaches_are_pinned_in_the_source(self):
        """Read, not run: each needs a real signature. SignReleaseBinariesRunTests runs the rest."""
        for needle in ("SIGN_ALREADY_SIGNED", "--write-runtime-manifest-only", "--verify-runtime-manifest",
                       "signing-record.json", "refreshed_files", "attested_sha256", "refreshed_sha256"):
            self.assertIn(needle, self.script)

    def test_pages_name_the_script_and_its_place_in_the_recipe(self):
        signing = SIGNING_DOC.read_text(encoding="utf-8")
        release = RELEASE_DOC.read_text(encoding="utf-8")
        self.assertIn("tools/sign_release_binaries.ps1", signing)
        self.assertIn("tools/sign_release_binaries.ps1", release)
        recipe = release.index("sign_release_binaries.ps1")
        self.assertLess(recipe, release.index("python tools/generate_sbom.py"))
        self.assertLess(recipe, release.index("python tools/stage_install.py --portable-zip"))

    def test_signtool_discovery_ignores_unversioned_sdk_folders(self):
        self.assertIn("bin/[0-9][0-9.]*/x64/signtool", self.script)


CC = shutil.which("gcc") or shutil.which("clang") or shutil.which("cc")

# The stub signtool logs each call and changes the file it is asked to sign, so a stage the script failed to
# restore shows in its bytes. Its exit codes come from the environment.
STUB_SIGNTOOL = """\
Add-Content -LiteralPath $env:NK_STUB_LOG -Value ($args -join ' ')
if ($args[0] -eq 'sign') {
    [System.IO.File]::AppendAllText($args[-1], 'changed by the stub signtool')
    exit [int]$env:NK_STUB_SIGN_EXIT
}
exit [int]$env:NK_STUB_VERIFY_EXIT
"""


@unittest.skipUnless(sys.platform == "win32" and shutil.which("pwsh") and CC,
                     "needs Windows, PowerShell 7 and a C compiler (Get-AuthenticodeSignature reads a real PE)")
class SignReleaseBinariesRunTests(unittest.TestCase):
    """Runs the script up to the point a real certificate is needed, and checks what a failure leaves."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="nk_sign_run_"))
        cls.addClassCleanup(shutil.rmtree, cls.tmp, ignore_errors=True)
        source = cls.tmp / "unsigned.c"
        source.write_text("int main(void) { return 0; }\n", encoding="utf-8")
        cls.unsigned_exe = cls.tmp / "unsigned.exe"
        result = subprocess.run([str(CC), "-o", str(cls.unsigned_exe), str(source)],
                                capture_output=True, text=True, check=False)
        if result.returncode != 0:
            raise AssertionError("the unsigned stand-in executable did not compile:\n" + result.stderr)
        cls.stub = cls.tmp / "stub_signtool.ps1"
        cls.stub.write_text(STUB_SIGNTOOL, encoding="utf-8")

    def setUp(self):
        work = tempfile.TemporaryDirectory(prefix="nk_sign_stage_")
        self.addCleanup(work.cleanup)
        self.work = Path(work.name)
        self.stage = self.work / "stage"
        (self.stage / "bin").mkdir(parents=True)
        for name in ("nakagawa_player.exe", "production_smoke.exe"):
            shutil.copyfile(self.unsigned_exe, self.stage / "bin" / name)
        (self.stage / "bin" / "runtime-manifest.json").write_bytes(b'{"attested": true}\n')
        self.log = self.work / "signtool.log"
        self.before = self.snapshot()

    def snapshot(self):
        return {path.relative_to(self.stage).as_posix(): path.read_bytes()
                for path in sorted(self.stage.rglob("*")) if path.is_file()}

    def run_script(self, *arguments, sign_exit=0, verify_exit=0):
        env = {**os.environ, "NK_STUB_LOG": str(self.log), "NK_STUB_SIGN_EXIT": str(sign_exit),
               "NK_STUB_VERIFY_EXIT": str(verify_exit)}
        return subprocess.run(["pwsh", "-NoProfile", "-NonInteractive", "-File", str(SCRIPT),
                               "-Stage", str(self.stage), "-SignTool", str(self.stub), *arguments],
                              env=env, capture_output=True, text=True, check=False, timeout=120)

    def calls(self):
        return self.log.read_text(encoding="utf-8").splitlines() if self.log.exists() else []

    def assert_stage_untouched(self, result):
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertEqual(self.snapshot(), self.before, "the stage was not restored to its unsigned bytes")
        self.assertFalse((self.stage / "signing-record.json").exists(), output)

    def test_a_release_signature_without_a_timestamp_is_refused_before_signtool_runs(self):
        result = self.run_script("-Thumbprint", "00")
        self.assert_stage_untouched(result)
        self.assertIn("SIGN_NO_TIMESTAMP", result.stdout + result.stderr)
        self.assertEqual(self.calls(), [])

    def test_a_failed_sign_restores_the_stage_and_asks_for_an_rfc3161_timestamp(self):
        result = self.run_script("-Thumbprint", "00", "-TimestampUrl", "http://timestamp.invalid", sign_exit=7)
        output = result.stdout + result.stderr
        self.assert_stage_untouched(result)
        self.assertIn("SIGN_FAILED: signtool exited 7 for bin/nakagawa_player.exe", output)
        self.assertIn("restored to its unsigned state", output)
        player = str(self.stage / "bin" / "nakagawa_player.exe")
        self.assertEqual(self.calls(), [
            f"sign /fd sha256 /sha1 00 /tr http://timestamp.invalid /td sha256 {player}"])

    def test_a_failed_verify_restores_the_signed_file(self):
        result = self.run_script("-Thumbprint", "00", "-TimestampUrl", "http://timestamp.invalid",
                                 verify_exit=1)
        output = result.stdout + result.stderr
        self.assert_stage_untouched(result)
        self.assertIn("SIGN_VERIFY: signtool verify /pa failed for bin/nakagawa_player.exe", output)
        player = str(self.stage / "bin" / "nakagawa_player.exe")
        self.assertEqual(self.calls()[1:], [f"verify /pa /all {player}"])


if __name__ == "__main__":
    unittest.main()
