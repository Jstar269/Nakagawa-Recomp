# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Make a ``compile_commands.json`` relocatable (issue #368).

A compilation database written by a tool carries the absolute checkout path in
``directory``, ``file`` and every argument. This tool rewrites every path under
the checkout root to a root-relative form and keeps the single absolute field
(``directory``) pointing at the same folder of the checkout it is run for, so
moving or cloning a workspace needs only ``--root`` to be repointed, with no
other path left over. An empty or filesystem-root old root is refused
(``COMPDB_ROOT_INVALID``): relocating from it would rewrite system paths too.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


#: An option glued to the path it takes: a one-letter flag (``-I``, ``-o``, ``-L``), a
#: preprocessor or dependency option that takes a path (``-isystem``, ``-MF``), or any
#: ``-name=`` / ``--name=`` form (``--sysroot=``).
_GLUED_FLAG = re.compile(
    r"-(?:[A-Za-z]|isystem|iquote|idirafter|imacros|include|iprefix|iwithprefix|iwithprefixbefore"
    r"|isysroot|MF|MT|MQ)|--?[A-Za-z][\w+-]*=")
#: One shell word of a ``command`` string, quotes and all; the whitespace between is kept.
_COMMAND_WORD = re.compile(r"""(?:[^\s"']|"[^"]*"|'[^']*')+""")


class RelocateError(ValueError):
    """A root the database cannot be relocated from, named by a code."""


def _norm(text: str) -> str:
    return text.replace("\\", "/")


def _checked_root(root: str) -> str:
    """``root`` normalised; an empty, filesystem-root or drive-root value is refused."""
    old = _norm(root).rstrip("/")
    if not old or re.fullmatch(r"[A-Za-z]:", old):
        raise RelocateError(f"COMPDB_ROOT_INVALID: old root {root!r} is not a checkout folder; "
                            "relocating from it would rewrite every absolute path")
    return old


def _rel(value: str, old_root: str) -> str:
    """Return ``value`` with a leading ``old_root`` made root-relative."""
    norm = _norm(value)
    if norm == old_root:
        return "."
    prefix = old_root + "/"
    if norm.startswith(prefix):
        return norm[len(prefix):]
    # Flag forms such as -I/abs/path, -isystem/abs/path or --sysroot=/abs/path.
    index = norm.find(old_root, 1)
    while index > 0:
        rest = norm[index:]
        if (rest == old_root or rest.startswith(prefix)) and _GLUED_FLAG.fullmatch(norm[:index]):
            return norm[:index] + _rel(rest, old_root)
        index = norm.find(old_root, index + 1)
    return value


def _rel_word(word: str, old_root: str) -> str:
    """One word of a ``command`` string, quoted or not, made root-relative."""
    bare = word.replace('"', "").replace("'", "")
    relative = _rel(bare, old_root)
    if relative == bare:
        return word
    return f'"{relative}"' if any(char.isspace() for char in relative) else relative


def _directory(directory: str, old_root: str, new_root: str) -> str:
    """An entry's ``directory`` re-anchored under ``new_root``; one outside the checkout stays."""
    norm = _norm(directory).rstrip("/")
    if norm == old_root:
        return new_root
    if norm.startswith(old_root + "/"):
        return new_root + norm[len(old_root):]
    return directory


def default_old_root(entries: list[dict]) -> str:
    """The deepest folder every entry's ``directory`` is in, or ``""`` when none has one."""
    directories = [_norm(entry["directory"]).rstrip("/") for entry in entries
                   if isinstance(entry, dict) and isinstance(entry.get("directory"), str) and entry["directory"]]
    if not directories:
        return ""
    common = directories[0]
    for directory in directories[1:]:
        while common and directory != common and not directory.startswith(common + "/"):
            common = common.rpartition("/")[0]
    return common


def relocate(entries: list[dict], old_root: str, new_root: str) -> list[dict]:
    """Relativise paths under ``old_root`` and re-anchor each ``directory`` under ``new_root``.

    An entry recorded in a sub-folder of the checkout (``<root>/build`` with ``../src/a.c``)
    keeps that sub-folder, so its relative paths still resolve against the same folder.
    """
    old = _checked_root(old_root)
    new = _norm(new_root).rstrip("/")
    out = []
    for entry in entries:
        item = dict(entry)
        item["directory"] = _directory(item["directory"], old, new) if "directory" in item else new
        if "file" in item:
            item["file"] = _rel(item["file"], old)
        if "output" in item:
            item["output"] = _rel(item["output"], old)
        if "arguments" in item:
            item["arguments"] = [_rel(a, old) for a in item["arguments"]]
        if "command" in item:
            item["command"] = _COMMAND_WORD.sub(lambda word: _rel_word(word.group(0), old), item["command"])
        out.append(item)
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("database", type=Path)
    parser.add_argument("--old-root", help="checkout root recorded in the database "
                        "(default: the deepest folder holding every entry's directory)")
    parser.add_argument("--root", type=Path, default=Path.cwd(),
                        help="checkout root to record (default: current directory)")
    args = parser.parse_args(argv)
    entries = json.loads(args.database.read_text(encoding="utf-8"))
    if not isinstance(entries, list):
        print("compdb_relocate: database is not a JSON list", file=sys.stderr)
        return 1
    old_root = args.old_root or default_old_root(entries)
    try:
        result = relocate(entries, old_root, args.root.resolve().as_posix())
    except RelocateError as exc:
        print(f"compdb_relocate: {exc}", file=sys.stderr)
        return 1
    args.database.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
