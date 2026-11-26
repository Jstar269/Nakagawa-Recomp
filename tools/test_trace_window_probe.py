# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Contract test for the SR_TRACE_PC address-windowed instruction trace.

The full per-instruction trace is one line per guest instruction, which no run
of any length can afford to write.  SR_TRACE_PC narrows it to a guest-pc window
and lets the operator name the registers whose ABSOLUTE value each record must
carry, because a window cannot reconstruct a value written before it.  That is
what makes an out-of-domain VFPU argument traceable back to its inputs (#69).

These assertions pin the contract that makes the window usable and
non-invasive:

* the window gate runs BEFORE the register snapshot, so an armed window costs
  one range compare per guest instruction and the snapshot only for in-window
  instructions;
* with no window armed the trace stream is byte-identical to the un-windowed
  one (the absolute-value block and the record budget are both behind the same
  arming flag), so the oracle/diff builds that compare traces keep working;
* the stream can be armed before guest execution, while per-vblank hooks retain
  the frame markers, so the trace build needs no title-specific code;
* the parse rejects a malformed window and never arms on one.

Most of it is a source-shape test: the hooks live in recomp.c, and the
behavioural evidence for them is profiler-selftest (the one native selftest that
links recomp.c) and the traced run itself. TraceWindowParseTests compiles the
recorder alone and checks how each SR_TRACE_* value arms or refuses the window.
"""

import os
import shutil
import subprocess
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from makefile_text import read_makefile  # noqa: E402

try:
    import scratch_dir
except ModuleNotFoundError:
    from tools import scratch_dir

ROOT = Path(__file__).resolve().parents[1]
RECOMP_C = (ROOT / "src/rt/recomp.c").read_text(encoding="utf-8")
RECOMP_H = (ROOT / "src/rt/recomp.h").read_text(encoding="utf-8")
GE_C = (ROOT / "src/rt/ge.c").read_text(encoding="utf-8")
DRIVER_C = (ROOT / "src/rt/driver.c").read_text(encoding="utf-8")
WINDOW_C = (ROOT / "src/rt/flight_recorder.c").read_text(encoding="utf-8")
WINDOW_H = (ROOT / "src/rt/flight_recorder.h").read_text(encoding="utf-8")
INTERP_C = (ROOT / "src/rt/guest_interp.c").read_text(encoding="utf-8")
CODEGEN_PY = (ROOT / "tools/codegen.py").read_text(encoding="utf-8")
DISPATCH_SELFTEST = (ROOT / "tests/native/dispatch_isolation_selftest.c").read_text(encoding="utf-8")
SCHED_C = (ROOT / "src/rt/sched.c").read_text(encoding="utf-8")
MAKEFILE = read_makefile(ROOT)
NK_CLI = (ROOT / "tools/nk_cli.py").read_text(encoding="utf-8")


def body_of(source: str, signature: str) -> str:
    """Return the brace-balanced body of a C function definition."""
    start = source.index(signature)
    depth = 0
    for i in range(start, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[start:i + 1]
    raise AssertionError(f"unbalanced body for {signature}")


class TraceWindowProbeTests(unittest.TestCase):
    def test_window_is_declared_for_hosts(self):
        for signature in ("void sr_trace_window_configure(void);",
                          "void sr_trace_note_frame(uint32_t frame);",
                          "int  sr_trace_window_armed(void);",
                          "int  sr_trace_window_begin_instruction(uint32_t pc);",
                          "const SrTraceWindowIndices *sr_trace_window_indices(void);",
                          "void sr_trace_window_record(uint32_t pc, const char *text);"):
            self.assertIn(signature, WINDOW_H)
        # A host that does not link the recorder must keep compiling, so the
        # not-linked branch of the header provides inert definitions.
        stubbed = WINDOW_H[WINDOW_H.index("#else"):]
        self.assertIn("static inline int sr_trace_window_armed(void) { return 0; }", stubbed)

    def test_environment_contract(self):
        configure = body_of(WINDOW_C, "void sr_trace_window_configure(void)")
        for name in ("SR_TRACE_PC", "SR_TRACE", "SR_TRACE_V", "SR_TRACE_F",
                     "SR_TRACE_LIMIT"):
            self.assertIn(f'getenv("{name}")', configure,
                          f"{name} must be read by the window probe")
        self.assertIn("strchr(window, ':')", configure)
        self.assertIn("strchr(window, '-')", configure)

    def test_malformed_window_never_arms(self):
        configure = body_of(WINDOW_C, "void sr_trace_window_configure(void)")
        # A window without a separator, or an inverted one, is refused before the
        # stream is opened, so a typo cannot silently produce an empty trace.
        self.assertLess(configure.index("needs LO:HI"), configure.index("fopen"))
        self.assertLess(configure.index("two 32-bit addresses"), configure.index("fopen"))
        self.assertLess(configure.index("empty window"), configure.index("fopen"))
        self.assertLess(configure.index("SR_TRACE_LIMIT needs a record count"),
                        configure.index("fopen"))
        self.assertIn("if (hi < lo)", configure)

    def test_window_is_env_probed_once(self):
        configure = body_of(WINDOW_C, "void sr_trace_window_configure(void)")
        self.assertTrue(configure.strip().startswith("void sr_trace_window_configure(void) {\n"
                                                    "    if (s_tw_configured) return;"))
        self.assertIn("s_tw_configured = 1;", configure)

    def test_gate_precedes_the_register_snapshot(self):
        begin = body_of(RECOMP_C, "void sr_begin_impl(CpuState *s, uint32_t pc, uint32_t op)")
        window_check = begin.index("sr_trace_window_armed()")
        snapshot = begin.index("memcpy(s_r, s->r")
        self.assertLess(window_check, snapshot,
                        "the window test must gate the snapshot, not follow it")
        self.assertIn("if (!s_fp) return;", begin)
        gate = body_of(WINDOW_C, "int sr_trace_window_begin_instruction(uint32_t pc)")
        self.assertIn("pc < s_tw_lo || pc > s_tw_hi", gate)

    def test_out_of_window_records_are_skipped(self):
        begin = body_of(RECOMP_C, "void sr_begin_impl(CpuState *s, uint32_t pc, uint32_t op)")
        self.assertIn("if (sr_trace_window_armed() && !sr_trace_window_begin_instruction(pc)) return;", begin)
        end = body_of(RECOMP_C, "void sr_end_impl(CpuState *s, uint32_t mem_addr, int mem_size)")
        # The end hook reads the begin hook's admission, never the window on the stale
        # s_pc (behaviour: dispatch_isolation_selftest's trace-window child).
        self.assertIn("s_trace_admitted = 1;", begin)
        self.assertIn("if (sr_trace_window_armed() && !s_trace_admitted) return;", end)
        self.assertNotIn("sr_trace_window_begin_instruction(s_pc)", end)
        record = body_of(WINDOW_C, "void sr_trace_window_record(uint32_t pc, const char *text)")
        self.assertIn("if (!s_tw_armed || s_tw_skip || !s_tw_fp) return;", record)

    def test_unarmed_window_changes_no_output(self):
        end = body_of(RECOMP_C, "void sr_end_impl(CpuState *s, uint32_t mem_addr, int mem_size)")
        # The canonical line still goes to the trace stream verbatim unless a
        # window is armed, so the oracle/diff traces keep their exact bytes.
        self.assertEqual(end.count("if (sr_trace_window_armed()) {"), 1)
        self.assertIn("fwrite(line, 1, n + 1, s_fp);", end)
        # register values are uint32_t and print through PRIx32 (format_portability_check.py)
        self.assertIn('abs v%u=0x%08" PRIx32', end)
        self.assertIn('abs f%u=0x%08" PRIx32', end)
        # Spending the record budget completes the window: the gate stays armed and
        # admits nothing more. Disarming it would hand every later instruction to the
        # full trace (the else branch above) and run into the 50M-step abort.
        record = body_of(WINDOW_C, "void sr_trace_window_record(uint32_t pc, const char *text)")
        self.assertIn("if (s_tw_limit && ++s_tw_count >= s_tw_limit)", record)
        self.assertIn("s_tw_complete = 1;", record)
        self.assertNotIn("s_tw_armed = 0", WINDOW_C)
        gate = body_of(WINDOW_C, "int sr_trace_window_begin_instruction(uint32_t pc)")
        self.assertIn("s_tw_skip = s_tw_complete || pc < s_tw_lo || pc > s_tw_hi;", gate)
        # Each record is flushed as it is written, so a host crash keeps it.
        self.assertLess(record.index('fprintf(s_tw_fp, "%s\\n", text);'),
                        record.index("fflush(s_tw_fp);"))
        self.assertLess(record.index("fflush(s_tw_fp);"), record.index("if (s_tw_limit"))

    def test_vblank_hooks_mark_trace_and_watch_timelines(self):
        flight_init = body_of(WINDOW_C, "void sr_flight_init(void)")
        self.assertIn("sr_watch_configure();", flight_init)

        configure = body_of(GE_C, "void ge_set_frame(uint32_t frame)")
        self.assertIn("sr_trace_window_configure();", configure)
        self.assertIn("sr_trace_note_frame(frame);", configure)
        self.assertIn("sr_watch_configure();", configure)
        self.assertIn("sr_watch_note_vblank(frame);", configure)
        # The frame marker is a no-op unless a window is armed.
        note = body_of(WINDOW_C, "void sr_trace_note_frame(uint32_t frame)")
        self.assertIn("if (!s_tw_armed || s_tw_complete || !s_tw_fp) return;", note)

    def test_window_is_armed_before_guest_execution(self):
        main = body_of(DRIVER_C, "int main(int argc, char **argv)")
        trace_open = main.index("sr_trace_open(out")
        configure = main.index("sr_trace_window_configure();")
        guest_start = main.index("sched_run(")
        self.assertLess(trace_open, configure)
        self.assertLess(configure, guest_start,
                        "a guest busy-loop before its first vblank must still be traceable")

    def test_index_list_is_bounded(self):
        listing = body_of(WINDOW_C, "static int sr_trace_window_list(")
        self.assertIn("if (*count >= SR_TRACE_WINDOW_MAX_INDEX)", listing)
        self.assertIn("if (value >= limit)", listing)
        configure = body_of(WINDOW_C, "void sr_trace_window_configure(void)")
        # Each list is bounded by the register file the record reads: v[128], fi[32].
        self.assertIn('sr_trace_window_list("SR_TRACE_V", getenv("SR_TRACE_V"), 128u,', configure)
        self.assertIn('sr_trace_window_list("SR_TRACE_F", getenv("SR_TRACE_F"), 32u,', configure)

    def test_store_watch_parses_an_overflow_safe_range_and_reports_each_overlap(self):
        configure = body_of(WINDOW_C, "void sr_watch_configure(void)")
        self.assertIn('getenv("SR_WATCH")', configure)
        self.assertIn("strchr(spec, ':')", configure)
        self.assertIn("length > (UINT32_MAX + 1ULL) - start", configure)
        self.assertIn("if (s_watch_configured) return;", configure)

        covers = body_of(WINDOW_C, "int sr_watch_covers(")
        self.assertIn("store_start < s_watch_end && store_end > s_watch_start", covers)
        store = body_of(WINDOW_C, "void sr_watch_store(")
        self.assertIn("sr_watch_covers(stored->addr, stored->width)", store)
        # The fields before #2809 keep their order; the context is appended after them.
        self.assertIn(
            '"SR_WATCH: pc=0x%08" PRIx32 " addr=0x%08" PRIx32 " val=0x%08" PRIx32'
            ' " width=%" PRIu32 " vblank=%" PRIu32\n'
            '            " %s thread=0x%08" PRIx32 " old=0x%08" PRIx32 "\\n"',
            store,
        )
        self.assertIn("s_watch_matches++", store)
        # #2809: the hit's thread comes from the source the scheduler registers, so the store
        # helpers need no scheduler symbol and every host that links them still links.
        self.assertIn("s_watch_thread_source ? s_watch_thread_source() : 0u", store)
        self.assertNotIn("sched_current_uid", body_of(RECOMP_H, "static inline void sr_watch_note_store("))
        self.assertIn("sr_watch_set_thread_source(sched_current_uid);",
                      body_of(SCHED_C, "void sched_init(CpuState *cpu)"))
        flight_init = body_of(WINDOW_C, "void sr_flight_init(void)")
        self.assertIn("sr_watch_configure();", flight_init)

    def test_aot_and_interpreter_stores_share_the_watched_pc_aware_helpers(self):
        aot_recorder_flags = [
            line for line in MAKEFILE.splitlines()
            if line.startswith("override RECOMP_FLAGS +=")
        ]
        self.assertTrue(
            any("SR_FLIGHT_RECORDER_LINKED" in line for line in aot_recorder_flags),
            "generated AOT chunks must compile the active recorder hooks, not header stubs",
        )
        self.assertTrue(
            any("SR_INSTRUCTION_TRACE" in line for line in aot_recorder_flags),
            "trace-enabled AOT chunks must keep the instruction trace define",
        )
        runtime_trace_flags = [
            line for line in MAKEFILE.splitlines()
            if line.startswith("override CFLAGS +=")
        ]
        self.assertTrue(
            any("SR_INSTRUCTION_TRACE" in line for line in runtime_trace_flags),
            "trace-enabled interpreter/runtime code must keep the instruction trace define",
        )
        self.assertIn('env["TRACE"] = "1"', NK_CLI)
        self.assertIn('runtime_opt = env.get("RUNTIME_OPT", "-O0")', NK_CLI)
        self.assertIn('env["RUNTIME_OPT"] = f"{runtime_opt} -DSR_INSTRUCTION_TRACE".strip()', NK_CLI)
        self.assertIn(
            "environment = _runtime_build_environment(instruction_trace=instruction_trace)",
            NK_CLI,
        )
        self.assertIn(
            'instruction_trace=bool(getattr(args, "instruction_trace", False))',
            NK_CLI,
        )
        self.assertIn("$(CC) $(RECOMP_FLAGS)", MAKEFILE)

        # Each store helper is an inline main-RAM fast path that runs only while no store
        # instrumentation is armed (sr_store_plain, which includes SR_WATCH_ACTIVE()), and
        # otherwise the *_slow body, which reports the store to the watch.
        plain = body_of(RECOMP_H, "static inline int sr_store_plain(")
        self.assertIn("SR_WATCH_ACTIVE()", plain)
        for helper in ("sr_w8_pc", "sr_w16_pc", "sr_w32_pc"):
            body = body_of(RECOMP_H, f"static inline void {helper}(")
            self.assertIn("sr_store_plain()", body)
            self.assertIn(f"{helper}_slow(a, v, pc);", body)
            slow = body_of(RECOMP_H, f"static inline __attribute__((noinline)) void {helper}_slow(")
            self.assertIn("SR_WATCH_STORE_IF_ARMED(pc, a, v", slow)
            # #2809: the replaced bytes are read before the store lands.
            self.assertLess(slow.index("sr_watch_old_bytes(a, "),
                            slow.index("SR_WATCH_STORE_IF_ARMED(pc, a, v"))

        self.assertIn("MEM_W32_PC(", CODEGEN_PY,
                      "generated AOT stores must keep their guest instruction PC")
        for store in ("MEM_W8_PC", "MEM_W16_PC", "MEM_W32_PC"):
            self.assertIn(store, INTERP_C,
                          "the interpreter store must reach the same watched helper")

        self.assertIn("test_valid_aot_miss_executes_guest_bytes", DISPATCH_SELFTEST)
        self.assertIn("test_aot_store_watch_synthetic_guest", DISPATCH_SELFTEST)
        self.assertIn("sr_watch_match_count()", DISPATCH_SELFTEST)
        self.assertIn("test_ram_store_watch_every_width", DISPATCH_SELFTEST)
        self.assertIn("memory-watch-selftest", MAKEFILE)
        self.assertIn("--watch 0x08801000:4", MAKEFILE)
        self.assertIn('strcmp(argv[1], "--watch") == 0', DISPATCH_SELFTEST)
        self.assertIn("native-core-tests: memory-watch-selftest", MAKEFILE)


PARSE_HARNESS = r"""
#include "flight_recorder.h"
#include <stdio.h>

int main(void) {
    sr_trace_window_configure();
    const SrTraceWindowIndices *indices = sr_trace_window_indices();
    printf("armed=%d v=%u f=%u\n", sr_trace_window_armed(), indices ? indices->vn : 0u,
           indices ? indices->fn : 0u);
    return 0;
}
"""


def _find_compiler() -> str:
    for candidate in (os.environ.get("CC", ""), "gcc", "cc", "clang"):
        word = candidate.split()[0] if candidate.strip() else ""
        path = shutil.which(word) if word else None
        if path:
            return path
    return ""


COMPILER = _find_compiler()


@unittest.skipUnless(COMPILER, "no C compiler on PATH")
class TraceWindowParseTests(unittest.TestCase):
    """The production SR_TRACE_* parse, compiled with the recorder alone."""

    @classmethod
    def setUpClass(cls):
        cls._scratch = scratch_dir.TemporaryDirectory()
        cls.tmp = Path(cls._scratch.name)
        source = cls.tmp / "trace_window_parse.c"
        source.write_text(PARSE_HARNESS, encoding="utf-8")
        cls.executable = cls.tmp / f"trace_window_parse{'.exe' if os.name == 'nt' else ''}"
        result = subprocess.run(
            [COMPILER, "-std=c99", "-O0", "-Wall", "-Wextra", "-Werror", "-I",
             str(ROOT / "src" / "rt"), "-DSR_FLIGHT_RECORDER_LINKED", str(source),
             "src/rt/flight_recorder.c", "-o", str(cls.executable)],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        if result.returncode != 0:
            cls._scratch.cleanup()
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls._scratch.cleanup()

    def _configure(self, **values):
        env = {key: value for key, value in os.environ.items()
               if not key.startswith("SR_TRACE") and not key.startswith("SR_FLIGHT")}
        env["SR_TRACE"] = str(self.tmp / "window.txt")
        env.update(values)
        result = subprocess.run([str(self.executable)], cwd=self.tmp, env=env,
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout.strip(), result.stderr

    def test_well_formed_windows_arm(self):
        for window in ("0x08804000:0x08805000", "0x08804000-0x08805000", "0:0"):
            with self.subTest(window=window):
                stdout, stderr = self._configure(SR_TRACE_PC=window, SR_TRACE_LIMIT="16")
                self.assertEqual(stdout, "armed=1 v=0 f=0", stderr)
        stdout, stderr = self._configure(SR_TRACE_PC="0x08804000:0x08805000",
                                         SR_TRACE_V="0,127", SR_TRACE_F="0, 31")
        self.assertEqual(stdout, "armed=1 v=2 f=2", stderr)

    def test_a_register_index_outside_its_file_refuses_the_window(self):
        # fi[] has 32 entries and v[] has 128: a record must never read past either.
        for name, value, message in (
            ("SR_TRACE_F", "40", "SR_TRACE_F index 40 out of range 0..31"),
            ("SR_TRACE_F", "32", "SR_TRACE_F index 32 out of range 0..31"),
            ("SR_TRACE_V", "128", "SR_TRACE_V index 128 out of range 0..127"),
            ("SR_TRACE_F", "3,x", "SR_TRACE_F needs a list of register indices"),
            ("SR_TRACE_V", "-1", "SR_TRACE_V needs a list of register indices"),
        ):
            with self.subTest(name=name, value=value):
                stdout, stderr = self._configure(SR_TRACE_PC="0x08804000:0x08805000",
                                                 **{name: value})
                self.assertEqual(stdout, "armed=0 v=0 f=0")
                self.assertIn(f"TRACE_WINDOW: {message}", stderr)

    def test_a_malformed_bound_or_limit_refuses_the_window(self):
        # A letter O for a zero, or a word, used to parse as 0 and arm an empty 0..0 window.
        for window in ("O8804000:08805000", "pc:end", "0x08804000:0x08805000x",
                       "0x08804000:0x100000000", ":0x08805000", "-1:5"):
            with self.subTest(window=window):
                stdout, stderr = self._configure(SR_TRACE_PC=window)
                self.assertEqual(stdout, "armed=0 v=0 f=0")
                self.assertIn("TRACE_WINDOW: SR_TRACE_PC needs", stderr)
        for limit in ("abc", "10x", "-3"):
            with self.subTest(limit=limit):
                stdout, stderr = self._configure(SR_TRACE_PC="0x08804000:0x08805000",
                                                 SR_TRACE_LIMIT=limit)
                self.assertEqual(stdout, "armed=0 v=0 f=0")
                self.assertIn("TRACE_WINDOW: SR_TRACE_LIMIT needs a record count", stderr)


if __name__ == "__main__":
    unittest.main()
