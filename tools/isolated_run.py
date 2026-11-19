# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Run a soak or title-route command on a disposable copy of the Memory Stick and settings.

A long unattended run writes saves and settings. This wrapper hashes the real Memory Stick
tree (and any settings files), copies them into a throw-away folder, runs the given command
with ``SR_MEMSTICK`` (and optional settings variables) pointed at the copy, then hashes the
originals again. It prints ``ORIGINALS_UNCHANGED`` or fails naming the first path that
differs. It is a wrapper only: it never stops the guest from writing, and a write to the copy
is expected and kept for inspection.

    python tools/isolated_run.py --memstick MS --work-dir PRIVATE_DIR
        [--settings FILE --settings-env NAME] -- <command> [args]

``--work-dir`` must be an existing folder outside the originals; keep it under the private
per-title folder, because the copy can hold save data (AGENTS.md section 5).

Exit status: the command's own when the originals are unchanged; ``ORIGINAL_CHANGED_STATUS``
(121) when an original changed; ``SETUP_FAILED_STATUS`` (120) when the setup failed, the
command could not be started, or the wrapper could not read the originals again. Both
wrapper codes sit above the small codes tools use for their own results (argparse's 2, a
gate's 3 for no verdict), so a status-only caller can tell them apart; the printed
``ORIGINALS_UNCHANGED: command_status=N`` or ``ORIGINAL_CHANGED`` line says the same. A
malformed wrapper command line exits 2 from argparse before anything is copied or run.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SETUP_FAILED_STATUS = 120
ORIGINAL_CHANGED_STATUS = 121
_HASH_CHUNK = 1 << 20


def _file_sha256(path: Path) -> str:
    """The SHA-256 of one file, read in chunks so a multi-GiB ISO never sits in memory whole."""
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(_HASH_CHUNK):
            digest.update(chunk)
    return digest.hexdigest()


def snapshot(root: Path) -> dict[str, str]:
    """Map relative path -> sha256 (or ``<dir>``) for a file or tree; absent -> empty."""
    result: dict[str, str] = {}
    if root.is_file():
        result["."] = _file_sha256(root)
        return result
    if not root.is_dir():
        return result
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root).as_posix()
        if path.is_dir():
            result[rel] = "<dir>"
        else:
            result[rel] = _file_sha256(path)
    return result


def first_difference(before: dict[str, str], after: dict[str, str]) -> str | None:
    for key in sorted(set(before) | set(after)):
        if before.get(key) != after.get(key):
            return key
    return None


def _overlaps(a: Path, b: Path) -> bool:
    return a == b or a in b.parents or b in a.parents


def _setup_failed(message: str) -> int:
    print(f"ISOLATED_RUN: {message}", file=sys.stderr)
    return SETUP_FAILED_STATUS


def run(args: argparse.Namespace) -> int:
    memstick = Path(args.memstick).resolve()
    work_dir = Path(args.work_dir).resolve()
    settings = [Path(p).resolve() for p in args.settings]
    if not memstick.is_dir():
        return _setup_failed(f"memstick is not a directory: {memstick}")
    if not work_dir.is_dir():
        return _setup_failed(f"work dir must exist: {work_dir}")
    for original in [memstick, *settings]:
        if _overlaps(work_dir, original):
            return _setup_failed(f"work dir overlaps an original: {original}")
    if len(args.settings_env) != len(settings):
        return _setup_failed("give one --settings-env per --settings")
    if not args.command:
        return _setup_failed("no command after --")

    env = dict(os.environ)
    before: dict[str, dict[str, str]] = {}
    originals = {"memstick": memstick}
    copy_root: Path | None = None
    try:
        copy_root = Path(tempfile.mkdtemp(prefix="isolated-", dir=work_dir))
        before["memstick"] = snapshot(memstick)
        ms_copy = copy_root / "memstick"
        shutil.copytree(memstick, ms_copy)
        env["SR_MEMSTICK"] = str(ms_copy)
        for index, (path, name) in enumerate(zip(settings, args.settings_env, strict=True)):
            label = f"settings{index}"
            before[label] = snapshot(path)
            originals[label] = path
            target = copy_root / label / path.name
            target.parent.mkdir()
            if path.is_dir():
                shutil.copytree(path, target)
            elif path.exists():
                shutil.copy2(path, target)
            env[name] = str(target)
    except (OSError, shutil.Error) as exc:
        # The command never ran; a partial copy stays where it was made, for inspection.
        where = f" (partial copy={copy_root})" if copy_root is not None else ""
        return _setup_failed(f"setup failed{where}: {exc}")
    print(f"ISOLATED_RUN: copy={copy_root}")
    try:
        status: int | None = subprocess.run(args.command, env=env, check=False).returncode
    except OSError as exc:
        print(f"ISOLATED_RUN: the command could not be started: {exc}", file=sys.stderr)
        status = None

    # The originals are compared again even when the command never started.
    for label, original in originals.items():
        try:
            after = snapshot(original)
        except OSError as exc:
            return _setup_failed(f"cannot hash {label} {original} again: {exc}")
        diff = first_difference(before[label], after)
        if diff is not None:
            print(f"ORIGINAL_CHANGED: {label} {original} first differing path: {diff}")
            return ORIGINAL_CHANGED_STATUS
    if status is None:
        print("ORIGINALS_UNCHANGED: command_status=not-started")
        return SETUP_FAILED_STATUS
    print(f"ORIGINALS_UNCHANGED: command_status={status}")
    return status


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--memstick", required=True, help="the real Memory Stick folder")
    parser.add_argument("--work-dir", required=True, help="existing private folder for the copy")
    parser.add_argument("--settings", action="append", default=[],
                        help="a settings file or folder to copy (repeatable)")
    parser.add_argument("--settings-env", action="append", default=[],
                        help="variable that receives the copy's path, one per --settings")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    if args.command and args.command[0] == "--":
        args.command = args.command[1:]
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
