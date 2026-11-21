# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The failure-code taxonomy and everything that emits a code agree.

``assets/failure_codes.json`` is the machine-readable list, ``docs/FAILURE_CODES.md`` explains
each code, and the emitters are the bring-up (``nk_cli.py``, ``title_qualification.py``), the
tool routes, game-file staging, built-in decryption, the disc checks, the player's problem
report and the library tools. Each test reads the emitter's source, so a code added anywhere
without its entry fails here.
"""

from __future__ import annotations

import ast
import json
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

from nk_core import decrypt_tool  # noqa: E402
import hle_sources  # noqa: E402

REGISTRY = json.loads((ROOT / "assets" / "failure_codes.json").read_text(encoding="utf-8"))
DOC = (ROOT / "docs" / "FAILURE_CODES.md").read_text(encoding="utf-8")
CODE = re.compile(r"^[A-Z][A-Z0-9]*(?:_[A-Z0-9]+)+$")
#: Any quoted all-caps literal of the registry's shape, with or without an underscore.
C_CODE = re.compile(r'"([A-Z][A-Z0-9_]*[A-Z0-9])"')


#: A C lookup-table row ``{"CODE", "text"}``, such as the player's badge table: it names a code
#: without emitting it.
C_TABLE_ROW = re.compile(r'\{\s*"[A-Z][A-Z0-9_]*"\s*,\s*"[^"\n]*"\s*\}')


def _emitted_literals(path: Path, text: str) -> set[str]:
    """The quoted all-caps literals in an emitter source, leaving out C lookup-table rows."""
    if path.suffix in (".c", ".h"):
        text = C_TABLE_ROW.sub("", text)
    return set(re.findall(r"[\"']([A-Z][A-Z0-9_]*)[\"']", text))


def codes_in(*vocabularies: str) -> set[str]:
    return {code for code, entry in REGISTRY["codes"].items()
            if set(entry["in"]) & set(vocabularies)}


def _tree(relative: str) -> ast.Module:
    return ast.parse((ROOT / relative).read_text(encoding="utf-8"))


def _values(node: ast.AST | None) -> set[str]:
    """The code constants an expression can evaluate to (not the ones it only compares)."""
    if isinstance(node, ast.Constant) and isinstance(node.value, str) and CODE.match(node.value):
        return {node.value}
    if isinstance(node, ast.IfExp):
        return _values(node.body) | _values(node.orelse)
    if isinstance(node, ast.BoolOp):
        return set().union(*(_values(value) for value in node.values))
    if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) \
            and node.func.attr == "get" and isinstance(node.func.value, ast.Dict):
        # {kind: code, ...}.get(kind, default)
        found = set().union(*(_values(value) for value in node.func.value.values))
        return found | (_values(node.args[1]) if len(node.args) > 1 else set())
    return set()


def _assigned(tree: ast.Module, names: set[str], keys: set[str], calls: dict[str, int],
              keywords: set[str]) -> set[str]:
    """Codes a module assigns to ``names``, stores under ``keys`` or passes to ``calls``."""
    found: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Call):
            func = node.func
            name = func.id if isinstance(func, ast.Name) else getattr(func, "attr", None)
            if name in calls and len(node.args) > calls[name]:
                found |= _values(node.args[calls[name]])
            for keyword in node.keywords:
                if keyword.arg in keywords:
                    found |= _values(keyword.value)
        elif isinstance(node, ast.Dict):
            for key, value in zip(node.keys, node.values, strict=True):
                if isinstance(key, ast.Constant) and key.value in keys:
                    found |= _values(value)
        elif isinstance(node, (ast.Assign, ast.AnnAssign)):
            assigned_value = node.value
            targets = node.targets if isinstance(node, ast.Assign) else [node.target]
            for target in targets:
                if isinstance(target, ast.Name) and target.id in names:
                    found |= _values(assigned_value)
                elif isinstance(target, ast.Subscript) and isinstance(target.slice, ast.Constant) \
                        and target.slice.value in keys:
                    found |= _values(assigned_value)
                elif isinstance(target, ast.Tuple):
                    for index, element in enumerate(target.elts):
                        if isinstance(element, ast.Name) and element.id in names:
                            found |= _values(
                                assigned_value.elts[index]
                                if isinstance(assigned_value, ast.Tuple)
                                else assigned_value
                            )
    return found


def _module_dict(tree: ast.Module, name: str) -> ast.Dict:
    for node in tree.body:
        value: ast.expr | None
        if isinstance(node, ast.Assign):
            targets = node.targets
            value = node.value
        elif isinstance(node, ast.AnnAssign):
            targets = [node.target]
            value = node.value
        else:
            continue
        if any(isinstance(target, ast.Name) and target.id == name for target in targets):
            assert isinstance(value, ast.Dict), name
            return value
    raise AssertionError(f"{name} is not a module-level dict")


def _c_function(relative: str, name: str) -> str:
    text = (ROOT / relative).read_text(encoding="utf-8")
    start = re.search(rf"^[^\n;]*\b{re.escape(name)}\s*\([^;{{]*\)\s*\{{", text, re.M)
    assert start, f"{name} not found in {relative}"
    depth, index = 0, start.end() - 1
    for index in range(start.end() - 1, len(text)):
        depth += {"{": 1, "}": -1}.get(text[index], 0)
        if depth == 0:
            break
    return text[start.start():index + 1]


#: Python modules whose exceptions carry a route code (the ``route`` vocabulary).
ROUTE_SOURCES = (
    "tools/nk_cli.py", "tools/title_codegen_plan.py", "tools/package_notices.py",
    "tools/imports.py", "tools/psp_import_table.py", "tools/analyze.py", "tools/codegen.py",
    "tools/nk_core/iso_inspect.py",
)
ROUTE_EXCEPTIONS = {
    "PackageRouteError": 0, "ImportTableError": 0, "ExperimentalModuleIntakeError": 0,
    "ExperimentalProfileBuildError": 0,
}

#: Player and core sources whose literals the problem report can show.
PLAYER_SOURCES = (
    sorted(str(path.relative_to(ROOT).as_posix()) for path in (ROOT / "src" / "player").glob("*.c"))
    + ["src/core/nk_launch.c", "src/core/nk_disc_expand.c", "src/core/nk_disc_expand.h",
       "src/core/nk_psp_crypto.c", "src/core/nk_iso.c"]
)
#: Uppercase literals in those sources that are not failure codes.
PLAYER_NOT_CODES = {
    # preflight check names (the bring-up report's preflight_checks[].code)
    "AUDIO_OUTPUT", "DATA_ROOT", "DISC_SFO", "GAME_BUILD", "GUEST_MODULES", "RUNTIME_PACKAGE", "SYSTEM_FONTS",
    "TITLE_PROFILE",
    # PARAM.SFO keys, disc folders and placeholders
    "DISC_ID", "DISC_VERSION", "PSP_GAME",
    # the success status of a decrypt (nk_psp_status_name for NK_PSP_OK)
    "VALIDATED_DECRYPTED",
    # how a disc check ended, in the player's DISC_CHECK log line (NkDiscVerifyResult)
    "READ_FAILED", "LIST_UNUSABLE",
    # gamepad and pointer names, UI actions and the UI test harness's tokens
    "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT", "DPAD_UP", "LEFT_SHOULDER", "RIGHT_SHOULDER",
    "MOUSE_DOWN", "MOUSE_UP", "MOUSE_X1", "MOUSE_X1_DOWN", "MOUSE_X1_UP", "OPEN_FOLDER",
    "ASSERT_GAME_RUNNING", "DROP_BEGIN", "DROP_COMPLETE", "FOCUS_GAINED", "FOCUS_LOST",
    "INVALIDATE_PACKAGE_CACHE", "RENDER_DEVICE_RESET",
    # an environment variable the package builder reads to find the MSYS2 UCRT64 bin
    "MSYS_PATH",
    # cover art: fetch status names (nk_cover_art_status_name, used by tests only) and the cache-clear
    # error code, which the Refresh notice replaces with its own text; neither reaches the problem report
    "COVER_ART_CLEAR_FAILED", "INVALID_PNG", "IO_ERROR", "NOT_FOUND", "NO_MATCH", "REDIRECT_REFUSED",
    "TOO_LARGE",
    # single words. Keyboard keys, gamepad buttons and labels, and the UI test harness's tokens
    "ALT", "BACK", "BACKSPACE", "BKSP", "CLOSE", "CREATE", "CTRL", "DEL", "DOWN", "EAST", "END",
    "ESCAPE", "EXTRA", "F1", "F12", "GUIDE", "HOME", "INS", "L2", "L3", "LEFT", "LS", "LT", "NONE",
    "NORTH", "P1", "P2", "P3", "P4", "PAGEDOWN", "PAGEUP", "QUIT", "R2", "R3", "RETURN", "RIGHT",
    "RS", "RT", "SHIFT", "SLASH", "SOUTH", "SPACE", "START", "TAB", "TOUCH", "UP", "VIEW", "WEST",
    "ZL", "ZR",
    # preflight check names
    "EXECUTABLE", "EXPERIMENTAL",
    # runtime-package and cover-art status words, a disc check's result and the package
    # builder's event statuses, none of them a problem-report code
    "OK", "MISSING", "STALE", "INCOMPATIBLE", "UNKNOWN", "UNSUPPORTED", "HASHED", "RUNNING",
    "PASS", "FAIL",
    # Discord RPC event names and the HTTP methods (RetroAchievements' requests are POSTs)
    "READY", "ERROR", "GET", "POST",
    # environment variables without a project prefix
    "APPDATA", "PATH", "PYTHON", "USER", "USERNAME", "USERPROFILE",
    # the second half of an environment variable's name: nk_launch.c writes SR_CHEATS_FILE as
    # two adjacent literals, the marker it looks for in a package's executable
    "FILE",
    # disc folders, PARAM.SFO keys, module and folder names, file magic and disc-format names
    "EXTRACTED", "KMODULE", "PPSSPP", "SYSDIR", "TEST", "TITLE", "USRDIR", "CD001", "CISO", "CSO",
    "ELF", "IHDR", "ZISO", "ZSO",
    # PNG chunk names of the UI test's synthetic sample badges (player_ui_test.c)
    "IDAT", "IEND",
}
#: Environment variable names.
PLAYER_NOT_CODE_PREFIXES = ("NK_", "SR_", "XDG_", "NAKAGAWA_", "PLAYER_UI_TEST_")
#: Windows reserved device names (the player refuses them as file names) and disc IDs.
PLAYER_NOT_CODE_PATTERN = re.compile(r"^(?:CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9]|[A-Z]{4}[0-9]{5})$")


class RegistryShapeTests(unittest.TestCase):
    def test_every_code_has_one_known_stage_and_known_places(self) -> None:
        stages = REGISTRY["stages"]
        self.assertEqual(stages, ["inspect", "decrypt", "import", "analyze", "codegen", "compile",
                                  "package", "run", "host"])
        self.assertEqual(set(REGISTRY["report_stages"]) - set(stages), set())
        for code, entry in REGISTRY["codes"].items():
            with self.subTest(code=code):
                self.assertRegex(code, r"^[A-Z][A-Z0-9_]*[A-Z0-9]$")
                self.assertIn(entry["stage"], stages)
                self.assertTrue(entry["in"])
                self.assertLessEqual(set(entry["in"]), set(REGISTRY["vocabularies"]))
        self.assertEqual(set(REGISTRY["retired"]) & set(REGISTRY["codes"]), set(),
                         "a retired code is never reused")

    def test_no_code_is_an_unclassified_bucket(self) -> None:
        for code in REGISTRY["codes"]:
            self.assertNotIn("UNCLASSIFIED", code)


class DocumentTests(unittest.TestCase):
    def _rows(self) -> dict[str, tuple[str, list[str]]]:
        rows: dict[str, tuple[str, list[str]]] = {}
        stage = None
        for line in DOC.splitlines():
            heading = re.match(r"^## (\S+)$", line)
            if heading:
                stage = heading.group(1) if heading.group(1) in REGISTRY["stages"] else None
                continue
            row = re.match(r"^\| `([A-Z][A-Z0-9_]*)` \| ([a-z, ]+) \| (.+) \|$", line)
            if stage and row:
                code, where, meaning = row.groups()
                self.assertNotIn(code, rows, f"{code} is listed twice")
                self.assertTrue(meaning.strip(), code)
                rows[code] = (stage, [place.strip() for place in where.split(",")])
        return rows

    def test_the_document_lists_every_code_with_its_stage_and_places(self) -> None:
        documented = self._rows()
        registered = {code: (entry["stage"], entry["in"])
                      for code, entry in REGISTRY["codes"].items()}
        self.assertEqual(sorted(set(registered) - set(documented)), [], "missing from the doc")
        self.assertEqual(sorted(set(documented) - set(registered)), [], "missing from the JSON")
        for code, row in documented.items():
            self.assertEqual(row, registered[code], code)

    def test_the_document_lists_every_stop_boundary(self) -> None:
        section = DOC.split("## Stop boundaries", 1)[1].split("\n## ", 1)[0]
        documented = dict(re.findall(r"^\| `([a-z0-9<>-]+)` \| (\d+) \|", section, re.M))
        boundaries = REGISTRY["stop_boundaries"]
        expected = {name: str(entry["issue"]) for name, entry in boundaries["names"].items()}
        expected.update({prefix + "<name>": str(entry["issue"])
                         for prefix, entry in boundaries["prefixes"].items()})
        self.assertEqual(documented, expected)


class BringupReportTests(unittest.TestCase):
    def test_the_report_schema_enum_is_the_report_vocabulary(self) -> None:
        schema = json.loads((ROOT / "assets" / "bringup_report.schema.json")
                            .read_text(encoding="utf-8"))
        enum = schema["properties"]["failure_class"]["enum"]
        self.assertEqual(len(enum), len(set(enum)))
        self.assertEqual(set(enum) - {"NONE"}, codes_in("report"))

    def test_every_class_the_bringup_writes_is_a_report_code(self) -> None:
        nk_cli = _tree("tools/nk_cli.py")
        emitted = _assigned(nk_cli, {"failure", "failure_class"}, {"failure_class"},
                            {"fail_stage": 2, "_fail_bringup": 2}, {"failure_class"})
        stage_defaults = _module_dict(nk_cli, "_STAGE_FAILURE_CLASS")
        emitted |= set().union(*(_values(value) for value in stage_defaults.values))
        messages = _module_dict(nk_cli, "_ANALYZER_BOUNDARY_MESSAGES")
        emitted |= set().union(*(_values(key) for key in messages.keys))
        qualification = _tree("tools/title_qualification.py")
        emitted |= _assigned(qualification, {"failure", "failure_class"}, {"failure_class"},
                             {"_stage_fail": 2}, {"failure_class"})
        self.assertGreater(len(emitted), 30, "the census found the emit sites")
        self.assertEqual(sorted(emitted - codes_in("report")), [])

    def test_the_analyze_stage_names_each_route_code_it_can_meet(self) -> None:
        nk_cli = _tree("tools/nk_cli.py")
        messages = _module_dict(nk_cli, "_ANALYZER_BOUNDARY_MESSAGES")
        named = set().union(*(_values(key) for key in messages.keys))
        self.assertIn("ANALYZER_INTERNAL_ERROR", named)
        self.assertLessEqual(named, codes_in("report"))


class RouteTests(unittest.TestCase):
    def _census(self) -> set[str]:
        found: set[str] = set()
        for relative in ROUTE_SOURCES:
            tree = _tree(relative)
            # raise PackageRouteError("CODE", ...), super().__init__("CODE", ...) in a subclass,
            # IsoInspectionError(..., boundary_code="CODE"), ImportTableError(..., code="CODE")
            # and title_codegen_plan's printed prefix for an error with no route code
            found |= _assigned(tree, {"prefix"}, set(), {**ROUTE_EXCEPTIONS, "__init__": 0},
                               {"boundary_code", "code"})
            for node in tree.body:
                # named code constants: CODE_REGIONS_MISMATCH = "...", PBP_SFO_INVALID = "..."
                if isinstance(node, ast.Assign) and len(node.targets) == 1 \
                        and isinstance(node.targets[0], ast.Name):
                    name = node.targets[0].id
                    value = _values(node.value)
                    if value and (name.startswith("CODE_") or name.endswith("_CODE")
                                  or value == {name}):
                        found |= value
        return found

    def test_every_module_that_raises_a_route_exception_is_censused(self) -> None:
        raisers: set[str] = set()
        for path in sorted(TOOLS.rglob("*.py")):
            if path.name.startswith("test_"):
                continue
            for node in ast.walk(_tree(path.relative_to(ROOT).as_posix())):
                if isinstance(node, ast.Call):
                    name = getattr(node.func, "id", None) or getattr(node.func, "attr", None)
                elif isinstance(node, ast.ClassDef):
                    name = next((getattr(base, "id", None) or getattr(base, "attr", None)
                                 for base in node.bases
                                 if (getattr(base, "id", None) or getattr(base, "attr", None))
                                 in ROUTE_EXCEPTIONS), None)
                else:
                    continue
                if name in ROUTE_EXCEPTIONS:
                    raisers.add(path.relative_to(ROOT).as_posix())
        self.assertIn("tools/imports.py", raisers)
        self.assertEqual(sorted(raisers - set(ROUTE_SOURCES)), [],
                         "a module that raises a route exception must be in ROUTE_SOURCES")

    def test_every_route_code_is_registered_and_every_registered_one_is_raised(self) -> None:
        census = self._census()
        self.assertEqual(sorted(census - codes_in("route")), [], "raised but not registered")
        self.assertEqual(sorted(codes_in("route") - census), [], "registered but never raised")


class StagingTests(unittest.TestCase):
    def test_every_staging_code_is_registered(self) -> None:
        census = _assigned(_tree("tools/nk_core/prep_engine.py"), set(), set(), {},
                           {"error_code"})
        self.assertEqual(census, codes_in("staging"))


class DecryptTests(unittest.TestCase):
    def _status_names(self) -> dict[str, str]:
        body = _c_function("src/core/nk_psp_crypto.c", "nk_psp_status_name")
        return dict(re.findall(r'case (NK_PSP_[A-Z_]+): return "([A-Z_]+)";', body))

    def test_every_engine_status_is_a_decrypt_code(self) -> None:
        names = set(self._status_names().values()) - {"VALIDATED_DECRYPTED"}
        body = _c_function("src/core/nk_psp_crypto.c", "nk_psp_status_name")
        names |= set(re.findall(r'default: return "([A-Z_]+)";', body))
        player = _c_function("src/player/player_state.c", "player_decrypt_disc_file")
        names |= set(re.findall(r'return "([A-Z_]+)";', player)) - {"VALIDATED_DECRYPTED"}
        self.assertGreater(len(names), 12)
        self.assertEqual(sorted(names - codes_in("decrypt")), [])

    def test_every_code_the_disc_decryption_records_is_registered(self) -> None:
        # decrypt_disc rows feed library_build's decrypt_step.first_failure_code; a coded
        # exception it raised would reach that record only as INTERNAL_ERROR.
        tree = _tree("tools/nk_core/decrypt_tool.py")
        found = _assigned(tree, set(), {"code"}, {}, {"boundary_code", "code"})
        self.assertIn("DISC_DECRYPT_MEMBER_MISSING", found)
        self.assertEqual(sorted(found - codes_in("decrypt", "report")), [],
                         "recorded but not registered")
        raised = [node for node in ast.walk(tree) if isinstance(node, ast.Raise)
                  and isinstance(node.exc, ast.Call)
                  and getattr(node.exc.func, "attr", None) == "IsoInspectionError"]
        self.assertEqual(raised, [], "a disc refusal is a coded row, not a raise")

    def test_nk_decrypt_exit_codes_match_the_tooling(self) -> None:
        names = self._status_names()
        body = _c_function("src/core/nk_decrypt_main.c", "print_diagnostic")
        exits = {int(code): names[status]
                 for status, code in re.findall(r"case (NK_PSP_[A-Z_]+): exit_code = (\d+);",
                                                body)}
        default = re.search(r"default: rc = (NK_PSP_[A-Z_]+); exit_code = (\d+);", body)
        self.assertIsNotNone(default)
        assert default is not None
        exits[int(default.group(2))] = names.get(default.group(1), "INTERNAL_ERROR")
        self.assertEqual(exits, decrypt_tool.EXIT_STATUS_CODES)
        self.assertLessEqual(set(exits.values()), codes_in("decrypt"))
        self.assertIsNone(decrypt_tool.status_code(0))
        self.assertEqual(decrypt_tool.status_code(2), "INTERNAL_ERROR")


class DiscTests(unittest.TestCase):
    def test_every_disc_refusal_is_a_disc_code(self) -> None:
        header = "".join((ROOT / "src" / "core" / name).read_text(encoding="utf-8")
                         for name in ("nk_disc_expand.h", "nk_disc_verify.h"))
        source = (ROOT / "src" / "core" / "nk_disc_expand.c").read_text(encoding="utf-8")
        found = set(re.findall(r'#define NK_DISC_[A-Z_]+\s+"([A-Z_]+)"', header + source))
        iso = (ROOT / "src" / "core" / "nk_iso.c").read_text(encoding="utf-8")
        found |= set(re.findall(r'"((?:PBP|DISC)_[A-Z_]+)"', iso)) - {"DISC_ID", "DISC_VERSION"}
        self.assertGreater(len(found), 18)
        self.assertEqual(sorted(found - codes_in("disc")), [])


class PlayerTests(unittest.TestCase):
    def test_a_single_word_literal_is_a_census_candidate(self) -> None:
        # The registry allows a code with no underscore (CANCELLED is one), so the census must
        # see one in both directions.
        self.assertEqual(C_CODE.findall('player_app_set_error(app, "TIMEOUT", detail);'),
                         ["TIMEOUT"])
        self.assertIn("CANCELLED", codes_in("player"))

    def test_every_code_the_problem_report_can_show_is_registered(self) -> None:
        literals: set[str] = set()
        for relative in PLAYER_SOURCES:
            literals |= set(C_CODE.findall((ROOT / relative).read_text(encoding="utf-8")))
        candidates = {literal for literal in literals
                      if literal not in PLAYER_NOT_CODES
                      and not literal.startswith(PLAYER_NOT_CODE_PREFIXES)
                      and not PLAYER_NOT_CODE_PATTERN.match(literal)}
        shown = codes_in("player", "disc", "decrypt")
        self.assertEqual(sorted(candidates - shown), [],
                         "a new uppercase literal is a registered code or a listed non-code")
        self.assertEqual(sorted(codes_in("player") - literals), [],
                         "registered but no longer in the player")


class LibraryTests(unittest.TestCase):
    def test_library_records_use_registered_codes(self) -> None:
        found: set[str] = set()
        for relative in ("tools/library_build.py", "tools/library_sweep.py"):
            found |= _assigned(_tree(relative), {"boundary_code", "failure_class"},
                               {"failure_class", "boundary_code", "identify_error",
                                "first_failure_code"}, {}, set())
        sweep = _tree("tools/library_sweep.py")
        for node in sweep.body:
            if isinstance(node, ast.Assign) and any(
                    isinstance(target, ast.Name) and target.id == "_PUBLIC_SWEEP_BLOCKER_CODES"
                    for target in node.targets):
                found |= {element.value for element in ast.walk(node.value)
                          if isinstance(element, ast.Constant) and isinstance(element.value, str)}
        self.assertGreater(len(found), 5)
        self.assertEqual(sorted(found - codes_in("library", "report", "decrypt")), [])


class StopBoundaryTests(unittest.TestCase):
    def test_every_stop_boundary_the_runtime_writes_is_registered(self) -> None:
        body = _c_function("src/rt/recomp.c", "dispatch_semantic_boundary_name")
        names = set(re.findall(r'return "([a-z0-9-]+)";', body))
        recomp = (ROOT / "src" / "rt" / "recomp.c").read_text(encoding="utf-8")
        names |= set(re.findall(r'return "(interpreter-rejection)";', recomp))
        # Boundaries the dispatcher names directly when it records a stop, with their issue.
        recorded = dict(re.findall(
            r'dispatch_record_semantic_stop_issue\(\s*"([a-z0-9-]+)",\s*(\d+)u\)', recomp))
        self.assertGreater(len(recorded), 1)
        names |= set(recorded)
        hle = hle_sources.text()
        names |= set(re.findall(r'#define HLE_LIBFONT_STARTUP_BOUNDARY\s+"([a-z0-9-]+)"', hle))
        prefixes = set(re.findall(r'"(unsupported-import-)"', hle))
        boundaries = REGISTRY["stop_boundaries"]
        self.assertEqual(names, set(boundaries["names"]))
        for name, issue in recorded.items():
            self.assertEqual(boundaries["names"][name]["issue"], int(issue), name)
        self.assertEqual(prefixes, set(boundaries["prefixes"]))


class LiveCodeTests(unittest.TestCase):
    def test_a_lookup_table_row_does_not_count_as_an_emitter(self) -> None:
        # The player's badge table names codes it can show, but a row there emits nothing.
        table = 'static const T k_error_badges[] = {\n    {"OLD_GONE_CODE", "Game stopped"},\n};\n'
        self.assertNotIn("OLD_GONE_CODE", _emitted_literals(Path("player_state.c"), table))
        emit = 'player_app_set_error(app, "OLD_GONE_CODE", "x");\n'
        self.assertIn("OLD_GONE_CODE", _emitted_literals(Path("player_state.c"), emit))

    def test_every_registered_code_still_appears_in_an_emitter(self) -> None:
        quoted: set[str] = set()
        for path in list((ROOT / "tools").rglob("*.py")) + list((ROOT / "src").rglob("*.[ch]")):
            if path.name.startswith("test_") or "selftest" in path.name:
                continue
            quoted |= _emitted_literals(path, path.read_text(encoding="utf-8", errors="replace"))
        self.assertEqual(sorted(set(REGISTRY["codes"]) - quoted), [],
                         "move a code no emitter uses to \"retired\"")


if __name__ == "__main__":
    unittest.main()
