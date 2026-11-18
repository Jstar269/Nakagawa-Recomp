# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Workspace path handling with spaces, Unicode, and long paths (#368).

Exercises the repository's path-handling helpers against hostile-but-legal
workspace shapes: the workspace Doctor's long-path and directory probes, the
package route's Make-safety checks, the XB extractor's containment utilities,
and the clean preview's own planning. All fixtures live in temporary
directories; every helper must either handle the path correctly or refuse it
with a clear, named error -- never raise a bare OSError/ValueError.

The tracked-tree layer additionally enforces the #368 acceptance item that every
first-party absolute machine/workspace path is removed, generated locally, an
explicit test/example, or a documented platform convention.
"""

from __future__ import annotations

import ast
import os
import re
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

import extract_xb  # noqa: E402
import nk_clean  # noqa: E402
import nk_doctor  # noqa: E402
import nk_doctor_checks  # noqa: E402
import nk_doctor_core  # noqa: E402
import publish_audit  # noqa: E402
import title_codegen_plan  # noqa: E402


def _write(path: Path, data: bytes = b"x") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


def _make_workspace(base: Path) -> Path:
    root = base / "ws"
    for marker in nk_clean.WORKSPACE_ANCHORS:
        _write(root / marker)
    return root


def _isolated_git_env() -> dict[str, str]:
    """The environment for a temporary fixture repository, free of the host's Git setup.

    The host's global and system configuration (a core.hooksPath commit-msg hook, an
    init.templateDir, a commit template) and inherited GIT_* variables (GIT_DIR or
    GIT_INDEX_FILE from a hook run, GIT_CONFIG_PARAMETERS) would otherwise reach the fixture
    and fail or redirect its commands for reasons unrelated to the code under test.
    """
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith("GIT_")}
    env["GIT_CONFIG_GLOBAL"] = os.devnull
    env["GIT_CONFIG_NOSYSTEM"] = "1"
    return env


def _git(cwd: Path, *args: str) -> None:
    subprocess.run(
        [
            "git",
            "-c",
            "user.name=doctor test",
            "-c",
            "user.email=doctor@example.invalid",
            "-c",
            "commit.gpgsign=false",
            *args,
        ],
        cwd=cwd,
        env=_isolated_git_env(),
        capture_output=True,
        text=True,
        check=True,
    )


class PackageRouteMakeSafetyTests(unittest.TestCase):
    def test_space_in_path_is_refused_with_actionable_message(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            spaced = Path(tmp) / "out dir" / "package"
            with self.assertRaises(title_codegen_plan.PackageRouteError) as ctx:
                title_codegen_plan._absolute_path(spaced, "output-dir")
            self.assertEqual(ctx.exception.code, "PACKAGE_UNSUPPORTED_PATH")
            self.assertIn("choose a location without them", str(ctx.exception))
            # #296 is closed: the refusal names a remedy, not pending work (#2281).
            self.assertNotIn("in the works", str(ctx.exception))

    def test_unicode_without_space_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            unicode_path = Path(tmp) / "ビルド出力" / "package"
            resolved = title_codegen_plan._absolute_path(unicode_path, "output-dir")
            self.assertIsInstance(resolved, Path)
            self.assertEqual(resolved.name, "package")

    def test_fullwidth_space_is_refused_as_unmakeable(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "出力\u3000dir"
            with self.assertRaises(title_codegen_plan.PackageRouteError) as ctx:
                title_codegen_plan._absolute_path(path, "output-dir")
            self.assertEqual(ctx.exception.code, "PACKAGE_UNSUPPORTED_PATH")

    def test_control_character_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad\x01name"
            with self.assertRaises(title_codegen_plan.PackageRouteError) as ctx:
                title_codegen_plan._absolute_path(path, "output-dir")
            self.assertEqual(ctx.exception.code, "PACKAGE_INVALID_PATH")

    def test_long_path_is_handled_or_clearly_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            long_path = Path(tmp) / ("L" * 180) / ("M" * 180) / "package"
            try:
                resolved = title_codegen_plan._absolute_path(long_path, "output-dir")
            except title_codegen_plan.PackageRouteError as exc:
                self.assertIn(exc.code, {"PACKAGE_INVALID_PATH", "PACKAGE_UNSUPPORTED_PATH"})
                self.assertTrue(str(exc))
            else:
                self.assertIsInstance(resolved, Path)

    def test_make_safe_build_root_env_override_needs_make_safe_value(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            safe_root = base / "safe_root"
            safe_root.mkdir()
            safe_str = str(safe_root)
            if any(
                char.isspace() or char in title_codegen_plan._MAKE_UNSAFE_PATH_CHARS
                for char in safe_str
            ):
                self.skipTest("temporary directory is not Make-safe on this host")
            with mock.patch.dict(os.environ, {"NK_BUILD_ROOT": safe_str}):
                resolved = title_codegen_plan._resolve_make_safe_build_root(
                    base / "any" / "dest"
                )
            self.assertEqual(resolved, safe_root.resolve())

            spaced_env = str(base / "bad root")
            with mock.patch.dict(os.environ, {"NK_BUILD_ROOT": spaced_env}):
                with mock.patch(
                    "title_codegen_plan._windows_short_path", return_value=None
                ):
                    with self.assertRaises(title_codegen_plan.PackageRouteError) as ctx:
                        title_codegen_plan._resolve_make_safe_build_root(
                            base / "dest with space" / "out"
                        )
            self.assertEqual(ctx.exception.code, "PACKAGE_UNSUPPORTED_PATH")
            self.assertIn("set NK_BUILD_ROOT to a folder without spaces", str(ctx.exception))


class DoctorWorkspacePathTests(unittest.TestCase):
    def test_long_paths_measures_space_and_unicode_deepest_path(self) -> None:
        root = Path(tempfile.mkdtemp(prefix="nakagawa 空白 "))
        self.addCleanup(lambda: os.path.isdir(root) and __import__("shutil").rmtree(root, ignore_errors=True))
        deepest = (
            Path("build") / ("テスト" * 20) / ("x" * 180) / "final file.bin"
        )
        report = nk_doctor.Report(root, "build")
        with mock.patch.object(
            nk_doctor_checks, "query_windows_long_paths_enabled", return_value=False
        ), mock.patch.object(nk_doctor_checks.platform, "system", return_value="Windows"):
            nk_doctor_checks.check_long_paths(report, root, deepest_rel=deepest)

        results = [r for r in report.results if r.code == "LONG_PATHS"]
        self.assertEqual(len(results), 1)
        expected = root.resolve() / deepest
        self.assertEqual(results[0].metadata.get("total_len"), len(str(expected)))
        self.assertGreater(results[0].metadata.get("total_len", 0), 260)
        self.assertTrue(results[0].metadata.get("exceeds_260"))
        self.assertEqual(results[0].status, "WARN")
        self.assertIn("テスト", results[0].metadata.get("deepest_path", ""))
        self.assertIn("final file.bin", results[0].metadata.get("deepest_path", ""))

    def test_bounded_directory_probe_handles_unicode_and_space_names(self) -> None:
        with tempfile.TemporaryDirectory(prefix="doktor ünicode ") as tmp:
            base = Path(tmp) / "data データ"
            _write(base / "plain.txt")
            _write(base / "üñî ファイル.txt")
            _write(base / "nested dir" / "日本語.log")
            count, error = nk_doctor_core._bounded_nonempty_directory(base)
            self.assertIsNone(error)
            self.assertEqual(count, 3)


@unittest.skipUnless(shutil.which("git"), "git is required to create worktrees")
class DoctorWorktreeAccumulationTests(unittest.TestCase):
    """The advisory inventory for linked worktrees and regenerable build output (#368)."""

    def _repo(self, base: Path) -> Path:
        repo = base / "repo"
        repo.mkdir()
        _git(repo, "init", "-b", "main")
        _write(repo / "tracked.txt")
        _git(repo, "add", "tracked.txt")
        _git(repo, "commit", "-m", "init")
        return repo

    def _report(self, repo: Path, **kwargs: object) -> nk_doctor.Report:
        report = nk_doctor.Report(repo, "repo")
        nk_doctor_checks.check_worktree_accumulation(report, repo, **kwargs)
        return report

    @staticmethod
    def _warnings(report: nk_doctor.Report) -> dict[str, nk_doctor_core.Result]:
        return {r.code: r for r in report.results if r.status == "WARN"}

    def test_fixture_repository_ignores_the_hosts_global_hooks(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            hooks = Path(tmp) / "host-hooks"
            hook = _write(hooks / "commit-msg",
                          b"#!/bin/sh\necho 'rejected by host policy' >&2\nexit 1\n")
            hook.chmod(0o755)
            config = _write(Path(tmp) / "host-gitconfig",
                            f"[core]\n\thooksPath = {hooks.as_posix()}\n".encode("utf-8"))
            with mock.patch.dict(os.environ, {"GIT_CONFIG_GLOBAL": str(config)}):
                repo = self._repo(Path(tmp))
            self.assertTrue((repo / ".git").is_dir())

    def test_single_checkout_passes_without_warning(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = self._repo(Path(tmp))
            report = self._report(repo)
            self.assertFalse(self._warnings(report))
            passes = [r for r in report.results if r.code == "WORKTREES" and r.status == "PASS"]
            self.assertEqual(len(passes), 1)
            self.assertIn("1 worktree", passes[0].summary)

    def test_nested_worktree_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = self._repo(Path(tmp))
            _git(repo, "worktree", "add", "--detach", str(repo / ".agents" / "lane"))
            warnings = self._warnings(self._report(repo))
            self.assertIn("WORKTREES_NESTED", warnings)
            self.assertIn("lane", warnings["WORKTREES_NESTED"].detail or "")

    def test_unregistered_directory_beside_registered_worktrees_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            repo = self._repo(base)
            sibling = base / "worktrees"
            _git(repo, "worktree", "add", "--detach", str(sibling / "registered"))
            (sibling / "stray copy").mkdir()
            warnings = self._warnings(self._report(repo))
            self.assertIn("WORKTREES_UNREGISTERED", warnings)
            self.assertIn("stray copy", warnings["WORKTREES_UNREGISTERED"].detail or "")
            self.assertNotIn("WORKTREES_NESTED", warnings)

    def test_unrelated_neighbouring_worktrees_directory_is_ignored(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            repo = self._repo(base)
            (base / "worktrees" / "unrelated project").mkdir(parents=True)
            self.assertNotIn("WORKTREES_UNREGISTERED", self._warnings(self._report(repo)))

    def test_missing_worktree_is_reported_stale(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            repo = self._repo(base)
            target = base / "worktrees" / "gone"
            _git(repo, "worktree", "add", "--detach", str(target))
            shutil.rmtree(target)
            warnings = self._warnings(self._report(repo))
            self.assertIn("WORKTREES_STALE", warnings)
            self.assertIn("gone", warnings["WORKTREES_STALE"].detail or "")

    def test_count_and_build_size_bounds_warn(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            repo = self._repo(base)
            lane = base / "worktrees" / "lane"
            _git(repo, "worktree", "add", "--detach", str(lane))
            _write(repo / "build" / "artifact.bin", b"x" * 2048)
            _write(lane / "build" / "artifact.bin", b"x" * 2048)
            warnings = self._warnings(
                self._report(
                    repo,
                    count_limit=0,
                    total_build_limit=1,
                    single_build_limit=1,
                    time_budget=30.0,
                )
            )
            self.assertIn("WORKTREES_COUNT", warnings)
            self.assertIn("WORKTREES_BUILD_SIZE", warnings)
            detail = warnings["WORKTREES_BUILD_SIZE"].detail or ""
            self.assertIn("4.0 KiB", detail)
            self.assertIn("advisory limits", detail)

    def test_bounded_directory_walk_reports_truncation(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp) / "build"
            _write(build / "a.bin", b"x" * 16)
            _write(build / "b.bin", b"x" * 16)
            measured, truncated = nk_doctor_checks._bounded_directory_bytes(
                build, deadline=time.monotonic() + 30.0, file_limit=1
            )
            self.assertTrue(truncated)
            self.assertGreater(measured, 0)


class ExtractXbPathUtilityTests(unittest.TestCase):
    def test_member_path_accepts_unicode_with_inner_space(self) -> None:
        name = "data/メニュー コモン/x.bin"
        self.assertEqual(extract_xb.normalize_member_path(name), name)

    def test_member_path_refuses_traversal_and_overlong_component(self) -> None:
        with self.assertRaises(extract_xb.UnsafeArchivePathError):
            extract_xb.normalize_member_path("../escape.bin")
        with self.assertRaises(extract_xb.UnsafeArchivePathError):
            extract_xb.normalize_member_path("data/" + "A" * (extract_xb.MAX_MEMBER_COMPONENT_CHARS + 1))

    def test_contained_path_resolves_unicode_and_refuses_escape(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root_real = extract_xb.canonical_root(tmp)
            target = extract_xb.contained_path(
                root_real, "ディレクトリ/file with space.bin"
            )
            self.assertEqual(
                target,
                os.path.join(root_real, "ディレクトリ", "file with space.bin"),
            )
            with self.assertRaises(extract_xb.UnsafeArchivePathError):
                extract_xb.contained_path(root_real, "../escape.bin")


def _remove_tree(path: Path) -> None:
    """Remove a fixture tree, including a path longer than MAX_PATH on Windows."""
    target = os.path.abspath(path)
    if os.name == "nt":
        target = "\\\\?\\" + target
    shutil.rmtree(target)


class CleanWorkspacePathTests(unittest.TestCase):
    def _temp_root(self, prefix: str) -> Path:
        root = Path(tempfile.mkdtemp(prefix=prefix))
        self.addCleanup(_remove_tree, root)
        return root

    def test_plan_and_delete_under_space_and_unicode_root(self) -> None:
        root = self._temp_root("nk clean 試験 ")
        for marker in nk_clean.WORKSPACE_ANCHORS:
            _write(root / marker)
        doomed = _write(root / "build" / "サブ ディレクトリ" / "オブジェクト.o", b"obj")
        keep = _write(root / "keys" / "secret.bin", b"key")

        preview = nk_clean.plan_clean(root, tracked=frozenset(), include_title_outputs=True)
        self.assertFalse(preview.errors, preview.errors)
        selected = {f.rel for f in preview.files}
        self.assertIn("build/サブ ディレクトリ/オブジェクト.o", selected)
        text = nk_clean.render(preview, confirm=True)
        self.assertIn("オブジェクト.o", text)

        failures = nk_clean.execute(preview)
        self.assertEqual(failures, [])
        self.assertFalse(doomed.exists())
        self.assertTrue(keep.exists())

    def test_long_path_under_build_is_selected_or_clearly_refused(self) -> None:
        root = self._temp_root("nk clean long ")
        for marker in nk_clean.WORKSPACE_ANCHORS:
            _write(root / marker)
        shallow = _write(root / "build" / "shallow.o", b"obj")
        deep_parent = root / "build" / ("x" * 100) / ("y" * 100) / ("z" * 100)
        deep_file = deep_parent / "long file.bin"
        created = False
        try:
            deep_parent.mkdir(parents=True, exist_ok=True)
            deep_file.write_bytes(b"deep")
            created = len(str(deep_file)) > 260
        except OSError:
            created = False

        preview = nk_clean.plan_clean(root, tracked=frozenset(), include_title_outputs=True)
        selected = {f.rel for f in preview.files}
        scan_errors = [message for _, message in preview.errors]
        if created:
            long_rel = deep_file.relative_to(root).as_posix()
            # The refusal must name the long path or a folder above it; an error about some
            # other path does not account for a long file missing from the preview.
            refused = any(
                rel == long_rel or long_rel.startswith(rel.rstrip("/") + "/")
                for rel, _ in preview.errors
            )
            self.assertTrue(
                long_rel in selected or refused,
                "a >260-char path must be either selected or reported as a scan refusal "
                f"(errors={preview.errors})",
            )
        else:
            self.assertTrue(
                preview.files,
                f"shallow tree must still be previewed (errors={scan_errors})",
            )
        self.assertTrue(shallow.exists())
        render = nk_clean.render(preview, confirm=False)
        self.assertIn("DRY-RUN", render)

    def test_containment_refuses_long_outside_component(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            root = _make_workspace(base)
            root_real = os.path.realpath(os.path.abspath(root))
            outside = _write(base / ("o" * 200) / ("p" * 200) / "file.bin", b"out")
            with self.assertRaises(nk_clean.CleanRefusedError):
                nk_clean._require_within(root_real, outside)


class ExplicitAssetRootTests(unittest.TestCase):
    """A command resolves its outputs from named roots, not the invoking cwd (#368)."""

    def test_prepare_resolves_from_its_data_root_whatever_the_cwd(self) -> None:
        import nk_cli  # noqa: PLC0415 - heavy; only this test needs it

        with tempfile.TemporaryDirectory() as tmp:
            user_root = Path(tmp) / "user data"
            elsewhere = Path(tmp) / "another checkout"
            elsewhere.mkdir()
            args = mock.Mock(user_data_root=user_root, player=None,
                             iso=str(Path(tmp) / "disc.iso"), dest=None)
            seen: list[Path] = []

            class _Engine:
                def __init__(self, base_dir=None, **_kwargs) -> None:
                    seen.append(base_dir)

                def prepare_game(self, *_args, **_kwargs):
                    return mock.Mock(success=False, error_code="TEST_STOP", error_message="stop")

            cwd = os.getcwd()
            try:
                os.chdir(elsewhere)
                with mock.patch.object(nk_cli, "PreparationEngine", _Engine), \
                        mock.patch.object(nk_cli, "NativeTitleStager"), \
                        mock.patch("sys.stderr"):
                    self.assertEqual(nk_cli.cmd_prepare(args), 1)
            finally:
                os.chdir(cwd)
            # Left out, the engine's base folder was the cwd: "another checkout".
            self.assertEqual(seen, [user_root.resolve(strict=False)])


class TrackedMachinePathTests(unittest.TestCase):
    """No undeclared absolute machine/workspace path in tracked first-party files (#368).

    Two structural layers run over ``git ls-files`` with the third-party imports
    excluded:

    * a user-profile or workspace root never qualifies, in any tracked file
      (tests included), outside an explicit exception;
    * every other drive-rooted token must sit on a documented platform
      convention or on an explicit fixture/prose exception below.

    Files that are tests or examples by construction (``tools/test_*``,
    ``tests/``, ``tests/fixtures/``, ``*.test.*``) are the explicit test/example class
    of the acceptance item; the first layer still applies to them, and the
    inventory of their fixture paths lives in the issue rather than here.
    """

    THIRD_PARTY_PREFIXES = ("src/rt/atrac3p/", "font/")

    #: Files allowed to name a user-profile/workspace root, with the reason the
    #: path is first-party-acceptable.
    ROOT_PATH_EXCEPTIONS = {
        "assets/public_source_profile.json": (
            "publication policy declares the maintainer's private workspace root; "
            "policy files are maintainer-owned and never edited by agents"
        ),
    }

    USER_OR_WORKSPACE_ROOTS = (
        (
            re.compile(r"[A-Za-z]:[\\/]{1,2}Users(?:[\\/]|(?![A-Za-z0-9_]))", re.IGNORECASE),
            "Windows user-profile path",
        ),
        (
            re.compile(r"[A-Za-z]:[\\/]{1,2}nk(?![A-Za-z0-9_])", re.IGNORECASE),
            "Nakagawa workspace root",
        ),
        (re.compile(r"/(?:home|Users)/[^\s/]+"), "POSIX user-home path"),
        # drvfs is case-insensitive, so a lowercase users folder or an uppercase drive letter
        # or workspace name reaches the same folders.
        (
            re.compile(r"/mnt/[a-z]/(?:users|nk)(?:[\\/]|(?![A-Za-z0-9_]))", re.IGNORECASE),
            "WSL user/workspace root",
        ),
    )

    DRIVE_TOKEN = re.compile(r"(?<![A-Za-z0-9_])[A-Za-z]:[\\/][A-Za-z0-9_./\\-]*")
    #: What a drive-rooted path can still hold after DRIVE_TOKEN's ASCII run stops (a
    #: non-ASCII or punctuation folder name), so `C:\` followed by such a name is compared
    #: whole and never passes as the bare drive root.
    DRIVE_TOKEN_CONTINUATION = re.compile(r"[^\s\"'`<>|),;\]}]+")

    #: Documented platform conventions and example placeholders, by normalised
    #: lower-case prefix. Every reason naming docs/SETUP.md must stay true there.
    ALLOWED_DRIVE_PREFIXES = {
        "c:/msys64": "default MSYS2 UCRT64 install root (docs/SETUP.md)",
        "c:\\msys64": "default MSYS2 UCRT64 install root (docs/SETUP.md)",
        "c:/vulkansdk": "default Vulkan SDK root (docs/SETUP.md)",
        "c:\\vulkansdk": "default Vulkan SDK root (docs/SETUP.md)",
        "c:/windows": "Windows system directory, the WINDIR fallback (docs/SETUP.md)",
        "c:\\windows": "Windows system directory, the WINDIR fallback (docs/SETUP.md)",
        "c:/path/": "placeholder root in usage examples",
        "c:\\path\\": "placeholder root in usage examples",
    }

    #: Exact normalised tokens that name no machine location, by reason.
    ALLOWED_DRIVE_TOKENS = {
        "c:\\": "bare drive root named by detectors and comment examples",
        "c:/": "bare drive root (forward-slash form) named by path-handling code and comments",
        "c:/x": "hostile drive-rooted input in the VFS containment selftest (tests/native/vfs_selftest.c)",
        "c:\\foo": "hostile device-path input in the VFS containment selftest (tests/native/vfs_selftest.c)",
        "c:\\extracted": "SR_DATAROOT comment example, an absolute data root (src/rt/hle.c)",
    }

    @staticmethod
    def _collapse_backslashes(token: str) -> str:
        while "\\\\" in token:
            token = token.replace("\\\\", "\\")
        return token

    @staticmethod
    def _is_explicit_test_or_fixture(rel: str) -> bool:
        parts = rel.split("/")
        name = parts[-1]
        if any(part in parts for part in ("tests", "fixtures")):
            return True
        if name.startswith("test_") or name.endswith(("_test.py", "_test.ps1")):
            return True
        if re.search(r"\.test\.(?:ts|tsx|js|mjs|cjs)$", name) or ".spec." in name:
            return True
        return False

    def _ls_files(self, root: Path, *options: str) -> list[str]:
        try:
            raw = subprocess.run(
                ["git", "-C", str(root), "ls-files", "-z", *options],
                capture_output=True,
                check=True,
            ).stdout
        except (OSError, subprocess.CalledProcessError) as exc:
            self.fail(f"git ls-files failed to enumerate tracked files: {exc}")
        # Git prints path bytes as stored; os.fsdecode maps them back to the names the file
        # system uses (UTF-8 on Windows, surrogateescape elsewhere), whatever the locale is.
        return [os.fsdecode(entry) for entry in raw.split(b"\0") if entry]

    def _tracked_texts(self, root: Path = ROOT) -> list[tuple[str, str]]:
        if shutil.which("git") is None:
            self.skipTest("git is required to enumerate tracked first-party files")
        texts: list[tuple[str, str]] = []
        unreadable: list[str] = []
        deleted: set[str] | None = None
        for rel in self._ls_files(root):
            if rel.startswith(self.THIRD_PARTY_PREFIXES):
                continue
            path = root / rel
            try:
                data = path.read_bytes()
            except OSError as exc:
                if deleted is None:
                    deleted = set(self._ls_files(root, "--deleted"))
                # A tracked file deleted in the worktree has no bytes to publish; any other
                # read failure would leave the file unchecked, so it is reported.
                if rel not in deleted or path.exists():
                    unreadable.append(f"{rel}: {exc}")
                continue
            if len(data) > 8 * 1024 * 1024 or publish_audit._is_binary_bytes(data):
                continue
            text = publish_audit._decode_text_safe(data)
            if text is None:
                continue
            texts.append((rel, text))
        self.assertEqual(unreadable, [], "these tracked files could not be read, so the path "
                         "scan would pass without checking them")
        return texts

    @staticmethod
    def _line_of(text: str, offset: int) -> int:
        return text.count("\n", 0, offset) + 1

    def _root_violations(self, rel: str, text: str) -> list[str]:
        return [
            f"{rel}:{self._line_of(text, match.start())}: {kind}: {match.group(0)!r}"
            for pattern, kind in self.USER_OR_WORKSPACE_ROOTS
            for match in pattern.finditer(text)
        ]

    def _drive_violations(self, rel: str, text: str) -> list[str]:
        violations: list[str] = []
        for match in self.DRIVE_TOKEN.finditer(text):
            whole = match.group(0)
            rest = self.DRIVE_TOKEN_CONTINUATION.match(text, match.end())
            if rest:
                whole += rest.group(0)
            token = self._collapse_backslashes(whole).lower()
            if token in self.ALLOWED_DRIVE_TOKENS:
                continue
            if any(token.startswith(prefix) for prefix in self.ALLOWED_DRIVE_PREFIXES):
                continue
            violations.append(f"{rel}:{self._line_of(text, match.start())}: {whole!r}")
        return violations

    def test_lowercase_and_uppercase_wsl_roots_are_flagged(self) -> None:
        # Built from pieces so this file's own text names no such root.
        for sample in ("/mnt/c/" + "users/someone/x", "/mnt/C/" + "Users/someone",
                       "/mnt/c/" + "NK/main", "/mnt/d/" + "nk"):
            with self.subTest(sample=sample):
                self.assertTrue(self._root_violations("sample.txt", f"see {sample} here"))

    def test_drive_root_followed_by_a_non_ascii_or_punctuation_folder_is_not_bare(self) -> None:
        for folder in ("作業\\nakagawa\\build", "~me\\x", "(private)\\a", "作業/nakagawa"):
            sample = "C:" + ("/" if "/" in folder else "\\") + folder
            with self.subTest(sample=sample):
                self.assertTrue(self._drive_violations("sample.txt", f"path {sample} end"))
        for allowed in ("C:\\", "C:/", "C:\\msys64\\ucrt64\\bin", "C:/path/to/作業"):
            with self.subTest(allowed=allowed):
                self.assertEqual(self._drive_violations("sample.txt", f'"{allowed}" end'), [])

    def test_tracked_path_scan_reads_non_ascii_names_under_any_locale(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp) / "repo"
            repo.mkdir()
            _git(repo, "init", "-b", "main")
            _write(repo / "試験 notes.txt", b"plain text\n")
            _git(repo, "add", "試験 notes.txt")
            # A legacy (non-UTF-8) locale decode is where a name would turn into mojibake.
            env = dict(os.environ, PYTHONUTF8="0", PYTHONIOENCODING="utf-8")
            child = subprocess.run(
                [sys.executable, "-c",
                 "import sys; from pathlib import Path; sys.path.insert(0, sys.argv[1]);"
                 "import test_workspace_paths as t;"
                 "case = t.TrackedMachinePathTests('test_drive_rooted_tokens_are_documented_or_explicit');"
                 "print(ascii([rel for rel, _ in case._tracked_texts(Path(sys.argv[2]))]))",
                 str(TOOLS), str(repo)],
                cwd=tmp, env=env, capture_output=True, text=True, encoding="utf-8", timeout=120)
            self.assertEqual(child.returncode, 0, child.stderr)
            self.assertEqual(child.stdout.strip(), ascii(["試験 notes.txt"]))

    def test_tracked_path_scan_reports_an_unreadable_tracked_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp) / "repo"
            repo.mkdir()
            _git(repo, "init", "-b", "main")
            _write(repo / "kept.txt", b"kept\n")
            _write(repo / "removed.txt", b"removed\n")
            _write(repo / "replaced.txt", b"replaced\n")
            _git(repo, "add", "kept.txt", "removed.txt", "replaced.txt")
            (repo / "removed.txt").unlink()
            self.assertEqual([rel for rel, _ in self._tracked_texts(repo)],
                             ["kept.txt", "replaced.txt"])
            (repo / "replaced.txt").unlink()
            (repo / "replaced.txt").mkdir()
            with self.assertRaises(AssertionError) as ctx:
                self._tracked_texts(repo)
            self.assertIn("replaced.txt", str(ctx.exception))

    def test_tracked_files_name_no_user_profile_or_workspace_root(self) -> None:
        violations: list[str] = []
        for rel, text in self._tracked_texts():
            if rel in self.ROOT_PATH_EXCEPTIONS:
                continue
            violations += self._root_violations(rel, text)
        self.assertEqual(
            violations,
            [],
            "tracked first-party files must not name a user-profile or workspace "
            "root; add the file and its reason to ROOT_PATH_EXCEPTIONS only for a "
            "deliberate policy declaration (#368):\n  " + "\n  ".join(violations),
        )

    def test_drive_rooted_tokens_are_documented_or_explicit(self) -> None:
        violations: list[str] = []
        for rel, text in self._tracked_texts():
            if rel in self.ROOT_PATH_EXCEPTIONS or self._is_explicit_test_or_fixture(rel):
                continue
            violations += self._drive_violations(rel, text)
        self.assertEqual(
            violations,
            [],
            "every drive-rooted token in first-party non-test code needs a "
            "documented platform convention or an explicit exception with a "
            "reason (#368):\n  " + "\n  ".join(violations),
        )

    def test_platform_conventions_are_documented_in_setup(self) -> None:
        # Single-sourced from ALLOWED_DRIVE_PREFIXES: every prefix whose reason names
        # docs/SETUP.md must appear there as a drive-rooted path, in either slash form
        # and any case, so the allowlist and the setup guide cannot drift apart.
        setup = (ROOT / "docs" / "SETUP.md").read_text(encoding="utf-8")
        documented = {
            self._collapse_backslashes(token).lower().replace("\\", "/")
            for token in self.DRIVE_TOKEN.findall(setup)
        }
        conventions = sorted({
            prefix.replace("\\", "/")
            for prefix, reason in self.ALLOWED_DRIVE_PREFIXES.items()
            if "docs/SETUP.md" in reason
        })
        self.assertTrue(conventions, "ALLOWED_DRIVE_PREFIXES names no docs/SETUP.md convention")
        for prefix in conventions:
            self.assertTrue(
                any(token == prefix or token.startswith(prefix + "/") for token in documented),
                f"docs/SETUP.md must document the {prefix!r} platform convention "
                "that ALLOWED_DRIVE_PREFIXES defers to (#368)",
            )


class ArtifactIgnoreTests(unittest.TestCase):
    """Generated artifacts stay outside `git add -A` reach (#368).

    Every path below was verified ignored against the live .gitignore rules
    when this guard landed; a rule removal, a pattern broadened past its
    output root, or an accidental `git add -f` of one of these names fails
    here instead of at a maintainer's next `git status`.
    """

    GENERATED_ARTIFACTS = (
        "progress.json",
        "visual_regression_report.json",
        "watchpoints.json",
        "benchmarks_report.json",
        "compile_commands.json",
        "logs/build_out_recomp.log",
        "build/mygame/mygame.o",
        "tools/__pycache__/cache.pyc",
        "scratch/notes.txt",
        "nidseq_mine.txt",
        "tex_0.pgm",
        "capture.psess",
        "capture.ngef",
        "notes.bak",
        "crash.dmp",
        "crash.stackdump",
        "guest_image.bin",
        "run.trace",
        "payload.obj",
        "SDL3.dll",
        "package/SDL3.dll",
    )

    def test_declared_output_roots_are_gitignored(self) -> None:
        # Bytes stdin: Python text-mode pipes translate \n to os.linesep on
        # Windows, and git check-ignore does not strip a trailing \r, which
        # would silently miss every real rule (#368).
        proc = subprocess.run(
            ["git", "check-ignore", "--stdin"],
            input=("\n".join(self.GENERATED_ARTIFACTS) + "\n").encode("utf-8"),
            cwd=ROOT,
            capture_output=True,
            check=False,
        )
        reported = {
            line for line in proc.stdout.decode("utf-8", "replace").splitlines() if line
        }
        missing = sorted(set(self.GENERATED_ARTIFACTS) - reported)
        self.assertEqual(
            missing,
            [],
            "these generated artifacts are no longer gitignored (or are now "
            "tracked), so a build/test run can stage them by accident (#368):\n  "
            + "\n  ".join(missing),
        )


def _check_ignored(paths: list[str], cwd: Path = ROOT) -> set[str]:
    """Return the subset of ``paths`` the checkout's ignore rules exclude."""
    # Bytes stdin for the same reason as ArtifactIgnoreTests above.
    proc = subprocess.run(
        ["git", "check-ignore", "--no-index", "--stdin"],
        input=("\n".join(paths) + "\n").encode("utf-8"),
        cwd=cwd,
        capture_output=True,
        check=False,
    )
    # 0: some path is ignored, 1: none is. Anything else (128: not a repository, a
    # safe.directory refusal) means git evaluated no rule, which is not "nothing ignored".
    if proc.returncode not in (0, 1):
        raise AssertionError(
            f"git check-ignore failed (exit {proc.returncode}), so no ignore rule was "
            f"evaluated: {proc.stderr.decode('utf-8', 'replace').strip()}")
    return {line for line in proc.stdout.decode("utf-8", "replace").splitlines() if line}


class OutsideOutputRootInventoryTests(unittest.TestCase):
    """Every fixed path outside BUILD_ROOT and LOG_DIR is declared, with its reason (#368).

    docs/WORKSPACE_DOCTOR.md, "Paths outside the output roots", lists what the runtime
    opens relative to its run folder, the names it joins to a folder it is given, and the
    fixed paths tools write. The runtime lists are derived from the source here, both
    ways. The source side is: a path default in src/rt/env_registry.h; an fopen of a
    literal name for writing and a `logs/`, `build/` or `place_game_here/` literal
    anywhere in src/; and a file name with an output extension in src/rt, bare or after
    `%s/`. Self-test output names and comments are skipped, and a literal split around
    an inttypes.h format macro is joined with the macro's name in place. A name the code assembles from separate pieces
    at run time is not seen, nor a literal default in code that env_registry.h does not
    record. Each runtime row's switches must be registered variables, and a literal
    fopen'd for writing must be declared as written. The tools table is checked row by
    row against its tool's source, not derived. A written path inside the checkout must
    be gitignored, or a debug run under `make run` leaves a file that `git add -A` stages.
    """

    DOC = ROOT / "docs" / "WORKSPACE_DOCTOR.md"
    SECTION = "### Paths outside the output roots"
    RUNTIME_HEADER = "| Path | Switch | Direction | Why it stays |"
    JOINED_HEADER = "| Name | Joined to |"
    TOOLS_HEADER = "| Path | Tool | Why it stays |"
    #: The title catalog's launch candidates and fixture memory-stick roots are joined to
    #: the root src/core/nk_launch.c is given, never to the working directory (the doc
    #: says so below the tables).
    EXCLUDED_DIRS = ("src/core/generated/",)
    REGISTRY_ROW = re.compile(r'X\("([A-Z0-9_]+)",\s*"([\w-]+)",\s*"([^"]*)"')
    RELATIVE_DEFAULT = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9_./%-]*$")
    FOPEN_WRITE = re.compile(r'fopen\(\s*"([^"]+)"\s*,\s*"[wa]')
    ROOTED_LITERAL = re.compile(r'"((?:logs|build|place_game_here)/[^"]*)"')
    BARE_NAME = re.compile(
        r'"(?:%s/)?([A-Za-z0-9][A-Za-z0-9_%.-]*\.'
        r'(?:ppm|pgm|png|bin|csv|json|jsonl|txt|ngef|wav|flag|tmp|mkv|bgr0|srt|nkcap))"')
    #: Comments, string literals and character literals, so a comment is dropped and a
    #: quote inside a literal does not end it.
    C_TOKEN = re.compile(
        r'//[^\n]*|/\*.*?\*/|("(?:\\.|[^"\\\n])*")|(\'(?:\\.|[^\'\\\n])*\')', re.S)
    FORMAT_MACRO_JOIN = re.compile(r'"\s*(PRI\w+)\s*"')
    CODE_SPAN = re.compile(r"`([^`]+)`")

    @classmethod
    def _table(cls, header: str) -> list[list[str]]:
        text = cls.DOC.read_text(encoding="utf-8")
        if cls.SECTION not in text:
            raise AssertionError(f"{cls.DOC.name} has no {cls.SECTION!r} section")
        section = text.split(cls.SECTION, 1)[1].split("\n## ", 1)[0]
        if header not in section:
            raise AssertionError(f"the section has no table headed {header!r}")
        rows = []
        for line in section.split(header, 1)[1].splitlines()[2:]:
            if not line.startswith("|"):
                break
            rows.append([cell.strip() for cell in line.strip().strip("|").split("|")])
        return rows

    @classmethod
    def _spans(cls, cell: str) -> list[str]:
        return cls.CODE_SPAN.findall(cell)

    @classmethod
    def _first_spans(cls, header: str) -> dict[str, list[str]]:
        """First code span of each row's first cell -> the row."""
        return {cls._spans(row[0])[0]: row for row in cls._table(header) if cls._spans(row[0])}

    @classmethod
    def _code(cls, text: str) -> str:
        """C source without comments, with format-macro concatenations joined."""
        text = cls.C_TOKEN.sub(lambda m: m.group(1) or m.group(2) or " ", text)
        return cls.FORMAT_MACRO_JOIN.sub(r"\1", text)

    @classmethod
    def _runtime_sources(cls) -> list[Path]:
        found = []
        for path in sorted((ROOT / "src").rglob("*")):
            relative = path.relative_to(ROOT).as_posix()
            if path.suffix not in {".c", ".h", ".inc"} or not path.is_file():
                continue
            if "selftest" in path.name or path.name.endswith("_test.c") or "/tests/" in relative:
                continue
            if relative.startswith(cls.EXCLUDED_DIRS):
                continue
            found.append(path)
        return found

    @classmethod
    def _registry(cls) -> dict[str, tuple[str, str]]:
        """Registered variable -> (kind, default)."""
        text = (ROOT / "src" / "rt" / "env_registry.h").read_text(encoding="utf-8")
        return {name: (kind, default) for name, kind, default in cls.REGISTRY_ROW.findall(text)}

    @classmethod
    def _source_paths(cls) -> tuple[dict[str, set[str]], set[str], dict[str, str]]:
        """(literal path -> the files naming it, the literals fopen'd for writing,
        registry path default -> its variable)."""
        literals: dict[str, set[str]] = {}
        opened_for_writing: set[str] = set()
        for path in cls._runtime_sources():
            relative = path.relative_to(ROOT).as_posix()
            text = cls._code(path.read_text(encoding="utf-8", errors="replace"))
            patterns = [cls.FOPEN_WRITE, cls.ROOTED_LITERAL]
            if relative.startswith("src/rt/"):
                patterns.append(cls.BARE_NAME)
            for pattern in patterns:
                for literal in pattern.findall(text):
                    if "selftest" in literal:  # a self-test's scratch output, in TEMP
                        continue
                    literals.setdefault(literal, set()).add(relative)
            opened_for_writing.update(cls.FOPEN_WRITE.findall(text))
        defaults = {
            default: name
            for name, (kind, default) in cls._registry().items()
            if kind == "path" and default not in {"off", "unset"}
            and cls.RELATIVE_DEFAULT.fullmatch(default)
        }
        return literals, opened_for_writing, defaults

    def test_every_runtime_path_is_declared(self) -> None:
        literals, _, defaults = self._source_paths()
        declared = set(self._first_spans(self.RUNTIME_HEADER)) | set(
            self._first_spans(self.JOINED_HEADER))
        missing = sorted(f"{literal} ({', '.join(sorted(files))})"
                         for literal, files in literals.items() if literal not in declared)
        missing += sorted(f"{default} ({name})" for default, name in defaults.items()
                          if default not in declared)
        self.assertEqual(missing, [], "these runtime paths outside the output roots are not "
                         f"declared in {self.DOC.name}:\n" + "\n".join(missing))

    def test_no_runtime_row_is_stale(self) -> None:
        literals, _, defaults = self._source_paths()
        declared = list(self._first_spans(self.RUNTIME_HEADER)) + list(
            self._first_spans(self.JOINED_HEADER))
        stale = sorted(path for path in declared if path not in literals and path not in defaults)
        self.assertEqual(stale, [], "these declared runtime paths no longer appear in src/")

    def test_runtime_rows_name_registered_switches_and_true_directions(self) -> None:
        _, opened_for_writing, defaults = self._source_paths()
        registry = self._registry()
        rows = self._first_spans(self.RUNTIME_HEADER)
        problems = []
        for path, row in rows.items():
            switches = self._spans(row[1])
            if not switches and row[1] != "none":
                problems.append(f"{path}: the Switch cell is neither variables nor 'none'")
            problems += [f"{path}: {name} is not in src/rt/env_registry.h"
                         for name in switches if name not in registry]
            if path in defaults and defaults[path] not in switches:
                problems.append(f"{path}: the registry default of {defaults[path]}, "
                                "which the Switch cell does not name")
            if path in opened_for_writing and "written" not in row[2]:
                problems.append(f"{path}: opened for writing, declared {row[2]!r}")
        self.assertEqual(problems, [])

    def test_every_tool_row_names_a_tool_that_writes_it(self) -> None:
        rows = self._table(self.TOOLS_HEADER)
        self.assertTrue(rows)
        for row in rows:
            path, tool = self._spans(row[0])[0], self._spans(row[1])[0]
            with self.subTest(path=path):
                source = ROOT / tool
                self.assertTrue(source.is_file(), f"{tool} does not exist")
                text = source.read_text(encoding="utf-8")
                self.assertTrue(self._tool_code_names_path(text, path),
                                f"{tool} has no code that names {path}")

    @staticmethod
    def _tool_code_names_path(source: str, path: str) -> bool:
        """True when the tool's code, not its comments or docstrings, spells ``path``.

        The path's folder names must appear in order, as string literals (or path pieces
        of one) on a single line of code, so the line that builds the path is found. A
        `%NAME%` component is the environment variable the code reads: a literal NAME.
        """
        tree = ast.parse(source)
        docstrings = {id(node.value) for node in ast.walk(tree)
                      if isinstance(node, ast.Expr) and isinstance(node.value, ast.Constant)}
        pieces_by_line: dict[int, list[str]] = {}
        literals: set[str] = set()
        for node in ast.walk(tree):
            if (isinstance(node, ast.Constant) and isinstance(node.value, str)
                    and id(node) not in docstrings):
                literals.add(node.value)
                pieces = [piece for piece in re.split(r"[\\/]", node.value) if piece]
                pieces_by_line.setdefault(node.lineno, []).extend(pieces)
        parts = [part for part in re.split(r"[\\/]", path) if part]
        def is_env(part: str) -> bool:
            return len(part) > 2 and part[0] == part[-1] == "%"

        env = [part[1:-1] for part in parts if is_env(part)]
        rest = [part for part in parts if not is_env(part)]
        if any(name not in literals for name in env):
            return False
        if not rest:
            return bool(env)
        for pieces in pieces_by_line.values():
            for start in range(len(pieces) - len(rest) + 1):
                if pieces[start:start + len(rest)] == rest:
                    return True
        return False

    def test_tool_path_check_needs_code_not_comments(self) -> None:
        names = self._tool_code_names_path
        path = "third_party/ghidra/exports"
        prose = ('"""Writes third_party/ghidra/exports."""\n'
                 "# the exports folder under third_party/ghidra\n"
                 'HELP = "ghidra"\nOTHER = "third_party"\nLAST = "exports.txt"\n')
        self.assertFalse(names(prose, path))
        self.assertTrue(names('EXPORT_DIR = os.path.join(ROOT, "third_party", "ghidra", "exports")\n',
                              path))
        self.assertTrue(names('OUT = REPO / "progress.json"\n', "progress.json"))
        self.assertFalse(names('OUT = REPO / "progress.jsonl"\n', "progress.json"))
        appdata = "%LOCALAPPDATA%\\Nakagawa"
        self.assertTrue(names('local = os.environ.get("LOCALAPPDATA")\n'
                              'root = Path(local) / "Nakagawa"\n', appdata))
        self.assertFalse(names('"""Uses %LOCALAPPDATA%\\\\Nakagawa."""\n'
                               'root = Path(local) / "Nakagawa"\n', appdata))

    def test_written_paths_in_the_checkout_are_gitignored(self) -> None:
        written = [path for path, row in self._first_spans(self.RUNTIME_HEADER).items()
                   if "written" in row[2]]
        written += [path for path in self._first_spans(self.TOOLS_HEADER) if "%" not in path]
        self.assertGreater(len(written), 5)
        # A folder is asked about with its trailing slash, so a rule for folders applies.
        asked = [path if Path(path).suffix else path + "/" for path in written]
        ignored = _check_ignored(asked)
        self.assertEqual(sorted(set(asked) - ignored), [],
                         "a run or tool writes these into the checkout, but `git add -A` "
                         "would stage them")


class KeyFileIgnoreTests(unittest.TestCase):
    """A KeyStore file dropped anywhere in the tree stays out of `git add -A`.

    publish_audit's KEY_FILE_NAME rejects such a file only when the hook or CI
    runs; the .gitignore rules are the first line of defence and must cover the
    JSON, text and binary forms of every name that rule blocks.
    """

    STEMS = (
        "keystore", "key_store", "key-store", "keyfile", "key_file", "key-file",
        "psp_key", "psp-keys", "kirk_key", "kirk-keys",
    )
    SUFFIXES = (".json", ".txt", ".dat", ".bin", ".key", ".pem")
    PLACES = ("", "assets/", "tools/sub/")

    def _key_paths(self) -> list[str]:
        paths = [
            f"{place}{prefix}{stem}{suffix}"
            for place in self.PLACES
            for prefix in ("", "my_")
            for stem in self.STEMS
            for suffix in self.SUFFIXES
        ]
        paths += [f"{place}{name}" for place in self.PLACES
                  for name in ("keys.json", "keys.txt", "keys.dat", "keys.bin",
                               "backup.keys", "psp-keyfile.json")]
        return paths

    def test_names_the_audit_rejects_are_gitignored(self) -> None:
        paths = self._key_paths()
        for path in paths:
            with self.subTest(path=path):
                self.assertEqual(publish_audit._forbidden_path(path), "local-only key file")
        missing = sorted(set(paths) - _check_ignored(paths))
        self.assertEqual(
            missing, [],
            "these key-file names are rejected by publish_audit but no longer "
            "gitignored, so `git add -A` can stage key material:\n  " + "\n  ".join(missing),
        )

    def test_sources_that_name_the_format_stay_trackable(self) -> None:
        sources = [
            "src/core/nk_psp_keystore.c",
            "src/core/nk_psp_keystore.h",
            "tools/test_keystore_loader.py",
            "docs/KEYSTORE_FORMAT.md",
        ]
        for path in sources:
            with self.subTest(path=path):
                self.assertIsNone(publish_audit._forbidden_path(path))
        self.assertEqual(sorted(_check_ignored(sources)), [])

    def test_a_git_failure_is_not_read_as_nothing_ignored(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            outside = Path(tmp)
            with mock.patch.dict(os.environ, {"GIT_CEILING_DIRECTORIES": str(outside.parent),
                                              "GIT_DIR": str(outside / "no-such-repository")}):
                with self.assertRaises(AssertionError) as ctx:
                    _check_ignored(["src/core/nk_psp_keystore.c"], cwd=outside)
        self.assertIn("git check-ignore failed", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
