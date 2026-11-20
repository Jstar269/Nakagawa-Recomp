# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Synthetic, retail-free malformed-GIM coverage for issue #171."""

from __future__ import annotations

import struct
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from extract_xb import decode_gim_data, unswizzle  # noqa: E402


def block(block_id: int, content: bytes, *, header_size: int = 16, size: int | None = None) -> bytes:
    actual_size = 16 + len(content) if size is None else size
    header = bytearray(16)
    struct.pack_into("<H", header, 0, block_id)
    struct.pack_into("<I", header, 4, actual_size)
    struct.pack_into("<I", header, 12, header_size)
    return bytes(header) + content


def image_block(*, fmt: int = 5, width: int = 2, height: int = 2,
                raw: bytes = b"\x00\x01\x02\x03", swiz: int = 0,
                pitch_align: int = 0, height_align: int = 0,
                data_end: int | None = None) -> bytes:
    content = bytearray(36)
    struct.pack_into("<H", content, 4, fmt)
    struct.pack_into("<H", content, 6, swiz)
    struct.pack_into("<H", content, 8, width)
    struct.pack_into("<H", content, 10, height)
    struct.pack_into("<H", content, 14, pitch_align)
    struct.pack_into("<H", content, 16, height_align)
    end = 36 + len(raw) if data_end is None else data_end
    struct.pack_into("<I", content, 28, 36)
    struct.pack_into("<I", content, 32, end)
    return block(0x0004, bytes(content) + raw)


def palette_block(*, fmt: int = 3, raw: bytes = b"\x00\x00\xff\xff") -> bytes:
    content = bytearray(36)
    struct.pack_into("<H", content, 4, fmt)
    struct.pack_into("<I", content, 28, 36)
    struct.pack_into("<I", content, 32, 36 + len(raw))
    return block(0x0005, bytes(content) + raw)


def gim(*blocks: bytes) -> bytes:
    return b"MIG.00.1PSP" + b"\0" * 5 + b"".join(blocks)


def swizzle(data: bytes, pitch: int, height: int) -> bytes:
    result = bytearray(len(data))
    rowblocks = pitch // 16
    for y in range(height):
        for x in range(pitch):
            block_idx = (x // 16) + (y // 8) * rowblocks
            source_offset = block_idx * 128 + (y % 8) * 16 + (x % 16)
            linear_offset = y * pitch + x
            result[source_offset] = data[linear_offset]
    return bytes(result)


class _CountingBytes(bytes):
    accesses: dict[str, int]

    def __new__(cls, payload: bytes, accesses: dict[str, int]):
        value = super().__new__(cls, payload)
        value.accesses = accesses
        return value

    def __getitem__(self, key):
        if isinstance(key, slice):
            self.accesses["slices"] += 1
            return _CountingBytes(super().__getitem__(key), self.accesses)
        self.accesses["scalars"] += 1
        return super().__getitem__(key)


class TestGimBounds(unittest.TestCase):
    def test_rgba8888_direct_preserves_rgba_channel_order(self) -> None:
        rgba = bytes((0x12, 0x34, 0x56, 0x78))
        raw = rgba + bytes(12)
        decoded = decode_gim_data(
            gim(image_block(fmt=3, width=1, height=1, raw=raw))
        )
        self.assertIsNotNone(decoded)
        assert decoded is not None
        self.assertEqual(decoded[2], rgba)

    def test_t8_rgba8888_palette_preserves_rgba_channel_order(self) -> None:
        rgba = bytes((0x12, 0x34, 0x56, 0x78))
        decoded = decode_gim_data(
            gim(
                image_block(fmt=5, width=1, height=1, raw=bytes(16)),
                palette_block(fmt=3, raw=rgba),
            )
        )
        self.assertIsNotNone(decoded)
        assert decoded is not None
        self.assertEqual(decoded[2], rgba)

    def test_t8_16_bit_palette_entries_match_direct_pixels(self) -> None:
        cases = (
            (0, b"\x1f\x00\xe0\x07"),  # 5650 red, green
            (1, b"\x1f\x80\xe0\x03"),  # 5551 opaque red, transparent green
            (2, b"\x0f\xf0\xf0\x0f"),  # 4444 red, green
        )
        for fmt, entries in cases:
            with self.subTest(fmt=fmt):
                direct = decode_gim_data(
                    gim(image_block(fmt=fmt, width=2, height=1, raw=entries + bytes(12)))
                )
                indexed = decode_gim_data(
                    gim(
                        image_block(fmt=5, width=2, height=1, raw=bytes((0, 1)) + bytes(14)),
                        palette_block(fmt=fmt, raw=entries),
                    )
                )
                self.assertIsNotNone(direct)
                self.assertIsNotNone(indexed)
                assert direct is not None and indexed is not None
                self.assertEqual(indexed[2], direct[2])

    def test_multiple_palette_blocks_are_rejected_as_ambiguous(self) -> None:
        decoded = decode_gim_data(
            gim(
                image_block(fmt=5, width=1, height=1, raw=bytes(16)),
                palette_block(fmt=3, raw=b"\x01\x02\x03\x04"),
                palette_block(fmt=3, raw=b"\x05\x06\x07\x08"),
            )
        )
        self.assertIsNone(decoded)

    def test_default_pitch_alignment_is_used_for_linear_rows(self) -> None:
        rgba0 = b"\x11\x22\x33\xff"
        rgba1 = b"\xaa\xbb\xcc\xff"
        row = bytes(8) + bytes((1,)) * 8
        raw = row + row
        decoded = decode_gim_data(
            gim(
                image_block(fmt=5, width=8, height=2, raw=raw),
                palette_block(fmt=3, raw=rgba0 + rgba1),
            )
        )
        self.assertIsNotNone(decoded)
        assert decoded is not None
        self.assertEqual(decoded[2], rgba0 * 16)

    def test_swizzled_image_uses_aligned_pitch_and_height_then_crops(self) -> None:
        width, height, pitch, padded_height = 20, 10, 32, 16
        linear = bytearray(pitch * padded_height)
        expected = bytearray()
        for y in range(padded_height):
            for x in range(pitch):
                value = (x + y) & 1
                linear[y * pitch + x] = value
                if y < height and x < width:
                    expected.append(value)
        rgba0 = b"\x10\x20\x30\xff"
        rgba1 = b"\x40\x50\x60\xff"
        decoded = decode_gim_data(
            gim(
                image_block(
                    fmt=5,
                    width=width,
                    height=height,
                    raw=swizzle(bytes(linear), pitch, padded_height),
                    swiz=1,
                ),
                palette_block(fmt=3, raw=rgba0 + rgba1),
            )
        )
        self.assertIsNotNone(decoded)
        assert decoded is not None
        expected_rgba = b"".join(rgba1 if value else rgba0 for value in expected)
        self.assertEqual(decoded[2], expected_rgba)

    def test_unswizzle_copies_tile_rows_without_per_byte_reads(self) -> None:
        pitch, height = 32, 16
        linear = bytes(index % 256 for index in range(pitch * height))
        accesses = {"slices": 0, "scalars": 0}
        decoded = unswizzle(_CountingBytes(swizzle(linear, pitch, height), accesses), pitch, height)

        self.assertEqual(decoded, linear)
        self.assertEqual(accesses["scalars"], 0)
        self.assertEqual(accesses["slices"], height * (pitch // 16))

    def test_rgba8888_conversion_reads_whole_rows(self) -> None:
        rgba = bytes((0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0))
        source = gim(image_block(fmt=3, width=2, height=1, raw=rgba + bytes(8)))
        accesses = {"slices": 0, "scalars": 0}
        decoded = decode_gim_data(_CountingBytes(source, accesses))

        self.assertIsNotNone(decoded)
        assert decoded is not None
        self.assertEqual(decoded[2], rgba)
        self.assertEqual(accesses["scalars"], 0)

    def test_valid_t8_fixture_decodes(self) -> None:
        raw = image_block(raw=bytes(32))
        decoded = decode_gim_data(gim(raw, palette_block(raw=b"\x00\x00\xff\xff")))
        self.assertIsNotNone(decoded)
        assert decoded is not None
        self.assertEqual(decoded[:2], (2, 2))
        self.assertEqual(len(decoded[2]), 16)

    def test_truncated_or_inconsistent_block_is_rejected(self) -> None:
        malformed = bytearray(gim(image_block()))
        struct.pack_into("<I", malformed, 16 + 4, 0)
        self.assertIsNone(decode_gim_data(bytes(malformed)))

        malformed = bytearray(gim(image_block()))
        struct.pack_into("<I", malformed, 16 + 12, 64)
        self.assertIsNone(decode_gim_data(bytes(malformed)))

        self.assertIsNone(decode_gim_data(gim(block(0x0004, b"\0" * 8))))
        self.assertIsNone(decode_gim_data(gim(block(0x0005, b"\0" * 8))))

    def test_dimensions_are_rejected_before_pixel_allocation(self) -> None:
        self.assertIsNotNone(
            decode_gim_data(gim(image_block(width=4096, height=1, raw=b"\0" * 4096), palette_block()))
        )
        self.assertIsNone(decode_gim_data(gim(image_block(width=4097, height=1, raw=b"\0" * 4097), palette_block())))
        self.assertIsNone(decode_gim_data(gim(image_block(width=1, height=4097, raw=b"\0" * 4097), palette_block())))
        self.assertIsNone(decode_gim_data(gim(image_block(width=0, height=1, raw=b""))))
        self.assertIsNone(decode_gim_data(gim(image_block(width=1, height=0, raw=b""))))
        self.assertIsNone(decode_gim_data(gim(image_block(width=2, height=2, raw=b"x"))))

    def test_image_and_palette_offsets_stay_inside_their_blocks(self) -> None:
        self.assertIsNone(decode_gim_data(gim(image_block(data_end=0x1000))))

        reversed_image = bytearray(image_block())
        struct.pack_into("<I", reversed_image, 16 + 28, 40)
        struct.pack_into("<I", reversed_image, 16 + 32, 36)
        self.assertIsNone(decode_gim_data(gim(bytes(reversed_image))))

        bad_palette = bytearray(palette_block())
        struct.pack_into("<I", bad_palette, 16 + 28, 0x1000)
        self.assertIsNone(decode_gim_data(gim(image_block(), bytes(bad_palette))))

        reversed_palette = bytearray(palette_block())
        struct.pack_into("<I", reversed_palette, 16 + 28, 40)
        struct.pack_into("<I", reversed_palette, 16 + 32, 36)
        self.assertIsNone(decode_gim_data(gim(image_block(), bytes(reversed_palette))))

    def test_nested_block_depth_is_bounded(self) -> None:
        nested = image_block()
        for _ in range(40):
            nested = block(0x0002, nested)
        self.assertIsNone(decode_gim_data(gim(nested)))


if __name__ == "__main__":
    unittest.main()
