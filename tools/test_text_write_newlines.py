#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

r"""A tool that emits a TRACKED text file must pin its line endings.

Python's text mode translates "\n" to "\r\n" on Windows unless the caller passes
``newline=""`` or ``newline="\n"``. This repository is developed on Windows, its
``.gitattributes`` declares ``eol=lf`` for source, and ``tools/publish_audit.py``
rejects CRLF in a tracked text file as ``TEXT_LINE_ENDING_CRLF``. A generator
that omits the argument therefore produces an artifact that cannot be
regenerated on a Windows host without the publication gate refusing it
afterwards -- which is what ``tools/title_catalog_codegen.py`` did to the
generated title catalog, silently, until someone regenerated it and then had to
work out why the provenance audit was complaining about a file they had not
meaningfully changed.

The scope here is deliberately narrow. Writes into a temporary directory are
invisible to the publication gate and spelling them any particular way is not
worth a rule, so the several hundred such writes across ``tools/test_*.py`` are
not flagged. What is checked is the small set of modules that write artifacts
git tracks. **Adding a generator means adding it to GENERATORS below** -- that
line is the whole maintenance cost of this lint, and it is cheaper than
rediscovering the failure from a CRLF finding in an audit log.
"""

from __future__ import annotations

import ast
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"

#: Modules that write files git tracks. The artifact each one owns is named so a
#: reader can confirm the entry still earns its place.
GENERATORS = {
    "title_catalog_codegen.py": "src/core/generated/nk_title_catalog.{c,h}",
    "policy_sync.py": "assets/public_source_profile.json and PUBLIC_EXPORT.json",
    "provenance_ledger.py": "assets/public_provenance_ledger.json",
}


#: The two newline= values that keep "\n" as written. None (universal newlines) is
#: the mode that writes os.linesep, so it is not a pin.
_PINNED_NEWLINES = ("", "\n")

#: A text-or-binary file mode, as opposed to a path or a name that happens to be the
#: same positional argument of another open() (zipfile's member name, say).
_MODE = re.compile(r"[rwxa+btU]{1,4}")

#: Calls that make a temporary file. Without dir= it lands in the system temporary
#: folder, which the publication gate never sees; with dir= it is staged beside a
#: tracked file and moved over it, so it must be pinned like any other write.
_TEMPFILE_FACTORIES = frozenset({"NamedTemporaryFile", "TemporaryFile"})


def _call_name(node: ast.Call) -> tuple[str | None, str | None]:
    """Return (owner, name): owner is the plain name a method is called on, if any."""
    if isinstance(node.func, ast.Name):
        return None, node.func.id
    if isinstance(node.func, ast.Attribute):
        owner = node.func.value.id if isinstance(node.func.value, ast.Name) else None
        return owner, node.func.attr
    return None, None


def _argument(node: ast.Call, keyword: str, position: int | None) -> ast.expr | None:
    for kw in node.keywords:
        if kw.arg == keyword:
            return kw.value
    if position is not None and len(node.args) > position:
        return node.args[position]
    return None


def _is_pinned(value: ast.expr | None) -> bool:
    return isinstance(value, ast.Constant) and value.value in _PINNED_NEWLINES


def _offending_write(node: ast.AST) -> str | None:
    """Describe `node` when it writes text without pinning newlines.

    Pinned means newline="" or newline="\\n" as a literal: newline=None and a
    computed value are both reported, because neither can be shown to stop the
    "\\n" to os.linesep translation.
    """
    if not isinstance(node, ast.Call):
        return None
    owner, name = _call_name(node)

    if name == "write_text" and isinstance(node.func, ast.Attribute):
        # Path.write_text(data, encoding=None, errors=None, newline=None)
        return None if _is_pinned(_argument(node, "newline", 3)) else "write_text() without newline="

    # (positional index of mode, positional index of newline, default mode)
    if name == "open" and owner in ("os", "codecs"):
        return None  # os.open takes flags and returns a descriptor; codecs.open never translates.
    if name == "open" and isinstance(node.func, ast.Attribute) and owner != "io":
        layout = (0, 4, "r")  # Path.open(mode='r', buffering=-1, encoding, errors, newline)
    elif name == "open":
        layout = (1, 6, "r")  # open(file, mode='r', buffering, encoding, errors, newline, closefd)
    elif name == "fdopen":
        layout = (1, 6, "r")  # os.fdopen(fd, mode='r', ...) has open()'s signature
    elif name in _TEMPFILE_FACTORIES:
        if _argument(node, "dir", None) is None:
            return None
        layout = (0, 3, "w+b")  # NamedTemporaryFile(mode='w+b', buffering, encoding, newline, ...)
    else:
        return None
    mode_position, newline_position, default_mode = layout

    mode_node = _argument(node, "mode", mode_position)
    if mode_node is None:
        mode: object = default_mode
    elif isinstance(mode_node, ast.Constant):
        mode = mode_node.value
    else:
        return None  # a computed mode cannot be judged here
    if not isinstance(mode, str) or not _MODE.fullmatch(mode) or "b" in mode:
        return None
    if not any(ch in mode for ch in "wxa+"):
        return None
    if _is_pinned(_argument(node, "newline", newline_position)):
        return None
    return f"{name}(..., {mode!r}) without newline=\"\" or newline=\"\\n\""


class TrackedArtifactNewlineTests(unittest.TestCase):
    def test_generators_pin_their_line_endings(self) -> None:
        offenders: list[str] = []
        for filename in sorted(GENERATORS):
            path = TOOLS / filename
            self.assertTrue(
                path.is_file(),
                f"{filename} is listed in GENERATORS but does not exist; remove the "
                f"entry or correct the name",
            )
            tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
            for node in ast.walk(tree):
                if not isinstance(node, ast.Call):
                    continue
                described = _offending_write(node)
                if described:
                    offenders.append(f"tools/{filename}:{node.lineno}: {described}")

        self.assertEqual(
            offenders,
            [],
            "These writes translate \\n to \\r\\n on Windows. The artifact is tracked, "
            "so tools/publish_audit.py rejects the result as TEXT_LINE_ENDING_CRLF and "
            "the failure appears only after someone regenerates on Windows. Pass "
            'newline="" to keep the emitted bytes exactly, or write bytes directly:\n  '
            + "\n  ".join(offenders),
        )

    def test_the_lint_flags_each_unpinned_form_and_accepts_each_pinned_one(self) -> None:
        """Each form a generator could use to write a tracked file, unpinned and pinned."""
        flagged = (
            'open(p, "w", encoding="utf-8")',
            'open(p, mode="a")',
            'open(p, "w", encoding="utf-8", newline=None)',
            'open(p, "w", newline=os.linesep)',
            'io.open(p, "w")',
            'path.write_text(text, encoding="utf-8")',
            'path.write_text(text, encoding="utf-8", newline=None)',
            'path.open("w", encoding="utf-8")',
            'args.policy.open(mode="x")',
            'path.open("r+")',
            'path.open("w", newline=None)',
            'os.fdopen(fd, "w", encoding="utf-8")',
            'tempfile.NamedTemporaryFile("w", dir=str(target.parent), delete=False)',
            'tempfile.NamedTemporaryFile(mode="w", dir=parent, newline=None)',
        )
        clean = (
            'open(p, "w", encoding="utf-8", newline="")',
            'open(p, "w", encoding="utf-8", newline="\\n")',
            'open(p, "wb")',
            'open(p)',
            'path.write_text(text, encoding="utf-8", newline="\\n")',
            'path.open("w", encoding="utf-8", newline="\\n")',
            'path.open("rb")',
            'path.open()',
            'archive.open("member.txt")',
            'os.open(p, os.O_WRONLY)',
            'codecs.open(p, "w", "utf-8")',
            'os.fdopen(fd, "wb")',
            'tempfile.NamedTemporaryFile(dir=parent, delete=False)',
            'tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)',
            'tempfile.NamedTemporaryFile(mode="w", dir=parent, newline="")',
        )
        for source in flagged:
            with self.subTest(flagged=source):
                self.assertIsNotNone(_offending_write(ast.parse(source, mode="eval").body))
        for source in clean:
            with self.subTest(clean=source):
                self.assertIsNone(_offending_write(ast.parse(source, mode="eval").body))

    def test_generator_list_covers_the_known_tracked_artifacts(self) -> None:
        """A generated artifact whose producer is unlisted is the gap this lint exists to close."""
        expected_producers = {
            "src/core/generated/nk_title_catalog.c": "title_catalog_codegen.py",
            "assets/public_provenance_ledger.json": "provenance_ledger.py",
            "PUBLIC_EXPORT.json": "policy_sync.py",
        }
        for artifact, producer in expected_producers.items():
            self.assertIn(
                producer, GENERATORS,
                f"{artifact} is a tracked generated artifact but its producer "
                f"{producer} is not covered by this lint",
            )
            self.assertTrue(
                (ROOT / artifact).is_file(),
                f"{artifact} is named here but is not present in the tree",
            )


if __name__ == "__main__":
    unittest.main()
