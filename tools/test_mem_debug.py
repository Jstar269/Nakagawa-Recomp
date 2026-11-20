#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Unit tests for tools/mem_debug.py (issue #180).

These tests exercise the pure helpers (guest-span validation, region
classification, host-offset mapping, PE image identity, process-candidate
selection, argument parsing) and the mutation gate + simulation behavior of
MemoryDebugger.  No test writes to or suspends a real process; on Windows,
LiveDiscoveryTest discovers a child Python process this file starts and reads
its mapped PE headers, read-only.
"""

import base64
from collections.abc import Mapping
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import mem_debug as md

REPO_MOCK_STATE = md.get_mock_state_path()


def _stat_or_none(path):
    """(size, mtime_ns) of ``path``, or None when it does not exist."""
    try:
        status = os.stat(path)
    except FileNotFoundError:
        return None
    return status.st_size, status.st_mtime_ns


class RedirectedMockState:
    """Every simulated debugger built by a test reads and writes a temporary mock state for
    the whole test, never the repository's build/hst/mock_debug_state.json."""

    def setUp(self):
        super().setUp()
        directory = tempfile.TemporaryDirectory(prefix="mem_debug_mock_")
        self.addCleanup(directory.cleanup)
        self.mock_state_path = os.path.join(directory.name, "mock_debug_state.json")
        patcher = mock.patch.object(md, "get_mock_state_path", return_value=self.mock_state_path)
        patcher.start()
        self.addCleanup(patcher.stop)


class ExecutablePathResolutionTest(unittest.TestCase):
    """The attached executable is resolved before any tool is spawned."""

    def test_nm_receives_an_absolute_executable_path(self):
        with tempfile.TemporaryDirectory(prefix="mem_debug_exe_") as tmp_dir:
            exe_dir = Path(tmp_dir)
            (exe_dir / "game.exe").write_bytes(b"MZ-stub")
            captured = {}

            def fake_run(argv, **kwargs):
                argv = [str(arg) for arg in argv]
                if "--version" in argv:
                    return subprocess.CompletedProcess(argv, 0, stdout="nm", stderr="")
                captured["argv"] = argv
                return subprocess.CompletedProcess(argv, 0, stdout="", stderr="")

            previous_cwd = os.getcwd()
            try:
                os.chdir(exe_dir)
                with mock.patch.dict(os.environ, {"NM": "nm-fake"}):
                    with mock.patch.object(md.subprocess, "run", side_effect=fake_run):
                        _rvas, provenance = md.get_symbol_rvas("game.exe")
            finally:
                os.chdir(previous_cwd)
            self.assertEqual(provenance, "fallback")  # empty nm output
            self.assertEqual(captured["argv"][0], "nm-fake")
            self.assertTrue(os.path.isabs(captured["argv"][1]))
            self.assertEqual(
                os.path.normpath(captured["argv"][1]),
                os.path.normpath(str(exe_dir / "game.exe")),
            )

    NM_SYMBOLS = "0000000010001000 B g_mem\n0000000010002000 B s_cpu\n"

    def _fake_nm(self, calls, version_rc=0):
        def fake_run(argv, **kwargs):
            argv = [str(arg) for arg in argv]
            calls.append(argv)
            if "--version" in argv:
                return subprocess.CompletedProcess(
                    argv, version_rc, stdout="GNU nm (GNU Binutils) 2.45\n", stderr="")
            return subprocess.CompletedProcess(argv, 0, stdout=self.NM_SYMBOLS, stderr="")
        return fake_run

    def _pe_exe(self, folder, image_base):
        path = os.path.join(folder, "game.exe")
        Path(path).write_bytes(PEImageIdentityTest._pe_page(image_base, mapped=False))
        return path

    def test_rvas_are_relative_to_the_executables_own_image_base(self):
        with tempfile.TemporaryDirectory(prefix="mem_debug_exe_") as tmp_dir:
            exe = self._pe_exe(tmp_dir, 0x10000000)
            calls = []
            with mock.patch.dict(os.environ, {"NM": "nm-fake"}):
                with mock.patch.object(md.subprocess, "run", side_effect=self._fake_nm(calls)):
                    rvas, provenance = md.get_symbol_rvas(exe)
            self.assertEqual(provenance, "nm")
            self.assertEqual((rvas["g_mem"], rvas["s_cpu"]), (0x1000, 0x2000))

    def test_an_nm_in_the_working_directory_or_on_a_relative_path_is_never_run(self):
        with tempfile.TemporaryDirectory(prefix="mem_debug_cwd_") as tmp_dir:
            exe = self._pe_exe(tmp_dir, 0x10000000)
            for planted in ("nm", "nm.exe"):
                Path(tmp_dir, planted).write_bytes(b"planted")
            env = {"PATH": tmp_dir + os.pathsep + ".",
                   "MSYS_PATH": os.path.join(tmp_dir, "no-msys")}
            calls = []
            previous_cwd = os.getcwd()
            try:
                os.chdir(tmp_dir)
                with mock.patch.dict(os.environ, env):
                    os.environ.pop("NM", None)
                    with mock.patch.object(md.subprocess, "run", side_effect=self._fake_nm(calls)):
                        _rvas, provenance = md.get_symbol_rvas(exe)
            finally:
                os.chdir(previous_cwd)
            self.assertEqual(provenance, "fallback")
            self.assertEqual(calls, [])

    def test_an_nm_that_fails_its_version_probe_is_not_trusted(self):
        with tempfile.TemporaryDirectory(prefix="mem_debug_exe_") as tmp_dir:
            exe = self._pe_exe(tmp_dir, 0x10000000)
            calls = []
            with mock.patch.dict(os.environ, {"NM": "nm-fake"}):
                with mock.patch.object(md.subprocess, "run",
                                       side_effect=self._fake_nm(calls, version_rc=1)):
                    _rvas, provenance = md.get_symbol_rvas(exe)
            self.assertEqual(provenance, "fallback")
            self.assertEqual([argv for argv in calls if "--version" not in argv], [])


class GuestSpanValidationTest(unittest.TestCase):
    def test_valid_ram_span(self):
        ok, region = md.guest_span_validate(0x08800000, 16, md.MAX_READ_BYTES)
        self.assertTrue(ok)
        self.assertEqual(region, "ram")

    def test_exact_arena_end_is_valid(self):
        # Last 4 bytes of RAM: [0x0BFFFFFFC, 0x0C000000) is fully inside.
        ok, _ = md.guest_span_validate(0x0BFFFFFC, 4, md.MAX_READ_BYTES)
        self.assertTrue(ok)
        # One byte past the arena end must fail.
        ok, _ = md.guest_span_validate(0x0BFFFFFD, 4, md.MAX_READ_BYTES)
        self.assertFalse(ok)
        ok, _ = md.guest_span_validate(0x0C000000, 1, md.MAX_READ_BYTES)
        self.assertFalse(ok)

    def test_below_ram_base_is_unsupported(self):
        # 0x08000000 is the RAM base; anything below is unmapped arena.
        ok, reason = md.guest_span_validate(0x07FFFFFF, 1, md.MAX_READ_BYTES)
        self.assertFalse(ok)
        self.assertIn("unsupported", reason)

    def test_vram_span(self):
        ok, region = md.guest_span_validate(0x04000000, 0x00200000, md.MAX_READ_BYTES)
        self.assertTrue(ok)
        self.assertEqual(region, "vram")
        # Past the 2 MiB VRAM window -> unmapped arena.
        ok, _ = md.guest_span_validate(0x04200000, 1, md.MAX_READ_BYTES)
        self.assertFalse(ok)

    def test_scratchpad_span(self):
        ok, region = md.guest_span_validate(0x00010000, 0x1000, md.MAX_READ_BYTES)
        self.assertTrue(ok)
        self.assertEqual(region, "scratchpad")
        ok, _ = md.guest_span_validate(0x00011000, 1, md.MAX_READ_BYTES)
        self.assertFalse(ok)

    def test_wraparound_guest_address(self):
        # 0xFFFFFFFF aliases to phys 0x1FFFFFFF, outside the arena.
        ok, reason = md.guest_span_validate(0xFFFFFFFF, 4, md.MAX_READ_BYTES)
        self.assertFalse(ok)
        self.assertIn("outside", reason)

    def test_oversized_span_rejected(self):
        ok, reason = md.guest_span_validate(0x08800000, md.MAX_READ_BYTES + 1,
                                            md.MAX_READ_BYTES)
        self.assertFalse(ok)
        self.assertIn("budget", reason)
        ok, _ = md.guest_span_validate(0x08800000, 1, md.MAX_READ_BYTES)
        self.assertTrue(ok)

    def test_zero_and_negative_sizes_rejected(self):
        ok, reason = md.guest_span_validate(0x08800000, 0, md.MAX_READ_BYTES)
        self.assertFalse(ok)
        self.assertIn("zero-size", reason)
        ok, reason = md.guest_span_validate(0x08800000, -4, md.MAX_READ_BYTES)
        self.assertFalse(ok)
        self.assertIn("negative", reason)

    def test_non_integer_size_rejected(self):
        ok, _ = md.guest_span_validate(0x08800000, True, md.MAX_READ_BYTES)
        self.assertFalse(ok)
        ok, _ = md.guest_span_validate(0x08800000, "16", md.MAX_READ_BYTES)
        self.assertFalse(ok)

    def test_region_boundary_straddle_rejected(self):
        # Span starting inside VRAM (0x041FFFFC) and crossing past its 2 MiB
        # window into unmapped arena must fail.
        ok, reason = md.guest_span_validate(0x041FFFFC, 8, md.MAX_READ_BYTES)
        self.assertFalse(ok)
        self.assertIn("boundary", reason)
        # A span starting in unmapped arena is rejected outright.
        ok, reason = md.guest_span_validate(0x07FFFFFC, 8, md.MAX_READ_BYTES)
        self.assertFalse(ok)
        self.assertIn("unsupported", reason)

    def test_supported_regions_disjoint(self):
        self.assertNotEqual(md.guest_region(0x08800000), md.guest_region(0x04000000))
        self.assertEqual(md.guest_region(0x08800000), "ram")
        self.assertEqual(md.guest_region(0x04000000), "vram")
        self.assertEqual(md.guest_region(0x00010000), "scratchpad")
        self.assertEqual(md.guest_region(0x00000000), "unmapped-arena")
        self.assertEqual(md.guest_region(0x2C000000), "out-of-arena")


class HostOffsetTest(unittest.TestCase):
    def test_host_offset_matches_sr_host(self):
        # SR_HOST(a) == g_mem + (SR_PHYS(a) - SR_RAM_BASE)
        g_mem = 0x0000000141234000  # g_mem points at guest RAM base
        self.assertEqual(md.host_offset_for_guest(g_mem, 0x08800000),
                         g_mem + 0x00800000)
        # VRAM (0x04000000) maps BELOW g_mem, matching the runtime arena.
        self.assertEqual(md.host_offset_for_guest(g_mem, 0x04000000),
                         g_mem - 0x04000000)

    def test_aliased_guest_high_bits_masked(self):
        g_mem = 0x1000
        self.assertEqual(md.host_offset_for_guest(g_mem, 0x88800000),
                         md.host_offset_for_guest(g_mem, 0x08800000))


class PEImageIdentityTest(unittest.TestCase):
    def test_identity_matches(self):
        self.assertTrue(md.image_identity_matches(b"abc", b"abc"))
        self.assertFalse(md.image_identity_matches(b"abc", b"abd"))
        self.assertFalse(md.image_identity_matches(b"abc", b"abcd"))
        self.assertFalse(md.image_identity_matches(None, b"abc"))
        self.assertFalse(md.image_identity_matches(b"abc", None))

    def test_sha256_file(self):
        with tempfile.NamedTemporaryFile(delete=False, suffix=".bin") as f:
            f.write(b"hello world")
            path = f.name
        try:
            digest = md.sha256_file(path)
            self.assertEqual(
                digest,
                "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9")
            self.assertIsNone(md.sha256_file(os.path.join(path, "missing")))
        finally:
            os.unlink(path)

    @staticmethod
    def _pe_page(image_base, *, mapped, size_of_headers=0x400, stamp=0x5F000000):
        """One synthetic PE32+ first page: on disk, raw section data follows the headers;
        mapped, the rest of the page is zero and the loader has rebased ImageBase."""
        page = bytearray(md.PE_PREFIX_BYTES)
        page[0:2] = b"MZ"
        page[0x3C:0x40] = (0x80).to_bytes(4, "little")
        page[0x80:0x84] = b"PE\0\0"
        page[0x84:0x86] = (0x8664).to_bytes(2, "little")
        page[0x88:0x8C] = stamp.to_bytes(4, "little")
        optional = 0x80 + 24
        page[optional:optional + 2] = (0x20B).to_bytes(2, "little")
        page[optional + 24:optional + 32] = image_base.to_bytes(8, "little")
        page[optional + 60:optional + 64] = size_of_headers.to_bytes(4, "little")
        if not mapped:
            for offset in range(size_of_headers, len(page)):
                page[offset] = (offset * 7 + 3) & 0xFF
        return bytes(page)

    def test_identity_compares_the_mapped_headers_not_the_whole_page(self):
        # MinGW and MSVC images use SizeOfHeaders 0x400: on disk the rest of the first
        # page is section data, mapped it is zero, and ASLR rewrites ImageBase.
        disk = self._pe_page(0x140000000, mapped=False)
        memory = self._pe_page(0x7FF712340000, mapped=True)
        self.assertTrue(md.image_identity_matches(disk, memory))
        other_build = self._pe_page(0x7FF712340000, mapped=True, stamp=0x5F000001)
        self.assertFalse(md.image_identity_matches(disk, other_build))
        self.assertFalse(md.image_identity_matches(disk, b"MZ" + bytes(md.PE_PREFIX_BYTES - 2)))

    def test_pe_prefix_bytes(self):
        with tempfile.NamedTemporaryFile(delete=False, suffix=".exe") as f:
            f.write(b"X" * 0x2000)
            path = f.name
        try:
            prefix = md.pe_prefix_bytes(path)
            self.assertEqual(len(prefix), md.PE_PREFIX_BYTES)
            self.assertEqual(md.pe_prefix_bytes(os.path.join(path, "missing")), None)
        finally:
            os.unlink(path)


class ProcessCandidateSelectionTest(unittest.TestCase):
    def test_unique_exact_path(self):
        candidates = [
            {"pid": 100, "exe_path": r"C:\repo\build\hst\hst.exe", "base_address": 0x140000000},
            {"pid": 200, "exe_path": r"C:\other\not_hst.exe", "base_address": 0x140000000},
        ]
        chosen, reason = md.select_process_candidate(
            candidates, r"C:\repo\build\hst\hst.exe")
        self.assertIsNotNone(chosen)
        self.assertEqual(chosen["pid"], 100)
        self.assertEqual(reason, "unique-exact-path")

    def test_case_and_slash_normalization(self):
        # Windows-normalize case-insensitive exe names and both slash styles;
        # POSIX paths are case-sensitive and forward-slash by convention.
        candidates = [
            {"pid": 300, "exe_path": "c:/repo/build/hst/HST.EXE", "base_address": 0},
        ]
        expected = r"C:\repo\build\hst\hst.exe"
        if os.name == "nt":
            chosen, _ = md.select_process_candidate(candidates, expected)
            self.assertIsNotNone(chosen)
            self.assertEqual(chosen["pid"], 300)
        else:
            # POSIX: backslash is an ordinary filename character; a Windows-
            # style expected path cannot match a POSIX candidate.  Instead
            # assert equivalent POSIX spellings match exactly.
            posix_candidates = [
                {"pid": 301, "exe_path": "build/hst/hst.exe", "base_address": 0},
            ]
            chosen, _ = md.select_process_candidate(posix_candidates, "build/hst/hst.exe")
            self.assertIsNotNone(chosen)
            self.assertEqual(chosen["pid"], 301)
            chosen, _ = md.select_process_candidate(posix_candidates, "./build/hst/hst.exe")
            self.assertIsNotNone(chosen)
            self.assertEqual(chosen["pid"], 301)

    def test_ambiguous_exact_matches(self):
        candidates = [
            {"pid": 100, "exe_path": r"C:\repo\build\hst\hst.exe", "base_address": 0},
            {"pid": 101, "exe_path": r"C:\repo\build\hst\hst.exe", "base_address": 0},
        ]
        chosen, reason = md.select_process_candidate(
            candidates, r"C:\repo\build\hst\hst.exe")
        self.assertIsNone(chosen)
        self.assertIn("ambiguous", reason)

    def test_name_only_match_not_selected(self):
        # A process named hst.exe elsewhere must NOT be auto-selected.
        candidates = [
            {"pid": 400, "exe_path": r"C:\elsewhere\hst.exe", "base_address": 0},
        ]
        chosen, reason = md.select_process_candidate(
            candidates, r"C:\repo\build\hst\hst.exe")
        self.assertIsNone(chosen)
        self.assertIn("no process matches", reason)

    def test_empty_candidates(self):
        chosen, reason = md.select_process_candidate([], r"C:\repo\build\hst\hst.exe")
        self.assertIsNone(chosen)
        self.assertIn("no process matches", reason)

    def test_missing_expected_path(self):
        chosen, reason = md.select_process_candidate([], None)
        self.assertIsNone(chosen)
        self.assertIn("no expected", reason)


class ParseUint32Test(unittest.TestCase):
    def test_valid_forms(self):
        self.assertEqual(md.parse_uint32_arg("0"), 0)
        self.assertEqual(md.parse_uint32_arg("0x0"), 0)
        self.assertEqual(md.parse_uint32_arg("0xFFFFFFFF"), 0xFFFFFFFF)
        self.assertEqual(md.parse_uint32_arg("4294967295"), 0xFFFFFFFF)
        self.assertEqual(md.parse_uint32_arg("0x08800000"), 0x08800000)

    def test_rejects_junk_and_out_of_range(self):
        with self.assertRaises(ValueError):
            md.parse_uint32_arg("0x100junk")
        with self.assertRaises(ValueError):
            md.parse_uint32_arg("123junk")
        with self.assertRaises(ValueError):
            md.parse_uint32_arg("-1")
        with self.assertRaises(ValueError):
            md.parse_uint32_arg("0x100000000")
        with self.assertRaises(ValueError):
            md.parse_uint32_arg("4294967296")
        with self.assertRaises(ValueError):
            md.parse_uint32_arg("")
        with self.assertRaises(ValueError):
            md.parse_uint32_arg("0x")
        with self.assertRaises(ValueError):
            md.parse_uint32_arg("3.5")


class MutationGateTest(RedirectedMockState, unittest.TestCase):
    """The fail-closed mutation gate, exercised without any live process."""

    def _dbg(self, **overrides):
        # simulate=True keeps the constructor away from live processes; the
        # gate tests then override the attributes the gate reads.
        dbg = md.MemoryDebugger(simulate=True, mutate=True)
        attrs = {
            "is_simulated": False,
            "is_offline": False,
            "candidate": {"pid": 1, "exe_path": "x", "base_address": 0x140000000},
            "image_verified": True,
            "rva_provenance": "nm",
            "mutate_enabled": True,
        }
        attrs.update(overrides)
        for k, v in attrs.items():
            setattr(dbg, k, v)
        return dbg

    def test_simulated_debuggers_use_a_temporary_mock_state(self):
        self.assertNotEqual(os.path.normcase(os.path.abspath(md.get_mock_state_path())),
                            os.path.normcase(os.path.abspath(REPO_MOCK_STATE)))

    def test_mutation_requires_flag(self):
        dbg = self._dbg(mutate_enabled=False)
        ok, reason = dbg._mutation_allowed("write_mem", needs_rvas=True, needs_image=True)
        self.assertFalse(ok)
        self.assertIn("--mutate", reason)

    def test_mutation_requires_identified_process(self):
        dbg = self._dbg(candidate=None, is_offline=True)
        ok, _ = dbg._mutation_allowed("write_mem", needs_rvas=True, needs_image=True)
        self.assertFalse(ok)

    def test_mutation_requires_verified_image(self):
        dbg = self._dbg(image_verified=False)
        ok, reason = dbg._mutation_allowed("write_mem", needs_rvas=True, needs_image=True)
        self.assertFalse(ok)
        self.assertIn("image identity", reason)

    def test_mutation_refused_on_fallback_rvas(self):
        dbg = self._dbg(rva_provenance="fallback")
        ok, reason = dbg._mutation_allowed("write_mem", needs_rvas=True, needs_image=True)
        self.assertFalse(ok)
        self.assertIn("fallback", reason)

    def test_mutation_allowed_when_all_verified(self):
        dbg = self._dbg()
        ok, _ = dbg._mutation_allowed("write_mem", needs_rvas=True, needs_image=True)
        self.assertTrue(ok)

    def test_non_mutating_action_rejected_by_gate(self):
        dbg = self._dbg()
        ok, _ = dbg._mutation_allowed("read_mem", needs_rvas=False, needs_image=False)
        self.assertFalse(ok)

    def test_simulation_never_requires_live_identity(self):
        dbg = md.MemoryDebugger(simulate=True, mutate=True)
        ok, _ = dbg._mutation_allowed("write_mem", needs_rvas=True, needs_image=True)
        self.assertTrue(ok)

    def test_simulation_without_mutate_still_refused(self):
        dbg = md.MemoryDebugger(simulate=True, mutate=False)
        ok, reason = dbg._mutation_allowed("write_mem", needs_rvas=True, needs_image=True)
        self.assertFalse(ok)
        self.assertIn("--mutate", reason)


class SimulationBehaviorTest(RedirectedMockState, unittest.TestCase):
    # The redirect lasts the whole test: save_mock_state looks the path up on every
    # write, so restoring it after construction sent the writes to the repository.
    def _sim(self, mutate=False):
        return md.MemoryDebugger(simulate=True, mutate=mutate)

    def test_simulated_mutations_never_touch_the_repository_mock_state(self):
        before = _stat_or_none(REPO_MOCK_STATE)
        dbg = self._sim(mutate=True)
        self.assertTrue(dbg.write_mem(0x08800000, "deadbeef")["success"])
        self.assertTrue(dbg.write_cpu("r1", 1)["success"])
        self.assertTrue(dbg.pause()["success"])
        self.assertEqual(_stat_or_none(REPO_MOCK_STATE), before)

    def test_read_mem_valid_span(self):
        dbg = self._sim()
        res = dbg.read_mem(0x08800000, 4, "hex")
        self.assertIn("hex", res)
        self.assertEqual(res["size"], 4)

    def test_read_mem_invalid_span_rejected(self):
        dbg = self._sim()
        res = dbg.read_mem(0x0C000000, 4, "hex")
        self.assertIn("error", res)
        self.assertIn("guest span rejected", res["error"])

    def test_write_mem_requires_mutate_even_in_simulation(self):
        dbg = self._sim(mutate=False)
        res = dbg.write_mem(0x08800000, "deadbeef")
        self.assertFalse(res["success"])
        self.assertIn("--mutate", res["error"])

    def test_write_mem_round_trip_in_simulation(self):
        dbg = self._sim(mutate=True)
        res = dbg.write_mem(0x08800000, "deadbeef")
        self.assertTrue(res["success"])
        self.assertEqual(res["bytes_written"], 4)
        rd = dbg.read_mem(0x08800000, 4, "hex")
        self.assertEqual(rd["hex"].replace(" ", ""), "deadbeef")

    def test_write_mem_zero_bytes_refused(self):
        dbg = self._sim(mutate=True)
        res = dbg.write_mem(0x08800000, "")
        self.assertFalse(res["success"])
        self.assertIn("zero-byte", res["error"])

    def test_write_mem_invalid_hex_refused(self):
        dbg = self._sim(mutate=True)
        res = dbg.write_mem(0x08800000, "zzzz")
        self.assertFalse(res["success"])
        self.assertIn("Invalid hex", res["error"])

    def test_write_mem_invalid_span_rejected(self):
        dbg = self._sim(mutate=True)
        res = dbg.write_mem(0x0BFFFFFE, "0102030405")  # crosses arena end
        self.assertFalse(res["success"])
        self.assertIn("guest span rejected", res["error"])

    def test_pause_resume_require_mutate(self):
        dbg = self._sim(mutate=False)
        self.assertFalse(dbg.pause()["success"])
        self.assertFalse(dbg.resume()["success"])

    def test_status_reports_mutation_and_provenance(self):
        dbg = self._sim(mutate=True)
        status = dbg.get_status()
        self.assertEqual(status["mode"], "simulation")
        self.assertEqual(status["mutation"], "enabled")
        self.assertEqual(status["rva_provenance"], "fallback")

    def test_simulated_cpu_uses_cpustate_abi_v3_tail(self):
        dbg = self._sim(mutate=True)
        cpu = dbg.read_cpu()["cpu"]
        self.assertEqual(len(cpu["cop0"]), 32)
        self.assertIn("flow_kind", cpu)
        self.assertIn("flow_target", cpu)
        self.assertIn("llbit", cpu)

        self.assertEqual(md._cpu_field_offset("cop0[0]"), (852, False))
        self.assertEqual(md._cpu_field_offset("cop0[12]"), (900, False))
        self.assertEqual(md._cpu_field_offset("next_pc"), (980, False))
        self.assertEqual(md._cpu_field_offset("in_delay_slot"), (984, False))
        self.assertEqual(md._cpu_field_offset("flow_kind"), (988, False))
        self.assertEqual(md._cpu_field_offset("flow_target"), (992, False))
        self.assertEqual(md._cpu_field_offset("llbit"), (996, False))
        self.assertEqual(md.CPU_STATE_ABI_VERSION, 3)
        self.assertEqual(md.CPU_STATE_SIZE, 1000)

    def test_simulated_read_mem_and_vram_are_sparse_and_exact(self):
        class _CountingMemory(Mapping):
            def __init__(self, backing):
                self._backing = dict(backing)
                self.lookups = 0

            def __getitem__(self, key):
                self.lookups += 1
                return self._backing[key]

            def __iter__(self):
                return iter(self._backing)

            def __len__(self):
                return len(self._backing)

        dbg = self._sim(mutate=True)
        expected_banner = b"Hot Shots Tennis: Get a Grip - Interactive Debugging Active."
        expected_ram = bytearray(0x100004)
        expected_ram[:len(expected_banner)] = expected_banner
        expected_ram[0x100000:0x100004] = b"\x78\x56\x34\x12"

        # Default mock state: large RAM span and full 2 MiB VRAM read iterate only stored keys.
        counting = _CountingMemory(dbg.mock["memory"])
        dbg.mock["memory"] = counting
        ram_res = dbg.read_mem(0x08800000, len(expected_ram), "hex")
        self.assertEqual(bytes.fromhex(ram_res["hex"]), bytes(expected_ram))
        self.assertLessEqual(counting.lookups, len(counting))

        counting.lookups = 0
        vram_res = dbg.read_vram()
        self.assertEqual(vram_res["size"], 2 * 1024 * 1024)
        self.assertEqual(base64.b64decode(vram_res["base64"]), bytes(2 * 1024 * 1024))
        self.assertLessEqual(counting.lookups, len(counting))

        # Small read (size <= len(memory)) iterates only the requested span.
        counting.lookups = 0
        small_res = dbg.read_mem(0x08800000, 4, "hex")
        self.assertEqual(bytes.fromhex(small_res["hex"]), expected_banner[:4])
        self.assertLessEqual(counting.lookups, 4)

        # Written VRAM bytes appear at the right offset and non-canonical keys are ignored.
        dbg.mock["memory"] = dict(counting._backing)
        self.assertTrue(dbg.write_mem(0x04000100, "aabbccdd")["success"])
        dbg.mock["memory"]["not-an-address"] = 0xFF
        dbg.mock["memory"][f"0{0x08800000}"] = 0xEE
        counting = _CountingMemory(dbg.mock["memory"])
        dbg.mock["memory"] = counting

        vram_after = dbg.read_vram()
        expected_vram = bytearray(2 * 1024 * 1024)
        expected_vram[0x100:0x104] = b"\xaa\xbb\xcc\xdd"
        self.assertEqual(base64.b64decode(vram_after["base64"]), bytes(expected_vram))
        self.assertLessEqual(counting.lookups, len(counting))

        counting.lookups = 0
        banner_span = dbg.read_mem(0x08800000, 128, "hex")
        self.assertEqual(
            bytes.fromhex(banner_span["hex"]),
            expected_banner + bytes(128 - len(expected_banner)),
        )
        self.assertLessEqual(counting.lookups, len(counting))


class ResolverFailureTest(RedirectedMockState, unittest.TestCase):
    def test_guest_memory_resolver_rejects_bad_span(self):
        # No live process is needed: span validation runs before any g_mem read.
        dbg = md.MemoryDebugger(simulate=True, mutate=True)
        dbg.is_simulated = False
        host, err = dbg._guest_memory_resolver(0x0C000000, 4, md.MAX_READ_BYTES)
        self.assertIsNone(host)
        self.assertIn("outside", err)

    def test_guest_memory_resolver_missing_g_mem(self):
        dbg = md.MemoryDebugger(simulate=True, mutate=True)
        dbg.is_simulated = False
        dbg.pid = 0  # no process handle can be opened
        host, err = dbg._guest_memory_resolver(0x08800000, 4, md.MAX_READ_BYTES)
        self.assertIsNone(host)
        self.assertIn("g_mem", err)


class CpuStateAbiUpgradeTest(RedirectedMockState, unittest.TestCase):
    """ABI v2 must never be applied to state produced by a pre-v2 build."""

    def _header(self, abi=md.CPU_STATE_ABI_VERSION, size=md.CPU_STATE_SIZE,
                stack_len=8, fmt=md.CRASH_DUMP_FORMAT):
        words = (fmt, abi, size, 0x09FF0000, stack_len)
        return md.CRASH_DUMP_MAGIC + b"".join(w.to_bytes(4, "little") for w in words)

    def test_versioned_dump_is_split_at_the_header_sizes(self):
        cpu = bytes(range(256)) * 3 + bytes(md.CPU_STATE_SIZE - 768)
        blob = self._header() + cpu + b""
        parsed, err = md.parse_crash_dump(blob)
        self.assertIsNone(err)
        cpu_bytes, base, stack = parsed
        self.assertEqual(cpu_bytes, cpu)
        self.assertEqual(base, 0x09FF0000)
        self.assertEqual(stack, b"")

    def test_headerless_v1_dump_is_rejected(self):
        # ABI v1: 864-byte CpuState followed by the 64 KiB stack snapshot.
        parsed, err = md.parse_crash_dump(bytes(864 + 0x10000))
        self.assertIsNone(parsed)
        self.assertIn("pre-ABI-v2", err)

    def test_other_abi_or_size_is_rejected(self):
        for header in (self._header(abi=1), self._header(size=864)):
            parsed, err = md.parse_crash_dump(header + bytes(md.CPU_STATE_SIZE + 8))
            self.assertIsNone(parsed)
            self.assertIn("ABI", err)

    def test_truncated_body_is_rejected(self):
        parsed, err = md.parse_crash_dump(self._header() + bytes(md.CPU_STATE_SIZE))
        self.assertIsNone(parsed)
        self.assertIn("size", err)

    def test_pre_v2_mock_state_is_migrated(self):
        old = {"status": "running", "memory": {},
               "cpu": {"r": [0] * 32, "pc": 0x08804000, "cop0": [7]}}
        mock = md._migrate_mock_state(old)
        self.assertEqual(len(mock["cpu"]["cop0"]), md.CPU_STATE_COP0_COUNT)
        self.assertEqual(mock["cpu"]["cop0"][0], 7)
        self.assertEqual(mock["cpu"]["next_pc"], 0x08804004)
        for field in ("in_delay_slot", "flow_kind", "flow_target", "llbit"):
            self.assertEqual(mock["cpu"][field], 0)

    def test_migrated_mock_accepts_new_cop0_writes(self):
        fd, path = tempfile.mkstemp(suffix=".json")
        os.close(fd)
        try:
            with open(path, "w", encoding="utf-8") as f:
                f.write('{"status": "running", "memory": {}, "cpu": {"r": [0], "pc": 0}}')
            orig = md.get_mock_state_path
            md.get_mock_state_path = lambda: path
            try:
                dbg = md.MemoryDebugger(simulate=True, mutate=True)
                res = dbg.write_cpu("cop0[12]", 0x10)
            finally:
                md.get_mock_state_path = orig
            self.assertTrue(res["success"], res)
            self.assertEqual(dbg.mock["cpu"]["cop0"][12], 0x10)
        finally:
            os.unlink(path)

    def _live(self, **overrides):
        dbg = md.MemoryDebugger(simulate=True, mutate=True)
        attrs = {
            "is_simulated": False,
            "is_offline": False,
            "candidate": {"pid": 1, "exe_path": "x", "base_address": 0x140000000},
            "base_address": 0x140000000,
            "image_verified": True,
            "rva_provenance": "nm",
            "rvas": {"g_mem": 1, "s_cpu": 2},
        }
        attrs.update(overrides)
        for k, v in attrs.items():
            setattr(dbg, k, v)
        return dbg

    def test_live_cpu_access_requires_exported_abi_marker(self):
        dbg = self._live()
        for res in (dbg.read_cpu(), dbg.write_cpu("cop0[12]", 1)):
            self.assertFalse(res["success"])
            self.assertIn(md.ABI_SYMBOL, res["error"])

    def test_live_cpu_access_rejects_other_abi_version(self):
        dbg = self._live(rvas={"g_mem": 1, "s_cpu": 2, md.ABI_SYMBOL: 3})
        dbg._read_process_bytes = lambda addr, size: (1).to_bytes(4, "little")
        res = dbg.write_cpu("cop0[12]", 1)
        self.assertFalse(res["success"])
        self.assertIn("ABI 1", res["error"])

    def test_live_cpu_abi_check_passes_for_v3(self):
        dbg = self._live(rvas={"g_mem": 1, "s_cpu": 2, md.ABI_SYMBOL: 3})
        dbg._read_process_bytes = lambda addr, size: (3).to_bytes(4, "little")
        self.assertIsNone(dbg._cpu_abi_check())

    def test_live_cpu_abi_check_rejects_v2(self):
        # v2 has no llbit word; reading 1000 bytes from it would run past the struct.
        dbg = self._live(rvas={"g_mem": 1, "s_cpu": 2, md.ABI_SYMBOL: 3})
        dbg._read_process_bytes = lambda addr, size: (2).to_bytes(4, "little")
        self.assertIn("ABI 2", dbg._cpu_abi_check())


class ExeOptionAndDefaultRejectionTest(RedirectedMockState, unittest.TestCase):
    def test_default_without_exe_fails_clearly(self):
        """MemoryDebugger without --simulate or explicit exe must fail clearly instead of assuming HST."""
        with self.assertRaises(ValueError) as ctx:
            md.MemoryDebugger(simulate=False)
        self.assertIn("No target executable", str(ctx.exception))
        self.assertIn("HST", str(ctx.exception))

    def test_explicit_exe_sets_target_identity(self):
        """Passing an explicit exe sets expected_exe and expected_exe_name rather than assuming hst.exe."""
        dbg = md.MemoryDebugger(simulate=True, exe=r"build\custom_title\custom_title.exe")
        self.assertEqual(dbg.expected_exe_name, "custom_title.exe")


class TargetDispositionTest(unittest.TestCase):
    """Issue #368 item 2: ``--exe`` is the explicit disposition.

    The HST default is a named, documented fallback only, and a
    target that does not exist on disk is a clear error.  Every
    path here is a synthetic temp-dir path; no real build output
    is read or written."""

    def test_missing_exe_target_fails_clearly(self):
        with tempfile.TemporaryDirectory(prefix="mem_debug_target_") as tmp_dir:
            missing = os.path.join(tmp_dir, "no_such_title", "no_such_title.exe")
            with self.assertRaises(ValueError) as ctx:
                md.MemoryDebugger(simulate=False, exe=missing)
            self.assertIn("does not exist", str(ctx.exception))
            self.assertIn(missing, str(ctx.exception))

    def test_directory_or_empty_exe_target_fails_clearly(self):
        # A directory exists but is not an executable target.
        with tempfile.TemporaryDirectory(prefix="mem_debug_target_") as tmp_dir:
            with self.assertRaises(ValueError) as ctx:
                md.MemoryDebugger(simulate=False, exe=tmp_dir)
            self.assertIn("does not exist", str(ctx.exception))
        # An empty --exe would resolve to the repository root; it is refused as
        # "no target" before any path is resolved.
        with self.assertRaises(ValueError) as ctx:
            md.MemoryDebugger(simulate=False, exe="")
        self.assertIn("--exe", str(ctx.exception))

    def test_cli_reports_missing_target_clearly(self):
        with tempfile.TemporaryDirectory(prefix="mem_debug_target_") as tmp_dir:
            missing = os.path.join(tmp_dir, "missing_title.exe")
            argv = ["mem_debug.py", "--exe", missing, "status"]
            out = io.StringIO()
            with mock.patch.object(sys, "argv", argv):
                with contextlib.redirect_stdout(out):
                    with self.assertRaises(SystemExit) as ctx:
                        md.main()
            self.assertEqual(ctx.exception.code, 1)
            payload = json.loads(out.getvalue())
            self.assertIn("does not exist", payload["error"])
            self.assertIn(missing, payload["error"])

    def test_existing_synthetic_exe_proceeds_to_offline(self):
        # An existing target passes the existence gate and reports
        # offline (no live process); the HST default is not involved.
        with tempfile.TemporaryDirectory(prefix="mem_debug_target_") as tmp_dir:
            exe = os.path.join(tmp_dir, "synthetic_title.exe")
            Path(exe).write_bytes(b"MZ-stub")
            dbg = md.MemoryDebugger(simulate=False, exe=exe)
            self.assertTrue(dbg.is_offline)
            self.assertEqual(dbg.expected_exe_name, "synthetic_title.exe")

    def test_hst_default_is_a_named_fallback(self):
        # The remaining HST default is a single named constant used
        # only for the simulation mock state, never an attach target.
        self.assertEqual(md.HST_DEFAULT_BUILD_DIR, os.path.join("build", "hst"))
        self.assertTrue(
            md.get_mock_state_path().endswith(
                os.path.join("build", "hst", "mock_debug_state.json")))


def _live_debugger(tmp_dir, **overrides):
    """A debugger in the attached state, without a process: tests replace the host calls."""
    dbg = md.MemoryDebugger(simulate=True, mutate=True)
    attrs = {
        "is_simulated": False,
        "is_offline": False,
        "pid": 4242,
        "candidate": {"pid": 4242, "exe_path": os.path.join(tmp_dir, "title.exe"),
                      "base_address": 0x140000000},
        "proc_info": {"pid": 4242, "exe_path": os.path.join(tmp_dir, "title.exe"),
                      "base_address": 0x140000000},
        "base_address": 0x140000000,
        "image_verified": True,
        "rva_provenance": "nm",
        "rvas": {"g_mem": 1, "s_cpu": 2, md.ABI_SYMBOL: 3},
    }
    attrs.update(overrides)
    for k, v in attrs.items():
        setattr(dbg, k, v)
    return dbg


class _FakeDll:
    def __init__(self, **functions):
        self.__dict__.update(functions)


class LiveResultTest(RedirectedMockState, unittest.TestCase):
    """What a live action reports, with the Windows calls replaced."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="mem_debug_live_")
        self.addCleanup(self._tmp.cleanup)
        self.tmp_dir = self._tmp.name

    def _dlls(self, **by_name):
        def win_dll(name, **_kwargs):
            return by_name[name.lower()]
        return mock.patch.object(md.ctypes, "WinDLL", side_effect=win_dll, create=True)

    def test_live_reads_report_where_g_mem_came_from(self):
        dbg = _live_debugger(self.tmp_dir, rva_provenance="fallback", image_verified=False)
        dbg._resolve_g_mem = lambda: 0x10000000
        dbg._read_process_bytes = lambda addr, size: bytes(size)
        for res in (dbg.read_mem(0x08800000, 4, "hex"), dbg.read_mem(0x08800000, 4, "words"),
                    dbg.read_mem(0x08800000, 4, "string"), dbg.read_vram()):
            self.assertEqual(res["rva_provenance"], "fallback")
            self.assertFalse(res["image_verified"])

    def test_float_register_writes_store_the_given_bit_pattern(self):
        dbg = _live_debugger(self.tmp_dir)
        dbg._cpu_abi_check = lambda: None
        dbg._resolve_cpu_state_addr = lambda: 0x1000
        written = {}
        dbg._write_process_bytes = lambda addr, data: written.update({addr: data}) or True
        res = dbg.write_cpu("f2", 0x3F800000)
        self.assertTrue(res["success"], res)
        self.assertEqual(written[0x1000 + 140 + 2 * 4], (0x3F800000).to_bytes(4, "little"))
        res = dbg.write_cpu("v5", 0xBF800000)
        self.assertTrue(res["success"], res)
        self.assertEqual(written[0x1000 + 276 + 5 * 4], (0xBF800000).to_bytes(4, "little"))

    def test_simulated_float_register_writes_store_the_given_bit_pattern(self):
        mock_path = os.path.join(self.tmp_dir, "mock_debug_state.json")
        with mock.patch.object(md, "get_mock_state_path", return_value=mock_path):
            dbg = md.MemoryDebugger(simulate=True, mutate=True)
            self.assertTrue(dbg.write_cpu("f2", 0x3F800000)["success"])
            self.assertTrue(dbg.write_cpu("v5", 0xBF800000)["success"])
        self.assertEqual(dbg.mock["cpu"]["f"][2], 1.0)
        self.assertEqual(dbg.mock["cpu"]["v"][5], -1.0)

    def test_status_is_offline_when_the_process_cannot_be_opened(self):
        dbg = _live_debugger(self.tmp_dir)
        kernel32 = _FakeDll(OpenProcess=lambda *args: 0, CloseHandle=lambda handle: 1)
        with self._dlls(**{"kernel32.dll": kernel32}):
            self.assertFalse(dbg.get_status()["online"])

    def test_a_failed_suspend_or_resume_is_reported_and_leaves_the_flag(self):
        dbg = _live_debugger(self.tmp_dir)
        access_denied = -0x3FFFFFDE  # NTSTATUS 0xC0000022 as the signed int ctypes returns
        kernel32 = _FakeDll(OpenProcess=lambda *args: 5, CloseHandle=lambda handle: 1)
        ntdll = _FakeDll(NtSuspendProcess=lambda handle: access_denied,
                         NtResumeProcess=lambda handle: access_denied)
        flag = os.path.join(self.tmp_dir, "paused.flag")
        with self._dlls(**{"kernel32.dll": kernel32, "ntdll.dll": ntdll}):
            res = dbg.pause()
            self.assertFalse(res["success"])
            self.assertIn("0xc0000022", res["error"])
            self.assertFalse(os.path.exists(flag))
            Path(flag).write_text("1")
            res = dbg.resume()
            self.assertFalse(res["success"])
            self.assertIn("0xc0000022", res["error"])
            self.assertTrue(os.path.exists(flag))

    def test_a_crash_dump_over_the_read_budget_is_not_read(self):
        dbg = _live_debugger(self.tmp_dir, process_started=None)
        stack = bytes(4096)
        words = (md.CRASH_DUMP_FORMAT, md.CPU_STATE_ABI_VERSION, md.CPU_STATE_SIZE, 0x09FF0000,
                 len(stack))
        header = md.CRASH_DUMP_MAGIC + b"".join(w.to_bytes(4, "little") for w in words)
        Path(self.tmp_dir, "crash_dump.bin").write_bytes(header + bytes(md.CPU_STATE_SIZE) + stack)
        with mock.patch.object(md, "MAX_READ_BYTES", 2048):
            res = dbg.trace_exit()
        self.assertIn("error", res)
        self.assertIn("budget", res["error"])


@unittest.skipUnless(sys.platform == "win32", "process enumeration is the Windows backend")
class ProcessEnumerationTest(unittest.TestCase):
    def test_more_processes_than_the_first_buffer_holds_are_all_seen(self):
        import ctypes
        target = r"C:\titles\title\title.exe"
        pids = list(range(4, 4 + 1500 * 4, 4))
        paths = {pid: r"C:\other\other.exe" for pid in pids}
        paths[pids[10]] = target
        paths[pids[1400]] = target

        def enum_processes(array_ref, size, needed_ref):
            array = array_ref._obj
            count = min(len(pids), size // ctypes.sizeof(ctypes.c_ulong))
            for index in range(count):
                array[index] = pids[index]
            needed_ref._obj.value = count * ctypes.sizeof(ctypes.c_ulong)
            return 1

        def image_name(handle, flags, buffer_ref, size_ref):
            buffer_ref._obj.value = paths[handle]
            return 1

        kernel32 = _FakeDll(OpenProcess=lambda access, inherit, pid: pid,
                            CloseHandle=lambda handle: 1,
                            QueryFullProcessImageNameW=image_name)
        psapi = _FakeDll(EnumProcesses=enum_processes, EnumProcessModules=lambda *args: 0)
        dlls = {"kernel32.dll": kernel32, "psapi.dll": psapi}
        with mock.patch.object(md.ctypes, "WinDLL",
                               side_effect=lambda name, **kwargs: dlls[name.lower()]):
            candidates = md.enumerate_process_candidates("title.exe")
        self.assertEqual(sorted(c["pid"] for c in candidates), [pids[10], pids[1400]])
        chosen, reason = md.select_process_candidate(candidates, target)
        self.assertIsNone(chosen)
        self.assertIn("ambiguous", reason)


@unittest.skipUnless(sys.platform == "win32", "live process discovery is the Windows backend")
class LiveDiscoveryTest(unittest.TestCase):
    """Discovery and the image identity check against a real child process, read-only."""

    def setUp(self):
        self.child = subprocess.Popen(
            [sys.executable, "-c", "import sys; sys.stdin.read()"],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(self._stop_child)

    def _stop_child(self):
        self.child.stdin.close()
        self.child.wait(timeout=60)

    def test_a_running_child_is_discovered_by_name(self):
        import ntpath
        candidates = md.enumerate_process_candidates(ntpath.basename(sys.executable))
        self.assertIn(self.child.pid, [candidate["pid"] for candidate in candidates])

    def test_attaching_by_pid_verifies_the_running_image(self):
        dbg = md.MemoryDebugger(simulate=False, exe=sys.executable, pid=self.child.pid)
        self.assertFalse(dbg.is_offline, dbg.identity_note)
        self.assertTrue(dbg.image_verified)
        self.assertIsNotNone(dbg.process_started)


class ExitArtifactStalenessTest(RedirectedMockState, unittest.TestCase):
    """exited.flag, paused.flag and crash_dump.bin older than the attached process are
    left over from an earlier run of the same build, never this run's state."""

    def _attached(self, tmp_dir, started):
        dbg = md.MemoryDebugger(simulate=True)
        dbg.is_simulated = False
        dbg.is_offline = False
        dbg.pid = 4242
        dbg.proc_info = {"pid": 4242, "exe_path": os.path.join(tmp_dir, "title.exe"),
                         "base_address": 0}
        dbg.process_started = started
        return dbg

    def _artifact(self, tmp_dir, name, mtime, data=b"1"):
        path = os.path.join(tmp_dir, name)
        Path(path).write_bytes(data)
        os.utime(path, (mtime, mtime))
        return path

    def test_an_exit_flag_from_an_earlier_run_is_not_this_runs_status(self):
        with tempfile.TemporaryDirectory(prefix="mem_debug_artifacts_") as tmp_dir:
            started = 1_800_000_000.0
            self._artifact(tmp_dir, "exited.flag", started - 60)
            dbg = self._attached(tmp_dir, started)
            self.assertEqual(dbg._run_status(), ("running", ["exited.flag"]))
            self._artifact(tmp_dir, "paused.flag", started + 5)
            self.assertEqual(dbg._run_status(), ("paused", ["exited.flag"]))
            self._artifact(tmp_dir, "exited.flag", started + 10)
            self.assertEqual(dbg._run_status(), ("exited", []))

    def test_trace_exit_refuses_a_crash_dump_from_an_earlier_run(self):
        with tempfile.TemporaryDirectory(prefix="mem_debug_artifacts_") as tmp_dir:
            started = 1_800_000_000.0
            self._artifact(tmp_dir, "crash_dump.bin", started - 60, b"SRCD")
            res = self._attached(tmp_dir, started).trace_exit()
            self.assertIn("error", res)
            self.assertIn("earlier run", res["error"])


if __name__ == "__main__":
    unittest.main()
