# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Non-code in an image without relocation evidence (tools/analyze.py).

A code overlay whose text section also holds data has no relocations, so every relocation gate
of the seed sweeps lets a data word through as evidence: a word that reads as a `j`, an `la`
pair, a table of text pointers stored as a vtable or loaded for a `jr`, a frame prologue. The
analysis withdraws every seed the image's structure does not name whose generated body would
hold a word that cannot be code, unless a body that stays reaches it. These tests build such an
overlay from a seed: 40 real functions, one more reached only by a call (it holds a valid VFPU
instruction the emitter does not support, on a path that never runs), and 34,000 words of data
that plant every one of those seed kinds; small images for each rule; and the public fixtures'
code, with and without data after it.
"""

import contextlib
import importlib.util
import io
import os
import pathlib
import random
import re
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
BLOB_WORDS = 34000
PROLOGUE = 0x27BDFFE0          # addiu sp, sp, -32
RESERVED = 0x68000000          # opcode 0x1a: a reserved opcode
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
    return mock.patch.object(analyze, "_non_code_entries",
                             lambda d, structural, cache=None: ({}, frozenset()))


class ReservedEncodingTest(unittest.TestCase):
    def test_reserved_classes(self):
        for word, why in ((0x68000000, "reserved opcode 0x1a"),
                          (0x00000030, "reserved SPECIAL function 0x30"),     # tge
                          (0x00000001, "reserved SPECIAL function 0x01"),
                          (0x04080000, "reserved REGIMM rt 0x08"),            # tgei
                          (0x18010000, "opcode 0x06 with rt 1")):             # blez, rt != 0
            self.assertEqual(analyze.reserved_encoding(word), why)

    def test_defined_forms_are_not_judged(self):
        # The Allegrex SPECIAL functions and REGIMM rt values, listed here rather than read
        # from the tables under test.
        special = (0x00, 0x02, 0x03, 0x04, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,
                   0x0F, 0x10, 0x11, 0x12, 0x13, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C,
                   0x1D, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B, 0x2C,
                   0x2D, 0x2E, 0x2F)
        for funct in special:
            self.assertIsNone(analyze.reserved_encoding(funct), hex(funct))
        for funct in set(range(0x40)) - set(special):
            self.assertIsNotNone(analyze.reserved_encoding(funct), hex(funct))
        for rt in (0x00, 0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13):
            self.assertIsNone(analyze.reserved_encoding((1 << 26) | (rt << 16)), hex(rt))
        # COP3 and jalx are named by the PSP toolchain's disassembler: never judged.
        for word in (0x18000000, 0x4C000000, 0x74000000, VSBN_S, PROLOGUE, JR_RA):
            self.assertIsNone(analyze.reserved_encoding(word))


class NonCodeOverlayTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.overlay = Overlay()
        cls.sizes = []
        cls.passes = []
        stock = analyze._non_code_entries

        def recording(d, structural, cache=None):
            found, void = stock(d, structural, cache)
            cls.passes.append((frozenset(d.non_code), frozenset(found)))
            return found, void

        with mock.patch.object(analyze, "_non_code_entries", recording):
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
        # tell), a `jr` through a register, as a leaf function does, or a transfer to another
        # entry, where a generated body ends. Nothing tells such a run apart from code, so it
        # keeps a body of its own. Over seeds 1 to 8, 1 to 7 such runs stayed, of 1 to 15
        # words. This seed leaves 2, each ending in a `j` out of the image or a `jr`.
        elf = analyze.Elf(analyze.code_overlay_elf(self.overlay.image))
        image = analyze.exec_ranges(elf)
        left = [a for a in entries if self.overlay.in_blob(a)]
        self.assertEqual(len(left), 2)
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

    def test_no_withdrawal_takes_a_translation_away(self):
        # Every withdrawn address that was an entry before had a body the emitter could not
        # translate (a stub): a withdrawal is judged on the body code generation emits.
        withdrawn = {a for a, *_ in self.module.non_code}
        before = self.entries(self.stock)
        self.assertTrue(withdrawn & before)
        self.assertLessEqual(withdrawn & before, {a for a, _ in self.stock.stubbed})

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
        for seed in (1,):
            with mock.patch.object(analyze, "_entry_order", shuffled(seed)):
                module, _ = translate(self.overlay)
            outputs.append(("\n".join(codegen.runtime_module_descriptor_lines(module)),
                            [str(t) for t in module.texts], module.non_code, module.spans))
        expected = ("\n".join(codegen.runtime_module_descriptor_lines(self.module)),
                    [str(t) for t in self.module.texts], self.module.non_code, self.module.spans)
        self.assertEqual(outputs, [expected])

    def test_the_withdrawal_only_grows_and_ends(self):
        passes = self.passes
        self.assertGreaterEqual(len(passes), 2)
        for (before, found), (after, _) in zip(passes, passes[1:], strict=False):
            self.assertTrue(found)                      # a pass that continues withdraws ...
            self.assertFalse(found & before)            # ... only new addresses ...
            self.assertEqual(after, before | found)     # ... and the set only grows
        self.assertFalse(passes[-1][1])                 # the last pass withdraws nothing


class TraceFunctionTest(unittest.TestCase):
    def test_a_jump_out_of_the_image_ends_a_body_not_the_discovery(self):
        # A generated body ends at a `j` out of the image, so the body walk does; the
        # discovery's own trace keeps running past it, as before.
        words = [PROLOGUE, (0x02 << 26) | ((0x08900000 >> 2) & 0x3FFFFFF), 0, 0x25080001, JR_RA, 0]
        elf = small_overlay(words)
        ranges = analyze.exec_ranges(elf)
        covered = set()
        analyze.trace_function(elf, TEXT, ranges, covered, set(), set())
        self.assertIn(TEXT + 16, covered)
        walk = analyze._BodyWalk(elf, ranges, frozenset({TEXT}), None)
        self.assertEqual(walk.reach([TEXT]), {TEXT, TEXT + 4, TEXT + 8})
        flow = codegen.function_flow(elf, TEXT, ranges, {TEXT})[0]
        self.assertEqual(flow, {TEXT, TEXT + 4, TEXT + 8})

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


def small_overlay(words, data_words=(), name=b"small.ovl"):
    """An overlay of ``words`` at TEXT, its data section holding ``data_words``."""
    return analyze.Elf(analyze.code_overlay_elf(overlay_image(words, data_words, name)))


def jal(target):
    return (0x03 << 26) | ((target >> 2) & 0x3FFFFFF)


def branch(op, rs, rt, at, target):
    return (op << 26) | (rs << 21) | (rt << 16) | (((target - (at + 4)) >> 2) & 0xFFFF)


ADDU = 0x01004021                 # addu t0, t0, zero


def jump(target):
    return (0x02 << 26) | ((target >> 2) & 0x3FFFFFF)


def overlay_image(words, data_words=(), name=b"small.ovl"):
    """The bytes of an overlay of ``words`` at TEXT, its data section holding ``data_words``."""
    text = b"".join(struct.pack("<I", w) for w in words)
    data = b"".join(struct.pack("<I", w) for w in data_words)
    header = struct.pack("<4s7I", b"MWo3", 1, LINK, len(text), len(data), 0x10 if data else 0,
                         TEXT + len(text), TEXT + len(text)) + name.ljust(0x20, b"\0")
    return header + text + data


def translate_image(image):
    """translate_runtime_module of an overlay's bytes, and what it wrote to stderr."""
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "small.ovl")
        pathlib.Path(path).write_bytes(image)
        with contextlib.redirect_stderr(io.StringIO()) as err:
            module = codegen.translate_runtime_module(0, path)
    return module, err.getvalue()


def fixed_executable(words, base, text=None):
    """A fixed-address ELF executable (no relocations) holding ``words`` (or ``text``) at ``base``."""
    text = b"".join(struct.pack("<I", w) for w in words) if text is None else text
    payload_off = 52 + 32
    blob = bytearray(payload_off + len(text))
    blob[:8] = b"\x7fELF\x01\x01\x01\x00"
    struct.pack_into("<HHIIIIIHHHHHH", blob, 16, 2, 8, 1, base, 52, 0, 0, 52, 32, 1, 0, 0, 0)
    struct.pack_into("<8I", blob, 52, 1, payload_off, base, base, len(text), len(text), 5, 4)
    blob[payload_off:] = text
    return bytes(blob)


def run_codegen(blob, extra=()):
    """codegen.main on the executable ``blob``: (rc, stderr, every generated .c and .h).
    ``extra`` is ((bytes, link address), ...) passed as --extra-elf images."""
    with tempfile.TemporaryDirectory() as tmp:
        elf_path = os.path.join(tmp, "exec.elf")
        out_c = os.path.join(tmp, "exec.c")
        pathlib.Path(elf_path).write_bytes(blob)
        args = []
        for index, (image, address) in enumerate(extra):
            path = os.path.join(tmp, f"extra{index}.elf")
            pathlib.Path(path).write_bytes(image)
            args.append(f"--extra-elf={path}@{address:x}")
        env = {k: v for k, v in os.environ.items()
               if k not in ("TITLE_EXTRA_SPANS", "HST_EXTRA_SPANS", "GAME_BASE")}
        with mock.patch.dict(os.environ, env, clear=True), \
                contextlib.redirect_stderr(io.StringIO()) as err, \
                contextlib.redirect_stdout(io.StringIO()):
            rc = codegen.main(["codegen.py", elf_path, out_c, "--base=0", "--profile=none", *args])
        generated = "".join(pathlib.Path(tmp, f).read_text(encoding="utf-8")
                            for f in sorted(os.listdir(tmp)) if f.endswith((".c", ".h")))
    return rc, err.getvalue(), generated


def function_text(generated, address):
    """The C body of the function at ``address`` in generated code."""
    start = generated.index(f"void f_{address:08x}(CpuState *s) {{")
    return generated[start:generated.index("\n}", start) + 2]


class WithdrawalRulesTest(unittest.TestCase):
    def test_an_entry_that_stays_keeps_all_it_reaches(self):
        # S, a code capture's entry, calls X and branches to C, its return, before it runs
        # into a reserved word; X, a call target only, branches to the same C. X is withdrawn;
        # S stays, and C, which S reaches past the reserved word, stays code.
        s, c, x = TEXT, TEXT + 20, TEXT + 32
        words = [jal(x), 0, branch(4, 8, 9, TEXT + 8, c), 0, RESERVED,       # S
                 ADDU, JR_RA, 0,                                              # C
                 branch(5, 8, 9, x, x + 16), 0, branch(4, 0, 0, x + 8, c), 0,  # X
                 RESERVED]
        report = {}
        with contextlib.redirect_stderr(io.StringIO()):
            starts, _ = analyze.analyze(small_overlay(words), entries=[s], report=report)
        self.assertIn(x, report["non_code"])
        self.assertIn(s, starts)
        self.assertFalse({c, c + 4, c + 8} & report["non_code_words"])

    def test_each_verdict_is_the_one_its_body_gives_alone(self):
        # A calls T and runs on into it; B branches into the run before T. One walk over all
        # three gives each the verdict a walk of it alone gives, in any order, and names the
        # lowest reserved word its body holds.
        a, w, t, b = TEXT, TEXT + 8, TEXT + 12, TEXT + 20
        words = [jal(t), 0, ADDU, ADDU, RESERVED, branch(4, 0, 0, b, w), 0, JR_RA, 0, RESERVED]
        elf = small_overlay(words)
        ranges = analyze.exec_ranges(elf)
        evidence = analyze._non_code_evidence(elf, ranges, {})
        boundaries = frozenset({a, t, b})

        def verdicts(starts):
            return analyze._BodyWalk(elf, ranges, boundaries, None).reserved(starts, evidence)

        alone = {}
        for start in (a, t, b):
            alone.update(verdicts([start]))
        self.assertEqual(alone, {t: (TEXT + 16, "reserved opcode 0x1a")})  # A ends at T, B at T
        for order in ((a, t, b), (b, t, a), (t, a, b)):
            self.assertEqual(verdicts(list(order)), alone, [hex(e) for e in order])

    def test_shared_runs_are_walked_once(self):
        # 500 seeds shaped like prologues, 8 words apart, share one run of valid words that
        # ends in a reserved word: each is withdrawn, and a pass reads each word a bounded
        # number of times, not once per seed that reaches it.
        words = []
        for _ in range(500):
            words += [PROLOGUE] + [ADDU] * 7
        words += [RESERVED]
        elf = small_overlay(words)
        read, reads, per_pass = elf.read_at_vaddr, [0], []

        def counting(vaddr, n):
            reads[0] += 1
            return read(vaddr, n)

        stock = analyze._non_code_entries

        def counted(d, structural, cache=None):
            before = reads[0]
            result = stock(d, structural, cache)
            per_pass.append(reads[0] - before)
            return result

        elf.read_at_vaddr = counting
        report = {}
        with contextlib.redirect_stderr(io.StringIO()), \
                mock.patch.object(analyze, "_non_code_entries", counted):
            analyze.analyze(elf, report=report)
        self.assertEqual(len(report["non_code"]), 500)
        for count in per_pass:
            self.assertLessEqual(count, 3 * len(words))

    def test_a_clean_body_stays_when_the_entry_it_jumps_to_is_withdrawn(self):
        # G (named by a data word) calls F and K. F returns on its hot path; its cold path
        # tail-jumps to K (S1b), or calls a no-return function and runs on into K (S1c). K's
        # cold path calls the no-return function and runs into a data word. K's generated body
        # holds that word, so it was a stub and is withdrawn; F's body ends at K, holds no
        # such word, and keeps its translation, as G does.
        g, f, k = TEXT, TEXT + 32, TEXT + 64
        abort = k + 32
        s1b = [PROLOGUE, jal(f), 0, jal(k), 0, 0x8FBF001C, JR_RA, 0x27BD0020,
               PROLOGUE, branch(5, 4, 0, f + 4, f + 20), 0, 0x8FBF001C, JR_RA, 0x27BD0020,
               jump(k), 0,
               ADDU, branch(5, 5, 0, k + 4, k + 16), 0, JR_RA, 0,
               jal(abort), 0, RESERVED,
               PROLOGUE, branch(4, 0, 0, abort + 4, abort + 4), 0]
        abort = k + 36
        s1c = [PROLOGUE, jal(f), 0, jal(k), 0, 0x8FBF001C, JR_RA, 0x27BD0020,
               ADDU, branch(5, 4, 0, f + 4, f + 24), 0, ADDU, JR_RA, 0,
               jal(abort), 0,
               ADDU, branch(5, 5, 0, k + 4, k + 20), 0, ADDU, JR_RA, 0,
               jal(abort), 0, RESERVED,
               PROLOGUE, branch(4, 0, 0, abort + 4, abort + 4), 0]
        for name, words in (("S1b", s1b), ("S1c", s1c)):
            with self.subTest(name):
                image = overlay_image(words, [g])
                module, _ = translate_image(image)
                with stock_analysis():
                    before, _ = translate_image(image)
                stubbed_before = {a for a, _ in before.stubbed}
                withdrawn = {a for a, *_ in module.non_code}
                self.assertIn(k, withdrawn)
                self.assertLessEqual(withdrawn, set(range(k, abort, 4)))   # K and its labels
                self.assertIn(k, stubbed_before)
                for kept in (g, f):
                    self.assertIn(kept, {a for a, _ in module.functions})
                    self.assertNotIn(kept, {a for a, _ in module.stubbed})
                    self.assertNotIn(kept, stubbed_before)

    def test_a_kept_entry_keeps_a_prologue_it_runs_through(self):
        # The entry point of a fixed-address executable runs into a prologue at 0x1004 that
        # starts its frame; a cold path tail-jumps to K, whose cold path runs into a data
        # word. K is withdrawn; the entry's body translates the same words as before (its `j K`
        # now goes through checked dispatch), and its spans do not split at the prologue.
        base, cold, k, kcold, abort = 0x1000, 0x1020, 0x1028, 0x103C, 0x1048
        words = [ADDU,
                 PROLOGUE, branch(5, 4, 0, 0x1008, cold), 0, 0x8FBF001C, JR_RA, 0x27BD0020, 0,
                 jump(k), 0,
                 ADDU, branch(5, 5, 0, 0x102C, kcold), 0, JR_RA, 0,
                 jal(abort), 0, RESERVED,
                 PROLOGUE, branch(4, 0, 0, abort + 4, abort + 4), 0,
                 PROLOGUE, jal(k), 0, 0x8FBF001C, JR_RA, 0x27BD0020]
        rc, log, generated = run_codegen(fixed_executable(words, base))
        with stock_analysis():
            rc0, _, before = run_codegen(fixed_executable(words, base))
        self.assertEqual((rc, rc0), (0, 0), log)
        def translated(text):
            return re.findall(r"sr_begin\(s, (0x[0-9a-f]+)u", function_text(text, base))

        self.assertEqual(translated(generated), translated(before))
        self.assertEqual(len(translated(generated)), 9)   # 0x1000 to 0x1024 but the padding at 0x101c
        self.assertNotIn("sr_unimplemented", function_text(generated, base))
        self.assertIn("sr_exec_span_register(0x00001000u, 0x00001028u)", generated)
        self.assertNotIn(f"void f_{k:08x}(CpuState *s) {{", generated)
        self.assertIn("executable: withdrew", log)

    def test_a_kept_entry_keeps_an_entry_it_branches_to(self):
        # The entry point branches to T, an entry whose own body holds a reserved word. The
        # generated entry body may run into T's words on that branch, so T stays code and the
        # build passes, as it did before.
        base, t = 0x1000, 0x1010
        words = [branch(4, 4, 0, base, t), 0, JR_RA, 0,
                 PROLOGUE, branch(4, 8, 9, t + 4, t + 16), 0, RESERVED,
                 ADDU, JR_RA, 0x27BD0020]
        rc, log, generated = run_codegen(fixed_executable(words, base))
        self.assertEqual(rc, 0, log)
        self.assertNotIn("NON_CODE_TRANSLATED", log)
        self.assertNotIn("withdrew", log)

    def test_an_entry_in_a_kept_delay_slot_keeps_its_own_body(self):
        # K, E and W are named by data words. E is the delay slot of K's `j`; E is a `j` too,
        # whose delay slot W is a data word, so E's body holds it and was a stub. W is
        # withdrawn. E stays, as K's body holds its word, and code generation still walks
        # E's body: W's word stays out of the void, and the module translates.
        k, e, w, t = TEXT, TEXT + 4, TEXT + 8, TEXT + 12
        image = overlay_image([jump(t), jump(t), RESERVED, JR_RA, 0], [k, e, w])
        module, log = translate_image(image)
        self.assertNotIn("NON_CODE_TRANSLATED", log)
        self.assertEqual({a for a, *_ in module.non_code}, {w})
        self.assertIn(e, {a for a, _ in module.functions})
        self.assertIn(e, {a for a, _ in module.stubbed})
        void = {a for lo, hi in module.non_code_spans for a in range(lo, hi, 4)}
        self.assertNotIn(w, void)

    def test_a_seed_a_run_reached_stays_whoever_seeded_it_first(self):
        # The entry point materializes X (an `la` pair), so a sweep seeds it; a run's dispatch
        # miss names it too. X's cold path runs into a data word after a no-return call. A seed
        # a run reached is structural however it was seeded: it stays, and is applied.
        base = 0x1000
        x = base + 16
        ok, abort = x + 24, x + 32
        words = [0x3C080000 | (x >> 16), 0x25080000 | (x & 0xFFFF), JR_RA, 0,
                 PROLOGUE, branch(4, 4, 0, x + 4, ok), 0, jal(abort), 0, RESERVED,
                 JR_RA, 0x27BD0020,
                 PROLOGUE, branch(4, 0, 0, abort + 4, abort + 4), 0]
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "e.elf")
            pathlib.Path(path).write_bytes(fixed_executable(words, base))
            for seeds in (set(), {x}):
                report = {}
                with contextlib.redirect_stderr(io.StringIO()):
                    starts, _ = analyze.analyze(analyze.Elf(path, base=0), report=report,
                                                seeds=seeds)
                if seeds:
                    self.assertIn(x, starts)
                    self.assertNotIn(x, report["non_code"])
                    self.assertEqual(report["miss_seeds"], {x: "applied"})
                else:
                    self.assertIn(x, report["non_code"])        # without the run, it is data

    def test_reserved_words_are_refused_by_the_emitter(self):
        # A word judged reserved is no control transfer and makes the emitter refuse: judging
        # one as evidence of data never takes a translatable word away. Every opcode, SPECIAL
        # function and REGIMM rt, with random other fields.
        rng = random.Random(7)
        words = [(op << 26) | rng.getrandbits(26) for op in range(64) for _ in range(32)]
        words += [(rng.getrandbits(20) << 6) | funct for funct in range(64) for _ in range(8)]
        words += [(1 << 26) | (rng.getrandbits(5) << 21) | (rt << 16) | rng.getrandbits(16)
                  for rt in range(32) for _ in range(8)]
        judged = [w for w in words if analyze.reserved_encoding(w)]
        self.assertGreater(len(judged), 500)
        for word in judged:
            self.assertFalse(codegen.is_control(word), hex(word))
            with self.assertRaises(codegen.Unsupported, msg=hex(word)):
                codegen.effect(0x1000, word)

    def test_a_withdrawn_seed_still_ends_the_body_before_it(self):
        # G calls N and runs on into X, a data seed (a data-section pointer) shaped like an
        # entry. X is withdrawn; G stopped at X before, and stays a function that still stops
        # there, as its generated body does at X's dispatch boundary.
        g, x, n = TEXT + 8, TEXT + 20, TEXT + 28
        words = [JR_RA, 0,                               # a leaf at the segment start
                 PROLOGUE, jal(n), 0,                    # G
                 PROLOGUE, RESERVED,                     # X
                 PROLOGUE, JR_RA, 0x27BD0020]            # N
        report = {}
        with contextlib.redirect_stderr(io.StringIO()):
            starts, _ = analyze.analyze(small_overlay(words, [g, x]), mixed_code_data=True,
                                        report=report)
        self.assertIn(x, report["non_code"])
        self.assertNotIn(g, report["non_code"])
        self.assertIn(g, starts)
        self.assertEqual(report["non_code_words"], frozenset({x, x + 4}))


class ExecutableNonCodeTest(unittest.TestCase):
    def test_withdrawn_words_of_the_executable_leave_its_spans(self):
        # A fixed-address executable whose entry calls a data seed: the seed is withdrawn,
        # gets no body, and its words leave the spans the interpreter may run.
        # Its body runs on past the reserved word into a return: all of it leaves.
        base = 0x1000
        words = [jal(base + 16), 0, JR_RA, 0,
                 PROLOGUE, RESERVED, JR_RA, 0]
        rc, log, generated = run_codegen(fixed_executable(words, base))
        self.assertEqual(rc, 0, log)
        self.assertIn("codegen: executable: withdrew 1 seeds as non-code, 4 words only they "
                      "reach, 1 executable spans left (no relocation evidence; first reserved "
                      "word: reserved opcode (1))", log)
        self.assertNotIn("void f_00001010(CpuState *s) {", generated)
        self.assertIn("sr_exec_span_register(0x00001000u, 0x00001010u)", generated)
        self.assertNotIn("sr_exec_span_register(0x00001000u, 0x00001020u)", generated)

    def test_an_extra_image_is_handled_as_the_executable_is(self):
        # An --extra-elf image without relocation evidence: its withdrawn seed gets no body,
        # the words only it reaches leave the spans, and the summary line names the image.
        base, extra = 0x1000, 0x2000
        words = [JR_RA, 0]
        extra_words = [jal(extra + 16), 0, JR_RA, 0, PROLOGUE, RESERVED, JR_RA, 0]
        rc, log, generated = run_codegen(fixed_executable(words, base),
                                         [(fixed_executable(extra_words, extra), extra)])
        self.assertEqual(rc, 0, log)
        self.assertIn("codegen: extra0.elf: withdrew 1 seeds as non-code, 4 words only they reach",
                      log)
        self.assertIn("void f_00002000(CpuState *s) {", generated)
        self.assertNotIn("void f_00002010(CpuState *s) {", generated)
        self.assertIn("sr_exec_span_register(0x00002000u, 0x00002010u)", generated)
        self.assertNotIn("sr_exec_span_register(0x00002000u, 0x00002020u)", generated)

    def test_a_body_in_withdrawn_words_stops_the_build(self):
        log = io.StringIO()
        with contextlib.redirect_stderr(log):
            self.assertFalse(codegen._non_code_disjoint("image", {0x1000, 0x1010}, {0x1010}))
            self.assertTrue(codegen._non_code_disjoint("image", {0x1000}, {0x1010}))
        self.assertIn("NON_CODE_TRANSLATED: 1 words", log.getvalue())


def fixture_generator(name):
    """fixtures/<name>/generate.py, the recipe of a public, source-owned guest."""
    path = pathlib.Path(__file__).resolve().parent.parent / "fixtures" / name / "generate.py"
    spec = importlib.util.spec_from_file_location(f"non_code_fixture_{name}", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def analysis(image, **elf_args):
    """(entries, spans, report) of the analysis with the withdrawal, then without it."""
    results = []
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "guest.elf")
        pathlib.Path(path).write_bytes(image)
        for patch in (contextlib.nullcontext(), stock_analysis()):
            report = {}
            with patch, contextlib.redirect_stderr(io.StringIO()), \
                    contextlib.redirect_stdout(io.StringIO()):
                entries, spans = analyze.analyze(analyze.Elf(path, **elf_args), report=report)
            results.append((entries, spans, report))
    return results


class PublicFixtureTest(unittest.TestCase):
    """The withdrawal on the code of the public, source-owned guests (fixtures/*/generate.py)."""

    GUESTS = ("cosim", "display_smoke", "production_smoke", "snapshot")
    LOAD = 0x08804000

    def text(self, generator):
        if "frames" in generator.build_text_segment.__code__.co_varnames:
            return generator.build_text_segment(generator.DEFAULT_FRAMES)
        return generator.build_text_segment()

    def test_a_module_with_relocations_is_untouched(self):
        # Each guest as its loader sees it, a PRX with its relocation map: analysed exactly
        # as without the withdrawal, and nothing withdrawn.
        for name in self.GUESTS:
            with self.subTest(name):
                (entries, spans, report), (before, before_spans, _) = analysis(
                    fixture_generator(name).build_prx(), base=self.LOAD)
                self.assertEqual((entries, spans), (before, before_spans))
                self.assertEqual(report["non_code"], {})

    def test_code_without_relocation_evidence_loses_nothing(self):
        # Each guest's code alone as a fixed-address executable (no relocation evidence): the
        # same analysis as without the withdrawal. With data after it (a reserved word, then
        # random words), the code keeps every entry but those whose generated body ran on into
        # the data before (padding after the last function), which code generation stubbed.
        rng = random.Random(1)
        data = struct.pack("<I", RESERVED) + b"".join(
            struct.pack("<I", rng.getrandbits(32)) for _ in range(2000))
        for name in self.GUESTS:
            with self.subTest(name):
                text = self.text(fixture_generator(name))
                (entries, spans, _), (before, before_spans, _) = analysis(
                    fixed_executable((), self.LOAD, text), base=0)
                self.assertEqual((entries, spans), (before, before_spans))
                image = fixed_executable((), self.LOAD, text + data)
                (entries, _, report), (before, _, _) = analysis(image, base=0)
                code = range(self.LOAD, self.LOAD + len(text))
                self.assertTrue(report["non_code"])
                with tempfile.TemporaryDirectory() as tmp:
                    path = os.path.join(tmp, "guest.elf")
                    pathlib.Path(path).write_bytes(image)
                    elf = analyze.Elf(path, base=0)
                for lost in sorted(a for a in before if a in code and a not in entries):
                    body = codegen.function_flow(elf, lost, analyze.exec_ranges(elf), set(before))[0]
                    self.assertTrue(any(analyze.reserved_encoding(codegen.read32(elf, a))
                                        for a in body), hex(lost))

    def test_a_module_with_an_empty_relocation_map_is_judged_as_an_executable(self):
        # A PRX whose loader finds no relocation has no address evidence either: placed at
        # the load address, it gets the same entries, spans and withdrawals as the same
        # bytes as a fixed-address executable there.
        rng = random.Random(1)
        data = struct.pack("<I", RESERVED) + b"".join(
            struct.pack("<I", rng.getrandbits(32)) for _ in range(2000))
        for name in self.GUESTS:
            with self.subTest(name):
                text = self.text(fixture_generator(name))
                prx = bytearray(fixed_executable((), 0, text + data))
                struct.pack_into("<H", prx, 16, 0xFFA0)  # ET_SCE_PRX, no relocation section
                (entries, spans, report), _ = analysis(bytes(prx), base=self.LOAD)
                (fixed, fixed_spans, fixed_report), _ = analysis(
                    fixed_executable((), self.LOAD, text + data), base=0)
                self.assertTrue(report["non_code"])
                self.assertEqual((entries, spans, report["non_code"]),
                                 (fixed, fixed_spans, fixed_report["non_code"]))


if __name__ == "__main__":
    unittest.main()
