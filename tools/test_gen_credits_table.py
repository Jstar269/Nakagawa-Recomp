#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The launcher's Love All table, ``src/player/ui/love_all_credits.c``, is CREDITS.md, current."""

from __future__ import annotations

from pathlib import Path
import re
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_credits_table  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


class GenCreditsTableTests(unittest.TestCase):
    def test_committed_table_is_current(self) -> None:
        committed = (ROOT / gen_credits_table.OUTPUT).read_bytes()
        self.assertEqual(
            committed,
            gen_credits_table.generate().encode("utf-8"),
            "src/player/ui/love_all_credits.c is stale; run python tools/regen_all.py",
        )

    def test_every_section_and_credited_name_of_credits_md_is_in_the_table(self) -> None:
        markdown = (ROOT / gen_credits_table.SOURCE).read_text(encoding="utf-8")
        rows = gen_credits_table.parse(markdown)
        headings = [name for kind, name, _ in rows if kind == gen_credits_table.HEADING]
        entries = [name for kind, name, _ in rows if kind == gen_credits_table.ENTRY]
        expected_headings = [gen_credits_table.plain(line[3:])
                             for line in markdown.splitlines() if line.startswith("## ")]
        self.assertEqual(headings, expected_headings)
        self.assertGreaterEqual(len(entries), 20)
        for line in markdown.splitlines():
            match = re.match(r"- \*\*(.+?)\*\*", line)
            if match:
                name = gen_credits_table.plain(match.group(1)).split(gen_credits_table.EM_DASH)[0]
                self.assertIn(name.strip(), entries, line)
        self.assertEqual(rows[-1][0], gen_credits_table.DEDICATION)

    def test_parse_reads_names_people_and_the_dedication(self) -> None:
        sample = "\n".join([
            "# Credits",
            "",
            "This project is dedicated to everyone who plays.",
            "",
            "## Upstreams",
            "",
            "- **Alpha — the Alpha authors** ([alpha](https://example.invalid/alpha)): a",
            "  continued line. **Beta** is credited here too.",
            "- **Gamma** — Ada Lovelace — the engine it uses.",
            "- **Delta** — " + "x" * 90 + " — a description.",
            "- **Epsilon**: no one named.",
            "",
            "## Fonts",
            "",
            "- **Zeta Sans — Zeta Studio**",
        ])
        self.assertEqual(gen_credits_table.parse(sample), [
            ("LOVE_ALL_HEADING", "Upstreams", None),
            ("LOVE_ALL_ENTRY", "Alpha", "the Alpha authors"),
            ("LOVE_ALL_ENTRY", "Beta", None),
            ("LOVE_ALL_ENTRY", "Gamma", "Ada Lovelace"),
            ("LOVE_ALL_ENTRY", "Delta", None),
            ("LOVE_ALL_ENTRY", "Epsilon", None),
            ("LOVE_ALL_HEADING", "Fonts", None),
            ("LOVE_ALL_ENTRY", "Zeta Sans", "Zeta Studio"),
            ("LOVE_ALL_DEDICATION", "Dedicated to everyone who plays", None),
        ])

    def test_render_is_ascii_c_with_escapes(self) -> None:
        text = gen_credits_table.render([("LOVE_ALL_ENTRY", 'Rydgård "q" \\', None)])
        text.encode("ascii")
        self.assertIn(r'{LOVE_ALL_ENTRY, "Rydg\303\245rd \"q\" \\",', text)
        self.assertIn("     NULL},", text)
        self.assertIn("const size_t g_love_all_credit_count =", text)

    def test_check_mode(self) -> None:
        self.assertEqual(gen_credits_table.main(["--check"]), 0)
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "x.c"
            self.assertEqual(gen_credits_table.main(["--out", str(out), "--check"]), 1)
            out.write_bytes(b"stale\n")
            self.assertEqual(gen_credits_table.main(["--out", str(out), "--check"]), 1)
            self.assertEqual(gen_credits_table.main(["--out", str(out)]), 0)
            self.assertEqual(gen_credits_table.main(["--out", str(out), "--check"]), 0)


if __name__ == "__main__":
    unittest.main()
