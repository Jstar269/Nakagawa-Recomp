# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Checks on the Authenticode signing script and the pages that describe it (#1284).

A real signature needs a certificate, so the paths only a signed file reaches are read, not run. The paths
that stop before a signature exists (a refused timestamp, a failing signtool sign or verify, and the restore
of the stage after it) run the script against a throwaway stage with a stub signtool, as do the checks around
signing: a stage that differs from its attested manifest, and a restore that fails.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import stage_runtime_dlls  # noqa: E402
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


@unittest.skipUnless(sys.platform == "win32" and shutil.which("pwsh"), "needs Windows and PowerShell 7")
@unittest.skipUnless(CC, "TOOLCHAIN_MISSING: needs a C compiler (Get-AuthenticodeSignature reads a real PE)")
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
        # The script checks bin/ against this manifest before it signs (SIGN_MANIFEST_ATTESTED), so it attests the
        # two executables as staged; the restore must bring back these exact bytes.
        files = []
        for name in ("nakagawa_player.exe", "production_smoke.exe"):
            data = (self.stage / "bin" / name).read_bytes()
            files.append({"path": name, "kind": "executable", "size": len(data),
                          "sha256": hashlib.sha256(data).hexdigest(), "imports": []})
        manifest = {"format": stage_runtime_dlls.RUNTIME_MANIFEST_FORMAT,
                    "schema_version": stage_runtime_dlls.RUNTIME_MANIFEST_SCHEMA_VERSION,
                    "files": files, "host_libraries": []}
        (self.stage / "bin" / "runtime-manifest.json").write_bytes(json.dumps(manifest).encode("ascii"))
        # The check also reads the licence index; the stage ships no runtime library, so it lists none.
        (self.stage / "bin" / "THIRD_PARTY_NOTICES").mkdir()
        (self.stage / "bin" / "THIRD_PARTY_NOTICES" / "index.json").write_bytes(b'{"components": []}\n')
        self.log = self.work / "signtool.log"
        self.before = self.snapshot()

    def snapshot(self):
        return {path.relative_to(self.stage).as_posix(): path.read_bytes()
                for path in sorted(self.stage.rglob("*")) if path.is_file()}

    def run_script(self, *arguments, sign_exit=0, verify_exit=0):
        env = {**os.environ, "NK_STUB_LOG": str(self.log), "NK_STUB_SIGN_EXIT": str(sign_exit),
               "NK_STUB_VERIFY_EXIT": str(verify_exit), "PYTHON": sys.executable}
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


def _minimal_pe() -> bytes:
    """A tiny unsigned PE32+ image (headers and one section), so Authenticode reports NotSigned."""
    dos = bytearray(64)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 64)
    coff = struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, 240, 0x0022)
    opt = bytearray(240)
    for fmt, offset, value in (("<H", 0, 0x20B), ("<I", 16, 0x1000), ("<I", 20, 0x1000),
                               ("<Q", 24, 0x140000000), ("<I", 32, 0x1000), ("<I", 36, 0x200),
                               ("<H", 40, 6), ("<H", 48, 6), ("<I", 56, 0x2000), ("<I", 60, 0x200),
                               ("<H", 68, 3), ("<Q", 72, 0x100000), ("<Q", 80, 0x1000),
                               ("<Q", 88, 0x100000), ("<Q", 96, 0x1000), ("<I", 108, 16)):
        struct.pack_into(fmt, opt, offset, value)
    section = struct.pack("<8sIIIIIIHHI", b".text", 0x1000, 0x1000, 0x200, 0x200, 0, 0, 0, 0, 0x60000020)
    headers = bytes(dos) + b"PE\0\0" + coff + bytes(opt) + section
    return headers + bytes(0x200 - len(headers)) + b"\xc3" + bytes(0x1FF)


@unittest.skipUnless(os.name == "nt" and shutil.which("pwsh"), "needs Windows Authenticode and PowerShell 7")
class SignReleaseBinariesManifestRunTests(unittest.TestCase):
    """Run the script on a synthetic stage with a stand-in signtool that never signs."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="nk_sign_")
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name)
        self.stage = self.dir / "stage"
        self.bin = self.stage / "bin"
        self.bin.mkdir(parents=True)
        for name in ("nakagawa_player.exe", "production_smoke.exe"):
            (self.bin / name).write_bytes(_minimal_pe())
        self.scratch = self.dir / "tmp"
        self.scratch.mkdir()
        self.marker = self.dir / "signtool-ran"

    def run_script(self, signtool_body: str) -> subprocess.CompletedProcess:
        signtool = self.dir / "signtool.ps1"
        signtool.write_text(f"New-Item -ItemType File -Path '{self.marker}' -Force | Out-Null\n"
                            + signtool_body + "\nexit 1\n", encoding="utf-8")
        env = dict(os.environ, PYTHON=sys.executable, TMP=str(self.scratch), TEMP=str(self.scratch))
        return subprocess.run(["pwsh", "-NoProfile", "-File", str(SCRIPT), "-Stage", str(self.stage),
                               "-Thumbprint", "00", "-TestCertificate", "-SignTool", str(signtool)],
                              capture_output=True, text=True, check=False, env=env, timeout=120)

    def test_a_stage_that_differs_from_its_attested_manifest_is_refused_before_signing(self):
        attested = b"attested runtime library"
        files = [{"path": "SDL3.dll", "kind": "runtime-library", "size": len(attested),
                  "sha256": hashlib.sha256(attested).hexdigest(), "imports": []}]
        for name in ("nakagawa_player.exe", "production_smoke.exe"):
            data = (self.bin / name).read_bytes()
            files.append({"path": name, "kind": "executable", "size": len(data),
                          "sha256": hashlib.sha256(data).hexdigest(), "imports": []})
        manifest = {"format": stage_runtime_dlls.RUNTIME_MANIFEST_FORMAT,
                    "schema_version": stage_runtime_dlls.RUNTIME_MANIFEST_SCHEMA_VERSION,
                    "files": files, "host_libraries": []}
        manifest_bytes = json.dumps(manifest).encode("ascii")
        (self.bin / "runtime-manifest.json").write_bytes(manifest_bytes)
        # A same-name library swapped after staging and attestation.
        (self.bin / "SDL3.dll").write_bytes(b"replaced runtime library")
        result = self.run_script("")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SIGN_MANIFEST_ATTESTED", result.stdout + result.stderr)
        self.assertIn("RUNTIME_FILE_CHANGED", result.stdout + result.stderr)
        self.assertFalse(self.marker.exists(), "signtool ran on a stage that does not match its attestation")
        self.assertEqual((self.bin / "runtime-manifest.json").read_bytes(), manifest_bytes)

    def test_a_failed_restore_keeps_the_backup(self):
        # The stand-in signtool fails, and the backup copy of the first file is gone, so its restore fails too.
        body = (f"Get-ChildItem -Directory -LiteralPath '{self.scratch}' -Filter 'nk-sign-backup-*' | "
                "ForEach-Object { Remove-Item -LiteralPath (Join-Path $_.FullName 'nakagawa_player.exe') }")
        result = self.run_script(body)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(self.marker.exists())
        self.assertIn("SIGN_FAILED", output)
        backups = list(self.scratch.glob("nk-sign-backup-*"))
        self.assertEqual(len(backups), 1, output)
        self.assertTrue((backups[0] / "production_smoke.exe").is_file())
        self.assertIn("could not be restored", output)


if __name__ == "__main__":
    unittest.main()
