# SPDX-License-Identifier: GPL-3.0-or-later

"""Regression tests for Prx._apply's R_MIPS_HI16/LO16 pairing.

2026-07-17: a source review found that the HI16 handler's forward scan for its
paired low half accepted the FIRST relocation of any type after it, rather than
specifically an R_MIPS_LO16 (or the PSP's R_MIPS_16 alternative). An unrelated
relocation record (R_MIPS_32, R_MIPS_26, ...) sitting between an HI16 and its
real LO16 in table order was misread as the low addend, corrupting the
reconstructed upper-16 immediate whenever the two words' sign bits disagreed.
These tests exercise Prx._apply directly (not the full ELF/section parser) so
each scenario is a small, explicit table of relocation records.
"""

from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import prxload
from prxload import (
    R_MIPS_16,
    R_MIPS_26,
    R_MIPS_32,
    R_MIPS_GPREL16,
    R_MIPS_HI16,
    R_MIPS_LITERAL,
    R_MIPS_LO16,
    R_MIPS_NONE,
    R_MIPS_REL32,
)


def make_prx(words, seg_vaddr):
    """A Prx instance with just enough state for _apply: a flat little-endian
    word array at guest base 0, and a caller-supplied segment vaddr table."""
    prx = object.__new__(prxload.Prx)
    prx.lo = 0
    prx.seg_vaddr = list(seg_vaddr)
    mem = bytearray(len(words) * 4)
    for i, w in enumerate(words):
        struct.pack_into("<I", mem, i * 4, w & 0xFFFFFFFF)
    prx.mem = mem
    return prx


def info(rtype, ofs_seg=0, addr_seg=0):
    return (rtype & 0xFF) | ((ofs_seg & 0xFF) << 8) | ((addr_seg & 0xFF) << 16)


class Hi16Lo16PairingTests(unittest.TestCase):
    def test_unknown_relocation_type_fails_loudly(self):
        # Type 0xF is outside the recognized Type-A PSP relocation set.  Keep
        # the fixture minimal so the test reaches the central relocation
        # dispatcher with a valid segment and target word.
        prx = make_prx([0xDEADBEEF], seg_vaddr=[0])

        with self.assertRaisesRegex(
            ValueError,
            r"unsupported Type-A relocation type 0xf at offset 0x00000000"
            r" \(offset segment 0, target segment 0\)",
        ):
            prx._apply([(0, info(0xF))])
        with self.assertRaisesRegex(
            ValueError,
            r"unsupported Type-A relocation type 0x15 at offset 0x00000000"
            r" \(offset segment 0, target segment 0\)",
        ):
            prx._apply([(0, info(0x15))])
        with self.assertRaisesRegex(
            ValueError,
            r"unsupported Type-A relocation type 0x16 at offset 0x00000004"
            r" \(offset segment 0, target segment 0\)",
        ):
            prx2 = make_prx([0x3C020000, 0x24420000], seg_vaddr=[0])
            prx2._apply([(0, info(R_MIPS_HI16)), (4, info(0x16))])

    def test_known_noop_relocation_types_remain_nonfatal(self):
        # These values are recognized Type-A categories, not unknown types:
        # NONE is a deliberate loader no-op and LITERAL is firmware's
        # diagnostic-only relocation category. GPREL16 (kind 7) is refused per
        # PRX_LOADER_SPEC.md section 3.6.
        for rtype in (R_MIPS_NONE, R_MIPS_LITERAL):
            with self.subTest(rtype=rtype):
                prx = make_prx([0xDEADBEEF], seg_vaddr=[0])
                prx._apply([(0, info(rtype))])
                self.assertEqual(prx.r32(0), 0xDEADBEEF)
        prx_gp = make_prx([0xDEADBEEF], seg_vaddr=[0])
        with self.assertRaisesRegex(ValueError, r"format A kind 7 \(GPREL16\) refused"):
            prx_gp._apply([(0, info(R_MIPS_GPREL16))])

    def test_known_but_unsupported_type_a_relocation_fails_loudly(self):
        # R_MIPS_REL32 is a known MIPS/PSP relocation vocabulary value, but it
        # is not supported by the Type-A section path (packed streams have a
        # separate dispatcher).
        prx = make_prx([0xDEADBEEF], seg_vaddr=[0])

        with self.assertRaisesRegex(ValueError, r"unsupported Type-A relocation type 0x3"):
            prx._apply([(0, info(R_MIPS_REL32))])

    def test_normal_hi16_lo16_pair(self):
        # lui $2,0x0010 ; addiu $2,$2,0x2000 relocated by +0x1000: the classic
        # case, no interference, sanity-checks the carry arithmetic itself.
        prx = make_prx([0x3C020010, 0x24422000], seg_vaddr=[0, 0x1000])
        rels = [(0, info(R_MIPS_HI16, addr_seg=1)), (4, info(R_MIPS_LO16, addr_seg=1))]
        prx._apply(rels)
        # True target = 0x00102000 + 0x1000 = 0x00103000 -> hi=0x0010, lo=0x3000.
        self.assertEqual(prx.r32(0) & 0xFFFF, 0x0010)
        self.assertEqual(prx.r32(4) & 0xFFFF, 0x3000)

    def test_unrelated_relocation_between_hi16_and_lo16_is_paired_per_spec(self):
        # Per PRX_LOADER_SPEC.md section 3.6 and prx_loader.c, a HI16 run pairs
        # with the FIRST non-HI16 record after the run, whatever its kind.
        # Here the first non-HI16 record has low16=0x7fff; adding +0x10 crosses
        # the 0x8000 carry boundary (0x7fff + 0x10 = 0x800f) and bumps hi to 0x0001.
        prx = make_prx(
            [0x3C020000, 0x12347FFF, 0x24420000],
            seg_vaddr=[0, 0x10],
        )
        rels = [
            (0, info(R_MIPS_HI16, addr_seg=1)),
            (4, info(R_MIPS_32, addr_seg=0)),
            (8, info(R_MIPS_LO16, addr_seg=1)),
        ]
        prx._apply(rels)
        self.assertEqual(prx.r32(0), 0x3C020001, "HI16 must pair with the first non-HI16 record")

    def test_unrelated_relocation_types_all_pair_per_spec(self):
        # Same shape as above with R_MIPS_26 as the first non-HI16 record.
        prx = make_prx(
            [0x3C020000, 0x0BFFFFFF, 0x24420000],
            seg_vaddr=[0, 0x10],
        )
        rels = [
            (0, info(R_MIPS_HI16, addr_seg=1)),
            (4, info(R_MIPS_26, addr_seg=0)),
            (8, info(R_MIPS_LO16, addr_seg=1)),
        ]
        prx._apply(rels)
        # Partner low16 is 0xffff (-1); -1 + 0x10 = 0x0000000f -> hi stays 0x0000.
        # Test with low16=0x7fff on the R_MIPS_26 word to distinguish from skipping:
        prx2 = make_prx(
            [0x3C020000, 0x0BFF7FFF, 0x24420000],
            seg_vaddr=[0, 0x10],
        )
        prx2._apply(rels)
        self.assertEqual(prx2.r32(0), 0x3C020001, "HI16 must pair with the first non-HI16 R_MIPS_26 record")

    def test_multiple_hi16_share_one_lo16(self):
        # Two HI16 records (e.g. two `lui`s addressing the same symbol) both
        # pair with the single LO16 that follows both of them.
        prx = make_prx(
            [0x3C020000, 0x3C030000, 0x24428100],
            seg_vaddr=[0, 0x10],
        )
        rels = [
            (0, info(R_MIPS_HI16, addr_seg=1)),
            (4, info(R_MIPS_HI16, addr_seg=1)),
            (8, info(R_MIPS_LO16, addr_seg=1)),
        ]
        prx._apply(rels)
        # raw lo16 = 0x8100 -> s16 = 0x8100-0x10000 = -0x7f00 (bit15 set, carry).
        # cur = 0 + (-0x7f00) + 0x10 = 0xffff8110 -> hi = 0xffff+1 = 0x0000.
        self.assertEqual(prx.r32(0) & 0xFFFF, 0x0000)
        self.assertEqual(prx.r32(4) & 0xFFFF, 0x0000)

    def test_psp_r_mips_16_is_a_valid_lo16_partner(self):
        # The PSP toolchain's alternative low-half relocation type (1) must be
        # accepted as a pairing target exactly like R_MIPS_LO16.
        prx = make_prx([0x3C020000, 0x24420000], seg_vaddr=[0, 0x8100])
        rels = [(0, info(R_MIPS_HI16, addr_seg=1)), (4, info(R_MIPS_16, addr_seg=1))]
        prx._apply(rels)
        # relocate_to alone (0x8100) crosses the carry boundary: cur=0x8100 -> hi=1.
        self.assertEqual(prx.r32(0) & 0xFFFF, 0x0001)

    def test_hi16_with_no_paired_partner_raises(self):
        # A HI16 run reaching the end of the table with no partner must fail closed.
        prx = make_prx([0x3C020000], seg_vaddr=[0, 0x10000])
        rels = [(0, info(R_MIPS_HI16, addr_seg=1))]
        with self.assertRaisesRegex(ValueError, "HI16 run without partner"):
            prx._apply(rels)


class PackedRelocationSegmentBitsTests(unittest.TestCase):
    """Type-B (0x700000a1) streams size the segment field from the PT_LOAD
    segment count (2 bits when >= 3 load segments, else 1 bit), matching
    PRX_LOADER_SPEC.md section 3.7 and apply_format_b in src/rt/prx_loader.c."""

    class _Image:
        def __init__(self, table, lo, words, nloads=2, nphdrs=3):
            self.data = table
            self.segments = [{} for _ in range(nphdrs)]
            self.seg_vaddr = [lo + i * 0x1000 for i in range(nloads)]
            self.lo = lo
            self.mem = bytearray(nloads * 0x1000)
            for addr, word in words.items():
                struct.pack_into("<I", self.mem, addr - lo, word)

        def r32(self, addr):
            return struct.unpack_from("<I", self.mem, addr - self.lo)[0]

        def w32(self, addr, value):
            struct.pack_into("<I", self.mem, addr - self.lo, value)

    def test_jal_in_first_segment_is_relocated(self):
        lo = 0x32200000
        flag_bits, type_bits, seg_bits = 2, 2, 1
        # flag table [size, set-base, relocate]; type table [size, none, JAL(7)]
        header = bytes([0, 0, flag_bits, type_bits, 3, 0x00, 0x01, 3, 0, 7])
        set_base = (0 << (seg_bits + flag_bits)) | (0 << flag_bits) | 1
        reloc = (0x34 << (type_bits + seg_bits + flag_bits)) | (2 << (seg_bits + flag_bits)) | (0 << flag_bits) | 2
        table = header + struct.pack("<HH", set_base, reloc)
        img = self._Image(table, lo, {lo + 0x34: 0x0C00001E, lo + 0x30: 0x27BDFFC0})
        count = prxload.Prx._apply_packed(img, {"seg_idx": 2, "off": 0, "size": len(table)})
        self.assertEqual(count, 1)
        self.assertEqual(img.r32(lo + 0x34), 0x0C88001E)   # jal 0x32200078
        self.assertEqual(img.r32(lo + 0x30), 0x27BDFFC0)   # neighbour untouched

    def test_segment_bits_uses_load_count_not_reloc_phdr_index(self):
        lo = 0x32200000
        flag_bits, type_bits, seg_bits = 2, 2, 1
        header = bytes([0, 0, flag_bits, type_bits, 3, 0x00, 0x01, 3, 0, 7])
        set_base = (0 << (seg_bits + flag_bits)) | (0 << flag_bits) | 1
        reloc = (0x34 << (type_bits + seg_bits + flag_bits)) | (2 << (seg_bits + flag_bits)) | (0 << flag_bits) | 2
        table = header + struct.pack("<HH", set_base, reloc)
        # 2 PT_LOAD segments -> seg_bits must be 1 even when rel_seg_idx is 3.
        img = self._Image(table, lo, {lo + 0x34: 0x0C00001E}, nloads=2, nphdrs=4)
        count = prxload.Prx._apply_packed(img, {"seg_idx": 3, "off": 0, "size": len(table)})
        self.assertEqual(count, 1)
        self.assertEqual(img.r32(lo + 0x34), 0x0C88001E)

    def test_packed_relocation_rejects_invalid_headers_and_commands(self):
        lo = 0x08804000
        flag_bits, type_bits, seg_bits = 2, 2, 1
        # 1. Nonzero header bytes 0-1
        bad_hdr = bytes([1, 0, flag_bits, type_bits, 3, 0x00, 0x01, 3, 0, 2])
        img = self._Image(bad_hdr, lo, {lo: 0})
        with self.assertRaisesRegex(ValueError, "header bytes 0-1"):
            prxload.Prx._apply_packed(img, {"seg_idx": 2, "off": 0, "size": len(bad_hdr)})

        # 2. flag_idx == 0
        hdr = bytes([0, 0, flag_bits, type_bits, 3, 0x00, 0x01, 3, 0, 2])
        cmd_flag0 = hdr + struct.pack("<H", 0)
        img = self._Image(cmd_flag0, lo, {lo: 0})
        with self.assertRaisesRegex(ValueError, "flag index"):
            prxload.Prx._apply_packed(img, {"seg_idx": 2, "off": 0, "size": len(cmd_flag0)})

        # 3. type_idx == 0 on a relocate command
        set_base = (0 << (seg_bits + flag_bits)) | (0 << flag_bits) | 1
        reloc_type0 = (0 << (type_bits + seg_bits + flag_bits)) | (0 << (seg_bits + flag_bits)) | (0 << flag_bits) | 2
        cmd_type0 = hdr + struct.pack("<HH", set_base, reloc_type0)
        img = self._Image(cmd_type0, lo, {lo: 0})
        with self.assertRaisesRegex(ValueError, "type index"):
            prxload.Prx._apply_packed(img, {"seg_idx": 2, "off": 0, "size": len(cmd_type0)})

        # 4. Invalid set-offset form (0x02)
        hdr_bad_set = bytes([0, 0, flag_bits, type_bits, 3, 0x02, 0x01, 3, 0, 2])
        cmd_bad_set = hdr_bad_set + struct.pack("<H", set_base)
        img = self._Image(cmd_bad_set, lo, {lo: 0})
        with self.assertRaisesRegex(ValueError, "set-offset form"):
            prxload.Prx._apply_packed(img, {"seg_idx": 2, "off": 0, "size": len(cmd_bad_set)})

        # 5. Invalid relocate-offset form (0x06)
        hdr_bad_rel = bytes([0, 0, flag_bits, type_bits, 3, 0x00, 0x07, 3, 0, 2])
        reloc_ok = (0 << (type_bits + seg_bits + flag_bits)) | (2 << (seg_bits + flag_bits)) | (0 << flag_bits) | 2
        cmd_bad_rel = hdr_bad_rel + struct.pack("<HH", set_base, reloc_ok)
        img = self._Image(cmd_bad_rel, lo, {lo: 0})
        with self.assertRaisesRegex(ValueError, "relocate-offset form"):
            prxload.Prx._apply_packed(img, {"seg_idx": 2, "off": 0, "size": len(cmd_bad_rel)})

        # 6. Invalid addend form ((flag >> 3) & 7 == 3 -> flag 0x19)
        hdr_bad_add = bytes([0, 0, flag_bits, type_bits, 3, 0x00, 0x19, 3, 0, 2])
        cmd_bad_add = hdr_bad_add + struct.pack("<HH", set_base, reloc_ok)
        img = self._Image(cmd_bad_add, lo, {lo: 0})
        with self.assertRaisesRegex(ValueError, "addend"):
            prxload.Prx._apply_packed(img, {"seg_idx": 2, "off": 0, "size": len(cmd_bad_add)})

        # 7. Segment index out of range (nloads=1 -> seg_idx=1 is >= nseg)
        reloc_seg1 = (0 << (type_bits + seg_bits + flag_bits)) | (2 << (seg_bits + flag_bits)) | (1 << flag_bits) | 2
        cmd_seg_oob = hdr + struct.pack("<HH", set_base, reloc_seg1)
        img_1seg = self._Image(cmd_seg_oob, lo, {lo: 0}, nloads=1, nphdrs=3)
        with self.assertRaisesRegex(ValueError, "segment index"):
            prxload.Prx._apply_packed(img_1seg, {"seg_idx": 2, "off": 0, "size": len(cmd_seg_oob)})

        # 8. Unknown packed rtype (8)
        hdr_bad_rtype = bytes([0, 0, flag_bits, type_bits, 3, 0x00, 0x01, 3, 0, 8])
        cmd_bad_rtype = hdr_bad_rtype + struct.pack("<HH", set_base, reloc_ok)
        img = self._Image(cmd_bad_rtype, lo, {lo: 0})
        with self.assertRaisesRegex(ValueError, "packed relocation type"):
            prxload.Prx._apply_packed(img, {"seg_idx": 2, "off": 0, "size": len(cmd_bad_rtype)})


if __name__ == "__main__":
    unittest.main()
