# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors, 2026-11-18.
# See NOTICE.md for upstream lineage and modification provenance.

"""Convert a framebuffer PPM snapshot to PNG.

Usage: ``python tools/ppm2png.py <input.ppm> <output.png>``.  Reads the PPM written
by ``SR_FBSNAP=<N>`` and writes a PNG for the diff tools and the UI baseline.
"""

import struct
import sys
import zlib

from picture_gate import PictureError, parse_ppm


def main():
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    # The shared P6 reader checks the magic, the maxval and the exact pixel count, so a truncated or
    # foreign file stops here by name instead of becoming a PNG with missing pixels.
    try:
        frame = parse_ppm(data, sys.argv[1])
    except PictureError as exc:
        print(f"ppm2png: {exc}", file=sys.stderr)
        return 1
    w, h, px = frame.width, frame.height, frame.pixels
    raw = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    with open(sys.argv[2], "wb") as f:
        f.write(png)
    return 0


if __name__ == "__main__":
    sys.exit(main())
