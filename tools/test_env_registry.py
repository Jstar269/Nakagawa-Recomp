#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for the runtime registry and documented launcher stability classes.

The header is one table naming every variable the runtime reads, with its kind, default,
reading files and purpose. These tests keep it honest:

1. Every row is well formed and the names are unique and sorted.
2. Every `SR_*` string literal in src/rt, and every name passed to getenv or to a
   switch-reading helper, has a row (a name ending in _N covers its numbered family). The
   src/core files mk/runtime_sources.mk links into the runtime are read the same way.
3. Every row is read or named, as a whole identifier, somewhere in those files (no stale
   rows), and its reading files exist.
4. The player's accepted options and SETUP.md's runtime and project variables
   have explicit classes in the packaging architecture document.
"""

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
RT = ROOT / "src" / "rt"
SRC = ROOT / "src"
NATIVE_TESTS = ROOT / "tests" / "native"
REGISTRY = RT / "env_registry.h"
PLAYER = SRC / "player" / "main.c"
SETUP = ROOT / "docs" / "SETUP.md"
STABILITY = ROOT / "docs" / "RUNTIME_PACKAGING_ARCHITECTURE.md"
# The makefiles whose assignments name the runtime's sources, and the lists that link it.
MAKE_FILES = (ROOT / "Makefile", *sorted((ROOT / "mk").glob("*.mk")))
RUNTIME_LISTS = ("RT_SRCS", "SDL3VK_SRCS")

KINDS = {"flag-presence", "flag-nonzero", "number", "path", "string", "list", "system"}
VENDORED = ("h264_ffmpeg_sub",)
SOURCE_SUFFIXES = (".c", ".h", ".cpp", ".inc")

ROW_RE = re.compile(
    r'^\s*X\("(?P<name>[A-Za-z0-9_]+)", "(?P<kind>[a-z-]+)", "(?P<default>(?:[^"\\]|\\.)*)", '
    r'"(?P<readers>(?:[^"\\]|\\.)*)", "(?P<purpose>(?:[^"\\]|\\.)*)"\)(?: \\)?$'
)
GETENV_RE = re.compile(r'\b_?w?getenv\(\s*L?"([A-Za-z][A-Za-z0-9_]*)"\s*\)')
SR_LITERAL_RE = re.compile(r'"(SR_[A-Z0-9_]+)"')
ASSIGNMENT_RE = re.compile(r"^\s*([A-Za-z0-9_]+)\s*(?::=|\?=|\+=|=)(.*)$")
REFERENCE_RE = re.compile(r"\$\(([A-Za-z0-9_]+)\)")

CLI_TABLE_RE = re.compile(r"^static const PlayerCliOption k_player_cli_options\[\] = \{\n(.*?)^\};$",
                          re.MULTILINE | re.DOTALL)
C_STRING = r'"(?:[^"\\\n]|\\.)*"'
CLI_TOKEN_RE = re.compile(
    rf'\{{\s*"(?P<option>--[a-z0-9-]+)"\s*,\s*(?:NULL|{C_STRING})\s*,'
    rf'\s*(?P<help>NULL|(?:{C_STRING}\s*)+)\s*,\s*(?:true|false)\s*\}},?'
    r'|(?P<directive>\#[^\n]+)|/\*[\s\S]*?\*/|//[^\n]*|\s+'
)
ENV_SYMBOL_RE = re.compile(r"\b[A-Z][A-Z0-9_]+\b")
CLI_CLASSES = {
    "Stable user-facing, printed in `--help`",
    "Documented QA, capture and scripted-run flags (**Proposal**)",
    "Hidden but accepted",
    "Removed, kept as a tombstone",
    "Compile-gated test options",
}
QA_HELP = {
    "--screenshot": True,
    "--headless-launch": True,
    "--headless-accept-presented-frames": False,
    "--headless-timeout-ms": False,
}
RUNTIME_OVERRIDES = "Documented runtime overrides"
LAUNCHER_OWNED = "Launcher-owned, not user knobs"
ENV_CLASSES = {
    "Build and toolchain inputs", RUNTIME_OVERRIDES, LAUNCHER_OWNED,
    "Inherited fallback, not a launcher export", "Retired runtime overrides",
    "Diagnostic and developer-only", "Host and system", "Test-only",
}
# These host variables belong to the launcher/Python setup and are outside the runtime registry.
SETUP_HOST_ONLY = {"APPDATA", "LOCALAPPDATA"}
SETUP_TOOL_ONLY = {"CODEGEN_ORACLE", "LOCAL_COSIM_TRACE", "MICROTEST_MODULE", "MICROTEST_ORACLE",
                   "PSP_HARDWARE_TRACE"}
# SETUP names this compile-time limit next to the manifest overlay directory.
SETUP_CODE_SYMBOLS = {"NK_MANIFEST_MAX_OVERLAYS"}
DUAL_USE_OVERRIDES = {"SR_CHEATS", "SR_CHEATS_FILE", "SR_USER_DATA_ROOT"}


def player_cli_options(source):
    """Return option, help visibility and UI-test gating across platform branches."""
    table = CLI_TABLE_RE.search(source)
    if not table:
        raise ValueError("missing k_player_cli_options table")
    options = []
    conditions = []
    body = table.group(1)
    pos = 0
    while pos < len(body):
        token = CLI_TOKEN_RE.match(body, pos)
        if not token:
            raise ValueError(f"unrecognized k_player_cli_options entry: {body[pos:].splitlines()[0]!r}")
        pos = token.end()
        if name := token["option"]:
            options.append((name, token["help"] != "NULL", any(conditions)))
        elif directive := token["directive"]:
            kind, *args = directive[1:].split()
            if kind in {"if", "ifdef", "ifndef"}:
                conditions.append(kind == "ifdef" and args == ["NK_PLAYER_UI_REGRESSION_TEST"])
            elif kind in {"else", "elif"}:
                conditions[-1] = False
            elif kind == "endif":
                conditions.pop()
    if conditions:
        raise ValueError("unterminated conditional in k_player_cli_options")
    return options


def stability_classes(document, column):
    """Read names from one class table, leaving contract text and examples out."""
    section = re.search(r"^### Flags and environment variables\n(.*?)(?=^### |\Z)",
                        document, re.MULTILINE | re.DOTALL)
    if not section:
        raise ValueError("missing Flags and environment variables section")
    classes = {}
    in_table = False
    for line in section[1].splitlines():
        if line.startswith(f"| Class | {column} |"):
            in_table = True
        elif in_table:
            if not line.startswith("|"):
                break
            _, label, names, _ = line.split("|", 3)
            label = label.strip()
            if label.startswith("---"):
                continue
            if label in classes:
                raise ValueError(f"duplicate stability class: {label}")
            members = [entry.split()[0] for entry in re.findall(r"`([^`]+)`", names)]
            if not members or len(members) != len(set(members)):
                raise ValueError(f"empty or duplicate stability names: {label}")
            classes[label] = set(members)
    return classes


def registry_rows():
    rows = []
    in_table = False
    for number, line in enumerate(REGISTRY.read_text(encoding="utf-8").split("\n"), 1):
        if line.startswith("#define NK_ENV_REGISTRY(X)"):
            in_table = True
            continue
        if in_table:
            if not line.strip():
                break
            match = ROW_RE.match(line)
            rows.append((number, line, match))
    return rows


def make_assignments(texts):
    """Variable -> every word assigned to it (:=, =, ?= and +=) in the given makefile texts."""
    values = {}
    for text in texts:
        for line in text.replace("\\\n", " ").split("\n"):
            match = ASSIGNMENT_RE.match(line)
            if match:
                words = match.group(2).split("#", 1)[0].split()
                values.setdefault(match.group(1), []).extend(words)
    return values


def linked_core_sources(values, lists=RUNTIME_LISTS):
    """The src/core/*.c files the named lists link, with $(VAR) references expanded.

    A variable assigned once per target OS (PLAYER_PLATFORM_SRC) contributes every file it names.
    """
    found = set()

    def expand(words, seen):
        for word in words:
            ref = REFERENCE_RE.fullmatch(word)
            if ref:
                if ref.group(1) not in seen:
                    expand(values.get(ref.group(1), ()), seen | {ref.group(1)})
            elif word.startswith("src/core/") and word.endswith(".c"):
                found.add(word)

    for name in lists:
        expand(values.get(name, ()), {name})
    return sorted(found)


def runtime_core_sources():
    values = make_assignments(path.read_text(encoding="utf-8") for path in MAKE_FILES)
    return [ROOT / rel for rel in linked_core_sources(values)]


def source_files():
    for path in sorted(RT.rglob("*")):
        if path.suffix in SOURCE_SUFFIXES and not any(part in VENDORED for part in path.parts):
            if path != REGISTRY:
                yield path
    # The src/core files linked into the runtime read switches of their own (the input profile).
    yield from runtime_core_sources()
    # The native self-tests moved out of src/rt (#1531) but are still runtime readers.
    yield from sorted(NATIVE_TESTS.glob("*_selftest.c"))


def reader_name(path):
    """How a row names a reading file: relative to src/rt or tests/native, else core/<file>."""
    if NATIVE_TESTS in path.parents:
        return path.relative_to(NATIVE_TESTS).as_posix()
    if RT in path.parents:
        return path.relative_to(RT).as_posix()
    return path.relative_to(SRC).as_posix()


def row_is_named(name, texts):
    """Whether a row's name appears as a whole identifier in any text.

    A _N row is named by its prefix followed by a slot number or a printf conversion
    ("SR_WATCH_%d"), never by a longer name that merely starts with the prefix.
    """
    if name.endswith("_N"):
        pattern = re.compile(rf"(?<![A-Za-z0-9_]){re.escape(name[:-1])}(?:\d|%)")
    else:
        pattern = re.compile(rf"(?<![A-Za-z0-9_]){re.escape(name)}(?![A-Za-z0-9_])")
    return any(pattern.search(text) for text in texts)


def covered(name, names, families):
    if name in names:
        return True
    return any(re.fullmatch(prefix + r"\d+", name) for prefix in families)


class EnvRegistryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rows = registry_rows()
        cls.entries = [m.groupdict() for _, _, m in cls.rows if m]
        cls.names = {e["name"] for e in cls.entries}
        cls.families = {e["name"][:-1] for e in cls.entries if e["name"].endswith("_N")}
        cls.texts = {reader_name(path): path.read_text(encoding="utf-8", errors="replace")
                     for path in source_files()}
        cls.used = {}
        cls.getenv_names = set()
        for rel, text in cls.texts.items():
            cls.getenv_names.update(GETENV_RE.findall(text))
            for regex in (GETENV_RE, SR_LITERAL_RE):
                for match in regex.finditer(text):
                    cls.used.setdefault(match.group(1), set()).add(rel)

    def test_every_row_is_well_formed(self):
        self.assertTrue(self.rows, "no rows found after #define NK_ENV_REGISTRY(X)")
        for number, line, match in self.rows:
            self.assertIsNotNone(match, f"env_registry.h:{number} is not a well-formed row: {line!r}")
        for entry in self.entries:
            name = entry["name"]
            self.assertIn(entry["kind"], KINDS, f"{name}: unknown kind {entry['kind']!r}")
            for field in ("default", "readers", "purpose"):
                self.assertTrue(entry[field].strip(), f"{name}: empty {field}")
            self.assertLessEqual(len(entry["purpose"]), 160, f"{name}: purpose is more than one line")

    def test_names_are_unique_and_sorted(self):
        names = [e["name"] for e in self.entries]
        self.assertEqual(len(names), len(set(names)), "duplicate row")
        self.assertEqual(names, sorted(names), "rows must stay sorted by name")

    def test_every_name_the_runtime_reads_has_a_row(self):
        missing = sorted(
            name for name in self.used
            if (name.startswith("SR_") or name in self.getenv_names)
            and not covered(name, self.names, self.families)
        )
        self.assertEqual(missing, [], "read in the runtime but missing from src/rt/env_registry.h: %s"
                         % ", ".join(missing))

    def test_runtime_linked_core_files_are_scanned(self):
        """The input profile's switches are read in src/core, which the runtime links."""
        core = {rel for rel in self.texts if rel.startswith("core/")}
        self.assertIn("core/nk_input_profile.c", core)
        self.assertIn("core/nk_shared_helpers.c", core)
        self.assertLessEqual({"core/nk_platform_posix.c", "core/nk_platform_win32.c"}, core,
                             "PLAYER_PLATFORM_SRC names one platform file per target OS; both are read")
        self.assertIn("core/nk_input_profile.c", self.used.get("NK_INPUT_PROFILE", ()))
        self.assertIn("core/nk_input_profile.c", self.used.get("SR_INPUT_PROFILE", ()))

    def test_linked_core_sources_expand_references_and_every_target_os(self):
        values = make_assignments([
            "PLAYER_PLATFORM_SRC := src/core/nk_platform_win32.c\n"
            "PLAYER_PLATFORM_SRC := src/core/nk_platform_posix.c\n"
            "UI := src/ui/a.c src/core/nk_ui_host.c # src/core/commented.c\n"
            "RT_SRCS := src/rt/a.c \\\n    src/core/nk_json.c \\\n    $(UI) $(PLAYER_PLATFORM_SRC)\n"
            "SDL3VK_SRCS := src/rt/b.c $(RT_SRCS) src/core/nk_input_profile.c\n"
            "OTHER := src/core/nk_unlinked.c\n"
        ])
        self.assertEqual(linked_core_sources(values), [
            "src/core/nk_input_profile.c", "src/core/nk_json.c", "src/core/nk_platform_posix.c",
            "src/core/nk_platform_win32.c", "src/core/nk_ui_host.c",
        ])

    def test_every_row_is_read_somewhere(self):
        texts = list(self.texts.values())
        stale = [entry["name"] for entry in self.entries
                 if not (entry["name"] in self.used or row_is_named(entry["name"], texts))]
        self.assertEqual(stale, [], "rows nothing in the runtime names: %s" % ", ".join(stale))

    def test_stale_row_check_matches_whole_identifiers(self):
        """A row whose reader is gone stays stale when a longer name or a family prefix remains."""
        live = ['getenv("SR_FOO_BAR");', 'getenv("SR_WATCH_CONTEXT_PC");', "/* SR_FOOD */"]
        self.assertFalse(row_is_named("SR_FOO", live))
        self.assertFalse(row_is_named("SR_WATCH_N", live))
        self.assertFalse(row_is_named("SR_OO", ['getenv("NK_SR_OO");']))
        self.assertTrue(row_is_named("SR_FOO", ["/* SR_FOO stays honoured */"]))
        self.assertTrue(row_is_named("SR_WATCH_N", ['snprintf(n, sizeof n, "SR_WATCH_%d", i);']))
        self.assertTrue(row_is_named("SR_WATCH_N", ['getenv("SR_WATCH_3")']))

    def test_reading_files_exist(self):
        for entry in self.entries:
            for rel in entry["readers"].split(","):
                self.assertTrue(
                    (RT / rel).is_file() or (NATIVE_TESTS / rel).is_file()
                    or (rel.startswith("core/") and (SRC / rel).is_file()),
                    f"{entry['name']}: reader {rel} does not exist",
                )

    def test_player_options_have_stability_classes(self):
        options = player_cli_options(PLAYER.read_text(encoding="utf-8"))
        classes = stability_classes(STABILITY.read_text(encoding="utf-8"), "Options")
        self.assertEqual(set(classes), CLI_CLASSES, "player CLI stability class headings changed")
        classified = set().union(*classes.values())
        self.assertEqual({name for name, _, _ in options}, classified,
                         "k_player_cli_options and documented option names differ")
        self.assertEqual(sum(map(len, classes.values())), len(classified),
                         "a player option has multiple stability classes")

        stable = classes["Stable user-facing, printed in `--help`"]
        qa = classes["Documented QA, capture and scripted-run flags (**Proposal**)"]
        self.assertEqual(qa, set(QA_HELP), "documented QA options need explicit classification")
        self.assertIn("--runtime-bin", classes["Hidden but accepted"], "old runtime-root spelling")
        self.assertEqual(classes["Removed, kept as a tombstone"], {"--ui"})
        for name, has_help, test_only in options:
            with self.subTest(option=name):
                self.assertEqual(has_help, name in stable or QA_HELP.get(name, False),
                                 "option class disagrees with --help visibility")
                self.assertEqual(test_only, name in classes["Compile-gated test options"],
                                 "option class disagrees with NK_PLAYER_UI_REGRESSION_TEST")

    def test_documented_environment_has_explicit_stability_classes(self):
        classes = stability_classes(STABILITY.read_text(encoding="utf-8"), "Variables")
        self.assertEqual(set(classes), ENV_CLASSES, "environment stability class headings changed")
        classified = set().union(*classes.values())
        self.assertEqual(sorted(name for name in classified if not ENV_SYMBOL_RE.fullmatch(name)),
                         [], "list each variable explicitly in its stability class")
        setup = set(ENV_SYMBOL_RE.findall(SETUP.read_text(encoding="utf-8"))) - SETUP_CODE_SYMBOLS
        documented = {name for name in setup if name.startswith(("SR_", "NK_"))
                      or covered(name, self.names, self.families)
                      or name in self.getenv_names | SETUP_HOST_ONLY | SETUP_TOOL_ONLY}
        # A numbered spelling in SETUP means the corresponding registered family.
        documented = {re.sub(r"\d+$", "N", name)
                      if name not in self.names and covered(name, self.names, self.families) else name
                      for name in documented}
        self.assertEqual(sorted(documented - classified), [], "SETUP.md variables missing a class")

        build = classes["Build and toolchain inputs"]
        host = classes["Host and system"]
        test = classes["Test-only"]
        runtime_names = classified - build - SETUP_HOST_ONLY - SETUP_TOOL_ONLY
        self.assertEqual(sorted(runtime_names - self.names), [],
                         "classified variables missing from src/rt/env_registry.h")
        self.assertEqual(sorted(build & self.names), [], "runtime variables classed as build inputs")
        self.assertLessEqual(SETUP_HOST_ONLY, host)
        self.assertLessEqual(SETUP_TOOL_ONLY, test)
        self.assertEqual(host & self.names, {e["name"] for e in self.entries
                                            if e["name"] in classified and e["kind"] == "system"})
        self.assertEqual(classes["Inherited fallback, not a launcher export"], {"SR_INPUT_PROFILE"})
        self.assertIn("NK_INPUT_PROFILE", classes[LAUNCHER_OWNED])
        self.assertLessEqual({"SR_DEBUG", "SR_GE_TIMING", "SR_HEADLESS_ACCEPT_PRESENTED_FRAMES"},
                             classes["Diagnostic and developer-only"],
                             "SETUP documents these runtime diagnostics in their developer role")
        self.assertEqual(classes["Retired runtime overrides"], {e["name"] for e in self.entries
                         if e["name"] in classified and e["purpose"].startswith("Retired ")})
        self.assertEqual(classes[RUNTIME_OVERRIDES] & classes[LAUNCHER_OWNED], DUAL_USE_OVERRIDES)
        self.assertEqual(sum(map(len, classes.values())), len(classified) + len(DUAL_USE_OVERRIDES),
                         "unexpected overlap between environment stability classes")


if __name__ == "__main__":
    unittest.main()
