# SPDX-License-Identifier: GPL-3.0-or-later

from pathlib import Path
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import codegen
import lowering_plan


class FakeElf:
    """Minimal read_at_vaddr shim: a dict of addr -> instruction word."""

    def __init__(self, words):
        self.words = words

    def read_at_vaddr(self, addr, size):
        w = self.words.get(addr)
        return w.to_bytes(4, "little") if w is not None else None


def _mk(words, labels: set[int] | frozenset[int] = frozenset()):
    return FakeElf(words), set(words), set(labels)


class StaticVerifyTests(unittest.TestCase):
    def setUp(self):
        self.options = codegen.CodegenOptions(static_verify=True)

    def test_lui_addiu_chain_is_predicted(self):
        # lui $a0, 0x2d ; addiu $a0, $a0, -0x518  -> a0 = 0x002cfae8
        words = {
            0x1000: (0x0F << 26) | (4 << 16) | 0x002D,
            0x1004: (0x09 << 26) | (4 << 21) | (4 << 16) | (0xFAE8),
        }
        elf, insns, labels = _mk(words)
        pts = codegen.sv_plan(elf, insns, labels, options=self.options)
        self.assertEqual(pts, {0x1004: [(4, 0x002CFAE8)]})

    def test_unknown_input_is_never_asserted(self):
        # lw $v0, 0($a0) ; addiu $v0, $v0, 8 : v0 stays Unknown, no check.
        # ori $a1, $zero, 5 in the same run IS known.
        words = {
            0x2000: (0x23 << 26) | (4 << 21) | (2 << 16),
            0x2004: (0x09 << 26) | (2 << 21) | (2 << 16) | 8,
            0x2008: (0x0D << 26) | (0 << 21) | (5 << 16) | 5,
        }
        elf, insns, labels = _mk(words)
        pts = codegen.sv_plan(elf, insns, labels, options=self.options)
        self.assertEqual(pts, {0x2008: [(5, 5)]})

    def test_state_resets_at_labels_and_branches(self):
        # ori $a0, $zero, 1 ; beq $zero,$zero,+2 ; nop(delay) ; ori $a1,$zero,2
        # The post-delay-slot instruction starts a fresh run of length 1 -> no
        # flush (run_len < 2), and nothing from before the branch leaks past it.
        words = {
            0x3000: (0x0D << 26) | (4 << 16) | 1,
            0x3004: (0x04 << 26) | 2,          # beq $zero, $zero, +2
            0x3008: 0,                          # delay slot nop
            0x300C: (0x0D << 26) | (5 << 16) | 2,
        }
        elf, insns, labels = _mk(words, labels={0x300C})
        pts = codegen.sv_plan(elf, insns, labels, options=self.options)
        self.assertNotIn(0x300C, pts)
        self.assertNotIn(0x3008, pts)
        # The same words with no label: only the branch and delay-slot resets can
        # keep the pre-branch ori from joining 0x300C in one two-instruction run.
        elf, insns, labels = _mk(words)
        pts = codegen.sv_plan(elf, insns, labels, options=self.options)
        self.assertEqual(pts, {})

    def test_sra_and_slt_are_signed(self):
        # lui $t0, 0x8000 ; sra $t1, $t0, 4 -> 0xF8000000 ; slt $t2, $t0, $zero -> 1
        words = {
            0x4000: (0x0F << 26) | (8 << 16) | 0x8000,
            0x4004: (8 << 16) | (9 << 11) | (4 << 6) | 0x03,
            0x4008: (8 << 21) | (0 << 16) | (10 << 11) | 0x2A,
        }
        elf, insns, labels = _mk(words)
        pts = codegen.sv_plan(elf, insns, labels, options=self.options)
        self.assertEqual(pts, {0x4008: [(8, 0x80000000), (9, 0xF8000000), (10, 1)]})

    def test_max_min_and_unmodeled_special_do_not_keep_stale_destinations(self):
        for funct, expected in ((0x2C, 5), (0x2D, 3), (0x3F, None)):
            with self.subTest(funct=funct):
                words = {
                    0x4800: (0x0F << 26) | (8 << 16) | 0x1234,  # lui t0, 0x1234
                    0x4804: (0x09 << 26) | (4 << 16) | 5,       # addiu a0, zero, 5
                    0x4808: (0x09 << 26) | (5 << 16) | 3,       # addiu a1, zero, 3
                    0x480C: (4 << 21) | (5 << 16) | (8 << 11) | funct,
                    0x4810: (0x0D << 26) | (9 << 16) | 7,      # ori t1, zero, 7
                }
                elf, insns, labels = _mk(words)
                checks = dict(codegen.sv_plan(elf, insns, labels, options=self.options)[0x4810])
                if expected is None:
                    self.assertNotIn(8, checks)
                else:
                    self.assertEqual(checks[8], expected)

    def test_syscall_resets_lattice(self):
        # ori $a0,$zero,1 ; syscall ; ori $a1,$zero,2 : only a1 asserted... and the
        # run after syscall has length 1, so nothing is asserted at all.
        words = {
            0x5000: (0x0D << 26) | (4 << 16) | 1,
            0x5004: 0x0000000C,
            0x5008: (0x0D << 26) | (5 << 16) | 2,
        }
        elf, insns, labels = _mk(words)
        pts = codegen.sv_plan(elf, insns, labels, options=self.options)
        for addr, checks in pts.items():
            for reg, _ in checks:
                self.assertNotEqual(reg, 4, f"r4 asserted after syscall at 0x{addr:x}")

    def test_labelled_delay_slot_value_is_not_asserted_past_the_slot(self):
        # 0x1000 lands on the delay slot of the beql at 0x1008. emit_function runs that slot
        # inline at the beql (annulled when it is not taken) and jumps over its labelled copy
        # to 0x1010 (_dup_slot_skips), so 0x1010 is reached without the copy having run: the
        # lui's a2 is no constant there. The two words after the slot still form a run.
        words = {
            0x1000: (0x04 << 26) | (4 << 21) | 2,    # beq a0, zero, 0x100C
            0x1004: 0,
            0x1008: (0x14 << 26) | (5 << 21) | 5,    # beql a1, zero, 0x1020
            0x100C: (0x0F << 26) | (6 << 16) | 0x1234,  # lui a2, 0x1234 (slot and label)
            0x1010: (0x0D << 26) | (7 << 16) | 5,    # ori a3, zero, 5
            0x1014: (0x0D << 26) | (8 << 16) | 6,    # ori t0, zero, 6
            0x1018: 0x03E00008,
            0x101C: 0,
            0x1020: 0x03E00008,
            0x1024: 0,
        }
        elf, insns, labels = _mk(words, labels={0x100C, 0x1020})
        pts = codegen.sv_plan(elf, insns, labels, options=self.options)
        self.assertEqual(pts, {0x1014: [(7, 5), (8, 6)]})
        text = "\n".join(codegen.emit_function(elf, 0x1000, [(0x1000, 0x1028)], {0x1000},
                                               options=self.options))
        self.assertIn("goto L_00001010; /* slot already ran inline (or was annulled)", text)
        self.assertNotIn("sr_sv_check(s, 0x00001014u, 6,", text)
        self.assertIn("sr_sv_check(s, 0x00001014u, 7, 0x00000005u);", text)

    # The witness of #3130: a backward branch to 0x1000 reaches the entry word at 0x1008 by
    # falling through the two words above it, while a call enters at 0x1008 directly.
    ENTRY_JOIN_WORDS = {
        0x1000: (0x0F << 26) | (6 << 16) | 0x1234,   # lui a2, 0x1234
        0x1004: (0x0D << 26) | (7 << 16) | 5,        # ori a3, zero, 5
        0x1008: (0x0D << 26) | (8 << 16) | 6,        # ori t0, zero, 6 (entry)
        0x100C: (0x0D << 26) | (9 << 16) | 7,        # ori t1, zero, 7
        0x1010: (0x05 << 26) | (4 << 21) | 0xFFFB,   # bne a0, zero, 0x1000
        0x1014: 0,
        0x1018: 0x03E00008,
        0x101C: 0,
    }

    def _entry_join_checks(self, entry, words=None):
        """The checks emit_function asserts and the LLVM plan carries, which must agree."""
        words = words or self.ENTRY_JOIN_WORDS
        elf, ranges, known = FakeElf(words), [(0x1000, 0x1020)], {entry}
        text = "\n".join(codegen.emit_function(elf, entry, ranges, known, options=self.options))
        in_c = sorted((int(pc, 16), int(reg), int(value, 16)) for pc, reg, value in re.findall(
            r"sr_sv_check\(s, 0x([0-9A-Fa-f]+)u, (\d+), 0x([0-9A-Fa-f]+)u\)", text))
        plan = lowering_plan.plan_function(codegen, elf, entry, ranges, known, options=self.options)
        in_plan = sorted((pc, reg, value) for pc, checks in plan.sv_checks.items()
                         for reg, value in checks)
        self.assertEqual(in_c, in_plan)
        return in_c

    def test_an_entry_above_the_lowest_address_is_a_join(self):
        # Entered at 0x1008 the first pass never ran 0x1000 or 0x1004, so a2 and a3 are no
        # constants at 0x100C. The run of 0x1000..0x1004 is still asserted where the branch
        # reaches it, and the entry word starts a run of its own.
        self.assertEqual(self._entry_join_checks(0x1008), [
            (0x1004, 6, 0x12340000), (0x1004, 7, 5), (0x100C, 8, 6), (0x100C, 9, 7)])

    def test_an_entry_at_the_lowest_address_keeps_its_checks(self):
        self.assertEqual(self._entry_join_checks(0x1000), [
            (0x100C, 6, 0x12340000), (0x100C, 7, 5), (0x100C, 8, 6)])

    def test_a_constant_after_the_entry_join_is_still_asserted(self):
        # The entry join drops what ran before it, not what the entry run sets.
        checks = self._entry_join_checks(0x1008)
        self.assertIn((0x100C, 8, 6), checks)
        self.assertIn((0x100C, 9, 7), checks)

    # The witness of #3163 for the checks: the entry at 0x1004 is the delay slot of the
    # transfer at 0x1000, which the backward branch reaches. emit_function runs the slot
    # inline there (annulled by a not-taken beql, followed by the call of a jal) and jumps
    # over the entry's copy to 0x1008, so 0x1008 is a join: the lui's a2 is no constant there.
    @staticmethod
    def _entry_slot_words(owner):
        return {
            0x1000: owner,
            0x1004: (0x0F << 26) | (6 << 16) | 0x1234,          # lui a2, 0x1234 (entry, slot)
            0x1008: (0x0D << 26) | (7 << 16) | 5,               # ori a3, zero, 5
            0x100C: (0x0D << 26) | (8 << 16) | 6,               # ori t0, zero, 6
            0x1010: (0x05 << 26) | (5 << 21) | 0xFFFB,          # bne a1, zero, 0x1000
            0x1014: (0x09 << 26) | (6 << 21) | (6 << 16) | 1,   # addiu a2, a2, 1 (slot)
            0x1018: 0x03E00008,
            0x101C: 0,
        }

    def test_the_word_after_an_entry_on_a_delay_slot_is_a_join(self):
        beql = (0x14 << 26) | (4 << 21) | 5                     # beql a0, zero, 0x1018
        jal = (0x03 << 26) | (0x2000 >> 2)                      # jal 0x2000
        for name, owner in (("beql", beql), ("jal", jal)):
            with self.subTest(owner=name):
                checks = self._entry_join_checks(0x1004, self._entry_slot_words(owner))
                self.assertEqual(checks, [(0x100C, 7, 5), (0x100C, 8, 6)])

    def test_disabled_by_default(self):
        words = {0x6000: (0x0F << 26) | (4 << 16) | 1,
                 0x6004: (0x0D << 26) | (4 << 21) | (4 << 16) | 2}
        elf, insns, labels = _mk(words)
        self.assertEqual(codegen.sv_plan(
            elf, insns, labels, options=codegen.CodegenOptions()), {})


if __name__ == "__main__":
    unittest.main()
