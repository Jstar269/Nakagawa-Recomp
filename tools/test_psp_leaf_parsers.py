# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Fail-closed tests for source-owned PSP oracle stream parsers.

Every mutation family here is generated from the parser's own ``StreamSpec``,
so the number of cases moves with the source rather than with a comment.  The
parsers share one strict-sequence engine, so each sequence mutation family is
asserted against every spec instead of being written out once per probe.
"""

from __future__ import annotations

from pathlib import Path
import re
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from psp_oracle.parse_golden import (
    AUDIO_SPEC,
    CACHE_SPEC,
    IO_SPEC,
    MBX_DELETE_WAIT_EXPECTED_FIELDS,
    MBX_DELETE_WAIT_SPEC,
    AUDIO_OUT_COUNTS,
    parse_audio_query_output,
    parse_cache_alias_output,
    parse_io_matrix_output,
    parse_mbx_delete_wait_output,
)
from psp_oracle.protocol import ProtocolError, StreamSpec

META = (
    "NAKAGAWA_PSP_META schema=1 source=psp model=unknown firmware=unknown "
    "binary_sha256=0000000000000000000000000000000000000000000000000000000000000000 "
    "source_commit=0000000000000000000000000000000000000000 fixture=test\n"
)

# (spec, parser) for every probe that rides the shared strict-sequence engine.
PROBES = (
    (IO_SPEC, parse_io_matrix_output),
    (AUDIO_SPEC, parse_audio_query_output),
    (CACHE_SPEC, parse_cache_alias_output),
    (MBX_DELETE_WAIT_SPEC, parse_mbx_delete_wait_output),
)


PROBE_SOURCE = Path(__file__).resolve().parents[1] / "tests" / "fixtures" / "psp_oracle" / "probe.c"

# The probe.c function that emits each stream.
PROBE_FUNCTIONS = {
    IO_SPEC.test_id: "static void run_io_matrix(int emulated) {",
    AUDIO_SPEC.test_id: "static void run_audio_query(int emulated) {",
    CACHE_SPEC.test_id: "static void run_cache_alias(int emulated) {",
    MBX_DELETE_WAIT_SPEC.test_id: "static uint32_t run_mbx_delete_wait(int emulated) {",
}

_DEFERRED_RECORD = re.compile(
    r"\b(?:defer_record|audio_defer)\(\s*(?:emulated\s*,\s*)?(?:\"([^\"]+)\"|case_id)"
    r"\s*,[^;]*?,\s*(\d+)\s*\);"
)
_EXTENDED_RECORD = re.compile(
    r"\bemit_record_extended\(\s*emulated\s*,\s*\"([^\"]+)\"\s*,\s*\"([^\"]+)\"[^;]*?,"
    r"\s*(\w+)\s*,\s*sizeof\(\3\)\s*/\s*sizeof\(\3\[0\]\)\s*\);"
)


def probe_function(test_id: str) -> str:
    source = PROBE_SOURCE.read_text(encoding="utf-8")
    start = source.index(PROBE_FUNCTIONS[test_id])
    return source[start:source.index("\n}\n", start)]


def probe_records(test_id: str) -> dict[str, int]:
    """Case id -> output count, in the order probe.c first emits each case.

    A case emitted on two paths (a failure path with fewer outputs) keeps its first,
    successful-path emission, which is the one a passing run produces.
    """

    source = PROBE_SOURCE.read_text(encoding="utf-8")
    body = probe_function(test_id)
    records: dict[str, int] = {}
    for match in _DEFERRED_RECORD.finditer(body):
        if match.group(1) is not None:
            records.setdefault(match.group(1), int(match.group(2)))
            continue
        # A formatted case id: snprintf(case_id, ..., "<prefix>%u", block) inside
        # for (uint32_t block = 0; block < <BOUND>; ++block).
        prefix = re.search(r'snprintf\(case_id, sizeof\(case_id\), "([^"]+)%u"', body)
        bound = re.search(r"for \(uint32_t block = 0; block < (\w+); \+\+block\)", body)
        assert prefix is not None and bound is not None, test_id
        count = re.search(rf"^#define {bound.group(1)} (\d+)$", source, re.MULTILINE)
        assert count is not None, bound.group(1)
        for block in range(int(count.group(1))):
            records.setdefault(f"{prefix.group(1)}{block}", int(match.group(2)))
    for match in _EXTENDED_RECORD.finditer(body):
        if match.group(1) != test_id:
            continue
        size = re.search(rf"\buint32_t {match.group(3)}\[(\d+)\]", body)
        assert size is not None, match.group(3)
        records.setdefault(match.group(2), int(size.group(1)))
    return records


# What probe.c emits for each stream, read from the source rather than restated.
PROBE_OUT_COUNTS = {test_id: probe_records(test_id) for test_id in PROBE_FUNCTIONS}


def value(spec: StreamSpec, case_id: str, index: int) -> int:
    """The synthetic value of out<index> in ``case_id``'s record.

    Each IO, audio and cache record carries values unique to its case, so an observable
    read from the wrong case or field cannot match.  The mailbox field-set tests pin
    the out<i> = i + 1 shape, so those records keep it.
    """

    if spec == MBX_DELETE_WAIT_SPEC:
        return index + 1
    position: int = spec.ordered_cases.index(case_id)
    return (position + 1) * 0x100 + index


def line(spec: StreamSpec, case_id: str, status: str = "PASS", *, out0: int | None = None) -> str:
    """One protocol record for ``case_id``, with the output count the probe emits."""

    if case_id == spec.terminal_case:
        terminal = spec.terminal_count if out0 is None else out0
        return (
            f"NAKAGAWA_PSP_TEST schema=1 test_id={spec.test_id} case_id={case_id} "
            f"status={status} result=0x00000000 out0=0x{terminal:08x}\n"
        )
    outputs = " ".join(
        f"out{i}=0x{value(spec, case_id, i):08x}"
        for i in range(PROBE_OUT_COUNTS[spec.test_id][case_id])
    )
    return (
        f"NAKAGAWA_PSP_TEST schema=1 test_id={spec.test_id} case_id={case_id} "
        f"status={status} result=0x00000000 {outputs}\n"
    )


def stream(spec: StreamSpec, cases, *, meta: str = META) -> str:
    return meta + "".join(line(spec, case) for case in cases)


def golden(spec: StreamSpec) -> str:
    return stream(spec, spec.ordered_cases)


class TestSpecsMatchProbeSource(unittest.TestCase):
    """The record contract: the specs' literals, and their case order and output
    counts checked against the records probe.c emits."""

    def test_io_contract(self) -> None:
        self.assertEqual(IO_SPEC.test_id, "PSP-IO-001")
        self.assertEqual(IO_SPEC.terminal_case, "io-done")
        self.assertEqual(IO_SPEC.terminal_count, 6)
        self.assertEqual(IO_SPEC.record_count, 7)
        self.assertEqual(
            IO_SPEC.semantic_cases,
            (
                "io-open-create",
                "io-write",
                "io-read-verify",
                "io-lseek",
                "io-append",
                "io-errors",
            ),
        )

    def test_audio_contract(self) -> None:
        self.assertEqual(AUDIO_SPEC.test_id, "PSP-AUDIO-001")
        self.assertEqual(AUDIO_SPEC.terminal_case, "audio-done")
        self.assertEqual(AUDIO_SPEC.terminal_count, 13)
        self.assertEqual(AUDIO_SPEC.record_count, 14)
        self.assertEqual(
            AUDIO_SPEC.semantic_cases,
            (
                "audio-ch-reserve",
                "audio-ch-query-before",
                "audio-ch-output-blocking-0",
                "audio-ch-output-blocking-1",
                "audio-ch-query-after",
                "audio-ch-release",
                "audio-ch-query-released",
                "audio-out2-reserve",
                "audio-out2-query-before",
                "audio-out2-output-blocking",
                "audio-out2-query-after",
                "audio-out2-release",
                "audio-src-reserve",
            ),
        )

    def test_cache_contract(self) -> None:
        self.assertEqual(CACHE_SPEC.test_id, "PSP-CACHE-001")
        self.assertEqual(CACHE_SPEC.terminal_case, "cache-done")
        self.assertEqual(CACHE_SPEC.terminal_count, 4)
        self.assertEqual(CACHE_SPEC.record_count, 5)
        self.assertEqual(
            CACHE_SPEC.semantic_cases,
            (
                "cache-alias-init",
                "cache-writeback-contrast",
                "cache-inval-contrast",
                "cache-wball",
            ),
        )

    def test_mbx_delete_wait_contract(self) -> None:
        self.assertEqual(MBX_DELETE_WAIT_SPEC.test_id, "PSP-KERNEL-001")
        self.assertEqual(
            MBX_DELETE_WAIT_SPEC.semantic_cases,
            ("mbx-delete-wait", "mbx-timeout-control"),
        )
        self.assertEqual(MBX_DELETE_WAIT_SPEC.terminal_case, "mbx-done")
        self.assertEqual(MBX_DELETE_WAIT_SPEC.terminal_count, 2)
        self.assertEqual(MBX_DELETE_WAIT_SPEC.record_count, 3)
        self.assertEqual(
            len(MBX_DELETE_WAIT_EXPECTED_FIELDS["mbx-delete-wait"]), 27
        )
        self.assertEqual(
            len(MBX_DELETE_WAIT_EXPECTED_FIELDS["mbx-timeout-control"]), 9
        )
        self.assertEqual(
            MBX_DELETE_WAIT_EXPECTED_FIELDS["mbx-done"], {"result", "out0"}
        )

    def test_terminal_is_never_counted_as_semantic(self) -> None:
        for spec, _ in PROBES:
            with self.subTest(spec.test_id):
                self.assertNotIn(spec.terminal_case, spec.semantic_cases)
                self.assertEqual(spec.record_count, spec.terminal_count + 1)

    def test_spec_case_order_is_the_probes_emission_order(self) -> None:
        for spec, _ in PROBES:
            with self.subTest(spec.test_id):
                self.assertEqual(tuple(PROBE_OUT_COUNTS[spec.test_id]), spec.ordered_cases)

    def test_deferred_probes_flush_under_their_own_test_id(self) -> None:
        for spec in (IO_SPEC, AUDIO_SPEC, CACHE_SPEC):
            with self.subTest(spec.test_id):
                self.assertIn(f'flush_deferred(emulated, "{spec.test_id}");', probe_function(spec.test_id))

    def test_audio_output_counts_are_the_probes(self) -> None:
        self.assertEqual(AUDIO_OUT_COUNTS, PROBE_OUT_COUNTS[AUDIO_SPEC.test_id])

    def test_mbx_field_sets_are_the_probes_output_arrays(self) -> None:
        for case, count in PROBE_OUT_COUNTS[MBX_DELETE_WAIT_SPEC.test_id].items():
            with self.subTest(case):
                self.assertEqual(
                    MBX_DELETE_WAIT_EXPECTED_FIELDS[case],
                    {"result", *(f"out{i}" for i in range(count))},
                )


class TestGoldenStreams(unittest.TestCase):
    def test_golden_passes_in_both_modes(self) -> None:
        for spec, parse in PROBES:
            for strict in (True, False):
                with self.subTest(spec.test_id, strict=strict):
                    report = parse(golden(spec), require_complete=strict)
                    self.assertTrue(report.complete)
                    self.assertTrue(report.all_passed)
                    self.assertTrue(report.terminal_present)
                    self.assertEqual(report.record_count, spec.record_count)

    def test_observables_are_read_from_the_named_fields(self) -> None:
        # Every value is unique to its case and field, so a parser reading another case
        # or another out<i> gets a different number.
        io_report = parse_io_matrix_output(golden(IO_SPEC))
        self.assertEqual(io_report.open_fd, value(IO_SPEC, "io-open-create", 0))
        self.assertEqual(io_report.removed_rc, value(IO_SPEC, "io-errors", 2))
        audio_report = parse_audio_query_output(golden(AUDIO_SPEC))
        self.assertEqual(audio_report.channel_reserve_rc, value(AUDIO_SPEC, "audio-ch-reserve", 0))
        self.assertEqual(audio_report.channel_rest_len, value(AUDIO_SPEC, "audio-ch-query-before", 0))
        cache_report = parse_cache_alias_output(golden(CACHE_SPEC))
        self.assertEqual(cache_report.stale_cached_read, value(CACHE_SPEC, "cache-inval-contrast", 0))
        self.assertEqual(cache_report.fresh_cached_read, value(CACHE_SPEC, "cache-inval-contrast", 1))

    def test_a_single_fail_cell_is_complete_but_not_passed(self) -> None:
        for spec, parse in PROBES:
            for case in spec.semantic_cases:
                with self.subTest(spec.test_id, case=case):
                    text = META + "".join(
                        line(spec, c, "FAIL" if c == case else "PASS")
                        for c in spec.ordered_cases
                    )
                    report = parse(text)
                    self.assertTrue(report.complete)
                    self.assertFalse(report.all_passed)


class TestTruncationNeverPasses(unittest.TestCase):
    """No prefix of a stream may ever report a semantic pass."""

    def test_every_ordinary_truncation_boundary(self) -> None:
        for spec, parse in PROBES:
            for k in range(1, spec.record_count):
                with self.subTest(spec.test_id, k=k):
                    text = stream(spec, spec.ordered_cases[:k])
                    with self.assertRaises(ProtocolError):
                        parse(text, require_complete=True)
                    report = parse(text, require_complete=False)
                    self.assertEqual(report.record_count, k)
                    self.assertFalse(report.complete)
                    self.assertFalse(report.all_passed)

    def test_every_truncation_that_kept_its_terminal_record(self) -> None:
        """The transport-defect shape: an ordered prefix ending at the terminal."""

        for spec, parse in PROBES:
            for k in range(1, spec.terminal_count):
                with self.subTest(spec.test_id, k=k):
                    text = stream(
                        spec, list(spec.semantic_cases[:k]) + [spec.terminal_case]
                    )
                    with self.assertRaises(ProtocolError):
                        parse(text, require_complete=True)
                    report = parse(text, require_complete=False)
                    self.assertTrue(report.terminal_present)
                    self.assertFalse(report.complete)
                    self.assertFalse(report.all_passed)

    def test_terminal_present_with_one_missing_middle_record(self) -> None:
        for spec, parse in PROBES:
            for dropped in spec.semantic_cases:
                with self.subTest(spec.test_id, dropped=dropped):
                    text = stream(
                        spec, [c for c in spec.ordered_cases if c != dropped]
                    )
                    with self.assertRaises(ProtocolError):
                        parse(text, require_complete=True)
                    report = parse(text, require_complete=False)
                    self.assertTrue(report.terminal_present)
                    self.assertFalse(report.complete)
                    self.assertFalse(report.all_passed)

    def test_missing_terminal_is_never_complete(self) -> None:
        for spec, parse in PROBES:
            with self.subTest(spec.test_id):
                text = stream(spec, spec.semantic_cases)
                with self.assertRaises(ProtocolError):
                    parse(text, require_complete=True)
                report = parse(text, require_complete=False)
                self.assertFalse(report.terminal_present)
                self.assertFalse(report.complete)
                self.assertFalse(report.all_passed)


class TestStructuralMutations(unittest.TestCase):
    def test_duplicate_semantic_record_rejected_in_both_modes(self) -> None:
        for spec, parse in PROBES:
            for case in spec.semantic_cases:
                with self.subTest(spec.test_id, case=case):
                    text = stream(spec, list(spec.ordered_cases) + [case])
                    for strict in (True, False):
                        with self.assertRaises(ProtocolError):
                            parse(text, require_complete=strict)

    def test_duplicate_terminal_rejected_in_both_modes(self) -> None:
        for spec, parse in PROBES:
            with self.subTest(spec.test_id):
                text = stream(spec, list(spec.ordered_cases) + [spec.terminal_case])
                for strict in (True, False):
                    with self.assertRaises(ProtocolError):
                        parse(text, require_complete=strict)

    def test_swapped_neighbouring_records_rejected(self) -> None:
        for spec, parse in PROBES:
            for i in range(spec.record_count - 1):
                with self.subTest(spec.test_id, i=i):
                    order = list(spec.ordered_cases)
                    order[i], order[i + 1] = order[i + 1], order[i]
                    text = stream(spec, order)
                    for strict in (True, False):
                        with self.assertRaises(ProtocolError):
                            parse(text, require_complete=strict)

    def test_records_after_the_terminal_are_rejected(self) -> None:
        """A fragment appended past the completion record can never extend it."""

        for spec, parse in PROBES:
            for case in spec.semantic_cases:
                with self.subTest(spec.test_id, case=case):
                    order = [c for c in spec.ordered_cases if c != case] + [case]
                    text = stream(spec, order)
                    for strict in (True, False):
                        with self.assertRaises(ProtocolError) as ctx:
                            parse(text, require_complete=strict)
                        self.assertIn("terminal", str(ctx.exception))

    def test_unknown_case_id_rejected_in_both_modes(self) -> None:
        for spec, parse in PROBES:
            with self.subTest(spec.test_id):
                text = META + "".join(
                    line(spec, c) for c in spec.semantic_cases[:1]
                ) + (
                    f"NAKAGAWA_PSP_TEST schema=1 test_id={spec.test_id} "
                    "case_id=alien-cell status=PASS result=0x00000000 out0=0x00000000\n"
                )
                for strict in (True, False):
                    with self.assertRaises(ProtocolError) as ctx:
                        parse(text, require_complete=strict)
                    self.assertIn("unknown case_id", str(ctx.exception))

    def test_wrong_test_id_rejected_in_both_modes(self) -> None:
        for spec, parse in PROBES:
            with self.subTest(spec.test_id):
                text = golden(spec).replace(spec.test_id, "PSP-WRONG-999", 1)
                for strict in (True, False):
                    with self.assertRaises(ProtocolError) as ctx:
                        parse(text, require_complete=strict)
                    self.assertIn("foreign test_id", str(ctx.exception))

    def test_a_probes_stream_is_rejected_by_every_other_probes_parser(self) -> None:
        for spec, _ in PROBES:
            for other_spec, other_parse in PROBES:
                if other_spec.test_id == spec.test_id:
                    continue
                with self.subTest(stream=spec.test_id, parser=other_spec.test_id):
                    for strict in (True, False):
                        with self.assertRaises(ProtocolError):
                            other_parse(golden(spec), require_complete=strict)


class TestTerminalCountIsLoadBearing(unittest.TestCase):
    def test_wrong_terminal_count_rejected_in_both_modes(self) -> None:
        for spec, parse in PROBES:
            for claimed in (0, spec.terminal_count - 1, spec.terminal_count + 1, 99):
                with self.subTest(spec.test_id, claimed=claimed):
                    text = META + "".join(
                        line(spec, c, out0=claimed if c == spec.terminal_case else None)
                        for c in spec.ordered_cases
                    )
                    for strict in (True, False):
                        with self.assertRaises(ProtocolError) as ctx:
                            parse(text, require_complete=strict)
                        self.assertIn("terminal count mismatch", str(ctx.exception))

    def test_terminal_without_out0_rejected(self) -> None:
        for spec, parse in PROBES:
            with self.subTest(spec.test_id):
                text = stream(spec, spec.semantic_cases) + (
                    f"NAKAGAWA_PSP_TEST schema=1 test_id={spec.test_id} "
                    f"case_id={spec.terminal_case} status=PASS result=0x00000000\n"
                )
                for strict in (True, False):
                    with self.assertRaises(ProtocolError) as ctx:
                        parse(text, require_complete=strict)
                    self.assertIn("out0", str(ctx.exception))


class TestStaleLogContaminationIsRejected(unittest.TestCase):
    """The four stale shapes an append-only device log can produce.

    Each must be rejected mechanically, in both strictness modes, without any
    host-side inspection of which run a record came from.
    """

    def test_two_complete_runs_concatenated(self) -> None:
        for spec, parse in PROBES:
            with self.subTest(spec.test_id):
                for strict in (True, False):
                    with self.assertRaises(ProtocolError):
                        parse(golden(spec) + golden(spec), require_complete=strict)

    def test_two_complete_runs_with_a_single_meta(self) -> None:
        """Even without the duplicate META, every case_id repeats."""

        for spec, parse in PROBES:
            with self.subTest(spec.test_id):
                text = golden(spec) + stream(spec, spec.ordered_cases, meta="")
                for strict in (True, False):
                    with self.assertRaises(ProtocolError) as ctx:
                        parse(text, require_complete=strict)
                    self.assertIn("duplicate", str(ctx.exception))

    def test_stale_prefix_then_a_complete_run(self) -> None:
        for spec, parse in PROBES:
            for k in range(1, spec.record_count):
                with self.subTest(spec.test_id, stale=k):
                    text = stream(spec, spec.ordered_cases[:k]) + stream(
                        spec, spec.ordered_cases, meta=""
                    )
                    for strict in (True, False):
                        with self.assertRaises(ProtocolError):
                            parse(text, require_complete=strict)

    def test_complete_run_then_a_partial_new_run(self) -> None:
        for spec, parse in PROBES:
            for k in range(1, spec.record_count):
                with self.subTest(spec.test_id, fresh=k):
                    text = golden(spec) + stream(spec, spec.ordered_cases[:k], meta="")
                    for strict in (True, False):
                        with self.assertRaises(ProtocolError):
                            parse(text, require_complete=strict)

    def test_stale_prefix_alone_is_not_complete(self) -> None:
        """A leftover prefix with no fresh run must never look like a pass."""

        for spec, parse in PROBES:
            for k in range(1, spec.record_count):
                with self.subTest(spec.test_id, k=k):
                    report = parse(
                        stream(spec, spec.ordered_cases[:k]), require_complete=False
                    )
                    self.assertFalse(report.complete)
                    self.assertFalse(report.all_passed)


class TestProvenanceIsReportedNeverUpgraded(unittest.TestCase):
    def test_placeholder_metadata_is_flagged_and_never_blocks_the_parse(self) -> None:
        for spec, parse in PROBES:
            with self.subTest(spec.test_id):
                report = parse(golden(spec))
                issues = " ".join(report.sequence.provenance)
                self.assertIn("binary_sha256", issues)
                self.assertIn("source_commit", issues)
                self.assertIn("model", issues)
                self.assertIn("firmware", issues)
                # Placeholder provenance is a reporting fact, not a semantic
                # verdict: the stream still parses and still passes.
                self.assertTrue(report.all_passed)

    def test_measured_metadata_raises_no_provenance_issue(self) -> None:
        measured = (
            "NAKAGAWA_PSP_META schema=1 source=psp model=PSP-3000 "
            "firmware=6.61-ARK-5.1.0 binary_sha256=" + "a" * 64 + " "
            "source_commit=d923abc795a7ec38c8e2f763b632909be73b26d2 fixture=test\n"
        )
        for spec, parse in PROBES:
            with self.subTest(spec.test_id):
                report = parse(stream(spec, spec.ordered_cases, meta=measured))
                self.assertEqual(report.sequence.provenance, ())


class TestMbxExactFieldSets(unittest.TestCase):
    def test_missing_and_extra_fields_are_rejected(self) -> None:
        text = golden(MBX_DELETE_WAIT_SPEC)
        mutations = (
            ("mbx-delete-wait", " result=0x00000000 ", " ", "primary result missing"),
            ("mbx-delete-wait", "out25=0x0000001a", "", "primary last field missing"),
            ("mbx-timeout-control", "out7=0x00000008", "", "control last field missing"),
            (
                "mbx-delete-wait",
                "out25=0x0000001a",
                "out25=0x0000001a out26=0x0000001b",
                "primary extra field",
            ),
            (
                "mbx-timeout-control",
                "out7=0x00000008",
                "out7=0x00000008 out8=0x00000009",
                "control extra field",
            ),
            (
                "mbx-done",
                "case_id=mbx-done status=PASS result=0x00000000 out0=0x00000002",
                "case_id=mbx-done status=PASS result=0x00000000 "
                "out0=0x00000002 out1=0x00000003",
                "terminal extra field",
            ),
        )
        for case_id, old, new, label in mutations:
            with self.subTest(label=label):
                lines = text.splitlines(keepends=True)
                row_index = next(
                    i for i, row in enumerate(lines) if f"case_id={case_id} " in row
                )
                self.assertIn(old, lines[row_index])
                lines[row_index] = lines[row_index].replace(old, new, 1)
                mutated = "".join(lines)
                for strict in (True, False):
                    with self.assertRaises(ProtocolError) as ctx:
                        parse_mbx_delete_wait_output(
                            mutated, require_complete=strict
                        )
                    self.assertIn(case_id, str(ctx.exception))

    def test_raw_api_values_are_not_parser_expectations(self) -> None:
        text = golden(MBX_DELETE_WAIT_SPEC)
        text = text.replace("result=0x00000000", "result=0xdeadbeef", 1)
        text = text.replace("out8=0x00000009", "out8=0x800201b5", 1)
        report = parse_mbx_delete_wait_output(text)
        self.assertTrue(report.complete)
        self.assertTrue(report.all_passed)


if __name__ == "__main__":
    unittest.main()
