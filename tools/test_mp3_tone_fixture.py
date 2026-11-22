# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The committed sceMp3 tone fixture header is exactly what its generator writes.

tests/fixtures/mp3_tone/mp3_tone.h feeds the sceMp3 HLE tests in tests/native/hle_thread_selftest.c.
It is generated, not hand-edited, and it must stay source-authored: frames built from the
MPEG-1 Layer III syntax by tests/fixtures/mp3_tone/generate.py, with no encoder output or recorded
audio in the repository.
"""

from __future__ import annotations

import importlib.util
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "tests" / "fixtures" / "mp3_tone" / "generate.py"
HEADER = ROOT / "tests" / "fixtures" / "mp3_tone" / "mp3_tone.h"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")


def _load_generator():
    spec = importlib.util.spec_from_file_location("mp3_tone_generate", GENERATOR)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def _array(text: str, name: str) -> bytes:
    match = re.search(rf"{name}\[(\d+)\] = \{{(.*?)\}};", text, re.S)
    assert match is not None, name
    values = bytes(int(v, 16) for v in re.findall(r"0x([0-9a-f]{2})", match.group(2)))
    assert len(values) == int(match.group(1)), name
    return values


class Mp3ToneFixtureTests(unittest.TestCase):
    def setUp(self) -> None:
        self.gen = _load_generator()
        self.text = HEADER.read_text(encoding="utf-8")

    def test_header_is_fresh(self) -> None:
        self.assertEqual(self.text, self.gen.render(), "run python tests/fixtures/mp3_tone/generate.py")
        self.assertEqual(self.gen.main(["--check"]), 0)

    def test_frames_are_mpeg1_layer3_at_the_declared_size(self) -> None:
        for name, mode in (("k_mp3_tone_stereo_frame", 0x00), ("k_mp3_tone_mono_frame", 0xC0)):
            frame = _array(self.text, name)
            self.assertEqual(len(frame), self.gen.FRAME_BYTES)
            # Sync, MPEG-1, Layer III, no CRC; 32 kbps at 44.1 kHz, no padding; channel mode.
            self.assertEqual(frame[:4], bytes([0xFF, 0xFB, 0x10, mode]), name)
            # main_data_begin is 0: no frame leans on another's bit reservoir.
            self.assertEqual(frame[4] << 1 | frame[5] >> 7, 0, name)

    def test_count1_codes_place_one_line_per_channel(self) -> None:
        bits = self.gen.count1_line(26).bits
        # Six all-zero quadruples (table B code 1111), then 0010 coded as 1101, then a + sign.
        self.assertEqual(bits, [1, 1, 1, 1] * 6 + [1, 1, 0, 1] + [0])
        self.assertEqual(len(self.gen.count1_line(52).bits), 13 * 4 + 4 + 1)

    def test_id3_prefix_is_an_empty_tag(self) -> None:
        self.assertEqual(_array(self.text, "k_mp3_tone_id3_prefix"), b"ID3\x03" + bytes(6))


# Decodes one frame through the sceMp3 bridge (mp3_bridge.c and the vendored minimp3) after
# filling the stack below main with a byte pattern, once per pattern. It prints whether every
# decode gave the same PCM, and whether every decode left the same synthesis state
# (mp3dec_t.qmf_state, which the next frame of the stream starts from). Only uninitialized
# stack reads in the decoder could make either differ.
STACK_HARNESS_C = r"""#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mp3_bridge.h"
#include "minimp3/minimp3.h"

#if defined(__GNUC__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

static NOINLINE void fill_stack(unsigned char value) {
    volatile unsigned char area[128 * 1024];
    for (size_t i = 0; i < sizeof(area); i++) area[i] = value;
}

static NOINLINE int decode_once(const unsigned char *frame, int len, short *pcm,
                                float *qmf_state) {
    Mp3Bridge *b = NULL;
    int consumed = 0, samples = 0;
    if (mp3_bridge_create(&b) != 0) return -1;
    int rc = mp3_bridge_decode(b, frame, len, pcm, &consumed, &samples, NULL);
    /* mp3_bridge.c's struct Mp3Bridge starts with the decoder's mp3dec_t. */
    memcpy(qmf_state, ((const mp3dec_t *)(const void *)b)->qmf_state,
           sizeof(((mp3dec_t *)0)->qmf_state));
    mp3_bridge_destroy(b);
    return rc == MP3_BRIDGE_OK ? samples : -1;
}

int main(int argc, char **argv) {
    static unsigned char frame[4096];
    static short pcm[3][MP3_BRIDGE_MAX_PCM_VALUES];
    static float qmf[3][15 * 2 * 32];
    static const unsigned char patterns[3] = {0x00u, 0x3cu, 0xa5u};
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    int len = (int)fread(frame, 1, sizeof(frame), f);
    fclose(f);
    int samples[3];
    for (int p = 0; p < 3; p++) {
        memset(pcm[p], 0, sizeof(pcm[p]));
        fill_stack(patterns[p]);
        samples[p] = decode_once(frame, len, pcm[p], qmf[p]);
        if (samples[p] <= 0) { printf("decode failed: %d\n", samples[p]); return 3; }
    }
    long peak = 0;
    for (int i = 0; i < samples[0] * 2; i++) {
        long v = pcm[0][i] < 0 ? -(long)pcm[0][i] : pcm[0][i];
        if (v > peak) peak = v;
    }
    int pcm_same = 1, state_same = 1;
    for (int p = 1; p < 3; p++) {
        if (pcm_same && (samples[p] != samples[0] || memcmp(pcm[p], pcm[0], sizeof(pcm[0])) != 0)) {
            int at = 0;
            while (at < MP3_BRIDGE_MAX_PCM_VALUES && pcm[p][at] == pcm[0][at]) at++;
            printf("pcm differs with stack pattern 0x%02x at value %d\n", patterns[p], at);
            pcm_same = 0;
        }
        if (state_same && memcmp(qmf[p], qmf[0], sizeof(qmf[0])) != 0) {
            int at = 0;
            while (at < 15 * 2 * 32 && memcmp(&qmf[p][at], &qmf[0][at], sizeof(float)) == 0) at++;
            printf("qmf_state differs with stack pattern 0x%02x at float %d\n", patterns[p], at);
            state_same = 0;
        }
    }
    if (pcm_same) printf("pcm identical, peak %ld\n", peak);
    if (state_same) printf("qmf_state identical\n");
    return 0;
}
"""


@unittest.skipUnless(CC, "TOOLCHAIN_MISSING: no C compiler on PATH")
class Mp3PrivateBitsDecodeTests(unittest.TestCase):
    """The decoder's output and carried state never depend on what the host stack held.

    An MPEG-1 stereo frame's private bits land in granule 0's right-channel scfsi. minimp3
    then reuses that channel's scalefactors from scratch it never wrote in this frame
    (upstream lieff/minimp3 a01e51e zeroes it; src/rt/minimp3/PROVENANCE.md, LOCAL DELTA), so
    the right channel's PCM followed whatever the host stack held.

    Every frame's synthesis also copied workspace lanes it never wrote into qmf_state, the
    state the stream's next frame starts from (upstream 19ff674 zeroes the workspace).
    """

    tmp: ClassVar[Path]
    frame_path: ClassVar[Path]
    plain_path: ClassVar[Path]
    exe: ClassVar[Path]

    @classmethod
    def setUpClass(cls) -> None:
        assert CC is not None
        cls.tmp = Path(tempfile.mkdtemp(prefix="mp3stack_"))
        plain = _load_generator().frame(mono=False)
        frame = bytearray(plain)
        # The three private bits follow main_data_begin's nine: byte 5, mask 0x70.
        frame[5] |= 0x70
        cls.frame_path = cls.tmp / "private_bits.mp3"
        cls.frame_path.write_bytes(bytes(frame))
        cls.plain_path = cls.tmp / "plain.mp3"
        cls.plain_path.write_bytes(bytes(plain))
        source = cls.tmp / "mp3_stack_harness.c"
        source.write_text(STACK_HARNESS_C, encoding="utf-8")
        cls.exe = cls.tmp / "mp3_stack_harness.exe"
        result = subprocess.run(
            [CC, "-std=c11", "-O0", "-Wall", "-Wextra", f"-I{ROOT / 'src' / 'rt'}", "-o",
             str(cls.exe), str(source), str(ROOT / "src" / "rt" / "mp3_bridge.c"), "-lm"],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            raise AssertionError("mp3 stack harness did not compile:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def _run(self, frame: Path) -> str:
        result: subprocess.CompletedProcess[str] = subprocess.run(
            [str(self.exe), str(frame)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_private_bits_frame_decodes_the_same_whatever_the_stack_held(self) -> None:
        out = self._run(self.frame_path)
        match = re.search(r"pcm identical, peak (\d+)", out)
        if match is None:
            self.fail(f"no 'pcm identical' line in the harness output:\n{out}")
        # The frame carries the fixture's tones, so the comparison is over real audio.
        self.assertGreater(int(match.group(1)), 1000, out)

    def test_synthesis_state_after_a_frame_is_the_same_whatever_the_stack_held(self) -> None:
        for frame in (self.plain_path, self.frame_path):
            with self.subTest(frame=frame.name):
                out = self._run(frame)
                self.assertIn("qmf_state identical", out)
                self.assertIn("pcm identical", out)


if __name__ == "__main__":
    unittest.main()
