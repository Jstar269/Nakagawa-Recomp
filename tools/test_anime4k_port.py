# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for tools/anime4k_port.py, the wrapper that turns Anime4K's mpv shader into compute shaders."""

from __future__ import annotations

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import anime4k_port  # noqa: E402
import shader_embed  # noqa: E402

SOURCE = anime4k_port.UPSTREAM / "Anime4K_Upscale_CNN_x2_M.glsl"


class Anime4kPortTests(unittest.TestCase):
    def test_generated_files_are_current(self) -> None:
        for path, text in anime4k_port.outputs().items():
            with self.subTest(path=path.name):
                self.assertEqual(path.read_text(encoding="utf-8"), text)

    def test_every_pass_body_is_kept_byte_for_byte(self) -> None:
        notice, passes = anime4k_port.parse(SOURCE.read_text(encoding="utf-8"))
        self.assertEqual(len(passes), 9)
        for k, p in enumerate(passes):
            shader = (anime4k_port.SHADER_DIR / f"anime4k_cnn_x2_m_{k}.comp").read_text(encoding="utf-8")
            with self.subTest(k=k):
                self.assertIn("\n".join(p.body), shader)
                self.assertTrue(shader.startswith("#version 450\n// SPDX-License-Identifier: MIT\n"))
                self.assertIn("Copyright (c) 2019-2021 bloc97", shader)
                self.assertIn("Modified for Nakagawa Recomp", shader)

    def test_pass_graph(self) -> None:
        _notice, passes = anime4k_port.parse(SOURCE.read_text(encoding="utf-8"))
        self.assertEqual([p.scale for p in passes], [1] * 8 + [2])
        self.assertEqual(passes[7].binds, [p.save for p in passes[:7]])
        self.assertEqual(passes[8].binds[0], "MAIN")
        self.assertLessEqual(max(len(p.binds) for p in passes), anime4k_port.MAX_BINDS)

    def test_shader_embed_lists_every_pass(self) -> None:
        listed = {(src.as_posix(), inc.as_posix()) for _kind, src, inc in shader_embed.SHADERS}
        for pair in anime4k_port.shader_list():
            with self.subTest(pair=pair):
                self.assertIn(pair, listed)

    def test_unsupported_size_expression_is_refused(self) -> None:
        text = SOURCE.read_text(encoding="utf-8").replace("//!WIDTH conv2d_last_tf.w 2 *", "//!WIDTH conv2d_last_tf.w 3 *", 1)
        self.assertNotEqual(text, SOURCE.read_text(encoding="utf-8"))
        with self.assertRaises(ValueError):
            anime4k_port.parse(text)


if __name__ == "__main__":
    unittest.main()
