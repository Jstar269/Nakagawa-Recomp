# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Source-shape gates for the measured PSP DMAC copy path.

The PSP-visible behavior itself is proven executably by
``tests/native/hle_thread_selftest.c``, which enters both DMAC NIDs through the real
``sr_syscall`` registry and asserts return values and guest memory contents.
These assertions guard the two properties a behavioral test cannot express:

* complete valid spans must not be silently clamped to the old 0xC000
  allocator-boundary observation, and
* a span past the end of the guest arena is refused whole: no partial prefix is
  copied, since the PSP's answer there is unmeasured (#303).

The cross-thread BUSY result is modelled from the PSP-3000 measurement (#303):
BUSY is returned only by a Try made while another thread's transfer is pending,
and the blocking form waits through a scheduler block, never a sleep or a spin.
The executable proof is ``test_dmac_active_transfer_across_threads``.

The last classes are the mutation campaign (``make dmac-mutants``): the executable
selftest is proven load-bearing by building it against copies of ``hle_cache_dma.c``
with one defect each, every one of which must fail the assertion it names.
"""

from pathlib import Path
import os
import re
import shutil
import subprocess
import unittest
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hle_sources  # noqa: E402

try:
    from . import scratch_dir
    from .psp_oracle.parse_golden import parse_dmac_invalid_tail_output
    from .psp_oracle.protocol import ProtocolError
except ImportError:
    import scratch_dir  # type: ignore[no-redef]
    from psp_oracle.parse_golden import parse_dmac_invalid_tail_output  # type: ignore[no-redef]
    from psp_oracle.protocol import ProtocolError  # type: ignore[no-redef]


ROOT = Path(__file__).resolve().parent.parent


def _dmac_region() -> str:
    """The sceDmac copy implementation, from its comment block to the end of dmac_transfer.

    The block is one stretch of the file that defines it (since #307, hle_cache_dma.c), so the
    slice stays inside that file.
    """
    text: str = dict(hle_sources.sources())[hle_sources.defining_file("dmac_transfer")]
    start = text.index("/* ---- sceDmacMemcpy / sceDmacTryMemcpy")
    transfer = hle_sources.function_text("uint32_t dmac_transfer(CpuState *s, int try_only) {")
    return text[start:text.index(transfer, start) + len(transfer)]


class TestDmacValidationOrder(unittest.TestCase):
    def test_measured_error_classes_are_returned(self) -> None:
        region = _dmac_region()
        self.assertIn("#define SCE_DMAC_ERROR_ILLEGAL_ADDR 0x80000103u", region)
        self.assertIn("#define SCE_DMAC_ERROR_ILLEGAL_SIZE 0x80000104u", region)
        self.assertIn("if (n == 0u) return SCE_DMAC_ERROR_ILLEGAL_SIZE;", region)
        self.assertIn(
            "if (dst == 0u || src == 0u) return SCE_DMAC_ERROR_ILLEGAL_ADDR;", region
        )

    def test_prefix_policy_precedes_copy_and_dirty(self) -> None:
        """The bounded prefix is selected before a host pointer is formed."""
        region = _dmac_region()
        size_check = region.index("if (n == 0u) return SCE_DMAC_ERROR_ILLEGAL_SIZE;")
        null_check = region.index("if (dst == 0u || src == 0u)")
        prefix = region.index("const uint32_t src_prefix = sr_guest_span_prefix(src, n);")
        policy = region.index("if (src_prefix != n || dst_prefix != n) {")
        effective = region.index("const uint32_t effective = n;")
        transfer = region.index("static uint32_t dmac_transfer(CpuState *s, int try_only) {")
        copy = region.index("dmac_copy(dst, src, effective);", transfer)
        helper = region.index("static void dmac_copy(uint32_t dst, uint32_t src, uint32_t n) {")
        helper_copy = region.index("memmove(SR_HOST(dst), SR_HOST(src), n);", helper)
        dirty = region.index("sr_guest_vram_write_commit_bytes(dst, n);", helper)

        self.assertLess(size_check, null_check)
        self.assertLess(null_check, prefix)
        self.assertLess(prefix, policy)
        self.assertLess(policy, effective, "a short span is refused before any length is chosen")
        self.assertLess(effective, copy, "the effective length must be selected before copying")
        self.assertLess(helper_copy, dirty, "the GPU is notified only after a real transfer")
        # Every write in the transfer goes through dmac_copy, after validation.
        body = region[transfer:]
        self.assertLess(body.index("return SCE_DMAC_ERROR_ILLEGAL_ADDR;\n    }\n\n"),
                        body.index("dmac_copy("))
        self.assertIn("sr_guest_span_readable_current(src, effective)", region)
        self.assertIn("sr_guest_span_writable(dst, effective)", region)

    def test_overlap_safe_primitive(self) -> None:
        """Hardware showed both overlap directions landing memmove-correct."""
        region = _dmac_region()
        self.assertIn("memmove(SR_HOST(dst), SR_HOST(src), n);", region)
        self.assertNotIn("memcpy(SR_HOST(dst), SR_HOST(src)", region)
        self.assertEqual(region.count("memmove(SR_HOST("), 1, "one copy primitive, dmac_copy")

    def test_both_copy_nids_register_in_the_shared_bulk_helper(self) -> None:
        """Both NIDs must route through production registration."""
        text = hle_sources.text()
        # The shared helper calls the DMA subsystem's registration run (#307).
        helper = hle_sources.function_text("static void hle_register_bulk_memory_handlers(void) {")
        self.assertIn("hle_register_bulk_memory_cache_dma();", helper)
        registration = hle_sources.function_text("void hle_register_bulk_memory_cache_dma(void) {")
        self.assertIn('sr_hle_register(0x617f3fe6, "sceDmacMemcpy", h_DmacMemcpy)', registration)
        self.assertIn(
            'sr_hle_register(0xd97f94d8, "sceDmacTryMemcpy", h_DmacTryMemcpy)', registration
        )
        self.assertEqual(text.count('"sceDmacTryMemcpy"'), 1)
        self.assertEqual(text.count('"sceDmacMemcpy"'), 1)


class TestDmacMeasuredBoundary(unittest.TestCase):
    def test_no_api_wide_effective_ceiling_is_encoded(self) -> None:
        region = _dmac_region()
        self.assertNotIn("SCE_DMAC_EFFECTIVE_MAX", region)
        self.assertIn("there is no\n *     API-wide 0xC000 ceiling", region)
        self.assertIn("fully valid RAM/VRAM spans copy their complete", region)

    def test_only_effective_prefix_has_side_effects(self) -> None:
        region = _dmac_region()
        self.assertIn("memmove(SR_HOST(dst), SR_HOST(src), n);", region)
        self.assertIn("sr_guest_vram_write_commit_bytes(dst, n);", region)
        self.assertIn("sr_heap_note_bulk_write(dst, n, 0u)", region)
        # Only the effective span, or its head and tail on a refused Try, is written.
        self.assertIn("dmac_copy(dst, src, effective);", region)
        self.assertIn("dmac_copy(dst, src, head);", region)
        self.assertIn("dmac_copy(dst + effective - tail, src + effective - tail, tail);", region)
        self.assertIn("const uint32_t body = effective - head - tail;", region)

    def test_busy_only_for_a_try_during_a_pending_transfer(self) -> None:
        """BUSY is the measured cross-thread Try result and nothing else."""
        region = _dmac_region()
        code = re.sub(r"/\*.*?\*/", "", region, flags=re.S)
        code = re.sub(r"//[^\n]*", "", code)
        self.assertIn("#define SCE_DMAC_ERROR_BUSY 0x80000021u", code)
        self.assertEqual(code.count("0x80000021"), 1)
        self.assertEqual(code.count("return SCE_DMAC_ERROR_BUSY;"), 1)
        pending = code.index("if (sched_transfer_pending(DMAC_TRANSFER_WAIT_OBJ, &holder) &&")
        bypass = code.index("if (same_phase && body <= DMAC_BYPASS_BODY_MAX) {")
        busy = code.index("return SCE_DMAC_ERROR_BUSY;")
        self.assertLess(code.index("if (dst == 0u || src == 0u)"), bypass)
        self.assertLess(bypass, pending, "a short same-phase body never sees the channel")
        self.assertLess(pending, busy)
        self.assertLess(busy, code.index("dmac_copy(dst, src, effective);\n\n"),
                        "a BUSY Try returns before the body is copied")
        self.assertIn("#define DMAC_BYPASS_BODY_MAX 0x100u", code)

    def test_waits_are_scheduler_blocks(self) -> None:
        """The blocking form waits through the scheduler, never a sleep or a spin."""
        code = re.sub(r"/\*.*?\*/", "", _dmac_region(), flags=re.S)
        self.assertEqual(code.count("sched_block_on(DMAC_TRANSFER_WAIT_OBJ);"), 2)
        self.assertIn("sched_transfer_begin(DMAC_TRANSFER_WAIT_OBJ, dmac_duration_us(dst, src, body))", code)
        self.assertIn("sched_transfer_enqueue(DMAC_TRANSFER_WAIT_OBJ)", code)
        self.assertIn("while (!sched_transfer_granted(DMAC_TRANSFER_WAIT_OBJ))", code)
        self.assertIn("sched_guest_thread_running() && sched_wait_permitted()", code)
        for forbidden in ("sched_delay_current", "sched_preempt", "Sleep(", "usleep", "sr_yield"):
            self.assertNotIn(forbidden, code)

    def test_span_past_arena_is_refused_whole(self) -> None:
        region = _dmac_region()
        self.assertIn("if (src_prefix != n || dst_prefix != n) {", region)
        self.assertIn("refused whole", region)
        self.assertNotIn("n - src_prefix != 1u", region)
        self.assertNotIn("one-byte arena-end tail", region)


class TestDmacExecutableCoverage(unittest.TestCase):
    def test_regression_covers_both_nids_through_production_dispatch(self) -> None:
        text = (ROOT / "tests" / "native" / "hle_thread_selftest.c").read_text(encoding="utf-8")
        self.assertIn("test_dmac_semantics();", text)
        self.assertIn("test_dmac_hardware_semantics(NID_SCE_DMAC_MEMCPY", text)
        self.assertIn("test_dmac_hardware_semantics(NID_SCE_DMAC_TRY_MEMCPY", text)
        self.assertIn("test_dmac_active_transfer_across_threads();", text)

    def test_regression_asserts_measured_and_policy_cases(self) -> None:
        text = (ROOT / "tests" / "native" / "hle_thread_selftest.c").read_text(encoding="utf-8")
        for needle in (
            "PSP: zero size returns the illegal-size error",
            "PSP: a NULL destination returns the illegal-address error",
            "PSP: a NULL source returns the illegal-address error",
            "a destination span one byte past the arena is refused",
            "a source span one byte past the arena is refused",
            "PSP: a same-pointer self copy leaves the buffer unchanged",
            "PSP: a forward-overlapping copy is memmove-correct across the span",
            "PSP: a backward-overlapping copy is memmove-correct across the span",
            "RAM-to-VRAM",
            "VRAM-to-RAM",
            "VRAM-to-VRAM",
            "aliased VRAM destination",
            "PSP: a hardware-verified transfer size copies every byte",
            "a multi-byte tail past the arena is refused",
            "a Try made while another thread's transfer is pending returns BUSY",
            "a blocking call made while another thread's transfer is pending returns 0",
            "the blocking call waits and returns only after the first transfer",
        ):
            self.assertIn(needle, text)

    def test_code_invalidation_boundary_is_explicit(self) -> None:
        region = _dmac_region()
        self.assertIn("does not currently translate guest self-modifying code", region)


class TestDmacSkipProducer(unittest.TestCase):
    """Compile the real C producer, not Python lookalike protocol rows.

    Only emit(), the durable-writer seam probe_emit_durable() (which forwards
    to emit() here) and the launch constants are synthetic. No PSP SDK, hardware,
    private input, allocation or DMAC call is involved in this setup-failure path.
    """

    def test_s0_and_b1_b4_producer_rows_satisfy_shared_skip_shape(self) -> None:
        compiler = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
        if not compiler:
            self.skipTest("a native C compiler is required for producer execution")
        probe = (ROOT / "fixtures/psp_oracle/probe.c").read_text(encoding="utf-8")
        record_start = probe.index("static void dmac_invalid_emit_record(")
        record = probe[record_start:probe.index("static int dmac_invalid_probe_neighbor", record_start)]
        skip_start = probe.index("static void dmac_invalid_emit_skips(")
        skips = probe[skip_start:probe.index("static void run_dmac_invalid_tier_s", skip_start)]
        macro_start = probe.index("#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_INVALID_TAIL_MEMCPY_DST", probe.index("#define DMAC_INVALID_MAX_DELTA"))
        macros = probe[macro_start:probe.index("static uint8_t s_dmac_invalid_io", macro_start)]
        constants = "\n".join(re.findall(
            r"^#define (?:PSP_ORACLE_CASE_DMAC_INVALID_TAIL_\w+|DMAC_INVALID_PAYLOAD_BYTES) .+$",
            probe, re.MULTILINE,
        ))
        launches = (
            ("dma-invalid-tail-s0", "S0", 8),
            ("dma-invalid-tail-memcpy-dst", "MEMCPY_DST", 4),
            ("dma-invalid-tail-memcpy-src", "MEMCPY_SRC", 4),
            ("dma-invalid-tail-try-dst", "TRY_DST", 4),
            ("dma-invalid-tail-try-src", "TRY_SRC", 4),
        )
        with scratch_dir.TemporaryDirectory(prefix="dmac-skip-producer-") as directory:
            root = Path(directory)
            for campaign, case, count in launches:
                for mutation in (False, True):
                    with self.subTest(campaign=campaign, integrity_mutation=mutation):
                        body = skips
                        if mutation:
                            # Generate/compile/execute the wrong source_intact argument
                            # for BOTH tiers; never mutate the tracked producer in place.
                            body = body.replace(
                                "0u, 0u, 0u, 0u, setup_mask",
                                "0u, 0u, 0u, 1u, setup_mask",
                            )
                            self.assertNotEqual(body, skips)
                        source = root / "producer.c"
                        binary = root / "producer.exe"
                        source.write_text(
                            "#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\n" + constants +
                            f"\n#define PSP_ORACLE_CASE PSP_ORACLE_CASE_DMAC_INVALID_TAIL_{case}\n" +
                            macros +
                            "static void emit(int emulated, const char *line) { (void)emulated; fputs(line, stdout); }\n" +
                            "static void probe_emit_durable(int emulated, const char *line, size_t length) "
                            "{ (void)length; emit(emulated, line); }\n" +
                            record + body +
                            "int main(void) { dmac_invalid_emit_skips(1, 3u, 0x80020190u); return 0; }\n",
                            encoding="utf-8",
                        )
                        built = subprocess.run(
                            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                             str(source), "-o", str(binary)],
                            capture_output=True, text=True, timeout=30,
                        )
                        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                        emitted = subprocess.run(
                            [str(binary)], capture_output=True, text=True, timeout=10,
                        )
                        self.assertEqual(emitted.returncode, 0, emitted.stderr)
                        self.assertEqual(len(emitted.stdout.splitlines()), count)
                        stream = (
                            "NAKAGAWA_PSP_META schema=1 source=nakagawa model=synthetic "
                            "firmware=test binary_sha256=" + "0" * 64 +
                            " source_commit=" + "0" * 40 + "\n" + emitted.stdout
                        )
                        if mutation:
                            with self.assertRaisesRegex(ProtocolError, "SKIP cannot claim source integrity"):
                                parse_dmac_invalid_tail_output(stream, campaign)
                        else:
                            parsed = parse_dmac_invalid_tail_output(stream, campaign)
                            self.assertEqual(len(parsed.results), count)
                            for result in parsed.results:
                                values = dict(result.values)
                                self.assertEqual(result.status, "SKIP")
                                for field in ("source_intact", "executed", "cache_discipline",
                                              "P", "matches", "guards_outside", "post_guard",
                                              "overflow_band", "payload_mutations"):
                                    self.assertEqual(int(values[field], 0), 0, field)


class TestDmacGpuAliasBoundary(unittest.TestCase):
    def test_renderer_canonicalizes_cpu_dirty_aliases(self) -> None:
        """DMA dirty notifications must invalidate aliased texture ranges."""
        text = (ROOT / "src" / "rt" / "gpu_sdl3vk" / "ge_gpu.c").read_text(
            encoding="utf-8"
        )
        start = text.index("static void hook_vram_dirty")
        region = text[start : text.index("/* ---- GE block transfer", start)]
        # The store and every cached range go through one canonical mapping: the store in
        # hook_vram_dirty, each entry's levels once when tex_get creates it (#282). Each level's
        # range comes from tex_source_range, which canonicalizes the address it is given (and,
        # through a reordering VRAM view, takes the envelope in the canonical 0x04000000 aperture).
        canon = text[text.index("static uint32_t tex_canonical"):]
        canon = canon[: canon.index("\n}\n")]
        self.assertIn("sr_vram_cpu_address(addr)", canon)
        self.assertIn("tex_canonical(addr)", region)
        ranges = text[text.index("static void tex_ent_set_ranges"):]
        ranges = ranges[: ranges.index("\n}\n")]
        self.assertIn("tex_source_range(e->addr,", ranges)
        self.assertIn("tex_source_range(e->mip.addr[l],", ranges)
        source = text[text.index("static void tex_source_range"):]
        source = source[: source.index("\n}\n")]
        self.assertIn("*lo = tex_canonical(addr);", source)
        self.assertIn("*lo = SR_VRAM_CPU_BASE + off;", source)
        self.assertIn("tex_ent_overlaps(e, dirty0, dirty1)", region)
        self.assertIn("vram_off(canonical_addr)", region)
        # The alias cases live in the GE self-tests, which ge_gpu.c includes from their own file.
        selftest = (ROOT / "src" / "rt" / "gpu_sdl3vk" / "ge_gpu_selftest.inc").read_text(
            encoding="utf-8"
        )
        self.assertIn('"alias-vram"', selftest)
        self.assertIn("if (tc->dirty_alias)", selftest)


# ---- the mutation campaign (#303) -------------------------------------------------------------
#
# Each mutant is one semantic defect in a COPY of src/rt/hle_cache_dma.c, never in the tree.  The copy
# replaces the original in the HLE source list, `hle-thread-selftest-build` compiles and links the real
# selftest against it, and the selftest runs.  Every mutant must build, and fail with the text it names:
# a compile error proves the compiler noticed, not the test (INVALID); a run that fails for another
# reason is WRONG_REASON, not a kill; a run that passes SURVIVED.  The unmutated sources must pass first,
# in the same build root, which every mutant reuses so only the selftest compile and link repeat.
#
# `make dmac-mutants` runs TestDmacMutationCampaign (minutes: one selftest build per mutant).  Plain test
# runs only check that the list still applies.

DMAC_TARGET = hle_sources.defining_file("dmac_transfer")
DMAC_SELFTEST = ROOT / "tests" / "native" / "hle_thread_selftest.c"

# (name, old text, new text, text the failure output must contain)
DMAC_MUTANTS: list[tuple[str, str, str, str]] = [
    ("a 0xC000 effective-transfer ceiling returns",
     "    const uint32_t effective = n;\n",
     "    const uint32_t effective = n > 0xC000u ? 0xC000u : n;\n",
     "PSP: a hardware-verified transfer size copies every byte"),
    ("the GPU dirty range stops at 0xC000 bytes",
     "    sr_guest_vram_write_commit_bytes(dst, n);\n    if (g_sr_heap_watch)",
     "    sr_guest_vram_write_commit_bytes(dst, n > 0xC000u ? 0xC000u : n);\n    if (g_sr_heap_watch)",
     "a large DMA reports its whole destination range to the GPU"),
    ("a refused Try copies nothing of its head and tail",
     "            dmac_copy(dst, src, head);\n"
     "            dmac_copy(dst + effective - tail, src + effective - tail, tail);\n",
     "",
     "a refused Try copies its 12 head bytes and nothing else"),
    ("a refused Try copies the whole span",
     "            dmac_copy(dst, src, head);\n"
     "            dmac_copy(dst + effective - tail, src + effective - tail, tail);\n",
     "            dmac_copy(dst, src, effective);\n",
     "PSP-DMAC-002 c3: a 0x110-byte aligned Try is refused with BUSY and copies nothing"),
    ("the size check follows the NULL-pointer check",
     "    if (n == 0u) return SCE_DMAC_ERROR_ILLEGAL_SIZE;\n"
     "    if (dst == 0u || src == 0u) return SCE_DMAC_ERROR_ILLEGAL_ADDR;\n",
     "    if (dst == 0u || src == 0u) return SCE_DMAC_ERROR_ILLEGAL_ADDR;\n"
     "    if (n == 0u) return SCE_DMAC_ERROR_ILLEGAL_SIZE;\n",
     "PSP: size 0 with a NULL destination returns the illegal-size error"),
    ("the copy is a forward byte loop, not memmove",
     "    memmove(SR_HOST(dst), SR_HOST(src), n);\n",
     "    for (uint32_t i = 0u; i < n; i++) SR_HOST(dst)[i] = SR_HOST(src)[i];\n",
     "memmove-correct"),
    ("the channel bypass covers a 0x110-byte body",
     "#define DMAC_BYPASS_BODY_MAX 0x100u",
     "#define DMAC_BYPASS_BODY_MAX 0x110u",
     "PSP-DMAC-002 c3"),
    ("a VRAM source crossing the end of VRAM is refused",
     "    if (!sr_guest_span_hits_swizzled_vram_mirror(src, n)) return 0;\n",
     "    return 0;\n",
     "PSP: a VRAM source running past the end of VRAM"),
]


def _dmac_hle_srcs() -> list[str]:
    """The HLE_SRCS list from mk/runtime_sources.mk, in order."""
    text = (ROOT / "mk" / "runtime_sources.mk").read_text(encoding="utf-8")
    match = re.search(r"^HLE_SRCS\s*:?=((?:.*\\\n)*.*)$", text, re.M)
    if not match:
        raise AssertionError("HLE_SRCS not found in mk/runtime_sources.mk")
    return match.group(1).replace("\\\n", " ").split()


class TestDmacMutantList(unittest.TestCase):
    def test_every_mutant_applies_exactly_once(self) -> None:
        source = (ROOT / DMAC_TARGET).read_text(encoding="utf-8")
        for name, old, new, _expect in DMAC_MUTANTS:
            with self.subTest(name):
                self.assertEqual(source.count(old), 1, "the old text must match one place")
                self.assertNotEqual(old, new)

    def test_every_expected_failure_is_a_selftest_assertion(self) -> None:
        selftest = DMAC_SELFTEST.read_text(encoding="utf-8")
        for name, _old, _new, expect in DMAC_MUTANTS:
            with self.subTest(name):
                self.assertTrue(expect in selftest, "the failure text must be an assertion it makes")

    def test_names_are_unique_and_the_target_is_an_hle_source(self) -> None:
        names = [m[0] for m in DMAC_MUTANTS]
        self.assertEqual(len(names), len(set(names)))
        self.assertIn(DMAC_TARGET, _dmac_hle_srcs())

    def test_the_gpu_dirty_range_of_a_large_copy_is_asserted(self) -> None:
        """The clamped-dirty-range mutant survived the selftest before the large-copy cells."""
        selftest = DMAC_SELFTEST.read_text(encoding="utf-8")
        self.assertIn("a large DMA reports its whole destination range to the GPU", selftest)
        self.assertTrue(any("dirty range" in m[0] for m in DMAC_MUTANTS))


@unittest.skipUnless(os.environ.get("NK_DMAC_MUTANTS_BUILD_DIR"), "run through `make dmac-mutants`")
class TestDmacMutationCampaign(unittest.TestCase):
    """Build the real selftest against each mutant; every mutant must be killed for its stated reason."""

    @staticmethod
    def _build_and_run(root: Path, srcs: list[str], log: Path) -> tuple[str, str]:
        """Return (stage, output); stage is BUILD, PASS or FAIL."""
        make = os.environ.get("NK_DMAC_MUTANTS_MAKE") or shutil.which("mingw32-make") or "make"
        build = subprocess.run(
            [make, "--no-print-directory", "hle-thread-selftest-build",
             f"BUILD_ROOT={root.as_posix()}", "HLE_SRCS=" + " ".join(srcs)],
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
        log.write_text(build.stdout, encoding="utf-8")
        exes = sorted(root.rglob("hle_thread_selftest.exe")) if build.returncode == 0 else []
        if not exes:
            return "BUILD", build.stdout
        run = subprocess.run([str(exes[0])], cwd=ROOT, text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, check=False)
        log.write_text(run.stdout, encoding="utf-8")
        return ("PASS" if run.returncode == 0 else "FAIL"), run.stdout

    def test_every_mutant_is_killed_for_its_stated_reason(self) -> None:
        base = Path(os.environ["NK_DMAC_MUTANTS_BUILD_DIR"]).resolve()
        only = os.environ.get("NK_DMAC_MUTANTS_ONLY", "")
        base.mkdir(parents=True, exist_ok=True)
        sources = _dmac_hle_srcs()
        original = (ROOT / DMAC_TARGET).read_text(encoding="utf-8")
        shared = base / "build"
        stage, out = self._build_and_run(shared, sources, base / "unmutated.log")
        self.assertEqual(stage, "PASS", f"the unmutated sources must pass first:\n{out[-2000:]}")
        for index, (name, old, new, expect) in enumerate(DMAC_MUTANTS):
            if only and only not in name:
                continue
            with self.subTest(name):
                copy = base / f"m{index:02d}" / Path(DMAC_TARGET).name
                copy.parent.mkdir(parents=True, exist_ok=True)
                copy.write_text(original.replace(old, new), encoding="utf-8")
                srcs = [copy.as_posix() if s == DMAC_TARGET else s for s in sources]
                stage, out = self._build_and_run(shared, srcs, copy.parent / "run.log")
                self.assertNotEqual(stage, "BUILD", "INVALID: the mutant did not build")
                self.assertNotEqual(stage, "PASS", "SURVIVED: the selftest passed the mutant")
                self.assertIn(expect, out, "WRONG_REASON: it failed, but not for the stated reason")


if __name__ == "__main__":
    unittest.main()
