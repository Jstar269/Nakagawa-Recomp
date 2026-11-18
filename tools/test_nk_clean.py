# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Preview/allowlist safety contracts for tools/nk_clean.py (#368).

Every test here builds a synthetic workspace in a temporary directory only.
The invariants pinned are: dry-run deletes nothing; only allowlisted,
untracked, in-root files are ever selected; protected targets planted behind a
symlink or Windows junction inside an allowlisted root survive; age bounding
still previews first; and the Makefile / nk.ps1 entry points stay wired to the
same tool.
"""

from __future__ import annotations

import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

import nk_clean  # noqa: E402
from makefile_text import read_makefile  # noqa: E402


def _write(path: Path, data: bytes = b"x") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


def _make_workspace(base: Path) -> Path:
    root = base / "ws"
    for marker in nk_clean.WORKSPACE_ANCHORS:
        _write(root / marker)
    return root


def _plant(root: Path) -> dict[str, Path]:
    planted: dict[str, Path] = {}
    _write(root / "build" / "game.o", b"abcd")
    _write(root / "build" / "sub" / "dir" / "file.bin", b"efghij")
    planted["tracked"] = _write(root / "build" / "keep_me.txt", b"tracked")
    _write(root / "logs" / "build_out_recomp.log", b"log1")
    _write(root / "logs" / "recomp_err.log", b"log2")
    planted["private_log"] = _write(root / "logs" / "oracle_run.log", b"secret run")
    planted["key"] = _write(root / "keys" / "secret.bin", b"key")
    planted["input"] = _write(root / "place_game_here" / "input.bin", b"input")
    _write(root / "scratch" / "stale.tmp", b"tmp")
    _write(root / "compile_commands.json", b"[]")
    return planted


def _run_main(root: Path, argv: list[str]) -> tuple[int, str, str]:
    """Run the CLI entry point against a synthetic workspace with nothing git-tracked."""
    out, err = io.StringIO(), io.StringIO()
    with mock.patch.object(nk_clean, "REPO_ROOT", root), \
            mock.patch.object(nk_clean, "git_tracked", return_value=frozenset()), \
            contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = nk_clean.main(argv)
    return code, out.getvalue(), err.getvalue()


def _make_dir_link(link: Path, target: Path) -> bool:
    link.parent.mkdir(parents=True, exist_ok=True)
    if os.name == "nt":
        proc = subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(link), str(target)],
            capture_output=True, text=True, check=False,
        )
        return proc.returncode == 0
    try:
        os.symlink(target, link, target_is_directory=True)
    except OSError:
        return False
    return True


class DryRunTests(unittest.TestCase):
    def test_dry_run_deletes_nothing_and_renders_grouped_sizes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = _make_workspace(Path(tmp))
            planted = _plant(root)

            preview = nk_clean.plan_clean(root, tracked=frozenset())
            self.assertFalse(preview.errors, preview.errors)
            text = nk_clean.render(preview, confirm=False)

            self.assertIn("DRY-RUN", text)
            for category in ("build outputs", "logs", "tool scratch"):
                self.assertIn(f"[{category}]", text)
            self.assertIn("build/game.o", text)
            self.assertIn(nk_clean.format_size(4), text)
            self.assertIn("No files were deleted", text)
            for path in planted.values():
                self.assertTrue(path.exists(), f"dry-run touched {path}")
            self.assertTrue((root / "build" / "game.o").exists())

    def test_only_yes_confirms_and_an_inherited_confirm_variable_stays_a_dry_run(self) -> None:
        """A shell or CI job that exports CONFIRM=1 for another tool must not turn the plain
        invocation, which the docs present as the dry run, into a deletion."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _make_workspace(Path(tmp))
            built = _write(root / "build" / "game.o", b"abcd")
            with mock.patch.dict(os.environ, {"CONFIRM": "1"}):
                code, out, _err = _run_main(root, [])
            self.assertEqual(code, 0, out)
            self.assertIn("DRY-RUN", out)
            self.assertTrue(built.exists(), "an inherited CONFIRM=1 deleted the plan")
            code, out, _err = _run_main(root, ["--yes"])
            self.assertEqual(code, 0, out)
            self.assertFalse(built.exists(), "--yes must still apply the plan")

    def test_a_planning_failure_is_not_reported_as_a_git_failure(self) -> None:
        """git ls-files and the plan fail for different reasons; each says its own."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _make_workspace(Path(tmp))
            built = _write(root / "build" / "game.o", b"abcd")
            with mock.patch.object(nk_clean.os.path, "realpath",
                                   side_effect=PermissionError(13, "denied")):
                code, out, err = _run_main(root, ["--yes"])
            self.assertEqual(code, 1, out)
            self.assertIn("nk_clean: cannot plan the clean:", err)
            self.assertNotIn("git ls-files", err)
            self.assertTrue(built.exists())

            with mock.patch.object(nk_clean, "REPO_ROOT", root), \
                    mock.patch.object(nk_clean, "git_tracked",
                                      side_effect=OSError("not a git repository")), \
                    contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(io.StringIO()) as git_err:
                self.assertEqual(nk_clean.main(["--yes"]), 1)
            self.assertIn("nk_clean: git ls-files failed: not a git repository",
                          git_err.getvalue())
            self.assertTrue(built.exists())


class RenderTests(unittest.TestCase):
    def test_confirmed_run_with_scan_errors_says_nothing_is_deleted(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = _make_workspace(Path(tmp))
            _write(root / "build" / "game.o", b"abcd")
            preview = nk_clean.plan_clean(root, tracked=frozenset())
            preview.errors.append(("build/broken", "scan failed: denied"))
            text = nk_clean.render(preview, confirm=True)
            self.assertIn("REFUSED", text)
            self.assertIn("no files will be deleted", text)
            self.assertIn("No files were deleted", text)
            self.assertNotIn(f"{nk_clean.format_size(4)} will be deleted", text)
            self.assertNotIn("CLEAN (confirmed)", text)
            self.assertNotIn("selected for deletion", text)

    def test_skip_reasons_are_printed_with_the_kept_title_outputs_by_name(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = _make_workspace(Path(tmp))
            _write(root / "build" / "hst" / "hst_recomp.c")
            _write(root / "logs" / "stdout_run.log")
            _write(root / "build" / "keep_me.txt")
            preview = nk_clean.plan_clean(root, tracked=frozenset({"build/keep_me.txt"}))
            text = nk_clean.render(preview, confirm=False)
            self.assertIn("1 x title output; pass --include-title-outputs to clean it", text)
            self.assertIn("    build/hst\n", text)
            self.assertIn("1 x title run log; pass --include-title-outputs to clean it", text)
            self.assertIn("    logs/stdout_run.log\n", text)
            self.assertIn("1 x git-tracked; protected", text)
            self.assertNotIn("    build/keep_me.txt", text, "only kept title outputs are listed")
            self.assertIn("INCLUDE_TITLE=1", text)

    def test_age_bound_line_reports_the_excluded_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = _make_workspace(Path(tmp))
            _write(root / "build" / "fresh.o", b"x" * 2048)
            preview = nk_clean.plan_clean(root, tracked=frozenset(), older_than_days=7)
            self.assertEqual(preview.age_excluded_bytes, 2048)
            text = nk_clean.render(preview, confirm=False)
            self.assertIn(f"1 newer file(s), {nk_clean.format_size(2048)} excluded", text)


class NeverPruneTests(unittest.TestCase):
    def test_the_fixture_tree_is_never_pruned_where_it_lives(self) -> None:
        """Deleting the oracle build output must not prune the tracked fixture tree above it."""
        self.assertIn("tests/fixtures", nk_clean.NEVER_PRUNE)
        self.assertIn("tests/fixtures/psp_oracle/build", nk_clean.NEVER_PRUNE)
        self.assertNotIn("fixtures", nk_clean.NEVER_PRUNE, "the root folder the tree moved from")

    def test_no_allowlist_rule_starts_in_a_protected_component(self) -> None:
        """PROTECTED_COMPONENTS holds because no rule can scan or select inside one."""
        for rule in nk_clean.TREE_RULES + nk_clean.FILE_RULES:
            with self.subTest(rule=rule.rel):
                self.assertNotIn(rule.rel.split("/", 1)[0], nk_clean.PROTECTED_COMPONENTS)

    def test_pruning_removes_every_emptied_directory_and_keeps_rule_roots(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = _make_workspace(Path(tmp))
            for index in range(3):
                _write(root / "build" / "synthetic" / "b" / f"c{index}" / "f.o")
                _write(root / "build" / "synthetic" / f"d{index}.o")
            _write(root / "build" / "test" / "kept.txt")
            preview = nk_clean.plan_clean(root, tracked=frozenset({"build/test/kept.txt"}))
            self.assertEqual(len(preview.files), 6)
            self.assertEqual(nk_clean.execute(preview), [])
            self.assertFalse((root / "build" / "synthetic").exists())
            self.assertTrue((root / "build" / "test" / "kept.txt").exists())
            self.assertTrue((root / "build").is_dir())


class SelectionTests(unittest.TestCase):
    def test_confirmed_delete_removes_exactly_the_previewed_set(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = _make_workspace(Path(tmp))
            _plant(root)
            tracked = frozenset({"build/keep_me.txt"})

            preview = nk_clean.plan_clean(root, tracked=tracked, include_title_outputs=True)
            selected = {f.rel for f in preview.files}
            self.assertEqual(
                selected,
                {
                    "build/game.o",
                    "build/sub/dir/file.bin",
                    "logs/build_out_recomp.log",
                    "logs/recomp_err.log",
                    "scratch/stale.tmp",
                    "compile_commands.json",
                },
            )
            self.assertNotIn("build/keep_me.txt", selected)
            self.assertNotIn("logs/oracle_run.log", selected)

            failures = nk_clean.execute(preview)
            self.assertEqual(failures, [])

            for rel in selected:
                self.assertFalse((root / rel).exists(), f"{rel} was previewed but not deleted")
            self.assertTrue((root / "build" / "keep_me.txt").exists(), "tracked file deleted")
            self.assertTrue((root / "logs" / "oracle_run.log").exists(), "private-run log deleted")
            self.assertTrue((root / "keys" / "secret.bin").exists(), "protected input deleted")
            self.assertTrue((root / "place_game_here" / "input.bin").exists(), "private input deleted")
            self.assertTrue((root / "build").exists(), "allowlisted root directory itself must remain")
            self.assertFalse((root / "build" / "sub").exists(), "emptied subdirectory should be pruned")
            self.assertTrue((root / "scratch").exists(), "rule roots are not removed")

    def test_older_than_bounds_the_selection_and_still_previews(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = _make_workspace(Path(tmp))
            _plant(root)
            old = _write(root / "build" / "old.o", b"old-object")
            now = time.time()
            stale = now - 10 * 86400
            os.utime(old, (stale, stale))

            preview = nk_clean.plan_clean(
                root, tracked=frozenset({"build/keep_me.txt"}), older_than_days=7, now=now,
                include_title_outputs=True,
            )
            selected = {f.rel for f in preview.files}
            self.assertIn("build/old.o", selected)
            self.assertNotIn("build/game.o", selected, "fresh file must be age-excluded")
            self.assertGreaterEqual(preview.age_excluded, 1)
            text = nk_clean.render(preview, confirm=False)
            self.assertIn("Age bound", text)

            failures = nk_clean.execute(preview)
            self.assertEqual(failures, [])
            self.assertFalse(old.exists())
            self.assertTrue((root / "build" / "game.o").exists(), "fresh file deleted by age-bounded run")


class TitleOutputTests(unittest.TestCase):
    def test_title_outputs_and_run_logs_need_explicit_opt_in(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = _make_workspace(Path(tmp))
            _write(root / "build" / "hst" / "hst_recomp.c")          # private title output
            _write(root / "build" / "mygame" / "recomp.o")           # public default build
            _write(root / "build" / "synthetic2" / "x.o")            # public fixture build
            _write(root / "logs" / "stdout_run.log")                 # title run log
            _write(root / "logs" / "link_err.log")                   # ordinary build log
            default = nk_clean.plan_clean(root, tracked=frozenset())
            chosen = {entry.rel for entry in default.files}
            self.assertNotIn("build/hst/hst_recomp.c", chosen)
            self.assertNotIn("logs/stdout_run.log", chosen)
            self.assertIn("build/mygame/recomp.o", chosen)
            self.assertIn("build/synthetic2/x.o", chosen)
            self.assertIn("logs/link_err.log", chosen)
            skipped = {entry.rel for entry in default.skipped}
            self.assertIn("build/hst", skipped)
            opted = nk_clean.plan_clean(root, tracked=frozenset(), include_title_outputs=True)
            chosen_opted = {entry.rel for entry in opted.files}
            self.assertIn("build/hst/hst_recomp.c", chosen_opted)
            self.assertIn("logs/stdout_run.log", chosen_opted)

    def test_default_build_dir_holding_a_title_build_needs_opt_in(self) -> None:
        """build/mygame is where a title built without its own GAME_NAME lands (docs/PORTING.md)."""
        for output in ("mygame_recomp.c", "mygame_recomp_3.c", "mygame_recomp_funcs.h",
                       "mygame_image.bin", "mygame_imports.toml"):
            with self.subTest(output=output), tempfile.TemporaryDirectory() as tmp:
                root = _make_workspace(Path(tmp))
                _write(root / "build" / "mygame" / output)
                _write(root / "build" / "mygame" / "mygame.exe")
                default = nk_clean.plan_clean(root, tracked=frozenset())
                chosen = {entry.rel for entry in default.files}
                self.assertNotIn(f"build/mygame/{output}", chosen)
                self.assertNotIn("build/mygame/mygame.exe", chosen)
                self.assertIn("build/mygame", {entry.rel for entry in default.skipped})
                opted = nk_clean.plan_clean(root, tracked=frozenset(), include_title_outputs=True)
                self.assertIn(f"build/mygame/{output}", {entry.rel for entry in opted.files})

    def test_public_build_families_match_only_at_a_separator(self) -> None:
        """A title whose name merely starts like a public build family stays a title output."""
        for name in ("synthetic", "synthetic2", "synthetic_title2", "test", "test_nk_zip",
                     "platform-ladder", "production-smoke", "production-smoke-gap",
                     "production_smoke", "pspdev_phase5", "nakagawa_player",
                     "nakagawa_player_ui_test", "cosim", "mygame"):
            with self.subTest(public=name):
                self.assertTrue(nk_clean._is_public_build_dir(name))
        for name in ("testdrive", "psmfplayer", "production_title", "productionx",
                     "syntheticworld", "pspdevil", "nakagawa_playerx"):
            with self.subTest(title=name):
                self.assertFalse(nk_clean._is_public_build_dir(name))
        with tempfile.TemporaryDirectory() as tmp:
            root = _make_workspace(Path(tmp))
            _write(root / "build" / "testdrive" / "testdrive.exe")
            default = nk_clean.plan_clean(root, tracked=frozenset())
            self.assertNotIn("build/testdrive/testdrive.exe", {e.rel for e in default.files})
            self.assertIn("build/testdrive", {e.rel for e in default.skipped})

    def test_title_run_logs_are_marked_on_their_rules(self) -> None:
        marked = {rule.rel for rule in nk_clean.FILE_RULES if rule.title_output}
        self.assertEqual(marked, {"logs/stdout_run.log", "logs/stderr_run.log"})

    def test_title_run_frame_captures_need_opt_in(self) -> None:
        """build/snapshots holds the frames a (possibly private) title run's SR_FBSNAP wrote."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _make_workspace(Path(tmp))
            _write(root / "build" / "snapshots" / "frame_0001.ppm")
            default = nk_clean.plan_clean(root, tracked=frozenset())
            self.assertNotIn("build/snapshots/frame_0001.ppm", {entry.rel for entry in default.files})
            self.assertIn("build/snapshots", {entry.rel for entry in default.skipped})
            opted = nk_clean.plan_clean(root, tracked=frozenset(), include_title_outputs=True)
            self.assertIn("build/snapshots/frame_0001.ppm", {entry.rel for entry in opted.files})

    def test_psp_run_records_in_the_oracle_host0_root_need_opt_in(self) -> None:
        """A host0 root under tests/fixtures/psp_oracle/build keeps a PSP run's report and log."""
        with tempfile.TemporaryDirectory() as tmp:
            root = _make_workspace(Path(tmp))
            oracle = root / "tests" / "fixtures" / "psp_oracle" / "build"
            _write(oracle / "probe.o")
            _write(oracle / "nakagawa_psp_oracle.elf")
            _write(oracle / "EBOOT.PBP")
            _write(oracle / "build-config.stamp")
            _write(oracle / "w6-size-bfff" / "dmac_size_matrix_cell.prx")
            _write(oracle / "w6-size-bfff" / "m1-01-dmac-boot3.json")         # runner report
            _write(oracle / "w6-size-bfff" / "dmac_size_matrix_cell_log.txt")  # host0 log
            _write(oracle / "w6-size-bfff" / "mpy_x.bin")                      # device dump
            default = nk_clean.plan_clean(root, tracked=frozenset())
            chosen = {entry.rel for entry in default.files}
            base = "tests/fixtures/psp_oracle/build"
            for built in ("probe.o", "nakagawa_psp_oracle.elf", "EBOOT.PBP", "build-config.stamp",
                          "w6-size-bfff/dmac_size_matrix_cell.prx"):
                self.assertIn(f"{base}/{built}", chosen)
            for record in ("w6-size-bfff/m1-01-dmac-boot3.json",
                           "w6-size-bfff/dmac_size_matrix_cell_log.txt", "w6-size-bfff/mpy_x.bin"):
                self.assertNotIn(f"{base}/{record}", chosen)
                self.assertIn(f"{base}/{record}", {entry.rel for entry in default.skipped})
            opted = nk_clean.plan_clean(root, tracked=frozenset(), include_title_outputs=True)
            self.assertIn(f"{base}/w6-size-bfff/m1-01-dmac-boot3.json",
                          {entry.rel for entry in opted.files})


class ReparsePointTests(unittest.TestCase):
    def test_junction_target_outside_repo_is_never_selected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            root = _make_workspace(base)
            outside_dir = base / "outside" / "protected_dir"
            secret = _write(outside_dir / "secret.bin", b"secret")
            link = root / "build" / "link_to_protected"
            if not _make_dir_link(link, outside_dir):
                self.skipTest("cannot create a directory symlink/junction on this host")

            preview = nk_clean.plan_clean(root, tracked=frozenset())
            for entry in preview.files:
                self.assertNotIn("secret", entry.rel)
                self.assertNotIn("outside", entry.rel)
            skipped = [entry for entry in preview.skipped if "link_to_protected" in entry.rel]
            self.assertTrue(skipped, "reparse point must be reported as skipped")
            self.assertIn("never followed", skipped[0].reason)

            failures = nk_clean.execute(preview)
            self.assertEqual(failures, [])
            self.assertTrue(secret.exists(), "protected target behind the junction was deleted")
            self.assertTrue(link.exists(), "the junction entry itself is left in place")

    def test_junction_to_protected_root_inside_repo_is_never_followed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = _make_workspace(Path(tmp))
            vault = root / "keys" / "vault"
            secret = _write(vault / "secret.bin", b"secret")
            link = root / "build" / "vault_link"
            if not _make_dir_link(link, vault):
                self.skipTest("cannot create a directory symlink/junction on this host")

            preview = nk_clean.plan_clean(root, tracked=frozenset())
            self.assertFalse(any("secret" in entry.rel for entry in preview.files))

            failures = nk_clean.execute(preview)
            self.assertEqual(failures, [])
            self.assertTrue(secret.exists(), "junction into keys/ was followed")

    def test_allowlist_root_replaced_by_junction_selects_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            root = _make_workspace(base)
            elsewhere = base / "elsewhere"
            outside_file = _write(elsewhere / "payload.bin", b"payload")
            build = root / "build"
            build.mkdir(parents=True, exist_ok=True)
            link_tmp = base / "build-link"
            if not _make_dir_link(link_tmp, elsewhere):
                self.skipTest("cannot create a directory symlink/junction on this host")
            os.rmdir(build)
            os.rename(link_tmp, build)

            preview = nk_clean.plan_clean(root, tracked=frozenset())
            self.assertFalse(
                any(entry.rel.startswith("build/") for entry in preview.files),
                "contents behind a junctioned allowlisted root must not be selected",
            )
            failures = nk_clean.execute(preview)
            self.assertEqual(failures, [])
            self.assertTrue(outside_file.exists())


class ContainmentTests(unittest.TestCase):
    def test_path_outside_repo_root_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            root = _make_workspace(base)
            root_real = os.path.realpath(os.path.abspath(root))
            outside = _write(base / "elsewhere" / "file.bin", b"out")
            inside = _write(root / "build" / "ok.o", b"in")

            nk_clean._require_within(root_real, inside)
            with self.assertRaises(nk_clean.CleanRefusedError):
                nk_clean._require_within(root_real, outside)

    def test_junction_on_an_intermediate_component_refuses_the_plan_and_deletes_nothing(
            self) -> None:
        """logs/ redirected by a junction: the plan lstats only the log file itself, so the
        containment check must refuse before the first unlink, wherever the junction points."""
        for where in ("outside", "inside"):
            with self.subTest(target=where), tempfile.TemporaryDirectory() as tmp:
                base = Path(tmp)
                root = _make_workspace(base)
                built = _write(root / "build" / "game.o", b"abcd")
                target = (base / "elsewhere") if where == "outside" else (root / "src" / "vault")
                redirected = _write(target / "build_out_recomp.log", b"not a build log")
                if not _make_dir_link(root / "logs", target):
                    self.skipTest("cannot create a directory symlink/junction on this host")

                preview = nk_clean.plan_clean(root, tracked=frozenset())
                self.assertIn("logs/build_out_recomp.log", {rel for rel, _ in preview.errors})
                self.assertNotIn("logs/build_out_recomp.log", {f.rel for f in preview.files})

                code, out, _err = _run_main(root, ["--yes"])
                self.assertEqual(code, 1, out)
                self.assertIn("REFUSED", out)
                self.assertTrue(built.exists(), "a containment failure still deleted build/")
                self.assertTrue(redirected.exists(), "the redirected log was deleted")

    def test_a_component_swapped_after_planning_deletes_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            root = _make_workspace(base)
            built = _write(root / "build" / "game.o", b"abcd")
            _write(root / "logs" / "build_out_recomp.log", b"log")
            preview = nk_clean.plan_clean(root, tracked=frozenset())
            self.assertFalse(preview.errors, preview.errors)
            self.assertIn("logs/build_out_recomp.log", {f.rel for f in preview.files})

            outside = _write(base / "elsewhere" / "build_out_recomp.log", b"outside")
            shutil.rmtree(root / "logs")
            if not _make_dir_link(root / "logs", base / "elsewhere"):
                self.skipTest("cannot create a directory symlink/junction on this host")

            failures = nk_clean.execute(preview)
            self.assertTrue(failures)
            self.assertEqual(failures[-1], nk_clean.NOTHING_DELETED)
            self.assertTrue(any(f.startswith("logs/build_out_recomp.log:") for f in failures))
            self.assertTrue(built.exists(), "files before the refused one were deleted")
            self.assertTrue(outside.exists())


class CheckoutDryRunTests(unittest.TestCase):
    def test_cli_dry_run_against_this_checkout_deletes_nothing(self) -> None:
        env = dict(os.environ)
        env.pop("CONFIRM", None)
        proc = subprocess.run(
            [sys.executable, str(TOOLS / "nk_clean.py")],
            cwd=ROOT, capture_output=True, text=True, env=env, timeout=600, check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("DRY-RUN", proc.stdout)
        self.assertTrue((ROOT / "AGENTS.md").is_file())
        self.assertTrue((ROOT / "Makefile").is_file())


class EntryPointWiringTests(unittest.TestCase):
    def test_makefile_exposes_preview_first_clean_target(self) -> None:
        text = read_makefile(ROOT)
        catalog = text.split("PUBLIC_TARGETS :=", 1)[1].split("INTERNAL_TARGETS :=", 1)[0]
        self.assertIn("clean-preview", catalog)
        self.assertIn("HELP_DESCRIPTION_clean-preview :=", text)
        self.assertIn("tools/nk_clean.py", text)
        recipe = next(
            line for line in text.splitlines()
            if line.startswith("\t") and "tools/nk_clean.py" in line
        )
        self.assertIn("--yes", recipe)
        self.assertIn("$(filter 1,$(CONFIRM))", recipe)
        self.assertIn("$(filter command line,$(origin CONFIRM))", recipe)
        self.assertIn("$(if $(filter 1,$(INCLUDE_TITLE)),--include-title-outputs,)", recipe)
        self.assertIn("--older-than", recipe)

    def test_make_confirms_only_from_its_command_line(self) -> None:
        make = shutil.which("mingw32-make") or shutil.which("make")
        if make is None:
            self.skipTest("no make on PATH")
        env = dict(os.environ)
        env.pop("INCLUDE_TITLE", None)
        env["CONFIRM"] = "1"
        # A scratch BUILD_ROOT: even a dry run's parse writes the SDL3 discovery fragment
        # beneath BUILD_ROOT, which must not be the checkout's build/.
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        build_root = Path(scratch.name).as_posix()
        inherited = subprocess.run(
            [make, "--no-print-directory", "-n", "clean-preview", f"BUILD_ROOT={build_root}"],
            cwd=ROOT, capture_output=True, text=True, env=env, timeout=600, check=False,
        )
        self.assertEqual(inherited.returncode, 0, inherited.stdout + inherited.stderr)
        line = next(x for x in inherited.stdout.splitlines() if "tools/nk_clean.py" in x)
        self.assertNotIn("--yes", line, "an inherited CONFIRM=1 must not confirm")
        env.pop("CONFIRM")
        explicit = subprocess.run(
            [make, "--no-print-directory", "-n", "clean-preview", f"BUILD_ROOT={build_root}",
             "CONFIRM=1", "INCLUDE_TITLE=1"],
            cwd=ROOT, capture_output=True, text=True, env=env, timeout=600, check=False,
        )
        self.assertEqual(explicit.returncode, 0, explicit.stdout + explicit.stderr)
        line = next(x for x in explicit.stdout.splitlines() if "tools/nk_clean.py" in x)
        self.assertIn("--yes", line)
        self.assertIn("--include-title-outputs", line)

    def test_frontend_exposes_clean_action(self) -> None:
        text = (ROOT / "nk.ps1").read_text(encoding="utf-8-sig")
        self.assertIn('"Clean"', text)
        self.assertIn("nk_clean.py", text)
        self.assertIn("[switch]$Yes", text)


if __name__ == "__main__":
    unittest.main()
