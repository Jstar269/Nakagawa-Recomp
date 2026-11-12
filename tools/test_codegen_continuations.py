# SPDX-License-Identifier: GPL-3.0-or-later

from contextlib import contextmanager
from pathlib import Path
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import codegen

#: The runtime header whose definitions the --state-resume allow-list (codegen._STATE_RESUME_CALLS)
#: rests on. A module-level name, so a check can point the test at a copy of the header.
RECOMP_H = ROOT / "src" / "rt" / "recomp.h"


def _header_definitions(text):
    """Every ``#define`` of a function-like macro (name -> bodies) and every name a C line
    declares or defines as a function, from header text with comments and line splices removed."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    text = text.replace("\\\n", " ")
    macros, functions = {}, set()
    for line in text.splitlines():
        define = re.match(r"\s*#\s*define\s+([A-Za-z_]\w*)\(([^)]*)\)\s*(.*)$", line)
        if define:
            macros.setdefault(define.group(1), []).append(define.group(3).strip())
            continue
        function = re.match(r"\s*(?!(?:return|else|case|goto|sizeof)\b)(?:(?:static|inline|extern)\s+)*"
                            r"[A-Za-z_]\w*(?:\s+|\s*\*+\s*)+"
                            r"([A-Za-z_]\w*)\s*\(", line)
        if function and not line.lstrip().startswith("#"):
            functions.add(function.group(1))
    return macros, functions


def _closes_at_end(body, opening):
    """Whether the bracket at ``opening`` is closed by the body's last character."""
    pair = {"(": ")", "{": "}"}[body[opening]]
    depth = 0
    for index in range(opening, len(body)):
        if body[index] == body[opening]:
            depth += 1
        elif body[index] == pair:
            depth -= 1
            if depth == 0:
                return index == len(body) - 1
    return False


def _declares_nothing(name, macros, functions, seen=()):
    """Whether a statement ``name(...);`` can declare nothing at its own scope: ``name`` is a
    function, or every definition of it is a parenthesized expression, a body that is exactly
    ``do { ... } while (0)``, or a call ``other(...)`` of a name that itself declares nothing."""
    if name in seen or name not in macros:
        return name in functions
    for body in macros[name]:
        if body.startswith("(") and _closes_at_end(body, 0):
            continue
        loop = re.fullmatch(r"do\s*(\{.*\})\s*while\s*\(\s*0\s*\)", body, re.S)
        if loop and _closes_at_end(loop.group(1), 0):
            continue
        call = re.match(r"([A-Za-z_]\w*)\s*\(", body)
        if (call and _closes_at_end(body, call.end() - 1)
                and _declares_nothing(call.group(1), macros, functions, (*seen, name))):
            continue
        return False
    return True


class FakeElf:
    def __init__(self, words):
        self.words = words

    def read_at_vaddr(self, addr, size):
        if size != 4 or addr not in self.words:
            return None
        return self.words[addr].to_bytes(4, "little")


class IterationTrackingKnownEntries:
    """Membership-only view that detects a full entry-set copy per function."""

    def __init__(self, entries):
        self.entries = set(entries)
        self.iterations = 0

    def __contains__(self, entry):
        return entry in self.entries

    def __iter__(self):
        self.iterations += 1
        return iter(self.entries)


class ScanCountingSpans(list):
    """A span list that counts every full scan of itself.

    ``in_ranges`` over a plain list scans it once per membership test; an index
    built from it scans it once.
    """

    def __init__(self, spans):
        super().__init__(spans)
        self.scans = 0

    def __iter__(self):
        self.scans += 1
        return super().__iter__()


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def j(target):
    return 0x08000000 | ((target >> 2) & 0x03FFFFFF)


def jalr(rd, rs):
    return (rs << 21) | (rd << 11) | 0x09


def beq(rs, rt, pc, target):
    offset = ((target - (pc + 4)) >> 2) & 0xFFFF
    return 0x10000000 | (rs << 21) | (rt << 16) | offset


def bne(rs, rt, pc, target):
    return beq(rs, rt, pc, target) | 0x04000000


def beql(rs, rt, pc, target):
    return (beq(rs, rt, pc, target) & 0x03FFFFFF) | 0x50000000


@contextmanager
def region_split_threshold(value):
    """Yield an isolated option object with the selected region-split threshold."""
    with codegen.options_scope(region_split_threshold=value) as options:
        yield options


def regimm(selector, rs, pc, target):
    """REGIMM branch: 0x10 bltzal, 0x11 bgezal (bal is bgezal $zero), 0x12/0x13 likely."""
    offset = ((target - (pc + 4)) >> 2) & 0xFFFF
    return 0x04000000 | (rs << 21) | (selector << 16) | offset


class ContinuationFlowTests(unittest.TestCase):
    def test_emission_does_not_copy_the_complete_known_entry_set(self):
        known = IterationTrackingKnownEntries(
            {0x1000, *range(0x4000, 0x8000, 4)}
        )
        text = codegen.emit_function(
            FakeElf({0x1000: 0x03E00008, 0x1004: 0}),
            0x1000, [(0x1000, 0x1008)], known,
        )

        self.assertTrue(any("void f_00001000" in line for line in text))
        self.assertEqual(known.iterations, 0)

    def test_flow_and_emission_scan_the_span_list_once_per_body(self):
        # An image whose owned code is split into hundreds of spans (each run of code
        # reached outside the named sections is its own span) made every membership
        # test of the walk scan them all: one profiled executable with 284 spans spent
        # 61% of its code-generation time in that scan. The walk indexes them once.
        body = 64
        words = {0x1000 + 4 * i: 0x24420001 for i in range(body)}  # addiu v0, v0, 1
        words[0x1000 + 4 * body] = 0x03E00008                        # jr ra
        words[0x1004 + 4 * body] = 0x00000000
        spans = [(0x1000, 0x1008 + 4 * body)]
        spans += [(0x100000 + 0x100 * i, 0x100080 + 0x100 * i) for i in range(300)]
        elf, known = FakeElf(words), {0x1000}

        ranges = ScanCountingSpans(spans)
        insns, _labels, _continuations = codegen.function_flow(elf, 0x1000, ranges, known)
        self.assertEqual(len(insns), body + 2)
        self.assertEqual(ranges.scans, 1)

        ranges = ScanCountingSpans(spans)
        indexed = "\n".join(codegen.emit_function(elf, 0x1000, list(spans), known))
        text = "\n".join(codegen.emit_function(elf, 0x1000, ranges, known))
        self.assertEqual(text, indexed)
        self.assertEqual(ranges.scans, 1)

    def test_linked_transfers_carry_the_native_resume_boundary(self):
        linked_jal = "\n".join(
            codegen.emit_function(
                FakeElf({
                    0x1000: jal(0x2000),
                    0x1004: 0x24030011,
                    0x1008: 0x03E00008,
                    0x100C: 0x24000000,
                }),
                0x1000, [(0x1000, 0x1010)], {0x1000},
            )
        )
        self.assertIn("if (dispatch_call_checked(s, 0x00002000u, 0x00001008u)) return;", linked_jal)

        linked_jalr = "\n".join(
            codegen.emit_function(
                FakeElf({
                    0x1100: jalr(31, 8),
                    0x1104: 0x24030011,
                    0x1108: 0x03E00008,
                    0x110C: 0x24000000,
                }),
                0x1100, [(0x1100, 0x1110)], {0x1100},
            )
        )
        self.assertIn("if (dispatch_call_checked(s, _t, 0x00001108u)) return;", linked_jalr)

    # #968: bltzal/bgezal (bal is bgezal $zero) write $ra and CALL their target. Out of
    # the body the taken path used to be a TAIL (`s->pc = target; dispatch; return`), so
    # the callee's `jr $ra` left this body and the continuation at addr + 8 never ran.
    @staticmethod
    def _linked_site(first, known, lle_cpu=False):
        """`first` at 0x1000, a counted delay slot, and the continuation at 0x1008."""
        return codegen.emit_function(
            FakeElf({
                0x1000: first,
                0x1004: 0x24030011,  # addiu v1, zero, 0x11 (delay slot)
                0x1008: 0x03E00008,  # continuation: jr ra
                0x100C: 0x00000000,
            }),
            0x1000, [(0x1000, 0x1010)], known, lle_cpu=lle_cpu,
        )

    @staticmethod
    def _between_slot_and_continuation(lines):
        slot = next(i for i, line in enumerate(lines) if "sr_begin(s, 0x00001004u" in line)
        resume = next(i for i, line in enumerate(lines) if "sr_begin(s, 0x00001008u" in line)
        if resume > slot + 1 and "SR_AOT_RETURN_CAPTURE" in lines[resume - 1]:
            # A `jr $ra` continuation latches its target immediately before its
            # own trace/slot lines; that return block is after the caller's
            # continuation boundary, not part of the linked-call statement.
            resume -= 2
        return " ".join(line.strip() for line in lines[slot + 1:resume])

    def test_linked_branch_out_of_the_body_is_the_same_call_as_jal(self):
        # The taken path must be exactly what `jal` emits for the same target: a direct
        # body call that completes the callee's tail chain (#855) for a known entry,
        # dispatch_call with the link as resume otherwise. Never a recorded TAIL.
        for known in ({0x1000}, {0x1000, 0x2000}):
            for lle_cpu in (False, True):
                jal_call = self._between_slot_and_continuation(
                    self._linked_site(jal(0x2000), known, lle_cpu))
                for name, word in (("bal", regimm(0x11, 0, 0x1000, 0x2000)),
                                   ("bltzal", regimm(0x10, 4, 0x1000, 0x2000))):
                    with self.subTest(known=sorted(known), lle_cpu=lle_cpu, branch=name):
                        lines = self._linked_site(word, known, lle_cpu)
                        text = "\n".join(lines)
                        self.assertEqual(
                            self._between_slot_and_continuation(lines),
                            f"if (_c) {{ {jal_call} }} }}",
                        )
                        self.assertNotIn("s->pc = 0x00002000u", text)
                        self.assertNotIn("sr_tail_record", text)
        unknown = "\n".join(self._linked_site(regimm(0x11, 0, 0x1000, 0x2000), {0x1000}))
        self.assertIn("if (_c) { if (dispatch_call_checked(s, 0x00002000u, 0x00001008u)) return; } }", unknown)
        known = "\n".join(self._linked_site(regimm(0x11, 0, 0x1000, 0x2000), {0x1000, 0x2000}))
        self.assertIn(
            "if (_c) { if (sr_aot_return_boundary_begin(s, 0x00001008u, "
            "0x00001000u, 1u)) return; f_00002000(s); SR_TAIL_DRAIN(s); "
            "if (sr_aot_return_boundary_end(s)) return; } }",
            known,
        )

    def test_linked_branch_out_of_the_body_links_and_runs_its_slot_once(self):
        # MIPS writes the link on both paths, before the slot; the slot runs once on
        # both paths; the not-taken path is a plain fall-through into the continuation.
        text = "\n".join(self._linked_site(regimm(0x10, 4, 0x1000, 0x2000), {0x1000}))
        call = "if (_c) { if (dispatch_call_checked(s, 0x00002000u, 0x00001008u)) return; } }"
        self.assertIn(call, text)
        link = text.index("s->r[31] = 0x00001008u;")
        slot = text.index("s->r[3] = ")
        taken = text.index(call)
        resume = text.index("sr_begin(s, 0x00001008u")
        self.assertLess(link, slot)
        self.assertLess(slot, taken)
        self.assertLess(taken, resume)
        self.assertEqual(text.count("sr_begin(s, 0x00001004u"), 1)
        self.assertNotIn("_sr_cont_", text)

    def test_linked_branch_to_its_own_continuation_stays_a_goto(self):
        # `bal .+8` reads the PC; its target is this body's own next pair, so the
        # in-body goto (no call) is exact and unchanged by #968.
        text = "\n".join(self._linked_site(regimm(0x11, 0, 0x1000, 0x1008), {0x1000}))
        self.assertIn("if (_c) { goto L_00001008; } }", text)
        self.assertNotIn("dispatch_call", text)

    def test_branch_likely_linked_forms_stay_refused(self):
        # bltzall/bgezall have no cond_expr case; #968 adds no opcode support.
        for selector in (0x12, 0x13):
            with self.subTest(selector=hex(selector)):
                with self.assertRaises(codegen.Unsupported):
                    self._linked_site(regimm(selector, 4, 0x1000, 0x2000), {0x1000})
                with self.assertRaises(codegen.Unsupported):
                    self._in_body_site(regimm(selector, 4, 0x1000, 0x1010), {0x1000, 0x1010})

    # #968 in the body: the target lies inside the caller's own span. function_flow used
    # to make it a label and the taken path a `goto`, so the callee's `jr $ra` was the
    # caller's host return and the continuation at the link never ran.
    _LEAF_WORDS = {
        0x1010: 0x24420001,  # leaf: addiu v0, v0, 1
        0x1014: 0x03E00008,  # jr ra
        0x1018: 0x00000000,
    }

    def _in_body_site(self, first, known, extra=None):
        """`first` at 0x1000, its slot, the continuation `jr ra` at 0x1008, a leaf at 0x1010."""
        words = {
            0x1000: first,
            0x1004: 0x24030011,  # addiu v1, zero, 0x11 (delay slot)
            0x1008: 0x03E00008,  # continuation: jr ra
            0x100C: 0x00000000,
            **self._LEAF_WORDS,
        }
        words.update(extra or {})
        return codegen.emit_function(FakeElf(words), 0x1000, [(0x1000, 0x101C)], known)

    def test_linked_branch_inside_the_body_is_the_same_call_as_jal(self):
        for known in ({0x1000}, {0x1000, 0x1010}):
            jal_call = self._between_slot_and_continuation(self._in_body_site(jal(0x1010), known))
            for name, word in (("bal", regimm(0x11, 0, 0x1000, 0x1010)),
                               ("bltzal", regimm(0x10, 4, 0x1000, 0x1010))):
                with self.subTest(known=sorted(known), branch=name):
                    lines = self._in_body_site(word, known)
                    text = "\n".join(lines)
                    self.assertEqual(
                        self._between_slot_and_continuation(lines),
                        f"if (_c) {{ {jal_call} }} }}",
                    )
                    self.assertNotIn("L_00001010", text, "the callee is not a label")
                    self.assertNotIn("0x00001010u, 0x24420001u", text, "nor part of this body")
        elf = FakeElf({0x1000: regimm(0x11, 0, 0x1000, 0x1010), 0x1004: 0,
                       0x1008: 0x03E00008, 0x100C: 0, **self._LEAF_WORDS})
        insns, labels, _ = codegen.function_flow(elf, 0x1000, [(0x1000, 0x101C)], {0x1000})
        self.assertEqual(sorted(insns), [0x1000, 0x1004, 0x1008, 0x100C])
        self.assertEqual(labels, set())

    def test_linked_call_keeps_its_call_when_another_branch_labels_the_target(self):
        # `bne` makes 0x1018 a native label of this body; the `bal` to the same address
        # must still call it rather than jump to that label.
        lines = codegen.emit_function(
            FakeElf({
                0x1000: (0x05 << 26) | (4 << 21) | ((0x1018 - 0x1004) >> 2),  # bne a0, zero
                0x1004: 0x00000000,
                0x1008: regimm(0x11, 0, 0x1008, 0x1018),                      # bal 0x1018
                0x100C: 0x24030011,
                0x1010: 0x03E00008,
                0x1014: 0x00000000,
                0x1018: 0x24420001,
                0x101C: 0x03E00008,
                0x1020: 0x00000000,
            }),
            0x1000, [(0x1000, 0x1024)], {0x1000, 0x1018},
        )
        text = "\n".join(lines)
        self.assertIn("if (_c) { goto L_00001018; } }", text)
        self.assertIn(
            "if (_c) { if (sr_aot_return_boundary_begin(s, 0x00001010u, "
            "0x00001008u, 1u)) return; f_00001018(s); SR_TAIL_DRAIN(s); "
            "if (sr_aot_return_boundary_end(s)) return; } }",
            text,
        )
        self.assertEqual(text.count("goto L_00001018;"), 1)

    def test_linked_call_whose_link_is_an_entry_continues_there(self):
        # The link is a known entry: after the call (and on the not-taken path) the body
        # ends at the ordinary continuation boundary, exactly as after a `jal`.
        for word in (jal(0x1010), regimm(0x11, 0, 0x1000, 0x1010)):
            with self.subTest(word=hex(word)):
                text = "\n".join(self._in_body_site(word, {0x1000, 0x1008, 0x1010}))
                call = text.index("f_00001010(s); SR_TAIL_DRAIN(s);")
                boundary = text.index("    goto _sr_cont_00001008;")
                self.assertLess(call, boundary)
                self.assertIn("_sr_cont_00001008: ;", text)
                self.assertNotIn("sr_begin(s, 0x00001008u", text)

    def test_linked_branch_into_a_frame_releasing_tail_stays_a_goto(self):
        # The target releases the frame it never allocated (a continuation), so its
        # `jr $ra` returns for this body: a jump that writes $ra, unchanged by #968.
        text = "\n".join(codegen.emit_function(
            FakeElf({
                0x1000: 0x27BDFFF0,                      # addiu sp, sp, -16
                0x1004: 0xAFBF0000,                      # sw ra, 0(sp)
                0x1008: regimm(0x11, 0, 0x1008, 0x1014),  # bal 0x1014
                0x100C: 0x24030011,
                0x1010: 0x24424000,                      # inline data, never executed
                0x1014: 0x03E02021,                      # addu a0, ra, zero
                0x1018: 0x8FBF0000,                      # lw ra, 0(sp)
                0x101C: 0x03E00008,                      # jr ra
                0x1020: 0x27BD0010,                      # addiu sp, sp, 16
            }),
            0x1000, [(0x1000, 0x1024)], {0x1000},
        ))
        self.assertIn("if (_c) { goto L_00001014; } }", text)
        self.assertNotIn("dispatch_call", text)
        self.assertNotIn("f_00001014(s)", text)

    def test_unlinked_computed_transfer_remains_a_tail_dispatch(self):
        text = "\n".join(
            codegen.emit_function(
                FakeElf({
                    0x1200: jalr(0, 8),
                    0x1204: 0x24030011,
                    0x1208: 0x03E00008,
                    0x120C: 0x24000000,
                }),
                0x1200, [(0x1200, 0x1210)], {0x1200},
            )
        )
        self.assertIn("sr_tail_record(s, _t, NULL,", text)
        self.assertNotIn("if (dispatch_call_checked(s, _t, 0x00001208u)) return;", text)

    def test_omitted_direct_jump_remains_a_tail_dispatch_boundary(self):
        elf = FakeElf(
            {
                0x1000: j(0x1040),
                0x1004: 0x24030011,  # delay slot belongs to the tail transfer
                0x1040: 0x24020022,  # intentionally omitted AOT target
                0x1044: 0x03E00008,
                0x1048: 0x00000000,
            }
        )
        known = {0x1000}
        omitted = {0x1040}
        insns, labels, continuations = codegen.function_flow(
            elf, 0x1000, [(0x1000, 0x104C)], known,
            dispatch_boundaries=omitted,
        )
        self.assertEqual(insns, {0x1000, 0x1004})
        self.assertNotIn(0x1040, labels)
        self.assertEqual(continuations, {})

        text = "\n".join(
            codegen.emit_function(
                elf, 0x1000, [(0x1000, 0x104C)], known,
                dispatch_boundaries=omitted,
            )
        )
        self.assertIn("sr_tail_record(s, _t, NULL,", text)
        self.assertNotIn("goto L_00001040", text)
        self.assertNotIn("f_00001040(s);", text)

    def test_call_fallthrough_uses_foreign_entry_before_frame_restore(self):
        elf = FakeElf(
            {
                0x1000: 0x27BDFFF0,  # addiu sp, sp, -16
                0x1004: 0xAFBF0000,  # sw ra, 0(sp)
                0x1008: jal(0x2000),
                0x100C: 0x24040001,  # addiu a0, zero, 1 (delay slot)
                0x1010: 0x00041823,  # foreign adjacent entry
                0x1014: 0x03E00008,
                0x1018: 0x00000000,
                0x2000: 0x03E00008,
                0x2004: 0x00000000,
            }
        )
        known = {0x1000, 0x1010, 0x2000}
        insns, _, continuations = codegen.function_flow(elf, 0x1000, [(0x1000, 0x2008)], known)
        self.assertNotIn(0x1010, insns)
        self.assertEqual(continuations, {0x1008: 0x1010})

        text = "\n".join(codegen.emit_function(elf, 0x1000, [(0x1000, 0x2008)], known))
        # The foreign entry runs as a TAIL transfer (#855): the boundary records it with
        # the owning entry's SP restore and returns; the invoker's drain runs the foreign
        # body and only then applies the restore, the order the nested call had.
        self.assertIn(
            "sr_tail_record(s, 0x00001010u, f_00001010, _sp_entry, 1u); return; "
            "/* synthetic boundary",
            text,
        )
        self.assertNotIn("f_00001010(s);", text)
        self.assertNotIn("s->r[29] = _sp_entry; /* synthetic boundary", text)
        self.assertIn("goto _sr_cont_00001010;", text)

    def test_stack_census_keeps_synthetic_boundary_annotation(self):
        elf = FakeElf(
            {
                0x1000: 0x27BDFFF0,  # addiu sp, sp, -16
                0x1004: 0xAFBF0000,  # sw ra, 0(sp)
                0x1008: jal(0x2000),
                0x100C: 0x24040001,  # addiu a0, zero, 1 (delay slot)
                0x1010: 0x00041823,  # foreign adjacent entry
                0x1014: 0x03E00008,
                0x1018: 0x00000000,
                0x2000: 0x03E00008,
                0x2004: 0x00000000,
            }
        )
        known = {0x1000, 0x1010, 0x2000}
        text = "\n".join(
            codegen.emit_function(
                elf, 0x1000, [(0x1000, 0x2008)], known, stack_census=True,
            )
        )
        # The census wrapper owns SP restoration (after its own drain), so the boundary
        # records no restore, but the generated source must still mark it as synthetic.
        self.assertIn(
            "sr_tail_record(s, 0x00001010u, f_00001010, 0u, 0u); return; "
            "/* synthetic boundary",
            text,
        )
        self.assertNotIn("s->r[29] = _sp_entry; /* synthetic boundary", text)

    def test_dense_linear_entries_chain_instead_of_truncating(self):
        elf = FakeElf(
            {
                0x1000: 0x24020001,
                0x1004: 0x24420001,
                0x1008: 0x03E00008,
                0x100C: 0x00000000,
            }
        )
        known = {0x1000, 0x1004, 0x1008}
        _, _, first = codegen.function_flow(elf, 0x1000, [(0x1000, 0x1010)], known)
        _, _, second = codegen.function_flow(elf, 0x1004, [(0x1000, 0x1010)], known)
        self.assertEqual(first, {0x1000: 0x1004})
        self.assertEqual(second, {0x1004: 0x1008})

        first_text = "\n".join(codegen.emit_function(elf, 0x1000, [(0x1000, 0x1010)], known))
        second_text = "\n".join(codegen.emit_function(elf, 0x1004, [(0x1000, 0x1010)], known))
        self.assertIn("sr_tail_record(s, 0x00001004u, f_00001004,", first_text)
        self.assertIn("sr_tail_record(s, 0x00001008u, f_00001008,", second_text)

    def test_every_direct_body_call_completes_its_tail_chain(self):
        # #855: a callee may leave through a recorded TAIL transfer, so every direct
        # CALL of a native body must drain it before the caller goes on.
        elf = FakeElf(
            {
                0x1000: jal(0x2000),
                0x1004: 0x00000000,
                0x1008: jal(0x3000),
                0x100C: 0x00000000,
                0x1010: 0x03E00008,
                0x1014: 0x00000000,
                0x2000: 0x08000C00,  # j 0x3000 (a direct tail)
                0x2004: 0x00000000,
                0x3000: 0x03E00008,
                0x3004: 0x00000000,
            }
        )
        known = {0x1000, 0x2000, 0x3000}
        ranges = [(0x1000, 0x1018), (0x2000, 0x2008), (0x3000, 0x3008)]
        text = "\n".join(
            "\n".join(codegen.emit_function(elf, entry, ranges, known))
            for entry in sorted(known)
        )
        calls = re.findall(r"\b(f_[0-9a-f]{8}(?:_real)?)\(s\);( SR_TAIL_DRAIN\(s\);)?", text)
        self.assertEqual(len(calls), 2, text)
        for symbol, drain in calls:
            self.assertTrue(drain, f"{symbol} is called without draining its tail chain")
        self.assertIn("sr_tail_record(s, 0x00003000u, f_00003000, _sp_entry, 1u); return;", text)

    def test_conditional_successors_stay_native_below_the_split_threshold(self):
        # Changed deliberately with entry splitting: a body
        # copies another entry's code over conditional edges only while its walk stays
        # within REGION_SPLIT_THRESHOLD, and never with the threshold off.
        # EntrySplitTests covers the same shape split.
        elf = FakeElf(
            {
                0x1000: beq(1, 2, 0x1000, 0x1020),
                0x1004: 0x00000000,
                0x1008: 0x24020007,  # foreign fall-through entry
                0x100C: 0x03E00008,
                0x1010: 0x00000000,
                0x1020: 0x24020009,
                0x1024: 0x03E00008,
                0x1028: 0x00000000,
            }
        )
        known = {0x1000, 0x1008, 0x1020}
        for threshold in (None, codegen.CodegenOptions().region_split_threshold):
            with self.subTest(threshold=threshold), region_split_threshold(threshold) as options:
                insns, labels, continuations = codegen.function_flow(
                    elf, 0x1000, [(0x1000, 0x102C)], known, options=options)
                self.assertIn(0x1020, labels)
                self.assertIn(0x1008, insns)
                self.assertEqual(continuations, {})

        text = "\n".join(codegen.emit_function(elf, 0x1000, [(0x1000, 0x102C)], known))
        taken = text.index("if (_c) { goto L_00001020; }")
        not_taken = text.index("0x00001008u", taken)
        target = text.index("L_00001020:", not_taken)
        self.assertLess(taken, not_taken)
        self.assertLess(not_taken, target)
        self.assertNotIn("_sr_cont_00001008", text)

    def test_conditional_foreign_target_dispatches_without_undefined_label(self):
        elf = FakeElf(
            {
                0x1000: beq(1, 2, 0x1000, 0x2000),
                0x1004: 0x00000000,
                0x1008: 0x03E00008,
                0x100C: 0x00000000,
            }
        )
        known = {0x1000, 0x2000}
        insns, labels, _continuations = codegen.function_flow(
            elf, 0x1000, [(0x1000, 0x1010)], known
        )
        self.assertNotIn(0x2000, insns)
        self.assertNotIn(0x2000, labels)

        text = "\n".join(
            codegen.emit_function(elf, 0x1000, [(0x1000, 0x1010)], known)
        )
        self.assertNotIn("goto L_00002000", text)
        self.assertIn("s->pc = 0x00002000u; sr_tail_record(s, s->pc, NULL,", text)


class EntrySplitTests(unittest.TestCase):
    """A split body leaves every other entry's code to that entry's own body.

    Entries 0x1000, 0x1010 and 0x1020 are adjacent. 0x1000 branches into 0x1020 and,
    when the branch is not taken, falls through into 0x1010; 0x1010 branches back to
    its own start. RANGES covers all three.
    """

    RANGES = [(0x1000, 0x1030)]
    KNOWN = {0x1000, 0x1010, 0x1020}

    def elf(self, branch=bne):
        return FakeElf({
            0x1000: 0x24020001,                       # addiu v0, zero, 1
            0x1004: 0x24030002,                       # addiu v1, zero, 2
            0x1008: branch(1, 2, 0x1008, 0x1020),     # into entry 0x1020
            0x100C: 0x24420004,                       # addiu v0, v0, 4 (slot)
            0x1010: 0x24420008,                       # entry: addiu v0, v0, 8
            0x1014: beq(2, 0, 0x1014, 0x1010),        # back to its own start
            0x1018: 0x00000000,
            0x101C: 0x00000000,
            0x1020: 0x24420010,                       # entry: addiu v0, v0, 16
            0x1024: 0x03E00008,                       # jr ra
            0x1028: 0x00000000,
        })

    def flow(self, elf, start, threshold=0, **kwargs):
        with region_split_threshold(threshold) as options:
            return codegen.function_flow(elf, start, self.RANGES, self.KNOWN,
                                        options=options, **kwargs)

    def emit(self, elf, start, threshold=0, **kwargs):
        with region_split_threshold(threshold) as options:
            return "\n".join(codegen.emit_function(
                elf, start, self.RANGES, self.KNOWN, options=options, **kwargs))

    def test_split_body_owns_no_other_entry(self):
        insns, labels, continuations = self.flow(self.elf(), 0x1000)
        self.assertEqual(insns, {0x1000, 0x1004, 0x1008, 0x100C})
        self.assertEqual(labels, set())
        self.assertEqual(continuations, {0x1008: 0x1010})

    def test_unsplit_body_copies_both_entries(self):
        insns, labels, continuations = self.flow(self.elf(), 0x1000, threshold=None)
        self.assertTrue({0x1010, 0x1014, 0x1020, 0x1024} <= insns)
        self.assertIn(0x1020, labels)
        # Straight-line fall-through into an entry was a continuation before splitting.
        self.assertEqual(continuations, {0x101C: 0x1020})

    def test_taken_edge_is_a_tail_transfer_after_the_delay_slot(self):
        text = self.emit(self.elf(), 0x1000)
        slot = text.index("s->r[2] = (s->r[2] + 0x00000004u)")
        tail = text.index("if (_c) { sr_tail_record(s, 0x00001020u, f_00001020, _sp_entry, 1u); return; }")
        self.assertLess(slot, tail)
        self.assertNotIn("L_00001020", text)

    def test_not_taken_edge_continues_into_the_next_entry(self):
        text = self.emit(self.elf(), 0x1000)
        branch = text.index("0x00001008u, 0x")
        cont = text.index("goto _sr_cont_00001010;", branch)
        self.assertLess(branch, cont)
        target = text.index("  _sr_cont_00001010: ;")
        self.assertIn("sr_tail_record(s, 0x00001010u, f_00001010, _sp_entry, 1u); return;",
                      text[target:])

    def test_likely_edge_runs_its_slot_only_when_taken(self):
        text = self.emit(self.elf(beql), 0x1000)
        self.assertIn("if (_c) { sr_begin(s, 0x0000100cu,", text)
        self.assertIn("sr_tail_record(s, 0x00001020u, f_00001020, _sp_entry, 1u); return; }", text)
        self.assertEqual(text.count("0x0000100cu"), 1, text)
        self.assertIn("goto _sr_cont_00001010;", text)

    def test_branch_to_its_own_start_stays_a_loop_edge(self):
        insns, labels, continuations = self.flow(self.elf(), 0x1010)
        self.assertIn(0x1010, labels)
        self.assertEqual(continuations, {0x101C: 0x1020})
        text = self.emit(self.elf(), 0x1010)
        self.assertIn("goto L_00001010;", text)

    def test_resume_body_is_entered_through_its_own_symbol(self):
        # 0x1020 as a resume label owned by 0x1010: another body's crossing enters it
        # through r_00001020; its owner still reaches it natively.
        owners = {0x1020: 0x1010}
        known = self.KNOWN - {0x1020}
        with region_split_threshold(0) as options:
            text = "\n".join(codegen.emit_function(
                self.elf(), 0x1000, self.RANGES, known, resume_owners=owners,
                options=options))
            owner_flow = codegen.function_flow(
                self.elf(), 0x1010, self.RANGES, known, resume_owners=owners,
                options=options)
        self.assertIn("sr_tail_record(s, 0x00001020u, r_00001020, _sp_entry, 1u); return;", text)
        self.assertIn(0x1020, owner_flow[0])

    def test_dispatch_boundary_target_is_dispatched_by_address(self):
        with region_split_threshold(0) as options:
            text = "\n".join(codegen.emit_function(
                self.elf(), 0x1000, self.RANGES, self.KNOWN - {0x1020},
                dispatch_boundaries={0x1020}, options=options))
        self.assertIn("s->pc = 0x00001020u; sr_tail_record(s, s->pc, NULL,", text)

    def test_threshold_splits_only_bodies_whose_walk_exceeds_it(self):
        elf = self.elf()
        unsplit = self.emit(elf, 0x1000, threshold=None)
        walked = len(self.flow(elf, 0x1000, threshold=None)[0])
        self.assertEqual(self.emit(elf, 0x1000, threshold=walked), unsplit)
        self.assertEqual(self.emit(elf, 0x1000, threshold=4), self.emit(elf, 0x1000))
        self.assertNotEqual(self.emit(elf, 0x1000), unsplit)

    def test_every_ownership_reader_sees_the_split_body(self):
        with region_split_threshold(0) as options:
            insns, _labels, _continuations = codegen.function_flow(
                self.elf(), 0x1000, self.RANGES, self.KNOWN, options=options)
            record = codegen.stale_block_for_function(
                self.elf(), 0x1000, self.RANGES, self.KNOWN, options=options)
        self.assertEqual(record[1], len(insns))

    def test_threshold_option_is_parsed_and_checked(self):
        options = codegen.CodegenOptions()
        cli = codegen._command_line_parser().parse_args(["--region-split-threshold=19"])
        codegen._apply_options(cli, codegen._validated_options(cli), options)
        self.assertEqual(options.region_split_threshold, 19)
        self.assertEqual(codegen.main(["codegen.py", "missing.elf", "out.c",
                                       "--region-split-threshold=-1"]), 2)
        self.assertEqual(codegen.main(["codegen.py", "missing.elf", "out.c",
                                       "--region-split-threshold=many"]), 2)


def addiu(rt, rs, imm):
    return (0x09 << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


JR_RA = 0x03E00008
JR_T9 = 0x03200008
NOP = 0
SYSCALL = 0x0000000C


def b(pc, target):
    return beq(0, 0, pc, target)


def reachable(words, entry):
    """Every word the guest runs from ``entry``, as a delay slot or not: the reference model.

    A branch or jump runs its delay slot, then goes to its target (and, if it can fall
    through, to the word after the slot). Any other word goes to the next word. `j`, `jr`
    and `b` (beq $zero, $zero) never fall through. A target is entered as an ordinary
    word, delay slot or not.
    """
    entered, slots, work = set(), set(), [entry]
    while work:
        pc = work.pop()
        if pc in entered or pc not in words:
            continue
        entered.add(pc)
        w = words[pc]
        op, fn = w >> 26, w & 0x3F
        if op == 2:
            slots.add(pc + 4)
            work.append(codegen.jump_target(pc, w))
        elif op == 0 and fn == 0x08:
            slots.add(pc + 4)
        elif op == 4 or op == 5:
            slots.add(pc + 4)
            work.append(codegen.branch_target(pc, w))
            if not (op == 4 and (w >> 16) & 0x3FF == 0):
                work.append(pc + 8)
        elif w == SYSCALL:
            pass                                                  # an HLE boundary ends the body
        else:
            work.append(pc + 4)
    return {pc for pc in entered | slots if pc in words}


class StraightLineEndTests(unittest.TestCase):
    """#1730: a body owns what its entry can run, and nothing after a transfer that never
    falls through (`j`, `jr`, `jalr $zero` and the `b` idiom) unless another edge reaches it."""

    def flow(self, words, start=0x1000, known=None):
        span = (min(words), max(words) + 4)
        with region_split_threshold(None) as options:
            return codegen.function_flow(FakeElf(words), start, [span], known or {start},
                                        options=options)

    def emit(self, words, start=0x1000, known=None):
        span = (min(words), max(words) + 4)
        with region_split_threshold(None) as options:
            return "\n".join(codegen.emit_function(
                FakeElf(words), start, [span], known or {start}, options=options))

    def test_b_has_no_not_taken_successor(self):
        words = {0x1000: addiu(2, 0, 1), 0x1004: b(0x1004, 0x1014), 0x1008: addiu(2, 2, 2),
                 0x100C: addiu(2, 2, 0x100), 0x1010: JR_RA,  # the next function's code
                 0x1014: JR_RA, 0x1018: NOP}
        insns, labels, continuations = self.flow(words)
        self.assertEqual(insns, {0x1000, 0x1004, 0x1008, 0x1014, 0x1018})
        self.assertEqual(labels, {0x1014})
        self.assertEqual(continuations, {})
        self.assertNotIn("0x0000100cu", self.emit(words))

    def test_any_other_beq_keeps_its_not_taken_successor(self):
        for rs, rt in ((0, 5), (5, 0), (5, 5)):
            with self.subTest(rs=rs, rt=rt):
                words = {0x1000: beq(rs, rt, 0x1000, 0x1010), 0x1004: NOP,
                         0x1008: JR_RA, 0x100C: NOP, 0x1010: JR_RA, 0x1014: NOP}
                insns, _labels, _continuations = self.flow(words)
                self.assertEqual(insns, set(words))

    def test_word_after_b_reached_by_another_edge_keeps_its_label(self):
        words = {0x1000: addiu(8, 0, 2), 0x1004: b(0x1004, 0x1018), 0x1008: addiu(2, 2, 1),
                 0x100C: addiu(2, 2, 0x10),                       # reached from 0x1018 only
                 0x1010: addiu(8, 8, -1), 0x1014: NOP,
                 0x1018: bne(8, 0, 0x1018, 0x100C), 0x101C: addiu(2, 2, 0x100),
                 0x1020: JR_RA, 0x1024: NOP}
        insns, labels, _continuations = self.flow(words)
        self.assertEqual(insns, set(words))
        self.assertIn(0x100C, labels)
        text = self.emit(words)
        self.assertLess(text.index("L_0000100c:"), text.index("0x00001010u"))

    def test_branch_into_a_terminator_slot_continues_after_the_slot(self):
        # The slot runs as an ordinary word on the branch's path, then the next word.
        # Fails before #1730 for `j` and `jr` (the walk dropped slot+4 after their slot was
        # seen), and with a `b` rule that does not continue from a slot label.
        for name, terminator in (("b", lambda pc: b(pc, 0x101C)), ("j", lambda pc: j(0x101C)),
                                 ("jr", lambda pc: JR_T9)):
            with self.subTest(terminator=name):
                words = {0x1000: bne(4, 0, 0x1000, 0x100C), 0x1004: NOP,
                         0x1008: terminator(0x1008), 0x100C: addiu(2, 2, 1),
                         0x1010: addiu(2, 2, 0x10), 0x1014: JR_RA, 0x1018: NOP,
                         0x101C: addiu(2, 2, 0x100), 0x1020: JR_RA, 0x1024: NOP}
                insns, labels, _continuations = self.flow(words)
                self.assertEqual(insns, reachable(words, 0x1000))
                self.assertIn(0x100C, labels)
                self.assertTrue({0x1010, 0x1014, 0x1018} <= insns)
                text = self.emit(words)
                slot_label = text.index("L_0000100c:")
                self.assertLess(slot_label, text.index("0x00001010u", slot_label))
                if 0x101C in insns:
                    self.assertLess(text.index("0x00001010u", slot_label),
                                    text.index("L_0000101c:"))

    def test_emitter_refuses_a_slot_label_without_its_successor(self):
        # A walk that lost slot+4 (the pre-#1730 `j` defect) is a generator bug: the
        # emitter must stop rather than let the label's C fall into the next block.
        words = {0x1000: bne(4, 0, 0x1000, 0x100C), 0x1004: NOP,
                 0x1008: j(0x101C), 0x100C: addiu(2, 2, 1),
                 0x1010: addiu(2, 2, 0x10), 0x1014: JR_RA, 0x1018: NOP,
                 0x101C: addiu(2, 2, 0x100), 0x1020: JR_RA, 0x1024: NOP}
        lost = ({0x1000, 0x1004, 0x1008, 0x100C, 0x101C, 0x1020, 0x1024}, {0x100C, 0x101C}, {})
        with self.assertRaisesRegex(AssertionError, "delay slot of 0x00001008"):
            codegen.emit_function(FakeElf(words), 0x1000, [(0x1000, 0x1028)], {0x1000}, flow=lost)

    def test_slot_label_walked_before_or_after_its_b_gives_the_same_body(self):
        tail = {0x2000: b(0x2000, 0x2014), 0x2004: addiu(2, 2, 1), 0x2008: addiu(2, 2, 0x10),
                0x200C: JR_RA, 0x2010: NOP, 0x2014: JR_RA, 0x2018: NOP}
        label_first = {0x1000: bne(4, 0, 0x1000, 0x1100), 0x1004: NOP,
                       0x1008: bne(5, 0, 0x1008, 0x2004), 0x100C: NOP,
                       0x1010: JR_RA, 0x1014: NOP, 0x1100: j(0x2000), 0x1104: NOP, **tail}
        b_first = {0x1000: bne(5, 0, 0x1000, 0x2004), 0x1004: NOP,
                   0x1008: j(0x2000), 0x100C: NOP, **tail}
        for name, words in (("label first", label_first), ("b first", b_first)):
            with self.subTest(order=name):
                insns, _labels, _continuations = self.flow(words)
                self.assertEqual(insns, reachable(words, 0x1000))
                self.assertTrue({0x2004, 0x2008, 0x200C, 0x2010} <= insns)

    def test_a_syscall_slot_ends_the_line_in_either_walk_order(self):
        # A syscall in a terminator's slot, entered as a label, is an HLE boundary like any
        # syscall: the body is the same whichever the walk meets first, and emits.
        tail = {0x2000: b(0x2000, 0x2014), 0x2004: SYSCALL, 0x2008: addiu(2, 2, 0x10),
                0x200C: JR_RA, 0x2010: NOP, 0x2014: JR_RA, 0x2018: NOP}
        label_first = {0x1000: bne(4, 0, 0x1000, 0x1100), 0x1004: NOP,
                       0x1008: bne(5, 0, 0x1008, 0x2004), 0x100C: NOP,
                       0x1010: JR_RA, 0x1014: NOP, 0x1100: j(0x2000), 0x1104: NOP, **tail}
        b_first = {0x1000: bne(5, 0, 0x1000, 0x2004), 0x1004: NOP,
                   0x1008: j(0x2000), 0x100C: NOP, **tail}
        for name, words in (("label first", label_first), ("b first", b_first)):
            with self.subTest(order=name):
                insns, _labels, _continuations = self.flow(words)
                self.assertEqual(insns, reachable(words, 0x1000))
                self.assertNotIn(0x2008, insns)
                self.emit(words)

    def test_b_to_its_own_slot_runs_the_slot_again_and_continues(self):
        words = {0x1000: b(0x1000, 0x1004), 0x1004: addiu(2, 2, 1),
                 0x1008: addiu(2, 2, 0x10), 0x100C: JR_RA, 0x1010: NOP}
        insns, labels, _continuations = self.flow(words)
        self.assertEqual(insns, set(words))
        self.assertEqual(labels, {0x1004})

    def test_slot_successor_that_is_another_entry_is_a_continuation(self):
        words = {0x1000: bne(4, 0, 0x1000, 0x100C), 0x1004: NOP,
                 0x1008: b(0x1008, 0x1018), 0x100C: addiu(2, 2, 1),
                 0x1010: addiu(2, 2, 0x10), 0x1014: JR_RA,          # 0x1010: another entry
                 0x1018: JR_RA, 0x101C: NOP}
        insns, _labels, continuations = self.flow(words, known={0x1000, 0x1010})
        self.assertNotIn(0x1010, insns)
        self.assertEqual(continuations, {0x100C: 0x1010})
        text = self.emit(words, known={0x1000, 0x1010})
        self.assertIn("goto _sr_cont_00001010;", text)

    def test_patched_b_keeps_its_not_taken_path(self):
        words = {0x1000: b(0x1000, 0x1010), 0x1004: NOP, 0x1008: JR_RA, 0x100C: NOP,
                 0x1010: JR_RA, 0x1014: NOP}
        saved = dict(codegen.GUEST_PATCHES)
        codegen.GUEST_PATCHES[0x1000] = {"kind": "cond", "value": 0, "why": "test"}
        try:
            insns, _labels, _continuations = self.flow(words)
        finally:
            codegen.GUEST_PATCHES.clear()
            codegen.GUEST_PATCHES.update(saved)
        self.assertEqual(insns, set(words))
        saved_sites = dict(codegen.ENHANCEMENT_SITES)
        codegen.ENHANCEMENT_SITES[0x1000] = {"id": "test", "rewrite": {"kind": "word", "word": NOP}}
        try:
            insns, _labels, _continuations = self.flow(words)
        finally:
            codegen.ENHANCEMENT_SITES.clear()
            codegen.ENHANCEMENT_SITES.update(saved_sites)
        self.assertEqual(insns, set(words))

    def test_walk_ends_straight_line_exactly_where_the_analyzer_cfg_does(self):
        # Every direct conditional-branch encoding the walk takes, with rs/rt zero and
        # not: pc+8 is owned iff the canonical CFG gives the branch a fall-through edge.
        import analyze
        encodings = []
        for op in (4, 5, 6, 7, 20, 21, 22, 23):
            for rs in (0, 5):
                for rt in ((0, 6) if op in (4, 5, 20, 21) else (0,)):
                    encodings.append((op << 26) | (rs << 21) | (rt << 16))
        for sel in (0, 1, 2, 3, 0x10, 0x11):
            for rs in (0, 5):
                encodings.append((1 << 26) | (rs << 21) | (sel << 16))
        for op in (0x11, 0x12):
            for tf in range(4):
                encodings.append((op << 26) | (8 << 21) | (tf << 16))
        for base in encodings:
            word = base | 3                                   # target pc + 16
            with self.subTest(word=f"0x{word:08x}"):
                self.assertTrue(codegen.is_cond_branch(word))
                words = {0x1000: word, 0x1004: NOP, 0x1008: JR_RA, 0x100C: NOP,
                         0x1010: JR_RA, 0x1014: NOP}
                insns, _labels, _continuations = self.flow(words)
                falls_through = analyze.branch_kind(word) != analyze.BRANCH_UNCOND
                self.assertEqual(0x1008 in insns, falls_through)
                self.assertEqual(analyze.is_hard_terminator(word), not falls_through)

    def test_random_bodies_own_exactly_what_their_entry_runs(self):
        # Property test against the reference model above: with no other entries and no
        # split, a body is exactly the set of words its entry can run.
        import random
        rng = random.Random(1730)
        for case in range(400):
            n = rng.randrange(6, 40)
            base = 0x1000
            words, i = {}, 0
            while i < n:
                pc = base + 4 * i
                kind = rng.choice(("op", "op", "op", "syscall", "b", "beq", "bne", "j", "jr"))
                if kind in ("op", "syscall") or i + 1 >= n:
                    words[pc] = SYSCALL if kind == "syscall" else addiu(2, 2, i + 1)
                    i += 1
                    continue
                target = base + 4 * rng.randrange(n)
                if kind == "b":
                    words[pc] = b(pc, target)
                elif kind == "beq":
                    words[pc] = beq(4, 5, pc, target)
                elif kind == "bne":
                    words[pc] = bne(4, 0, pc, target)
                elif kind == "j":
                    words[pc] = j(target)
                else:
                    words[pc] = JR_RA
                # A delay slot is never a transfer; now and then it is a syscall.
                words[pc + 4] = SYSCALL if rng.random() < 0.15 else addiu(3, 3, i + 1)
                i += 2
            with self.subTest(case=case):
                insns, _labels, _continuations = self.flow(words, start=base)
                self.assertEqual(insns, reachable(words, base))
                self.emit(words, start=base)                       # the slot check holds


class StateResumeTests(unittest.TestCase):
    """``--state-resume`` (#2172): resume points for direct save states.

    The pass rewrites three statement shapes and nothing else, so these pin the shapes, the
    switch that is the body's one list of its points, the bodies it must leave alone and the
    default output. The executable proof is ``mingw32-make cosim-selftest-resume``.
    """

    RANGES = [(0x1000, 0x1100)]
    KNOWN = {0x1000, 0x1040}

    def words(self):
        return {
            0x1000: addiu(16, 0, 3),
            0x1004: jal(0x1040),
            0x1008: addiu(2, 2, 1),
            0x100C: addiu(16, 16, -1),
            0x1010: bne(16, 0, 0x1010, 0x1004),   # the loop edge: a yield point
            0x1014: NOP,
            0x1018: regimm(0x11, 4, 0x1018, 0x1040),   # bgezal: a call inside a block
            0x101C: NOP,
            0x1020: JR_RA,
            0x1024: NOP,
            0x1040: JR_RA,
            0x1044: NOP,
        }

    def emit(self, words=None, start=0x1000, known=None, **kwargs):
        options = codegen.CodegenOptions(state_resume=kwargs.pop("state_resume", True))
        return "\n".join(codegen.emit_function(
            FakeElf(words or self.words()), start, self.RANGES,
            self.KNOWN if known is None else known, options=options, **kwargs))

    def test_default_output_has_no_resume_point(self):
        plain = "\n".join(codegen.emit_function(
            FakeElf(self.words()), 0x1000, self.RANGES, self.KNOWN))
        self.assertEqual(self.emit(state_resume=False), plain)
        for token in ("_nk_resume", "_nk_enter", "SR_STATE_ENTRY", "SR_YIELD_PARK",
                      "sr_aot_return_boundary_begin_at", "sr_resume_"):
            self.assertNotIn(token, plain)

    def test_entry_tests_the_request_before_the_yield_point(self):
        text = self.emit()
        # The request is tested first, before the host-stack check and the entry yield, so
        # a resume counts no yield point and sr_resume_validate's question runs neither.
        self.assertIn("void f_00001000(CpuState *s) {\n"
                      "    SR_STATE_RESUME_TEST();\n"
                      "    SR_HOST_STACK_CHECK(0x00001000u);\n"
                      "    SR_STATE_ENTRY(s, 0x00001000u); _nk_enter: ;\n"
                      "    uint32_t _sp_entry = s->r[29]; (void)_sp_entry;", text)
        self.assertNotIn("SR_YIELD(s, 0x00001000u);", text)
        # A real resume makes the host-stack check in the switch; the question does not.
        self.assertIn("    const SrResumeEntry _nk_e = sr_resume_take(s, 0x00001000u);\n"
                      "    if (!_nk_e.probe) { SR_HOST_STACK_CHECK(0x00001000u); }\n", text)
        # A body with no direct call has no host-stack check to make.
        leaf = self.emit({0x1000: JR_RA, 0x1004: NOP}, known={0x1000})
        self.assertIn("    SR_STATE_RESUME_TEST();\n    SR_STATE_ENTRY(s, 0x00001000u);", leaf)
        self.assertNotIn("SR_HOST_STACK_CHECK", leaf)
        self.assertIn("case SR_RESUME_KEY(SR_RESUME_POINT_ENTRY, 0x00001000u): "
                      "SR_RESUME_AT(_nk_e, SR_RESUME_POINT_ENTRY, 0u); goto _nk_enter;", text)

    def test_loop_edge_parks_naming_its_target(self):
        text = self.emit()
        self.assertIn("if (_c) { SR_YIELD_PARK(s, 0x00001010u, 0x00001000u, 0x00001004u, "
                      "SR_RESUME_POINT_YIELD, _sp_entry); goto L_00001004; }", text)
        self.assertIn("case SR_RESUME_KEY(SR_RESUME_POINT_YIELD, 0x00001004u): "
                      "SR_RESUME_AT(_nk_e, SR_RESUME_POINT_YIELD, 0u); goto L_00001004;", text)

    def test_direct_call_records_the_caller_and_resumes_before_its_own_drain(self):
        text = self.emit()
        self.assertIn(
            "if (sr_aot_return_boundary_begin_at(s, 0x0000100cu, 0x00001004u, 0x00001040u, "
            "0x00001000u, _sp_entry)) return; f_00001040(s); R_0000100c: SR_TAIL_DRAIN(s); "
            "if (sr_aot_return_boundary_end(s)) return;", text)
        self.assertIn("case SR_RESUME_KEY(SR_RESUME_POINT_CALL, 0x0000100cu): "
                      "SR_RESUME_AT(_nk_e, SR_RESUME_POINT_CALL, 0x00001040u); "
                      "sr_resume_inner(s); goto R_0000100c;", text)
        # A linked branch's call sits in the branch's block; nothing after the label reads
        # the block's local, so it has a point too.
        self.assertIn("f_00001040(s); R_00001020: SR_TAIL_DRAIN(s); "
                      "if (sr_aot_return_boundary_end(s)) return; } }", text)

    def test_every_point_is_in_the_switch_once_and_anything_else_is_refused(self):
        text = self.emit()
        block = text[text.index("  _nk_resume: {"):]
        self.assertEqual(block.count("case SR_RESUME_KEY("), 4)
        self.assertEqual(len(set(re.findall(r"case SR_RESUME_KEY\(([^)]*)\)", block))), 4)
        self.assertIn("_sp_entry = _nk_e.sp;", block)
        self.assertIn("default: sr_resume_no_point(s, 0x00001000u, &_nk_e); return;", block)
        # The body's own exits cannot fall into the resume block.
        self.assertIn("    return;\n  _nk_resume: {", text)
        self.assertEqual(text.count("R_0000100c:"), 1)

    def test_pass_only_rewrites_its_three_shapes(self):
        plain = self.emit(state_resume=False).split("\n")
        resumed = self.emit().split("\n")
        self.assertEqual(resumed[1], "    SR_STATE_RESUME_TEST();")
        del resumed[1]
        body = resumed[:resumed.index("    return;")] + ["}", ""]
        self.assertEqual(len(body), len(plain))
        changed = [(a, b) for a, b in zip(plain, body, strict=True) if a != b]
        self.assertEqual(len(changed), 4)   # the entry, the loop edge, two direct calls
        for before, after in changed:
            self.assertTrue(
                "SR_YIELD(" in before or "sr_aot_return_boundary_begin(" in before, before)
            self.assertNotEqual(before, after)

    def test_a_body_with_guest_owned_stack_exits_keeps_its_text(self):
        words = {
            0x1000: (0x23 << 26) | (4 << 21) | (29 << 16),   # lw sp, 0(a0): the guest owns $sp
            0x1004: JR_RA,
            0x1008: NOP,
        }
        self.assertEqual(self.emit(words, known={0x1000}),
                         self.emit(words, known={0x1000}, state_resume=False))

    def test_a_live_frame_resume_entry_keeps_its_text(self):
        kwargs = {"resume_owners": {0x1040: 0x1000}, "resumable": True}
        self.assertEqual(self.emit(start=0x1040, known={0x1000}, **kwargs),
                         self.emit(start=0x1040, known={0x1000}, state_resume=False, **kwargs))

    def test_a_call_followed_by_more_of_its_line_gets_no_point(self):
        line = ("    if (sr_aot_return_boundary_begin(s, 0x0000100cu, 0x00001004u, 1u)) return; "
                "f_00001040(s); SR_TAIL_DRAIN(s); if (sr_aot_return_boundary_end(s)) return; "
                "if (s->flow_kind != 0u) return;")
        out = ["void f_00001000(CpuState *s) {", "    SR_YIELD(s, 0x00001000u);",
               "    uint32_t _sp_entry = s->r[29]; (void)_sp_entry;", line, "}", ""]
        result = codegen.state_resume_pass(out, 0x1000)
        self.assertIn(line, result)
        self.assertNotIn("SR_RESUME_POINT_CALL", "\n".join(result))

    def test_a_call_in_a_block_that_stays_open_gets_no_point(self):
        # An enhancement site declares locals, calls, and reads them on a later line. A
        # label after the call would be entered past their initializers.
        call = ("      if (sr_aot_return_boundary_begin(s, 0x0000100cu, 0x00001004u, 1u)) return; "
                "f_00001040(s); SR_TAIL_DRAIN(s); if (sr_aot_return_boundary_end(s)) return;")
        out = ["void f_00001000(CpuState *s) {", "    SR_YIELD(s, 0x00001000u);",
               "    uint32_t _sp_entry = s->r[29]; (void)_sp_entry;",
               "    { uint32_t _nk_a0 = s->r[4];", call,
               "      MEM_W32_PC(_nk_a0 + 16u, 0u, 0x00001004u); }",
               "    s->r[29] = _sp_entry; return;", "}", ""]
        result = "\n".join(codegen.state_resume_pass(out, 0x1000))
        self.assertIn(call, result)
        self.assertNotIn("SR_RESUME_POINT_CALL", result)
        self.assertIn("SR_STATE_ENTRY", result)
        # The same call with its block closed on its own line has one.
        closed = out[:4] + [call + " }"] + out[6:]
        self.assertIn("SR_RESUME_POINT_CALL", "\n".join(codegen.state_resume_pass(closed, 0x1000)))
        # A brace in a comment or a string is not a block.
        noisy = out[:3] + ["    /* } */ s->r[2] = (uint32_t)sizeof(\"}\") + (uint32_t)'}';", "    " + call.strip()] + out[6:]
        self.assertIn("SR_RESUME_POINT_CALL", "\n".join(codegen.state_resume_pass(noisy, 0x1000)))
        # A comment left open, a line comment and a preprocessor line cannot be read: the
        # body is left as it is.
        for unreadable in ("    /* open", "    // }", "#if 0"):
            hidden = out[:3] + [unreadable, "    " + call.strip()] + out[6:]
            self.assertIs(codegen.state_resume_pass(hidden, 0x1000), hidden, unreadable)

    def test_a_call_inside_a_loop_or_switch_gets_no_point(self):
        # Wherever the block was opened: a label inside it is not followed by straight-line
        # code, and the block's header may read a local the resume switch does not restore.
        call = ("if (sr_aot_return_boundary_begin(s, 0x0000100cu, 0x00001004u, 1u)) return; "
                "f_00001040(s); SR_TAIL_DRAIN(s); if (sr_aot_return_boundary_end(s)) return;")
        head = ["void f_00001000(CpuState *s) {", "    SR_YIELD(s, 0x00001000u);",
                "    uint32_t _sp_entry = s->r[29]; (void)_sp_entry;"]
        tail = ["    s->r[29] = _sp_entry; return;", "}", ""]
        for middle in (
            ["    while (s->r[8] != 0u) { " + call + " }"],
            # A brace inside the loop's own header does not use up the loop.
            ["    for (s->r[8] = (uint32_t){0}; s->r[8] < 2u; s->r[8]++) { " + call + " }"],
            ["    while (({ s->r[8]; })) { " + call + " }"],
            ["    while (s->r[8] != 0u) {", "      " + call + " }"],
            ["    for (;;) {", "      { " + call + " } }"],
            ["    { s->r[9] = 0u;", "      do {", "        " + call + " } while (s->r[9]); }"],
            ["    switch (s->r[8]) {", "      case 1: " + call + " }"],
        ):
            with self.subTest(middle=middle[0]):
                result = "\n".join(codegen.state_resume_pass(head + middle + tail, 0x1000))
                self.assertIn("SR_STATE_ENTRY", result)
                self.assertNotIn("SR_RESUME_POINT_CALL", result)
        # The same call in a plain block, and after a loop that has closed, has one: a
        # loop with a block, one without, and `do { } while (0);`.
        for middle in (["    if (s->r[8] != 0u) { " + call + " }"],
                       ["    while (s->r[8] != 0u) { s->r[8]--; }", "    " + call],
                       ["    for (int i = 0; i < 4; i++) s->r[8]++;", "    { " + call + " }"],
                       ["    do { s->r[8]--; } while (0);", "    { " + call + " }"]):
            with self.subTest(middle=middle[0]):
                self.assertIn("SR_RESUME_POINT_CALL", "\n".join(
                    codegen.state_resume_pass(head + middle + tail, 0x1000)))

    def test_the_call_allow_list_declares_nothing_in_the_runtime_header(self):
        # A function-scope `NAME(args);` keeps a body's resume points only for these names
        # (codegen._STATE_RESUME_CALLS); a macro among them that declared a local would leave
        # that local live, unrestored, at every resume label. Each is pinned to its definitions.
        macros, functions = _header_definitions(RECOMP_H.read_text(encoding="utf-8"))
        for name in sorted(codegen._STATE_RESUME_CALLS):
            with self.subTest(name=name):
                self.assertTrue(name in macros or name in functions, f"{name} is not in recomp.h")
                self.assertTrue(_declares_nothing(name, macros, functions),
                                f"{name} may declare a name at the statement's scope: {macros.get(name)}")
        # The reading itself: a declaring body, one hidden behind another macro, a block that
        # is not the whole body, and a name defined nowhere are each caught.
        for header, name in (
            ("#define SR_X(pc) uint32_t _chk = (pc)\n", "SR_X"),
            ("#define SR_X(pc) SR_Y((pc))\n#define SR_Y(v) uint32_t _y = (v)\n", "SR_X"),
            ("#define SR_X(pc) do { (void)(pc); } while (0); uint32_t _z = 0\n", "SR_X"),
            ("#define SR_X(pc) (void)(pc), uint32_t _z\n", "SR_X"),
            ("", "SR_X"),
        ):
            with self.subTest(header=header):
                self.assertFalse(_declares_nothing(name, *_header_definitions(header)))
        for header in ("#define SR_X(pc) do { uint32_t _t = (pc); (void)_t; } while (0)\n",
                       "#define SR_X(pc) ((void)(pc))\n",
                       "static inline void sr_x(uint32_t pc) { (void)pc; }\n#define SR_X(pc) sr_x((pc))\n"):
            with self.subTest(header=header):
                self.assertTrue(_declares_nothing("SR_X", *_header_definitions(header)))

    def test_a_body_with_another_function_scope_local_keeps_its_text(self):
        # The resume switch restores the entry $sp only; any other local would be live at
        # every label.
        head = ["void f_00001000(CpuState *s) {", "    SR_YIELD(s, 0x00001000u);",
                "    uint32_t _sp_entry = s->r[29]; (void)_sp_entry;"]
        tail = ["    s->r[29] = _sp_entry; return;", "}", ""]
        for declaration in (
            "    uint32_t _gpr8 = s->r[8];",
            "    unsigned _x = 1u;",
            "    unsigned int _x;",
            "    bool _x = 0;",
            "    uintptr_t _x = 0;",
            "    CpuState *_x = s;",
            "    RecompFn _x = NULL;",
            "    struct T _x;",
            "    volatile uint32_t _x = 0u;",
            "    long long _x = 0;",
            "    void (*_x)(void) = NULL;",
            "    s->r[2] = 0u; uint32_t _late = 1u;",
            "    { s->r[2] = 0u;\n    }\n    uint32_t _x = 1u;",
            "    { s->r[2] = 0u; } uint32_t _x = 1u;",
            "  L_00001004: uint32_t _x = s->r[8];",
            "    uint32_t (*_x)[2] = NULL;",
            "    __typeof__(s->r[8]) _x = 0u;",
            "    __attribute__((unused)) uint32_t _x = 0u;",
            "    _Alignas(8) uint32_t _x = 0u;",
            "    _Atomic(uint32_t) _x = 0u;",
            "    SR_NOTE(s) uint32_t _x = 0u;",
            "    uint32_t\n    _x = 1u;",
            # A call-shaped statement whose name could be a macro that declares a local.
            "    NK_DECLARE_LOCAL(_x);",
            "    sr_note(s);",
            "    sr_note(s) ? (void)0 : (void)0;",
            "    f_0000100(s);",
        ):
            with self.subTest(declaration=declaration):
                out = head + [declaration] + tail
                self.assertIs(codegen.state_resume_pass(out, 0x1000), out)
        # The same, cut across two lines.
        out = head + ["    uint32_t", "    _x = 1u;"] + tail
        self.assertIs(codegen.state_resume_pass(out, 0x1000), out)
        # Statements that only look like one keep the body's points, and so does a local
        # inside a block.
        for statement in (
            "    charge = 1;", "    double_buf[0] = 1;", "    intx = 2;", "    SR_COUNT_total = 1;",
            "    goto L_00001004;", "    (void)_sp_entry;", "    s->r[2] = s->r[3] * s->r[4];",
            "    { uint32_t _t = s->r[8]; s->r[9] = _t; }",
            "    if (s->r[8]) { s->r[9] = 0u; } else { s->r[9] = 1u; }",
            "    sr_begin(s, 0x00001000u, 0x24100003u); s->r[16] = (0u + 3u); sr_end(s, 0u, 0);",
            "    SR_HOST_STACK_CHECK(0x00001000u);",
            "    MEM_W32_PC(s->r[29] + 12u, s->r[31], 0x00001004u);",
            "  L_00001004: ;",
            "    f_00001040(s);",
            "    sr_tail_record(s, 0x00001040u);",
            # A loop header's own local ends with the loop, and no label is granted inside one.
            "    for (uint32_t _i = (uint32_t){0}; _i < 2u; _i++) { s->r[8]++; }",
        ):
            with self.subTest(statement=statement):
                out = head + [statement] + tail
                self.assertIn("SR_STATE_ENTRY", "\n".join(codegen.state_resume_pass(out, 0x1000)))

    def test_a_loop_edge_to_a_label_inside_a_block_keeps_its_plain_yield(self):
        edge = "      if (_c) { SR_YIELD(s, 0x00001010u); goto L_00001004; } }"
        out = ["void f_00001000(CpuState *s) {", "    SR_YIELD(s, 0x00001000u);",
               "    uint32_t _sp_entry = s->r[29]; (void)_sp_entry;",
               "    { uint32_t _t = s->r[8];", "  L_00001004: ;", "      s->r[9] = _t; }",
               "    { uint32_t _c = (s->r[16] != 0u);", edge,
               "    s->r[29] = _sp_entry; return;", "}", ""]
        result = "\n".join(codegen.state_resume_pass(out, 0x1000))
        self.assertIn(edge, result)
        self.assertNotIn("SR_RESUME_POINT_YIELD", result)
        scoped = out[:3] + ["  L_00001004: ;"] + out[6:]
        self.assertIn("SR_RESUME_KEY(SR_RESUME_POINT_YIELD, 0x00001004u)",
                      "\n".join(codegen.state_resume_pass(scoped, 0x1000)))

    def test_option_is_parsed_and_refuses_what_it_cannot_restore(self):
        options = codegen.CodegenOptions()
        cli = codegen._command_line_parser().parse_args(["--state-resume"])
        codegen._apply_options(cli, codegen._validated_options(cli), options)
        self.assertTrue(options.state_resume)
        self.assertFalse(codegen.CodegenOptions().state_resume)
        for other in ("--stack-census", "--promote-registers", "--backend=llvm", "--lle-cpu",
                      "--lle-import-seam"):
            cli = codegen._command_line_parser().parse_args(["--state-resume", other])
            with self.assertRaises(codegen._Refusal) as refusal:
                codegen._validated_options(cli)
            self.assertIn(f"--state-resume cannot be combined with {other}",
                          str(refusal.exception.args))
            # Each is accepted alone.
            codegen._validated_options(codegen._command_line_parser().parse_args([other]))


if __name__ == "__main__":
    unittest.main()
