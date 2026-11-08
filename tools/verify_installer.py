# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Install the Windows installer into a temp folder, check it, and uninstall it (#2208, #324).

The check a built installer has to pass before it is a release asset:

1. a silent per-user install into a temp folder (no administrator);
2. every installed file equals the release package's ``SHA256SUMS.txt`` (the installer carries the
   ZIP's bytes), and nothing else but Inno's own ``unins000.*`` is there;
3. the Start-menu shortcut exists and carries the player's taskbar identity (``Nakagawa.Recomp``);
4. the uninstall entry is under HKCU only, never HKLM;
5. a silent uninstall removes the shortcut, the entry and the program, keeps a portable ``bin/data``,
   ``bin/portable.txt``, a file the person added, and the per-user data: before anything is installed the tool
   plants marker files in all five data folders under ``%LOCALAPPDATA%\\Nakagawa`` (``config``, ``data``,
   ``cache``, ``logs`` and ``saves``, plus a nested folder in ``data`` and ``saves``), creating any folder that is
   missing, checks that every marker is still there, and at the end removes the markers and the folders it made;
6. an upgrade in place: it builds an older synthetic release with the pinned Inno Setup compiler (the package plus
   files a newer release dropped), installs it, adds files and changes one installed file, then runs this
   installer over it. The folder must then equal this package's ``SHA256SUMS.txt`` plus exactly the files the
   person added or changed: the dropped files and their emptied folder are gone. A silent uninstall then keeps
   the person's files and the markers again.

WARNING: it plants and removes marker files in the real ``%LOCALAPPDATA%\\Nakagawa`` folders, and writes the real
per-user uninstall entry and Start-menu shortcut while it runs, because the installer uses those places and not an
environment variable. It only adds and removes its own files, but never run it where real Nakagawa data lives: use
a throwaway Windows user account, Windows Sandbox or a virtual machine (the hosted CI job runs it on a fresh
runner). If an installer under test ever deleted that data on a silent uninstall, this check would be the one that
lost it.

It refuses to run when a shortcut of the name it tests or the installer's uninstall entry already exists, so a PC
with an installed copy keeps it. Windows only; nothing here uses the network.

    python tools/verify_installer.py --installer <setup.exe> --package-dir <unpacked ZIP> [--iscc <ISCC.exe>]
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Callable

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import build_installer  # noqa: E402
import stage_install  # noqa: E402

#: The shortcut a release installer writes (nakagawa.iss, ShortcutName).
DEFAULT_SHORTCUT_NAME = "Nakagawa Recomp"
APP_ID = "A37F65B8-FB5B-41BB-AC7B-3700FE43CB02"
UNINSTALL_KEY = rf"Software\Microsoft\Windows\CurrentVersion\Uninstall\{{{APP_ID}}}_is1"
SENTINEL_NAME = "installer-verify-sentinel.txt"
#: Inno's own files, beside the program in the install folder.
INNO_OWN_PREFIX = "unins"


def compare_installed(package_dir: Path, installed_dir: Path) -> list[str]:
    """Differences between an install folder and the package it came from; empty when they match.

    ``unins000.*`` (Inno's uninstaller and its data) is the one allowed addition."""
    sums = build_installer.read_sums(package_dir)
    problems = []
    for problem in build_installer.compare_with_sums(installed_dir, sums):
        code, _, name = problem.partition(": ")
        if code == "UNLISTED" and "/" not in name and name.startswith(INNO_OWN_PREFIX):
            continue
        problems.append(problem)
    return problems


#: What the person adds beside the program; every install, upgrade and uninstall keeps it.
PERSON_FILES = ("bin/data/save.bin", "bin/portable.txt", "notes.txt")
#: The synthetic older release of the upgrade check: the package plus files a newer release dropped.
UPGRADE_VERSION = "0.0.0-upgrade-check"
UPGRADE_EPOCH = 1_700_000_000
UPGRADE_DROPPED_FOLDER = "installer-verify-dropped"
UPGRADE_DROPPED = ("bin/installer-verify-dropped.dll", f"{UPGRADE_DROPPED_FOLDER}/nested/old.txt")
#: Installed by the older release and then changed by the person, so it is no longer what Setup installed.
UPGRADE_EDITED = "bin/installer-verify-edited.txt"


def make_older_package(package_dir: Path, older_dir: Path) -> None:
    """A copy of the package that also holds ``UPGRADE_DROPPED`` and ``UPGRADE_EDITED``, with its own manifest."""
    shutil.copytree(package_dir, older_dir)
    for name in (*UPGRADE_DROPPED, UPGRADE_EDITED):
        path = older_dir / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"installed by the older release: {name}\n", encoding="utf-8")
    stage_install.write_sha256sums(older_dir)


def add_person_files(dest: Path) -> None:
    """A portable data folder, the portable switch and a note, as a person would leave them beside the program."""
    for name in PERSON_FILES:
        path = dest / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"the person's own file")


def upgrade_problems(package_dir: Path, dest: Path) -> list[str]:
    """What an upgrade over ``make_older_package``'s install got wrong; empty when it is right.

    Right is: the folder equals the new package's ``SHA256SUMS.txt`` (plus Inno's ``unins000.*``) and holds
    exactly the person's files and the installed file they changed, and the dropped files' folder is gone."""
    kept = {*PERSON_FILES, UPGRADE_EDITED}
    problems = [problem for problem in compare_installed(package_dir, dest)
                if not (problem.startswith("UNLISTED: ") and problem.removeprefix("UNLISTED: ") in kept)]
    problems += [f"REMOVED_KEPT_FILE: {name}" for name in sorted(kept) if not (dest / name).is_file()]
    if (dest / UPGRADE_DROPPED_FOLDER).exists():
        problems.append(f"LEFT_FOLDER: {UPGRADE_DROPPED_FOLDER}")
    return problems


def egress_findings(script_text: str) -> list[str]:
    """Names in an Inno script that reach the network. A deterministic text test, like the repo's other egress tests.

    The one COM object the script may create is Windows' local scripting shell (it reads the shortcut a
    previous install left); any other ``CreateOleObject`` could be an HTTP client and is a finding."""
    lowered = script_text.lower()
    names = ("downloadtemporaryfile", "itdownload", "itd_", "winhttp", "urlmon", "wininet",
             "shellexec('open', 'http", 'shellexec("open", "http')
    findings = [f"INSTALLER_NETWORK_CALL: {name}" for name in names if name in lowered]
    for match in re.finditer(r"createoleobject\s*\(([^)]*)\)", lowered):
        if match.group(1).strip() != "'wscript.shell'":
            findings.append(f"INSTALLER_NETWORK_CALL: createoleobject({match.group(1).strip()})")
    return findings


def setup_log_reason(log: Path) -> str:
    """Why Setup stopped, from its ``/LOG=`` file: the PrepareToInstall message when there is one (Setup exit 7),
    else the log's last lines. Empty when Setup wrote no log."""
    try:
        lines = log.read_text(encoding="utf-8-sig", errors="replace").splitlines()
    except OSError:
        return ""
    prepare = [line for line in lines if "PrepareToInstall" in line]
    shown = prepare or lines[-12:]
    return f" (Setup log {log.name}: " + " | ".join(line.strip() for line in shown) + ")" if shown else ""


def _hkcu_key_exists() -> bool:
    if sys.platform != "win32":
        return False
    import winreg
    try:
        winreg.CloseKey(winreg.OpenKey(winreg.HKEY_CURRENT_USER, UNINSTALL_KEY))
        return True
    except OSError:
        return False


def _hklm_key_exists() -> bool:
    if sys.platform != "win32":
        return False
    import winreg
    for view in (winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_32KEY):
        try:
            winreg.CloseKey(winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, UNINSTALL_KEY, 0, winreg.KEY_READ | view))
            return True
        except OSError:
            continue
    return False


def _wait_for(condition: Callable[[], bool], seconds: float = 90.0) -> bool:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if condition():
            return True
        time.sleep(0.5)
    return condition()


#: The per-user folders the uninstall opt-in may delete (assets/installer/nakagawa.iss, UserDataFolder).
USER_DATA_FOLDERS = ("config", "data", "cache", "logs", "saves")
NESTED_SENTINEL_FOLDERS = ("data", "saves")
SENTINEL_FOLDER_NAME = "installer-verify-nested"


class Sentinels:
    """Marker files planted in every per-user data folder, to prove an uninstall that was not asked to delete
    them kept them. Creates what is missing and removes only what it made."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.files: list[Path] = []
        self._created: list[Path] = []

    def _make_dir(self, path: Path) -> None:
        if not path.exists():
            path.mkdir()
            self._created.append(path)

    def plant(self) -> None:
        if self.root.is_symlink() or self.root.is_junction() or (self.root.exists() and not self.root.is_dir()):
            raise build_installer.InstallerError(f"INSTALLER_VERIFY_REFUSED: {self.root} is not a plain folder")
        self.root.parent.mkdir(parents=True, exist_ok=True)
        self._make_dir(self.root)
        for name in USER_DATA_FOLDERS:
            folder = self.root / name
            self._make_dir(folder)
            places = [folder]
            if name in NESTED_SENTINEL_FOLDERS:
                nested = folder / SENTINEL_FOLDER_NAME
                self._make_dir(nested)
                places.append(nested)
            for place in places:
                marker = place / SENTINEL_NAME
                marker.write_text("planted by tools/verify_installer.py; safe to delete\n", encoding="utf-8")
                self.files.append(marker)

    def missing(self) -> list[str]:
        """Planted markers that are gone; an empty list with nothing planted is itself a failure."""
        if not self.files:
            return ["no sentinel was planted"]
        return [str(path) for path in self.files if not path.is_file()]

    def remove(self) -> None:
        for path in self.files:
            path.unlink(missing_ok=True)
        for path in reversed(self._created):
            try:
                path.rmdir()
            except OSError:
                pass  # something else is in it: leave it


def _hold_open(path: Path):
    """Open ``path`` the way a running program's image is open: others may read it, none may write or delete it.
    Returns a callable that releases it. Windows only."""
    if sys.platform != "win32":
        raise build_installer.InstallerError("INSTALLER_VERIFY_UNSUPPORTED: the installer runs on Windows only")
    import ctypes
    from ctypes import wintypes
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateFileW.restype = wintypes.HANDLE
    kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                                     wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    generic_read, share_read, open_existing = 0x80000000, 0x1, 3
    handle = kernel32.CreateFileW(str(path), generic_read, share_read, None, open_existing, 0, None)
    if handle in (None, wintypes.HANDLE(-1).value):
        raise build_installer.InstallerError(f"INSTALLER_VERIFY_FAILED: cannot hold {path} open")
    return lambda: kernel32.CloseHandle(handle)


def verify_installer(installer: Path, package_dir: Path, *, shortcut_name: str = DEFAULT_SHORTCUT_NAME,
                     iscc: Path | None = None) -> dict:
    """Run the whole check; raise ``InstallerError`` on the first failure. Windows only.

    ``iscc`` is the pinned compiler for the upgrade leg's older installer (default: ``build_installer.find_iscc``)."""
    if os.name != "nt":
        raise build_installer.InstallerError("INSTALLER_VERIFY_UNSUPPORTED: the installer runs on Windows only")
    installer = installer.resolve()
    package_dir = package_dir.resolve()
    start_menu = stage_install.start_menu_folder()
    shortcut = start_menu / f"{shortcut_name}.lnk"
    if shortcut.exists() or _hkcu_key_exists():
        raise build_installer.InstallerError(
            f"INSTALLER_VERIFY_REFUSED: {shortcut} or the installer's uninstall entry already exists on this "
            "PC; the check would replace and then remove it. Run it in a throwaway Windows user account, "
            "Windows Sandbox or a virtual machine with no installed copy")
    local = os.environ.get("LOCALAPPDATA")
    if not local:
        raise build_installer.InstallerError("INSTALLER_VERIFY_REFUSED: LOCALAPPDATA is not set")
    # Marker files in every per-user data folder (made if missing, removed after): a silent uninstall must keep
    # them all. With none planted the check would prove nothing, so that is a failure of its own.
    sentinels = Sentinels(Path(local) / "Nakagawa")

    def fail(code: str, why: str):
        raise build_installer.InstallerError(f"{code}: {why}")

    def install(setup: Path, folder: Path, code: str) -> None:
        log = Path(temp) / f"setup-{code.lower()}.log"
        done = subprocess.run([str(setup), *silent_install, f"/DIR={folder}", f"/LOG={log}"], capture_output=True,
                              text=True, timeout=600, check=False)
        if done.returncode != 0:
            fail(code, f"{setup.name} exited {done.returncode}: {done.stdout}{done.stderr}{setup_log_reason(log)}")

    def check_shortcut_and_entry() -> None:
        if not shortcut.is_file():
            fail("INSTALLER_NO_SHORTCUT", f"{shortcut} was not created")
        app_id = stage_install.read_shortcut_app_id(shortcut)
        if app_id != stage_install.APP_USER_MODEL_ID:
            fail("INSTALLER_SHORTCUT_IDENTITY", f"the shortcut carries {app_id!r}, not {stage_install.APP_USER_MODEL_ID!r}")
        if not _hkcu_key_exists():
            fail("INSTALLER_NO_UNINSTALL_ENTRY", f"HKCU\\{UNINSTALL_KEY} is missing")
        if _hklm_key_exists():
            fail("INSTALLER_WROTE_HKLM", "an uninstall entry exists under HKLM: the install was not per user")

    def uninstall(folder: Path) -> list[str]:
        """Uninstall silently and return the files left in ``folder``."""
        subprocess.run([str(folder / "unins000.exe"), *silent_uninstall], capture_output=True, text=True,
                       timeout=600, check=False)
        if not _wait_for(lambda: not _hkcu_key_exists() and not shortcut.exists()):
            fail("INSTALLER_UNINSTALL_INCOMPLETE", "the uninstall entry or the shortcut is still there after 90 s")
        _wait_for(lambda: not (folder / "unins000.exe").exists(), 30.0)
        return sorted(str(path.relative_to(folder)).replace("\\", "/") for path in folder.rglob("*") if path.is_file())

    silent_install = ["/VERYSILENT", "/SUPPRESSMSGBOXES", "/CURRENTUSER", "/NORESTART", "/SP-", "/NOCANCEL"]
    silent_uninstall = ["/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"]
    result: dict = {"installer": str(installer), "shortcut": str(shortcut)}
    with tempfile.TemporaryDirectory(prefix="nk-installer-verify-") as temp:
        dest = Path(temp) / "Nakagawa Recomp"
        upgraded = Path(temp) / "Nakagawa Recomp upgraded"
        # The upgrade leg's older release is built first, so a missing or unpinned compiler stops the check
        # before anything is planted or installed. It is a synthetic test package: its runtime manifest is the
        # new package's, so that check is not repeated for it.
        older_package = Path(temp) / "older-package"
        make_older_package(package_dir, older_package)
        older_setup = Path(build_installer.build_installer(
            older_package, UPGRADE_VERSION, Path(temp) / "older-setup", source_date_epoch=UPGRADE_EPOCH, iscc=iscc,
            verify_runtime=lambda _bin: None,
            extra_defines=None if shortcut_name == DEFAULT_SHORTCUT_NAME else {"ShortcutName": shortcut_name},
        )["installer"])
        sentinels.plant()
        try:
            # 1. A shortcut that starts another copy (what tools/stage_install.py writes) is never taken over.
            other = Path(temp) / "other-copy"
            (other / "bin").mkdir(parents=True)
            shutil.copy2(package_dir / "bin" / "nakagawa_player.exe", other / "bin" / "nakagawa_player.exe")
            stage_install.write_shortcut(shortcut, other)
            foreign = shortcut.read_bytes()
            refused = subprocess.run([str(installer), *silent_install, f"/DIR={dest}"], capture_output=True,
                                     text=True, timeout=600, check=False)
            if (refused.returncode == 0 or _hkcu_key_exists() or (dest.exists() and any(dest.rglob("*")))):
                fail("INSTALLER_TOOK_OVER_SHORTCUT",
                     f"Setup exited {refused.returncode} and installed over another copy's shortcut")
            if not shortcut.is_file() or shortcut.read_bytes() != foreign:
                fail("INSTALLER_TOOK_OVER_SHORTCUT", f"{shortcut} was changed or removed by the refused install")
            shortcut.unlink()

            # 2. The install itself.
            install(installer, dest, "INSTALLER_INSTALL_FAILED")
            problems = compare_installed(package_dir, dest)
            if problems:
                fail("INSTALLER_INSTALL_DIFFERS", "; ".join(problems[:8]))
            if not (dest / "unins000.exe").is_file():
                fail("INSTALLER_NO_UNINSTALLER", f"{dest} has no unins000.exe")
            check_shortcut_and_entry()
            result["installed_files"] = len(build_installer.files_under(dest))

            # What the person owns beside the program survives the uninstall.
            add_person_files(dest)

            # 3. While the player runs (its program file is open and cannot be written), a silent uninstall stops
            #    and changes nothing: Inno's uninstaller does not close programs the way Setup does.
            player = dest / "bin" / "nakagawa_player.exe"
            release = _hold_open(player)
            try:
                blocked = subprocess.run([str(dest / "unins000.exe"), *silent_uninstall], capture_output=True,
                                         text=True, timeout=600, check=False)
            finally:
                release()
            if (blocked.returncode == 0 or not _hkcu_key_exists() or not shortcut.is_file()
                    or not player.is_file() or not (dest / "unins000.exe").is_file()):
                fail("INSTALLER_UNINSTALL_WHILE_RUNNING",
                     f"the uninstaller exited {blocked.returncode} with the player running and left a partial uninstall")

            # 4. The uninstall.
            left = uninstall(dest)
            expected = sorted(PERSON_FILES)
            if left != expected:
                fail("INSTALLER_UNINSTALL_LEFTOVER", f"left {left}, expected {expected}")
            gone = sentinels.missing()
            if gone:
                fail("INSTALLER_UNINSTALL_TOUCHED_USER_DATA", f"a silent uninstall removed {gone}")
            result["left_after_uninstall"] = left

            # 5. An upgrade in place: the older release, the person's files and one installed file they changed,
            #    then this installer over it. Setup removes exactly the files the older install's own
            #    SHA256SUMS.txt lists, the new one does not, and that are unchanged; nothing else.
            install(older_setup, upgraded, "INSTALLER_UPGRADE_OLDER_FAILED")
            problems = compare_installed(older_package, upgraded)
            if problems:
                fail("INSTALLER_UPGRADE_OLDER_DIFFERS", "; ".join(problems[:8]))
            add_person_files(upgraded)
            (upgraded / UPGRADE_EDITED).write_text("changed by the person\n", encoding="utf-8")
            install(installer, upgraded, "INSTALLER_UPGRADE_FAILED")
            problems = upgrade_problems(package_dir, upgraded)
            if problems:
                fail("INSTALLER_UPGRADE_DIFFERS", "; ".join(problems[:8]))
            check_shortcut_and_entry()
            gone = sentinels.missing()
            if gone:
                fail("INSTALLER_UPGRADE_TOUCHED_USER_DATA", f"the upgrade removed {gone}")
            left = uninstall(upgraded)
            lost = sorted(set(PERSON_FILES) - set(left))
            if lost:
                fail("INSTALLER_UNINSTALL_LEFTOVER", f"the uninstall after the upgrade removed {lost}")
            gone = sentinels.missing()
            if gone:
                fail("INSTALLER_UNINSTALL_TOUCHED_USER_DATA", f"a silent uninstall removed {gone}")
            result["upgrade_removed"] = list(UPGRADE_DROPPED)
            result["upgrade_kept"] = sorted({*PERSON_FILES, UPGRADE_EDITED})
            result["user_data_sentinels_kept"] = len(sentinels.files)
            result["status"] = "PASS"
        finally:
            for folder in (upgraded, dest):
                if _hkcu_key_exists() and (folder / "unins000.exe").is_file():
                    subprocess.run([str(folder / "unins000.exe"), *silent_uninstall], capture_output=True,
                                   text=True, timeout=600, check=False)
                    _wait_for(lambda: not _hkcu_key_exists(), 60.0)
            if shortcut.is_file() and stage_install.read_shortcut_app_id(shortcut) == stage_install.APP_USER_MODEL_ID:
                shortcut.unlink()  # the one this check made (the check refused to start with one present)
            sentinels.remove()
            shutil.rmtree(dest, ignore_errors=True)
            shutil.rmtree(upgraded, ignore_errors=True)
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0],
                                     epilog="WARNING: writes marker files into the real %LOCALAPPDATA%\\Nakagawa "
                                            "folders and the real per-user uninstall entry and Start-menu shortcut; "
                                            "never run it where real Nakagawa data lives. Use a throwaway Windows "
                                            "user account, Windows Sandbox or a virtual machine.")
    parser.add_argument("--installer", type=Path, required=True)
    parser.add_argument("--package-dir", type=Path, required=True)
    parser.add_argument("--shortcut-name", default=DEFAULT_SHORTCUT_NAME,
                        help="the shortcut name the installer was built with (tests build others through the API)")
    parser.add_argument("--iscc", type=Path, default=None,
                        help="the pinned ISCC.exe, for the upgrade leg's older installer (default: as build_installer)")
    args = parser.parse_args(argv)
    try:
        result = verify_installer(args.installer, args.package_dir, shortcut_name=args.shortcut_name, iscc=args.iscc)
    except build_installer.InstallerError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    print(json.dumps({"event": "installer-verify", **result}, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
