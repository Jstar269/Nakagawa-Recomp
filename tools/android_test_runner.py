#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Run the native CTest set on an Android device over adb (#360).

CMake uses this as ``CMAKE_CROSSCOMPILING_EMULATOR`` when the top-level project is configured with
the NDK toolchain and ``NK_ANDROID_TEST_DIR`` (docs/ANDROID_DEVELOPMENT.md, "Native tests on a
device"). Every test executable CTest starts is then pushed to the device and run there, and its
exit status is the test's result, so ``ctest`` reports the device's PASS, FAIL and SKIP (exit 77)
and nothing the host did on its behalf.

Two subcommands:

``sync``
    Copies the repository's tracked files (the same set ``git ls-files`` lists, read from the
    working tree) to ``<device dir>/src`` and creates ``<device dir>/work/build``. Tests whose
    CTest working directory is the source tree read their source-owned inputs from there. CTest
    runs it first, as the setup fixture of every test.

``run``
    Pushes one executable to ``<device dir>/bin`` and runs it. The host working directory CTest
    chose maps to the device: the binary tree to ``<device dir>/work``, the source tree to
    ``<device dir>/src``. Arguments and ``NK_*``/``SR_*`` environment values that name a path in
    either tree are mapped the same way. A working directory outside both trees is refused, so
    a test never runs somewhere its inputs are not.

adb comes from ``NK_ADB`` or ``PATH``; pick the device with adb's own ``ANDROID_SERIAL``. The
device directory must be under ``/data/local/tmp``, the shell user's scratch area: nothing else
on the device is written. The device needs USB debugging; the tool asks for no root and changes
no setting. This is test tooling only: it never runs in the player or the runtime.
"""

from __future__ import annotations

import argparse
import io
import os
from pathlib import Path, PurePosixPath
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile

DEVICE_SCRATCH_ROOT = PurePosixPath("/data/local/tmp")
ENV_PREFIXES = ("NK_", "SR_")
# Variables that configure this tool, not the test, stay on the host.
HOST_ONLY_ENV = {"NK_ADB"}


class RunnerError(Exception):
    """A refusal or adb failure; the message names what to fix."""


def adb_command() -> list[str]:
    adb = os.environ.get("NK_ADB") or shutil.which("adb")
    if not adb:
        raise RunnerError("adb not found: put platform-tools on PATH or set NK_ADB")
    return [adb]


def checked_device_dir(value: str) -> PurePosixPath:
    path = PurePosixPath(value)
    if (not path.is_absolute() or ".." in path.parts
            or DEVICE_SCRATCH_ROOT not in path.parents):
        raise RunnerError(f"device dir must be a folder under {DEVICE_SCRATCH_ROOT}: {value}")
    return path


def _norm(path: str | os.PathLike[str]) -> str:
    return os.path.normcase(os.path.abspath(os.fspath(path)))


class PathMap:
    """Maps host paths in the binary and source trees to their device copies.

    The binary tree is checked first because a build directory may sit inside the source tree.
    """

    def __init__(self, device_dir: PurePosixPath, source_root: str, binary_root: str) -> None:
        self.roots = [
            (_norm(binary_root), device_dir / "work"),
            (_norm(source_root), device_dir / "src"),
        ]

    def map(self, host_path: str) -> PurePosixPath | None:
        candidate = _norm(host_path)
        for root, device_root in self.roots:
            if candidate == root:
                return device_root
            prefix = root.rstrip("\\/") + os.sep
            if candidate.startswith(prefix):
                rel = candidate[len(prefix):].replace(os.sep, "/")
                return device_root / rel
        return None

    def map_value(self, value: str) -> str:
        """Maps a value that is an absolute host path in a tree; anything else is unchanged."""
        if not value or not os.path.isabs(value):
            return value
        mapped = self.map(value)
        return str(mapped) if mapped is not None else value


def adb_shell(adb: list[str], script: str) -> int:
    return subprocess.run([*adb, "shell", script], check=False).returncode


def adb_checked(adb: list[str], *args: str) -> None:
    proc = subprocess.run([*adb, *args], check=False, capture_output=True, text=True)
    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout).strip()
        raise RunnerError(f"adb {' '.join(args[:2])} failed ({proc.returncode}): {detail}")


def tracked_files(source_root: Path) -> list[str]:
    proc = subprocess.run(["git", "-C", str(source_root), "ls-files", "-z"], check=False,
                          capture_output=True)
    if proc.returncode != 0:
        raise RunnerError("git ls-files failed: sync needs the repository's git checkout")
    return [name for name in proc.stdout.decode("utf-8").split("\0") if name]


def build_source_tar(source_root: Path, names: list[str]) -> bytes:
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w", format=tarfile.PAX_FORMAT) as tar:
        for name in names:
            path = source_root / name
            # A tracked file deleted in the working tree is absent from the copy, as on the host.
            if path.is_file():
                tar.add(path, arcname=name, recursive=False)
    return buffer.getvalue()


def cmd_sync(args: argparse.Namespace) -> int:
    device_dir = checked_device_dir(args.device_dir)
    source_root = Path(args.source_root)
    adb = adb_command()
    names = tracked_files(source_root)
    payload = build_source_tar(source_root, names)
    remote_tar = device_dir / "src.tar"
    src = device_dir / "src"
    adb_checked(adb, "shell", f"mkdir -p {shlex.quote(str(device_dir))}")
    with tempfile.TemporaryDirectory() as tmp:
        local_tar = Path(tmp) / "src.tar"
        local_tar.write_bytes(payload)
        adb_checked(adb, "push", str(local_tar), str(remote_tar))
    script = " && ".join([
        f"rm -rf {shlex.quote(str(src))}",
        f"mkdir -p {shlex.quote(str(src))} {shlex.quote(str(device_dir / 'work' / 'build'))}"
        f" {shlex.quote(str(device_dir / 'bin'))}",
        f"tar -xf {shlex.quote(str(remote_tar))} -C {shlex.quote(str(src))}",
        f"rm -f {shlex.quote(str(remote_tar))}",
    ])
    rc = adb_shell(adb, script)
    if rc != 0:
        raise RunnerError(f"unpacking the source copy on the device failed ({rc})")
    print(f"android-test-runner: synced {len(names)} tracked files to {src}")
    return 0


def device_environment(path_map: PathMap, environ: dict[str, str]) -> list[str]:
    assignments = []
    for key in sorted(environ):
        if key in HOST_ONLY_ENV or not key.startswith(ENV_PREFIXES):
            continue
        assignments.append(f"{key}={shlex.quote(path_map.map_value(environ[key]))}")
    return assignments


def remote_run_script(device_dir: PurePosixPath, path_map: PathMap, cwd: str, exe_name: str,
                      argv: list[str], environ: dict[str, str]) -> str:
    device_cwd = path_map.map(cwd)
    if device_cwd is None:
        raise RunnerError(f"working directory {cwd} is outside the source and binary trees")
    exe = device_dir / "bin" / exe_name
    command = [str(exe), *(path_map.map_value(arg) for arg in argv)]
    env = device_environment(path_map, environ)
    run = " ".join([*(["env", *env] if env else []), *map(shlex.quote, command)])
    return (f"mkdir -p {shlex.quote(str(device_cwd))} && cd {shlex.quote(str(device_cwd))}"
            f" && {run}")


def cmd_run(args: argparse.Namespace) -> int:
    device_dir = checked_device_dir(args.device_dir)
    if not args.command:
        raise RunnerError("run needs an executable")
    host_exe = Path(args.command[0])
    if not host_exe.is_file():
        raise RunnerError(f"no such executable: {host_exe}")
    path_map = PathMap(device_dir, args.source_root, args.binary_root)
    adb = adb_command()
    remote_exe = device_dir / "bin" / host_exe.name
    adb_checked(adb, "push", str(host_exe), str(remote_exe))
    adb_checked(adb, "shell", f"chmod 755 {shlex.quote(str(remote_exe))}")
    script = remote_run_script(device_dir, path_map, os.getcwd(), host_exe.name, args.command[1:],
                               dict(os.environ))
    sys.stdout.flush()
    return adb_shell(adb, script)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    sync = sub.add_parser("sync", help="copy the tracked source tree to the device")
    sync.add_argument("--device-dir", required=True)
    sync.add_argument("--source-root", required=True)
    run = sub.add_parser("run", help="push and run one test executable on the device")
    run.add_argument("--device-dir", required=True)
    run.add_argument("--source-root", required=True)
    run.add_argument("--binary-root", required=True)
    run.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    if args.cmd == "run" and args.command[:1] == ["--"]:
        args.command = args.command[1:]
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        return cmd_sync(args) if args.cmd == "sync" else cmd_run(args)
    except RunnerError as exc:
        print(f"android-test-runner: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
