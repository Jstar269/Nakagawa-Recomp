# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Checks on the Authenticode signing script and the pages that describe it (#1284).

Signing needs a certificate and signtool, so most of these tests read the script. The run tests use a
synthetic stage and a stand-in signtool that never signs, to check the steps before and around signing.
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
        command = ("$e = $null; [System.Management.Automation.Language.Parser]::ParseFile("
                   f"'{SCRIPT}', [ref]$null, [ref]$e) | Out-Null; "
                   "if ($e.Count) { $e | ForEach-Object { $_.Message }; exit 1 }")
        result = subprocess.run(["pwsh", "-NoProfile", "-Command", command],
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

    def test_a_release_signature_is_timestamped_and_verified(self):
        for needle in ("'/tr'", "'/td'", "'sha256'", "verify /pa /all", "SIGN_NO_TIMESTAMP", "SIGN_ALREADY_SIGNED",
                       "--write-runtime-manifest-only", "--verify-runtime-manifest", "signing-record.json"):
            self.assertIn(needle, self.script)

    def test_pages_name_the_script_and_its_place_in_the_recipe(self):
        signing = SIGNING_DOC.read_text(encoding="utf-8")
        release = RELEASE_DOC.read_text(encoding="utf-8")
        self.assertIn("tools/sign_release_binaries.ps1", signing)
        self.assertIn("tools/sign_release_binaries.ps1", release)
        recipe = release.index("sign_release_binaries.ps1")
        self.assertLess(recipe, release.index("python tools/generate_sbom.py"))
        self.assertLess(recipe, release.index("python tools/stage_install.py --portable-zip"))

    def test_a_failed_run_restores_the_stage_and_the_record_covers_the_refreshed_manifest(self):
        for needle in ("} catch {", "restored to its unsigned state", "refreshed_files", "attested_sha256",
                       "refreshed_sha256"):
            self.assertIn(needle, self.script)

    def test_signtool_discovery_ignores_unversioned_sdk_folders(self):
        self.assertIn("bin/[0-9][0-9.]*/x64/signtool", self.script)


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
class SignReleaseBinariesRunTests(unittest.TestCase):
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
