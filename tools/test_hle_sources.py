# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for tools/hle_sources.py, the one list of runtime HLE sources (issue #307).

Synthetic trees are written to a temporary directory for the fail-closed cases; the live
tests pin this checkout: every ``src/rt/hle*.c`` file is listed or named as not runtime,
the readers return every listed file, and the item splitter gives each one back byte for
byte.
"""

from __future__ import annotations

from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hle_manifest  # noqa: E402
import hle_sources  # noqa: E402
from hle_sources import HleSourcesError  # noqa: E402
from makefile_text import read_makefile  # noqa: E402

ROOT = hle_sources.ROOT

SYNTH_C = """\
// header comment
#include "recomp.h"

#define A0 (s->r[4])
#ifndef SCE_X
#define SCE_X 0x80000001u
#endif

typedef struct { int a; } Pair;
enum { MODE_A, MODE_B = 2 };
static int s_counter = 0;
const char *g_names[] = { "a;b", "c{" };

/* a helper */
static uint32_t helper(int x) {
    if (x) { return 1; }
    return 0;
}

#ifdef _WIN32
static void host_sleep(unsigned ms) { Sleep(ms); }
#else
static void host_sleep(unsigned ms) { usleep(ms * 1000u); }
#endif

UTILITY_DIALOG_HANDLERS(h_Msg, 1)

void sr_hle_init(void) {
    sr_hle_register(0x1u, "a", h_a);
}
"""


class SplitItemsTests(unittest.TestCase):
    def test_round_trips_the_text(self) -> None:
        items = hle_sources.split_items(SYNTH_C)
        rebuilt = "".join(item.lead + item.text for item in items)
        self.assertEqual(rebuilt + SYNTH_C[items[-1].end:], SYNTH_C)

    def test_kinds_names_and_conditions(self) -> None:
        items = hle_sources.split_items(SYNTH_C)
        by_name = {(item.kind, name): item for item in items for name in item.names}
        self.assertIn(("macro", "A0"), by_name)
        self.assertEqual(by_name[("macro", "SCE_X")].cond, ("#ifndef SCE_X",))
        self.assertIn(("decl", "Pair"), by_name)
        self.assertIn(("decl", "MODE_B"), by_name)
        self.assertTrue(by_name[("decl", "s_counter")].is_static)
        self.assertFalse(by_name[("decl", "g_names")].is_static)
        helper = by_name[("func", "helper")]
        self.assertTrue(helper.is_static)
        self.assertIn("/* a helper */", helper.lead)
        self.assertTrue(helper.text.endswith("return 0;\n}"))
        arms = [item for item in items if item.kind == "func" and item.names == ["host_sleep"]]
        self.assertEqual([arm.cond for arm in arms], [("#ifdef _WIN32",), ("#ifdef _WIN32 | #else",)])
        self.assertIn(("macro_call", "h_Msg"), by_name)
        self.assertIn(("func", "sr_hle_init"), by_name)

    def test_braces_and_semicolons_in_literals_do_not_split_items(self) -> None:
        items = hle_sources.split_items(SYNTH_C)
        names = [item for item in items if "g_names" in item.names]
        self.assertEqual(len(names), 1)
        self.assertTrue(names[0].text.endswith('"c{" };'))

    def test_static_after_a_leading_attribute_is_static(self) -> None:
        source = "__attribute__((format(printf, 1, 2)))\nstatic int note(const char *f, ...) { return 0; }\n"
        self.assertTrue(hle_sources.split_items(source)[0].is_static)
        self.assertFalse(hle_sources.split_items("__attribute__((cold)) int f(void) { return 0; }\n")[0].is_static)

    def test_object_definitions_exclude_prototypes_and_types(self) -> None:
        source = "static int a;\nint b = 1;\nextern int c;\nint f(void);\ntypedef int T;\nstruct S { int x; };\n"
        kinds = {item.names[0]: hle_sources.is_object_definition(item) for item in hle_sources.split_items(source)}
        self.assertEqual(kinds, {"a": True, "b": True, "c": False, "f": False, "T": False, "S": False})

    def test_every_live_source_round_trips(self) -> None:
        for relative, body in hle_sources.sources(headers=True):
            with self.subTest(path=relative):
                items = hle_sources.split_items(body)
                self.assertEqual("".join(item.lead + item.text for item in items) + body[items[-1].end:], body)


class LiveListTests(unittest.TestCase):
    def test_hle_c_is_first(self) -> None:
        self.assertEqual(hle_sources.RUNTIME_HLE_SOURCES[0], "src/rt/hle.c")

    def test_every_hle_c_file_on_disk_is_listed_or_named_as_not_runtime(self) -> None:
        self.assertEqual(hle_sources.unlisted_hle_files(), [])
        for path in hle_sources.RUNTIME_HLE_SOURCES + hle_sources.NOT_RUNTIME + hle_sources.HLE_PRIVATE_HEADERS:
            with self.subTest(path=path):
                self.assertTrue((ROOT / path).is_file())

    def test_makefile_and_cmake_list_the_same_sources(self) -> None:
        makefile = read_makefile(ROOT)
        self.assertEqual(hle_sources.makefile_hle_srcs(makefile), list(hle_sources.RUNTIME_HLE_SOURCES))
        headers = re.search(r"(?m)^HLE_HDRS\s*:=\s*(.*)$", makefile)
        assert headers is not None
        self.assertEqual(headers.group(1).split(), list(hle_sources.HLE_PRIVATE_HEADERS))
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        runtime = re.search(r"(?ms)^set\(NK_RUNTIME_SOURCES\b(.*?)\)", cmake)
        assert runtime is not None
        listed = runtime.group(1).split()
        hle = [path for path in listed if path in hle_sources.RUNTIME_HLE_SOURCES
               or re.fullmatch(r"src/rt/hle(?:_[a-z0-9_]+)?\.c", path)]
        self.assertEqual(hle, list(hle_sources.RUNTIME_HLE_SOURCES), "CMake lists the HLE sources together, in order")

    def test_manifest_reads_the_same_sources(self) -> None:
        self.assertEqual(
            [path for path, _ in hle_manifest.runtime_hle_sources()],
            list(hle_sources.RUNTIME_HLE_SOURCES),
        )

    def test_text_is_not_vacuous(self) -> None:
        joined = hle_sources.text()
        self.assertEqual(joined.count(hle_sources.INIT_SIGNATURE), 1)
        regs = hle_manifest.extract_registrations(hle_sources.text(headers=False))
        baseline = (ROOT / "tools" / "import_audit_baseline.json").read_text(encoding="utf-8")
        self.assertEqual(len(regs), baseline.count('"classification"'))

    def test_defining_file_follows_the_definition(self) -> None:
        self.assertEqual(hle_sources.defining_file("h_PowerIsLowBattery"), "src/rt/hle_power.c")
        self.assertEqual(hle_sources.defining_file("sr_hle_init"), "src/rt/hle.c")

    def test_ifdef_arms_in_one_file_count_once(self) -> None:
        self.assertEqual(hle_sources.definitions()["sr_host_sleep_ms"], ["src/rt/hle_host.c"])

    def test_every_hle_file_carries_the_licence_of_the_file_it_came_from(self) -> None:
        """Each file split from hle.c keeps hle.c's GPL-2.0-or-later, derived-translated header (#307).

        A new file under src/ gets the project's GPL-3.0-or-later header by default; for code
        derived from sal063 and PPSSPP that would be a licensing error, so the header is checked here.
        """
        licence = "".join((ROOT / "src/rt/hle.c").read_text(encoding="utf-8").splitlines(keepends=True)[:8])
        self.assertIn("SPDX-License-Identifier: GPL-2.0-or-later", licence)
        self.assertIn("Derived from PPSSPP", licence)
        for relative, body in hle_sources.sources(headers=True):
            if relative in hle_sources.OWN_LINEAGE_SOURCES:
                continue  # a new file with its own lineage; checked below
            with self.subTest(path=relative):
                self.assertTrue(body.startswith(licence), relative)

    def test_a_new_hle_file_carries_its_own_lineage_header(self) -> None:
        """A runtime HLE file written new, not split from hle.c, is listed as such and carries an
        SPDX header of its own instead of hle.c's (src/rt/hle_videocodec.c, #286)."""
        for relative in hle_sources.OWN_LINEAGE_SOURCES:
            with self.subTest(path=relative):
                self.assertIn(relative, hle_sources.RUNTIME_HLE_SOURCES)
                head = (ROOT / relative).read_text(encoding="utf-8").splitlines()[:8]
                self.assertTrue(head[0].startswith("// SPDX-License-Identifier: "), relative)
                self.assertNotIn("Derived from PPSSPP", "\n".join(head), relative)

    def test_every_hle_source_includes_the_private_header_first(self) -> None:
        for relative, body in hle_sources.sources():
            with self.subTest(path=relative):
                first = re.search(r'(?m)^\s*#\s*include\s+(\S+)', body)
                assert first is not None
                self.assertEqual(first.group(1), '"hle_internal.h"')


class FailClosedTests(unittest.TestCase):
    def _tree(self, files: dict[str, str]) -> Path:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        for relative, body in files.items():
            (root / relative).parent.mkdir(parents=True, exist_ok=True)
            (root / relative).write_text(body, encoding="utf-8")
        return root

    def _complete(self) -> dict[str, str]:
        files = {path: "int x;\n" for path in hle_sources.RUNTIME_HLE_SOURCES + hle_sources.HLE_PRIVATE_HEADERS}
        files["src/rt/hle.c"] = "void sr_hle_init(void) {\n}\n"
        return files

    def test_complete_synthetic_tree_reads(self) -> None:
        root = self._tree(self._complete())
        self.assertIn("sr_hle_init", hle_sources.text(root))

    def test_missing_file_is_an_error(self) -> None:
        files = self._complete()
        del files[hle_sources.RUNTIME_HLE_SOURCES[-1]]
        with self.assertRaisesRegex(HleSourcesError, "cannot be read"):
            hle_sources.text(self._tree(files))

    def test_empty_file_is_an_error(self) -> None:
        files = self._complete()
        files[hle_sources.RUNTIME_HLE_SOURCES[-1]] = "\n  \n"
        with self.assertRaisesRegex(HleSourcesError, "is empty"):
            hle_sources.text(self._tree(files))

    def test_lost_or_duplicated_init_is_an_error(self) -> None:
        for body in ("int y;\n", "void sr_hle_init(void) {}\nvoid sr_hle_init(void) {}\n"):
            files = self._complete()
            files["src/rt/hle.c"] = body
            with self.subTest(body=body), self.assertRaisesRegex(HleSourcesError, "sr_hle_init"):
                hle_sources.text(self._tree(files))

    def test_defining_file_refuses_zero_or_two_definitions(self) -> None:
        files = self._complete()
        files[hle_sources.RUNTIME_HLE_SOURCES[-1]] = "int twice(void) { return 0; }\n"
        files["src/rt/hle.c"] += "int twice(void) { return 1; }\n"
        root = self._tree(files)
        with self.assertRaisesRegex(HleSourcesError, "2 runtime HLE files"):
            hle_sources.defining_file("twice", root)
        with self.assertRaisesRegex(HleSourcesError, "0 runtime HLE files"):
            hle_sources.defining_file("absent", root)

    def test_function_text_refuses_an_ambiguous_signature(self) -> None:
        files = self._complete()
        files["src/rt/hle.c"] += "static int f1(void) { return 1; }\nstatic int f2(void) { return 2; }\n"
        root = self._tree(files)
        self.assertEqual(hle_sources.function_text("int f2(void)", root), "static int f2(void) { return 2; }")
        with self.assertRaisesRegex(HleSourcesError, "2 runtime HLE function definitions"):
            hle_sources.function_text("static int f", root)

    def test_unlisted_hle_file_is_reported(self) -> None:
        files = self._complete()
        files["src/rt/hle_extra.c"] = "int z;\n"
        files["tests/native/hle_thread_selftest.c"] = "int t;\n"
        self.assertEqual(hle_sources.unlisted_hle_files(self._tree(files)), ["src/rt/hle_extra.c"])


CC = shutil.which("gcc")
NM = shutil.which("nm")
THREAD_HARNESS = ROOT / "tests" / "native" / "hle_thread_selftest.c"
MPEG_PORT = ROOT / "src" / "rt" / "mpeg.c"
_HARNESS_DEFINITION = re.compile(r"^(?!static\b)[A-Za-z_][\w \t*]*?\b(mpeg_\w+)\s*\(", re.MULTILINE)


def _harness_mpeg_definitions(source: str) -> set[str]:
    """The ``mpeg_*`` functions a C file defines (a prototype ends in ``;`` before any ``{``)."""
    names = set()
    for match in _HARNESS_DEFINITION.finditer(source):
        rest = source[match.end():]
        brace, semicolon = rest.find("{"), rest.find(";")
        if brace != -1 and (semicolon == -1 or brace < semicolon):
            names.add(match.group(1))
    return names


class ThreadHarnessMpegLinkTests(unittest.TestCase):
    """The executable HLE harness links no ``src/rt/mpeg.c`` (#317).

    hle-thread-selftest-build compiles the sceMpeg handlers with ``SR_HLE_THREAD_SELFTEST`` and
    supplies its own ``mpeg_init``/``mpeg_finish``/``mpeg_create``/``mpeg_delete``. Any other
    reference into the port is an undefined symbol: GNU ld with ``--gc-sections`` drops it when
    nothing reaches it, lld refuses the link. This compiles the handler file as the harness does
    and requires every port function it refers to to be one the harness defines.

    It covers the mpeg half of the link only. The harness's two interpreter dispatch entries
    (``dispatch_call_interp``, ``dispatch_call_continue``, which guest_interp.c refers to while
    recomp.c is not linked) and any new reference of that kind are caught by an lld build of
    hle-thread-selftest-build, not here. The handler file is compiled with ``gcc -std=c11`` and
    the include path, not the build's full flags, and a harness definition is counted whatever
    ``#if`` surrounds it; a guarded definition would need both to be tightened.
    """

    def _symbols(self, source: Path, out: Path, *flags: str, defined: bool) -> set[str]:
        includes = re.search(r"^HLE_INCLUDES\s*:=(.*)$", read_makefile(ROOT), re.MULTILINE)
        self.assertIsNotNone(includes, "HLE_INCLUDES is not assigned in the Makefile")
        assert includes is not None and CC is not None and NM is not None
        compiled = subprocess.run(
            [CC, "-std=c11", "-Isrc/rt", "-Isrc/core", *includes.group(1).split(), "-ffunction-sections",
             "-fdata-sections", *flags, "-c", str(source), "-o", str(out)],
            cwd=ROOT, capture_output=True, text=True, check=False)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        listed = subprocess.run([NM, "-g", "--defined-only" if defined else "-u", str(out)],
                                capture_output=True, text=True, check=True)
        return {line.split()[-1] for line in listed.stdout.splitlines() if line.strip()}

    def test_the_harness_defines_every_port_function_its_handlers_call(self) -> None:
        if CC is None or NM is None:
            self.skipTest("NOT_RUN: needs gcc and nm on PATH")
        handlers = ROOT / hle_sources.defining_file("hle_register_psmf_player_handlers")
        with tempfile.TemporaryDirectory() as tmp:
            port = {name for name in self._symbols(MPEG_PORT, Path(tmp) / "mpeg.o", defined=True)
                    if name.startswith("mpeg_")}
            called = self._symbols(handlers, Path(tmp) / "handlers.o", "-DSR_HLE_THREAD_SELFTEST",
                                   defined=False) & port
        supplied = _harness_mpeg_definitions(THREAD_HARNESS.read_text(encoding="utf-8"))
        # The interrupt-context check of sceMpegCreate goes through these; a pass on none proves nothing.
        self.assertTrue({"mpeg_create", "mpeg_delete"} <= called & supplied, sorted(called))
        self.assertEqual(sorted(called - supplied), [],
                         "port functions the harness neither links nor defines")

    def test_definitions_are_told_from_prototypes(self) -> None:
        source = ("uint32_t mpeg_a(void);\nuint32_t mpeg_b(uint32_t x,\n    uint32_t y) {\n  return 0;\n}\n"
                  "static uint32_t mpeg_c(void) { return 0; }\n")
        self.assertEqual(_harness_mpeg_definitions(source), {"mpeg_b"})


class MakefileTests(unittest.TestCase):
    def test_continuation_lines_are_read(self) -> None:
        text = "X := 1\nHLE_SRCS := src/rt/hle.c \\\n            src/rt/hle_io.c\nY := 2\n"
        self.assertEqual(hle_sources.makefile_hle_srcs(text), ["src/rt/hle.c", "src/rt/hle_io.c"])

    def test_missing_assignment_is_an_error(self) -> None:
        with self.assertRaises(HleSourcesError):
            hle_sources.makefile_hle_srcs("RT_SRCS := src/rt/hle.c\n")


class PathConstantGuardTests(unittest.TestCase):
    """The detector the no-other-reader check runs on every tool."""

    def test_path_built_from_parts_is_found(self) -> None:
        source = 'from pathlib import Path\nROOT = Path(".")\nHLE = ROOT / "src" / "rt" / "hle.c"\n'
        self.assertEqual(hle_sources.hle_source_path_constants(source), [(3, "hle.c")])

    def test_subsystem_file_path_is_found(self) -> None:
        source = 'TEXT = open("src/rt/hle_io.c").read()\n'
        self.assertEqual(hle_sources.hle_source_path_constants(source), [(1, "src/rt/hle_io.c")])

    def test_regex_glob_and_fstring_paths_are_found(self) -> None:
        source = (
            'import re\n'
            'A = re.compile(r"src/rt/hle\\.c")\n'
            'B = sorted(ROOT.glob("src/rt/hle*.c"))\n'
            'C = f"src/rt/hle_{name}.c"\n'
            'D = f"{prefix}/rt/x.c"\n'
        )
        self.assertEqual(
            hle_sources.hle_source_path_constants(source),
            [(2, "src/rt/hle\\.c"), (3, "src/rt/hle*.c"), (4, "src/rt/hle_*.c")],
        )

    def test_prose_docstrings_and_non_runtime_files_are_not_found(self) -> None:
        source = (
            '"""Reads src/rt/hle.c."""\n'
            'def f():\n    """hle.c"""\n    return "h_X not found in src/rt/hle.c"\n'
            'SELFTEST = "tests/native/hle_thread_selftest.c"\n'
        )
        self.assertEqual(hle_sources.hle_source_path_constants(source), [])


class NoOtherReaderTests(unittest.TestCase):
    """No tool or fixture names a runtime HLE source except as data (#307, I10).

    A tool that reads ``src/rt/hle.c`` by path sees only the part of HLE left in that
    file after the split, and passes on nothing. Every reader goes through
    tools/hle_sources.py; the files below name a source only as data, never to read it.
    """

    #: Files that name a runtime HLE source as data, each with the reason.
    PATH_DATA = {
        "tools/hle_sources.py": "the list itself",
        "tools/hle_move_proof.py": "the base revision's HLE file names, for git show",
        "tools/provenance_record_gap.py": "KNOWN_DISPOSITION_GAPS records hle_power.c by path (#359)",
        "tools/purity_gate.py": "allowlist entries keyed by the file that holds each literal",
        "tools/test_build_graph_snapshot.py": "a synthetic dependency file",
        "tools/test_ci_paths.py": "changed paths handed to the CI router",
        "tools/test_contrib_check.py": "changed paths handed to contrib_check",
        "tools/test_hle_manifest.py": "synthetic sources and the owner_file values the manifest reports",
        "tools/test_hle_move_proof.py": "a synthetic split",
        "tools/test_hle_sources.py": "synthetic trees and this guard's own cases",
        "tools/test_import_stub_census.py": "a synthetic tree",
        "tools/test_package_cache.py": "a synthetic compile command",
        "tools/test_portability_inventory.py": "paths handed to the classifier",
        "tools/test_progress_evidence.py": "a synthetic tree",
        "tools/test_provenance_record_gap.py": "paths handed to the record-gap check",
        "tools/test_purity_gate.py": "a synthetic tree",
    }

    def test_no_tool_names_an_hle_source(self) -> None:
        found: dict[str, list[tuple[int, str]]] = {}
        for path in sorted([*(ROOT / "tools").rglob("*.py"), *(ROOT / "fixtures").rglob("*.py")]):
            relative = path.relative_to(ROOT).as_posix()
            hits = hle_sources.hle_source_path_constants(path.read_text(encoding="utf-8"), relative)
            if hits:
                found[relative] = hits
        unexpected = {relative: hits for relative, hits in found.items() if relative not in self.PATH_DATA}
        self.assertEqual(unexpected, {}, "read the runtime HLE sources through tools/hle_sources.py")
        self.assertEqual(sorted(set(self.PATH_DATA) - set(found)), [], "allowlist entries that name nothing")


if __name__ == "__main__":
    unittest.main()
