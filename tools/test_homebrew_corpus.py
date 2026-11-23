# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The public homebrew corpus must stay pinned, licence-compatible and honest (#2144).

These tests use no network. The build and runtime integrations are opt-in.
They check ``tests/fixtures/homebrew_corpus/corpus.json``
against the PSPDEV lock, against PROVENANCE.md and CREDITS.md, and against the unit rules in
``tools/homebrew_corpus.py``. Where a PSPDEV is installed (``PSPDEV`` or ``/usr/local/pspdev``)
they also recompute every pinned file's Git blob ID from its samples, so a toolchain that
carries other sources than the pin fails here instead of being used silently.

Run the gu-cube runtime test on Linux with the locked PSPDEV, GCC, SDL3,
Vulkan, a working desktop display (Xvfb is sufficient), and CPython 3.14 or
3.15. Set ``NK_HOMEBREW_RUNTIME_REQUIRED=1`` to enable it. Set ``PYTHON`` to
the supported CPython executable if it differs from the test interpreter.
"""

from __future__ import annotations

import copy
from contextlib import suppress
import json
import os
from pathlib import Path
import re
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import homebrew_corpus  # noqa: E402
from make_showcase_animation import HEIGHT, WIDTH, read_ppm  # noqa: E402
from scratch_dir import TemporaryDirectory  # noqa: E402

PROVENANCE = ROOT / "tests" / "fixtures" / "homebrew_corpus" / "PROVENANCE.md"
CREDITS = ROOT / "CREDITS.md"
COMPATIBILITY = ROOT / "docs" / "COMPATIBILITY.md"
REQUIRED_ENV = "NK_HOMEBREW_CORPUS_REQUIRED"
RUNTIME_REQUIRED_ENV = "NK_HOMEBREW_RUNTIME_REQUIRED"

# The pinned sample clears to PSP GU ABGR 0xff554433 before drawing the cubes.
CUBE_BACKGROUND = bytes.fromhex("334455")
CUBE_MIN_FOREGROUND = 1000
CUBE_PRESENTS = 120


def cube_face_counts(pixels: list[bytes]) -> list[int]:
    """Count red, green and blue faces where GU_TFX_ADD adds a gray texel."""
    counts = [0, 0, 0]
    for pixel in pixels:
        if pixel == CUBE_BACKGROUND:
            continue
        channel = pixel.index(max(pixel))
        weak_a, weak_b = pixel[(channel + 1) % 3], pixel[(channel + 2) % 3]
        if abs(weak_a - weak_b) <= 6 and pixel[channel] - max(weak_a, weak_b) >= 32:
            counts[channel] += 1
    return counts


def check_cube_frames(frames: list[Path]) -> tuple[int, int, list[int]]:
    """Count textured cube frames and measure motion in their RGB pixels."""
    reference = None
    textured = 0
    motion = 0
    face_totals = [0, 0, 0]
    bottom = (HEIGHT - 1) * WIDTH
    required_background = (0, WIDTH - 1, bottom, bottom + WIDTH - 1)
    for frame in frames:
        rgb = read_ppm(frame)
        pixels = [rgb[i:i + 3] for i in range(0, len(rgb), 3)]
        if any(pixels[i] != CUBE_BACKGROUND for i in required_background):
            continue
        foreground = len(pixels) - pixels.count(CUBE_BACKGROUND)
        if not CUBE_MIN_FOREGROUND <= foreground < WIDTH * HEIGHT // 2:
            continue
        if pixels[(HEIGHT // 2) * WIDTH + WIDTH // 2] == CUBE_BACKGROUND or len(set(pixels)) <= 16:
            continue
        # With gray texture values added to the pinned vertices' primary
        # colors, most foreground pixels keep one dominant channel.
        faces = cube_face_counts(pixels)
        if 5 * sum(faces) < 3 * foreground:
            continue
        textured += 1
        for channel, count in enumerate(faces):
            face_totals[channel] += count
        if reference is None:
            reference = pixels
        else:
            motion = max(motion, sum(a != b for a, b in zip(reference, pixels, strict=True)))
    if textured < 2:
        raise ValueError(f"only {textured} of {len(frames)} captures showed the textured cube")
    if motion < CUBE_MIN_FOREGROUND:
        raise ValueError(f"textured cube did not move: only {motion} pixels changed")
    if sum(count >= CUBE_MIN_FOREGROUND for count in face_totals) < 2:
        raise ValueError(f"fewer than two textured primary face colors appeared: {face_totals}")
    return textured, motion, face_totals


def linux_pidfds_available() -> bool:
    if not hasattr(os, "pidfd_open") or not hasattr(signal, "pidfd_send_signal"):
        return False
    try:
        fd = os.pidfd_open(os.getpid())
        try:
            signal.pidfd_send_signal(fd, 0)
        finally:
            os.close(fd)
    except OSError:
        return False
    return True


def stop_phase_session(process: subprocess.Popen[bytes]) -> bool:
    """Stop live members of this phase's Linux session and report if any were stopped."""
    # Keep the original leader unreaped until cleanup completes. Even if it has
    # exited, its PID still identifies this session and cannot have been recycled.
    session = process.pid
    if process.returncode is not None or os.getsid(session) != session:
        raise RuntimeError("cannot verify ownership of the phase's session")

    deadline = time.monotonic() + 5.0
    empty_scan = False
    stopped = False
    while time.monotonic() < deadline:
        live = False
        for entry in Path("/proc").iterdir():
            if not entry.name.isdigit():
                continue
            pid = int(entry.name)
            with suppress(ProcessLookupError):
                if os.getsid(pid) != session:
                    continue
                fd = os.pidfd_open(pid)
                try:
                    # The PID could have changed owners before pidfd_open. Check
                    # membership again and signal the open identity, not its number.
                    if os.getsid(pid) != session:
                        continue
                    watcher = select.poll()
                    watcher.register(fd, select.POLLIN)
                    if not watcher.poll(0):
                        live = True
                        signal.pidfd_send_signal(fd, signal.SIGKILL)
                        stopped = True
                finally:
                    os.close(fd)
        # Rescan for a child created while the first scan was still in progress.
        if not live and empty_scan:
            return stopped
        empty_scan = not live
        time.sleep(0.01)
    raise TimeoutError("some processes in the phase's session are still alive")


def installed_samples_root() -> Path | None:
    for prefix in (os.environ.get("PSPDEV"), "/usr/local/pspdev"):
        if prefix:
            candidate = Path(prefix) / str(homebrew_corpus.load_corpus()["source"]["delivery"]["samples_root"])
            if candidate.is_dir():
                return candidate
    return None


class CorpusPinTests(unittest.TestCase):
    def setUp(self) -> None:
        self.corpus = homebrew_corpus.load_corpus()

    def test_checked_in_corpus_is_valid(self) -> None:
        homebrew_corpus.validate(self.corpus)
        self.assertGreaterEqual(len(self.corpus["programs"]), 3)

    def test_unknown_ids_blob_ids_and_unpinned_revisions_are_refused(self) -> None:
        def refused(mutate, message: str) -> None:
            broken = copy.deepcopy(self.corpus)
            mutate(broken)
            with self.assertRaisesRegex(homebrew_corpus.CorpusError, message):
                homebrew_corpus.validate(broken)

        refused(lambda c: c["source"].update(commit="master"), "exact 40-hex revision")
        refused(lambda c: c["source"].update(commit="0" * 40), "PSPDEV lock pins")
        refused(lambda c: c["source"].update(license_expression="GPL-2.0-only"), "compatible")
        refused(lambda c: c["source"]["delivery"].update(archive_sha256="0" * 64), "SHA-256 the PSPDEV lock")
        refused(lambda c: c["programs"][0]["files"][0].update(git_blob_sha1="abc"), "40-hex git_blob_sha1")
        refused(lambda c: c["programs"][0]["files"][0].update(path="../escape.c"), "inside the samples root")
        refused(lambda c: c["programs"].__delitem__(slice(2, None)), "at least 3 programs")
        refused(lambda c: c["programs"][1].update(id=c["programs"][0]["id"]), "malformed or repeated")
        refused(lambda c: c["programs"][0].update(smoke_checks=["nonblank_frame"]), "first_frame")
        refused(lambda c: c["programs"][0]["expected"].update(status="PASSED"), "expected.status")
        refused(lambda c: c["programs"][0]["files"].append(dict(c["programs"][0]["files"][0])), "listed twice")
        refused(lambda c: c["programs"][0]["files"].__delitem__(0), "Makefile is not pinned")
        refused(lambda c: c["programs"][0].pop("sample_dir"), "sample_dir is required")
        refused(lambda c: c["programs"][0].update(target="../cube"), "plain filename stem")
        refused(lambda c: c["programs"][0].update(smoke_checks=["first_frame", "bogus"]), "only the vocabulary")
        refused(lambda c: c["programs"][0]["files"][0].update(upstream_path="elsewhere/x.c"), "under src/samples/")
        # Windows joins a backslash or drive-qualified path outside the root, so none passes.
        for escape in ("..\\..\\x.c", "gu\\cube\\..\\..\\..\\x.c", "C:/Windows/win.ini", "C:x.c",
                       "gu//cube.c", "./gu/cube.c", "gu/./cube.c", "/abs.c"):
            refused(lambda c, p=escape: c["programs"][0]["files"][0].update(path=p), "inside the samples root")
        refused(lambda c: c["programs"][0].update(sample_dir="..\\..\\gu"), "sample_dir must be relative")
        refused(lambda c: c["programs"][0].update(sample_dir="C:/gu/cube"), "sample_dir must be relative")
        refused(lambda c: c["programs"][0]["files"][0].update(upstream_path="src/samples/../../x.c"),
                "under src/samples/")

    def test_a_result_without_backing_evidence_is_refused(self) -> None:
        def refused(**expected: object) -> None:
            broken = copy.deepcopy(self.corpus)
            broken["programs"][0]["expected"].update(expected)
            with self.assertRaises(homebrew_corpus.CorpusError):
                homebrew_corpus.validate(broken)

        refused(status="PASS")
        refused(status="PASS", evidence="")
        refused(status="PASS", evidence="reports/smoke/gu-cube.log")
        refused(evidence="reports/smoke/gu-cube.log")
        for issue in (True, "308", 0, -1, 3.5):
            refused(status="KNOWN_FAIL", issue=issue)

    def test_known_fail_must_name_its_issue(self) -> None:
        broken = copy.deepcopy(self.corpus)
        broken["programs"][0]["expected"].update(status="KNOWN_FAIL", issue=None)
        with self.assertRaisesRegex(homebrew_corpus.CorpusError, "KNOWN_FAIL needs"):
            homebrew_corpus.validate(broken)
        broken["programs"][0]["expected"]["issue"] = 308
        homebrew_corpus.validate(broken)

    def test_git_blob_id_matches_git(self) -> None:
        self.assertEqual(homebrew_corpus.git_blob_sha1(b"hello world"),
                         "95d09f2b10159347eece71399a7e2e907ea3df4f")
        self.assertEqual(homebrew_corpus.git_blob_sha1(b""),
                         "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391")

    def test_installed_pspdev_carries_exactly_the_pinned_sources(self) -> None:
        root = installed_samples_root()
        if root is None:
            if os.environ.get(REQUIRED_ENV) == "1":
                self.fail(f"{REQUIRED_ENV}=1, but no PSPDEV samples were found")
            self.skipTest("NOT_RUN: PSPDEV samples are not installed on this host")
        self.assertEqual(homebrew_corpus.verify_sources(self.corpus, root), [])

    def test_verify_sources_reports_a_changed_and_a_missing_file(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-corpus-") as tmp:
            root = Path(tmp)
            first = self.corpus["programs"][0]["files"][0]
            target = root / first["path"]
            target.parent.mkdir(parents=True)
            target.write_bytes(b"not the pinned bytes")
            differences = homebrew_corpus.verify_sources(self.corpus, root)
        self.assertTrue(any(first["path"] in line and "pinned" in line for line in differences))
        self.assertTrue(any("missing" in line for line in differences))

    def test_verify_sources_never_reads_outside_the_samples_root(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-corpus-") as tmp:
            outside = Path(tmp) / "outside.c"
            outside.write_bytes(b"outside")
            root = Path(tmp) / "samples"
            root.mkdir()
            broken = copy.deepcopy(self.corpus)
            entry = broken["programs"][0]["files"][0]
            entry.update(path="..\\outside.c", git_blob_sha1=homebrew_corpus.git_blob_sha1(b"outside"))
            differences = homebrew_corpus.verify_sources(broken, root)
        self.assertTrue(any("outside.c" in line and "inside the samples root" in line
                            for line in differences), differences)


    def test_build_refuses_modified_sources_before_make(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-corpus-tamper-") as tmp:
            pspdev = Path(tmp)
            for tool in ("psp-gcc", "psp-config"):
                binary = pspdev / "bin" / tool
                binary.parent.mkdir(exist_ok=True)
                binary.touch()
            root = pspdev / self.corpus["source"]["delivery"]["samples_root"]
            first = root / self.corpus["programs"][0]["files"][0]["path"]
            first.parent.mkdir(parents=True)
            first.write_text("other bytes", encoding="ascii")
            output = pspdev / "sample.elf"
            with patch.object(homebrew_corpus.subprocess, "run") as make:
                with self.assertRaisesRegex(homebrew_corpus.CorpusError, "pinned"):
                    homebrew_corpus.build_program("gu-cube", pspdev, output)
            make.assert_not_called()
            self.assertFalse(output.exists())

    @unittest.skipUnless(os.environ.get(REQUIRED_ENV) == "1", "NOT_RUN: corpus build was not requested")
    def test_pinned_samples_build_to_guest_elfs(self) -> None:
        root = installed_samples_root()
        self.assertIsNotNone(root, "PSPDEV samples are not installed on this host")
        assert root is not None
        prefix = root.parents[2]
        with tempfile.TemporaryDirectory(prefix="nk-corpus-build-") as tmp:
            for program in self.corpus["programs"]:
                with self.subTest(program=program["id"]):
                    output = Path(tmp) / (program["id"] + ".elf")
                    homebrew_corpus.build_program(program["id"], prefix, output)
                    header = output.read_bytes()[:20]
                    self.assertEqual(header[:6], b"\x7fELF\x01\x01")
                    self.assertEqual(int.from_bytes(header[18:20], "little"), 8)  # EM_MIPS


class CorpusRecordsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.corpus = homebrew_corpus.load_corpus()
        self.provenance = PROVENANCE.read_text(encoding="utf-8")

    def test_provenance_lists_the_pin_and_every_program_file(self) -> None:
        source = self.corpus["source"]
        for pin in (source["commit"], source["delivery"]["archive_sha256"], source["delivery"]["release"],
                    source["license_blob_sha1"], source["license_expression"]):
            self.assertIn(pin, self.provenance)
        for program in self.corpus["programs"]:
            for entry in program["files"] + program["shared_files"]:
                self.assertIn(entry["git_blob_sha1"], self.provenance, entry["path"])
                self.assertIn(entry["upstream_path"], self.provenance, entry["path"])

    def test_credits_name_pspsdk_samples_and_the_corpus_page_lists_every_program(self) -> None:
        credits = CREDITS.read_text(encoding="utf-8")
        self.assertIn("tests/fixtures/homebrew_corpus/PROVENANCE.md", credits)
        page = COMPATIBILITY.read_text(encoding="utf-8")
        self.assertIn("## Homebrew corpus", page)
        for program in self.corpus["programs"]:
            row = next((line for line in page.splitlines() if line.startswith(f"| `{program['id']}` |")), None)
            self.assertIsNotNone(row, program["id"])
            assert row is not None
            self.assertTrue(row.rstrip(" |").endswith(f"| {program['expected']['status'].lower()}"), row)

    def test_every_copyright_holder_is_credited(self) -> None:
        for program in self.corpus["programs"]:
            for holder in program["copyright"]:
                self.assertIn(holder, self.provenance, program["id"])


class CubeFrameChecksTests(unittest.TestCase):
    @staticmethod
    def write_frame(path: Path, left: int, *, background: bytes = CUBE_BACKGROUND,
                    row: bytes | None = None) -> None:
        pixels = bytearray(background * (WIDTH * HEIGHT))
        if row is None:
            # Two textured primary colors, with a gray ramp added to both faces.
            row = b"".join(bytes((i + 127, i, i)) if i < 32 else bytes((i, i, i + 127))
                           for i in range(64))
        for y in range(100, 164):
            start = (y * WIDTH + left) * 3
            pixels[start:start + len(row)] = row
        path.write_bytes(f"P6\n{WIDTH} {HEIGHT}\n255\n".encode("ascii") + pixels)

    def test_moving_textured_frames_pass(self) -> None:
        with TemporaryDirectory(prefix="nk-cube-frames-") as tmp:
            frames = [Path(tmp) / name for name in ("frame_v28.ppm", "frame_v36.ppm")]
            self.write_frame(frames[0], 200)
            self.write_frame(frames[1], 216)
            self.assertEqual(check_cube_frames(frames), (2, 5120, [4096, 0, 4096]))

    def test_wrong_background_and_static_texture_are_refused(self) -> None:
        with TemporaryDirectory(prefix="nk-cube-mutant-") as tmp:
            frames = [Path(tmp) / name for name in ("frame_v28.ppm", "frame_v36.ppm")]
            for frame in frames:
                self.write_frame(frame, 200)
            with self.assertRaisesRegex(ValueError, "did not move"):
                check_cube_frames(frames)
            self.write_frame(frames[1], 216, background=b"\x00\x00\x00")
            with self.assertRaisesRegex(ValueError, "only 1 of 2 captures"):
                check_cube_frames(frames)

    def test_moving_arbitrary_colors_and_one_face_color_are_refused(self) -> None:
        with TemporaryDirectory(prefix="nk-cube-colors-") as tmp:
            frames = [Path(tmp) / name for name in ("frame_v28.ppm", "frame_v36.ppm")]
            arbitrary = b"".join(bytes((i, i * 2, 255)) for i in range(64))
            for frame, left in zip(frames, (200, 216), strict=True):
                self.write_frame(frame, left, row=arbitrary)
            with self.assertRaisesRegex(ValueError, "only 0 of 2 captures"):
                check_cube_frames(frames)
            red_only = b"".join(bytes((i + 127, i, i)) for i in range(64))
            for frame, left in zip(frames, (200, 216), strict=True):
                self.write_frame(frame, left, row=red_only)
            with self.assertRaisesRegex(ValueError, "fewer than two textured primary face colors"):
                check_cube_frames(frames)


class CorpusRuntimeSmokeTests(unittest.TestCase):
    def run_phase(self, phase: str, argv: list[str | Path], env: dict[str, str],
                  *, timeout: int = 120, exit_code: int = 0) -> str:
        # Waiting on the pidfd observes exit without reaping the session leader.
        # communicate() may reap it while handling Ctrl-C, losing our identity anchor.
        with tempfile.TemporaryFile() as log:
            process = subprocess.Popen(
                argv, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
            )
            try:
                fd = os.pidfd_open(process.pid)
                try:
                    watcher = select.poll()
                    watcher.register(fd, select.POLLIN)
                    exited = bool(watcher.poll(timeout * 1000))
                finally:
                    os.close(fd)
            finally:
                try:
                    stopped = stop_phase_session(process)
                except KeyboardInterrupt:
                    stop_phase_session(process)
                    process.wait(timeout=10)
                    raise
                process.wait(timeout=10)
            log.seek(0)
            output = log.read().decode("utf-8", errors="replace")
        self.assertTrue(exited, f"{phase} timed out after {timeout}s\n{output[-6000:]}")
        self.assertFalse(stopped, f"{phase} exited with live session members\n{output[-6000:]}")
        self.assertEqual(process.returncode, exit_code, f"{phase}\n{output[-6000:]}")
        return output

    @unittest.skipUnless(sys.platform.startswith("linux"), "NOT_RUN: Linux session cleanup test")
    def test_timed_out_phase_stops_another_process_group(self) -> None:
        self.check_phase_cleanup("timeout", AssertionError, "fixture phase timed out")

    @unittest.skipUnless(sys.platform.startswith("linux"), "NOT_RUN: Linux session cleanup test")
    def test_phase_cleanup_keeps_session_after_early_exit_and_interrupt(self) -> None:
        with self.subTest(mode="early-exit"):
            self.check_phase_cleanup("early-exit", AssertionError, "fixture phase exited with live session members")
        with self.subTest(mode="interrupt"):
            self.check_phase_cleanup("interrupt", KeyboardInterrupt, "")

    def check_phase_cleanup(self, mode: str, error: type[BaseException], message: str) -> None:
        if not linux_pidfds_available():
            self.skipTest("NOT_RUN: Linux pidfds are unavailable")
        # The leader and its child can exit naturally after the bounded wait,
        # even if the cleanup under test leaves a process alive.
        code = """
import os
from pathlib import Path
import signal
import sys
import time

marker, stop, mode = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
caller = int(sys.argv[4])
child = os.fork()
if child == 0:
    os.setpgid(0, 0)
    ready = marker.with_suffix(".tmp")
    ready.write_text(f"{os.getpid()} {os.getpgrp()} {os.getsid(0)}")
    ready.replace(marker)
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline and not stop.exists():
        time.sleep(0.02)
else:
    if mode == "timeout":
        os.waitpid(child, 0)
    else:
        deadline = time.monotonic() + 5
        while not marker.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        if mode == "interrupt" and marker.exists() and os.getppid() == caller:
            fd = os.pidfd_open(caller)
            try:
                if os.getppid() == caller:
                    signal.pidfd_send_signal(fd, signal.SIGINT)
            finally:
                os.close(fd)
        os._exit(0)
"""
        with TemporaryDirectory(prefix="nk-cube-session-") as tmp:
            marker, stop = Path(tmp) / "child", Path(tmp) / "stop"
            try:
                with self.assertRaisesRegex(error, message):
                    self.run_phase(
                        "fixture phase", [sys.executable, "-c", code, marker, stop, mode, str(os.getpid())],
                        dict(os.environ), timeout=2 if mode == "timeout" else 8,
                    )
                pid, group, session = map(int, marker.read_text(encoding="utf-8").split())
                self.assertEqual(pid, group)
                self.assertNotEqual(group, session)
                with suppress(ProcessLookupError):
                    fd = os.pidfd_open(pid)
                    try:
                        if os.getsid(pid) == session:
                            watcher = select.poll()
                            watcher.register(fd, select.POLLIN)
                            stopped = bool(watcher.poll(0))
                            stop.touch()
                            self.assertTrue(watcher.poll(2000), "fixture child did not stop")
                            self.assertTrue(stopped, "child in another process group survived")
                    finally:
                        os.close(fd)
            finally:
                stop.touch()

    @unittest.skipUnless(
        os.environ.get(RUNTIME_REQUIRED_ENV) == "1",
        f"NOT_RUN: opt in with {RUNTIME_REQUIRED_ENV}=1 on a Linux Vulkan display",
    )
    def test_gu_cube_runtime_smoke(self) -> None:
        """Go from the pinned PSPDEV sample to a changing cube in real player presents."""
        self.assertTrue(sys.platform.startswith("linux"), "gu-cube smoke requires Linux host PSPDEV")
        self.assertTrue(linux_pidfds_available(),
                        "gu-cube smoke requires Linux pidfds to clean up its build and runtime children")
        samples = installed_samples_root()
        self.assertIsNotNone(samples, "gu-cube smoke requires the locked PSPDEV installation")
        assert samples is not None
        pspdev = samples.parents[2]
        for tool in ("make", "gcc"):
            self.assertIsNotNone(shutil.which(tool), f"gu-cube smoke needs {tool} on PATH")
        for tool in ("mksfoex", "pack-pbp"):
            self.assertTrue((pspdev / "bin" / tool).is_file(), f"PSPDEV is missing bin/{tool}")
        self.assertTrue(os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"),
                        "gu-cube smoke needs a real Vulkan presentation surface (Xvfb works)")
        self.assertNotIn(os.environ.get("SDL_VIDEODRIVER"), ("dummy", "offscreen"),
                         "gu-cube smoke needs a windowed SDL video driver")
        python = shutil.which(os.environ.get("PYTHON") or sys.executable)
        self.assertIsNotNone(python, "PYTHON must name a runnable CPython 3.14 or 3.15")
        assert python is not None

        with TemporaryDirectory(prefix="nk-gu-cube-smoke-") as tmp:
            root = Path(tmp)
            child_tmp = root / "tmp"
            child_tmp.mkdir()
            # Limit the route to its new profiles, build and caches. Ignore inherited runtime
            # debug knobs that could alter rendering or direct a child to another profile.
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith(("NK_", "NAKAGAWA_", "SR_")) or key == "SR_SOURCE_COMMIT"}
            for key in ("MAKEFLAGS", "GNUMAKEFLAGS", "MFLAGS", "MAKEFILES"):
                env.pop(key, None)
            env.update(
                PYTHON=python,
                TMPDIR=str(child_tmp),
                XDG_CONFIG_HOME=str(root / "config"),
                XDG_DATA_HOME=str(root / "data"),
                XDG_CACHE_HOME=str(root / "cache"),
                XDG_STATE_HOME=str(root / "state"),
                NK_BUILD_ROOT=str(root / "runtime-build"),
                SDL_AUDIODRIVER="dummy",
            )
            stage_env = env | {"SDL_VIDEODRIVER": "dummy"}
            elf = root / "cube.elf"
            # End the whole build session before the CLI's own 180 s Make timeout.
            self.run_phase("build the pinned gu-cube ELF",
                           [python, ROOT / "tools" / "homebrew_corpus.py", "build", "gu-cube",
                            "--pspdev", pspdev, "--out", elf], env, timeout=120)
            sfo = root / "PARAM.SFO"
            eboot = root / "GUCUBE" / "EBOOT.PBP"
            eboot.parent.mkdir()
            self.run_phase("create the homebrew SFO", [pspdev / "bin" / "mksfoex", "Cube Sample", sfo], env)
            self.run_phase("package the pinned ELF", [pspdev / "bin" / "pack-pbp", eboot, sfo,
                           "NULL", "NULL", "NULL", "NULL", "NULL", elf, "NULL"], env)
            expected_pbp = eboot.read_bytes()
            wrong = root / "wrong" / "GUCUBE" / "WRONG.PBP"
            wrong.parent.mkdir(parents=True)
            shutil.copyfile(eboot, wrong)
            self.assertEqual(wrong.read_bytes(), expected_pbp)

            tools = root / "tools"
            self.run_phase("build native test entry points",
                           ["make", "-j4", "CC=gcc", f"BUILD_ROOT={tools}", "player",
                            "package-builder-test-bin", "player-state-test-bin"], stage_env, timeout=600)
            player = tools / "nakagawa_player"
            wrong_user = root / "wrong-profile"
            wrong_user.mkdir()
            wrong_log = self.run_phase(
                "refuse the byte-identical wrong basename",
                [player, f"--user-data-root={wrong_user}", f"--iso={wrong}", "--stage-only"],
                stage_env, exit_code=1,
            )
            self.assertIn("HOMEBREW_FOLDER_UNSUPPORTED", wrong_log)
            self.assertIn("WRONG.PBP", wrong_log)
            self.assertEqual(wrong.read_bytes(), expected_pbp)
            self.assertFalse((wrong_user / "library.json").exists())
            for folder in ("discs", "experimental", "packages"):
                self.assertEqual(list((wrong_user / folder).glob("*")), [])

            user = root / "user"
            user.mkdir()
            imported = self.run_phase(
                "import the standard EBOOT.PBP",
                [player, f"--user-data-root={user}", f"--iso={eboot}", "--stage-only"],
                stage_env, exit_code=3,  # Experimental import stops at the title list.
            )
            found = re.search(r"Disc ID: (X[A-Z]{3}\d{5}), Title: Cube Sample, Supported: 0", imported)
            self.assertIsNotNone(found, imported)
            assert found is not None
            disc_id = found[1]
            profile = json.loads((user / "experimental" / disc_id / "profile.json").read_text(encoding="utf-8"))
            manifest = profile["manifest"]
            self.assertEqual(manifest["verification_profile"], "experimental-unverified")
            self.assertEqual(manifest["display_name"], "Cube Sample")
            disc = user / "discs" / disc_id
            iso, = disc.glob("*.iso")
            view = disc / f"{disc_id}.ms0" / "PSP" / "GAME" / "GUCUBE" / "EBOOT.PBP"
            self.assertEqual(view.read_bytes(), expected_pbp)
            self.assertEqual(eboot.read_bytes(), expected_pbp)

            # Like the production package-route tests, stage the record ADD TO LIBRARY writes,
            # then use the native BUILD PACKAGE harness and production package validator.
            title_id = manifest["id"]
            (user / "library.json").write_text(json.dumps({
                "schema_version": 1,
                "games": [{
                    "disc_id": disc_id, "title_id": title_id, "title_name": "Cube Sample",
                    "is_experimental": True, "selected_executable": "EBOOT.BIN", "iso_path": str(iso),
                }],
            }), encoding="utf-8")
            package = user / "packages" / disc_id
            self.assertFalse(package.exists())
            built = self.run_phase(
                "build the AOT package",
                [tools / "test_package_builder", "--build-package", ROOT, user, disc_id, user / "logs"],
                stage_env, timeout=960,
            )
            self.assertIn("PACKAGE_BUILD_ROUTE status=PASS", built)
            self.assertIn("Package checked and ready:", built, "the fresh profile must compile its package")
            validated = self.run_phase(
                "validate the built package",
                [tools / "test_player_state", "--validate-package", user, disc_id, title_id, "1", "EBOOT.BIN"],
                stage_env,
            )
            self.assertIn("PACKAGE_STATUS=OK", validated)
            self.assertEqual(list(package.glob("frame_v*.ppm")), [])

            launch_env = env | {
                "SR_WINDOW_HIDDEN": "1",
                "SR_DETERMINISTIC_CLOCK": "1",
                "SR_FBSNAP": "8",
                "SR_FBSNAP_WINDOWS": "28-130",
            }
            launched = self.run_phase(
                "play through the real Vulkan presenter",
                [player, "--empty", "--launch-index=0", "--headless-launch",
                 f"--runtime-root={user}", f"--user-data-root={user}",
                 f"--headless-accept-presented-frames={CUBE_PRESENTS}", "--headless-timeout-ms=60000"],
                launch_env,
            )
            self.assertIn(f"PLAY NOW available for {disc_id}", launched)
            self.assertIn(f"HEADLESS_ACCEPTANCE status=PASS presented_frames={CUBE_PRESENTS} child_exit=42", launched)
            boot_path, = (user / "cache").glob("player-boot-*.events")
            boot = boot_path.read_text(encoding="utf-8", errors="replace")
            self.assertIn("phase=render_path presenter=vulkan ge=gpu ", boot)
            self.assertIn("COMPAT_PATCH_REPORT: fired=0 none", boot)
            captures = re.findall(r"FBSNAP f=(\d+) swapchain capture -> frame_v\1\.ppm \(result=(-?\d+)\)", boot)
            self.assertTrue(captures and all(result == "1" for _, result in captures), captures)
            frames = sorted(package.glob("frame_v*.ppm"))
            self.assertEqual({frame.name for frame in frames}, {f"frame_v{vcount}.ppm" for vcount, _ in captures})
            textured, motion, faces = check_cube_frames(frames)
            print(f"GU_CUBE_RUNTIME_SMOKE presents={CUBE_PRESENTS} textured={textured}/{len(frames)} "
                  f"moving_pixels={motion} face_pixels_rgb={faces}")


if __name__ == "__main__":
    unittest.main()
