# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Source-owned showcase art and disc-image regression checks."""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from typing import Any
import unittest
from unittest import mock
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests" / "fixtures" / "showcase"))
sys.path.insert(0, str(ROOT / "tools"))

import showcase
from nk_core import package_cache
from nk_core.iso_inspect import inspect_iso


def minimal_elf() -> bytes:
    header = bytearray(52)
    header[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHIIIIIHHHHHH", header, 16,
                     2, 8, 1, 0x08804000, 52, 0, 0, 52, 32, 1, 40, 0, 0)
    program = struct.pack("<IIIIIIII", 1, 0x100, 0x08804000, 0x08804000,
                          4, 0x100, 5, 0x1000)
    return bytes(header) + program + bytes(0x100 - 84) + b"\x13\x37\x00\x00"


SCENE = next(demo for demo in showcase.DEMOS if demo["disc_id"] == "TEST00007")
BREAKOUT = next(demo for demo in showcase.DEMOS if demo["disc_id"] == "TEST00008")
CAPTURE = b"P6\n1 1\n255\n\0\0\0"


class ShowcasePauseLogTests(unittest.TestCase):
    def test_pause_input_uses_script_start_not_latch_vblank(self) -> None:
        output = (
            "SR_PAUSE: entered source=script vcount=120 vbl=120 vtime_us=200000 "
            "latency_us=10 quit_allowed=1 reason=none\n"
            "SR_PAUSE: resumed vcount=120 vbl=120 vtime_us=200000 paused_ms=1500 "
            "count=1 quit_allowed=1 reason=none\n"
            "ctrl_latch: vcount=122 buttons 0x0000 -> 0x4000 lx=128 ly=128\n"
            "ctrl_read: vcount=122 guest read scripted 0x4000 "
            "(latched 1 samples since vblank 121)\n"
            "ctrl_latch: vcount=126 buttons 0x4000 -> 0x0000 lx=128 ly=128\n"
        )
        showcase._check_pause_input(output, (121, 0x4000, 4), "synthetic")

    def test_pause_log_key_values_parse_for_scheduler_and_audio_checks(self) -> None:
        log = (
            "SR_PAUSE: requested source=script vcount=120\n"
            "SR_PAUSE: entered source=script vcount=120 vbl=120 vtime_us=200000 "
            "latency_us=12000 quit_allowed=0 reason=savedata\n"
            "SR_PAUSE: resumed vcount=120 vbl=120 vtime_us=200000 paused_ms=1500 "
            "count=1 quit_allowed=0 reason=savedata\n"
        )
        parsed = showcase.parse_pause_lines(log)
        self.assertEqual(parsed["requested"], [{"source": "script", "vcount": "120"}])
        entered, resumed = parsed["entered"][0], parsed["resumed"][0]
        self.assertEqual((entered["vcount"], entered["vbl"], entered["vtime_us"]),
                         (resumed["vcount"], resumed["vbl"], resumed["vtime_us"]))
        self.assertEqual((entered["quit_allowed"], entered["reason"]), ("0", "savedata"))
        self.assertEqual(resumed["paused_ms"], "1500")

    def test_pause_boundary_rejects_guest_input_or_ge_work_inside_window(self) -> None:
        output = (
            "SR_PAUSE: entered source=script vcount=120 vbl=120 vtime_us=200000 "
            "latency_us=10 quit_allowed=1 reason=none\n"
            "ctrl_latch: vcount=120 buttons 0x0000 -> 0x0000 lx=128 ly=128\n"
            "SR_PAUSE: resumed vcount=120 vbl=120 vtime_us=200000 paused_ms=1500 "
            "count=1 quit_allowed=1 reason=none\n"
        )
        with self.assertRaisesRegex(showcase.ShowcaseError, "latched guest input"):
            showcase._pause_pair_frozen(output, "synthetic")

    @staticmethod
    def press_lines(latch: int, read: tuple[int, int, int] | None, release: int | None) -> str:
        """The Cross press as the runtime logs it: latch, guest read (vcount, samples,
        since), release."""
        lines = f"ctrl_latch: vcount={latch} buttons 0x0000 -> 0x4000 lx=128 ly=128\n"
        if read is not None:
            lines += (f"ctrl_read: vcount={read[0]} guest read scripted 0x4000 "
                      f"(latched {read[1]} samples since vblank {read[2]})\n")
        if release is not None:
            lines += f"ctrl_latch: vcount={release} buttons 0x4000 -> 0x0000 lx=128 ly=128\n"
        return lines

    @classmethod
    def scheduler_pause_log(cls, resume_fields: str | None, exited: bool = True, *,
                            before: str = "", inside: str = "", after: str | None = None,
                            exit_vblanks: int = 180, pause_vcount: int = 120) -> str:
        """Breakout's scheduler-pause route: entry at VCOUNT 120, the scripted Cross after it."""
        resumed = "" if resume_fields is None else (
            f"SR_PAUSE: resumed vcount={pause_vcount} vbl={pause_vcount} vtime_us=200000 "
            f"{resume_fields} count=1 quit_allowed=1 reason=none\n")
        exit_event = (
            f"BOOT_EVENT phase=exit_at_vblank vblanks={exit_vblanks} (SR_EXIT_AT_VBLANK=180)\n"
            if exited else "")
        if after is None:
            after = cls.press_lines(122, (122, 1, 121), 126)
        return (
            before +
            f"SR_PAUSE: entered source=script vcount={pause_vcount} vbl={pause_vcount} "
            "vtime_us=200000 latency_us=10 quit_allowed=1 reason=none\n"
            + inside + resumed + after + exit_event
        )

    def test_scheduler_pause_is_judged_by_its_own_hold_record(self) -> None:
        # A run that held the full 1500 ms but took 4.129 s against a 2.753 s unpaused
        # baseline: the old wall-time difference (1.376 s < 1.4 s) failed it on an idle host.
        showcase._check_pause_smoke(self.scheduler_pause_log("paused_ms=1500"), 4.129,
                                    "synthetic")

    def test_scheduler_pause_rejects_a_skipped_or_short_hold(self) -> None:
        for held in ("paused_ms=0", "paused_ms=16", "paused_ms=1499"):
            with self.subTest(held=held), \
                    self.assertRaisesRegex(showcase.ShowcaseError,
                                           "less than the requested 1500 ms"):
                showcase._check_pause_smoke(self.scheduler_pause_log(held), 4.0, "synthetic")
        with self.assertRaisesRegex(showcase.ShowcaseError, "carries no paused_ms"):
            showcase._check_pause_smoke(self.scheduler_pause_log(""), 4.0, "synthetic")
        with self.assertRaisesRegex(showcase.ShowcaseError, "entered/resumed pair"):
            showcase._check_pause_smoke(self.scheduler_pause_log(None), 4.0, "synthetic")

    def test_scheduler_pause_hold_must_fit_inside_the_run(self) -> None:
        with self.assertRaisesRegex(showcase.ShowcaseError, "hold inside a 1.200 s run"):
            showcase._check_pause_smoke(self.scheduler_pause_log("paused_ms=1500"), 1.2,
                                        "synthetic")

    def test_scheduler_pause_run_must_still_reach_its_exit_vcount(self) -> None:
        with self.assertRaisesRegex(showcase.ShowcaseError, "SR_EXIT_AT_VBLANK completion"):
            showcase._check_pause_smoke(
                self.scheduler_pause_log("paused_ms=1500", exited=False), 4.0, "synthetic")
        with self.assertRaisesRegex(showcase.ShowcaseError, "before its target 180"):
            showcase._check_pause_smoke(
                self.scheduler_pause_log("paused_ms=1500", exit_vblanks=179), 4.0, "synthetic")

    def test_scheduler_pause_accepts_a_host_that_fell_behind(self) -> None:
        # Each of these came from the WSL smoke with the host at 70-74% CPU: the guest read a
        # frame or two late, a batch latched the press at 122, the exit stepped to 181.
        for name, after, exit_vblanks in (
                ("read a frame late", self.press_lines(121, (122, 2, 121), 125), 180),
                ("read two frames late", self.press_lines(121, (123, 3, 121), 126), 180),
                ("batched latch", self.press_lines(122, (123, 3, 122), 125), 180),
                ("exit stepped past", self.press_lines(121, (121, 1, 121), 125), 181)):
            with self.subTest(name):
                showcase._check_pause_smoke(
                    self.scheduler_pause_log("paused_ms=1500", after=after,
                                             exit_vblanks=exit_vblanks), 4.2, "synthetic")
        # VCOUNT stepped from 119 onto the press's row: the press was latched, then the pause
        # was requested and entered at 121; the guest read the press after the resume.
        showcase._check_pause_smoke(
            self.scheduler_pause_log(
                "paused_ms=1500", pause_vcount=121,
                before="ctrl_latch: vcount=121 buttons 0x0000 -> 0x4000 lx=128 ly=128\n",
                after="ctrl_read: vcount=122 guest read scripted 0x4000 "
                      "(latched 2 samples since vblank 121)\n"
                      "ctrl_latch: vcount=125 buttons 0x4000 -> 0x0000 lx=128 ly=128\n"),
            4.36, "synthetic")

    def test_scheduler_pause_input_keeps_its_exact_lower_bounds(self) -> None:
        cases = (
            ("a press latched before its row",
             dict(before=self.press_lines(119, (119, 1, 119), None),
                  after="ctrl_latch: vcount=123 buttons 0x4000 -> 0x0000 lx=128 ly=128\n"),
             "latched the press at VCOUNT 119, before its row at 121"),
            ("a press latched early after the resume",
             dict(after=self.press_lines(120, (121, 1, 120), 124)),
             "latched the press at VCOUNT 120, before its row at 121"),
            ("a dropped press", dict(after=""), "never latched the scheduled press"),
            ("another latch first after the resume",
             dict(after="ctrl_latch: vcount=121 buttons 0x0000 -> 0x0008 lx=128 ly=128\n"
                        + self.press_lines(122, (122, 1, 121), 126)),
             "did not latch the scheduled press first after resume"),
            ("a release before the row ended",
             dict(after=self.press_lines(121, (121, 1, 121), 124)),
             "released the press at VCOUNT 124, before its row ended at 125"),
            ("a press never released", dict(after=self.press_lines(121, (122, 2, 121), None)),
             "never released the scheduled press before exit"),
            ("a pause entered before its trigger", dict(pause_vcount=119),
             "entered at vcount 119, before its trigger 120"),
            ("a latch during the pause",
             dict(inside="ctrl_latch: vcount=121 buttons 0x0000 -> 0x4000 lx=128 ly=128\n"),
             "latched guest input or closed a GE window while paused"),
            ("a press never handed to the guest",
             dict(after=self.press_lines(121, None, 125)),
             "never handed the guest the scheduled press"),
            ("a read of a segment begun before its row",
             dict(after=self.press_lines(121, (121, 1, 120), 125)),
             "does not match its latch"),
            ("a release before the guest read the press",
             dict(after="ctrl_latch: vcount=121 buttons 0x0000 -> 0x4000 lx=128 ly=128\n"
                        "ctrl_latch: vcount=125 buttons 0x4000 -> 0x0000 lx=128 ly=128\n"
                        "ctrl_read: vcount=126 guest read scripted 0x4000 "
                        "(latched 4 samples since vblank 121)\n"),
             "released the press before the guest was handed it"),
        )
        for name, fields, message in cases:
            with self.subTest(name), self.assertRaisesRegex(showcase.ShowcaseError, message):
                showcase._check_pause_smoke(
                    self.scheduler_pause_log("paused_ms=1500", **fields), 4.2, "synthetic")

    def test_user_pause_refusal_parses_without_an_entry(self) -> None:
        output = "SR_PAUSE: refused reason=automation source=user vcount=60\n"
        showcase._check_scripted_pause_refusal(output, "synthetic")
        with self.assertRaisesRegex(showcase.ShowcaseError, "expected exactly one"):
            showcase._check_scripted_pause_refusal(
                "SR_PAUSE: requested source=user vcount=60\n", "synthetic")


def scene_log(*windows: tuple[int, int]) -> str:
    """A runtime log whose SR_GESTAT windows close at (vblank, tri3d) pairs."""
    return "".join(
        f"GESTAT f={frame} wall=10ms ge=4ms tri2d=0 tri3d={tri3d} spr2d=0 "
        f"px2d=130560 px3d={4096 if tri3d else 0} mw=1/1/1\n"
        for frame, tri3d in windows
    ) + "ctrl_latch: vcount=12 buttons 0x0000 -> 0x4000\n"


class ShowcaseFirstFrameCheckpointTests(unittest.TestCase):
    """The checkpoint is the guest's first drawn frame, at whatever vblank it landed."""

    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.package = Path(self._temp.name)
        self.log = self.package / "runtime.log"

    def tearDown(self) -> None:
        self._temp.cleanup()

    def test_window_closed_past_the_multiple_is_the_checkpoint(self) -> None:
        # A loaded host stepped VCOUNT 59 -> 61: the first window closes at 61 and its
        # capture is snap_f00061.ppm. The smoke used to demand the literal "GESTAT f=60"
        # and failed with "did not reach its first frame checkpoint" here.
        (self.package / "snap_f00061.ppm").write_bytes(CAPTURE)
        checkpoint, ppm = showcase.first_frame_checkpoint(
            SCENE, scene_log((61, 12), (120, 12), (181, 12)), self.package, self.log)
        self.assertEqual(checkpoint.frame, 61)
        self.assertEqual(checkpoint.counters["tri3d"], 12)
        self.assertEqual(ppm, self.package / "snap_f00061.ppm")

    def test_slow_boot_moves_the_checkpoint_to_the_first_drawn_window(self) -> None:
        # A starved host booted the guest late: its first flip came at vblank 55, so the
        # window closing at 64 holds only the clear and the cube first appears in the
        # window closing at 121. That window is the checkpoint, with its own capture.
        (self.package / "snap_f00064.ppm").write_bytes(CAPTURE)
        (self.package / "snap_f00121.ppm").write_bytes(CAPTURE)
        checkpoint, ppm = showcase.first_frame_checkpoint(
            SCENE, scene_log((64, 0), (121, 84), (180, 48)), self.package, self.log)
        self.assertEqual(checkpoint.frame, 121)
        self.assertEqual(ppm, self.package / "snap_f00121.ppm")

    def test_no_drawn_window_within_the_budget_is_a_missed_checkpoint(self) -> None:
        with self.assertRaisesRegex(
                showcase.ShowcaseError,
                r"TEST00007 did not reach its first frame checkpoint: no GE statistics window "
                r"before vblank 180 drew tri3d and px3d \(windows closed at vblank: 60, 120\)"):
            showcase.first_frame_checkpoint(SCENE, scene_log((60, 0), (120, 0)),
                                            self.package, self.log)
        with self.assertRaisesRegex(showcase.ShowcaseError,
                                    r"TEST00008 did not reach .*spr2d and px2d .*vblank: none"):
            showcase.first_frame_checkpoint(BREAKOUT, "BOOT_EVENT phase=init\n",
                                            self.package, self.log)

    def test_capture_must_be_the_checkpoint_window_s_own(self) -> None:
        # A capture from another window (or an earlier run) is not this checkpoint's frame.
        (self.package / "snap_f00120.ppm").write_bytes(CAPTURE)
        with self.assertRaisesRegex(showcase.ShowcaseError,
                                    r"no GE framebuffer capture .*\(vblank 61\)"):
            showcase.first_frame_checkpoint(SCENE, scene_log((61, 12), (120, 12)),
                                            self.package, self.log)

    def test_duplicate_window_breaks_the_runtime_contract(self) -> None:
        with self.assertRaisesRegex(showcase.ShowcaseError, "TEST00007 .*does not cross"):
            showcase.first_frame_checkpoint(SCENE, scene_log((60, 12), (60, 0), (120, 12)),
                                            self.package, self.log)

    def test_every_demo_names_the_counters_its_frame_draws(self) -> None:
        self.assertEqual(SCENE["frame_counters"], ("tri3d", "px3d"))
        self.assertEqual(BREAKOUT["frame_counters"], ("spr2d", "px2d"))


class ShowcaseScriptedPressTests(unittest.TestCase):
    """A failed check that depends on a scripted press says whether the guest read it."""

    def test_pad_script_rows_match_the_runtime_format(self) -> None:
        self.assertEqual(showcase._padscript(showcase.CROSS_PRESS, showcase.START_PRESS),
                         "12 4000 4\n240 0008 4\n")

    def test_a_press_the_guest_read_is_named_with_its_read(self) -> None:
        # Guest-time delivery holds the press until the guest reads it, so a late read is
        # normal on a loaded host; what the message needs is that the read happened.
        log = ("ctrl_latch: vcount=15 buttons 0x0000 -> 0x4000 lx=128 ly=128\n"
               "ctrl_read: vcount=57 guest read scripted 0x4000 (latched 43 samples since vblank 15)\n")
        self.assertEqual(
            showcase.scripted_press_note(log, showcase.CROSS_PRESS),
            " (the guest read the scripted press due at vblank 12 at vblank 57)")

    def test_a_press_the_guest_never_read_says_so(self) -> None:
        log = "ctrl_latch: vcount=120 buttons 0x0000 -> 0x0008 lx=128 ly=128\n"
        self.assertEqual(
            showcase.scripted_press_note(log, showcase.SAVE_START_PRESS),
            " (the guest never read the scripted press due at vblank 120)")

    def test_a_named_runtime_failure_reaches_the_smoke_message(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            cwd = Path(temp)
            log = ("ROUTE: pad script loaded\n"
                   "ROUTE_FAIL: pad script segment 2 of 4: the guest did not read the controller "
                   "while CROSS was held for 1800 vblanks\n")
            failed = subprocess.CompletedProcess(["runtime"], 86, stdout=log)
            with mock.patch.object(showcase.subprocess, "run", return_value=failed):
                with self.assertRaisesRegex(
                        showcase.ShowcaseError,
                        r"TEST00007 runtime exited 86 \(pad script segment 2 of 4: the guest did not "
                        r"read the controller while CROSS was held for 1800 vblanks\); see "):
                    showcase._run_runtime(["runtime"], {}, cwd, cwd / "runtime.log",
                                          "TEST00007 runtime")


class ShowcaseRuntimeRunTests(unittest.TestCase):
    def test_timeout_keeps_the_partial_log_and_names_it(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            cwd = Path(temp)
            log = cwd / "runtime.log"
            expired = subprocess.TimeoutExpired(["runtime"], 15, output=b"GESTAT f=60 tri3d=1\n")
            with mock.patch.object(showcase.subprocess, "run", side_effect=expired):
                with self.assertRaisesRegex(showcase.ShowcaseError,
                                            r"TEST00007 runtime exceeded the 15 second smoke window; see "):
                    showcase._run_runtime(["runtime"], {}, cwd, log, "TEST00007 runtime")
            self.assertEqual(log.read_text(encoding="utf-8"),
                             "GESTAT f=60 tri3d=1\n\nSHOWCASE_SMOKE_TIMEOUT\n")

    def test_captures_from_an_earlier_run_are_cleared_first(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            cwd = Path(temp)
            stale = cwd / "snap_f00060.ppm"
            stale.write_bytes(b"old")
            done = subprocess.CompletedProcess(["runtime"], 0, stdout="ok\n")
            with mock.patch.object(showcase.subprocess, "run", return_value=done) as run:
                output = showcase._run_runtime(["runtime"], {}, cwd, cwd / "runtime.log",
                                               "TEST00007 runtime")
            self.assertEqual(output, "ok\n")
            self.assertFalse(stale.exists())
            self.assertEqual(run.call_args.kwargs["timeout"], showcase.SMOKE_TIMEOUT_SECONDS)

    def test_exit_budget_is_three_statistics_windows(self) -> None:
        self.assertEqual(showcase.SMOKE_EXIT_VBLANK, 3 * showcase.WINDOW_VBLANKS)


class ShowcaseImageTests(unittest.TestCase):
    def test_framebuffer_metrics_require_visible_pixels(self) -> None:
        background = bytes((16, 24, 32))
        pixels = background + bytes((255, 0, 0)) + bytes((0, 255, 0)) + bytes((0, 0, 255))
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "frame.ppm"
            path.write_bytes(b"P6\n2 2\n255\n" + pixels)
            self.assertEqual(showcase._ppm_metrics(path, background), (4, 3))

    def test_ppm_size_rejects_truncated_or_non_p6_captures(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "frame.ppm"
            path.write_bytes(b"P6\n2 1\n255\n" + bytes(6))
            self.assertEqual(showcase._ppm_size(path), (2, 1))
            path.write_bytes(b"P6\n2 1\n255\n" + bytes(5))
            with self.assertRaises(showcase.ShowcaseError):
                showcase._ppm_size(path)
            path.write_bytes(b"P3\n2 1\n255\n" + bytes(6))
            with self.assertRaises(showcase.ShowcaseError):
                showcase._ppm_size(path)

    def test_headless_capture_reads_presented_and_published_vblanks_in_the_window(self) -> None:
        first, last = showcase.HEADLESS_CAPTURE_WINDOW
        output = "\n".join([
            f"HOST_PRESENT_SUBMITTED f={first - 1} buf=0x04000000 fmt=3 stride=512",
            f"HOST_PRESENT_SUBMITTED f={first} buf=0x04000000 fmt=3 stride=512",
            f"HOST_PRESENT_SUBMITTED f={last} buf=0x04000000 fmt=3 stride=512",
            f"FBSNAP f={first} swapchain capture -> frame_v{first}.ppm (result=1)",
            # A latched flip is reported at the next SetFrameBuf: the file name, not the
            # report's own f=, names the presented vblank.
            f"FBSNAP f={last + 1} swapchain capture -> frame_v{last}.ppm (result=1)",
            f"FBSNAP f={last + 2} swapchain capture -> frame_v{last + 2}.ppm (result=-1)",
        ])
        presented = showcase._window_vblanks(r"^HOST_PRESENT_SUBMITTED f=(\d+) ", output)
        published = showcase._window_vblanks(
            r"^FBSNAP f=\d+ swapchain capture -> frame_v(\d+)\.ppm \(result=1\)$", output
        )
        self.assertEqual(presented, {first, last})
        self.assertEqual(published, {first, last})

    def test_headless_capture_sets_only_the_capture_window(self) -> None:
        demo = showcase.DEMOS[0]
        with tempfile.TemporaryDirectory() as temp:
            package = Path(temp) / "packages" / str(demo["disc_id"])
            package.mkdir(parents=True)
            (package / "package.json").write_text(
                '{"executable": {"path": "game.exe"}, "runtime": {"run_entry": "0x08804000"}}',
                encoding="utf-8",
            )
            with mock.patch.object(showcase, "DEMO_ROOT", Path(temp)), \
                    mock.patch.dict("os.environ", {"SR_FBSNAP": "4", "SR_FBDUMP": "9"}):
                command, env, _ = showcase._headless_capture_command(demo, Path(temp) / "pad.txt")
        first, last = showcase.HEADLESS_CAPTURE_WINDOW
        self.assertEqual(command[-1], "--gui")
        self.assertEqual(env["SR_VIDEO"], "offscreen")
        self.assertEqual(env["SR_FBSNAP_WINDOWS"], f"{first}-{last}")
        # Host-speed independence: guest-driven vblanks and no wall-clock output cap.
        self.assertEqual(env["SR_NOVBPACE"], "1")
        self.assertEqual(env["SR_FPS_CAP"], "0")
        self.assertNotIn("SR_FBSNAP", env)
        self.assertNotIn("SR_FBDUMP", env)

    def test_headless_capture_pause_keeps_the_turbo_capture_route(self) -> None:
        demo = showcase.DEMOS[0]
        with tempfile.TemporaryDirectory() as temp:
            package = Path(temp) / "packages" / str(demo["disc_id"])
            package.mkdir(parents=True)
            (package / "package.json").write_text(
                '{"executable": {"path": "game.exe"}, "runtime": {"run_entry": "0x08804000"}}',
                encoding="utf-8",
            )
            with mock.patch.object(showcase, "DEMO_ROOT", Path(temp)), \
                    mock.patch.dict("os.environ", {"SR_FBSNAP": "4", "SR_FBDUMP": "9"}):
                command, env, _ = showcase._headless_capture_command(
                    demo, Path(temp) / "pad.txt", showcase.HEADLESS_PAUSE_SCRIPT)
        self.assertEqual(command[-1], "--gui")
        self.assertEqual(env["SR_NOVBPACE"], "1")
        self.assertEqual(env["SR_PAUSE_SCRIPT"], "30:500")
        self.assertEqual(env["SR_FBSNAP_WINDOWS"],
                         f"{showcase.HEADLESS_CAPTURE_WINDOW[0]}-"
                         f"{showcase.HEADLESS_CAPTURE_WINDOW[1]}")

    def test_icon_png_is_deterministic_144_by_80_rgba(self) -> None:
        palette = ((20, 40, 60), (80, 160, 200))
        first = showcase.make_icon_png(palette)
        second = showcase.make_icon_png(palette)
        self.assertEqual(first, second)
        self.assertTrue(first.startswith(b"\x89PNG\r\n\x1a\n"))
        self.assertEqual(struct.unpack_from(">II", first, 16), (144, 80))
        offset = 8
        raw = None
        while offset < len(first):
            size = struct.unpack_from(">I", first, offset)[0]
            kind = first[offset + 4:offset + 8]
            data = first[offset + 8:offset + 8 + size]
            if kind == b"IDAT":
                raw = zlib.decompress(data)
            offset += size + 12
        self.assertIsNotNone(raw)
        assert raw is not None
        self.assertEqual(len(raw), 80 * (1 + 144 * 4))

    def test_generated_iso_has_psp_tree_and_stable_synthetic_identity(self) -> None:
        elf = minimal_elf()
        icon = showcase.make_icon_png(((10, 30, 50), (70, 150, 210)))
        notice = b"PSPSDK three-clause BSD license test notice\n"
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "showcase.iso"
            showcase.write_iso(path, "TEST00007", "Nakagawa 3D Showcase", icon, elf, notice)
            first = path.read_bytes()
            showcase.write_iso(path, "TEST00007", "Nakagawa 3D Showcase", icon, elf, notice)
            self.assertEqual(path.read_bytes(), first)
            metadata = inspect_iso(path)
        self.assertEqual(metadata.disc_id, "TEST00007")
        self.assertEqual(metadata.title, "Nakagawa 3D Showcase")

    def test_iso_builder_rejects_non_elf_eboot(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(showcase.ShowcaseError, "plaintext EBOOT"):
                showcase.write_iso(Path(temp) / "bad.iso", "TEST00007", "Bad",
                                   b"icon", b"not an ELF", b"notice")

    def test_already_based_elf_is_rejected_instead_of_packaged_as_prx(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "showcase_scene_app.prx"
            path.write_bytes(minimal_elf())
            with self.assertRaisesRegex(showcase.ShowcaseError,
                                        "not a final relocatable PSP PRX"):
                showcase.validate_psp_prx(path, 0x08804000)

    def test_stage_iso_writes_a_test00007_disc_without_pspdev(self) -> None:
        """#308: the Windows staging smoke's disc needs no PSPDEV, only the display guest."""
        with tempfile.TemporaryDirectory() as temp:
            iso = showcase.stage_iso(Path(temp) / "disc")
            self.assertEqual(iso.name, "TEST00007.iso")
            metadata = inspect_iso(iso)
        self.assertEqual(metadata.disc_id, "TEST00007")
        self.assertEqual(metadata.title, "Nakagawa 3D Showcase")



class ShowcasePrebuiltPrxTests(unittest.TestCase):
    """The release route: PRXs from a PSPDEV host, packages on a host without PSPDEV."""

    LICENSE = b"PSPSDK three-clause BSD license test notice\n"

    def test_build_prx_writes_each_demo_prx_and_the_license(self) -> None:
        built: list[str] = []

        def fake_build_prx(demo: dict[str, object], out_dir: Path) -> Path:
            out_dir.mkdir(parents=True, exist_ok=True)
            prx = out_dir / f"{demo['target']}.prx"
            prx.write_bytes(minimal_elf() + str(demo["id"]).encode("ascii"))
            built.append(str(demo["id"]))
            return prx

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            out = root / "prx"
            with mock.patch.object(showcase, "BUILD_ROOT", root / "build"), \
                    mock.patch.object(showcase, "_check_pspdev") as check, \
                    mock.patch.object(showcase, "_build_prx", side_effect=fake_build_prx), \
                    mock.patch.object(showcase, "_check_prx_entry") as entry, \
                    mock.patch.object(showcase, "_pspsdk_license", return_value=self.LICENSE):
                self.assertEqual(showcase.main(["build-prx", "--out-dir", str(out)]), 0)
            check.assert_called_once_with()
            self.assertEqual(built, [str(demo["id"]) for demo in showcase.DEMOS])
            self.assertEqual(entry.call_count, len(showcase.DEMOS))
            self.assertEqual((out / showcase.PSPSDK_LICENSE_NAME).read_bytes(), self.LICENSE)
            for demo in showcase.DEMOS:
                self.assertEqual((out / f"{demo['target']}.prx").read_bytes(),
                                 minimal_elf() + str(demo["id"]).encode("ascii"))

    def _prebuilt(self, root: Path) -> Path:
        prebuilt = root / "prx"
        prebuilt.mkdir()
        for demo in showcase.DEMOS:
            (prebuilt / f"{demo['target']}.prx").write_bytes(
                minimal_elf() + str(demo["disc_id"]).encode("ascii"))
        (prebuilt / showcase.PSPSDK_LICENSE_NAME).write_bytes(self.LICENSE)
        return prebuilt

    def test_build_from_prebuilt_prxs_needs_no_pspdev(self) -> None:
        packaged: list[list[str]] = []

        environments: list[dict[str, str]] = []

        def fake_package(command: list[str], **options: object) -> subprocess.CompletedProcess:
            packaged.append(command)
            environments.append(options["env"])  # type: ignore[arg-type]
            out = Path(command[command.index("--output-dir") + 1])
            (out / "package.json").write_text(
                '{"format": "nakagawa-aot-package", "executable": {"path": "demo.exe"}}',
                encoding="utf-8")
            (out / "demo.exe").write_bytes(b"MZ")
            return subprocess.CompletedProcess(command, 0)

        refused = AssertionError("the prebuilt route must not reach PSPDEV")
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            prebuilt = self._prebuilt(root)
            demos = root / "dist" / "bin" / "demos"
            with mock.patch.object(showcase, "BUILD_ROOT", root / "showcase"), \
                    mock.patch.object(showcase, "DEMO_ROOT", demos), \
                    mock.patch.object(showcase, "_check_pspdev", side_effect=refused), \
                    mock.patch.object(showcase, "_build_prx", side_effect=refused), \
                    mock.patch.object(showcase, "_pspsdk_license", side_effect=refused), \
                    mock.patch.object(showcase, "_check_prx_entry") as entry, \
                    mock.patch.object(showcase, "trim_to_runtime_package") as trim, \
                    mock.patch.object(showcase, "_package_fingerprint", return_value="0" * 16), \
                    mock.patch.object(showcase.subprocess, "run", side_effect=fake_package):
                self.assertEqual(
                    showcase.main(["build", "--prebuilt-prx-dir", str(prebuilt)]), 0)
            self.assertEqual(entry.call_count, len(showcase.DEMOS))
            self.assertEqual(len(packaged), len(showcase.DEMOS))
            # Only the installed copy of each package is trimmed, never the build folder the
            # Android nk.titles route may read, and under the game name the planner chose.
            installed = [demos / "packages" / str(demo["disc_id"]) for demo in showcase.DEMOS]
            self.assertEqual([call.args[0] for call in trim.call_args_list], installed)
            self.assertEqual([call.args[1] for call in trim.call_args_list],
                             ["showcase-scene", "showcase-breakout"])
            for environment in environments:
                self.assertEqual(environment["RUNTIME_RUNPATH_ORIGIN"], "../../..")
            for demo, command in zip(showcase.DEMOS, packaged, strict=True):
                staged = (root / "showcase" / str(demo["id"]) / "psp-build" /
                          f"{demo['target']}.prx")
                self.assertEqual(Path(command[command.index("--game-elf") + 1]), staged)
                self.assertEqual(staged.read_bytes(), (prebuilt / staged.name).read_bytes())
                disc = str(demo["disc_id"])
                self.assertTrue((demos / "packages" / disc / "demo.exe").is_file())
                self.assertEqual(inspect_iso(demos / "images" / f"{disc}.iso").disc_id, disc)
            self.assertEqual(
                (demos / "THIRD_PARTY_NOTICES" / showcase.PSPSDK_LICENSE_NAME).read_bytes(),
                self.LICENSE)

    def test_a_missing_prebuilt_prx_or_license_is_named(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            prebuilt = self._prebuilt(root)
            (prebuilt / f"{BREAKOUT['target']}.prx").unlink()
            with self.assertRaisesRegex(showcase.ShowcaseError,
                                        "prebuilt PRX for showcase-breakout-v1 is missing"):
                showcase._prebuilt_prx(BREAKOUT, prebuilt, root / "stage")
            (prebuilt / showcase.PSPSDK_LICENSE_NAME).write_bytes(b"")
            with mock.patch.object(showcase, "_check_pspdev",
                                   side_effect=AssertionError("no PSPDEV")):
                with self.assertRaisesRegex(showcase.ShowcaseError,
                                            "PSPSDK license notice is missing or empty"):
                    showcase.build_all(prebuilt)

    def test_the_command_line_pairs_each_option_with_its_action(self) -> None:
        with mock.patch("sys.stderr"):
            with self.assertRaises(SystemExit):
                showcase.main(["build-prx"])
            with self.assertRaises(SystemExit):
                showcase.main(["smoke", "--prebuilt-prx-dir", "prx"])
        with mock.patch("sys.stderr"):
            with self.assertRaises(SystemExit):
                showcase.main(["smoke", "--target", "android"])
        with mock.patch.object(showcase, "build_all") as build:
            self.assertEqual(showcase.main(["build"]), 0)
            self.assertEqual(showcase.main(["build", "--prebuilt-prx-dir", "prx"]), 0)
            self.assertEqual(showcase.main(["build", "--target", "android"]), 0)
        self.assertEqual(build.call_args_list, [mock.call(None, "host"),
                                                mock.call(Path("prx"), "host"),
                                                mock.call(None, "android")])

    def test_android_packages_are_built_for_android_in_their_own_stage(self) -> None:
        """`build --target android` builds the packages the Android app bundles (#360): the
        planner builds lib<game>.so, in a stage of its own, and the demos folder has the
        layout the desktop showcase discovery reads (packages/<disc>, images/<disc>.iso)."""
        packaged: list[list[str]] = []

        environments: list[dict[str, str]] = []

        def fake_package(command: list[str], **options: object) -> subprocess.CompletedProcess:
            packaged.append(command)
            environments.append(options["env"])  # type: ignore[arg-type]
            out = Path(command[command.index("--output-dir") + 1])
            # What the planner leaves for an Android target: a package the check accepts, whose
            # executable is lib<game>.so and whose image is lib<game>_image.bin, with the
            # build's own files (these name the build folder) beside it.
            _synthetic_package(out.parent, executable="libdemo.so", into=out)
            return subprocess.CompletedProcess(command, 0)

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            prebuilt = self._prebuilt(root)
            demos = root / "android-demos"
            with mock.patch.object(showcase, "BUILD_ROOT", root / "showcase"), \
                    mock.patch.object(showcase, "DEMO_ROOT", demos), \
                    mock.patch.object(showcase, "_check_prx_entry"), \
                    mock.patch.object(showcase, "_package_fingerprint", return_value="0" * 16), \
                    mock.patch.object(showcase.subprocess, "run", side_effect=fake_package):
                self.assertEqual(showcase.main(["build", "--prebuilt-prx-dir", str(prebuilt),
                                                "--target", "android"]), 0)
            self.assertEqual(len(packaged), len(showcase.DEMOS))
            for demo, command in zip(showcase.DEMOS, packaged, strict=True):
                self.assertEqual(command[command.index("--target") + 1], "android")
                stage = Path(command[command.index("--output-dir") + 1])
                self.assertEqual(stage.name, "package-android-" + "0" * 16)
                disc = str(demo["disc_id"])
                installed = demos / "packages" / disc
                self.assertTrue((installed / "libdemo.so").is_file())
                self.assertEqual(inspect_iso(demos / "images" / f"{disc}.iso").disc_id, disc)
                # The Android demos folder is trimmed like the desktop one (#306): nk.demos
                # bundles exactly what the completion manifest lists and the launcher checks
                # each package as the desktop player does, so the rewritten manifest and the
                # library and image the check requires are all it needs. The build files, which
                # name the build folder, are not in it; the build stage keeps them.
                valid, reason = package_cache.validate_package_cache(installed)
                self.assertTrue(valid, reason)
                self.assertEqual(showcase.files_naming(installed, [BUILD_HOME]), [])
                self.assertTrue((installed / "libdemo_image.bin").is_file())
                self.assertEqual(sorted(path.name for path in installed.glob("*.d")), [])
                self.assertTrue((stage / "demo_recomp_0.d").is_file())
                listed = {entry["path"] for entry in json.loads(
                    (installed / "completion-manifest.json").read_text(encoding="utf-8"))["artifacts"]}
                self.assertIn("libdemo.so", listed)
                self.assertNotIn("demo_recomp_0.d", listed)
            # An Android library finds SDL3 in the APK: no run path is asked of it.
            for environment in environments:
                self.assertNotIn("RUNTIME_RUNPATH_ORIGIN", environment)


ON_LINUX = sys.platform.startswith("linux")
HAS_GCC = shutil.which("gcc") is not None


def _link_probe(directory: Path, name: str, *flags: str) -> Path:
    """A tiny executable linked with `flags`, so its loader records are a real link's."""
    source = directory / f"{name}.c"
    source.write_text("int main(void) { return 0; }\n", encoding="utf-8")
    target = directory / name
    subprocess.run(["gcc", str(source), "-o", str(target), *flags], check=True,
                   capture_output=True)
    return target


@unittest.skipUnless(ON_LINUX and HAS_GCC, "ELF run paths are a Linux link-time property")
class LinuxRunPathAuditTests(unittest.TestCase):
    """The audit that keeps a shipped Linux executable from naming the builder's folders (#306)."""

    def test_an_absolute_run_path_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            probe = _link_probe(Path(temp), "absolute", "-Wl,-rpath,/opt/builder/lib")
            needed, entries = showcase.elf_run_paths(probe)
            self.assertIn("libc.so.6", needed)
            self.assertEqual(entries, ["/opt/builder/lib"])
            problems = showcase.audit_run_paths([probe])
            self.assertEqual(len(problems), 1)
            self.assertIn("/opt/builder/lib", problems[0])

    def test_an_old_style_absolute_rpath_is_reported_too(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            probe = _link_probe(Path(temp), "rpath", "-Wl,--disable-new-dtags",
                                "-Wl,-rpath,/opt/builder/lib")
            self.assertEqual(showcase.elf_run_paths(probe)[1], ["/opt/builder/lib"])
            self.assertEqual(len(showcase.audit_run_paths([probe])), 1)

    def test_origin_relative_and_empty_run_paths_pass(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            relative = _link_probe(directory, "relative", "-Wl,-rpath,$ORIGIN/../..")
            beside = _link_probe(directory, "beside", "-Wl,-rpath,$ORIGIN")
            bare = _link_probe(directory, "bare")
            self.assertEqual(showcase.elf_run_paths(relative)[1], ["$ORIGIN/../.."])
            self.assertEqual(showcase.audit_run_paths([relative, beside, bare]), [])

    def test_a_run_path_relative_to_the_working_directory_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            probe = _link_probe(Path(temp), "cwd", "-Wl,-rpath,lib")
            self.assertEqual(len(showcase.audit_run_paths([probe])), 1)

    def test_a_file_that_is_not_an_elf_object_is_not_audited(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            text = Path(temp) / "package.json"
            text.write_text("{}\n", encoding="utf-8")
            self.assertIsNone(showcase.elf_run_paths(text))
            self.assertEqual(showcase.audit_run_paths([text]), [])

    def test_a_library_beside_the_program_is_found_from_a_moved_folder(self) -> None:
        """An $ORIGIN run path finds the library with no ldconfig entry and no wrapper."""
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            (directory / "libnkprobe.c").write_text("int nk_probe(void) { return 7; }\n",
                                                    encoding="utf-8")
            install = directory / "install"
            (install / "bin").mkdir(parents=True)
            subprocess.run(["gcc", "-shared", "-fPIC", "-Wl,-soname,libnkprobe.so.0",
                            str(directory / "libnkprobe.c"), "-o",
                            str(install / "bin" / "libnkprobe.so.0")],
                           check=True, capture_output=True)
            (directory / "main.c").write_text(
                "int nk_probe(void);\nint main(void) { return nk_probe() == 7 ? 0 : 1; }\n",
                encoding="utf-8")
            subprocess.run(["gcc", str(directory / "main.c"), "-o", str(install / "bin" / "probe"),
                            "-L", str(install / "bin"), "-l:libnkprobe.so.0",
                            "-Wl,-rpath,$ORIGIN"], check=True, capture_output=True)
            moved = directory / "moved" / "elsewhere"
            moved.parent.mkdir()
            shutil.move(str(install), str(moved))
            run = subprocess.run([str(moved / "bin" / "probe")], check=False,
                                 env={"PATH": os.environ.get("PATH", "")},
                                 capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(showcase.audit_run_paths([moved / "bin" / "probe"]), [])


def _sdl_probe(directory: Path, name: str, *flags: str) -> Path:
    """An executable that needs a libSDL3.so.0 (a stand-in library), linked with `flags`."""
    library = directory / "stub" / "libSDL3.so.0"
    if not library.exists():
        library.parent.mkdir(exist_ok=True)
        (directory / "stub.c").write_text("int SDL_Stub(void) { return 1; }\n", encoding="utf-8")
        subprocess.run(["gcc", "-shared", "-fPIC", "-Wl,-soname,libSDL3.so.0",
                        str(directory / "stub.c"), "-o", str(library)],
                       check=True, capture_output=True)
    source = directory / f"{name}.c"
    source.write_text("int SDL_Stub(void);\nint main(void) { return SDL_Stub() - 1; }\n",
                      encoding="utf-8")
    target = directory / name
    subprocess.run(["gcc", str(source), "-o", str(target), "-L", str(library.parent),
                    "-l:libSDL3.so.0", *flags], check=True, capture_output=True)
    return target


@unittest.skipUnless(ON_LINUX and HAS_GCC, "ELF run paths are a Linux link-time property")
class LinuxRequiredRunPathTests(unittest.TestCase):
    """A demo executable must search the player's folder for SDL3, not merely be well formed.

    These need only gcc, so they run in the hosted shards that have no SDL3: dropping the
    run path from the demo link line (mk/housekeeping.mk) is caught by the audit that
    `make showcase-linux` runs after the build, and the audit's own logic is held here.
    """

    REQUIRED = "$ORIGIN/" + showcase.RUNTIME_RUNPATH_ORIGIN

    def test_an_sdl3_executable_without_the_player_folder_run_path_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            none = _sdl_probe(directory, "none")
            wrong = _sdl_probe(directory, "wrong", "-Wl,-rpath,$ORIGIN")
            deeper = _sdl_probe(directory, "deeper", "-Wl,-rpath,$ORIGIN/../../../..")
            absolute = _sdl_probe(directory, "absolute", "-Wl,-rpath,/usr/local/lib")
            self.assertIn("libSDL3.so.0", showcase.elf_run_paths(none)[0])
            for probe in (none, wrong, deeper, absolute):
                with self.subTest(probe=probe.name):
                    problems = showcase.audit_required_run_path([probe], self.REQUIRED)
                    self.assertEqual(len(problems), 1)
                    self.assertIn(self.REQUIRED, problems[0])

    def test_the_player_folder_run_path_passes_and_is_found_from_a_moved_folder(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            install = directory / "an install"
            package = install / "bin" / "demos" / "packages" / "TEST00001"
            package.mkdir(parents=True)
            probe = _sdl_probe(directory, "demo", f"-Wl,-rpath,{self.REQUIRED}")
            self.assertEqual(showcase.audit_required_run_path([probe], self.REQUIRED), [])
            shutil.move(str(directory / "stub" / "libSDL3.so.0"), str(install / "bin"))
            shutil.move(str(probe), str(package / "demo"))
            run = subprocess.run([str(package / "demo")], check=False, capture_output=True,
                                 env={"PATH": os.environ.get("PATH", "")}, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)

    def test_a_file_that_does_not_need_sdl3_is_not_held_to_it(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            plain = _link_probe(Path(temp), "plain")
            data = Path(temp) / "package.json"
            data.write_text("{}\n", encoding="utf-8")
            self.assertEqual(showcase.audit_required_run_path([plain, data], self.REQUIRED), [])

    def test_the_demo_folder_audit_fails_for_a_package_without_it(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            demo_root = Path(temp) / "demos"
            package = demo_root / "packages" / "TEST00001"
            package.mkdir(parents=True)
            shutil.copy2(_sdl_probe(Path(temp), "demo"), package / "demo")
            problems = showcase.audit_demo_root(demo_root)
            self.assertEqual(len(problems), 1, problems)
            self.assertIn(self.REQUIRED, problems[0])
            shutil.copy2(_sdl_probe(Path(temp), "demo2", f"-Wl,-rpath,{self.REQUIRED}"),
                         package / "demo")
            self.assertEqual(showcase.audit_demo_root(demo_root), [])


def _arm64_library(directory: Path, name: str, *flags: str) -> Path:
    """An ELF shared object that needs libSDL3.so.0 and says it is for arm64, as the Android
    package's library is (only the ELF machine field differs from a host build)."""
    probe = _sdl_probe(directory, name + "-host", *flags)
    data = bytearray(probe.read_bytes())
    data[18:20] = (183).to_bytes(2, "little")
    target = directory / name
    target.write_bytes(bytes(data))
    return target


@unittest.skipUnless(ON_LINUX and HAS_GCC, "ELF run paths are a Linux link-time property")
class AndroidDemoAuditTests(unittest.TestCase):
    """The audit refuses a Linux demo without its run path but not a correct Android package (#360)."""

    REQUIRED = "$ORIGIN/" + showcase.RUNTIME_RUNPATH_ORIGIN

    def _folder(self, temp: str, executable: Path, name: str) -> Path:
        demo_root = Path(temp) / "demos"
        package = demo_root / "packages" / "TEST00001"
        package.mkdir(parents=True)
        shutil.copy2(executable, package / name)
        return demo_root

    def test_an_android_library_with_no_run_path_passes_the_audit(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            library = _arm64_library(Path(temp), "libdemo.so")
            self.assertTrue(showcase.is_android_library(library))
            self.assertIn("libSDL3.so.0", showcase.elf_run_paths(library)[0])
            demo_root = self._folder(temp, library, "libdemo.so")
            self.assertEqual(showcase.audit_demo_root(demo_root), [])

    def test_the_android_exemption_is_only_for_an_arm64_lib_so(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            # A desktop executable that lost its run path is refused, however it is named.
            for name in ("demo", "libdemo.so"):
                with self.subTest(name=name):
                    host = _sdl_probe(directory, "host-" + name.replace(".", "-"))
                    self.assertFalse(showcase.is_android_library(host))
                    demo_root = self._folder(temp + "/" + name.replace(".", "-"), host, name)
                    problems = showcase.audit_demo_root(demo_root)
                    self.assertEqual(len(problems), 1, problems)
                    self.assertIn(self.REQUIRED, problems[0])
            # An arm64 library not named lib<game>.so is not an Android package library either.
            odd = _arm64_library(directory, "demo")
            self.assertFalse(showcase.is_android_library(odd))
            self.assertEqual(len(showcase.audit_demo_root(self._folder(temp + "/odd", odd, "demo"))), 1)

    def test_an_android_library_is_still_held_to_the_other_rules(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            library = _arm64_library(Path(temp), "libdemo.so", "-Wl,-rpath,/opt/builder/lib")
            problems = showcase.audit_demo_root(self._folder(temp, library, "libdemo.so"))
            self.assertEqual(len(problems), 1, problems)
            self.assertIn("/opt/builder/lib", problems[0])
            demo_root = Path(temp) / "demos"
            (demo_root / "packages" / "TEST00001" / "build.log").write_text(
                "built in " + str(showcase.BUILD_ROOT) + "\n", encoding="utf-8")
            self.assertTrue(any("build.log" in problem
                                for problem in showcase.audit_demo_root(demo_root)))


def _make_dry_run(*arguments: str) -> str:
    result = subprocess.run(["make", "-n", "-B", *arguments], cwd=ROOT, check=False,
                            capture_output=True, text=True)
    return result.stdout.replace("\\\n", " ") + result.stderr


def _has_system_sdl3() -> bool:
    if not (ON_LINUX and shutil.which("make") and shutil.which("pkg-config")):
        return False
    return subprocess.run(["pkg-config", "--exists", "sdl3"], check=False,
                          capture_output=True).returncode == 0


@unittest.skipUnless(_has_system_sdl3(), "needs make and a system SDL3")
class LinuxLinkLineTests(unittest.TestCase):
    """The link lines write a relative run path and never a developer folder (#306)."""

    def test_the_makefile_player_runs_from_the_folder_it_is_installed_in(self) -> None:
        output = _make_dry_run("player", "CC=gcc", f"PYTHON={sys.executable}")
        links = [line for line in output.splitlines()
                 if " -o build/nakagawa_player" in line and "-lSDL3" in line]
        self.assertEqual(len(links), 1, output[-2000:])
        self.assertIn("-Wl,-rpath,'$ORIGIN'", links[0])
        self.assertNotRegex(links[0], r"-rpath,'?/")

    def test_the_title_runtime_link_takes_a_relative_run_path_from_the_caller(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            build = (Path(temp) / "build").as_posix()
            common = ["compile", "CC=gcc", f"PYTHON={sys.executable}", "GAME_NAME=probe",
                      f"GAME_ELF={temp}/probe.prx", f"BUILD_DIR={build}"]
            marker = f"-o {build}/probe "
            links = [line for line in _make_dry_run(*common, "RUNTIME_RUNPATH_ORIGIN=../../..")
                     .splitlines() if marker in line]
            self.assertEqual(len(links), 1)
            self.assertIn("-Wl,-rpath,'$ORIGIN/../../..'", links[0])
            plain = [line for line in _make_dry_run(*common).splitlines() if marker in line]
            self.assertEqual(len(plain), 1)
            self.assertNotIn("-rpath", plain[0],
                             "no caller, no run path: a title build is unchanged")

    @unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "needs cmake and ninja")
    def test_the_cmake_player_has_only_an_origin_relative_run_path(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            build = Path(temp) / "cmake"
            configured = subprocess.run(
                ["cmake", "-S", str(ROOT), "-B", str(build), "-G", "Ninja", "-DBUILD_PLAYER=ON",
                 "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=gcc"],
                check=False, capture_output=True, text=True)
            self.assertEqual(configured.returncode, 0,
                             configured.stdout[-1500:] + configured.stderr[-1500:])
            commands = subprocess.run(
                ["ninja", "-C", str(build), "-t", "commands", "nakagawa_player"],
                check=False, capture_output=True, text=True)
            links = [line for line in commands.stdout.splitlines()
                     if " -o nakagawa_player" in line and " -lm" in line]
            self.assertEqual(len(links), 1, commands.stdout[-1500:])
            # Ninja prints the entry quoted for the shell: -Wl,-rpath,"\$ORIGIN".
            self.assertRegex(links[0], r"""-Wl,-rpath,["']?\\?\$ORIGIN["']?(\s|$)""")
            self.assertEqual(len(re.findall(r"-Wl,-rpath", links[0])), 1)
            self.assertNotRegex(links[0], r"""-Wl,-rpath,["']?/""")


BUILD_HOME = "/srv/builder/nk-build-7f3a"


def _synthetic_package(root: Path, build_dir: str = BUILD_HOME, executable: str = "demo",
                       into: Path | None = None) -> Path:
    """A runtime package the way the planner leaves it: runtime files and the build's own.

    The `.d`, `.o` and `.map` files carry the folder the build ran in, as a compiler and
    linker write it, and the completion manifest lists every file, as the planner's does.
    """
    inputs: dict[str, Any] = {"manifest": {"sha256": "1" * 64}, "executable": {"sha256": "2" * 64},
                              "modules": [], "psp_header": None}
    identity = package_cache.build_title_input_identity(
        manifest={"id": "synthetic-cache-test", "schema_version": 1},
        executable_name="EBOOT.BIN", executable_sha256="2" * 64, modules=[],
        disc_id="TEST00001", region="TEST", disc_version="1.00",
        param_sfo={"DISC_ID": "TEST00001", "DISC_VERSION": "1.00"},
        container={"format": "iso9660", "volume_id": "CACHE_TEST", "size_bytes": 2048,
                   "pvd_sector": 16, "sector_size": 2048})
    key = package_cache.build_cache_key(
        input_hashes=inputs, title_input_identity=identity,
        codegen_options={"profile": "none", "funcs_per_chunk": 2000},
        analyzer_sha256="3" * 64, codegen_sha256="4" * 64, compiler="gcc:test",
        target="x86_64-test", runtime_source_digest="5" * 64, compile_flags="-O0",
        link_flags="-lm")
    cache = package_cache.cache_metadata(key, {"profile": "none", "funcs_per_chunk": 2000})
    package_dir = into if into is not None else root / "package"
    package_dir.mkdir(exist_ok=True)
    (package_dir / executable).write_bytes(b"\x7fELF native")
    (package_dir / f"{Path(executable).stem}_image.bin").write_bytes(b"image")
    (package_dir / "demo_recomp_0.c").write_text("/* generated */\n", encoding="utf-8")
    (package_dir / "demo_recomp_0.o").write_bytes(
        b"\x7fELF\0" + f"{build_dir}/demo_recomp_0.c".encode() + b"\0")
    (package_dir / "demo_recomp_0.d").write_text(
        f"{build_dir}/demo_recomp_0.o: {build_dir}/demo_recomp_0.c\n", encoding="utf-8")
    (package_dir / "demo.map").write_text(f"LOAD {build_dir}/demo_recomp_0.o\n", encoding="utf-8")
    (package_dir / "link-objects.rsp").write_text(f"{build_dir}/demo_recomp_0.o\n",
                                                  encoding="utf-8")
    (package_dir / ".game-inputs").write_text(f"{build_dir}\n", encoding="utf-8")
    (package_dir / ".runtime-profile-0123456789abcdef").write_text("stamp\n", encoding="utf-8")
    (package_dir / "sr_title_config.h").write_text("#define X 1\n", encoding="utf-8")
    (package_dir / "demo_recomp_funcs.h").write_text("/* funcs */\n", encoding="utf-8")
    (package_dir / "nk_lib_0.c").write_text("/* library */\n", encoding="utf-8")
    (package_dir / "demo_imports.toml").write_text("# imports\n", encoding="utf-8")
    (package_dir / "demo_recomp_coverage.json").write_text("{}\n", encoding="utf-8")
    (package_dir / "portable-core").mkdir()
    (package_dir / "h264sub_libavcodec").mkdir()
    (package_dir / "h264sub_libavcodec" / "h264.o").write_bytes(b"\x7fELF\0" + build_dir.encode())
    package = {
        "format": "nakagawa-aot-package", "schema_version": 3,
        "title": {"id": identity["manifest"]["id"], "manifest_sha256": "1" * 64},
        "title_input_identity": identity, "cache": cache, "inputs": inputs,
        "executable": {"path": executable,
                       "sha256": package_cache.sha256_file(package_dir / executable)},
        "generated_objects": [],
    }
    (package_dir / "package.json").write_text(package_cache.canonical_json(package),
                                              encoding="utf-8")
    (package_dir / "build-report.json").write_text(package_cache.canonical_json({"cache": cache}),
                                                   encoding="utf-8")
    package_cache.write_completion_manifest(package_dir, key, title_input_identity=identity,
                                             backends="public", limits=["synthetic"])
    valid, reason = package_cache.validate_package_cache(package_dir, expected_key=key)
    assert valid, reason
    return package_dir


class ShowcaseRuntimeOnlyPackageTests(unittest.TestCase):
    """A showcase package ships its runtime files only, as a package the player builds does (#306)."""

    def test_the_planner_output_names_the_build_folder_in_files_the_manifest_lists(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            package_dir = _synthetic_package(Path(temp))
            leaks = showcase.files_naming(package_dir, [BUILD_HOME])
            self.assertIn("demo_recomp_0.d", leaks)
            self.assertIn("demo.map", leaks)

    def test_trimming_leaves_a_valid_package_that_names_no_build_folder(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            package_dir = _synthetic_package(Path(temp))
            showcase.trim_to_runtime_package(package_dir, "demo")
            self.assertEqual(showcase.files_naming(package_dir, [BUILD_HOME]), [])
            kept = sorted(path.name for path in package_dir.iterdir())
            # The runtime files and the generated C that the title runtime route compiles;
            # no build file, no empty folder.
            self.assertEqual(kept, ["build-report.json", "completion-manifest.json", "demo",
                                    "demo_image.bin", "demo_recomp_0.c", "demo_recomp_funcs.h",
                                    "nk_lib_0.c", "package.json", "sr_title_config.h"])
            valid, reason = package_cache.validate_package_cache(package_dir)
            self.assertTrue(valid, reason)

    def test_a_trimmed_package_still_validates_from_another_folder(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            package_dir = _synthetic_package(Path(temp))
            showcase.trim_to_runtime_package(package_dir, "demo")
            elsewhere = Path(temp) / "moved" / "to somewhere"
            elsewhere.parent.mkdir()
            shutil.move(str(package_dir), str(elsewhere))
            valid, reason = package_cache.validate_package_cache(elsewhere)
            self.assertTrue(valid, reason)

    def test_the_check_still_refuses_a_changed_a_missing_or_an_extra_file(self) -> None:
        for damage, expected in (
            (lambda root: (root / "demo").write_bytes(b"\x7fELF changed"), "digest"),
            (lambda root: (root / "demo_image.bin").write_bytes(b"other image"), "digest"),
            (lambda root: (root / "demo_image.bin").unlink(), "image is missing"),
            (lambda root: (root / "demo").unlink(), "executable"),
            (lambda root: (root / "extra.bin").write_bytes(b"unlisted"), "artifact set"),
        ):
            with self.subTest(expected=expected), tempfile.TemporaryDirectory() as temp:
                package_dir = _synthetic_package(Path(temp))
                showcase.trim_to_runtime_package(package_dir, "demo")
                damage(package_dir)
                valid, reason = package_cache.validate_package_cache(package_dir)
                self.assertFalse(valid)
                self.assertIn(expected, reason)

    def test_trimming_does_not_hide_a_package_that_was_already_damaged(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            package_dir = _synthetic_package(Path(temp))
            (package_dir / "demo_image.bin").unlink()
            with self.assertRaises(showcase.ShowcaseError):
                showcase.trim_to_runtime_package(package_dir, "demo")

    def test_the_title_runtime_route_still_finds_its_generated_sources(self) -> None:
        """`nk_add_title_runtime` (the Android nk.titles route) compiles the package it is given.

        The file patterns are read from the CMake function itself, so the test follows the
        route and not a copy of it, and the trimmed package must still match them.
        """
        text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        block = re.search(r"file\(GLOB generated CONFIGURE_DEPENDS(.*?)\)", text, re.S)
        self.assertIsNotNone(block, "nk_add_title_runtime no longer globs its generated sources")
        assert block is not None
        patterns = re.findall(r'"\$\{package_dir\}/([^"]+)"', block.group(1))
        self.assertTrue(patterns)
        with tempfile.TemporaryDirectory() as temp:
            package_dir = _synthetic_package(Path(temp))
            showcase.trim_to_runtime_package(package_dir, "demo")
            found = sorted(path.name for pattern in patterns
                           for path in package_dir.glob(pattern.replace("${title_id}", "demo")))
            self.assertEqual(found, ["demo_recomp_0.c", "nk_lib_0.c"])
            self.assertTrue((package_dir / "sr_title_config.h").is_file())
            self.assertEqual(showcase.files_naming(package_dir, [BUILD_HOME]), [])

    def test_the_build_files_of_a_library_named_package_are_covered(self) -> None:
        """A package whose executable is `lib<game>.so` still loses <game>_imports.toml and
        <game>_recomp_coverage.json: the trim takes the game name the planner chose, which
        the executable's file name does not give."""
        for demo in showcase.DEMOS:
            name = showcase.package_game_name(demo)
            self.assertFalse(name.startswith("lib"))
            self.assertEqual(name, Path(str(demo["manifest"])).stem)
        with tempfile.TemporaryDirectory() as temp:
            package_dir = _synthetic_package(Path(temp))
            showcase.trim_to_runtime_package(package_dir, "demo")
            self.assertFalse((package_dir / "demo_imports.toml").exists())
            self.assertFalse((package_dir / "demo_recomp_coverage.json").exists())
        with tempfile.TemporaryDirectory() as temp:
            package_dir = _synthetic_package(Path(temp))
            showcase.trim_to_runtime_package(package_dir, "libdemo")  # the executable's stem
            self.assertTrue((package_dir / "demo_imports.toml").exists())

    def test_trimming_removes_the_folders_it_empties(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            package_dir = _synthetic_package(Path(temp))
            self.assertTrue((package_dir / "portable-core").is_dir())
            showcase.trim_to_runtime_package(package_dir, "demo")
            self.assertEqual([path.name for path in package_dir.rglob("*") if path.is_dir()], [])

    def test_trimming_the_installed_copy_leaves_the_build_folder_whole(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            build = _synthetic_package(Path(temp))
            before = sorted(path.name for path in build.rglob("*"))
            installed = Path(temp) / "installed"
            shutil.copytree(build, installed)
            showcase.trim_to_runtime_package(installed, "demo")
            self.assertEqual(sorted(path.name for path in build.rglob("*")), before)
            valid, reason = package_cache.validate_package_cache(build)
            self.assertTrue(valid, reason)
            self.assertIn("demo_recomp_0.d", before)

    def test_the_audit_names_a_file_that_holds_the_home_folder(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            demo_root = Path(temp) / "demos"
            (demo_root / "packages" / "TEST00001").mkdir(parents=True)
            note = demo_root / "packages" / "TEST00001" / "build.log"
            note.write_text("linked in " + str(Path.home()) + "/work\n", encoding="utf-8")
            problems = showcase.audit_demo_root(demo_root)
            self.assertEqual(len(problems), 1, problems)
            self.assertIn("build.log", problems[0])
            note.write_text("nothing of the builder\n", encoding="utf-8")
            self.assertEqual(showcase.audit_demo_root(demo_root), [])

    def test_trimming_a_package_never_touches_files_outside_it(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            outside = Path(temp) / "neighbour.o"
            outside.write_bytes(b"keep")
            package_dir = _synthetic_package(Path(temp))
            showcase.trim_to_runtime_package(package_dir, "demo")
            self.assertEqual(outside.read_bytes(), b"keep")


@unittest.skipUnless(ON_LINUX and shutil.which("gcc") and _has_system_sdl3(),
                     "needs Linux, gcc and a system SDL3")
class ShowcaseRelocationTests(unittest.TestCase):
    """The real showcase packages, built in one folder and used from another (#306).

    The packages come from the demo folder `showcase.py build` already wrote (for example by
    `make showcase-linux`, which runs the build, `showcase.py audit` and the smoke, but not this
    test), or are built here when NK_SHOWCASE_RELOCATION_BUILD=1 and PSPDEV is installed.
    Without either the test skips: a PSPDEV build takes minutes and is not started by an
    ordinary test run, and no hosted job runs it. What a hosted job holds for the demo link
    is `showcase.py audit` (the run-path tests above and the audit in `make showcase-linux`).
    """

    def _built_demos(self, root: Path) -> tuple[Path, Path]:
        """The demo folder to use and the build folder its files must not name."""
        if os.environ.get("NK_SHOWCASE_RELOCATION_BUILD") == "1":
            if not shutil.which("psp-gcc"):
                self.skipTest("NK_SHOWCASE_RELOCATION_BUILD=1 needs PSPDEV (psp-gcc) on PATH")
            built = root / "build-site" / "demos"
            work = root / "build-site" / "work"
            with (mock.patch.object(showcase, "BUILD_ROOT", work),
                  mock.patch.object(showcase, "DEMO_ROOT", built)):
                showcase.build_all()
            return built, work
        if not (showcase.DEMO_ROOT / "packages").is_dir():
            self.skipTest("no built demo folder; run `showcase.py build` first")
        return showcase.DEMO_ROOT, showcase.BUILD_ROOT

    def test_packages_built_in_one_folder_validate_and_run_from_another(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nk-reloc-") as temp:
            root = Path(temp)
            built, work = self._built_demos(root)
            moved = root / "an install" / "bin" / "demos"
            moved.parent.mkdir(parents=True)
            shutil.copytree(built, moved, symlinks=True)
            needles = [str(ROOT), str(built), str(work), str(Path.home())]
            self.assertEqual(showcase.files_naming(moved, needles), [])
            executables = []
            for demo in showcase.DEMOS:
                package_dir = moved / "packages" / str(demo["disc_id"])
                # A smoke run in the build folder leaves its frame captures beside the
                # package files. They are no part of the package (and were scanned above),
                # so the validation sees the package as it was built.
                listed = {entry["path"] for entry in json.loads(
                    (package_dir / "completion-manifest.json").read_text(encoding="utf-8"))["artifacts"]}
                for found in sorted(package_dir.rglob("*")):
                    relative = found.relative_to(package_dir).as_posix()
                    if found.is_file() and relative not in listed and relative != "completion-manifest.json":
                        found.unlink()
                valid, reason = package_cache.validate_package_cache(package_dir)
                self.assertTrue(valid, f"{demo['disc_id']}: {reason}")
                executables.append(package_dir / json.loads(
                    (package_dir / "package.json").read_text(encoding="utf-8"))["executable"]["path"])
            self.assertEqual(showcase.audit_run_paths(executables), [])
            # The player folder holds the SDL3 library (the install's bin/, where a Windows
            # install holds SDL3.dll). With every system copy hidden, each demo finds it
            # through its own run path, from the moved folder, with no wrapper.
            copies = showcase.system_library_files("libSDL3.so")
            self.assertTrue(copies, "no system SDL3 to copy beside the player")
            shutil.copy2(copies[0], moved.parent / "libSDL3.so.0")
            for executable in executables:
                try:
                    unresolved = showcase.unresolved_libraries(executable)
                except showcase.ShowcaseError as exc:
                    self.skipTest(str(exc))
                self.assertEqual(unresolved, [], executable.name)
            # A change to a file the manifest lists is still refused after the move.
            victim = executables[0]
            original = victim.read_bytes()
            victim.write_bytes(original + b"\0")
            valid, reason = package_cache.validate_package_cache(victim.parent)
            victim.write_bytes(original)
            self.assertFalse(valid)
            self.assertIn("digest", reason)
            image = victim.parent / f"{victim.stem}_image.bin"
            image.rename(image.with_suffix(".moved"))
            valid, reason = package_cache.validate_package_cache(victim.parent)
            image.with_suffix(".moved").rename(image)
            self.assertFalse(valid)
            # Both demos run from the moved folder and present real frames there, captured
            # through the runtime's own SR_FBSNAP_WINDOWS facility.
            smoke = root / "smoke"
            smoke.mkdir()
            with (mock.patch.object(showcase, "BUILD_ROOT", root / "work"),
                  mock.patch.object(showcase, "DEMO_ROOT", moved)):
                for demo in showcase.DEMOS:
                    self.assertTrue(showcase._smoke_headless_capture(demo, None, smoke))


if __name__ == "__main__":
    unittest.main()
