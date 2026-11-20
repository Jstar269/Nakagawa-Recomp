# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for the Win32/portability inventory (tools/portability_inventory.py).

Guards the classification contract: PSP semantic core files must surface as
SEMANTIC_CORE_CONTAMINATION, host backends as BACKEND_EXPECTED, canonical manager
files as MANAGER, and the scan must not produce false positives from
Vulkan object handles (VK_NULL_HANDLE) or produce nondeterministic output.
"""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import portability_inventory as pi


class ClassificationTests(unittest.TestCase):
    def test_core_files_are_semantic_core(self) -> None:
        self.assertEqual(pi.classify_file("src/rt/hle.c")[1], "SEMANTIC_CORE_CONTAMINATION")
        self.assertEqual(pi.classify_file("src/rt/sched.c")[1], "SEMANTIC_CORE_CONTAMINATION")
        self.assertEqual(pi.classify_file("src/rt/sr_coro.c")[1], "SEMANTIC_CORE_CONTAMINATION")
        self.assertEqual(pi.classify_file("src/rt/recomp.c")[1], "SEMANTIC_CORE_CONTAMINATION")

    def test_hle_private_headers_are_semantic_core(self) -> None:
        """The headers every runtime HLE source includes are HLE core, not the src/rt backend default."""
        import hle_sources

        self.assertTrue(hle_sources.HLE_PRIVATE_HEADERS)
        for header in hle_sources.HLE_PRIVATE_HEADERS:
            with self.subTest(header=header):
                self.assertEqual(pi.classify_file(header), ("CORE", "SEMANTIC_CORE_CONTAMINATION"))

    def test_backend_files_are_backend(self) -> None:
        self.assertEqual(pi.classify_file("src/rt/gpu_sdl3vk/sdl3vk.c")[1], "BACKEND_EXPECTED")
        self.assertEqual(pi.classify_file("src/rt/gpu_sdl3vk/ge_gpu.c")[1], "BACKEND_EXPECTED")
        self.assertEqual(pi.classify_file("src/rt/h264_mf.c")[1], "BACKEND_EXPECTED")
        self.assertEqual(pi.classify_file("src/rt/osk_win.c")[1], "BACKEND_EXPECTED")
        self.assertEqual(pi.classify_file("src/rt/gui.c")[1], "BACKEND_EXPECTED")
        self.assertEqual(pi.classify_file("src/rt/atrac3p/libavcodec/atrac.c")[1], "BACKEND_EXPECTED")

    def test_manager_and_build(self) -> None:
        self.assertEqual(pi.classify_file("tools/nk_manager.ps1")[1], "CANONICAL_MANAGER")
        self.assertEqual(pi.classify_file("nk.ps1")[1], "CANONICAL_MANAGER")
        self.assertEqual(pi.classify_file("Makefile")[1], "BUILD_TOOL_ONLY")
        self.assertEqual(pi.classify_file("tools/codegen.py")[1], "BUILD_TOOL_ONLY")

    def test_selftests_are_test_only(self) -> None:
        self.assertEqual(pi.classify_file("tests/native/sched_selftest.c")[1], "TEST_ONLY")
        self.assertEqual(pi.classify_file("tests/native/hle_thread_selftest.c")[1], "TEST_ONLY")


    def test_host_source_areas_are_backend_not_build_tooling(self) -> None:
        """src/core, src/player, src/ui and the PSP companion are host code, not build tools (audit F-e37a9823)."""
        for path, area in (
            ("src/core/nk_iso.c", "HOST_CORE"),
            ("src/core/nk_platform_win32.c", "HOST_CORE"),
            ("src/player/player_host.c", "PLAYER"),
            ("src/ui/nk_ui.c", "UI"),
            ("src/psp_companion/fontcopy.c", "COMPANION"),
        ):
            with self.subTest(path=path):
                self.assertEqual(pi.classify_file(path), (area, "BACKEND_EXPECTED"))
        self.assertEqual(pi.classify_file("src/psp_companion/Makefile"), ("COMPANION", "BUILD_TOOL_ONLY"))

    def test_uncovered_paths_are_unclassified_not_build(self) -> None:
        self.assertEqual(pi.classify_file("src/newarea/x.c"), ("SRC_UNCLASSIFIED", "UNCLASSIFIED"))
        self.assertEqual(pi.classify_file("elsewhere/x.py"), ("OTHER", "UNCLASSIFIED"))

    def test_mk_and_github_files_are_tools(self) -> None:
        """The mk/ and .github/ rules match their files, not only a bare directory (audit F-2fcfc191)."""
        self.assertEqual(pi.classify_file("mk/rules.mk"), ("TOOLS", "BUILD_TOOL_ONLY"))
        self.assertEqual(pi.classify_file(".github/workflows/ci.yml"), ("TOOLS", "BUILD_TOOL_ONLY"))


class ScanTests(unittest.TestCase):
    def test_scan_reads_only_tracked_files(self) -> None:
        """An untracked local file under a scan area is never scanned or quoted (audit F-da671bee)."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            (root / "tools").mkdir()
            (root / "tools" / "tracked.py").write_text("# _WIN32\n", encoding="utf-8")
            (root / "tools" / "scratch.py").write_text("p = 'Z:/someone/private'\n", encoding="utf-8")
            subprocess.run(["git", "-C", str(root), "add", "tools/tracked.py"], check=True)
            with mock.patch.object(pi, "ROOT", root):
                paths = [rec["path"] for rec in pi.scan()]
        self.assertEqual(paths, ["tools/tracked.py"])

    def test_scan_fails_closed_when_git_fails(self) -> None:
        failed = subprocess.CompletedProcess(args=[], returncode=128, stdout=b"", stderr=b"not a git repository")
        with mock.patch.object(pi.subprocess, "run", return_value=failed):
            with self.assertRaisesRegex(RuntimeError, "not a git repository"):
                pi.scan()

    def test_vulkan_handles_are_not_false_positives(self) -> None:
        text = "VkPipeline p = VK_NULL_HANDLE;\nsr_x(s_dev, VK_NULL_HANDLE, 1, &pci, NULL, &p);\n"
        hits = pi.scan_text(text, pi.PATTERNS)
        self.assertEqual(hits, [], "VK_NULL_HANDLE must not be reported as a Win32 HANDLE")

    def test_real_handle_hit_is_found(self) -> None:
        text = "    HANDLE find;\n"
        hits = pi.scan_text(text, pi.PATTERNS)
        self.assertTrue(any(h["label"] == "handle_type" for h in hits))

    def test_live_inventory_has_expected_class_distribution(self) -> None:
        files = pi.scan()
        classes = {f["class"] for f in files}
        self.assertEqual(classes, {"semantic_core", "backend", "build", "tests", "manager"})
        by_path = {f["path"]: f for f in files}
        self.assertEqual(by_path["src/rt/hle.c"]["class"], "semantic_core")
        self.assertEqual(by_path["src/rt/gpu_sdl3vk/sdl3vk.c"]["class"], "backend")
        self.assertEqual(by_path["tools/nk_manager.ps1"]["class"], "manager")

    def test_live_scan_is_deterministic(self) -> None:
        a = json.dumps(pi.scan(), sort_keys=True)
        b = json.dumps(pi.scan(), sort_keys=True)
        self.assertEqual(a, b)


if __name__ == "__main__":
    unittest.main()
