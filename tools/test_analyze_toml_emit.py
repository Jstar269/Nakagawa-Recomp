# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Locks the analyze.py TOML emitter's fail-closed string contract.

emit_toml is the one place analyzer output interpolates arbitrary strings into
a host-syntax document. Guest bytes never reach it today (model fields are
host constants or round-tripped TOML), but the emitter must never silently
produce a malformed document: strings are escaped for basic strings and
control characters fail closed instead of being emitted raw.
"""

from __future__ import annotations

import sys
import tomllib
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

from analyze import _fmt_value  # noqa: E402


class TestTomlEmit(unittest.TestCase):
    def test_string_escape_contract(self):
        self.assertEqual(_fmt_value("name", 'a"b\\c'), r'"a\"b\\c"')

    def test_plain_strings_pass_through(self):
        self.assertEqual(_fmt_value("kind", "function"), '"function"')
        self.assertEqual(_fmt_value("note", "bal tail"), '"bal tail"')

    def test_non_string_values(self):
        self.assertEqual(_fmt_value("addr", 0x1234), "0x00001234")
        self.assertEqual(_fmt_value("entry", 0x8000), "0x00008000")
        self.assertEqual(_fmt_value("skip", True), "true")
        self.assertEqual(_fmt_value("size", 42), "42")

    def test_control_characters_are_rejected_fail_closed(self):
        for hostile in ("a\nb", "a\rb", "a\tb", "a\x01b", "\x00", "a\x1fb", "a\x7fb"):
            with self.assertRaisesRegex(ValueError, "control"):
                _fmt_value("note", hostile)

    def test_printable_unicode_output_is_toml_parseable(self):
        value = "café π 🙂"
        rendered = _fmt_value("note", value)
        parsed = tomllib.loads(f"note = {rendered}\n")
        self.assertEqual(parsed["note"], value)

    def test_emit_toml_roundtrips_printable_unicode(self):
        import tempfile

        from analyze import emit_toml, load_toml

        model = {
            "module": "módulo",
            "entry": 0x1000,
            "functions": [{"addr": 0x1000, "kind": "function", "name": "Écran",
                           "note": "日本語"}],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "functions.toml"
            emit_toml(model, path)
            self.assertEqual(load_toml(path), model)

    def test_roundtrip_check_handles_zero_or_one_function(self):
        import tempfile

        from analyze import roundtrip_check

        for functions in ([], [{"addr": 0x1000, "kind": "function"}]):
            with self.subTest(function_count=len(functions)), tempfile.TemporaryDirectory() as directory:
                ok, _path = roundtrip_check(
                    {"module": "elf", "entry": 0x1000, "functions": functions},
                    directory,
                )
                self.assertTrue(ok)


    def test_emit_toml_insertion_order_determinism(self):
        import tempfile
        from analyze import emit_toml

        fn_a = {"addr": 0x8000, "kind": "function", "z_extra": "value_z", "a_extra": "value_a"}
        fn_b = {"addr": 0x8004, "kind": "function", "a_extra": "value_a", "z_extra": "value_z"}
        # The second model holds copies whose keys were inserted in the
        # reverse order, so each function's dict order differs between the
        # two emits, as does the function list order.
        fn_a_reversed = dict(reversed(list(fn_a.items())))
        fn_b_reversed = dict(reversed(list(fn_b.items())))
        self.assertNotEqual(list(fn_a), list(fn_a_reversed))

        model1 = {"module": "elf", "entry": 0x8000, "functions": [fn_b, fn_a]}
        model2 = {"module": "elf", "entry": 0x8000, "functions": [fn_a_reversed, fn_b_reversed]}

        with tempfile.TemporaryDirectory() as directory:
            first = Path(directory) / "first.toml"
            second = Path(directory) / "second.toml"
            emit_toml(model1, first)
            emit_toml(model2, second)
            out1 = first.read_text(encoding="ascii")
            out2 = second.read_text(encoding="ascii")

        self.assertEqual(out1, out2)
        # Known keys keep their schema order and extra keys follow, sorted.
        tables = out1.split("[[function]]\n")[1:]
        self.assertEqual(len(tables), 2)
        for table in tables:
            keys = [line.split(" = ", 1)[0] for line in table.splitlines() if line]
            self.assertEqual(keys, ["addr", "kind", "a_extra", "z_extra"])

    def test_cfg_edge_serializer_order_determinism(self):
        from analyze import canonical_cfg_report
        from test_elf_bounds import _canonical_cfg_report_reference

        class FakeImage:
            def __init__(self):
                self.sections = []
                self.reloc = None
                self.entry_point = 0x1000

            def read_at_vaddr(self, addr, size):
                if addr == 0x1000:
                    return (0x0C000800).to_bytes(4, "little")  # jal 0x2000
                elif addr == 0x1004:
                    return (0x00000000).to_bytes(4, "little")  # nop
                elif addr == 0x2000:
                    return (0x03E00008).to_bytes(4, "little")  # jr ra
                return None

        img = FakeImage()
        forward = [(0x1000, 0x1008), (0x2000, 0x2004)]
        backward = [(0x2000, 0x2004), (0x1000, 0x1008)]
        # The production serializer (codegen --cfg-report) must not depend on
        # the order of its ranges.
        rep1 = canonical_cfg_report(img, ranges=forward, entries=[0x1000])
        rep2 = canonical_cfg_report(img, ranges=backward, entries=[0x1000])
        self.assertEqual(rep1, rep2)
        self.assertTrue(rep1["call_edges"])
        # Cross-check against the test-side reference implementation.
        self.assertEqual(
            rep1, _canonical_cfg_report_reference(img, ranges=backward, entries=[0x1000])
        )


    def test_cfg_edge_serializer_detail_order_determinism(self):
        from analyze import CanonicalCfgState

        st1 = CanonicalCfgState()
        st1.addresses.append(0x1000)
        st1.words.append(0)
        st1.terminators.append(0)
        st1.delay_of.append(0x100000000)
        st1.delay_annulled.append(0)
        st1.owner_single.append(0x100000000)
        st1.owner_reason_masks.append(0)

        # Add two edges from source index 0 (0x1000) to target 0x2000 with same kind 'call' but different details
        # detail 3 = jal, detail 5 = linking-branch
        st1.edge_sources.append(0)
        st1.edge_targets.append(0x2000)
        st1.edge_kinds.append(2)  # call
        st1.edge_details.append(3)  # jal

        st1.edge_sources.append(0)
        st1.edge_targets.append(0x2000)
        st1.edge_kinds.append(2)  # call
        st1.edge_details.append(5)  # linking-branch

        st2 = CanonicalCfgState()
        st2.addresses.append(0x1000)
        st2.words.append(0)
        st2.terminators.append(0)
        st2.delay_of.append(0x100000000)
        st2.delay_annulled.append(0)
        st2.owner_single.append(0x100000000)
        st2.owner_reason_masks.append(0)

        # Reverse edge insertion order
        st2.edge_sources.append(0)
        st2.edge_targets.append(0x2000)
        st2.edge_kinds.append(2)  # call
        st2.edge_details.append(5)  # linking-branch

        st2.edge_sources.append(0)
        st2.edge_targets.append(0x2000)
        st2.edge_kinds.append(2)  # call
        st2.edge_details.append(3)  # jal

        rep1 = st1.to_report()
        rep2 = st2.to_report()

        self.assertEqual(rep1["call_edges"], rep2["call_edges"])
        self.assertEqual(rep1["edges"], rep2["edges"])


class TestMergeSpans(unittest.TestCase):
    def test_merge_spans_empty_touching_overlapping_and_drop_empty(self):
        import analyze

        with self.assertRaises(TypeError):
            analyze.merge_spans([])  # drop_empty is keyword-only
        with self.assertRaises(TypeError):
            analyze.merge_spans([], True)

        cases = [
            ([], [], []),
            ([(0x1000, 0x1000)], [], [(0x1000, 0x1000)]),
            ([(0x2000, 0x1000)], [], [(0x2000, 0x1000)]),
            (
                [(0x1000, 0x1000), (0x2000, 0x2010)],
                [(0x2000, 0x2010)],
                [(0x1000, 0x1000), (0x2000, 0x2010)],
            ),
            (
                [(0x1000, 0x1000), (0x1000, 0x1020), (0x1010, 0x1010), (0x1020, 0x1020)],
                [(0x1000, 0x1020)],
                [(0x1000, 0x1020)],
            ),
            (
                [(0x2000, 0x2010), (0x1000, 0x1010), (0x1010, 0x2000)],
                [(0x1000, 0x2010)],
                [(0x1000, 0x2010)],
            ),
            (
                [(0x1000, 0x1040), (0x1010, 0x1020), (0x1030, 0x1060), (0x2000, 0x2010)],
                [(0x1000, 0x1060), (0x2000, 0x2010)],
                [(0x1000, 0x1060), (0x2000, 0x2010)],
            ),
        ]
        for raw, expected_drop, expected_keep in cases:
            with self.subTest(raw=raw):
                self.assertEqual(analyze.merge_spans(iter(raw), drop_empty=True), expected_drop)
                self.assertEqual(analyze.merge_spans(iter(raw), drop_empty=False), expected_keep)

    def test_merge_spans_matches_all_four_call_sites(self):
        from types import SimpleNamespace
        import analyze

        sample = [
            (0x3000, 0x3000),
            (0x2000, 0x2010),
            (0x1000, 0x1020),
            (0x1010, 0x1030),
            (0x1030, 0x1040),
        ]
        drop_expected = analyze.merge_spans(sample, drop_empty=True)
        keep_expected = analyze.merge_spans(sample, drop_empty=False)

        # 1. _SpanIndex (drop_empty=True)
        index = analyze._SpanIndex(sample)
        self.assertEqual(list(zip(index.starts, index.ends, strict=True)), drop_expected)

        # 2. _cfg_normalize_ranges (drop_empty=True, keeps its own range validation)
        self.assertEqual(analyze._cfg_normalize_ranges(sample), drop_expected)
        with self.assertRaisesRegex(ValueError, "invalid CFG range"):
            analyze._cfg_normalize_ranges([(-4, 4)])

        # 3. exec_ranges (drop_empty=False, via extra_spans on an un-relocated image)
        fake_elf = SimpleNamespace(sections=[], shnum=0, shentsize=0, reloc=None, base=0)
        self.assertEqual(analyze.exec_ranges(fake_elf, extra_spans=sample), keep_expected)

        # 4. _Discovery.trace_ranges_for_entry (drop_empty=False)
        disc = object.__new__(analyze._Discovery)
        disc.ranges = [(0x1000, 0x1020), (0x3000, 0x3000)]
        disc.file_exec_ranges = [(0x1010, 0x1040)]
        disc.proven_spans = []
        self.assertEqual(
            disc.trace_ranges_for_entry(0x1028),
            analyze.merge_spans([*disc.ranges, (0x1010, 0x1040)], drop_empty=False),
        )

    def test_merge_spans_many_overlapping_and_touching_spans(self):
        import analyze

        spans = [(0x1000 + i * 4, 0x1000 + i * 4 + 8) for i in range(5000)]
        spans.append((0x80000, 0x80000))
        spans.reverse()
        expected_end = 0x1000 + 4999 * 4 + 8
        self.assertEqual(analyze.merge_spans(spans, drop_empty=True), [(0x1000, expected_end)])
        self.assertEqual(
            analyze.merge_spans(spans, drop_empty=False),
            [(0x1000, expected_end), (0x80000, 0x80000)],
        )
        self.assertEqual(analyze._cfg_normalize_ranges(spans), [(0x1000, expected_end)])
        idx = analyze._SpanIndex(spans)
        self.assertEqual(list(zip(idx.starts, idx.ends, strict=True)), [(0x1000, expected_end)])


if __name__ == "__main__":
    unittest.main()
