# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Offline tests for tools/ppm2png.py and tools/pngcmp.py (synthetic images only)."""

import contextlib
import io
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))

import pngcmp  # noqa: E402
import ppm2png  # noqa: E402

W, H = 4, 3


def make_ppm(pixels, w=W, h=H):
    return b"P6\n%d %d\n255\n" % (w, h) + pixels


def base_pixels():
    return bytes((x * 20 + y * 7 + c) & 0xFF for y in range(H) for x in range(W) for c in range(3))


def chunk(tag, payload):
    return (struct.pack(">I", len(payload)) + tag + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))


def make_png(color=2, depth=8, interlace=0, filt=0):
    px = base_pixels()
    raw = b"".join(bytes([filt]) + px[y * W * 3:(y + 1) * W * 3] for y in range(H))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, depth, color, 0, 0, interlace))
            + chunk(b"IDAT", zlib.compress(raw))
            + chunk(b"IEND", b""))


class PngCmpTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name)

    def convert(self, name, pixels, w=W, h=H):
        ppm = self.dir / (name + ".ppm")
        png = self.dir / (name + ".png")
        ppm.write_bytes(make_ppm(pixels, w, h))
        with patch.object(sys, "argv", ["ppm2png", str(ppm), str(png)]):
            ppm2png.main()
        return png

    def compare(self, a, b, *extra):
        out = io.StringIO()
        with patch.object(sys, "argv", ["pngcmp", str(a), str(b), *extra]), \
                contextlib.redirect_stdout(out):
            code = pngcmp.main()
        return code, out.getvalue()

    def test_round_trip_decodes_original_pixels(self):
        px = base_pixels()
        w, h, rows = pngcmp.decode(self.convert("a", px))
        self.assertEqual((w, h), (W, H))
        self.assertEqual(b"".join(rows), px)

    def test_a_truncated_or_foreign_ppm_is_refused_and_writes_no_png(self):
        px = base_pixels()
        cases = {
            "truncated": make_ppm(px[:-5]),
            "extra": make_ppm(px + b"\x00"),
            "p3": b"P3\n%d %d\n255\n" % (W, H) + px,
            "maxval": b"P6\n%d %d\n65535\n" % (W, H) + px,
        }
        for name, data in cases.items():
            with self.subTest(case=name):
                ppm = self.dir / (name + ".ppm")
                png = self.dir / (name + ".png")
                ppm.write_bytes(data)
                err = io.StringIO()
                with patch.object(sys, "argv", ["ppm2png", str(ppm), str(png)]), \
                        contextlib.redirect_stderr(err):
                    code = ppm2png.main()
                self.assertEqual(code, 1)
                self.assertIn("ppm2png:", err.getvalue())
                self.assertFalse(png.exists())

    def test_a_comment_in_the_ppm_header_still_converts(self):
        px = base_pixels()
        ppm = self.dir / "comment.ppm"
        png = self.dir / "comment.png"
        ppm.write_bytes(b"P6\n# a comment\n%d %d\n255\n" % (W, H) + px)
        with patch.object(sys, "argv", ["ppm2png", str(ppm), str(png)]):
            self.assertEqual(ppm2png.main(), 0)
        w, h, rows = pngcmp.decode(png)
        self.assertEqual(((w, h), b"".join(rows)), ((W, H), px))

    def test_identical_images_agree_fully(self):
        a = self.convert("a", base_pixels())
        b = self.convert("b", base_pixels())
        code, text = self.compare(a, b)
        self.assertEqual(code, 0)
        self.assertIn("pixels=12 identical=12 (100.00%)", text)
        self.assertIn("worst_channel_delta=0", text)

    def test_one_changed_pixel_is_reported(self):
        base = base_pixels()
        px = bytearray(base)
        px[0] = (base[0] + 50) & 0xFF
        a = self.convert("a", base)
        b = self.convert("b", bytes(px))
        _, text = self.compare(a, b)
        self.assertIn("identical=11 (91.67%)", text)
        self.assertIn("worst_channel_delta=%d" % abs(px[0] - base[0]), text)

    def test_tolerance_and_crop(self):
        base = base_pixels()
        px = bytearray(base)
        px[0] = base[0] + 2 if base[0] < 250 else base[0] - 2
        a = self.convert("a", base)
        b = self.convert("b", bytes(px))
        self.assertIn("identical=12", self.compare(a, b, "--tolerance", "2")[1])
        # The changed pixel is at x=0, outside this crop.
        self.assertIn("pixels=4 identical=4", self.compare(a, b, "--crop", "1,0,3,2")[1])

    def test_unsupported_pngs_are_rejected_loudly(self):
        cases = {
            "rgba": make_png(color=6),
            "depth16": make_png(depth=16),
            "interlaced": make_png(interlace=1),
            "filtered": make_png(filt=1),
            "not_png": b"GIF89a" + bytes(32),
        }
        for name, data in cases.items():
            with self.subTest(name):
                path = self.dir / (name + ".png")
                path.write_bytes(data)
                with self.assertRaises(SystemExit) as ctx:
                    pngcmp.decode(path)
                self.assertIn(str(path), str(ctx.exception))

    def test_image_data_that_does_not_match_the_header_is_rejected(self):
        def png(w, h, raw):
            return (b"\x89PNG\r\n\x1a\n"
                    + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                    + chunk(b"IDAT", zlib.compress(raw))
                    + chunk(b"IEND", b""))
        cases = {
            "short": png(2, 1, b"\x00\x01\x02\x03"),
            "short_row": png(2, 2, b"\x00" + bytes(6) + b"\x00" + bytes(4)),
            "long": png(1, 1, b"\x00" + bytes(3) + b"\x00"),
            "empty": png(0, 0, b""),
        }
        for name, data in cases.items():
            with self.subTest(name):
                path = self.dir / (name + ".png")
                path.write_bytes(data)
                with self.assertRaises(SystemExit) as ctx:
                    pngcmp.decode(path)
                self.assertIn(str(path), str(ctx.exception))

    def test_size_mismatch_and_empty_crop_exit(self):
        a = self.convert("a", base_pixels())
        b = self.convert("b", bytes(2 * 2 * 3), 2, 2)
        with self.assertRaises(SystemExit) as ctx:
            self.compare(a, b)
        self.assertIn("size mismatch", str(ctx.exception))
        with self.assertRaises(SystemExit) as ctx:
            self.compare(a, a, "--crop", "2,2,2,2")
        self.assertIn("empty crop", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
