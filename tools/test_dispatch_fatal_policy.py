#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for fail-closed dispatch and unknown NID handling.

Fail-closed dispatch is unconditional: a dispatch miss executes only inside
analyzer-owned executable spans (the production interpreter floor) and every
rejection terminates the run. No environment variable can make a miss
permissive; SR_DISPATCH_FATAL was removed as vestigial once that floor landed.

Verifies:
1. Unknown NIDs fail closed: sr_syscall in src/rt/hle.c terminates execution
   (the run end with status 7, or longjmp) when a NID is not registered in the HLE
   table, and never returns fake success (0 / H_OK).
2. Unmapped / non-executable dispatch targets fail closed: dispatch() in src/rt/recomp.c
   terminates via exit(1) on dispatch miss rather than limping onward, and the
   miss path routes to the interpreter floor (dispatch_run_interp).
3. No dispatch-control environment variable exists: nothing in src/rt or
   src/core reads or writes one, and the native launch session (src/core/nk_launch.c)
   injects none into the runtime.
4. The production dispatch() (the real recomp.c and interpreter floor, linked with
   nothing registered) stops an unowned target by name with exit code 1, with no
   environment variable set and regardless of any hostile continuation flag.
5. The production runner (tools/nk_manager.ps1) sets no dispatch control and reports
   the retired SR_UNSAFE_CONTINUE_ON_DISPATCH_MISS escape as inert.
6. Every stop that ends the process with _Exit, which skips atexit, goes through the one run
   end, sr_run_end in src/rt/hle_route.c (audit 2026-11-17, F-1ee85ec7): no runtime source
   outside it calls _Exit or _exit. The run end finishes a video recording with the status it
   exits with (sr_videorec_finish_status, #1273), so record.json never stays INCOMPLETE after a
   stop the runtime chose and its exit_status is the code the stop exits with; it finishes a
   texture dump first (sr_texdump_exit_finish, #1880), so the files the dump's workers hold land
   and its exit line prints; and it runs the rest of the finalizer list the SR_EXIT_AT_VBLANK
   exit established, which the copies at each stop had drifted from.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

try:
    import scratch_dir
except ModuleNotFoundError:
    from tools import scratch_dir

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hle_sources  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RT = ROOT / "src" / "rt"
CORE = ROOT / "src" / "core"
RECOMP_C = ROOT / "src" / "rt" / "recomp.c"
LAUNCH_C = ROOT / "src" / "core" / "nk_launch.c"
MGR = ROOT / "tools" / "nk_manager.ps1"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")


def function_body(source: str, signature: str) -> str:
    """Return the complete text of the function declared by ``signature``.

    The body is delimited by matching the declaration's braces, not by taking a
    fixed-size slice. A byte window silently stops covering the fail-closed block
    as soon as the function grows, which turns a policy invariant into a test of
    how much unrelated code sits above it: sr_syscall gained the runtime PRX
    import-linking path, and the retired 2000-byte window no longer reached
    ``if (!e)``. Brace matching keeps the assertion tied to the policy instead.

    Quoted spans and comments are skipped so that a brace inside a literal or a
    comment cannot end the body early.
    """
    start = source.find(signature)
    if start < 0:
        return ""
    open_brace = source.find("{", start)
    if open_brace < 0:
        return ""
    depth = 0
    i = open_brace
    while i < len(source):
        ch = source[i]
        if ch in "\"'":
            quote, i = ch, i + 1
            while i < len(source) and source[i] != quote:
                i += 2 if source[i] == "\\" else 1
        elif source.startswith("//", i):
            i = source.find("\n", i)
            if i < 0:
                break
        elif source.startswith("/*", i):
            end = source.find("*/", i + 2)
            i = len(source) if end < 0 else end + 1
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return source[start:i + 1]
        i += 1
    return source[start:]


class TestDispatchFatalPolicySource(unittest.TestCase):
    """Source-level invariants verifying fail-closed dispatch and NID handling."""

    def test_hle_unregistered_nid_fails_closed(self):
        """sr_syscall must terminate on unregistered NIDs and never return 0."""
        hle_src = hle_sources.text()
        self.assertIn("uint32_t sr_syscall(CpuState *s, uint32_t nid)", hle_src)
        fn_body = function_body(hle_src, "uint32_t sr_syscall(CpuState *s, uint32_t nid)")
        self.assertIn("if (!e)", fn_body)
        self.assertIn('sr_run_end(7, "an unregistered NID stopped the run")', fn_body)
        self.assertIn("longjmp(g_hle_jmp", fn_body)
        self.assertIn("HLE: unimplemented nid", fn_body)

    def test_unregistered_nid_stop_is_named_for_the_player(self):
        """The unregistered-NID stop writes the player's semantic-boundary line first."""
        hle_src = hle_sources.text()
        fn_body = function_body(hle_src, "uint32_t sr_syscall(CpuState *s, uint32_t nid)")
        record = fn_body.index("hle_record_unsupported_import_stop(nid, nm);")
        self.assertLess(record, fn_body.index("sr_run_end(7,"))
        helper = function_body(
            hle_src,
            "static void hle_record_unsupported_import_stop(uint32_t nid, const char *name)")
        self.assertIn('getenv("SR_BOOT_EVENT_FILE")', helper)
        self.assertIn('"unsupported-import-"', helper)
        self.assertIn(
            r'"BOOT_EVENT phase=stop reason=semantic-boundary boundary=%s issue=%u\n"', helper)
        # Only the player's [a-z0-9-] alphabet reaches the line (player_read_semantic_stop).
        self.assertIn("if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');", helper)
        self.assertIn("if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))) ch = '-';",
                      helper)

    def test_unmodelled_argument_shape_stop_is_named_for_the_player(self):
        """A handler's unmodelled-shape stop names the import for the player, then exits 7."""
        hle_src = hle_sources.text()
        stop = function_body(
            hle_src, "uint32_t hle_unmodelled_stop(const char *import_name, const char *fmt, ...)")
        production = stop.split("#else", 1)[1]
        record = production.index("hle_record_unsupported_import_stop(sr_last_nid, import_name);")
        self.assertLess(record, production.index("sr_run_end(7,"))
        self.assertNotIn("return HLE_KERNEL_ERROR_NOT_IMPLEMENTED", production)

    def test_recomp_dispatch_miss_terminates(self):
        """dispatch() wrapper must terminate via exit(1) on dispatch rejection."""
        recomp_src = RECOMP_C.read_text(encoding="utf-8")
        self.assertIn("void dispatch(CpuState *s, uint32_t target)", recomp_src)
        dispatch_block = re.search(
            r"void\s+dispatch\(CpuState\s*\*s,\s*uint32_t\s+target\)\s*\{(.*?)\}",
            recomp_src,
            re.DOTALL
        )
        self.assertIsNotNone(dispatch_block)
        body = dispatch_block.group(1)
        self.assertIn("exit(1)", body, "dispatch() must call exit(1) on failure")
        self.assertIn("dispatch_try(s, target) < 0", body)

    def test_miss_path_routes_to_interpreter_floor(self):
        """A non-PLT miss must reach dispatch_run_interp, never a permissive return."""
        recomp_src = RECOMP_C.read_text(encoding="utf-8")
        body = function_body(
            recomp_src,
            "static int dispatch_try_with_boundary(\n"
            "    CpuState *s,\n"
            "    uint32_t target,\n"
            "    const SrGuestInterpCallBoundary *call_boundary,\n"
            "    int drain) {",
        )
        self.assertTrue(body, "dispatch_try_with_boundary definition not found")
        self.assertIn("dispatch_run_interp(s, target, call_boundary, drain)", body)
        self.assertIn("SR_PERF_INTERP_DISPATCH_MISS", body)

    def test_no_dispatch_control_environment_variable_exists(self):
        """Dispatch is unconditional: no source may read or write a fatal switch."""
        for directory in (RT, CORE):
            for path in sorted(directory.rglob("*.c")) + sorted(directory.rglob("*.h")):
                text = path.read_text(encoding="utf-8")
                self.assertNotIn("SR_DISPATCH_FATAL", text, str(path))
                self.assertNotIn("SR_UNSAFE_CONTINUE_ON_DISPATCH_MISS", text, str(path))
        launch_src = LAUNCH_C.read_text(encoding="utf-8")
        self.assertNotIn("diagnostic_mode", launch_src)

    def test_hst_manager_dispatch_override_is_inert(self):
        """Manager sets no dispatch control and reports the retired escape honestly."""
        mgr_src = MGR.read_text(encoding="utf-8")
        self.assertNotIn("SR_DISPATCH_FATAL", mgr_src)
        self.assertIn("SR_UNSAFE_CONTINUE_ON_DISPATCH_MISS", mgr_src)
        self.assertIn("unconditionally fail-closed", mgr_src)


FATAL_EXIT_RE = re.compile(r"\b_[Ee]xit\(")
EXIT_ARG_RE = re.compile(r"\b_[Ee]xit\(\s*([^()]*?)\s*\)")
FINISH_STATUS_RE = re.compile(r"\bsr_videorec_finish_status\(\s*\"[^\"]*\"\s*,\s*([^()]*?)\s*\)")
TEXDUMP_FINISH_RE = re.compile(r"\bsr_texdump_exit_finish\s*\(\s*\)\s*;")


def _without_comments(source: str) -> str:
    """Blank out comments and string literals, keeping every offset and newline."""
    out = list(source)
    i = 0
    while i < len(source):
        ch = source[i]
        if ch in "\"'":
            quote, j = ch, i + 1
            while j < len(source) and source[j] != quote and source[j] != "\n":
                j += 2 if source[j] == "\\" else 1
            for k in range(i + 1, min(j, len(source))):
                if out[k] != "\n":
                    out[k] = " "
            i = j + 1
        elif source.startswith("//", i):
            j = source.find("\n", i)
            j = len(source) if j < 0 else j
            out[i:j] = " " * (j - i)
            i = j
        elif source.startswith("/*", i):
            j = source.find("*/", i + 2)
            j = len(source) if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        else:
            i += 1
    return "".join(out)


def _enclosing_block(code: str, at: int) -> str:
    """The text from the innermost unmatched ``{`` before ``at`` up to ``at``."""
    depth = 0
    for i in range(at - 1, -1, -1):
        if code[i] == "}":
            depth += 1
        elif code[i] == "{":
            if depth == 0:
                return code[i:at]
            depth -= 1
    return code[:at]


class TestFatalStopsFinishRecording(unittest.TestCase):
    """A stop that skips atexit must finish the run's recording itself (#1273, #2080 review),
    and does so through the one run end (audit 2026-11-17, F-1ee85ec7)."""

    #: The verifier's own test executable (#1843): it links no HLE source, so it has no run end
    #: to call, and its --hard-exit has no recording, dump or report to finish.
    NOT_THE_RUNTIME = {"verify_harness.c"}

    def test_every_runtime_exit_goes_through_the_run_end(self):
        """No source under src/rt ends the process with _Exit or _exit outside sr_run_end, so no
        stop can skip a finalizer the run end runs (the copies at each stop had drifted: the
        watchdog and the SR_FBDUMP exits skipped the SR_AUDIODUMP close, the PSMF A/V report,
        the presenter release, the SR_SAMPLE_OUT tail and Ace Mode's report)."""
        outside = []
        inside = 0
        for path in sorted(RT.glob("*.c")):
            if path.name in self.NOT_THE_RUNTIME:
                continue
            code = _without_comments(path.read_text(encoding="utf-8"))
            for match in FATAL_EXIT_RE.finditer(code):
                line = code.count("\n", 0, match.start()) + 1
                start = code.rfind("\n_Noreturn void sr_run_end(int status, const char *reason) {",
                                   0, match.start())
                body = function_body(code[start + 1:], "_Noreturn void sr_run_end(") if start >= 0 else ""
                if body and match.start() < start + 1 + len(body):
                    inside += 1
                else:
                    outside.append(f"{path.name}:{line}")
        self.assertEqual(outside, [], "these stops end the process without the run end (sr_run_end)")
        self.assertEqual(inside, 1, "the run end ends the process exactly once, with _Exit(status)")

    def test_the_run_end_finishes_the_recording_with_its_exit_status(self):
        """The run end finishes a texture dump, then the recording with the status it exits with,
        and every finalizer the SR_EXIT_AT_VBLANK exit ran comes before its _Exit."""
        body = function_body(hle_sources.text(), "_Noreturn void sr_run_end(int status, const char *reason) {")
        self.assertTrue(body, "sr_run_end is not defined in the runtime HLE sources")
        code = _without_comments(body)
        finishes = re.findall(r"\bsr_videorec_finish_status\(\s*reason\s*,\s*status\s*\)", code)
        self.assertEqual(len(finishes), 1, "the run end finishes the recording with its own status")
        exit_arg = EXIT_ARG_RE.search(code)
        self.assertIsNotNone(exit_arg)
        self.assertEqual(exit_arg.group(1), "status", "the run end exits with the status it recorded")
        dump = TEXDUMP_FINISH_RE.search(code)
        self.assertIsNotNone(dump, "the run end does not finish a texture dump (sr_texdump_exit_finish)")
        self.assertLess(dump.start(), code.index("sr_videorec_finish_status"),
                        "the texture dump is finished before the recording")
        for step in ("sr_trace_close();", "hle_exit_audio_stats();", "hle_exit_close_audio_dump();",
                     "sr_dump_calls();", "psmf_av_final(reason);", "ge_scene_dump_finish();",
                     "sr_compat_report();", "gui_shutdown();", "sr_perf_shutdown();",
                     "sr_profile_dump_final();", "SR_VERIFY_REPORT_NOW();",
                     "hle_state_sample_close();", "ace_finish();"):
            with self.subTest(step=step):
                self.assertIn(step, code)
                self.assertLess(code.index(step), exit_arg.start(), f"{step} runs after _Exit")
        self.assertLess(code.index("hle_exit_audio_stats();"), code.index("hle_exit_close_audio_dump();"),
                        "the audio statistics print before the SR_AUDIODUMP file is finalised")
        self.assertLess(code.index("gui_shutdown();"), code.index("sr_perf_shutdown();"),
                        "the presenter writes the SR_PERF summary before the shutdown after it")

    def test_every_stop_passes_a_named_reason_and_its_status(self):
        """Each call names its stop for the recording, with the status it exits with; the stops
        this covers today, and a new one is checked the same way."""
        calls = re.findall(r"\bsr_run_end\(\s*([^,()]+?)\s*,\s*\"([^\"]+)\"\s*\)",
                           hle_sources.text())
        self.assertGreaterEqual(len(calls), 11, calls)
        for status, reason in calls:
            with self.subTest(stop=reason):
                self.assertTrue(status and reason.strip(), (status, reason))

    def test_exit_at_vblank_records_a_clean_end(self):
        """SR_EXIT_AT_VBLANK is the documented end of a showcase recording: exit status 0."""
        self.assertIn('sr_run_end(0, "SR_EXIT_AT_VBLANK")', hle_sources.text())

    def test_texdump_finish_pattern(self):
        code = _without_comments("{ /* sr_texdump_exit_finish(); */ sr_texdump_exit_finish (); _Exit(1); }")
        self.assertEqual(len(TEXDUMP_FINISH_RE.findall(code)), 1)

    def test_finish_status_pattern(self):
        code = _without_comments('{ sr_videorec_finish_status("a, b", ROUTE_FAIL_EXIT); _Exit(ROUTE_FAIL_EXIT); }')
        self.assertEqual(FINISH_STATUS_RE.findall(code), ["ROUTE_FAIL_EXIT"])
        self.assertEqual(EXIT_ARG_RE.search(code).group(1), "ROUTE_FAIL_EXIT")

    def test_comments_and_strings_do_not_count(self):
        code = _without_comments('a(); /* _Exit(1) */ b("_exit(2)"); // _Exit(3)\n_Exit(4);')
        self.assertEqual(len(FATAL_EXIT_RE.findall(code)), 1)


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestDispatchFatalPolicyBehavioral(unittest.TestCase):
    """The production dispatch() stops a run with exit code 1 on an unowned target."""

    UNOWNED_TARGET = 0x08900000
    # Registers no function and no executable span, then hands the production dispatch() a
    # target in guest memory. Reaching the puts means the miss returned to its caller.
    HARNESS = """\
#include <stdio.h>
#include <string.h>
#include "recomp.h"
void sr_register_all(void) {{}}
int main(void) {{
    CpuState s;
    memset(&s, 0, sizeof s);
    sr_mem_init();
    s.r[29] = 0x09F00000u;
    s.pc = 0x{target:08x}u;
    dispatch(&s, 0x{target:08x}u);
    puts("DISPATCH_RETURNED");
    return 0;
}}
"""

    def test_dispatch_unmapped_target_exits_code_1(self):
        """Link the real recomp.c and interpreter floor, register nothing, and dispatch.

        The target lies in guest memory but in no registered function and no executable
        span, so dispatch() reports the miss by name, the interpreter floor refuses it, and
        the run ends with exit code 1 instead of returning to the caller. No environment
        variable changes that, a hostile continuation flag included.
        """
        assert CC is not None
        with scratch_dir.TemporaryDirectory(prefix="nk_dispatch_fatal_") as tmp_dir:
            work = Path(tmp_dir)
            harness = work / "dispatch_miss.c"
            harness.write_text(self.HARNESS.format(target=self.UNOWNED_TARGET), encoding="ascii")
            config_dir = work / "title-config"
            config_dir.mkdir()
            gen = subprocess.run(
                [sys.executable, str(ROOT / "tools" / "title_runtime_config.py"),
                 "--output", str(config_dir / "sr_title_config.h")],
                cwd=ROOT, capture_output=True, text=True,
            )
            self.assertEqual(gen.returncode, 0, gen.stdout + gen.stderr)
            exe_path = work / ("dispatch_miss.exe" if os.name == "nt" else "dispatch_miss")
            comp = subprocess.run(
                [CC, "-O0", "-w", "-D_CRT_SECURE_NO_WARNINGS", "-DSR_GATE_BUILD",
                 "-I", str(RT), "-I", str(CORE), "-I", str(config_dir),
                 "-o", str(exe_path), str(harness),
                 *(str(RT / name) for name in (
                     "recomp.c", "guest_interp.c", "cpu_lle.c", "domain_mode.c",
                     "stale_code.c", "vfpu_tables.c", "title_config.c", "ace.c",
                     "videorec.c")),
                 str(CORE / "nk_shared_helpers.c"), str(ROOT / "tools" / "gate_stub.c"),
                 "-lm"],
                cwd=ROOT, capture_output=True, text=True,
            )
            self.assertEqual(comp.returncode, 0, f"Compilation failed: {comp.stdout}{comp.stderr}")
            environments = (
                ("inherited", None),
                ("hostile continuation flags", {
                    **os.environ,
                    "SR_DISPATCH_FATAL": "0",
                    "SR_UNSAFE_CONTINUE_ON_DISPATCH_MISS": "1",
                }),
                ("no environment variable", {"PATH": os.environ.get("PATH", "")}),
            )
            for label, env in environments:
                with self.subTest(environment=label):
                    run = subprocess.run([str(exe_path)], cwd=work, capture_output=True,
                                         text=True, env=env, timeout=60)
                    detail = f"rc={run.returncode} stdout={run.stdout!r} stderr={run.stderr!r}"
                    self.assertEqual(run.returncode, 1, detail)
                    self.assertNotIn("DISPATCH_RETURNED", run.stdout, detail)
                    self.assertIn(
                        f"DISPATCH_MISS_NEW[1]: target=0x{self.UNOWNED_TARGET:08x}",
                        run.stderr, detail)
                    self.assertIn("SEMANTIC_BOUNDARY: target-not-executable;", run.stderr, detail)


if __name__ == "__main__":
    unittest.main()
