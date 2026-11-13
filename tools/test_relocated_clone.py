#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Contributor quick check in a checkout whose own path contains spaces (#368).

An outside contributor clones the public repository wherever their tooling puts
it, and a checkout under a user-profile directory whose name contains spaces is
not an exotic location. This module
copies the TRACKED tree -- the bytes on disk, including uncommitted work, so the
check exercises the change under review and not the last commit -- into a
temporary directory whose path contains spaces, and runs a bounded contributor
command set there.

Bounded on purpose: ``make check`` and ``make readiness`` remain the
authoritative gates and ``mingw32-make native-core-tests`` covers the native
build. This is a relocation smoke, so it runs the Makefile parse, one hermetic C
selftest that compiles and executes, and the Python tooling tests whose fixtures
touch the build system. Every subprocess carries an explicit timeout, and the
module is skipped -- with the reason printed -- only when a tool it needs is
absent from this host.

Why it is not ``git archive``: that reads committed blobs, so an uncommitted fix
would be invisible to the very test meant to pin it.
"""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from typing import ClassVar


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

#: The directory name embeds spaces on purpose; the assertion below proves the
#: harness still tested a spaced path if a host overrides TEMP.
SPACED_DIR_NAME = "nakagawa relocated clone"

#: Per-command wall-clock budget. Generous for a cold C compile on a loaded
#: runner, far below the hosted shard timeout.
COMMAND_TIMEOUT_S = 900


def tracked_paths(root: Path) -> list[str]:
    """Tracked paths, read from Git with NUL separation so no name is split."""
    proc = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"],
        capture_output=True,
        check=True,
        timeout=120,
    )
    return [
        item.decode("utf-8", errors="surrogateescape")
        for item in proc.stdout.split(b"\0")
        if item
    ]


def copy_tracked_tree(source: Path, destination: Path) -> int:
    """Copy every tracked file that still exists. Returns the number copied.

    A path deleted in the working tree is skipped rather than failing: the
    question this test asks is whether the surviving tree builds and checks from
    an arbitrary path, and a staged deletion is an unrelated concern.
    """
    copied = 0
    for relative in tracked_paths(source):
        origin = source / relative
        if not origin.is_file():
            continue
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(origin, target)
        copied += 1
    return copied


class RelocatedCloneTests(unittest.TestCase):
    """The documented contributor commands, run from a path with spaces."""
    copied: int

    clone: Path
    make: str
    failures: list[str]

    @classmethod
    def setUpClass(cls) -> None:
        make = shutil.which("mingw32-make") or shutil.which("make")
        if not make:
            reason = "GNU Make is required for the relocated-contributor check"
            print(f"SKIP {__name__}: {reason} (no mingw32-make or make on PATH)")
            raise unittest.SkipTest(reason)
        if not (shutil.which("git") or (ROOT / ".git").exists()):
            reason = "Git is required to enumerate the tracked tree"
            print(f"SKIP {__name__}: {reason}")
            raise unittest.SkipTest(reason)
        if not (os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc")):
            reason = "a C compiler is required to build the relocated selftest"
            print(f"SKIP {__name__}: {reason} (no gcc or cc on PATH)")
            raise unittest.SkipTest(reason)

        cls.make = make
        base = Path(tempfile.mkdtemp(prefix=f"{SPACED_DIR_NAME} "))
        cls.clone = base / "nakagawa"
        cls.clone.mkdir()
        cls.copied = copy_tracked_tree(ROOT, cls.clone)
        cls.failures = []
        if " " not in str(cls.clone):
            # Unreachable on a sane host, but a harness that silently lost its
            # space would report a meaningless pass.
            raise AssertionError(f"relocated clone path carries no space: {cls.clone}")

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(getattr(cls, "clone", Path()).parent, ignore_errors=True)

    def run_step(self, label: str, command: list[str]) -> None:
        proc = subprocess.run(
            command,
            cwd=self.clone,
            capture_output=True,
            text=True,
            check=False,
            timeout=COMMAND_TIMEOUT_S,
        )
        if proc.returncode == 0:
            return
        self.failures.append(
            f"--- {label} ({proc.returncode})\n"
            f"  command: {' '.join(command)}\n"
            f"  stdout: {proc.stdout[-2000:]}\n"
            f"  stderr: {proc.stderr[-2000:]}"
        )

    def test_the_relocated_path_really_contains_a_space(self) -> None:
        self.assertIn(" ", str(self.clone))
        self.assertGreater(self.copied, 100, "the tracked tree was not copied")
        self.assertTrue((self.clone / "Makefile").is_file())
        self.assertTrue((self.clone / "tools" / "contrib_check.py").is_file())

    def test_build_root_naming_the_spaced_parent_is_refused_as_an_ancestor(self) -> None:
        """From a spaced checkout, BUILD_ROOT=<its parent> names the checkout, not the space.

        The clone's parent directory contains a space, so it trips both of the
        Makefile's BUILD_ROOT refusals. The checkout-scope refusal must win: it is
        the one that says clean-all would reach the checkout. `help` is
        information-only, so the probe creates and deletes nothing even if the
        refusal were lost.
        """
        parent = self.clone.parent.as_posix()
        self.assertIn(" ", parent)
        proc = subprocess.run(
            [self.make, "--no-print-directory", "help", f"BUILD_ROOT={parent}"],
            cwd=self.clone,
            capture_output=True,
            text=True,
            check=False,
            timeout=COMMAND_TIMEOUT_S,
        )
        self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("the repository root or one of its ancestors", proc.stderr)
        self.assertNotIn("contains a space", proc.stderr)

    def test_relocated_contributor_quick_check(self) -> None:
        """Makefile parse, one hermetic C selftest, and the path-sensitive tests."""
        self.run_step("make help", [self.make, "--no-print-directory", "help"])
        self.run_step(
            "strbuf-selftest",
            [self.make, "--no-print-directory", "strbuf-selftest"],
        )
        self.run_step(
            "documentation lint",
            [sys.executable, "tools/lint_docs.py"],
        )
        self.run_step(
            "build-truth tooling tests",
            [
                sys.executable, "-m", "unittest",
                "tools.test_build_truth.BuildTruthTests",
                "tools.test_build_truth.BuildArtifactLifecycleTests",
            ],
        )
        # A whitespace-bearing BUILD_DIR must never reach a Make recipe: it used to
        # be split into several targets, which left a spaces/ tree in the checkout
        # root and made the publication audit reject the litter.
        self.assertFalse(
            (self.clone / "spaces").exists(),
            "the relocated run created a spaces/ tree in the checkout root",
        )
        # The step labels go last on purpose: a caller that keeps only the tail of
        # a failure (mingw32-make contrib-check prints the final lines) still sees
        # which step failed.
        self.assertEqual(
            self.failures, [],
            "\n\n".join(self.failures)
            + f"\n\nfailed steps: {len(self.failures)} in {self.clone}",
        )


#: The route both checkouts build at once: a generated title-config header (Python), a
#: runtime capture written by a C program, Python translation of that capture into C, and
#: two links against runtime sources. Generated and runtime assets, all from one checkout.
ISOLATION_GOAL = "code-capture-selftest"
#: The source both binaries compile, which each checkout marks with its own name.
ISOLATION_MARKED_SOURCE = Path("tests/native/code_capture_selftest.c")
ISOLATION_BINARIES = ("code_capture_selftest_capture.exe", "code_capture_selftest_bound.exe")


def _marker(name: str) -> bytes:
    return f"nk-isolation-marker-{name}".encode("ascii")


def _snapshot(root: Path, skip: tuple[str, ...] = ()) -> dict[str, tuple[int, int]]:
    """Every file under ``root``, except in its top-level folders named in ``skip``:
    relative path -> (size, mtime_ns)."""
    found: dict[str, tuple[int, int]] = {}
    for directory, folders, files in os.walk(root):
        here = Path(directory)
        if here == root:
            folders[:] = [folder for folder in folders if folder not in skip]
        for name in files:
            path = here / name
            stat = path.stat()
            found[path.relative_to(root).as_posix()] = (stat.st_size, stat.st_mtime_ns)
    return found


def _stop(run: subprocess.Popen[bytes]) -> None:
    if run.poll() is None:
        run.kill()
        run.wait()


def _changed(before: dict[str, tuple[int, int]], after: dict[str, tuple[int, int]]) -> list[str]:
    return sorted(path for path in before.keys() | after.keys() if before.get(path) != after.get(path))


class TwoCheckoutIsolationTests(unittest.TestCase):
    """Two checkouts building at once use only their own generated and runtime assets (#368).

    Two copies of the tracked tree, under a parent whose name has a space, each with a
    different title name (so a different BUILD_DIR) and its own marker compiled into the
    route's binaries, build ``code-capture-selftest`` at the same time with no BUILD_ROOT
    set, as a developer with two worktrees would. Each build logs to a file, so a full
    pipe cannot make one wait for the other. Then:

    - nothing in the parent the two share changed, apart from those logs;
    - each checkout built the route's binaries in its own build folder, and they carry
      its own marker and not the other's;
    - no file one checkout built names the other checkout's folder, in any case (which
      also catches a relative, MSYS or absolute form of its path).

    The marker proves only the marked source's origin: the generated header, the capture
    and the translated unit are identical in both checkouts, so a build that used the
    other checkout's copy of those is caught only by the last check, a backstop with no
    mutant behind it. A location elsewhere on the machine is not watched: outputs moved
    there fail the own-build-folder check, and a shared input read from there is caught
    only if it differs between the checkouts. The route involves no game input or
    per-user data; ExplicitAssetRootTests in test_workspace_paths covers those roots.
    """

    make: ClassVar[str]

    @classmethod
    def setUpClass(cls) -> None:
        make = shutil.which("mingw32-make") or shutil.which("make")
        if not make:
            reason = "GNU Make is required for the two-checkout isolation check"
            print(f"SKIP {__name__}: {reason} (no mingw32-make or make on PATH)")
            raise unittest.SkipTest(reason)
        if not (shutil.which("git") or (ROOT / ".git").exists()):
            reason = "Git is required to enumerate the tracked tree"
            print(f"SKIP {__name__}: {reason}")
            raise unittest.SkipTest(reason)
        if not (os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc")):
            reason = "a C compiler is required to build the isolation route"
            print(f"SKIP {__name__}: {reason} (no gcc or cc on PATH)")
            raise unittest.SkipTest(reason)
        cls.make = make

    def test_two_checkouts_building_at_once_consume_only_their_own_assets(self) -> None:
        base = Path(tempfile.mkdtemp(prefix="nakagawa two checkouts "))
        self.addCleanup(shutil.rmtree, base, True)
        self.assertIn(" ", base.name)
        checkouts = {name: base / f"checkout-{name}" for name in ("a", "b")}
        logs = base / "logs"
        logs.mkdir()
        for name, checkout in checkouts.items():
            checkout.mkdir()
            self.assertGreater(copy_tracked_tree(ROOT, checkout), 100)
            source = checkout / ISOLATION_MARKED_SOURCE
            source.write_bytes(
                source.read_bytes()
                + b"\n__attribute__((used)) static const char nk_isolation_marker[] = \""
                + _marker(name) + b"\";\n"
            )
        private = ("checkout-a", "checkout-b", "logs")
        outside = _snapshot(base, skip=private)

        runs = {}
        for name, checkout in checkouts.items():
            log = (logs / f"{name}.log").open("w", encoding="utf-8", errors="replace")
            self.addCleanup(log.close)
            run = subprocess.Popen(
                [self.make, "--no-print-directory", ISOLATION_GOAL, f"GAME_NAME=iso{name}",
                 f"PYTHON={Path(sys.executable).as_posix()}"],
                cwd=checkout, stdout=log, stderr=subprocess.STDOUT,
            )
            # A run still going when the test ends (a timeout, a failed assertion) is
            # stopped, so it neither keeps building nor holds the temporary tree open.
            self.addCleanup(_stop, run)
            runs[name] = run
        for run in runs.values():
            run.wait(timeout=COMMAND_TIMEOUT_S)
        for name, run in runs.items():
            output = (logs / f"{name}.log").read_text(encoding="utf-8", errors="replace")
            self.assertEqual(run.returncode, 0, f"checkout {name}:\n{output[-4000:]}")

        self.assertEqual(_changed(outside, _snapshot(base, skip=private)), [],
                         "the two builds wrote beside their checkouts, in a place both share")

        for name, checkout in checkouts.items():
            other = "b" if name == "a" else "a"
            build_dir = checkout / "build" / f"iso{name}"
            for binary in ISOLATION_BINARIES:
                with self.subTest(checkout=name, binary=binary):
                    self.assertTrue((build_dir / binary).is_file(),
                                    f"checkout {name} did not build {binary} in its own "
                                    f"build folder {build_dir}")
                    data = (build_dir / binary).read_bytes()
                    self.assertIn(_marker(name), data)
                    self.assertNotIn(_marker(other), data)
            other_folder = f"checkout-{other}".encode("ascii")
            naming_other = sorted(
                path.relative_to(checkout).as_posix()
                for path in (checkout / "build").rglob("*")
                if path.is_file() and other_folder in path.read_bytes().lower()
            )
            self.assertEqual(naming_other, [],
                             f"checkout {name} built files that name checkout {other}")


if __name__ == "__main__":
    unittest.main()
