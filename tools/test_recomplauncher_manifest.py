# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""recomplauncher.json: RecompLauncher's setup file for Hot Shots Tennis: Get a Grip.

RecompLauncher (https://recomplauncher.com) installs a release, runs its headless setup step
(``prepare``) once with the player's disc image, then starts the game (``start``) and edits the
player's settings file (``config`` and ``settings``). These tests hold the file to what the
player really does, with no disc and no network: every argument is an option the player takes,
every path is one the player writes, and every settings key and value is one the player reads.

The file's schema lives at https://recomplauncher.com/schema/recomplauncher.json. Tests never
touch the network, so the full schema check runs only against a copy you downloaded yourself:
``NK_RECOMPLAUNCHER_SCHEMA=<schema.json> python -m unittest tools.test_recomplauncher_manifest``
(it needs the ``jsonschema`` package). Without one, the structural checks here still apply the
schema's rules this file depends on (``additionalProperties: false``, the required fields and the
``inputs`` form that replaced the deprecated top-level ``game``).
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = ROOT / "recomplauncher.json"
MAIN_C = ROOT / "src" / "player" / "main.c"
PLAYER_STATE_C = ROOT / "src" / "player" / "player_state.c"
PLAYER_STATE_H = ROOT / "src" / "player" / "player_state.h"
PLATFORM_WIN32_C = ROOT / "src" / "core" / "nk_platform_win32.c"
HST_TITLE = ROOT / "assets" / "titles" / "hst-ucus98701.json"
PREVIEW_RELEASE = ROOT / "docs" / "PREVIEW_RELEASE.md"

#: The schema's top-level properties (its root has additionalProperties: false).
TOP_LEVEL_KEYS = {
    "inputs", "$schema", "format", "name", "repo", "game", "start", "prepare", "config",
    "folders", "exit_codes", "settings", "groups", "saves", "games", "looks", "mods", "actions",
    "system",
}
INPUT_KEYS = {"id", "label", "role", "optional", "shared", "destination", "formats", "check",
              "dumps", "contains", "into", "name", "extract", "sha256"}
CHECK_KEYS = {"file", "xdvdfs", "iso9660", "nup_build_id", "ids"}
START_KEYS = {"program", "args", "env"}
PREPARE_KEYS = {"program", "args", "label", "done", "env", "keeps_copy", "options"}
CONFIG_KEYS = {"id", "format", "file", "game_path", "ensure", "always", "new"}
SETTING_KEYS = {"id", "label", "help", "in", "choices", "defaults", "when", "group", "advanced",
                "slider", "args", "bindings"}
CHOICE_KEYS = {"label", "set", "args"}
ACTION_KEYS = {"id", "label", "help", "open", "clear"}
PLACE_KEYS = {"path", "if"}

#: Disc image formats the player imports (nk_disc_image_format, player_path_is_pbp_package).
PLAYER_DISC_FORMATS = {"iso", "cso", "zso", "chd", "pbp"}


def load_manifest() -> dict:
    loaded: dict = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    return loaded


def player_cli_options() -> dict[str, bool]:
    """k_player_cli_options in main.c: each option's name, and whether it takes a value."""
    source = MAIN_C.read_text(encoding="utf-8")
    table = source.split("k_player_cli_options[] = {", 1)[1].split("\n};", 1)[0]
    options = {}
    for name, value in re.findall(r'\{"(--[a-z0-9-]+)", (NULL|"[^"]*")', table):
        options[name] = value != "NULL"
    return options


def player_settings_keys() -> set[str]:
    """The top-level keys player_app_save_settings writes into settings.json."""
    source = PLAYER_STATE_C.read_text(encoding="utf-8")
    body = source.split("NkResult player_app_save_settings(", 1)[1].split("\n}\n", 1)[0]
    keys = set(re.findall(r'\\"([a-z0-9_]+)\\": ', body))
    keys.update(re.findall(r'player_settings_write_string\(f, "([a-z0-9_]+)"', body))
    # play_settings_write writes "enhanced" and "title_play" (the play modes, #2055).
    keys.update({"enhanced", "title_play"})
    return keys


def enhanced_part_words() -> dict[str, set[str]]:
    """k_enhanced_parts: each part's settings key and the words its value is written as."""
    source = PLAYER_STATE_C.read_text(encoding="utf-8")
    words = {
        name: set(re.findall(r'"([^"]*)"', values))
        for name, values in re.findall(
            r"static const char \*const (k_part_[a-z0-9_]+_words)\[\] = \{([^}]*)\};", source)
    }
    table = source.split("k_enhanced_parts[PLAYER_PART_COUNT] = {", 1)[1].split("\n};", 1)[0]
    parts = {}
    for key, words_name in re.findall(
            r'\{"([a-z0-9]+)",[^{}]*?\d+, k_part_[a-z0-9_]+_values, [A-Za-z0-9_]+, '
            r'(k_part_[a-z0-9_]+_words)', table, re.S):
        parts[key] = words[words_name]
    return parts


def json_literal(text: str) -> object:
    """A settings value as RecompLauncher writes it: the JSON text itself (strings quoted)."""
    return json.loads(text)


def check_places(test: unittest.TestCase, places: list) -> None:
    test.assertIsInstance(places, list)
    test.assertGreaterEqual(len(places), 1)
    for place in places:
        test.assertLessEqual(set(place), PLACE_KEYS)
        test.assertIn("path", place)


class RecompLauncherManifestShapeTests(unittest.TestCase):
    """The file follows the schema's rules: no field the schema does not allow."""

    def setUp(self) -> None:
        self.manifest = load_manifest()

    def test_top_level_fields_are_the_schemas(self) -> None:
        self.assertEqual(self.manifest["format"], 1)
        self.assertEqual(self.manifest["$schema"],
                         "https://recomplauncher.com/schema/recomplauncher.json")
        self.assertLessEqual(set(self.manifest), TOP_LEVEL_KEYS)
        # The deprecated top-level game field is never used: the copy is the input "game".
        self.assertNotIn("game", self.manifest)

    def test_the_players_copy_is_the_input_named_game(self) -> None:
        inputs = self.manifest["inputs"]
        self.assertEqual(len(inputs), 1)
        game = inputs[0]
        self.assertLessEqual(set(game), INPUT_KEYS)
        self.assertEqual((game["id"], game["role"]), ("game", "game"))
        self.assertTrue(game["label"])
        self.assertIn("UCUS-98701", game["label"])
        self.assertEqual(len(game["formats"]), 1)
        file_format = game["formats"][0]
        self.assertEqual(file_format["type"], "file")
        extensions = file_format["extensions"]
        self.assertEqual(len(extensions), len(set(extensions)))
        for extension in extensions:
            self.assertRegex(extension, r"^[A-Za-z0-9]+$")
        self.assertEqual(set(extensions), PLAYER_DISC_FORMATS)
        self.assertLessEqual(set(game["check"]), CHECK_KEYS)
        for disc in game["check"]["ids"]:
            self.assertRegex(disc, r"^[A-Za-z0-9._-]{3,16}$")

    def test_start_prepare_config_settings_and_actions_use_only_schema_fields(self) -> None:
        manifest = self.manifest
        self.assertLessEqual(set(manifest["start"]), START_KEYS)
        self.assertLessEqual(set(manifest["prepare"]), PREPARE_KEYS)
        for section in ("start", "prepare"):
            self.assertGreaterEqual(len(manifest[section]["program"]), 1)
            for argument in manifest[section]["args"]:
                self.assertIsInstance(argument, str)
        for config in manifest["config"]:
            self.assertLessEqual(set(config), CONFIG_KEYS)
            self.assertIn(config["format"], ("toml", "ini", "json"))
            check_places(self, config["file"])
            for block in ("ensure", "always", "new"):
                for value in config.get(block, {}).values():
                    self.assertIsInstance(value, str)
        group_ids = {group["id"] for group in manifest["groups"]}
        for group in manifest["groups"]:
            self.assertLessEqual(set(group), {"id", "advanced"})
        for setting in manifest["settings"]:
            self.assertLessEqual(set(setting), SETTING_KEYS)
            self.assertTrue(setting["id"] and setting["label"])
            self.assertIn(setting["group"], group_ids)
            self.assertGreaterEqual(len(setting["choices"]), 2)
            for choice in setting["choices"]:
                self.assertLessEqual(set(choice), CHOICE_KEYS)
        for name, places in manifest["folders"].items():
            check_places(self, places)
        for action in manifest["actions"]:
            self.assertLessEqual(set(action), ACTION_KEYS)
            self.assertIn(action["open"], manifest["folders"])
        for code, message in manifest["exit_codes"].items():
            self.assertRegex(code, r"^\d+$")
            self.assertIsInstance(message, str)

    def test_full_schema_when_a_downloaded_copy_is_given(self) -> None:
        schema_path = os.environ.get("NK_RECOMPLAUNCHER_SCHEMA", "")
        if not schema_path:
            self.skipTest("NOT_RUN: set NK_RECOMPLAUNCHER_SCHEMA to a downloaded copy of "
                          "https://recomplauncher.com/schema/recomplauncher.json")
        try:
            import jsonschema
        except ImportError:
            self.skipTest("NOT_RUN: the jsonschema package is not installed")
        schema = json.loads(Path(schema_path).read_text(encoding="utf-8"))
        errors = sorted(jsonschema.Draft7Validator(schema).iter_errors(self.manifest),
                        key=lambda error: list(error.path))
        self.assertEqual([f"{list(error.path)}: {error.message}" for error in errors], [])


class RecompLauncherManifestPlayerTests(unittest.TestCase):
    """The file says what the player really takes, writes and reads."""

    def setUp(self) -> None:
        self.manifest = load_manifest()
        self.options = player_cli_options()
        title = json.loads(HST_TITLE.read_text(encoding="utf-8"))
        self.disc_id = title["disc"]["id"]

    def assert_player_arguments(self, arguments: list[str]) -> dict[str, str]:
        """Every argument is one player option, written --name=value or --name; returns them."""
        given = {}
        for argument in arguments:
            name, equals, value = argument.partition("=")
            self.assertIn(name, self.options, f"{argument}: not an option of nakagawa_player")
            self.assertEqual(self.options[name], bool(equals), argument)
            self.assertNotIn(name, given, f"{name} is given twice")
            given[name] = value
        return given

    def test_programs_are_the_release_players(self) -> None:
        for section in ("start", "prepare"):
            self.assertEqual(self.manifest[section]["program"],
                             ["bin/nakagawa_player.exe", "bin/nakagawa_player"])

    def test_prepare_builds_from_the_copy_into_the_data_folder(self) -> None:
        prepare = self.manifest["prepare"]
        given = self.assert_player_arguments(prepare["args"])
        self.assertIn("--prepare", given)
        self.assertEqual(given["--user-data-root"], "{data}")
        self.assertEqual(given["--expect-disc"], self.disc_id)
        # The copy is one argument, so a launcher that leaves it out leaves nothing behind.
        self.assertEqual(given["--game"], "{input:game}")
        self.assertEqual(sum("{input:game}" in argument for argument in prepare["args"]), 1)
        # The build cannot run again without the copy (it reads the disc image), so the
        # launcher must not be told it can.
        self.assertFalse(prepare.get("keeps_copy", False))
        # The file nk_cli.py writes last, by an atomic move of the finished package
        # (packages/<disc ID>/package.json, the authentic package --prepare always builds).
        self.assertEqual(prepare["done"], [f"{{data}}/packages/{self.disc_id}/package.json"])
        nk_cli = (ROOT / "tools" / "nk_cli.py").read_text(encoding="utf-8")
        self.assertIn('user_root / "packages" / disc_id', nk_cli)

    def test_start_plays_the_game_from_the_same_data_folder(self) -> None:
        given = self.assert_player_arguments(self.manifest["start"]["args"])
        self.assertEqual(given, {"--user-data-root": "{data}", "--launch-title": self.disc_id})
        source = PLAYER_STATE_H.read_text(encoding="utf-8")
        self.assertIn('#define PLAYER_LAUNCH_TITLE_ARG "--launch-title="', source)
        # Play needs no copy: it starts the built package.
        self.assertNotIn("{input:game}", json.dumps(self.manifest["start"]))

    def test_folders_and_saves_are_the_data_roots_folders(self) -> None:
        platform = PLATFORM_WIN32_C.read_text(encoding="utf-8")
        for kind, leaf in (("NK_PATH_CONFIG", "config"), ("NK_PATH_LOGS", "logs"),
                           ("NK_PATH_SAVES", "saves")):
            self.assertIsNotNone(re.search(rf'case {kind}:\s+leaf = "{leaf}";', platform), kind)
        folders = self.manifest["folders"]
        self.assertEqual(folders["saves"], [{"path": "{data}/saves"}])
        self.assertEqual(folders["logs"], [{"path": "{data}/logs"}])
        self.assertEqual(folders["data"], [{"path": "{data}"}])
        # Cheats are read from <data root>/cheats/<disc ID>.ini (launch_cheats_file).
        self.assertEqual(folders["cheats"], [{"path": "{data}/cheats"}])
        launch = (ROOT / "src" / "core" / "nk_launch.c").read_text(encoding="utf-8")
        self.assertIn('"%s%ccheats%c%s.ini"', launch)
        self.assertEqual(self.manifest["saves"], ["{data}/saves"])

    def test_config_is_the_players_settings_file(self) -> None:
        (config,) = self.manifest["config"]
        self.assertEqual((config["id"], config["format"]), ("main", "json"))
        self.assertEqual(config["file"], [{"path": "{data}/config/settings.json"}])
        self.assertIn('"settings.json"', PLAYER_STATE_C.read_text(encoding="utf-8"))
        # A file the launcher makes must carry a schema version the player reads.
        header = PLAYER_STATE_H.read_text(encoding="utf-8")
        current = int(re.search(r"#define NK_PLAYER_SETTINGS_SCHEMA_VERSION (\d+)", header)[1])
        version = int(json_literal(config["ensure"]["schema_version"]))
        self.assertTrue(1 <= version <= current, version)
        # The player never updates itself under the launcher, which brings every release.
        self.assertEqual(config["always"], {"check_updates_at_start": "false"})
        self.assertIn("check_updates_at_start", player_settings_keys())

    def test_every_setting_writes_a_key_and_value_the_player_reads(self) -> None:
        top_level = player_settings_keys()
        parts = enhanced_part_words()
        self.assertIn("wide", parts)
        self.assertIn("af", parts)
        self.assertIn("ssaa", parts)
        plain = {
            "fullscreen": bool, "vsync": bool, "resolution_scale": int, "present_scaling": str,
        }
        scaling_names = {"sharp", "integer", "bilinear"}
        play_modes = {"authentic", "enhanced", "ace"}
        title_key = f"title_play/{self.disc_id}/mode"
        for setting in self.manifest["settings"]:
            self.assertEqual(setting.get("in", "main"), "main")
            keys = [set(choice["set"]) for choice in setting["choices"]]
            self.assertTrue(all(len(k) == 1 and k == keys[0] for k in keys), setting["id"])
            self.assertEqual(set(setting["defaults"]), keys[0], setting["id"])
            (key,) = keys[0]
            values = [json_literal(choice["set"][key]) for choice in setting["choices"]]
            values.append(json_literal(setting["defaults"][key]))
            self.assertEqual(len(set(map(str, values[:-1]))), len(values) - 1, setting["id"])
            with self.subTest(key=key):
                if key in plain:
                    self.assertIn(key, top_level)
                    for value in values:
                        self.assertIs(type(value), plain[key])
                    if key == "resolution_scale":
                        self.assertLessEqual(set(values), {1, 2, 3, 4})
                    if key == "present_scaling":
                        self.assertLessEqual(set(values), scaling_names)
                elif key.startswith("enhanced/"):
                    part = key.split("/", 1)[1]
                    self.assertIn(part, parts)
                    self.assertLessEqual(set(values), parts[part])
                elif key == title_key:
                    self.assertLessEqual(set(values), play_modes)
                else:
                    self.fail(f"{key}: not a key the player's settings.json has")
        source = PLAYER_STATE_C.read_text(encoding="utf-8")
        for word in play_modes:
            self.assertIn(f'strcmp(word, "{word}") == 0', source)
        self.assertIn('nk_json_obj_get(root, "title_play")', source)

    def test_release_layout_puts_the_file_beside_bin(self) -> None:
        recipe = PREVIEW_RELEASE.read_text(encoding="utf-8")
        layout = recipe.split("function New-WindowsReleaseLayout", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("Copy-Item recomplauncher.json (Join-Path $Layout '.')", layout)
        self.assertIn("$Bin = Join-Path $Layout 'bin'", layout)


if __name__ == "__main__":
    unittest.main()
