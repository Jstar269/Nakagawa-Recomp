# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Non-code in an image without relocation evidence (tools/analyze.py).

A code overlay whose text section also holds data has no relocations, so every relocation gate
of the seed sweeps lets a data word through as evidence: a word that reads as a `j`, an `la`
pair, a table of text pointers stored as a vtable or loaded for a `jr`, a frame prologue. The
analysis withdraws every seed the image's structure does not name whose trace reaches a word
that cannot be code. These tests build such an overlay from a seed: 40 real functions, one more
reached only by a call (it holds a valid VFPU instruction the emitter does not support, on a
path that never runs), and 40,000 words of data that plant every one of those seed kinds.
"""

import contextlib
import io
import os
import random
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import analyze  # noqa: E402
import codegen  # noqa: E402

LINK = 0x08C00000
TEXT = LINK + 0x40
BLOB_WORDS = 40000
PROLOGUE = 0x27BDFFE0          # addiu sp, sp, -32
RESERVED = 0x68000000          # opcode 0x1a: no Allegrex instruction
JR_RA = 0x03E00008
VSBN_S = 0x61394C10            # vsbn.s: valid; the emitter has no native form for it
VTABLE = 0x08C10000            # 64 KiB aligned: the sweep's vtable rule reads `lui` alone
JUMP_TABLE = 0x08C20000


def lui(rt, value):
    return (0x0F << 26) | (rt << 16) | (value >> 16)


def addiu(rt, rs, imm):
    return (0x09 << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def real_function(rng, call=None):
    body = [PROLOGUE, 0xAFBF001C]                          # sw ra, 28(sp)
    body += [rng.choice([0x25080001, 0x00851021, 0xAFB00010, 0x8FB00010])
             for _ in range(rng.randint(3, 9))]
    if call is not None:
        body += [(0x03 << 26) | ((call >> 2) & 0x3FFFFFF), 0]   # jal call; nop
    body += [0x11090001, 0, 0x25080001]                    # beq t0, t1, +1; nop; addiu
    body += [0x8FBF001C, JR_RA, 0x27BD0020]                # lw ra; jr ra; addiu sp, sp, 32
    return body


def unsupported_function():
    """A real function reached only by a call: its `bne $a0, $zero` is not taken in any run, and the
    `vsbn.s` it would reach is a valid instruction the emitter does not support."""
    return [PROLOGUE, 0x14800004, 0, 0x8FBF001C, JR_RA, 0x27BD0020, VSBN_S, JR_RA, 0]


class Overlay:
    """The synthetic overlay and the addresses the tests look at."""

    def __init__(self, seed=2373):
        rng = random.Random(seed)
        words, self.starts = [], []
        self.unsupported = None
        # The callee's address is known once the 40 functions are laid out; lay them out twice.
        for _ in range(2):
            words, self.starts = [], []
            rng = random.Random(seed)
            for index in range(40):
                self.starts.append(TEXT + 4 * len(words))
                words += real_function(rng, call=(self.unsupported or TEXT) if index == 0 else None)
            self.unsupported = TEXT + 4 * len(words)
            words += unsupported_function()
        self.code_words = len(words)
        self.blob_lo = TEXT + 4 * len(words)
        blob = [rng.getrandbits(32) for _ in range(BLOB_WORDS)]
        at = lambda address: (address - self.blob_lo) // 4  # noqa: E731
        self.planted = {}

        def target(address, kind):
            # Entry-shaped data: a prologue word, then a word no instruction has.
            blob[at(address)] = PROLOGUE
            blob[at(address) + 1] = RESERVED
            self.planted[address] = kind

        def code(address, fragment):
            blob[at(address) - 2:at(address)] = [0, 0]    # no transfer runs into the fragment
            blob[at(address):at(address) + len(fragment)] = fragment

        base = self.blob_lo + 0x400
        # A pointer-shaped word that reads as `j`, its target after a `jr $ra`: tail promotion.
        jump_target = base + 0x40
        blob[at(jump_target) - 2] = JR_RA
        target(jump_target, "j target")
        blob[at(base)] = (0x02 << 26) | ((jump_target >> 2) & 0x3FFFFFF)
        # An `la` pair naming an entry-shaped word: a high-confidence seed.
        la_target = base + 0x200
        target(la_target, "la pair")
        code(base + 0x100, [lui(8, la_target + 0x8000), addiu(8, 8, la_target)])
        # A `sw` of a pointer to a run of text pointers: the vtable rule.
        vt = [VTABLE + 0x100, VTABLE + 0x140]
        for t in vt:
            target(t, "vtable entry")
        blob[at(VTABLE):at(VTABLE) + 3] = vt + [0]
        code(base + 0x300, [lui(9, VTABLE), (0x2B << 26) | (4 << 21) | (9 << 16)])  # sw t1, 0(a0)
        # A `lw` from a table of text pointers, then `jr` through it: the jump-table rule.
        jt = [JUMP_TABLE + 0x100, JUMP_TABLE + 0x140]
        for t in jt:
            target(t, "jump-table entry")
        blob[at(JUMP_TABLE):at(JUMP_TABLE) + 3] = jt + [0]
        code(base + 0x380, [lui(10, JUMP_TABLE), (0x23 << 26) | (10 << 21) | (11 << 16),
                            (11 << 21) | 0x08, 0])        # lw t3, 0(t2); jr t3; nop
        # A data-section word that equals an address in the data: no relocation says it is a
        # pointer, so it is a seed like the sweeps', not one the format names.
        data_pointer = base + 0x600
        target(data_pointer, "data-section pointer")
        words += blob
        self.blob_hi = TEXT + 4 * len(words)
        text = b"".join(struct.pack("<I", w) for w in words)
        # The real functions' table, then the pointer-shaped data word.
        data = b"".join(struct.pack("<I", a) for a in [*self.starts, data_pointer])
        header = struct.pack("<4s7I", b"MWo3", 1, LINK, len(text), len(data), 0x10,
                             TEXT + len(text), TEXT + len(text)) + b"Synthetic.ovl".ljust(0x20, b"\0")
        self.image = header + text + data

    def in_blob(self, address):
        return self.blob_lo <= address < self.blob_hi


def translate(overlay, flow_sizes=None):
    """translate_runtime_module of the overlay."""
    stock = codegen.function_flow

    def flow(elf, start, *args, **kwargs):
        result = stock(elf, start, *args, **kwargs)
        if flow_sizes is not None:
            flow_sizes.append(len(result[0]))
        return result

    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "Synthetic.ovl")
        with open(path, "wb") as handle:
            handle.write(overlay.image)
        with mock.patch.object(codegen, "function_flow", flow), \
                contextlib.redirect_stderr(io.StringIO()) as err:
            module = codegen.translate_runtime_module(0, path)
    return module, err.getvalue()


def stock_analysis():
    """The analysis as it was before the withdrawal: no candidate is ever withdrawn."""
    return mock.patch.object(analyze, "_non_code_entries", lambda d, structural, cache=None: ({}, set()))


class ReservedEncodingTest(unittest.TestCase):
    def test_reserved_classes(self):
        for word, why in ((0x68000000, "reserved opcode 0x1a"),
                          (0x00000030, "reserved SPECIAL function 0x30"),     # tge
                          (0x00000001, "reserved SPECIAL function 0x01"),
                          (0x04080000, "reserved REGIMM rt 0x08"),            # tgei
                          (0x18010000, "opcode 0x06 with rt 1")):             # blez, rt != 0
            self.assertEqual(analyze.reserved_encoding(word), why)

    def test_defined_forms_are_not_judged(self):
        for funct in analyze._SPECIAL_FUNCTS:
            self.assertIsNone(analyze.reserved_encoding(funct))
        for rt in analyze._REGIMM_RTS:
            self.assertIsNone(analyze.reserved_encoding((1 << 26) | (rt << 16)))
        # COP3 and jalx are named by the PSP toolchain's disassembler: never judged.
        for word in (0x18000000, 0x4C000000, 0x74000000, VSBN_S, PROLOGUE, JR_RA):
            self.assertIsNone(analyze.reserved_encoding(word))


class NonCodeOverlayTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.overlay = Overlay()
        cls.sizes = []
        cls.module, cls.log = translate(cls.overlay, cls.sizes)
        with stock_analysis():
            cls.stock_sizes = []
            cls.stock, _ = translate(cls.overlay, cls.stock_sizes)

    def entries(self, module):
        return {a for a, _ in module.functions}

    def test_every_planted_seed_kind_is_an_entry_without_the_withdrawal(self):
        stock = self.entries(self.stock)
        for address, kind in self.overlay.planted.items():
            self.assertIn(address, stock, kind)
        self.assertGreater(sum(map(self.overlay.in_blob, stock)), 40)

    def test_data_gets_no_entry(self):
        entries = self.entries(self.module)
        for address, kind in self.overlay.planted.items():
            self.assertNotIn(address, entries, kind)
        withdrawn = {a for a, *_ in self.module.non_code}
        self.assertTrue(set(self.overlay.planted) <= withdrawn)
        # What stays in the data is code-shaped to the last word: a few valid words that end in
        # a `j` out of the image (a tail call into another image, for all the analysis can
        # tell) or a `jr` through a register, as a leaf function does. Nothing tells such a run
        # apart from code, so it keeps a (translated) body of its own. Over this seed and eight
        # others, 2 to 5 such runs stayed, of 2 to 15 words; this seed leaves 3.
        elf = analyze.Elf(analyze.code_overlay_elf(self.overlay.image))
        image = analyze.exec_ranges(elf)
        left = [a for a in entries if self.overlay.in_blob(a)]
        self.assertEqual(len(left), 3)
        for a in left:
            insns = sorted(codegen.function_flow(elf, a, self.module.spans, entries)[0])
            self.assertLessEqual(len(insns), 16)
            words = [codegen.read32(elf, word_address) for word_address in insns]
            for word in words:
                self.assertIsNone(analyze.reserved_encoding(word))
            last = words[-2]                                     # the delay slot follows it
            target = (insns[-2] & 0xF0000000) | ((last & 0x3FFFFFF) << 2)
            self.assertTrue((last >> 26 == 0x02 and not analyze.in_ranges(target, image))
                            or (last >> 26 == 0 and last & 0x3F == 0x08), hex(last))

    def test_every_real_function_keeps_its_translated_body(self):
        stubbed = {a for a, _ in self.module.stubbed}
        entries = self.entries(self.module)
        for start in self.overlay.starts:
            self.assertIn(start, entries)
            self.assertNotIn(start, stubbed)

    def test_a_valid_unsupported_instruction_is_no_evidence(self):
        callee = self.overlay.unsupported
        self.assertIn(callee, self.entries(self.module))
        self.assertNotIn(callee, {a for a, *_ in self.module.non_code})
        # The emitter hands vsbn.s to the VFPU interpreter at run time: the body is translated,
        # word for word as without the change.
        self.assertNotIn(callee, dict(self.module.stubbed))
        text = dict(zip((a for a, _ in self.module.functions), self.module.texts, strict=True))[callee]
        stock = dict(zip((a for a, _ in self.stock.functions), self.stock.texts, strict=True))[callee]
        self.assertIn(f"sr_vfpu_interp(s,0x{VSBN_S:08x}u)", text)
        self.assertEqual(str(text), str(stock))

    def test_body_sizes_and_codegen_are_bounded_by_the_code(self):
        self.assertLessEqual(sum(self.sizes), 2 * self.overlay.code_words)
        self.assertGreater(sum(self.stock_sizes), 10 * self.overlay.code_words)

    def test_withdrawn_words_leave_the_spans_and_are_named(self):
        words = {a for lo, hi in self.module.non_code_spans for a in range(lo, hi, 4)}
        self.assertTrue(words)
        for lo, hi in self.module.spans:
            self.assertFalse(any(lo <= a < hi for a in words))
        self.assertIn("withdrew", self.log)
        lines = "\n".join(codegen.runtime_module_descriptor_lines(self.module))
        self.assertIn("static const SrModuleNonCode sr_m0_non_code = {", lines)
        self.assertNotIn("SrModuleNonCode", "\n".join(
            codegen.runtime_module_descriptor_lines(self.stock)))

    def test_two_runs_write_the_same_bytes_whatever_the_candidate_order(self):
        def shuffled(seed):
            def order(entries):
                items = sorted(entries)
                random.Random(seed).shuffle(items)
                return items
            return order

        outputs = []
        for seed in (1, 2):
            with mock.patch.object(analyze, "_entry_order", shuffled(seed)):
                module, _ = translate(self.overlay)
            outputs.append(("\n".join(codegen.runtime_module_descriptor_lines(module)),
                            [str(t) for t in module.texts], module.non_code, module.spans))
        expected = ("\n".join(codegen.runtime_module_descriptor_lines(self.module)),
                    [str(t) for t in self.module.texts], self.module.non_code, self.module.spans)
        self.assertEqual(outputs, [expected, expected])

    def test_the_withdrawal_only_grows_and_ends(self):
        passes = []
        stock = analyze._non_code_entries

        def recording(d, structural, cache=None):
            found, reached = stock(d, structural, cache)
            passes.append((frozenset(d.non_code), frozenset(found)))
            return found, reached

        with mock.patch.object(analyze, "_non_code_entries", recording):
            translate(self.overlay)
        self.assertGreaterEqual(len(passes), 2)
        for (before, found), (after, _) in zip(passes, passes[1:], strict=False):
            self.assertTrue(found)                      # a pass that continues withdraws ...
            self.assertFalse(found & before)            # ... only new addresses ...
            self.assertEqual(after, before | found)     # ... and the set only grows
        self.assertFalse(passes[-1][1])                 # the last pass withdraws nothing

    def test_an_image_with_relocation_evidence_is_untouched(self):
        def refuse(d, structural, cache=None):
            raise AssertionError("an image with relocation evidence was checked")

        with mock.patch.object(analyze, "_relocated_words", lambda elf: {}), \
                mock.patch.object(analyze, "_non_code_entries", refuse):
            module, _ = translate(self.overlay)
        self.assertEqual(module.non_code, ())


class TraceFunctionTest(unittest.TestCase):
    def test_a_jump_out_of_the_image_is_unchanged_for_the_discovery(self):
        # The checking trace ends a path at a `j` out of the image; the discovery's own trace
        # keeps running past it, as before.
        words = [PROLOGUE, (0x02 << 26) | ((0x08900000 >> 2) & 0x3FFFFFF), 0, 0x25080001, JR_RA, 0]
        text = b"".join(struct.pack("<I", w) for w in words)
        header = struct.pack("<4s7I", b"MWo3", 1, LINK, len(text), 0, 0, TEXT + len(text),
                             TEXT + len(text)) + b"j.ovl".ljust(0x20, b"\0")
        elf = analyze.Elf(analyze.code_overlay_elf(header + text))
        ranges = analyze.exec_ranges(elf)
        covered = set()
        analyze.trace_function(elf, TEXT, ranges, covered, set(), set())
        self.assertIn(TEXT + 16, covered)
        covered = set()
        analyze.trace_function(elf, TEXT, ranges, covered, set(), set(), end_at_outside_jump=True)
        self.assertIn(TEXT + 8, covered)                 # the delay slot
        self.assertNotIn(TEXT + 12, covered)

    def test_a_tail_many_entries_share_is_checked_once(self):
        # 32 functions jump into one 256-word tail, and one word past the code cannot be
        # code, so the check runs. Each pass of the check walks the tail once, not once per
        # function: it reads each word of the image at most twice (the trace's read and its
        # entry-boundary test), where a walk per function would read the tail 32 times.
        tail = [0x01004021] * 256 + [JR_RA, 0]                   # addu t0, t0, zero
        words = list(tail)
        for _ in range(32):
            words += [PROLOGUE, (0x02 << 26) | ((TEXT >> 2) & 0x3FFFFFF), 0]
        words += [RESERVED]
        text = b"".join(struct.pack("<I", w) for w in words)
        header = struct.pack("<4s7I", b"MWo3", 1, LINK, len(text), 0, 0, TEXT + len(text),
                             TEXT + len(text)) + b"tail.ovl".ljust(0x20, b"\0")
        image = header + text
        elf = analyze.Elf(analyze.code_overlay_elf(image))
        read, reads, per_check = elf.read_at_vaddr, [0], []

        def counting(vaddr, n):
            reads[0] += 1
            return read(vaddr, n)

        stock = analyze._non_code_entries

        def counted(d, structural, cache=None):
            before = reads[0]
            result = stock(d, structural, cache)
            per_check.append(reads[0] - before)
            return result

        elf.read_at_vaddr = counting
        with contextlib.redirect_stderr(io.StringIO()), \
                mock.patch.object(analyze, "_non_code_entries", counted):
            starts, _ = analyze.analyze(elf)
        with contextlib.redirect_stderr(io.StringIO()), stock_analysis():
            stock_starts, _ = analyze.analyze(analyze.Elf(analyze.code_overlay_elf(image)))
        # The word after the last function's `j` seeds an entry; it cannot be code.
        self.assertEqual(sorted(starts),
                         sorted(a for a in stock_starts if a != TEXT + 4 * (len(words) - 1)))
        self.assertEqual(len(starts), 33)                       # the tail and the 32
        self.assertEqual(len(per_check), 2)                     # one withdrawal, then none
        for count in per_check:
            self.assertLessEqual(count, 2 * len(words) + 8)

    def test_a_delay_slot_an_earlier_check_covered_does_not_end_a_path(self):
        # The first function's `j` covers its delay slot without expanding it. The data seed
        # after it branches to that word and runs on into a reserved word: the check of the
        # data seed must see that word, though the delay slot is already covered by a check
        # that reached none.
        first = TEXT
        callee = TEXT + 4 * 8
        data_seed = TEXT + 4 * 11
        slot = first + 4 * 4
        words = [PROLOGUE, (0x03 << 26) | ((data_seed >> 2) & 0x3FFFFFF), 0,   # jal data_seed
                 (0x02 << 26) | ((callee >> 2) & 0x3FFFFFF), 0,               # j callee; slot
                 RESERVED, JR_RA, 0,
                 PROLOGUE, JR_RA, 0x27BD0020,                                  # the callee
                 PROLOGUE, 0x10000000 | ((slot - (data_seed + 8)) >> 2 & 0xFFFF), 0,  # b slot
                 JR_RA, 0]
        text = b"".join(struct.pack("<I", w) for w in words)
        header = struct.pack("<4s7I", b"MWo3", 1, LINK, len(text), 0, 0, TEXT + len(text),
                             TEXT + len(text)) + b"slot.ovl".ljust(0x20, b"\0")
        report = {}
        with contextlib.redirect_stderr(io.StringIO()):
            starts, _ = analyze.analyze(analyze.Elf(analyze.code_overlay_elf(header + text)),
                                        mixed_code_data=True, report=report)
        self.assertIn(data_seed, report["non_code"])
        self.assertNotIn(data_seed, starts)
        self.assertIn(first, starts)
        self.assertIn(callee, starts)


if __name__ == "__main__":
    unittest.main()
