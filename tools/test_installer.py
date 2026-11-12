# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The per-user Windows installer: its script, pinned compiler and build tool (#2208, #324).

Everything here runs without Inno Setup except ``RealCompilerTests``, which skips (reported SKIP, never a
pass) when the pinned compiler is not installed.
"""

from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from typing import Any

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))

import build_installer  # noqa: E402
import stage_install  # noqa: E402
import verify_installer  # noqa: E402

SCRIPT = ROOT / "assets" / "installer" / "nakagawa.iss"
FAKE_BYTES = b"installer test bytes"


def _directive(text: str, name: str) -> str | None:
    match = re.search(rf"^{name}=(.*)$", text, re.MULTILINE)
    return match.group(1).strip() if match else None


def _code(text: str) -> str:
    """The script without its ``;`` comment lines, which name the very things the rules forbid."""
    return "\n".join(line for line in text.splitlines() if not line.lstrip().startswith(";"))


_PROBE_BLOCK = re.compile(r"^#ifdef TestProbe\n.*?^#endif\n?", re.MULTILINE | re.DOTALL)


def _release(text: str) -> str:
    """The script as a release build compiles it: without the test-only ``#ifdef TestProbe`` block."""
    return _PROBE_BLOCK.sub("", text)


def _section(text: str, name: str) -> str:
    match = re.search(rf"^\[{name}\]\n(.*?)(?=^\[|\Z)", text, re.MULTILINE | re.DOTALL)
    return match.group(1) if match else ""


class InstallerScriptTests(unittest.TestCase):
    """Rules the script keeps; each is a promise in the design (docs and issue #2208)."""

    @classmethod
    def setUpClass(cls):
        cls.text = SCRIPT.read_text(encoding="utf-8")

    def test_it_installs_per_user_and_never_asks_for_administrator(self):
        self.assertEqual(_directive(self.text, "PrivilegesRequired"), "lowest")
        # Present and empty: the dialog never offers an all-users install.
        self.assertEqual(_directive(self.text, "PrivilegesRequiredOverridesAllowed"), "")
        self.assertEqual(_directive(self.text, "DefaultDirName"), r"{autopf}\{#AppName}")
        self.assertIn("x64compatible", _directive(self.text, "ArchitecturesAllowed") or "")

    def test_it_writes_nothing_all_users_and_no_registry_values_of_its_own(self):
        lowered = _code(self.text).lower()
        for forbidden in ("hklm", "{commonpf", "{commonappdata}", "{commondesktop", "{commonprograms",
                          "{sys}", "{win}", "{cf", "[registry]", "[ini]", "[services]", "{pf}", "{pf32}", "{pf64}"):
            self.assertNotIn(forbidden, lowered, forbidden)
        self.assertEqual(_directive(self.text, "ChangesEnvironment"), "no")
        self.assertEqual(_directive(self.text, "ChangesAssociations"), "no")

    def test_it_uses_no_network(self):
        self.assertEqual(verify_installer.egress_findings(_code(self.text)), [])
        self.assertEqual(verify_installer.egress_findings("x := DownloadTemporaryFile('https://a', 'b', '', nil);"),
                         ["INSTALLER_NETWORK_CALL: downloadtemporaryfile"])

    def test_the_app_id_is_the_one_the_verifier_and_upgrades_rely_on(self):
        self.assertEqual(_directive(self.text, "AppId"), "{{" + verify_installer.APP_ID + "}")

    def test_both_shortcuts_carry_the_players_taskbar_identity(self):
        self.assertIn(f'#define AppUserModelID "{stage_install.APP_USER_MODEL_ID}"', self.text)
        icons = [line for line in _section(self.text, "Icons").splitlines() if line.startswith("Name:")]
        self.assertEqual(len(icons), 2)
        for line in icons:
            self.assertIn('AppUserModelID: "{#AppUserModelID}"', line)
        self.assertIn("{userprograms}", icons[0])
        self.assertIn("{userdesktop}", icons[1])
        self.assertIn("Tasks: desktopicon", icons[1])
        self.assertIn("Flags: unchecked", _section(self.text, "Tasks"))

    def test_the_only_run_entry_is_the_optional_launch_and_never_silent(self):
        entries = [line for line in _section(self.text, "Run").splitlines() if line.strip()]
        self.assertEqual(len(entries), 1)
        self.assertIn("postinstall", entries[0])
        self.assertIn("skipifsilent", entries[0])
        self.assertIn("runasoriginaluser", entries[0])

    def test_the_files_are_the_package_and_get_deterministic_times(self):
        files = [line for line in _section(self.text, "Files").splitlines() if line.strip()]
        self.assertEqual(len(files), 1)
        self.assertTrue(files[0].startswith('Source: "{#PackageDir}\\*"'))
        self.assertIn("touch", files[0].split("Flags:")[1])
        self.assertEqual(_directive(self.text, "TouchDate"), "{#TouchDate}")
        self.assertEqual(_directive(self.text, "TouchTime"), "{#TouchTime}")
        self.assertEqual(_directive(self.text, "TimeStampsInUTC"), "yes")

    def test_a_running_player_is_closed_by_the_restart_manager_not_a_fixed_mutex(self):
        # The player names its single-instance mutex from a hash of its data root, so no fixed AppMutex exists.
        self.assertEqual(_directive(self.text, "CloseApplications"), "yes")
        self.assertIsNone(_directive(self.text, "AppMutex"))

    def test_the_developer_install_tools_folder_is_refused(self):
        self.assertIn(stage_install.MARKER, self.text)
        self.assertIn("function PrepareToInstall", self.text)

    def test_another_copys_shortcut_is_refused_not_taken_over(self):
        # tools/stage_install.py writes the same shortcut name and taskbar identity, so installing or removing
        # either copy would replace or remove the other's shortcut.
        prepare = self.text[self.text.index("function PrepareToInstall"):self.text.index("{ Deletes Dir")]
        self.assertIn("{userprograms}\\{#ShortcutName}.lnk", prepare)
        self.assertIn("{userdesktop}\\{#ShortcutName}.lnk", prepare)
        self.assertIn("WizardIsTaskSelected('desktopicon')", prepare)  # the desktop one only when it will be written
        other = self.text[self.text.index("function OtherCopyShortcutTarget"):self.text.index("function PrepareToInstall")]
        self.assertIn("(not SamePath(Target, Ours)) and FileExists(Target)", other)
        self.assertIn("FileExists(Target)", other)  # a stale shortcut (its target is gone) is replaced
        self.assertIn("Ours := ExpandConstant('{app}') + '\\bin\\{#AppExe}'", other)
        self.assertIn("Uninstall or delete that copy", prepare)

    def test_paths_compare_in_long_form(self):
        # A hosted runner's TEMP has the short part RUNNER~1: /DIR= keeps that form, while the shell reads an
        # installed shortcut's target back in long form. A text compare took the install's own shortcut for another
        # copy's and refused the upgrade (Setup exit 7).
        same = self.text[self.text.index("function SamePath"):self.text.index("{ The target of an existing shortcut")]
        self.assertIn("CompareText(LongPathName(RemoveBackslashUnlessRoot(ExpandFileName(A))),", same)
        self.assertIn("LongPathName(RemoveBackslashUnlessRoot(ExpandFileName(B)))) = 0", same)
        self.assertIn("external 'GetLongPathNameW@kernel32.dll stdcall'", self.text)
        self.assertEqual(re.findall(r"external\s+'([^']*)'", self.text), ["GetLongPathNameW@kernel32.dll stdcall"])
        located = self.text[self.text.index("function IsSetupInstallIn"):self.text.index("procedure CurStepChanged")]
        self.assertIn("Result := SamePath(Location, Dir);", located)

    def test_the_only_com_object_is_the_local_scripting_shell(self):
        self.assertEqual(re.findall(r"CreateOleObject\(([^)]*)\)", self.text), ["'WScript.Shell'"])
        self.assertEqual(verify_installer.egress_findings("x := CreateOleObject('WScript.Shell');"), [])
        self.assertEqual(verify_installer.egress_findings("x := CreateOleObject('MSXML2.XMLHTTP');"),
                         ["INSTALLER_NETWORK_CALL: createoleobject('msxml2.xmlhttp')"])


class UninstallRuleTests(unittest.TestCase):
    """The uninstall keeps user data unless the person ticks one box; a silent uninstall never deletes it."""

    @classmethod
    def setUpClass(cls):
        cls.text = SCRIPT.read_text(encoding="utf-8")

    def test_there_is_no_silent_delete_flag(self):
        code = _code(self.text)
        self.assertNotIn("/DELETEUSERDATA", code.upper())  # the variable DeleteUserData is the box, not a flag
        self.assertNotIn("GetCmdTail", code)
        self.assertNotIn("CmdLineParamExists", code)

    def test_the_data_folders_are_the_five_the_player_resolves_and_the_docs_list(self):
        names = dict(re.findall(r"^\s+(\d): Result := '(\w+)';", self.text, re.MULTILINE))
        self.assertEqual([names[str(index)] for index in range(5)], ["config", "data", "cache", "logs", "saves"])
        self.assertIn("USER_DATA_FOLDER_COUNT = 5", self.text)
        # The folders the player itself resolves under %LOCALAPPDATA%\\Nakagawa on Windows: a layout change
        # there (the data-folder work, #324) must change this script with it.
        platform = (ROOT / "src" / "core" / "nk_platform_win32.c").read_text(encoding="utf-8")
        resolve = platform[platform.index("static bool resolve_windows_path"):]
        resolve = resolve[:resolve.index("\n}\n")]
        resolved = re.findall(r'case NK_PATH_\w+:\s+subdir = "(\w+)";', resolve)
        self.assertEqual(sorted(names.values()), sorted(set(resolved)))
        self.assertIn('"%s\\\\Nakagawa\\\\%s"', resolve)
        # The docs table, once #2164 lands it, lists the same five.
        setup_doc = (ROOT / "docs" / "SETUP.md").read_text(encoding="utf-8")
        rows = re.findall(r"^\| `(\w+)/` \|", setup_doc, re.MULTILINE)
        if rows:
            self.assertEqual(sorted(names.values()), sorted(rows))

    def test_the_delete_is_reached_only_from_the_unchecked_box_and_never_when_silent(self):
        release = _release(self.text)
        self.assertEqual(len(re.findall(r"\bRemoveUserData\b", release)), 2)  # the procedure and its one call
        self.assertEqual(len(re.findall(r"\bRemoveUserDataIn\b", release)), 2)  # its body and the one caller
        self.assertIn("Box.Checked := False;", self.text)
        call = re.search(r"CurUninstallStepChanged.*?end;\s*\Z", release, re.DOTALL)
        self.assertIsNotNone(call)
        self.assertIn("DeleteUserData and (not UninstallSilent)", call.group(0))
        init = self.text[self.text.index("function InitializeUninstall"):]
        # Silent returns before any form is built, with the flag still False.
        self.assertLess(init.index("if UninstallSilent then"), init.index("CreateCustomForm"))
        self.assertLess(init.index("DeleteUserData := False;"), init.index("if UninstallSilent then"))

    def test_the_delete_follows_no_link_and_names_only_the_per_user_root(self):
        body = self.text[self.text.index("function DeleteTreeNoLinks"):self.text.index("{ True while the player")]
        # Every entry is classified before it is entered or deleted; a link is removed as a link.
        self.assertIn("(Found.Attributes and FILE_ATTRIBUTE_REPARSE_POINT) <> 0", body)
        self.assertLess(body.index("if IsLink then"), body.index("else if IsDir then"))
        self.assertIn("RemoveDir(Path)", body)
        self.assertIn("IsReparsePoint(Root)", body)
        self.assertIn("IsReparsePoint(Folder)", body)
        # DelTree (which could follow a link) is used only on one named file, never on a folder.
        self.assertEqual(re.findall(r"DelTree\(([^)]*)\)", self.text), ["Path, False, True, False"])
        self.assertIn("ExpandConstant('{localappdata}') + '\\Nakagawa'", self.text)
        # A second, defaulting-to-No confirmation precedes it.
        self.assertIn("MB_YESNO or MB_DEFBUTTON2", self.text)

    def test_a_path_that_cannot_be_examined_counts_as_a_link(self):
        body = self.text[self.text.index("function IsReparsePoint"):self.text.index("{ Two things this installer must not do")]
        body = body[:body.index("\nend;")]
        # The result starts True and only a successful look at the entry can make it False.
        self.assertLess(body.index("Result := True;"), body.index("if FindFirst(Path, Found) then"))
        self.assertNotIn("Result := False", body)
        self.assertIn("Result := (Found.Attributes and FILE_ATTRIBUTE_REPARSE_POINT) <> 0;", body)

    def test_an_uninstall_while_the_player_runs_changes_nothing(self):
        # Inno's uninstaller does not close programs (measured: a silent uninstall left the locked player behind
        # after removing the shortcut and the uninstall entry), so the script stops it first.
        init = self.text[self.text.index("function InitializeUninstall"):]
        self.assertLess(init.index("while PlayerIsRunning do"), init.index("if UninstallSilent then\n    Exit;"))
        loop = init[init.index("while PlayerIsRunning do"):init.index("if UninstallSilent then\n    Exit;")]
        self.assertIn("Result := False;", loop)
        self.assertIn("MB_RETRYCANCEL", loop)
        probe = self.text[self.text.index("function PlayerIsRunning"):self.text.index("{ The uninstall page")]
        self.assertIn("not SaveStringToFile(Exe, '', True)", probe)  # open for write, nothing written


class TestProbeHookTests(unittest.TestCase):
    """The compile-time hook RealProbeTests uses is test-only: a release build never has it."""

    @classmethod
    def setUpClass(cls):
        cls.text = SCRIPT.read_text(encoding="utf-8")

    def test_the_probe_is_one_ifdef_block_at_the_end_and_the_only_initialize_setup(self):
        blocks = _PROBE_BLOCK.findall(self.text)
        self.assertEqual(len(blocks), 1)
        self.assertTrue(self.text.rstrip().endswith(blocks[0].rstrip()))
        self.assertIn("function InitializeSetup", blocks[0])
        self.assertNotIn("InitializeSetup", _release(self.text))
        # Unknown or missing /PROBE= does nothing and Setup still exits before its wizard.
        self.assertLess(blocks[0].index("Result := False;"), blocks[0].index("Probe := "))

    def test_a_release_build_never_defines_it(self):
        command = build_installer.compiler_command(Path("ISCC.exe"), Path("p"), "0.0.2", Path("o"), 1793900000)
        self.assertFalse(any("TestProbe" in part for part in command))
        self.assertNotIn("TestProbe", (TOOLS / "build_installer.py").read_text(encoding="utf-8"))
        self.assertNotIn("TestProbe", (TOOLS / "verify_installer.py").read_text(encoding="utf-8"))


class UpgradeRuleTests(unittest.TestCase):
    """An upgrade in place deletes only files the previous install's own manifest lists and the new one drops."""

    text: str
    release: str

    @classmethod
    def setUpClass(cls):
        cls.text = SCRIPT.read_text(encoding="utf-8")
        cls.release = _release(cls.text)

    def _body(self, start: str, end: str) -> str:
        return self.release[self.release.index(start):self.release.index(end)]

    def test_there_is_no_blanket_delete_section(self):
        lowered = _code(self.text).lower()
        self.assertNotIn("[installdelete]", lowered)
        self.assertNotIn("[uninstalldelete]", lowered)

    def test_the_previous_manifest_is_read_before_the_copy_only_for_setups_own_install_here(self):
        step = self._body("procedure CurStepChanged", "{ Deletes Dir")
        self.assertLess(step.index("if CurStep = ssInstall then"), step.index("(CurStep = ssPostInstall)"))
        self.assertLess(step.index("HavePreviousManifest := False;"), step.index("IsSetupInstallIn("))
        self.assertLess(step.index("IsSetupInstallIn("), step.index("ReadManifest("))
        self.assertIn("(CurStep = ssPostInstall) and HavePreviousManifest", step)
        owner = self._body("function IsSetupInstallIn", "procedure CurStepChanged")
        self.assertIn("unins000.dat", owner)
        self.assertIn("RegQueryStringValue(HKCU, UNINSTALL_KEY, 'InstallLocation', Location)", owner)
        self.assertIn(f"UNINSTALL_KEY = '{verify_installer.UNINSTALL_KEY}';", self.release)
        self.assertIn(f"MANIFEST_NAME = '{build_installer.SUMS_NAME}';", self.release)

    def test_a_file_is_deleted_only_when_dropped_unchanged_and_not_behind_a_link(self):
        remove = self._body("procedure RemoveDroppedFiles", "{ True only when the uninstall entry")
        self.assertIn("if not ReadManifest(Dir + '\\' + MANIFEST_NAME, NewPaths, NewDigests) then\n    Exit;", remove)
        self.assertIn("if not Carried.Find(Lowercase(OldPaths[Index]), Found) then", remove)
        delete = remove.index("DeleteFile(Path)")
        for guard in ("FileExists(Path)", "(not LinkOnPath(Dir, Relative))", "FileHasDigest(Path, OldDigests[Index])"):
            with self.subTest(guard):
                self.assertLess(remove.index(guard), delete)
        self.assertEqual(remove.count("DeleteFile("), 1)
        # Only emptied folders go, never the install folder itself.
        self.assertIn("while (Length(Folder) > Length(Dir)) and RemoveDir(Folder) do", remove)

    def test_a_manifest_that_is_not_a_plain_relative_list_is_not_used(self):
        part = self._body("function IsSafeManifestPart", "{ A manifest path that stays inside")
        # A trailing dot or space is an alias Windows strips; '~' may be an 8.3 short name.
        self.assertIn("if (Part[Length(Part)] = '.') or (Part[Length(Part)] = ' ') then\n    Exit;", part)
        self.assertIn("(Ord(Part[Index]) < 32) or (Ord(Part[Index]) = 127) or (Pos(Part[Index], '~*?:<>\"|\\/') > 0)",
                      part)
        self.assertIn("if Part = '' then\n    Exit;", part)
        names = self._body("function IsSafeManifestName", "{ The paths and SHA-256 values")
        self.assertIn("Lowered := Lowercase(Name);", names)
        for refused in ("(Pos('unins', Lowered) = 1)", "(Lowered = 'sha256sums.txt')", "(Lowered = 'bin/portable.txt')",
                        "(Lowered = 'bin/data')", "(Pos('bin/data/', Lowered) = 1)", "if not IsSafeManifestPart(Part) then"):
            with self.subTest(refused):
                self.assertIn(refused, names)
        self.assertEqual(verify_installer.INNO_OWN_PREFIX, "unins")
        self.assertEqual(build_installer.SUMS_NAME.lower(), "sha256sums.txt")
        read = self._body("function ReadManifest", "{ True when Dir, or any folder")
        self.assertIn("if IsReparsePoint(FileName) then\n    Exit;", read)
        # One bad line empties both lists and returns False.
        bad = read[read.index("if (Copy(Line, 65, 2) <> '  ')"):]
        self.assertLess(bad.index("SetArrayLength(Paths, 0);"), bad.index("Exit;"))

    def test_every_folder_on_the_path_is_checked_for_a_link(self):
        body = self._body("function LinkOnPath", "function FileHasDigest")
        self.assertIn("Result := IsReparsePoint(Dir);", body)
        self.assertIn("Result := IsReparsePoint(Current);", body)


class SetupLogReasonTests(unittest.TestCase):
    """A failed Setup run names its reason from the /LOG= file, so a hosted failure says why (exit 7 alone does not)."""

    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="nk-installer-log-")
        self.addCleanup(temp.cleanup)
        self.log = Path(temp.name) / "setup.log"

    def test_the_prepare_to_install_message_is_named(self):
        self.log.write_text("﻿2026-11-08 09:00:00.000   Setup version: Inno Setup version 6.7.3\n"
                            "2026-11-08 09:00:01.000   PrepareToInstall failed: A shortcut named X already starts "
                            "another copy\n2026-11-08 09:00:01.100   Need to restart Windows? No\n", encoding="utf-8")
        reason = verify_installer.setup_log_reason(self.log)
        self.assertIn("PrepareToInstall failed: A shortcut named X already starts another copy", reason)
        self.assertNotIn("Need to restart", reason)

    def test_without_that_message_the_last_lines_are_shown(self):
        self.log.write_text("".join(f"line {index}\n" for index in range(30)), encoding="utf-8")
        reason = verify_installer.setup_log_reason(self.log)
        self.assertIn("line 29", reason)
        self.assertIn("line 18", reason)
        self.assertNotIn("line 17", reason)

    def test_no_log_is_no_reason(self):
        self.assertEqual(verify_installer.setup_log_reason(self.log), "")


class VerifierUpgradeTests(unittest.TestCase):
    """The upgrade leg's older package and its expectation, without Inno Setup."""

    root: Path
    package: Path
    older: Path

    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.root = Path(self._temp.name)
        self.package = self.root / "package"
        (self.package / "bin").mkdir(parents=True)
        (self.package / "bin" / "nakagawa_player.exe").write_bytes(b"player")
        (self.package / "LICENSE").write_bytes(b"l")
        stage_install.write_sha256sums(self.package)
        self.older = self.root / "older"
        verify_installer.make_older_package(self.package, self.older)

    def _upgraded(self) -> Path:
        """A folder as a correct upgrade leaves it: the new package, the person's files and their changed file."""
        dest = self.root / "upgraded"
        shutil.copytree(self.package, dest)
        (dest / "unins000.exe").write_bytes(b"u")
        verify_installer.add_person_files(dest)
        (dest / verify_installer.UPGRADE_EDITED).write_text("changed by the person\n", encoding="utf-8")
        return dest

    def test_the_older_package_is_the_new_one_plus_the_dropped_files_under_its_own_manifest(self):
        sums = build_installer.read_sums(self.older)
        self.assertEqual(build_installer.compare_with_sums(self.older, sums), [])
        new = build_installer.read_sums(self.package)
        self.assertEqual(sorted(set(sums) - set(new)),
                         sorted({*verify_installer.UPGRADE_DROPPED, verify_installer.UPGRADE_EDITED}))
        self.assertEqual({name: sums[name] for name in new}, new)
        self.assertEqual(build_installer.read_sums(self.package), new)  # the package itself is untouched

    def test_a_correct_upgrade_has_no_problem(self):
        self.assertEqual(verify_installer.upgrade_problems(self.package, self._upgraded()), [])

    def test_a_dropped_file_left_behind_is_a_problem(self):
        dest = self._upgraded()
        for name in verify_installer.UPGRADE_DROPPED:
            (dest / name).parent.mkdir(parents=True, exist_ok=True)
            (dest / name).write_text("stale", encoding="utf-8")
        self.assertEqual(verify_installer.upgrade_problems(self.package, dest),
                         [f"UNLISTED: {name}" for name in sorted(verify_installer.UPGRADE_DROPPED)]
                         + [f"LEFT_FOLDER: {verify_installer.UPGRADE_DROPPED_FOLDER}"])

    def test_an_emptied_folder_left_behind_is_a_problem(self):
        dest = self._upgraded()
        (dest / verify_installer.UPGRADE_DROPPED_FOLDER / "nested").mkdir(parents=True)
        self.assertEqual(verify_installer.upgrade_problems(self.package, dest),
                         [f"LEFT_FOLDER: {verify_installer.UPGRADE_DROPPED_FOLDER}"])

    def test_a_removed_person_file_or_changed_file_is_a_problem(self):
        dest = self._upgraded()
        (dest / "notes.txt").unlink()
        (dest / verify_installer.UPGRADE_EDITED).unlink()
        self.assertEqual(verify_installer.upgrade_problems(self.package, dest),
                         ["REMOVED_KEPT_FILE: bin/installer-verify-edited.txt", "REMOVED_KEPT_FILE: notes.txt"])

    def test_a_missing_or_stale_new_file_is_a_problem(self):
        dest = self._upgraded()
        (dest / "LICENSE").unlink()
        (dest / "bin" / "nakagawa_player.exe").write_bytes(b"older player")
        self.assertEqual(verify_installer.upgrade_problems(self.package, dest),
                         ["MISSING: LICENSE", "CHANGED: bin/nakagawa_player.exe"])


def _lock() -> dict[str, Any]:
    lock: dict[str, Any] = build_installer.load_lock()
    return lock


class LockTests(unittest.TestCase):
    def test_the_checked_in_lock_is_valid_and_names_the_version_it_documents(self):
        lock = _lock()
        self.assertEqual(lock["version"], "6.7.3")
        self.assertEqual(lock["license"]["name"], "Inno Setup License")
        self.assertIn("ISCC.exe", lock["files"])

    def _written(self, mutate) -> Path:
        lock = copy.deepcopy(_lock())
        mutate(lock)
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        path = Path(temp.name) / "lock.json"
        path.write_text(json.dumps(lock), encoding="utf-8")
        return path

    def test_a_lock_that_is_not_pinned_by_hash_is_refused(self):
        cases = {
            "schema": lambda lock: lock.update(schema_version=2),
            "version": lambda lock: lock.update(version="six"),
            "url host": lambda lock: lock["installer"].update(
                url="https://example.com/jrsoftware/issrc/releases/download/is-6_7_3/innosetup-6.7.3.exe"),
            "installer hash": lambda lock: lock["installer"].update(sha256="ABC"),
            "no compiler": lambda lock: lock["files"].pop("ISCC.exe"),
            "path in files": lambda lock: lock["files"].update({"sub/x.dll": "0" * 64}),
            "file hash": lambda lock: lock["files"].update({"ISCC.exe": "short"}),
            "licence": lambda lock: lock["license"].update(text_sha256="x"),
            "licence name": lambda lock: lock["license"].update(name="MIT"),
        }
        for label, mutate in cases.items():
            with self.subTest(label), self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_LOCK_INVALID"):
                build_installer.load_lock(self._written(mutate))

    def test_a_missing_or_unreadable_lock_is_refused(self):
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_LOCK_INVALID"):
            build_installer.load_lock(Path("does-not-exist.json"))


class BuildToolTests(unittest.TestCase):
    """build_installer with a fake compiler: the checks come before it and nothing is written when they fail."""

    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.root = Path(self._temp.name)
        self.iscc_dir = self.root / "inno"
        self.iscc_dir.mkdir()
        self.lock = copy.deepcopy(_lock())
        self.lock["files"] = {"ISCC.exe": self._file(self.iscc_dir / "ISCC.exe", b"compiler"),
                              "Setup.e32": self._file(self.iscc_dir / "Setup.e32", b"loader")}
        self.package = self.root / "package"
        (self.package / "bin").mkdir(parents=True)
        for name, data in (("bin/nakagawa_player.exe", b"player"), ("bin/runtime-manifest.json", b"{}"),
                           ("LICENSE", b"licence"), ("docs/README.md", b"readme")):
            path = self.package / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        stage_install.write_sha256sums(self.package)
        self.calls: list[list[str]] = []

    @staticmethod
    def _file(path: Path, data: bytes) -> str:
        path.write_bytes(data)
        digest: str = build_installer.sha256_file(path)
        return digest

    def _fake_run(self, *, returncode=0, produce=True):
        def run(command, **_kwargs):
            self.calls.append(command)
            if produce:
                out = Path(next(part[2:] for part in command if part.startswith("/O")))
                name = next(part[2:] for part in command if part.startswith("/F"))
                (out / f"{name}.exe").write_bytes(FAKE_BYTES)
            return subprocess.CompletedProcess(command, returncode, "", "boom" if returncode else "")
        return run

    def build(self, **overrides):
        arguments = dict(source_date_epoch=1793900000, iscc=self.iscc_dir / "ISCC.exe", lock=self.lock,
                         run=self._fake_run(), verify_runtime=lambda _bin: None, environ={})
        arguments.update(overrides)
        return build_installer.build_installer(self.package, "0.0.2", self.root / "out", **arguments)

    def test_it_compiles_with_the_documented_inputs_and_writes_the_checksum(self):
        summary = self.build()
        installer = Path(summary["installer"])
        self.assertEqual(installer.name, "nakagawa-recomp-0.0.2-windows-x64-setup.exe")
        self.assertEqual(Path(summary["checksum_file"]).read_text(encoding="utf-8"),
                         f"{summary['sha256']}  {installer.name}\n")
        command = self.calls[0]
        # Compare resolved paths: on a hosted runner the temp folder is an 8.3 short name
        # (RUNNER~1) while the tool reports the long form of the same folder.
        self.assertEqual(Path(command[0]).resolve(), (self.iscc_dir / "ISCC.exe").resolve())
        for expected in (f"/DPackageDir={self.package.resolve()}", "/DAppVersion=0.0.2",
                         "/DTouchDate=2026-11-05", "/DTouchTime=17:33:20", "/Onakagawa"[:2]):
            self.assertTrue(any(part.startswith(expected) for part in command), expected)
        self.assertEqual(command[-1], str(build_installer.SCRIPT_PATH))
        self.assertEqual(summary["inno_setup"], "6.7.3")

    def test_the_time_inside_the_installer_follows_the_epoch_not_the_clock(self):
        self.assertEqual(build_installer.touch_for_epoch(1793900000), ("2026-11-05", "17:33:20"))
        self.assertEqual(build_installer.touch_for_epoch(0), ("1980-01-01", "00:00:00"))
        self.assertEqual(build_installer.compiler_command(
            Path("ISCC.exe"), Path("p"), "0.0.2", Path("o"), 1793900000),
            build_installer.compiler_command(Path("ISCC.exe"), Path("p"), "0.0.2", Path("o"), 1793900000))
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_EPOCH_MISSING"):
            build_installer.resolve_epoch(None, {})
        self.assertEqual(build_installer.resolve_epoch(None, {"SOURCE_DATE_EPOCH": "42"}), 42)

    def test_an_unpinned_compiler_is_refused_before_anything_is_compiled(self):
        (self.iscc_dir / "Setup.e32").write_bytes(b"patched loader")
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_COMPILER_UNPINNED: Setup.e32"):
            self.build()
        self.assertEqual(self.calls, [])
        self.assertFalse((self.root / "out").exists())

    def test_a_missing_compiler_says_how_to_install_it(self):
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_COMPILER_MISSING"):
            self.build(iscc=self.root / "nowhere" / "ISCC.exe")

    def test_a_package_that_changed_after_its_checksums_is_refused(self):
        (self.package / "bin" / "nakagawa_player.exe").write_bytes(b"tampered")
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_PACKAGE_CHANGED.*CHANGED: bin/nakagawa_player.exe"):
            self.build()
        (self.package / "extra.txt").write_text("x", encoding="utf-8")
        with self.assertRaisesRegex(build_installer.InstallerError, "UNLISTED: extra.txt"):
            self.build()
        self.assertEqual(self.calls, [])

    def test_a_folder_that_is_not_a_package_is_refused(self):
        (self.package / "LICENSE").unlink()
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_PACKAGE_INVALID: LICENSE"):
            self.build()
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_PACKAGE_MISSING"):
            build_installer.check_package(self.root / "nothing")

    def test_a_runtime_manifest_that_does_not_verify_is_refused(self):
        def reject(_bin):
            raise RuntimeError("RUNTIME_FILE_CHANGED: bin/SDL3.dll")
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_RUNTIME_UNVERIFIED.*RUNTIME_FILE_CHANGED"):
            self.build(verify_runtime=reject)

    def test_an_existing_output_is_never_overwritten(self):
        self.build()
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_OUTPUT_EXISTS"):
            self.build()

    def test_an_output_inside_the_package_is_refused(self):
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_OUTPUT_INSIDE_PACKAGE"):
            build_installer.build_installer(self.package, "0.0.2", self.package / "out", source_date_epoch=1,
                                            iscc=self.iscc_dir / "ISCC.exe", lock=self.lock, environ={})

    def test_a_failed_compile_leaves_no_installer_and_shows_the_compilers_words(self):
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_COMPILE_FAILED: ISCC exited 2"):
            self.build(run=self._fake_run(returncode=2))
        self.assertEqual(list((self.root / "out").glob("*")), [])

    def test_a_version_that_is_not_semver_is_refused(self):
        for version in ("", "0.0", "v0.0.2", "0.0.2 beta", "../0.0.2"):
            with self.subTest(version), self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_VERSION_INVALID"):
                build_installer.build_installer(self.package, version, self.root / "o", source_date_epoch=1,
                                                lock=self.lock, environ={})

    def test_the_package_is_never_modified(self):
        before = {name: build_installer.sha256_file(path) for name, path in build_installer.files_under(self.package).items()}
        self.build()
        after = {name: build_installer.sha256_file(path) for name, path in build_installer.files_under(self.package).items()}
        self.assertEqual(before, after)


class VerifierPureTests(unittest.TestCase):
    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.addCleanup(self._temp.cleanup)
        self.root = Path(self._temp.name)
        self.package = self.root / "package"
        (self.package / "bin").mkdir(parents=True)
        (self.package / "bin" / "a.dll").write_bytes(b"a")
        (self.package / "LICENSE").write_bytes(b"l")
        stage_install.write_sha256sums(self.package)
        self.installed = self.root / "installed"
        shutil.copytree(self.package, self.installed)

    def test_sentinels_are_planted_in_every_data_folder_and_only_what_was_made_is_removed(self):
        base = self.root / "local" / "Nakagawa"
        (base / "data").mkdir(parents=True)
        (base / "data" / "library.json").write_text("mine", encoding="utf-8")
        sentinels = verify_installer.Sentinels(base)
        sentinels.plant()
        for name in verify_installer.USER_DATA_FOLDERS:
            self.assertTrue((base / name / verify_installer.SENTINEL_NAME).is_file(), name)
        self.assertTrue((base / "saves" / verify_installer.SENTINEL_FOLDER_NAME / verify_installer.SENTINEL_NAME).is_file())
        self.assertEqual(len(sentinels.files), 7)
        self.assertEqual(sentinels.missing(), [])
        (base / "logs" / verify_installer.SENTINEL_NAME).unlink()
        self.assertEqual(sentinels.missing(), [str(base / "logs" / verify_installer.SENTINEL_NAME)])
        sentinels.remove()
        self.assertEqual(sorted(path.name for path in base.iterdir()), ["data"])  # made folders gone, mine kept
        self.assertEqual((base / "data" / "library.json").read_text(encoding="utf-8"), "mine")
        self.assertEqual(sorted(path.name for path in (base / "data").iterdir()), ["library.json"])

    def test_a_check_with_no_sentinel_planted_fails_instead_of_passing_vacuously(self):
        self.assertEqual(verify_installer.Sentinels(self.root / "never").missing(), ["no sentinel was planted"])

    def test_a_linked_root_is_not_written_through(self):
        target = self.root / "elsewhere"
        target.mkdir()
        link = self.root / "Nakagawa"
        try:
            os.symlink(target, link, target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest("this host cannot make a symbolic link")
        with self.assertRaises(build_installer.InstallerError):
            verify_installer.Sentinels(link).plant()
        self.assertEqual(list(target.iterdir()), [])

    def test_an_install_equal_to_its_package_matches_even_with_the_uninstaller_beside_it(self):
        (self.installed / "unins000.exe").write_bytes(b"u")
        (self.installed / "unins000.dat").write_bytes(b"d")
        self.assertEqual(verify_installer.compare_installed(self.package, self.installed), [])

    def test_a_changed_missing_or_extra_file_is_reported(self):
        (self.installed / "bin" / "a.dll").write_bytes(b"changed")
        (self.installed / "LICENSE").unlink()
        (self.installed / "bin" / "unins001.exe").write_bytes(b"x")  # only unins* at the top level is Inno's
        (self.installed / "stray.txt").write_text("x", encoding="utf-8")
        self.assertEqual(sorted(verify_installer.compare_installed(self.package, self.installed)),
                         ["CHANGED: bin/a.dll", "MISSING: LICENSE", "UNLISTED: bin/unins001.exe", "UNLISTED: stray.txt"])

    def test_a_malformed_checksum_list_is_refused(self):
        (self.package / "SHA256SUMS.txt").write_text("nothex  bin/a.dll\n", encoding="utf-8")
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_PACKAGE_INVALID"):
            build_installer.read_sums(self.package)
        (self.package / "SHA256SUMS.txt").write_text(f"{'0' * 64}  ../outside\n", encoding="utf-8")
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_PACKAGE_INVALID"):
            build_installer.read_sums(self.package)

    def test_the_check_refuses_to_run_off_windows(self):
        if os.name == "nt":
            self.skipTest("this host is Windows")
        with self.assertRaisesRegex(build_installer.InstallerError, "INSTALLER_VERIFY_UNSUPPORTED"):
            verify_installer.verify_installer(Path("x.exe"), self.package)


def _short_path(path: Path) -> str | None:
    """Windows' 8.3 short spelling of an existing path, or None when it cannot be read."""
    if sys.platform != "win32":
        return None
    import ctypes
    from ctypes import wintypes

    get_short = ctypes.windll.kernel32.GetShortPathNameW
    get_short.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD]
    get_short.restype = wintypes.DWORD
    buffer = ctypes.create_unicode_buffer(1024)
    written = get_short(str(path), buffer, len(buffer))
    return buffer.value if 0 < written < len(buffer) else None


def _pinned_compiler() -> Path | None:
    try:
        iscc: Path = build_installer.find_iscc()
        build_installer.verify_toolchain(iscc, _lock())
    except build_installer.InstallerError:
        return None
    return iscc


#: Set to 1 where the pinned compiler must be present (the hosted installer job): then a missing compiler fails
#: these tests instead of skipping them.
REQUIRE_COMPILER_ENV = "NK_INSTALLER_REQUIRE_COMPILER"
_RUN_COMPILER_TESTS = os.name == "nt" and (os.environ.get(REQUIRE_COMPILER_ENV) == "1" or _pinned_compiler() is not None)
_NO_COMPILER = "the pinned Inno Setup is not installed (SKIP, not a pass)"


def _player_package(package: Path) -> None:
    """A minimal package whose player is a real Windows program, so the script compiles."""
    for name, data in (("bin/runtime-manifest.json", b"{}"), ("LICENSE", b"licence"), ("docs/README.md", b"readme")):
        path = package / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    shutil.copy(Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "notepad.exe",
                package / "bin" / "nakagawa_player.exe")
    stage_install.write_sha256sums(package)


@unittest.skipUnless(_RUN_COMPILER_TESTS, _NO_COMPILER)
class RealCompilerTests(unittest.TestCase):
    """Compile the real script with the pinned compiler: two builds are byte-identical, and the file times follow the epoch."""

    def test_the_script_compiles_and_the_bytes_do_not_depend_on_the_path_or_the_clock(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            package = root / "package"
            _player_package(package)
            moved = root / "elsewhere" / "copy"
            shutil.copytree(package, moved)
            for path in moved.rglob("*"):
                os.utime(path, (4_000_000_000, 4_000_000_000))
            digests = []
            for index, source in enumerate((package, moved)):
                summary = build_installer.build_installer(source, "0.0.2", root / f"out{index}",
                                                          source_date_epoch=1793900000, verify_runtime=lambda _bin: None)
                digests.append(summary["sha256"])
            self.assertEqual(digests[0], digests[1])
            later = build_installer.build_installer(package, "0.0.2", root / "out-later", source_date_epoch=1793900000 + 86400,
                                                    verify_runtime=lambda _bin: None)
            self.assertNotEqual(later["sha256"], digests[0])


def _junction(target: Path, link: Path) -> None:
    import _winapi  # Windows only; a junction needs no privilege, unlike a symbolic link
    # The repository's mypy run checks with Linux's stubs, which lack this Windows-only function.
    _winapi.CreateJunction(str(target), str(link))  # type: ignore[attr-defined,unused-ignore]


def _is_link(path: Path) -> bool:
    return path.is_symlink() or path.is_junction()


def _sums_line(path: Path, name: str) -> str:
    return f"{build_installer.sha256_file(path)}  {name}\n"


@unittest.skipUnless(_RUN_COMPILER_TESTS, _NO_COMPILER)
class RealProbeTests(unittest.TestCase):
    """The script's own delete routines, compiled with the test-only ``TestProbe`` hook and run on temp folders.

    The probe Setup runs one routine on the folder it is given and exits before its wizard, so nothing is installed:
    no program folder, shortcut or uninstall entry, and nothing under the real ``%LOCALAPPDATA%\\Nakagawa``. It is
    started without ``/SILENT``: if the hook were ever missing, Setup would only show its wizard (the timeout ends
    it), never install."""

    probe: Path
    root: Path
    outside: Path

    @classmethod
    def setUpClass(cls):
        cls._temp = tempfile.TemporaryDirectory(prefix="nk-installer-probe-")
        root = Path(cls._temp.name)
        _player_package(root / "package")
        summary = build_installer.build_installer(
            root / "package", "0.0.2", root / "out", source_date_epoch=1793900000, verify_runtime=lambda _bin: None,
            extra_defines={"TestProbe": "1", "ShortcutName": "Nakagawa Recomp installer probe"})
        cls.probe = Path(summary["installer"])

    @classmethod
    def tearDownClass(cls):
        cls._temp.cleanup()

    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="nk-installer-probe-case-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.outside = self.root / "outside"
        self.outside.mkdir()

    def run_probe(self, probe: str, **params: Path) -> None:
        done = self.root / f"done-{probe}.txt"
        done.unlink(missing_ok=True)
        command = [str(self.probe), f"/PROBE={probe}", *(f"/{name}={value}" for name, value in params.items()),
                   f"/DONE={done}"]
        process = subprocess.Popen(command)
        try:
            process.wait(timeout=120)
        except subprocess.TimeoutExpired:
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(process.pid)], capture_output=True, check=False)
            self.fail("the probe did not exit: Setup was built without its TestProbe hook (nothing was installed)")
        self.assertEqual(done.read_text(encoding="utf-8"), probe)  # the routine ran; not a vacuous pass

    def same_path(self, a: str, b: str) -> str:
        out = self.root / "samepath.txt"
        out.unlink(missing_ok=True)
        self.run_probe("samepath", A=Path(a), B=Path(b), OUT=out)
        return out.read_text(encoding="utf-8")

    def test_a_short_and_a_long_spelling_of_one_path_are_the_same_path(self):
        # The upgrade leg on a hosted runner: /DIR= under the short RUNNER~1, the shortcut's target in long form.
        folder = self.root / "longfoldername-for-a-short-name"
        player = folder / "bin" / "nakagawa_player.exe"
        player.parent.mkdir(parents=True)
        player.write_bytes(b"player")
        other = self.root / "other-copy" / "bin" / "nakagawa_player.exe"
        other.parent.mkdir(parents=True)
        other.write_bytes(b"player")
        short = _short_path(player)
        if short is None or short.lower() == str(player).lower():
            self.skipTest("this volume makes no 8.3 short names (SKIP, not a pass)")
        self.assertEqual(self.same_path(short, str(player)), "same")
        self.assertEqual(self.same_path(str(player).upper(), str(player)), "same")
        self.assertEqual(self.same_path(str(_short_path(folder)) + "\\", str(folder)), "same")
        self.assertEqual(self.same_path(str(other), str(player)), "different")
        self.assertEqual(self.same_path(short, str(other)), "different")

    def _outside(self, name: str, text: str = "keep me") -> Path:
        path = self.outside / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def _data_root(self) -> Path:
        base = self.root / "local" / "Nakagawa"
        for name in verify_installer.USER_DATA_FOLDERS:
            (base / name).mkdir(parents=True)
            (base / name / "file.txt").write_text(name, encoding="utf-8")
        (base / "data" / "library" / "deep").mkdir(parents=True)
        (base / "data" / "library" / "deep" / "save.bin").write_bytes(b"save")
        (base / "other").mkdir()
        (base / "other" / "untouched.txt").write_text("not a data folder", encoding="utf-8")
        return base

    def test_the_data_delete_removes_junctions_as_links_and_read_only_files(self):
        base = self._data_root()
        kept = self._outside("keep/target.txt")
        _junction(kept.parent, base / "saves" / "linked")
        cache_kept = self._outside("cache-target/cache.bin")
        (base / "cache" / "file.txt").unlink()
        (base / "cache").rmdir()
        _junction(cache_kept.parent, base / "cache")  # a data folder that is itself a junction
        read_only = base / "saves" / "slot.bin"
        read_only.write_bytes(b"slot")
        os.chmod(read_only, 0o444)
        self.run_probe("userdata", ROOT=base)
        for name in ("config", "data", "logs", "saves"):
            with self.subTest(name):
                self.assertFalse(os.path.lexists(base / name))
        self.assertTrue(_is_link(base / "cache"))  # neither followed nor deleted
        self.assertEqual(cache_kept.read_text(encoding="utf-8"), "keep me")
        self.assertEqual(kept.read_text(encoding="utf-8"), "keep me")
        self.assertEqual((base / "other" / "untouched.txt").read_text(encoding="utf-8"), "not a data folder")

    def test_the_data_delete_removes_symbolic_links_as_links(self):
        base = self._data_root()
        file_target = self._outside("file.txt")
        folder_target = self._outside("folder/inside.txt").parent
        try:
            os.symlink(file_target, base / "logs" / "file-link.txt")
            os.symlink(folder_target, base / "config" / "folder-link", target_is_directory=True)
        except OSError:
            self.skipTest("this account cannot make a symbolic link (SKIP, not a pass)")
        self.run_probe("userdata", ROOT=base)
        self.assertFalse(os.path.lexists(base / "logs"))
        self.assertFalse(os.path.lexists(base / "config"))
        self.assertEqual(file_target.read_text(encoding="utf-8"), "keep me")
        self.assertEqual((folder_target / "inside.txt").read_text(encoding="utf-8"), "keep me")

    def test_the_data_delete_does_nothing_through_a_linked_root(self):
        real = self.outside / "real-root"
        (real / "config").mkdir(parents=True)
        (real / "config" / "settings.json").write_text("{}", encoding="utf-8")
        (self.root / "local").mkdir()
        _junction(real, self.root / "local" / "Nakagawa")
        self.run_probe("userdata", ROOT=self.root / "local" / "Nakagawa")
        self.assertEqual((real / "config" / "settings.json").read_text(encoding="utf-8"), "{}")

    def _upgrade_tree(self) -> tuple[Path, Path]:
        """An install folder after the new files were copied, and the previous install's manifest."""
        app = self.root / "app"
        files = {"bin/nakagawa_player.exe": b"new player", "bin/kept.dll": b"kept", "LICENSE": b"licence"}
        for name, data in files.items():
            (app / name).parent.mkdir(parents=True, exist_ok=True)
            (app / name).write_bytes(data)
        (app / "SHA256SUMS.txt").write_text("".join(_sums_line(app / name, name) for name in sorted(files)),
                                            encoding="utf-8", newline="\n")
        old = {"bin/dropped.dll": b"dropped", "bin/edited.txt": b"as installed",
               "dropped-folder/nested/old.txt": b"old"}
        for name, data in old.items():
            (app / name).parent.mkdir(parents=True, exist_ok=True)
            (app / name).write_bytes(data)
        lines = [_sums_line(app / name, name) for name in sorted(old)]
        lines.append(f"{'0' * 64}  bin/nakagawa_player.exe\n")  # the older player: overwritten, never deleted
        lines.append(_sums_line(app / "bin" / "kept.dll", "BIN/Kept.DLL"))  # names compare without case
        (app / "bin" / "edited.txt").write_bytes(b"changed by the person")
        linked = self._outside("linked-target/old.txt", "behind a link")
        _junction(linked.parent, app / "linked")
        lines.append(_sums_line(linked, "linked/old.txt"))
        for name in verify_installer.PERSON_FILES:
            (app / name).parent.mkdir(parents=True, exist_ok=True)
            (app / name).write_bytes(b"mine")
        old_sums = self.root / "previous-SHA256SUMS.txt"
        old_sums.write_text("".join(lines), encoding="utf-8", newline="\n")
        return app, old_sums

    def test_an_upgrade_removes_only_dropped_unchanged_files_and_their_emptied_folders(self):
        app, old_sums = self._upgrade_tree()
        self.run_probe("upgrade", ROOT=app, OLDSUMS=old_sums)
        self.assertFalse((app / "bin" / "dropped.dll").exists())
        self.assertFalse((app / "dropped-folder").exists())
        self.assertEqual((app / "bin" / "edited.txt").read_bytes(), b"changed by the person")
        self.assertEqual((app / "bin" / "kept.dll").read_bytes(), b"kept")
        self.assertEqual((app / "bin" / "nakagawa_player.exe").read_bytes(), b"new player")
        self.assertTrue((app / "SHA256SUMS.txt").is_file())
        self.assertTrue(_is_link(app / "linked"))
        self.assertEqual((self.outside / "linked-target" / "old.txt").read_text(encoding="utf-8"), "behind a link")
        for name in verify_installer.PERSON_FILES:
            with self.subTest(name):
                self.assertEqual((app / name).read_bytes(), b"mine")

    #: A bad old-list line and the file it would name if Windows resolved it, carrying that file's real digest:
    #: each must make the whole list unused, so nothing at all is deleted.
    BAD_LINES = {
        "../outside.txt": "notes.txt",
        "C:/Windows/notepad.exe": "notes.txt",
        "bin//x": "notes.txt",
        "bin\\x": "notes.txt",
        "/notes.txt": "notes.txt",
        "notes.txt/": "notes.txt",
        "./notes.txt": "notes.txt",
        "bin/./dropped.dll": "bin/dropped.dll",
        "bin/../notes.txt": "notes.txt",
        # The person's portable data, in any case and through the aliases Windows strips.
        "bin/data/save.bin": "bin/data/save.bin",
        "BIN/Data/Save.bin": "bin/data/save.bin",
        "bin/data./save.bin": "bin/data/save.bin",
        "bin/data /save.bin": "bin/data/save.bin",
        "bin/data": "bin/data/save.bin",
        "bin/portable.txt": "bin/portable.txt",
        "Bin/PORTABLE.TXT": "bin/portable.txt",
        "bin/portable.txt.": "bin/portable.txt",
        "bin/portable.txt ": "bin/portable.txt",
        "bin/portable.txt..": "bin/portable.txt",
        "bin/PORTAB~1.TXT": "bin/portable.txt",
        # Setup's own top-level files.
        "unins000.exe": "unins000.exe",
        "UNINS000.DAT": "unins000.dat",
        "unins000.exe.": "unins000.exe",
        "SHA256SUMS.txt": "SHA256SUMS.txt",
        "sha256sums.TXT": "SHA256SUMS.txt",
        # Wildcards, streams, reserved and control characters.
        "bin/*.dll": "bin/dropped.dll",
        "bin/dropped?dll": "bin/dropped.dll",
        "bin/dropped.dll:stream": "bin/dropped.dll",
        "bin/<dropped>.dll": "bin/dropped.dll",
        "bin/\"dropped\".dll": "bin/dropped.dll",
        "bin/dropped|.dll": "bin/dropped.dll",
        "bin/drop\tped.dll": "bin/dropped.dll",
        "bin/drop\x01ped.dll": "bin/dropped.dll",
        "bin/drop\x7fped.dll": "bin/dropped.dll",
    }

    def test_an_upgrade_uses_no_manifest_with_a_bad_line(self):
        for bad, target_name in self.BAD_LINES.items():
            with self.subTest(bad):
                app, old_sums = self._upgrade_tree()
                try:
                    for own in ("unins000.exe", "unins000.dat"):
                        (app / own).write_bytes(b"Setup's own " + own.encode())
                    target = app / target_name
                    before = target.read_bytes()
                    with old_sums.open("a", encoding="utf-8", newline="\n") as stream:
                        stream.write(_sums_line(target, bad))
                    self.run_probe("upgrade", ROOT=app, OLDSUMS=old_sums)
                    self.assertTrue(target.is_file(), f"{target_name} was deleted")
                    self.assertEqual(target.read_bytes(), before)
                    self.assertTrue((app / "bin" / "dropped.dll").is_file(), "the list was used")
                finally:
                    shutil.rmtree(app)  # a junction inside is removed as a link
                    shutil.rmtree(self.outside / "linked-target")

    def test_an_upgrade_without_a_new_manifest_deletes_nothing(self):
        app, old_sums = self._upgrade_tree()
        (app / "SHA256SUMS.txt").unlink()
        self.run_probe("upgrade", ROOT=app, OLDSUMS=old_sums)
        self.assertEqual((app / "bin" / "dropped.dll").read_bytes(), b"dropped")


if __name__ == "__main__":
    unittest.main()
