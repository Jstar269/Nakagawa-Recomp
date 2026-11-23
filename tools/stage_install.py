#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Stage a runnable player install outside the build tree, with desktop and Start-menu shortcuts.

The install uses the release layout the player already searches
(``docs/PREVIEW_RELEASE.md``)::

    <dest>/bin/nakagawa_player.exe   the built player, its runtime DLL closure,
                                     licence notices and UI fonts (on Linux the
                                     player is ``bin/nakagawa_player``, with no
                                     DLL closure: the system libraries apply)
    <dest>/source/                   the committed source tree (``git archive``),
                                     which carries tools/nk_cli.py, the Makefile
                                     and assets/prereq_manifest.json, so BUILD
                                     PACKAGE works from the install
    <dest>/nakagawa.ico              the app icon, assets/brand/nakagawa.ico
                                     (the one the executables carry; never a
                                     console icon)

Only a folder this tool created (it holds a ``.nakagawa-install.json`` that passes the
checks ``--uninstall`` applies) is ever replaced, so pointing ``--dest`` at an unrelated
folder fails closed. Staging again over a portable install (a ``bin/portable.txt``) moves
``bin/portable.txt`` and ``bin/data`` into the new ``bin/``, also out of a ``bin.old`` a
crashed update left, so an update never deletes the user's data. A folder the
Windows installer set up (it holds ``unins000.exe``) is refused for staging and for
``--uninstall`` (``INSTALL_DESTINATION_INSTALLER_OWNED``), and a shortcut that starts a
different existing copy is never replaced (``INSTALL_SHORTCUT_OTHER_COPY``, checked for
every requested shortcut before anything is staged). A
shortcut is a Windows ``.lnk`` written through ``WScript.Shell``; it starts the
player with no arguments by default, the way Explorer would start it, and
carries the player's AppUserModelID (``APP_USER_MODEL_ID``, #2006), so a pinned
shortcut, the launcher and every game it starts share one taskbar group.
``--desktop-shortcut`` puts one on the Desktop and ``--start-menu-shortcut`` one
in the user's Start menu (#2003); both may be given.
On Linux, either flag writes a per-user ``.desktop`` entry (which the desktop
shows in its application menu) and a 256 px icon under ``$XDG_DATA_HOME``
(``~/.local/share`` when unset). The marker records the paths written outside
``--dest`` in ``integration`` and keeps them across runs. Neither flag runs on its
own: the portable ZIP writes no shortcut, so a Start-menu entry needs this tool,
for example ``python tools/stage_install.py --dest <folder> --start-menu-shortcut``.

``--uninstall --dest <folder>`` removes what this tool wrote and nothing else (#2003,
#324): ``bin/``, ``source/``, the icon, the marker, any half-finished ``.old`` or
``.staging-*`` copy, and the shortcuts and desktop entries the marker records. It never
reads or deletes the per-user folders: the library, saves, settings, logs, fonts and
cache live under ``%LOCALAPPDATA%\\Nakagawa`` (on Linux ``~/.local/share/Nakagawa``, or
``~/.local/share/nakagawa-recomp`` for an earlier install),
not in the install. In portable mode (a ``bin/portable.txt``) the data folder
``bin/data``, the portable marker and the install marker stay where they are, so the
user's data is never removed with the program. A recorded path whose name this tool would
not have written is skipped, so an edited marker cannot make it delete another file; the one
exception is a ``.lnk`` (a ``--shortcut`` of your own name) that starts this install's player. A
folder with no marker is refused. ``--dry-run`` lists what would go.

The Windows install's ``bin/`` also carries ``runtime-manifest.json``
(``tools/stage_runtime_dlls.py``): the size, SHA-256 and imports of the player
and every DLL beside it, the identity contract the portable ZIP shares.

``--portable-zip`` writes the release's portable ZIP from an already staged
folder (``docs/PREVIEW_RELEASE.md``) so the same commit and inputs give the same
bytes (#324): ``SHA256SUMS.txt`` is rewritten over every file in ordinal path
order, the runtime manifest in ``bin/`` is verified, and the archive stores its
entries sorted by path, all dated ``SOURCE_DATE_EPOCH`` (the source commit's
time unless set), with fixed permissions, no extra fields and no comment.
Links, absolute or parent-relative names and Git metadata (a `.git` entry,
which the source export rewrites on every run) are refused.

Usage::

    python tools/stage_install.py --dest "<install folder>" --desktop-shortcut --start-menu-shortcut
    python tools/stage_install.py --portable-zip <out.zip> --zip-stage <staged folder>
    python tools/stage_install.py --check-assets <release folder, or file names>

``--check-assets`` checks the names of a release's files against the one-archive-
per-platform contract in ``docs/PREVIEW_RELEASE.md`` (#1876).
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import io
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import re
import subprocess
import sys
import tarfile
import time
from typing import NamedTuple, NoReturn
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "tools") not in sys.path:
    sys.path.insert(0, str(ROOT / "tools"))

import make_app_icon  # noqa: E402
from nk_core.package_cache import sha256_file  # noqa: E402
from nk_powershell import powershell_env, powershell_exe  # noqa: E402
import stage_runtime_dlls  # noqa: E402

IS_WINDOWS = os.name == "nt"
MARKER = ".nakagawa-install.json"
SCHEMA_VERSION = 2
SHORTCUT_NAME = "Nakagawa Recomp.lnk"
#: The player's identity (#2006): NK_APP_USER_MODEL_ID and NK_APP_DESKTOP_ID in
#: src/core/nk_launch.h, and their copies in src/rt/gpu_sdl3vk/sdl3vk.c for the game window.
#: tools/test_stage_install.py keeps the four equal.
APP_USER_MODEL_ID = "Nakagawa.Recomp"
APP_DESKTOP_ID = "nakagawa-recomp"
DESKTOP_ENTRY_NAME = f"{APP_DESKTOP_ID}.desktop"
DESKTOP_ICON_NAME = "nakagawa-recomp.png"
DEFAULT_ARGUMENTS = ""


class InstallError(RuntimeError):
    """A named, fail-closed install boundary."""


def default_player_path(windows: bool = IS_WINDOWS) -> Path:
    """The player build's output on the given host, under ``build/``."""
    return ROOT / "build" / ("nakagawa_player.exe" if windows else "nakagawa_player")


def _build_hint() -> str:
    return "`mingw32-make player`" if IS_WINDOWS else "`make player`"


def _platform_name() -> str:
    return "windows" if IS_WINDOWS else sys.platform


def app_icon() -> bytes:
    """The committed app icon, the one the player executable carries."""
    returned: bytes = (ROOT / make_app_icon.OUTPUT).read_bytes()
    return returned


_GIT_FAILED = "INSTALL_SOURCE_EXPORT_FAILED"


def _git(repo: Path, *arguments: str, binary: bool = False):
    try:
        result = subprocess.run(["git", "-C", str(repo), *arguments], capture_output=True,
                                check=False)
    except OSError as exc:  # git missing or not runnable: a FAIL line, not a traceback
        raise InstallError(f"{_GIT_FAILED}: git {arguments[0]} could not run: {exc}") from exc
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", "replace").strip()
        raise InstallError(f"{_GIT_FAILED}: git {arguments[0]} failed: {detail}")
    return result.stdout if binary else result.stdout.decode("utf-8", "replace").strip()


def export_source(repo: Path, destination: Path, revision: str = "HEAD") -> str:
    """Extract the committed tree at ``revision`` into ``destination``; return its SHA."""
    commit = _git(repo, "rev-parse", "--verify", f"{revision}^{{commit}}")
    archive = _git(repo, "archive", "--format=tar", commit, binary=True)
    destination.mkdir(parents=True, exist_ok=False)
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as tar:
        tar.extractall(destination, filter="data")
    if not (destination / "tools" / "nk_cli.py").is_file():
        raise InstallError("INSTALL_SOURCE_EXPORT_FAILED: the export has no tools/nk_cli.py")
    returned: str = commit
    return returned


PLAYER_BUILD_BUILT_FOR = "BUILT_FOR"
PLAYER_BUILD_BUILT_FOR_OTHER = "BUILT_FOR_OTHER"
PLAYER_BUILD_UNKNOWN = "UNKNOWN"
#: Stamp file mk/pipeline.mk writes beside the player:
#: ``$(BUILD_ROOT)/.player-identity-<commit|none>-<epoch|none>``.
_PLAYER_IDENTITY_STAMP = re.compile(r"\.player-identity-([0-9a-fA-F]{7,64}|none)-(none|[0-9]+)")


class PlayerBuildIdentity(NamedTuple):
    """What the player build folder's identity stamps say about ``expected_commit``."""

    status: str
    commit: str | None = None


def _commits_match(left: str, right: str) -> bool:
    left_norm = left.strip().lower()
    right_norm = right.strip().lower()
    if not left_norm or not right_norm:
        return False
    return left_norm.startswith(right_norm) or right_norm.startswith(left_norm)


def player_build_identity(player: Path, expected_commit: str) -> PlayerBuildIdentity:
    """Compare ``player``'s Makefile identity stamp(s) against ``expected_commit``.

    Reads ``.player-identity-<commit|none>-<epoch|none>`` files in ``player.parent``
    (written by ``mk/pipeline.mk``):
    - ``BUILT_FOR``: the newest stamp not newer than ``player`` names ``expected_commit``
      (compared by prefix).
    - ``BUILT_FOR_OTHER``: the newest stamp not newer than ``player`` names a different
      commit (returned as ``commit``), or every commit-bearing stamp beside ``player`` is
      newer than ``player`` (the rebuild did not finish; ``commit`` is ``None``).
    - ``UNKNOWN``: no commit-bearing stamp beside ``player`` (for example, a ``--player``
      outside a Make build folder, a CMake build, or a ``none`` no-commit build).
    """
    expected = expected_commit.strip().lower()
    if not expected:
        return PlayerBuildIdentity(PLAYER_BUILD_UNKNOWN, None)
    try:
        exe_mtime_ns = player.stat().st_mtime_ns
        with os.scandir(player.parent) as scanner:
            entries = list(scanner)
    except OSError:
        return PlayerBuildIdentity(PLAYER_BUILD_UNKNOWN, None)
    not_newer: list[tuple[int, str, str | None]] = []
    newer_with_commit = False
    for entry in entries:
        match = _PLAYER_IDENTITY_STAMP.fullmatch(entry.name)
        if not match:
            continue
        try:
            if not entry.is_file(follow_symlinks=False):
                continue
            stamp_mtime_ns = entry.stat(follow_symlinks=False).st_mtime_ns
        except OSError:
            continue
        raw_commit = match.group(1).lower()
        stamp_commit = None if raw_commit == "none" else raw_commit
        if stamp_mtime_ns <= exe_mtime_ns:
            not_newer.append((stamp_mtime_ns, entry.name, stamp_commit))
        elif stamp_commit is not None:
            newer_with_commit = True
    if not_newer:
        max_mtime = max(item[0] for item in not_newer)
        latest = sorted(item for item in not_newer if item[0] == max_mtime)
        commits = [item[2] for item in latest if item[2] is not None]
        if not commits:
            return PlayerBuildIdentity(PLAYER_BUILD_UNKNOWN, None)
        for commit in commits:
            if not _commits_match(commit, expected):
                return PlayerBuildIdentity(PLAYER_BUILD_BUILT_FOR_OTHER, commit)
        return PlayerBuildIdentity(PLAYER_BUILD_BUILT_FOR, commits[0])
    if newer_with_commit:
        return PlayerBuildIdentity(PLAYER_BUILD_BUILT_FOR_OTHER, None)
    return PlayerBuildIdentity(PLAYER_BUILD_UNKNOWN, None)


#: Inno Setup's uninstaller and its data, written beside the program by the Windows installer (#2208).
INSTALLER_UNINSTALL_FILES = ("unins000.exe", "unins000.dat")


def _check_not_installer_owned(dest: Path) -> None:
    """Refuse a folder the Windows installer set up. Staging into it or uninstalling it from here would leave
    Setup's uninstaller and its Apps & features entry pointing at a different program, or at nothing."""
    if any((dest / name).exists() for name in INSTALLER_UNINSTALL_FILES):
        raise InstallError(
            f"INSTALL_DESTINATION_INSTALLER_OWNED: {dest} was set up by the Windows installer (Setup.exe), "
            "which has its own uninstall in Apps & features; two uninstallers must never share a folder. "
            "Choose another folder")


def _check_destination(dest: Path) -> None:
    if dest.exists() and not dest.is_dir():
        raise InstallError(f"INSTALL_DESTINATION_INVALID: {dest} is not a folder")
    _check_not_installer_owned(dest)
    if dest.is_dir() and any(dest.iterdir()) and not (dest / MARKER).is_file():
        raise InstallError(
            f"INSTALL_DESTINATION_NOT_OWNED: {dest} already holds files this tool did not "
            "create; choose an empty or new folder"
        )
    if dest.is_dir() and (dest / MARKER).is_file():
        # A file of that name is not proof by itself: staging replaces bin/ and source/, so
        # the marker must pass the checks uninstall applies before anything is renamed.
        _read_marker(dest)
    try:
        dest.resolve().relative_to(ROOT.resolve())
    except ValueError:
        return
    raise InstallError("INSTALL_DESTINATION_INVALID: the install must be outside the source tree")


def _carry_entries(old: Path, final: Path, carry: tuple[str, ...]) -> None:
    """Move each ``carry`` entry of ``old`` into ``final``. One both hold is refused untouched."""
    for name in carry:
        source = old / name
        if not (source.exists() or source.is_symlink()):
            continue
        target = final / name
        if target.exists() or target.is_symlink():
            raise InstallError(
                f"INSTALL_PORTABLE_DATA_CONFLICT: both {source} and {target} exist; a crashed update "
                "left two copies of the portable data. Keep the one you want, move the other out of "
                "the install, and run the install again")
        try:
            source.rename(target)
        except OSError as exc:
            raise InstallError(
                f"INSTALL_IN_USE: {source} could not be moved into {final} ({exc}); close the player "
                "and run the install again") from exc


def _replace_owned(staged: Path, final: Path, carry: tuple[str, ...] = ()) -> None:
    """Swap a freshly staged folder into place; the old copy is removed only after.

    The ``carry`` entries (the portable marker and data folder in ``bin/``) belong to the user:
    they move from the replaced copy into the new one before it is removed. A leftover ``.old``
    copy from a crashed update hands its entries on the same way, so an update never deletes them."""
    old = final.with_name(final.name + ".old")
    if old.exists():
        if carry and any((old / name).exists() or (old / name).is_symlink() for name in carry):
            if final.exists():
                _carry_entries(old, final, carry)
            else:
                # The crash came between the two renames: the leftover is the live copy.
                old.rename(final)
        if old.exists():
            shutil.rmtree(old)
    if final.exists():
        try:
            final.rename(old)
        except OSError as exc:
            raise InstallError(
                f"INSTALL_IN_USE: {final} could not be replaced ({exc}); close the player "
                "and run the install again"
            ) from exc
    staged.rename(final)
    if old.exists():
        if carry:
            _carry_entries(old, final, carry)
        shutil.rmtree(old)


def stage_install(dest: Path, player: Path, *, repo: Path = ROOT,
                  revision: str = "HEAD", stage_dlls=None,
                  allow_player_source_mismatch: bool = False) -> dict:
    """Stage bin/, source/ and the icon under ``dest``; return the install record."""
    dest = dest.resolve()
    player = player.resolve()
    if not player.is_file():
        raise InstallError(
            f"INSTALL_PLAYER_MISSING: {player} was not found; build it with {_build_hint()}"
        )
    _check_destination(dest)
    commit = _git(repo, "rev-parse", "--verify", f"{revision}^{{commit}}")
    identity = player_build_identity(player, commit)
    if identity.status == PLAYER_BUILD_BUILT_FOR_OTHER:
        if identity.commit is not None:
            detail = (
                f"{player} was built from commit {identity.commit}, not {commit}; "
                f"rebuild it with {_build_hint()} or pass --allow-player-source-mismatch"
            )
        else:
            detail = (
                f"{player} is older than the build identity stamp in {player.parent}, "
                f"so it was not built for {commit} (a rebuild did not finish); "
                f"rebuild it with {_build_hint()} or pass --allow-player-source-mismatch"
            )
        if not allow_player_source_mismatch:
            raise InstallError(f"INSTALL_PLAYER_SOURCE_MISMATCH: {detail}")
        print(f"stage_install: WARNING: INSTALL_PLAYER_SOURCE_MISMATCH: {detail}", file=sys.stderr)
    elif identity.status == PLAYER_BUILD_UNKNOWN:
        print(
            f"stage_install: WARNING: could not verify which commit {player} was built from "
            f"(no build identity stamp in {player.parent}); continuing against source {commit}",
            file=sys.stderr,
        )
    dest.mkdir(parents=True, exist_ok=True)
    # Shortcuts an earlier staging wrote stay recorded: staging again replaces bin/ and
    # source/, not the shortcuts outside dest.
    integration = _recorded_integration(dest)
    stage = dest / f".staging-{os.getpid()}"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir()
    try:
        bin_dir = stage / "bin"
        bin_dir.mkdir()
        shutil.copy2(player, bin_dir / player.name)
        # The PE DLL closure exists only for Windows; an injected stager still runs (tests).
        if IS_WINDOWS or stage_dlls is not None:
            stager = stage_dlls or (lambda target: stage_runtime_dlls.stage_runtime_dlls(
                target, include_ui_fonts=True, refresh=True, repo_root=repo,
                runtime_manifest=True))
            try:
                staged = stager(bin_dir)
            except stage_runtime_dlls.StageError as exc:
                raise InstallError(f"INSTALL_RUNTIME_STAGE_FAILED: {exc}") from exc
            runtime_stage = "STAGED"
        else:
            # No DLL closure (the system loader resolves shared libraries), but the player
            # still needs its UI fonts, the launcher music and the Redump list beside it,
            # as `make player` stages them on every host.
            try:
                staged = [
                    *stage_runtime_dlls.stage_ui_font_assets(bin_dir, repo_root=repo),
                    *stage_runtime_dlls.stage_launcher_music(bin_dir, repo_root=repo),
                    *stage_runtime_dlls.stage_redump_list(bin_dir, repo_root=repo),
                ]
            except stage_runtime_dlls.StageError as exc:
                raise InstallError(f"INSTALL_RUNTIME_STAGE_FAILED: {exc}") from exc
            runtime_stage = "SKIPPED_NOT_PE"
        commit = export_source(repo, stage / "source", commit)
        (stage / "nakagawa.ico").write_bytes(app_icon())
        record = {
            "schema_version": SCHEMA_VERSION,
            "source_commit": commit,
            "player_commit": identity.commit,
            "platform": _platform_name(),
            "player": f"bin/{player.name}",
            "runtime_stage": runtime_stage,
            "runtime_files": sorted(staged),
            "integration": integration,
            "staged_at": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        }
        if not (dest / MARKER).is_file():
            # A first install is marked before its swaps: one that fails between them leaves
            # bin/ without source/, and only a marked folder can be staged again or uninstalled.
            _write_marker(dest, record)
        _replace_owned(bin_dir, dest / "bin", carry=(PORTABLE_DATA_LEAF, PORTABLE_MARKER))
        _replace_owned(stage / "source", dest / "source")
        os.replace(stage / "nakagawa.ico", dest / "nakagawa.ico")
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    _write_marker(dest, record)
    return record


def _write_marker(dest: Path, record: dict) -> None:
    (dest / MARKER).write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


def _recorded_integration(dest: Path) -> list[str]:
    """The paths an earlier run recorded in ``dest``'s marker, or [] without a readable one."""
    try:
        record = json.loads((dest / MARKER).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []
    paths = record.get("integration") if isinstance(record, dict) else None
    return [str(path) for path in paths] if isinstance(paths, list) else []


def record_integration(dest: Path, paths) -> None:
    """Add the paths a shortcut wrote outside ``dest`` to the install marker, keeping the
    ones recorded before. ``--uninstall`` reads the list back and removes those files."""
    marker = dest.resolve() / MARKER
    record = json.loads(marker.read_text(encoding="utf-8"))
    kept = [str(path) for path in record.get("integration", [])]
    for path in paths:
        if str(path) not in kept:
            kept.append(str(path))
    record["integration"] = kept
    _write_marker(dest.resolve(), record)


def xdg_data_home() -> Path:
    """``$XDG_DATA_HOME``, or ``~/.local/share`` when it is unset or not an absolute path."""
    value = os.environ.get("XDG_DATA_HOME", "")
    if value and Path(value).is_absolute():
        return Path(value)
    return Path.home() / ".local" / "share"


def _exec_quote(value: str) -> str:
    """Quote one ``Exec=`` word as the Desktop Entry spec requires; ``%`` becomes ``%%``."""
    escaped = (value.replace("\\", "\\\\").replace('"', '\\"')
               .replace("`", "\\`").replace("$", "\\$"))
    return '"' + escaped.replace("%", "%%") + '"'


def write_desktop_entry(dest: Path, arguments: str = DEFAULT_ARGUMENTS) -> list[Path]:
    """Write the per-user application entry and its 256 px icon; return both paths."""
    dest = dest.resolve()
    data = xdg_data_home()
    entry = data / "applications" / DESKTOP_ENTRY_NAME
    icon = data / "icons" / "hicolor" / "256x256" / "apps" / DESKTOP_ICON_NAME
    player = dest / "bin" / "nakagawa_player"
    lines = [
        "[Desktop Entry]",
        "Type=Application",
        "Name=Nakagawa Recomp",
        "Comment=Nakagawa Recomp player",
        f"Exec={_exec_quote(str(player))} {arguments}".rstrip(),
        f"TryExec={player}",
        f"Path={dest / 'bin'}",
        f"Icon={icon}",
        f"StartupWMClass={APP_DESKTOP_ID}",
        "Terminal=false",
        "Categories=Game;",
        "",
    ]
    icon.parent.mkdir(parents=True, exist_ok=True)
    icon.write_bytes(make_app_icon.png_image(app_icon(), 256))
    entry.parent.mkdir(parents=True, exist_ok=True)
    entry.write_bytes("\n".join(lines).encode("utf-8"))
    return [entry, icon]


#: PowerShell writes redirected output in the console's OEM code page, which neither Python's
#: ANSI page nor UTF-8 decodes: a user folder with an accented letter came back garbled.
#: Every script here first switches its output to UTF-8, the encoding it is then read in.
_UTF8_OUTPUT = "[Console]::OutputEncoding = [System.Text.Encoding]::UTF8; "


def _run_powershell(script: str, code: str, env=None) -> subprocess.CompletedProcess[str]:
    """Run one PowerShell script with UTF-8 output; its stdout and stderr come back as text.

    The bytes are decoded here, not by ``subprocess``: on Windows its reader thread swallows a
    decoding error and leaves the output ``None``. Output that is not UTF-8 raises ``code``."""
    result = subprocess.run(
        [powershell_exe(), "-NoProfile", "-NonInteractive", "-Command", _UTF8_OUTPUT + script],
        capture_output=True, env=powershell_env(env), check=False)
    try:
        stdout = result.stdout.decode("utf-8")
        stderr = result.stderr.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise InstallError(f"{code}: PowerShell's output is not UTF-8 ({exc})") from exc
    return subprocess.CompletedProcess(result.args, result.returncode, stdout, stderr)


def _special_folder(name: str, code: str) -> Path:
    result = _run_powershell(f"[Environment]::GetFolderPath('{name}')", code)
    folder = result.stdout.strip()
    if result.returncode != 0 or not folder:
        raise InstallError(f"{code}: Windows did not report a {name} folder")
    return Path(folder)


def desktop_folder() -> Path:
    """The signed-in user's Desktop folder, wherever Windows has redirected it."""
    return _special_folder("Desktop", "INSTALL_DESKTOP_UNKNOWN")


def start_menu_folder() -> Path:
    """The signed-in user's Start menu Programs folder, where a shortcut shows in Start."""
    return _special_folder("Programs", "INSTALL_START_MENU_UNKNOWN")


# WScript.Shell cannot set a shortcut's AppUserModelID, so the shortcut's property store
# (System.AppUserModel.ID, PKEY {9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3},5) is written through
# SHGetPropertyStoreFromParsingName. Each PowerShell process that writes or reads a shortcut's
# AppUserModelID compiles it again (Add-Type).
_APP_ID_CSHARP = r"""
using System;
using System.Runtime.InteropServices;
public static class NkShortcutAppId {
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct PropertyKey { public Guid fmtid; public uint pid; }
    [StructLayout(LayoutKind.Explicit, Size = 24)]
    public struct PropVariant { [FieldOffset(0)] public ushort vt; [FieldOffset(8)] public IntPtr p; }
    [ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IPropertyStore {
        void GetCount(out uint count);
        void GetAt(uint index, out PropertyKey key);
        void GetValue(ref PropertyKey key, out PropVariant value);
        void SetValue(ref PropertyKey key, ref PropVariant value);
        void Commit();
    }
    [DllImport("shell32.dll", CharSet = CharSet.Unicode, PreserveSig = false)]
    static extern void SHGetPropertyStoreFromParsingName(string path, IntPtr bindContext,
        int flags, ref Guid iid, out IPropertyStore store);
    [DllImport("ole32.dll", PreserveSig = false)]
    static extern void PropVariantClear(ref PropVariant value);
    const ushort VT_LPWSTR = 31;
    static PropertyKey AppIdKey() {
        PropertyKey key;
        key.fmtid = new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3");
        key.pid = 5;
        return key;
    }
    static IPropertyStore Open(string path, int flags) {
        Guid iid = typeof(IPropertyStore).GUID;
        IPropertyStore store;
        SHGetPropertyStoreFromParsingName(path, IntPtr.Zero, flags, ref iid, out store);
        return store;
    }
    public static void Set(string path, string id) {
        IPropertyStore store = Open(path, 2);  // GPS_READWRITE
        try {
            PropertyKey key = AppIdKey();
            PropVariant value = new PropVariant();
            value.vt = VT_LPWSTR;
            value.p = Marshal.StringToCoTaskMemUni(id);
            try { store.SetValue(ref key, ref value); store.Commit(); }
            finally { Marshal.FreeCoTaskMem(value.p); }
        } finally { Marshal.ReleaseComObject(store); }
    }
    public static string Get(string path) {
        IPropertyStore store = Open(path, 0);  // GPS_DEFAULT
        try {
            PropertyKey key = AppIdKey();
            PropVariant value;
            store.GetValue(ref key, out value);
            try { return value.vt == VT_LPWSTR ? Marshal.PtrToStringUni(value.p) : ""; }
            finally { PropVariantClear(ref value); }
        } finally { Marshal.ReleaseComObject(store); }
    }
}
"""

_SHORTCUT_SCRIPT = (
    "$ErrorActionPreference = 'Stop'; "
    "$s = (New-Object -ComObject WScript.Shell).CreateShortcut($env:NK_LNK); "
    "$s.TargetPath = $env:NK_TARGET; $s.Arguments = $env:NK_ARGS; "
    "$s.WorkingDirectory = $env:NK_WORKDIR; $s.IconLocation = $env:NK_ICON + ',0'; "
    "$s.Description = 'Nakagawa Recomp'; $s.Save(); "
    "Add-Type -TypeDefinition $env:NK_APP_ID_CS; "
    "[NkShortcutAppId]::Set($env:NK_LNK, $env:NK_APP_ID)"
)

_READ_APP_ID_SCRIPT = (
    "$ErrorActionPreference = 'Stop'; "
    "Add-Type -TypeDefinition $env:NK_APP_ID_CS; "
    "[NkShortcutAppId]::Get($env:NK_LNK)"
)


def _shell_path(path: Path) -> str:
    """A path as the Windows shell parses it. MSYS2's Python joins with '/', and
    SHGetPropertyStoreFromParsingName refuses those (E_INVALIDARG)."""
    return str(path).replace("/", "\\")


def read_shortcut_app_id(shortcut: Path) -> str:
    """The AppUserModelID a .lnk carries, or "" when it has none (Windows only)."""
    if os.name != "nt":
        raise InstallError("INSTALL_SHORTCUT_UNSUPPORTED: shortcuts are read on Windows only")
    environment = {**os.environ, "NK_LNK": _shell_path(shortcut), "NK_APP_ID_CS": _APP_ID_CSHARP}
    result = _run_powershell(_READ_APP_ID_SCRIPT, "INSTALL_SHORTCUT_UNREADABLE", environment)
    if result.returncode != 0:
        raise InstallError(
            f"INSTALL_SHORTCUT_UNREADABLE: {shortcut}: {result.stderr.strip()}")
    return result.stdout.strip()


_READ_TARGET_SCRIPT = (
    "$ErrorActionPreference = 'Stop'; "
    "(New-Object -ComObject WScript.Shell).CreateShortcut($env:NK_LNK).TargetPath"
)


def read_shortcut_target(shortcut: Path) -> str:
    """The program a .lnk starts, or "" when it cannot be read (Windows only)."""
    if os.name != "nt":
        raise InstallError("INSTALL_SHORTCUT_UNSUPPORTED: shortcuts are read on Windows only")
    result = _run_powershell(_READ_TARGET_SCRIPT, "INSTALL_SHORTCUT_UNREADABLE",
                             {**os.environ, "NK_LNK": _shell_path(shortcut)})
    return result.stdout.strip() if result.returncode == 0 else ""


def _check_shortcut_not_another_copys(shortcut: Path, dest: Path) -> None:
    """Refuse to replace the shortcut of a different, existing copy (the Windows installer writes the same
    name with the same taskbar identity, so replacing it would break that copy's shortcut, and removing either
    copy would remove the other's). The same copy, a stale shortcut whose program is gone, and one that cannot
    be read are replaced."""
    if not shortcut.is_file():
        return
    target = read_shortcut_target(shortcut)
    if not target:
        print(f"stage_install: note: could not read where {shortcut} points, so it is replaced", file=sys.stderr)
        return
    ours = dest / "bin" / "nakagawa_player.exe"
    existing = Path(target)
    if existing.is_file() and os.path.normcase(os.path.realpath(existing)) != os.path.normcase(
            os.path.realpath(ours)):
        raise InstallError(
            f"INSTALL_SHORTCUT_OTHER_COPY: {shortcut} already starts another copy of Nakagawa ({existing}); "
            "writing it here would replace that copy's shortcut. Uninstall or delete that copy and its "
            "shortcut first, or choose another shortcut with --shortcut")


def write_shortcut(shortcut: Path, dest: Path, arguments: str = DEFAULT_ARGUMENTS) -> None:
    """Write a .lnk that starts bin/nakagawa_player.exe the way Explorer does, with the
    player's AppUserModelID. Refuses to replace another copy's shortcut."""
    if os.name != "nt":
        raise InstallError("INSTALL_SHORTCUT_UNSUPPORTED: shortcuts are written on Windows only")
    dest = dest.resolve()
    _check_shortcut_not_another_copys(shortcut, dest)
    environment = os.environ.copy()
    environment.update({
        "NK_LNK": _shell_path(shortcut),
        "NK_TARGET": str(dest / "bin" / "nakagawa_player.exe"),
        "NK_ARGS": arguments,
        "NK_WORKDIR": str(dest / "bin"),
        "NK_ICON": str(dest / "nakagawa.ico"),
        "NK_APP_ID": APP_USER_MODEL_ID,
        "NK_APP_ID_CS": _APP_ID_CSHARP,
    })
    result = _run_powershell(_SHORTCUT_SCRIPT, "INSTALL_SHORTCUT_FAILED", environment)
    if result.returncode != 0 or not shortcut.is_file():
        raise InstallError(
            f"INSTALL_SHORTCUT_FAILED: {shortcut} was not written: {result.stderr.strip()}")


# ---- Reproducible portable ZIP (#324) --------------------------------------

SUMS_NAME = "SHA256SUMS.txt"
RUNTIME_MANIFEST_PATH = f"bin/{stage_runtime_dlls.RUNTIME_MANIFEST_NAME}"
#: The earliest and latest times a ZIP (MS-DOS) timestamp can hold, in UTC.
ZIP_EPOCH_MIN = 315532800        # 1980-01-01T00:00:00Z
ZIP_EPOCH_MAX = 4354819198       # 2107-12-31T23:59:58Z
ZIP_FILE_MODE = 0o100644
ZIP_DIR_MODE = 0o040755
ZIP_COMPRESS_LEVEL = 9
_MSDOS_DIRECTORY = 0x10


def resolve_source_date_epoch(explicit: str | None = None, *, repo: Path = ROOT,
                              revision: str = "HEAD", env=None) -> int:
    """The archive time: ``explicit``, else ``$SOURCE_DATE_EPOCH``, else the commit's time.

    The value is a decimal Unix time (reproducible-builds.org); anything else is
    refused rather than replaced by the clock.
    """
    environ = os.environ if env is None else env
    value = explicit if explicit is not None else environ.get("SOURCE_DATE_EPOCH", "")
    origin = "--source-date-epoch" if explicit is not None else "SOURCE_DATE_EPOCH"
    if not value:
        value = _git(repo, "log", "-1", "--format=%ct", f"{revision}^{{commit}}")
        origin = f"the commit time of {revision}"
    value = value.strip()
    if not value.isdigit() or not value.isascii():
        raise InstallError(f"PORTABLE_EPOCH_INVALID: {origin} is not a decimal Unix time: {value!r}")
    return int(value)


def _zip_date_time(epoch: int) -> tuple[int, int, int, int, int, int]:
    return time.gmtime(min(max(epoch, ZIP_EPOCH_MIN), ZIP_EPOCH_MAX))[:6]


def _check_entry_name(relative: str) -> str:
    path = PurePosixPath(relative)
    if (not relative or path.is_absolute() or any(part in ("", ".", "..") for part in path.parts)
            or "\\" in relative or ":" in relative or "\0" in relative):
        raise InstallError(f"PORTABLE_STAGE_NAME_INVALID: {relative!r} cannot be a ZIP entry name")
    return relative


def _stage_tree(stage: Path) -> tuple[list[tuple[str, Path]], list[str]]:
    """Regular files and empty folders under ``stage``, by relative POSIX name.

    A symbolic link or junction anywhere in the stage is refused: a ZIP made on
    Windows cannot carry one, and following it would package bytes from outside.
    """
    files: list[tuple[str, Path]] = []
    empty: list[str] = []
    pending = [(stage, "")]
    while pending:
        folder, prefix = pending.pop()
        children = 0
        with os.scandir(folder) as entries:
            for entry in entries:
                children += 1
                relative = _check_entry_name(prefix + entry.name)
                if entry.name == ".git":
                    raise InstallError(
                        f"PORTABLE_STAGE_GIT_METADATA: {relative} is Git metadata, which differs on every "
                        "export (commit time, index timestamps); delete it from the stage before archiving")
                path = Path(entry.path)
                if entry.is_symlink() or path.is_junction():
                    raise InstallError(f"PORTABLE_STAGE_LINK: {relative} is a link; stage real files")
                if entry.is_dir(follow_symlinks=False):
                    pending.append((path, relative + "/"))
                elif entry.is_file(follow_symlinks=False):
                    files.append((relative, path))
                else:
                    raise InstallError(f"PORTABLE_STAGE_SPECIAL_FILE: {relative} is not a regular file")
        if children == 0 and prefix:
            empty.append(prefix)
    order = lambda name: name.encode("utf-8")  # noqa: E731 - one ordinal byte order everywhere
    return sorted(files, key=lambda item: order(item[0])), sorted(empty, key=order)


def write_sha256sums(stage: Path) -> int:
    """Rewrite ``SHA256SUMS.txt`` over every other file in ordinal path order; return the count."""
    files, _ = _stage_tree(stage)
    lines = [f"{sha256_file(path)}  {relative}\n" for relative, path in files if relative != SUMS_NAME]
    (stage / SUMS_NAME).write_bytes("".join(lines).encode("utf-8"))
    return len(lines)


def _zip_info(name: str, date_time, *, directory: bool, size: int = 0) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name, date_time)
    info.create_system = 3
    if directory:
        info.external_attr = (ZIP_DIR_MODE << 16) | _MSDOS_DIRECTORY
        info.compress_type = zipfile.ZIP_STORED
    else:
        info.external_attr = ZIP_FILE_MODE << 16
        info.compress_type = zipfile.ZIP_DEFLATED
        info.compress_level = ZIP_COMPRESS_LEVEL
        # Known before the write, so the Zip64 decision depends on the size alone.
        info.file_size = size
    return info


#: The attested toolchain record the release recipe copies to the stage root; its
#: ``candidate.commit`` is the source commit the package was built from (#324).
TOOLCHAIN_RECORD = "toolchain.json"
#: A full commit id: SHA-1 (40 hex digits) or SHA-256 (64), as tools/record_toolchain.py records it.
_COMMIT_SHA = re.compile(r"[0-9a-f]{40}(?:[0-9a-f]{24})?")
#: The ``--record`` file of a portable ZIP: what a release dry run compares (#324).
PORTABLE_RECORD_FORMAT = "nakagawa-portable-zip-record"
PORTABLE_RECORD_SCHEMA_VERSION = 1


def staged_source_commit(stage: Path) -> str:
    """The source commit the stage's ``toolchain.json`` names (``candidate.commit``)."""
    try:
        record = json.loads((Path(stage) / TOOLCHAIN_RECORD).read_text(encoding="utf-8"))
        commit = record["candidate"]["commit"]
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, KeyError, TypeError) as exc:
        raise InstallError(
            f"PORTABLE_SOURCE_COMMIT_MISSING: the stage's {TOOLCHAIN_RECORD} names no candidate "
            f"commit ({type(exc).__name__}: {exc})"
        ) from exc
    if not isinstance(commit, str) or not _COMMIT_SHA.fullmatch(commit):
        raise InstallError(
            f"PORTABLE_SOURCE_COMMIT_MISSING: the stage's {TOOLCHAIN_RECORD} candidate commit "
            f"{commit!r} is not a full lower-case commit id (40 or 64 hex digits)"
        )
    return commit


def write_portable_zip(stage: Path, zip_path: Path, *, source_date_epoch: int,
                       verify_runtime=None, expected_commit: str | None = None) -> dict:
    """Write ``stage`` as a byte-reproducible ZIP at ``zip_path``; return its summary.

    ``SHA256SUMS.txt`` is rewritten first and ``bin/runtime-manifest.json`` is
    verified (``verify_runtime`` replaces that check; tests pass their own).
    Entries carry no root folder, the layout ``ZipFile.CreateFromDirectory``
    produced for v0.0.1: one per file and one per empty folder. The summary
    names the source commit from the stage's ``toolchain.json``, which must be
    ``expected_commit`` when that is given.
    """
    stage = Path(stage).resolve()
    zip_path = Path(zip_path).resolve()
    if not stage.is_dir():
        raise InstallError(f"PORTABLE_STAGE_MISSING: {stage} is not a folder")
    if zip_path.exists():
        raise InstallError(f"PORTABLE_ZIP_EXISTS: {zip_path} already exists; write a new file")
    if zip_path.is_relative_to(stage):
        raise InstallError("PORTABLE_ZIP_INSIDE_STAGE: write the ZIP outside the staged folder")
    source_commit = staged_source_commit(stage)
    if expected_commit is not None and source_commit != expected_commit:
        raise InstallError(
            f"PORTABLE_SOURCE_COMMIT_MISMATCH: the stage was built from {source_commit}, "
            f"not from {expected_commit}"
        )
    bin_dir = stage / "bin"
    try:
        if verify_runtime is None:
            stage_runtime_dlls.verify_runtime_manifest(bin_dir)
        else:
            verify_runtime(bin_dir)
    except stage_runtime_dlls.StageError as exc:
        raise InstallError(f"PORTABLE_RUNTIME_UNVERIFIED: {exc}") from exc
    write_sha256sums(stage)
    files, empty = _stage_tree(stage)
    date_time = _zip_date_time(source_date_epoch)
    entries = [(name, path) for name, path in files] + [(name, None) for name in empty]
    entries.sort(key=lambda item: item[0].encode("utf-8"))
    zip_path.parent.mkdir(parents=True, exist_ok=True)
    partial = zip_path.with_name(zip_path.name + ".part")
    total = 0
    try:
        with zipfile.ZipFile(partial, "w", allowZip64=True) as archive:
            for name, path in entries:
                if path is None:
                    archive.writestr(_zip_info(name, date_time, directory=True), b"")
                    continue
                size = path.stat().st_size
                with path.open("rb") as source, \
                        archive.open(_zip_info(name, date_time, directory=False, size=size), "w") as output:
                    shutil.copyfileobj(source, output, 1 << 20)
                total += size
        os.replace(partial, zip_path)
    finally:
        partial.unlink(missing_ok=True)
    return {
        "zip": str(zip_path),
        "zip_bytes": zip_path.stat().st_size,
        "sha256": sha256_file(zip_path),
        "files": len(files),
        "empty_folders": len(empty),
        "unpacked_bytes": total,
        "source_date_epoch": source_date_epoch,
        "source_commit": source_commit,
        "sha256sums_sha256": sha256_file(stage / "SHA256SUMS.txt"),
        "zlib": zlib.ZLIB_RUNTIME_VERSION,
    }


def portable_zip_record(summary: dict) -> bytes:
    """The ``--record`` bytes of a portable ZIP summary: no local path, so two builds compare.

    It names the ZIP by file name with its size and SHA-256, the source commit, the
    SHA-256 of ``SHA256SUMS.txt`` (which covers every other file), the entry time and
    the zlib that deflated it. Two builds of one package give the same bytes.
    """
    record = {key: value for key, value in summary.items() if key != "zip"}
    record.update({
        "format": PORTABLE_RECORD_FORMAT,
        "schema_version": PORTABLE_RECORD_SCHEMA_VERSION,
        "zip_name": Path(summary["zip"]).name,
    })
    return (json.dumps(record, indent=2, sort_keys=True, ensure_ascii=True) + "\n").encode("ascii")


def resolve_commit(revision: str, *, repo: Path = ROOT) -> str:
    """The full commit id ``revision`` names in ``repo`` (this tool's own checkout by default)."""
    try:
        return str(_git(repo, "rev-parse", "--verify", f"{revision}^{{commit}}"))
    except InstallError as exc:
        detail = str(exc).removeprefix(f"{_GIT_FAILED}: ")
        raise InstallError(f"PORTABLE_REVISION_UNRESOLVED: {revision!r} names no commit in {repo}: "
                           f"{detail}") from exc


def _check_record_path(record: Path, zip_path: Path, stage: Path) -> Path:
    """Refuse a ``--record`` path that exists, is or holds the ZIP, or lies in the stage."""
    record = Path(record).resolve()
    zip_path = Path(zip_path).resolve()
    if record.is_relative_to(zip_path) or zip_path.is_relative_to(record):
        raise InstallError("PORTABLE_RECORD_IS_ZIP: --record and --portable-zip name the same path, "
                           "or one lies inside the other")
    if record.is_relative_to(Path(stage).resolve()):
        raise InstallError("PORTABLE_RECORD_INSIDE_STAGE: write the record outside the staged folder; "
                           "SHA256SUMS.txt would not list it")
    if record.exists():
        raise InstallError(f"PORTABLE_RECORD_EXISTS: {record} already exists; write a new file")
    return record


_VERSION = r"\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?"
_PLAYER_ZIP = re.compile(rf"nakagawa-recomp-({_VERSION})-(windows-x64)\.zip")
_PLAYER_TAR = re.compile(rf"nakagawa-recomp-({_VERSION})-(linux-x64)\.tar\.gz")
_SOURCE_ZIP = re.compile(rf"nakagawa-recomp-({_VERSION})-prerequisite-sources\.zip")
# The Windows installer (#2208): a second file for the platform that sits beside the ZIP, never in place of it.
_SETUP_EXE = re.compile(rf"nakagawa-recomp-({_VERSION})-(windows-x64)-setup\.exe")
# What a catalogue launcher drops before it ranks archives: checksum, signature, source and
# debug files. Every released file that is not the player archive carries one of these words.
_AUXILIARY_TOKENS = ("sha256", "checksum", ".minisig", ".sig", ".asc", "source", "src", "debug", "symbols",
                     ".pdb", "sbom")
_ARCHIVE_SUFFIXES = (".zip", ".7z", ".tar", ".tar.gz", ".tgz", ".tar.xz", ".tar.zst", ".exe", ".msi", ".dmg",
                     ".appimage", ".deb", ".rpm")


def is_auxiliary_asset(name: str) -> bool:
    """True when a name filter that skips checksum, signature, source and debug files drops ``name``."""
    lowered = name.lower()
    return any(token in lowered for token in _AUXILIARY_TOKENS)


def check_release_assets(names) -> list[str]:
    """Return the problems with a release's file names; empty when the contract holds (#1876).

    One ``nakagawa-recomp-<version>-windows-x64.zip`` and optionally one
    ``nakagawa-recomp-<version>-linux-x64.tar.gz``, each with its ``.sha256`` (and optionally that
    file's ``.minisig``); the prerequisite source archive with its
    ``.sha256``; optionally the platform's installer ``nakagawa-recomp-<version>-windows-x64-setup.exe``
    with its ``.sha256`` (and optionally ``.minisig``), only beside that platform's ZIP; one version
    throughout; and nothing else a launcher's name filter would keep as a second candidate.

    The installer is not a name a filter drops. A launcher ranks the archive formats it can install and
    puts a ZIP ahead of an ``.exe`` (Quiver Launcher's ``FORMATS``: ``.zip`` first, ``.exe`` near the
    end), so the ZIP stays the one candidate and the installer is for people on the release page.
    """
    names = sorted(set(names))
    problems: list[str] = []
    players: dict[str, list[str]] = {}
    setups: list[re.Match[str]] = []
    versions: set[str] = set()
    for name in names:
        player = _PLAYER_ZIP.fullmatch(name) or _PLAYER_TAR.fullmatch(name)
        source = _SOURCE_ZIP.fullmatch(name)
        setup = _SETUP_EXE.fullmatch(name)
        if player:
            players.setdefault(player[2], []).append(name)
        if setup:
            setups.append(setup)
        matched = player or source or setup
        if matched:
            versions.add(matched[1])
            continue
        checked = re.fullmatch(r"(nakagawa-recomp-.+\.(?:zip|exe|tar\.gz))\.sha256(\.minisig)?", name)
        if checked and any(pattern.fullmatch(checked[1]) for pattern in
                           (_PLAYER_ZIP, _PLAYER_TAR, _SOURCE_ZIP, _SETUP_EXE)):
            # A signature signs the checksum file, so it needs the checksum and the archive both.
            needed = [checked[1]] + ([f"{checked[1]}.sha256"] if checked[2] else [])
            problems += [f"ASSET_ORPHAN: {name} has no {other}" for other in needed if other not in names]
        elif name.lower().endswith(_ARCHIVE_SUFFIXES) and not is_auxiliary_asset(name):
            problems.append(f"ASSET_SECOND_ARCHIVE: {name} would compete with the player archive; name it "
                            "nakagawa-recomp-<version>-windows-x64.zip or nakagawa-recomp-<version>-linux-x64.tar.gz, "
                            "or name it as a source, checksum or debug file")
        else:
            problems.append(f"ASSET_NAME_UNKNOWN: {name} is not a name the release contract lists")
    if "windows-x64" not in players:
        problems.append("ASSET_PLAYER_MISSING: no nakagawa-recomp-<version>-windows-x64.zip")
    for platform, found in players.items():
        if len(found) > 1:
            problems.append(f"ASSET_PLATFORM_DUPLICATE: {platform} has {len(found)} archives: {', '.join(found)}")
        for archive in found:
            if f"{archive}.sha256" not in names:
                problems.append(f"ASSET_CHECKSUM_MISSING: {archive}.sha256")
    for setup in setups:
        installer = setup.group(0)
        zips = [_PLAYER_ZIP.fullmatch(archive) for archive in players.get(setup[2], [])]
        if not any(found and found[1] == setup[1] for found in zips):
            problems.append(f"ASSET_INSTALLER_WITHOUT_ZIP: {installer} needs {setup[2]} ZIP of the same version "
                            "beside it; the installer is never the only download")
        if f"{installer}.sha256" not in names:
            problems.append(f"ASSET_CHECKSUM_MISSING: {installer}.sha256")
    sources = [name for name in names if _SOURCE_ZIP.fullmatch(name)]
    if not sources:
        problems.append("ASSET_SOURCE_MISSING: no nakagawa-recomp-<version>-prerequisite-sources.zip; the ZIP "
                        "bundles prerequisites and is published only with their source archive")
    elif len(sources) > 1:
        problems.append(f"ASSET_SOURCE_DUPLICATE: {', '.join(sources)}")
    problems += [f"ASSET_CHECKSUM_MISSING: {name}.sha256" for name in sources if f"{name}.sha256" not in names]
    if len(versions) > 1:
        problems.append(f"ASSET_VERSION_MIXED: {', '.join(sorted(versions))}")
    kept = [name for name in names if not is_auxiliary_asset(name) and name.lower().endswith(_ARCHIVE_SUFFIXES)
            and not _SETUP_EXE.fullmatch(name)]
    if sorted(kept) != sorted(n for found in players.values() for n in found):
        problems.append("ASSET_FILTER_MISMATCH: a name filter that drops checksum, signature, source and debug "
                        f"files would leave {kept}, not the player archives")
    return problems


#: What this tool writes directly under ``dest`` (besides the marker). ``--uninstall`` removes
#: these and nothing else, so a file the user put beside them survives.
OWNED_ENTRIES = ("bin", "source", "nakagawa.ico")
#: The file names a recorded integration path may have: the ones this tool writes.
INTEGRATION_NAMES = frozenset({SHORTCUT_NAME, DESKTOP_ENTRY_NAME, DESKTOP_ICON_NAME})
#: In portable mode these stay in ``bin/`` (``NK_PORTABLE_MARKER`` and ``NK_PORTABLE_DATA_LEAF``
#: in src/core/nk_portable.h): the marker the user created and their data folder.
PORTABLE_MARKER = "portable.txt"
PORTABLE_DATA_LEAF = "data"


def _remove_path(path: Path) -> None:
    """Delete a file, link or folder tree; a link is unlinked, never followed."""
    try:
        if path.is_junction():
            os.rmdir(path)  # removes the junction itself; its target is never entered
        elif path.is_symlink() or path.is_file():
            path.unlink()
        elif path.is_dir():
            shutil.rmtree(path)
    except OSError as exc:
        raise InstallError(
            f"UNINSTALL_IN_USE: {path} could not be removed ({exc}); close the player and run "
            "the uninstall again") from exc


#: The keys every marker this tool wrote has (``stage_install`` writes more; they are checked
#: when present).
MARKER_REQUIRED_KEYS = ("schema_version", "source_commit", "platform", "player")


def _plain_name(value) -> bool:
    """A single file name: no folder part, no drive, no ``.`` or ``..``."""
    return (isinstance(value, str) and value not in ("", ".", "..")
            and not any(char in value for char in '/\\:\0'))


def _runtime_file_path(value: object) -> bool:
    """A ``runtime_files`` entry as ``stage_install`` records it: a file's path under ``bin/``, plain names
    joined by ``/`` (``SDL3.dll``, ``assets/redump/redump-psp.zip``). An absolute path, a drive or any other
    colon, a backslash, a NUL and an empty, ``.`` or ``..`` segment are all refused."""
    return isinstance(value, str) and all(_plain_name(segment) for segment in value.split("/"))


def _read_marker(dest: Path) -> dict:
    """The marker of ``dest`` once it passes the checks that make it this tool's own.

    A file named ``.nakagawa-install.json`` is not proof by itself: any folder can hold one.
    It must be a JSON object with the keys ``stage_install`` writes, and every path it lists
    must stay inside ``dest``. Anything else raises ``INSTALL_MARKER_INVALID`` and nothing
    has been touched."""
    marker = dest / MARKER

    def refuse(why: str) -> NoReturn:
        raise InstallError(
            f"INSTALL_MARKER_INVALID: {marker} {why}, so this tool will not trust it; nothing "
            "was removed. If it is not yours, delete the folder by hand")

    try:
        record = json.loads(marker.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        refuse(f"cannot be read as JSON ({exc})")
    if not isinstance(record, dict):
        refuse("is not a JSON object")
    missing = [key for key in MARKER_REQUIRED_KEYS if key not in record]
    if missing:
        refuse(f"lacks {', '.join(missing)}")
    version = record["schema_version"]
    if not isinstance(version, int) or isinstance(version, bool) or not 1 <= version <= SCHEMA_VERSION:
        refuse(f"has schema_version {version!r}, not 1 to {SCHEMA_VERSION}")
    player = record["player"]
    if not (isinstance(player, str) and player.startswith("bin/") and _plain_name(player[4:])):
        refuse(f"lists the player {player!r}, not bin/<file name>")
    files = record.get("runtime_files", [])
    if not isinstance(files, list):
        refuse("has runtime_files that is not a list")
    unsafe = [name for name in files if not _runtime_file_path(name)]
    if unsafe:
        refuse(f"lists the runtime file {unsafe[0]!r}, not a relative path of plain names inside bin/")
    integration = record.get("integration", [])
    if not isinstance(integration, list) or not all(isinstance(path, str) for path in integration):
        refuse("lists an integration entry that is not a path")
    root = dest.resolve()
    for relative in [player, *(f"bin/{name}" for name in files)]:
        try:
            (root / relative).resolve().relative_to(root)
        except ValueError:
            refuse(f"lists {relative}, which resolves outside {root}")
    return record


def _check_uninstall_destination(dest: Path) -> None:
    """Refuse a folder no install of this tool would be in: a drive root, the home folder, or
    a folder holding the home folder."""
    home = Path.home().resolve()
    if dest.parent == dest or dest == home or dest in home.parents:
        raise InstallError(
            f"UNINSTALL_DESTINATION_REFUSED: {dest} is a drive root or holds the home folder; "
            "name the folder the install was staged into")


def _starts_this_install(shortcut: Path, player: Path) -> bool:
    """True for a Windows .lnk file that starts ``player``: the one proof that a recorded shortcut whose name
    this tool does not always write (``--shortcut <path>``) is its own."""
    if not IS_WINDOWS or shortcut.suffix.lower() != ".lnk" or not shortcut.is_file():
        return False
    target = read_shortcut_target(shortcut)
    return bool(target) and os.path.normcase(os.path.realpath(target)) == os.path.normcase(
        os.path.realpath(player))


def plan_uninstall(dest: Path) -> dict:
    """What ``uninstall`` would remove and keep, without touching anything.

    Raises ``INSTALL_DESTINATION_NOT_OWNED`` for a folder without this tool's marker and
    ``INSTALL_MARKER_INVALID`` for one whose marker is not what ``stage_install`` writes."""
    dest = dest.resolve()
    _check_uninstall_destination(dest)
    _check_not_installer_owned(dest)
    if not (dest / MARKER).is_file():
        raise InstallError(
            f"INSTALL_DESTINATION_NOT_OWNED: {dest} has no {MARKER}, so this tool did not "
            "create it; remove an unpacked portable ZIP by deleting its folder")
    record = _read_marker(dest)
    remove: list[Path] = []
    keep: list[Path] = []
    portable = (dest / "bin" / PORTABLE_MARKER).is_file()
    for name in OWNED_ENTRIES:
        entry = dest / name
        if not (entry.exists() or entry.is_symlink()):
            continue
        if name == "bin" and portable and entry.is_dir() and not entry.is_symlink() and not entry.is_junction():
            # Portable mode: the program goes, the user's data and their marker stay.
            for child in sorted(entry.iterdir()):
                if child.name in (PORTABLE_DATA_LEAF, PORTABLE_MARKER):
                    keep.append(child)
                else:
                    remove.append(child)
        else:
            remove.append(entry)
    leftovers = {f"{name}.old" for name in OWNED_ENTRIES}
    for child in sorted(dest.iterdir()):
        # A junction is not is_symlink() on Windows; it is listed itself and removed as a link,
        # never searched for portable data that would be another folder's files.
        if (child.name == "bin.old" and child.is_dir() and not child.is_symlink()
                and not child.is_junction()):
            # A crashed update can leave the previous bin/, with the user's portable data in it.
            inner = sorted(child.iterdir())
            if any(item.name in (PORTABLE_DATA_LEAF, PORTABLE_MARKER) for item in inner):
                for item in inner:
                    (keep if item.name in (PORTABLE_DATA_LEAF, PORTABLE_MARKER) else remove).append(item)
            else:
                remove.append(child)
        elif child.name.startswith(".staging-") or child.name in leftovers:
            remove.append(child)
    integration: list[Path] = []
    skipped: list[str] = []
    for recorded in record.get("integration", []):
        path = Path(recorded)
        if not path.is_absolute() or path.is_dir() or path.is_symlink():
            skipped.append(recorded)
        elif path.name in INTEGRATION_NAMES:
            if path.exists():
                integration.append(path)
        elif _starts_this_install(path, dest / record["player"]):
            # A --shortcut of the user's own name: removed only while it starts this install.
            integration.append(path)
        else:
            skipped.append(recorded)
    return {"dest": dest, "remove": remove, "keep": keep, "integration": integration,
            "skipped_integration": skipped, "portable": portable}


def uninstall(dest: Path, *, dry_run: bool = False) -> dict:
    """Remove an install this tool made; the module docstring says exactly what goes."""
    plan = plan_uninstall(dest)
    dest = plan["dest"]
    left_in_folder: list[str] = []
    if not dry_run:
        # The install first: if it is in use the shortcuts stay, still recorded, and a second
        # run finishes. The marker goes last so a half-done uninstall can be run again.
        for path in plan["remove"]:
            _remove_path(path)
        for path in plan["integration"]:
            _remove_path(path)
        if plan["keep"]:
            # The user's portable data lives here, so the install marker stays with it.
            left_in_folder = sorted(child.name for child in dest.iterdir())
        else:
            (dest / MARKER).unlink()
            left_in_folder = sorted(child.name for child in dest.iterdir())
            if not left_in_folder:
                dest.rmdir()
    return {"dest": str(dest), "dry_run": dry_run,
            "removed": [str(path) for path in plan["remove"]],
            "removed_integration": [str(path) for path in plan["integration"]],
            "skipped_integration": plan["skipped_integration"],
            "kept": [str(path) for path in plan["keep"]],
            "left_in_folder": left_in_folder}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--dest", type=Path,
                        help="install folder outside the source tree")
    parser.add_argument("--portable-zip", type=Path,
                        help="instead of installing, write this reproducible portable ZIP from "
                             "--zip-stage")
    parser.add_argument("--check-assets", nargs="+", metavar="PATH",
                        help="check a release folder's file names (or the file names given) against the "
                             "one-archive-per-platform contract")
    parser.add_argument("--zip-stage", type=Path,
                        help="the staged release folder --portable-zip archives")
    parser.add_argument("--record", type=Path,
                        help="with --portable-zip, also write the ZIP's record (file name, size, SHA-256, "
                             "source commit) to this new file, the evidence a release dry run compares")
    parser.add_argument("--source-date-epoch",
                        help="ZIP entry time as a Unix time (default: $SOURCE_DATE_EPOCH, else "
                             "the commit time of --revision)")
    default_player = default_player_path(IS_WINDOWS)
    parser.add_argument("--player", type=Path, default=default_player,
                        help=f"built player executable (default: build/{default_player.name})")
    parser.add_argument("--revision", default="HEAD",
                        help="committed revision exported as source/; with --portable-zip, the commit the "
                             "stage's toolchain.json must name (default: HEAD)")
    parser.add_argument("--allow-player-source-mismatch", action="store_true",
                        help="warn instead of stopping when the player executable's build identity "
                             "stamp does not match the source commit being exported")
    parser.add_argument("--desktop-shortcut", action="store_true",
                        help=f"also write '{SHORTCUT_NAME}' on the Desktop (Windows), or a "
                             f"'{DESKTOP_ENTRY_NAME}' entry (Linux)")
    parser.add_argument("--start-menu-shortcut", action="store_true",
                        help=f"also write '{SHORTCUT_NAME}' in the Start menu (Windows); on Linux "
                             f"the same '{DESKTOP_ENTRY_NAME}' entry, which is the application menu")
    parser.add_argument("--shortcut", type=Path,
                        help="write the Windows shortcut at this exact .lnk path instead")
    parser.add_argument("--arguments", default=DEFAULT_ARGUMENTS,
                        help="player arguments in the shortcut (default: none)")
    parser.add_argument("--uninstall", action="store_true",
                        help="remove the install in --dest and the shortcuts and desktop entries "
                             "it recorded; never the per-user data folders")
    parser.add_argument("--dry-run", action="store_true",
                        help="with --uninstall, list what would be removed and remove nothing")
    args = parser.parse_args(argv)
    if args.record is not None and args.portable_zip is None:
        parser.error("--record goes with --portable-zip")
    if args.dry_run and not args.uninstall:
        parser.error("--dry-run goes with --uninstall")
    if args.uninstall:
        if args.dest is None or args.portable_zip is not None or args.zip_stage is not None:
            parser.error("--uninstall needs --dest and cannot be combined with --portable-zip")
        try:
            summary = uninstall(args.dest, dry_run=args.dry_run)
        except InstallError as exc:
            print(f"stage_install: FAIL: {exc}", file=sys.stderr)
            return 1
        print(json.dumps({"event": "uninstall", **summary}, sort_keys=True))
        return 0
    if args.check_assets is not None:
        paths = [Path(item) for item in args.check_assets]
        if len(paths) == 1 and paths[0].is_dir():
            names = [child.name for child in paths[0].iterdir()]
        else:
            names = [item.name for item in paths]
        problems = check_release_assets(names)
        for problem in problems:
            print(f"stage_install: FAIL: {problem}", file=sys.stderr)
        return 1 if problems else 0
    if args.portable_zip is not None or args.zip_stage is not None:
        if args.portable_zip is None or args.zip_stage is None or args.dest is not None:
            parser.error("--portable-zip and --zip-stage go together, without --dest")
        try:
            record_path = (None if args.record is None
                           else _check_record_path(args.record, args.portable_zip, args.zip_stage))
            expected_commit = resolve_commit(args.revision)
            epoch = resolve_source_date_epoch(args.source_date_epoch, revision=args.revision)
            summary = write_portable_zip(args.zip_stage, args.portable_zip, source_date_epoch=epoch,
                                         expected_commit=expected_commit)
            if record_path is not None:
                try:
                    record_path.parent.mkdir(parents=True, exist_ok=True)
                    with record_path.open("xb") as output:
                        output.write(portable_zip_record(summary))
                except OSError as exc:
                    raise InstallError(f"PORTABLE_RECORD_UNWRITTEN: {args.record}: {exc}; the ZIP "
                                       f"{summary['zip']} was written, its record was not") from exc
        except InstallError as exc:
            print(f"stage_install: FAIL: {exc}", file=sys.stderr)
            return 1
        print(json.dumps({"event": "portable-zip", **summary}, sort_keys=True))
        return 0
    if args.dest is None:
        parser.error("--dest is required unless --portable-zip is given")
    try:
        if args.shortcut is not None and not IS_WINDOWS:
            raise InstallError("INSTALL_SHORTCUT_UNSUPPORTED: --shortcut writes a Windows .lnk; "
                               "on Linux use --desktop-shortcut")
        shortcuts: list[Path] = []
        if IS_WINDOWS:
            if args.shortcut is not None:
                # Absolute, as uninstall accepts only an absolute recorded path.
                shortcuts.append(args.shortcut.absolute())
            else:
                if args.desktop_shortcut:
                    shortcuts.append(desktop_folder() / SHORTCUT_NAME)
                if args.start_menu_shortcut:
                    shortcuts.append(start_menu_folder() / SHORTCUT_NAME)
            # Every requested shortcut is checked before anything is staged or written, so a refusal leaves
            # nothing half done.
            for shortcut in shortcuts:
                _check_shortcut_not_another_copys(shortcut, args.dest.resolve())
        record = stage_install(
            args.dest, args.player, revision=args.revision,
            allow_player_source_mismatch=args.allow_player_source_mismatch,
        )
        print(f"stage_install: staged {record['player']} and source {record['source_commit'][:12]} "
              f"into {args.dest.resolve()}")
        if not IS_WINDOWS:
            if args.desktop_shortcut or args.start_menu_shortcut:
                entry, icon = write_desktop_entry(args.dest, args.arguments)
                record_integration(args.dest, [entry, icon])
                print(f"stage_install: desktop entry {entry}")
        else:
            for shortcut in shortcuts:
                write_shortcut(shortcut, args.dest, args.arguments)
                # Recorded as each is written, so --uninstall removes it even if a later one fails.
                record_integration(args.dest, [shortcut])
                print(f"stage_install: shortcut {shortcut}")
    except InstallError as exc:
        print(f"stage_install: FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
