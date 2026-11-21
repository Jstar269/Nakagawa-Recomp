# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""`make contrib-check` must route, and must never turn a skip into a pass.

The local fast path is only worth having if it selects the same gates the
hosted workflow would, and only if a gate that could not run is reported as
such.  These tests pin both properties, plus the one place the script
interprets an audit result: the findings only a maintainer can clear, by
regenerating the controls from the private trusted ledger.
"""

from __future__ import annotations

import ast
from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

import contrib_check  # noqa: E402
from makefile_text import read_makefile  # noqa: E402

MAINTAINER_LINE = "PROVENANCE_CONTENT_MISMATCH: tools/thing.py: audited bytes differ"
CONTRIBUTOR_LINE = "POLICY_UNCLASSIFIED: tools/new_thing.py: no explicit disposition"
SUMMARY_LINE = "publication audit: FAIL (2 findings across 748 tracked files, worktree content)"


class _Completed:
    def __init__(self, returncode: int, stdout: str = "", stderr: str = "") -> None:
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


class TestRouting(unittest.TestCase):
    def test_a_shared_tool_pulls_in_its_shared_test_modules(self) -> None:
        modules = contrib_check.python_test_modules(["tools/public_export.py"])
        self.assertIn("tools.test_public_export", modules)
        self.assertIn("tools.test_publish_audit", modules)

    def test_a_changed_test_module_runs_itself(self) -> None:
        self.assertEqual(
            contrib_check.python_test_modules(["tools/test_sched_invariants.py"]),
            ["tools.test_sched_invariants"],
        )

    def test_a_tool_with_a_companion_test_runs_that_test(self) -> None:
        modules = contrib_check.python_test_modules(["tools/policy_sync.py"])
        self.assertIn("tools.test_publication_policy_gate", modules)

    def test_a_deleted_file_is_not_handed_to_a_gate_that_opens_it(self) -> None:
        """Failing-before: a file the branch deleted went to ruff (E902), unittest (an import error) and
        markdownlint, so a valid deletion failed contrib-check."""
        deleted = ["docs/no_such_page.md", "tools/no_such_tool.py", "tools/test_no_such_tool.py"]
        gate = contrib_check.Gate("x", contrib_check.NOT_RUN)
        with mock.patch.object(contrib_check, "changed_paths", return_value=deleted), \
                mock.patch.object(contrib_check, "ruff_gate", return_value=gate) as ruff, \
                mock.patch.object(contrib_check, "unit_gate", return_value=gate) as unit, \
                mock.patch.object(contrib_check, "markdown_gate", return_value=gate) as markdown, \
                mock.patch.object(contrib_check, "c_gate", return_value=gate), \
                mock.patch.object(contrib_check, "docs_gate", return_value=gate) as docs, \
                mock.patch.object(contrib_check, "purity_gate", return_value=gate), \
                mock.patch.object(contrib_check, "publication_gate", return_value=(gate, gate)), \
                mock.patch("sys.stdout"):
            contrib_check.run("origin/main")
        ruff.assert_called_once_with([])
        unit.assert_called_once_with([])
        markdown.assert_called_once_with([])
        # The deletion still routes: the documentation check runs over what remains.
        docs.assert_called_once_with(deleted)

    def test_a_module_whose_test_has_another_name_selects_that_test(self) -> None:
        """Failing-before: routing went by basename only, so these selected no test module."""
        for path, module in (
            ("tools/nk_core/disc_expand_tool.py", "tools.test_disc_expand"),
            ("tests/fixtures/cosim/mutate.py", "tools.test_cosim_mutation_gate_outcome"),
        ):
            with self.subTest(path=path):
                self.assertTrue((ROOT / path).is_file())
                self.assertTrue((ROOT / (module.replace(".", "/") + ".py")).is_file())
                self.assertIn(module, contrib_check.python_test_modules([path]))

    def test_an_unknown_base_is_the_named_stop_not_a_traceback(self) -> None:
        """Failing-before: `git merge-base` exiting non-zero raised CalledProcessError."""
        with self.assertRaises(SystemExit) as raised:
            contrib_check.changed_paths("refs/heads/contrib-check-no-such-base")
        self.assertIn("cannot resolve a merge base with refs/heads/contrib-check-no-such-base",
                      str(raised.exception))

    def test_a_base_with_no_common_ancestor_is_the_named_stop(self) -> None:
        with mock.patch.object(contrib_check, "_git", return_value=_Completed(1)):
            with self.assertRaises(SystemExit) as raised:
                contrib_check.changed_paths("unrelated")
        self.assertIn("no common ancestor", str(raised.exception))

    def test_a_tools_data_file_selects_the_test_modules_that_name_it(self) -> None:
        """Failing-before: hosted CI routes a changed tools/*.json, *.toml or tools/README.md to
        the Python job (ci_paths._is_python_tool), but python_test_modules mapped it to nothing,
        so python-unittest was NOT_RUN and contrib-check ended OK (audit F-8a1d1877)."""
        modules = contrib_check.python_test_modules(["tools/import_audit_baseline.json"])
        self.assertIn("tools.test_hle_manifest", modules)   # names the path in a string
        self.assertIn("tools.test_hle_sources", modules)    # ROOT / "tools" / "import_audit_baseline.json"
        self.assertIn("tools.test_regen_all", modules)
        self.assertIn("tools.test_tree_lint", contrib_check.python_test_modules(["tools/tree_lint_allow.json"]))
        # Path(__file__).resolve().parent / "README.md" is tools/README.md.
        self.assertIn("tools.test_lint_docs", contrib_check.python_test_modules(["tools/README.md"]))

    def test_synthetic_root_selects_the_readers_of_a_tools_data_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            fake_root = Path(tmp)
            (fake_root / "tools").mkdir(parents=True)
            for name in ("demo.json", "demo.toml", "README.md", "unread.json"):
                (fake_root / "tools" / name).write_text("{}\n", encoding="utf-8")
            (fake_root / "tools" / "test_demo_reader.py").write_text(
                'from pathlib import Path\n'
                'ROOT = Path(__file__).resolve().parent.parent\n'
                'DATA = ROOT / "tools" / "demo.json"\n'
                'CONFIG = "tools/demo.toml"\n'
                'INDEX = Path(__file__).resolve().parent / "README.md"\n',
                encoding="utf-8",
            )
            # Names tools/demo.json only in its docstring and a comment, and other README files.
            (fake_root / "tools" / "test_demo_docs.py").write_text(
                '"""Mentions tools/demo.json in prose only."""\n'
                'from pathlib import Path\n'
                'ROOT = Path(__file__).resolve().parent.parent\n'
                'DOC = ROOT / "docs" / "README.md"\n'
                'NEAR = Path(__file__).resolve().parent.parent / "README.md"\n'
                '# tools/demo.toml\n',
                encoding="utf-8",
            )
            with mock.patch.object(contrib_check, "ROOT", fake_root):
                for path in ("tools/demo.json", "tools/demo.toml", "tools/README.md"):
                    with self.subTest(path=path):
                        self.assertEqual(contrib_check.python_test_modules([path]), ["tools.test_demo_reader"])
                self.assertEqual(contrib_check.python_test_modules(["tools/unread.json"]), [])
                with mock.patch.dict(
                    contrib_check.TOOLS_DATA_TEST_EXCLUSIONS,
                    {"tools.test_demo_reader": "synthetic exclusion"},
                ):
                    self.assertEqual(contrib_check.python_test_modules(["tools/demo.json"]), [])

    def test_documentation_and_build_files_select_no_python_module(self) -> None:
        self.assertEqual(
            contrib_check.python_test_modules(["CONTRIBUTING.md", "Makefile", "mk/build_common.mk"]),
            [],
        )

    def test_routing_uses_the_hosted_classifier(self) -> None:
        """The local selection must come from tools/ci_paths, not a second copy."""
        source = (ROOT / "tools" / "contrib_check.py").read_text(encoding="utf-8")
        self.assertIn("ci_paths.classify", source)
        self.assertNotIn("def _is_docs", source)
        self.assertNotIn("def _is_native", source)


def _derive_source_test_pairs(root: Path) -> dict[str, set[str]]:
    """Independently derive ``(source_path -> {tools.test_*})`` pairs from test sources.

    Scans every ``tools/test_*.py`` AST for literal paths that exist under
    ``src/`` or ``tests/native/`` (string literals, ``Path`` ``/`` joins with
    directory-variable resolution, ``Path``/``os.path.join``/``joinpath`` calls,
    and inline ``#include "<unit>.c/.inc"`` directives).
    """
    existing: set[str] = set()
    by_basename: dict[str, list[str]] = {}
    for base in ("src", "tests/native"):
        base_dir = root / base
        if not base_dir.is_dir():
            continue
        for dirpath, _dirnames, filenames in os.walk(base_dir):
            rel_dir = Path(dirpath).relative_to(root).as_posix()
            for filename in filenames:
                rel_path = f"{rel_dir}/{filename}"
                existing.add(rel_path)
                by_basename.setdefault(filename, []).append(rel_path)

    include_re = re.compile(r'#\s*include\s*"([^"]+)"')

    def norm(raw: str) -> str:
        s = raw.replace("\\", "/").strip("/")
        while s.startswith("./"):
            s = s[2:]
        return s

    def anchor(joined: str) -> str | None:
        for prefix in ("src", "tests/native"):
            if joined == prefix:
                return prefix
            marker = prefix + "/"
            if joined.startswith(marker):
                return joined
            pos = joined.find("/" + marker)
            if pos != -1:
                return joined[pos + 1:]
        return None

    def div_path(node: ast.BinOp, env: dict[str, str]) -> str | None:
        parts: list[str] = []
        cur: ast.AST = node
        while isinstance(cur, ast.BinOp) and isinstance(cur.op, ast.Div):
            if isinstance(cur.right, ast.Constant) and isinstance(cur.right.value, str):
                parts.append(cur.right.value)
            else:
                return None
            cur = cur.left
        if isinstance(cur, ast.Constant) and isinstance(cur.value, str):
            parts.append(cur.value)
        elif isinstance(cur, ast.Name) and cur.id in env:
            parts.append(env[cur.id])
        elif isinstance(cur, ast.Attribute) and cur.attr in env:
            parts.append(env[cur.attr])
        parts.reverse()
        return anchor("/".join(norm(p) for p in parts if p))

    def call_path(node: ast.Call, env: dict[str, str]) -> str | None:
        fn = node.func
        base: str | None = None
        if isinstance(fn, ast.Attribute) and fn.attr == "joinpath":
            if isinstance(fn.value, ast.Name) and fn.value.id in env:
                base = env[fn.value.id]
            elif isinstance(fn.value, ast.BinOp) and isinstance(fn.value.op, ast.Div):
                base = div_path(fn.value, env)
        elif isinstance(fn, ast.Attribute) and fn.attr == "join":
            pass
        elif isinstance(fn, ast.Name) and fn.id in {"Path", "PurePosixPath", "PurePath"}:
            pass
        else:
            return None
        parts: list[str] = [base] if base else []
        for idx, arg in enumerate(node.args):
            if isinstance(arg, ast.Constant) and isinstance(arg.value, str):
                parts.append(arg.value)
            elif isinstance(arg, ast.Name) and arg.id in env:
                parts.append(env[arg.id])
            elif idx == 0 and base is None:
                continue
            else:
                return None
        if not parts:
            return None
        return anchor("/".join(norm(p) for p in parts if p))

    pairs: dict[str, set[str]] = {}
    for test_file in sorted((root / "tools").glob("test_*.py")):
        if not test_file.is_file():
            continue
        module = f"tools.{test_file.stem}"
        text = test_file.read_text(encoding="utf-8")
        if "src" not in text and "tests" not in text and "#include" not in text:
            continue
        tree = ast.parse(text, filename=str(test_file))
        docs: set[int] = set()
        for node in ast.walk(tree):
            if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
                if node.body and isinstance(node.body[0], ast.Expr):
                    val = node.body[0].value
                    if isinstance(val, ast.Constant) and isinstance(val.value, str):
                        docs.add(id(val))
        env: dict[str, str] = {}
        assigns = [n for n in ast.walk(tree) if isinstance(n, ast.Assign) and len(n.targets) == 1]
        for _ in range(2):
            for assign in assigns:
                target = assign.targets[0]
                name = (
                    target.id if isinstance(target, ast.Name)
                    else target.attr if isinstance(target, ast.Attribute)
                    else None
                )
                if not name:
                    continue
                resolved: str | None = None
                if isinstance(assign.value, ast.BinOp) and isinstance(assign.value.op, ast.Div):
                    resolved = div_path(assign.value, env)
                elif isinstance(assign.value, ast.Call):
                    resolved = call_path(assign.value, env)
                elif isinstance(assign.value, ast.Constant) and isinstance(assign.value.value, str):
                    cand = norm(assign.value.value)
                    if cand in {"src", "tests/native"} or cand.startswith(("src/", "tests/native/")):
                        resolved = cand
                if resolved:
                    env[name] = resolved
        for node in ast.walk(tree):
            if isinstance(node, ast.Constant) and isinstance(node.value, str):
                if id(node) in docs:
                    continue
                literal = norm(node.value)
                if literal in existing:
                    pairs.setdefault(literal, set()).add(module)
                if "#include" in node.value:
                    for match in include_re.finditer(node.value):
                        inc = match.group(1).replace("\\", "/")
                        if inc.endswith((".c", ".inc")):
                            matches = by_basename.get(inc, [])
                            if len(matches) == 1:
                                pairs.setdefault(matches[0], set()).add(module)
            elif isinstance(node, ast.BinOp) and isinstance(node.op, ast.Div):
                div_resolved = div_path(node, env)
                if div_resolved and div_resolved in existing:
                    pairs.setdefault(div_resolved, set()).add(module)
            elif isinstance(node, ast.Call):
                call_resolved = call_path(node, env)
                if call_resolved and call_resolved in existing:
                    pairs.setdefault(call_resolved, set()).add(module)
    return pairs


class TestSourceFileTestSelection(unittest.TestCase):
    """Changing a ``tests/native/`` or ``src/`` file must select the Python tests that exercise it."""

    derived_pairs: dict[str, set[str]]

    @classmethod
    def setUpClass(cls) -> None:
        cls.derived_pairs = _derive_source_test_pairs(ROOT)

    def test_dispatch_isolation_selftest_selects_dispatch_call_boundary(self) -> None:
        modules = contrib_check.python_test_modules(["tests/native/dispatch_isolation_selftest.c"])
        self.assertIn("tools.test_dispatch_call_boundary", modules)
        self.assertIn("tools.test_dispatch_c", modules)
        self.assertIn("tools.test_analyzer_span_scope", modules)
        self.assertIn("tools.test_title_runtime_config", modules)
        self.assertIn("tools.test_trace_window_probe", modules)

    def test_every_literal_source_path_in_test_modules_is_selected_or_excluded(self) -> None:
        self.assertTrue(self.derived_pairs, "expected literal src/ or tests/native/ references in tools/test_*.py")
        missed: list[tuple[str, str]] = []
        leaked_excluded: list[tuple[str, str]] = []
        for source_path, expected_modules in sorted(self.derived_pairs.items()):
            selected = set(contrib_check.python_test_modules([source_path]))
            for module in sorted(expected_modules):
                if module in contrib_check.SOURCE_TEST_EXCLUSIONS:
                    if module in selected:
                        leaked_excluded.append((source_path, module))
                elif module not in selected:
                    missed.append((source_path, module))
        self.assertEqual(
            missed,
            [],
            f"contrib_check.python_test_modules missed {len(missed)} (source, test) pair(s): {missed[:10]}",
        )
        self.assertEqual(
            leaked_excluded,
            [],
            f"excluded test modules were still selected by source changes: {leaked_excluded[:10]}",
        )

    def test_source_test_exclusions_are_valid_documented_and_non_stale(self) -> None:
        referenced_modules = {mod for mods in self.derived_pairs.values() for mod in mods}
        for module, reason in contrib_check.SOURCE_TEST_EXCLUSIONS.items():
            with self.subTest(module=module):
                self.assertTrue(module.startswith("tools.test_"), f"{module} is not a tools.test_* name")
                rel = f"tools/{module.removeprefix('tools.')}.py"
                self.assertTrue((ROOT / rel).is_file(), f"{module} does not exist at {rel}")
                self.assertTrue(reason.strip(), f"{module} exclusion has an empty reason")
                self.assertIn(
                    module,
                    referenced_modules,
                    f"{module} is in SOURCE_TEST_EXCLUSIONS but no longer references any src/ or tests/native/ path",
                )
                # Direct edits to an excluded test module itself must still run that module.
                self.assertIn(module, contrib_check.python_test_modules([rel]))

    def test_selection_stays_focused_on_affected_modules(self) -> None:
        all_test_modules = list((ROOT / "tools").glob("test_*.py"))
        selected = contrib_check.python_test_modules(["tests/native/dispatch_isolation_selftest.c"])
        self.assertGreater(len(selected), 0)
        self.assertLess(len(selected), len(all_test_modules) // 10)
        self.assertEqual(
            contrib_check.python_test_modules(["src/rt/nonexistent_unreferenced_file.c"]),
            [],
        )

    def test_synthetic_root_selects_and_excludes_literal_source_paths(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            fake_root = Path(tmp)
            (fake_root / "src" / "rt").mkdir(parents=True)
            (fake_root / "tests" / "native").mkdir(parents=True)
            (fake_root / "tools").mkdir(parents=True)
            (fake_root / "src" / "rt" / "demo_unit.c").write_text("int demo(void) { return 0; }\n", encoding="utf-8")
            (fake_root / "tests" / "native" / "demo_selftest.c").write_text("int main(void) { return 0; }\n", encoding="utf-8")
            (fake_root / "tools" / "test_demo_unit.py").write_text(
                'from pathlib import Path\n'
                'ROOT = Path(__file__).resolve().parent.parent\n'
                'UNIT = ROOT / "src" / "rt" / "demo_unit.c"\n'
                'SELFTEST = "tests/native/demo_selftest.c"\n',
                encoding="utf-8",
            )
            with mock.patch.object(contrib_check, "ROOT", fake_root):
                self.assertEqual(
                    contrib_check.python_test_modules(["src/rt/demo_unit.c"]),
                    ["tools.test_demo_unit"],
                )
                self.assertEqual(
                    contrib_check.python_test_modules(["tests/native/demo_selftest.c"]),
                    ["tools.test_demo_unit"],
                )
                with mock.patch.dict(
                    contrib_check.SOURCE_TEST_EXCLUSIONS,
                    {"tools.test_demo_unit": "synthetic exclusion"},
                ):
                    self.assertEqual(
                        contrib_check.python_test_modules(["src/rt/demo_unit.c"]),
                        [],
                    )
                    self.assertEqual(
                        contrib_check.python_test_modules(["tools/test_demo_unit.py"]),
                        ["tools.test_demo_unit"],
                    )


class TestGateStatusVocabulary(unittest.TestCase):
    def test_a_header_only_native_change_names_the_hosted_build(self) -> None:
        """The hosted classifier routes a header to native_runtime; the local NOT_RUN says so."""
        gate = contrib_check.c_gate(["src/core/nk_example.h"])
        self.assertEqual(gate.status, contrib_check.NOT_RUN)
        self.assertIn("native_runtime", gate.detail)
        self.assertIn("hosted CI", gate.detail)

    def test_a_changed_header_is_named_unchecked_and_ok_says_so(self) -> None:
        """Failing-before: a header-only change printed a bare NOT_RUN and `contrib-check: OK`."""
        header = "src/rt/recomp.h"
        self.assertTrue((ROOT / header).is_file())
        gate = contrib_check.c_gate([header])
        self.assertEqual(gate.status, contrib_check.NOT_RUN)
        self.assertTrue(gate.unchecked)
        self.assertIn(f"changed header(s) not compiled: {header}", gate.detail)
        out = io.StringIO()
        with mock.patch.object(contrib_check, "run", return_value=[gate]), redirect_stdout(out):
            self.assertEqual(contrib_check.main([]), 0)
        self.assertIn("NOT CHECKED here, although the change touches it: strict-c-compile", out.getvalue())
        # A surface the change does not touch is NOT_RUN without that line.
        out = io.StringIO()
        with mock.patch.object(contrib_check, "run", return_value=[contrib_check.c_gate(["docs/a.md"])]), \
                redirect_stdout(out):
            contrib_check.main([])
        self.assertNotIn("NOT CHECKED", out.getvalue())

    def test_an_untouched_surface_is_not_run_not_pass(self) -> None:
        self.assertEqual(contrib_check.c_gate(["docs/a.md"]).status, contrib_check.NOT_RUN)
        self.assertEqual(contrib_check.markdown_gate(["tools/a.py"]).status, contrib_check.NOT_RUN)
        self.assertEqual(contrib_check.ruff_gate([]).status, contrib_check.NOT_RUN)
        self.assertEqual(contrib_check.purity_gate(["docs/a.md"]).status, contrib_check.NOT_RUN)

    def test_mdx_is_markdown_for_both_markdown_gates(self) -> None:
        """Failing-before: both gates matched only .md and .markdown, while the hosted classifier
        (ci_paths._is_markdown) also counts .mdx, so a changed .mdx page was NOT_RUN locally."""
        page = "docs/page.mdx"
        self.assertTrue(contrib_check.ci_paths._is_markdown(page))
        with mock.patch.object(contrib_check, "find_tool", return_value=None):
            self.assertEqual(contrib_check.markdown_gate([page]).status, contrib_check.SKIP)
        passed = contrib_check.Gate("documentation-freshness", contrib_check.PASS)
        with mock.patch.object(contrib_check, "_run", return_value=passed) as run:
            self.assertEqual(contrib_check.docs_gate([page]).status, contrib_check.PASS)
        run.assert_called_once()

    def test_a_runtime_change_runs_the_core_purity_gate(self) -> None:
        gate = contrib_check.purity_gate(["src/rt/hle.c"])
        self.assertEqual(gate.name, "core-purity")
        self.assertEqual(gate.status, contrib_check.PASS, gate.detail)

    def test_a_missing_tool_is_skipped_rather_than_passed(self) -> None:
        with mock.patch.object(contrib_check.shutil, "which", return_value=None):
            gate = contrib_check.markdown_gate(["docs/a.md"])
        self.assertEqual(gate.status, contrib_check.SKIP)

    def test_a_failing_command_fails_with_its_own_output(self) -> None:
        with mock.patch.object(
            contrib_check.subprocess, "run",
            return_value=_Completed(1, "", "boom: something is wrong"),
        ):
            gate = contrib_check.ruff_gate(["tools/a.py"])
        self.assertEqual(gate.status, contrib_check.FAIL)
        self.assertIn("boom", gate.detail)

    def test_a_timeout_is_a_failure_not_a_skip(self) -> None:
        with mock.patch.object(
            contrib_check.subprocess, "run",
            side_effect=subprocess.TimeoutExpired(cmd="ruff", timeout=1),
        ):
            gate = contrib_check.ruff_gate(["tools/a.py"])
        self.assertEqual(gate.status, contrib_check.FAIL)
        self.assertIn("timed out", gate.detail)


class TestStrictCompileIncludePath(unittest.TestCase):
    """A `tests/native` unit may include a `src/player` header by bare name.

    Those units sit outside `src/`, so the include resolves only when the
    strict-compile line carries `-Isrc/player`.  The flag was missing, so five
    tracked test units the change never touched failed the gate with a bare
    `fatal error: <player header>: No such file or directory`.
    """

    def _player_headers(self) -> set[str]:
        return {path.name for path in (ROOT / "src" / "player").glob("*.h")}

    def _unit_player_includes(self, relative: str) -> list[str]:
        """The `src/player` headers a `tests/native` unit includes by bare name."""
        source = (ROOT / relative).read_text(encoding="utf-8", errors="replace")
        names = {
            Path(name).name
            for name in re.findall(r'#\s*include\s+"([^"]+)"', source)
        }
        return sorted(names & self._player_headers())

    def _player_including_tests(self) -> list[str]:
        found: list[str] = []
        for path in sorted((ROOT / "tests" / "native").glob("*.c")):
            relative = path.relative_to(ROOT).as_posix()
            if self._unit_player_includes(relative):
                found.append(relative)
        return found

    def _needs_installed_sdk(self, relative: str) -> bool:
        """Does one of the unit's player headers pull a header outside the project?

        A slash inside an angle include means an SDK header (`<SDL3/SDL.h>`), and
        standard C headers never carry one.  `ui_clip.h` and `player_host.h`
        pull SDL3, which the native and UI gates build and the Python-only shard
        that runs this file does not install.
        """
        for name in self._unit_player_includes(relative):
            header = (ROOT / "src" / "player" / name).read_text(encoding="utf-8")
            if re.search(r'#\s*include\s+<[^>]*/[^>]*>', header):
                return True
        return False

    def test_the_player_include_path_is_on_the_strict_compile_line(self) -> None:
        self.assertIn("-Isrc/player", contrib_check.STRICT_C_FLAGS)

    def test_every_unit_including_a_player_header_compiles(self) -> None:
        files = self._player_including_tests()
        self.assertTrue(
            files,
            "no tests/native unit includes a src/player header any more; "
            "retire this regression together with the last such unit",
        )
        for relative in files:
            with self.subTest(unit=relative):
                gate = contrib_check.c_gate([relative])
                if gate.status == contrib_check.SKIP:
                    self.skipTest(gate.detail)
                if gate.status == contrib_check.PASS:
                    continue
                # This regression owns the include path, not every dependency of
                # every unit.  A unit whose player header pulls an SDK header can
                # only reach a full compile where that SDK is installed, so on a
                # runner that installs none the SDK header may be what is
                # missing - a *player* header may not, which is the claim here.
                self.assertTrue(self._needs_installed_sdk(relative), gate.detail)
                for name in self._unit_player_includes(relative):
                    self.assertNotIn(f"{name}: No such file or directory",
                                     gate.detail)


def _makefile_text() -> str:
    """The Makefile and the fragments it includes, continuation lines joined."""
    parts = [read_makefile(ROOT)]
    parts.extend(path.read_text(encoding="utf-8") for path in sorted((ROOT / "mk").glob("*.mk")))
    return re.sub(r"\\\n[ \t]*", " ", "\n".join(parts))


def _assignments(text: str) -> dict[str, list[str]]:
    """Every value assigned to each variable, across all conditional branches.

    The drift checks ask whether a flag is on a recipe at all, so the union of
    the alternatives is the right reading: a flag present in any branch counts.
    """
    found: dict[str, list[str]] = {}
    pattern = re.compile(r"^(?:override\s+)?([A-Za-z0-9_.-]+)\s*(?::=|\?=|\+=|=)\s*(.*)$", re.M)
    for name, value in pattern.findall(text):
        values = found.setdefault(name, [])
        # The same value assigned twice is one alternative; joining both copies
        # would split a flag such as -I$(DIR)/include into "-Idir dir/include".
        if value not in values:
            values.append(value)
    return found


def _expand(text: str, assignments: dict[str, list[str]], depth: int = 0) -> str:
    if depth > 10:
        return text
    return re.sub(
        r"\$\(([A-Za-z0-9_.-]+)\)",
        lambda match: _expand(" ".join(assignments.get(match.group(1), [])), assignments, depth + 1),
        text,
    )


def _rule_blocks(text: str, target: str) -> list[str]:
    """Header plus recipe of every rule whose target is exactly *target*."""
    lines = text.splitlines()
    blocks: list[str] = []
    for index, line in enumerate(lines):
        if not line.startswith(target + ":") or line.startswith(target + ":="):
            continue
        recipe = [line]
        for following in lines[index + 1:]:
            if not following.startswith("\t"):
                break
            recipe.append(following)
        blocks.append("\n".join(recipe))
    return blocks


def _has_flag(text: str, flag: str) -> bool:
    return re.search(re.escape(flag) + r"(?![A-Za-z0-9_./-])", text) is not None


def _python_builder_info(
    module_path: str, func_name: str,
) -> tuple[set[str], set[str], set[str], set[str]]:
    """Return ``(sources, defines, includes, warnings)`` from a Python build helper."""
    path = ROOT / module_path
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=module_path)
    func = next(
        (node for node in ast.walk(tree)
         if isinstance(node, ast.FunctionDef) and node.name == func_name),
        None,
    )
    if func is None:
        raise AssertionError(f"no function `{func_name}` in {module_path}")

    sources: set[str] = set()
    defines: set[str] = set()
    includes: set[str] = set()
    warnings: set[str] = set()
    for node in ast.walk(func):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            text = node.value
            if text.startswith("-D") and len(text) > 2:
                defines.add(text[2:])
            elif text.startswith("-I") and len(text) > 2:
                includes.add(text[2:])
            elif text.startswith("-Wno-"):
                warnings.add(text)
            elif text.endswith(".c"):
                sources.add(text)
        elif isinstance(node, ast.BinOp) and isinstance(node.op, ast.Div):
            parts: list[str] = []
            cur: ast.AST = node
            while isinstance(cur, ast.BinOp) and isinstance(cur.op, ast.Div):
                if isinstance(cur.right, ast.Constant) and isinstance(cur.right.value, str):
                    parts.append(cur.right.value)
                    cur = cur.left
                else:
                    parts = []
                    break
            if parts and isinstance(cur, ast.Name) and cur.id == "ROOT":
                sources.add("/".join(reversed(parts)))
    return sources, defines, includes, warnings


class TestStrictCompileFlagsMirrorTheMakefile(unittest.TestCase):
    """Each source is compiled with the flags the build compiles it with.

    The gate compiled every changed source with one fixed line, so a selftest
    whose hooks are declared under its own define (`SR_HLE_THREAD_SELFTEST`,
    `SR_SCHED_LIVENESS_TEST`), a player screen that includes `nk_ui.h`, and the
    Vulkan backend all failed on unchanged `origin/main`, for every contributor
    who touched them.  The per-source table must stay the build's own flags.
    """
    assignments: dict[str, list[str]]
    makefile: str

    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile = _makefile_text()
        cls.assignments = _assignments(cls.makefile)

    def _assert_unit_matches_build(self, source: str, unit: contrib_check.StrictUnit) -> None:
        self.assertTrue((ROOT / source).is_file(), f"{source} is not in the tree")
        self.assertTrue(
            bool(unit.recipes) != bool(unit.builder),
            f"{source} must name either Makefile recipes or a Python builder, not neither or both",
        )
        if unit.builder is not None:
            module_path, func_name = unit.builder
            self.assertTrue((ROOT / module_path).is_file(), f"{module_path} is not in the tree")
            sources, defines, includes, warnings = _python_builder_info(module_path, func_name)
            self.assertIn(
                source, sources,
                f"{module_path}:{func_name} does not compile {source}; the table names the wrong builder",
            )
            self.assertEqual(
                set(unit.defines), defines,
                f"defines for {source} ({sorted(unit.defines)}) differ from "
                f"{module_path}:{func_name} ({sorted(defines)})",
            )
            for include in unit.includes:
                self.assertIn(
                    include, includes,
                    f"-I{include} is listed for {source} but is not on {module_path}:{func_name}",
                )
            covered_includes = set(contrib_check.STRICT_C_FLAGS) | {f"-I{inc}" for inc in unit.includes}
            for include in sorted(includes):
                self.assertIn(
                    f"-I{include}", covered_includes,
                    f"{module_path}:{func_name} passes -I{include} for {source}, "
                    "which is missing from STRICT_C_FLAGS and STRICT_C_UNITS",
                )
            for warning in unit.warnings:
                self.assertIn(
                    warning, warnings,
                    f"{warning} is listed for {source} but is not on {module_path}:{func_name}",
                )
            self.assertFalse(
                unit.generated,
                f"{source} uses a Python builder and cannot name Makefile generated headers",
            )
            self.assertFalse(unit.leading_includes, f"{source} uses a Python builder and cannot order Makefile includes")
            return

        building: list[str] = []
        raw: list[str] = []
        for target in unit.recipes:
            blocks = _rule_blocks(self.makefile, target)
            self.assertTrue(blocks, f"no Makefile rule `{target}:` (named for {source})")
            for block in blocks:
                expanded = _expand(block, self.assignments)
                if source in expanded:
                    building.append(expanded)
                    raw.append(block)
        self.assertTrue(
            building, f"none of {unit.recipes} compiles {source}; the table names the wrong rule",
        )
        recipe = "\n".join(building)
        for define in unit.defines:
            self.assertTrue(
                _has_flag(recipe, f"-D{define}"),
                f"-D{define} is listed for {source} but is not on {unit.recipes}",
            )
        for include in unit.includes:
            self.assertTrue(
                _has_flag(recipe, f"-I{include}"),
                f"-I{include} is listed for {source} but is not on {unit.recipes}",
            )
        for warning in unit.warnings:
            self.assertTrue(
                _has_flag(recipe, warning),
                f"{warning} is listed for {source} but is not on {unit.recipes}",
            )
        for include in unit.leading_includes:
            flag = f"-I{include}"
            commands = [_expand(" ".join(block.splitlines()[1:]), self.assignments).split() for block in raw]
            self.assertTrue(any(flag in tokens for tokens in commands),
                            f"{flag} is listed first for {source} but is not on {unit.recipes}")
            for tokens in commands:
                if flag in tokens and "-Isrc/rt" in tokens:
                    self.assertLess(tokens.index(flag), tokens.index("-Isrc/rt"),
                                    f"{flag} is listed first for {source} but {unit.recipes} passes it after -Isrc/rt")
        for header in unit.generated:
            self.assertTrue(
                any(header.include in block for block in raw),
                f"{header.include} ({header.name}) is not on {unit.recipes}",
            )

    def test_every_listed_flag_is_on_the_recipe_that_builds_the_source(self) -> None:
        for source, unit in contrib_check.STRICT_C_UNITS.items():
            with self.subTest(source=source):
                self._assert_unit_matches_build(source, unit)

    def test_every_common_include_directory_is_one_the_makefile_uses(self) -> None:
        for flag in contrib_check.STRICT_C_FLAGS:
            if not flag.startswith("-I"):
                continue
            with self.subTest(flag=flag):
                self.assertTrue((ROOT / flag[2:]).is_dir(), f"{flag} names no directory")
                self.assertTrue(_has_flag(self.makefile, flag), f"the Makefile never passes {flag}")

    def test_the_drift_check_catches_a_define_the_recipe_does_not_set(self) -> None:
        """Guard the guard: an invented define must not be found on the recipe."""
        blocks = _rule_blocks(self.makefile, "sched-selftest-one")
        recipe = "\n".join(_expand(block, self.assignments) for block in blocks)
        self.assertTrue(_has_flag(recipe, "-DSR_SCHED_LIVENESS_TEST"))
        self.assertFalse(_has_flag(recipe, "-DSR_SCHED_LIVENESS"))
        self.assertFalse(_has_flag(recipe, "-DSR_HLE_THREAD_SELFTEST"))

    def test_the_drift_check_catches_a_python_builder_define_mismatch(self) -> None:
        """Guard the guard: missing or extra defines on a Python-built unit fail."""
        missing = contrib_check.StrictUnit(
            builder=("tools/nk_cli.py", "_build_vram_decoder"),
            defines=(),
        )
        with self.assertRaises(AssertionError):
            self._assert_unit_matches_build("src/rt/ge_vram_cli.c", missing)

        wrong = contrib_check.StrictUnit(
            builder=("tools/nk_cli.py", "_build_vram_decoder"),
            defines=("SR_GE_VRAM_DECODER_CLI", "SR_NOT_IN_BUILDER"),
        )
        with self.assertRaises(AssertionError):
            self._assert_unit_matches_build("src/rt/ge_vram_cli.c", wrong)

    def test_sources_that_include_vendored_headers_compile_clean(self) -> None:
        """The achievements and CHD sources find their vendored headers.

        Failing before: strict-c-compile reported ``rc_client.h`` and
        ``libchdr/chd.h`` as missing for any change to these files, a FAIL the
        build never sees.
        """
        compiler = contrib_check.find_tool("gcc") or contrib_check.find_tool("cc")
        if compiler is None:
            self.skipTest("no C compiler on PATH")
        for source in ("src/core/nk_achievements.c", "src/core/nk_disc_chd.c",
                       "tests/native/test_disc_expand.c"):
            with self.subTest(source=source):
                command, reason = contrib_check.strict_compile_command(compiler, source, [])
                self.assertEqual(reason, "")
                assert command is not None
                result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=600)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_a_leading_include_comes_before_the_common_folders(self) -> None:
        """Failing before: nk_h264_api.c failed strict-c-compile on unchanged main.

        Its rule puts `-Isrc/rt/h264_ffmpeg_sub` before `$(CFLAGS)`, so the subset's
        `config.h` is found first. The table could only append include folders, so the
        strict line found another header first and the compile stopped.
        """
        source = "src/rt/h264_ffmpeg_sub/nk_h264_api.c"
        command, reason = contrib_check.strict_compile_command("cc", source, [])
        self.assertEqual(reason, "")
        assert command is not None
        self.assertLess(command.index("-Isrc/rt/h264_ffmpeg_sub"), command.index("-Isrc/rt"))
        self.assertIn("-Werror", command)
        self.assertEqual(command[-1], source)

    def test_the_drift_check_catches_a_leading_include_the_recipe_does_not_pass(self) -> None:
        """Guard the guard: a first folder the rule does not pass must fail."""
        unit = contrib_check.STRICT_C_UNITS["src/rt/h264_ffmpeg_sub/nk_h264_api.c"]
        wrong = contrib_check.StrictUnit(recipes=unit.recipes, defines=unit.defines,
                                         leading_includes=("src/rt/h264_ffmpeg_sub/libavcodec",))
        with self.assertRaises(AssertionError):
            self._assert_unit_matches_build("src/rt/h264_ffmpeg_sub/nk_h264_api.c", wrong)

    def test_a_selftest_is_compiled_with_its_own_defines(self) -> None:
        command, reason = contrib_check.strict_compile_command("cc", "tests/native/hle_thread_selftest.c", [])
        self.assertEqual(reason, "")
        for define in ("SR_HLE_THREAD_SELFTEST", "SR_CORO_LIFECYCLE_TEST", "SR_SCHED_LIVENESS_TEST"):
            self.assertIn(f"-D{define}", command)
        self.assertEqual(command[-1], "tests/native/hle_thread_selftest.c")
        plain, _ = contrib_check.strict_compile_command("cc", "src/rt/hle.c", [])
        self.assertFalse([flag for flag in plain if flag.startswith("-D")])

    def test_achievements_bridge_uses_player_rcheevos_include_paths(self) -> None:
        source = "src/core/nk_achievements.c"
        command, reason = contrib_check.strict_compile_command("cc", source, [])
        self.assertEqual(reason, "")
        self.assertIn("-Ithird_party/rcheevos/include", command)
        self.assertIn("-Ithird_party/rcheevos/src", command)

        unit = contrib_check.STRICT_C_UNITS[source]
        self.assertEqual(unit.recipes, ("$(PLAYER_EXE)",))
        recipe = " ".join(
            _expand(block, self.assignments)
            for block in _rule_blocks(self.makefile, "$(PLAYER_EXE)")
        )
        self.assertIn(source, recipe)
        self.assertTrue(_has_flag(recipe, "-Ithird_party/rcheevos/include"))
        self.assertTrue(_has_flag(recipe, "-Ithird_party/rcheevos/src"))


class TestStrictCompileUnavailableInputs(unittest.TestCase):
    def _fail_if_compiled(self, *args, **kwargs):
        raise AssertionError("the compiler must not run for a file that cannot be compiled here")

    def test_a_missing_generated_header_is_not_run_rather_than_failed(self) -> None:
        with tempfile.TemporaryDirectory() as empty, \
                mock.patch.dict(contrib_check.os.environ, {"BUILD_ROOT": empty, "CC": "cc"}), \
                mock.patch.object(contrib_check, "vulkan_include_flags", return_value=([], "")), \
                mock.patch.object(contrib_check.subprocess, "run", side_effect=self._fail_if_compiled):
            gate = contrib_check.c_gate(["tests/fixtures/cosim/cosim_selftest.c"])
        self.assertEqual(gate.status, contrib_check.NOT_RUN)
        self.assertIn("cosim_recomp_funcs.h", gate.detail)
        self.assertIn("make cosim-selftest", gate.detail)

    def test_a_present_generated_header_is_put_on_the_include_path(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            fixture = Path(root) / "cosim" / "fixture"
            fixture.mkdir(parents=True)
            (Path(root) / "cosim" / "cosim_recomp_funcs.h").write_text("", encoding="ascii")
            for name in ("cosim_cells.h", "cosim_fpu_corpus.h"):
                (fixture / name).write_text("", encoding="ascii")
            with mock.patch.dict(contrib_check.os.environ, {"BUILD_ROOT": root}):
                command, reason = contrib_check.strict_compile_command(
                    "cc", "tests/fixtures/cosim/cosim_selftest.c", [],
                )
        self.assertEqual(reason, "")
        self.assertIn(f"-I{root}/cosim", command)
        self.assertIn(f"-I{root}/cosim/fixture", command)
        self.assertEqual(command.count(f"-I{root}/cosim/fixture"), 1)
        self.assertIn("-DSR_INSTRUCTION_TRACE", command)

    def test_a_missing_vulkan_sdk_is_a_skip_with_its_reason_never_a_pass(self) -> None:
        missing = _Completed(
            1, "", "src/rt/gpu_sdl3vk/ge_gpu.c:82:10: fatal error: vulkan/vulkan.h: "
            "No such file or directory\ncompilation terminated.",
        )
        with mock.patch.dict(contrib_check.os.environ, {"CC": "cc"}), \
                mock.patch.object(contrib_check, "discover_vulkan_sdk",
                                  side_effect=contrib_check.VulkanSdkError("No usable Vulkan SDK found.")), \
                mock.patch.object(contrib_check.subprocess, "run", return_value=missing):
            gate = contrib_check.c_gate(["src/rt/gpu_sdl3vk/ge_gpu.c"])
        self.assertEqual(gate.status, contrib_check.SKIP)
        self.assertIn("Vulkan SDK", gate.detail)
        self.assertIn("No usable Vulkan SDK found.", gate.detail)

    def _gate_for_output(self, source: str, stderr: str):
        with mock.patch.dict(contrib_check.os.environ, {"CC": "cc"}), \
                mock.patch.object(contrib_check, "vulkan_include_flags", return_value=([], "")), \
                mock.patch.object(contrib_check.subprocess, "run", return_value=_Completed(1, "", stderr)):
            return contrib_check.c_gate([source])

    def test_a_missing_pspsdk_header_is_a_skip_with_its_reason_never_a_fail(self) -> None:
        for header in ("pspkernel.h", "pspiofilemgr_fcntl.h", "psputility_osk.h"):
            with self.subTest(header=header):
                gate = self._gate_for_output(
                    "tests/fixtures/psp_oracle/probe_ge_sample.c",
                    f"tests/fixtures/psp_oracle/probe_ge_sample.c:5:10: fatal error: {header}: "
                    "No such file or directory\ncompilation terminated.",
                )
                self.assertEqual(gate.status, contrib_check.SKIP)
                self.assertIn("PSPSDK", gate.detail)
                self.assertIn(header, gate.detail)

    def test_a_missing_project_psp_header_still_fails(self) -> None:
        gate = self._gate_for_output(
            "tests/native/test_player_state.c",
            "tests/native/test_player_state.c:3:10: fatal error: psp_seal_fixture.h: "
            "No such file or directory\ncompilation terminated.",
        )
        self.assertEqual(gate.status, contrib_check.FAIL)

    def test_a_real_error_beside_a_missing_sdk_header_still_fails(self) -> None:
        for header in ("pspkernel.h", "vulkan/vulkan.h"):
            with self.subTest(header=header):
                gate = self._gate_for_output(
                    "tests/fixtures/psp_oracle/probe_ge_sample.c",
                    "tests/fixtures/psp_oracle/probe_ge_sample.c:2:1: error: unknown type name 'foo_t'\n"
                    f"tests/fixtures/psp_oracle/probe_ge_sample.c:5:10: fatal error: {header}: "
                    "No such file or directory\ncompilation terminated.",
                )
                self.assertEqual(gate.status, contrib_check.FAIL)
                self.assertIn("unknown type name", gate.detail)

    def test_a_real_error_far_above_a_missing_sdk_header_still_fails(self) -> None:
        """Failing-before: the SDK classification read only the six lines shown in the detail, so an error with
        its source and caret lines above the missing header's block was classified as a missing SDK (SKIP)."""
        source = "tests/fixtures/psp_oracle/probe_ge_sample.c"
        gate = self._gate_for_output(
            source,
            "In file included from tests/fixtures/psp_oracle/probe_ge_sample.c:1:\n"
            "tests/fixtures/psp_oracle/h.h:1:1: error: unknown type name 'foo_t'\n"
            "    1 | foo_t x;\n"
            "      | ^~~~~\n"
            f"{source}:2:10: fatal error: vulkan/vulkan.h: No such file or directory\n"
            "    2 | #include <vulkan/vulkan.h>\n"
            "      |          ^~~~~~~~~~~~~~~~~\n"
            "compilation terminated.",
        )
        self.assertEqual(gate.status, contrib_check.FAIL, gate.detail)

    def test_a_discovered_vulkan_sdk_is_on_the_include_path(self) -> None:
        sdk = Path(tempfile.gettempdir()) / "VulkanSDK" / "1.4.0"
        with mock.patch.object(contrib_check, "discover_vulkan_sdk", return_value=sdk):
            flags, reason = contrib_check.vulkan_include_flags()
        self.assertEqual(reason, "")
        self.assertIn(f"-I{sdk.as_posix()}/include", flags)

    def test_a_real_failure_outranks_a_skip_and_a_not_run(self) -> None:
        def fake_run(command, **kwargs):
            if command[-1].endswith("ge_gpu.c"):
                return _Completed(1, "", "fatal error: vulkan/vulkan.h: No such file or directory")
            return _Completed(1, "", "src/rt/hle.c:1:1: error: boom")

        with tempfile.TemporaryDirectory() as empty, \
                mock.patch.dict(contrib_check.os.environ, {"BUILD_ROOT": empty, "CC": "cc"}), \
                mock.patch.object(contrib_check, "vulkan_include_flags", return_value=([], "none")), \
                mock.patch.object(contrib_check.subprocess, "run", side_effect=fake_run):
            gate = contrib_check.c_gate([
                "src/rt/gpu_sdl3vk/ge_gpu.c", "tests/fixtures/cosim/cosim_selftest.c", "src/rt/hle.c",
            ])
        self.assertEqual(gate.status, contrib_check.FAIL)
        self.assertIn("src/rt/hle.c: ", gate.detail)
        self.assertIn("SKIP src/rt/gpu_sdl3vk/ge_gpu.c", gate.detail)
        self.assertIn("NOT_RUN tests/fixtures/cosim/cosim_selftest.c", gate.detail)

    def test_a_deleted_source_is_not_compiled(self) -> None:
        with mock.patch.object(contrib_check.subprocess, "run", side_effect=self._fail_if_compiled):
            gate = contrib_check.c_gate(["src/rt/no_such_file_any_more.c"])
        self.assertEqual(gate.status, contrib_check.NOT_RUN)


class TestStrictCompileOnUnchangedMain(unittest.TestCase):
    """The sources that failed the gate on unchanged `origin/main` no longer do.

    Each one either compiles, or reports why it could not be compiled here
    (`SKIP` for an absent SDK, `NOT_RUN` for an absent generated header).
    """

    SOURCES = (
        "tests/native/hle_thread_selftest.c",
        "tests/native/sched_selftest.c",
        "src/player/ui/player_ui_nk.c",
        "src/player/ui/scr_title.c",
        "tests/native/test_player_screens.c",
        "src/rt/gpu_sdl3vk/ge_gpu.c",
        "tests/fixtures/cosim/cosim_selftest.c",
        "tests/native/texrepl_selftest.c",
        "src/rt/ge_vram_cli.c",
        "src/rt/h264_ffmpeg_sub/nk_h264_api.c",
    )

    def test_no_unchanged_source_fails_the_strict_compile(self) -> None:
        if not (contrib_check.os.environ.get("CC") or contrib_check.find_tool("gcc")
                or contrib_check.find_tool("cc")):
            self.skipTest("no C compiler on PATH")
        for source in self.SOURCES:
            with self.subTest(source=source):
                gate = contrib_check.c_gate([source])
                if gate.status in (contrib_check.SKIP, contrib_check.NOT_RUN):
                    # Not compiled here: a skipped subtest, never a pass (AGENTS.md section 9).
                    self.skipTest(f"{gate.status}: {gate.detail}")
                self.assertEqual(gate.status, contrib_check.PASS, gate.detail)

    def test_an_uncompiled_source_is_reported_skipped_not_passed(self) -> None:
        """Failing-before: the test asserted only `!= FAIL`, so a source the gate never compiled
        (an absent SDK or generated header) counted as a pass."""
        for status in (contrib_check.SKIP, contrib_check.NOT_RUN):
            with self.subTest(status=status):
                inner = TestStrictCompileOnUnchangedMain("test_no_unchanged_source_fails_the_strict_compile")
                result = unittest.TestResult()
                with mock.patch.object(contrib_check, "find_tool", return_value="gcc"), \
                        mock.patch.object(contrib_check, "c_gate",
                                          return_value=contrib_check.Gate("C", status, "not compiled")):
                    inner.run(result)
                self.assertEqual(len(result.skipped), len(self.SOURCES), result.skipped)
                self.assertEqual(result.failures + result.errors, [])
        inner = TestStrictCompileOnUnchangedMain("test_no_unchanged_source_fails_the_strict_compile")
        result = unittest.TestResult()
        with mock.patch.object(contrib_check, "find_tool", return_value="gcc"), \
                mock.patch.object(contrib_check, "c_gate",
                                  return_value=contrib_check.Gate("C", "BLOCKED", "odd")):
            inner.run(result)
        self.assertEqual(len(result.failures), len(self.SOURCES))


class TestPublicationGateSplit(unittest.TestCase):
    """Only a maintainer can clear the generated-control findings."""

    def _run(self, audit_stdout: str, audit_returncode: int = 1):
        def fake_run(command, **kwargs):
            if "policy_sync.py" in " ".join(command):
                return _Completed(0, "policy sync: OK\n")
            return _Completed(audit_returncode, audit_stdout, "")

        with mock.patch.object(contrib_check.subprocess, "run", side_effect=fake_run):
            return contrib_check.publication_gate()

    def test_only_maintainer_side_findings_do_not_fail_a_contributor(self) -> None:
        policy, audit = self._run(f"{MAINTAINER_LINE}\n{SUMMARY_LINE}\n")
        self.assertEqual(policy.status, contrib_check.PASS)
        self.assertEqual(audit.status, contrib_check.MAINTAINER_SIDE)
        self.assertIn("PROVENANCE_CONTENT_MISMATCH", audit.detail)

    def test_a_contributor_fixable_finding_still_fails(self) -> None:
        _policy, audit = self._run(f"{MAINTAINER_LINE}\n{CONTRIBUTOR_LINE}\n{SUMMARY_LINE}\n")
        self.assertEqual(audit.status, contrib_check.FAIL)
        self.assertIn("POLICY_UNCLASSIFIED", audit.detail)
        self.assertNotIn("PROVENANCE_CONTENT_MISMATCH", audit.detail)

    def test_an_unclassified_new_path_is_a_contributor_problem(self) -> None:
        """Adding a file the policy does not classify is the author's own gap."""
        _policy, audit = self._run(f"{CONTRIBUTOR_LINE}\n{SUMMARY_LINE}\n")
        self.assertEqual(audit.status, contrib_check.FAIL)

    def test_a_clean_audit_passes(self) -> None:
        _policy, audit = self._run("publication audit: OK\n", audit_returncode=0)
        self.assertEqual(audit.status, contrib_check.PASS)

    def test_the_stale_control_hint_does_not_turn_maintainer_side_into_a_failure(self) -> None:
        """publish_audit prints its regen hint after a stale-control finding; the hint is not a finding."""
        import publish_audit

        _policy, audit = self._run(f"{MAINTAINER_LINE}\n{SUMMARY_LINE}\n{publish_audit.STALE_CONTROL_FIX}\n")
        self.assertEqual(audit.status, contrib_check.MAINTAINER_SIDE, audit.detail)

    def test_an_audit_without_a_complete_verdict_fails_rather_than_passes(self) -> None:
        """A killed, crashed or silent audit with no finding line never reports a pass."""
        for stdout, returncode in (("", 1), ("", -9), ("MemoryError\n", 1), ("", 0),
                                   ("publication audit: FAIL (0 findings)\n", 1)):
            with self.subTest(stdout=stdout, returncode=returncode):
                _policy, audit = self._run(stdout, audit_returncode=returncode)
                self.assertEqual(audit.status, contrib_check.FAIL)
                self.assertIn(f"exited {returncode}", audit.detail)

    def test_policy_drift_fails_before_the_audit_runs(self) -> None:
        def fake_run(command, **kwargs):
            if "policy_sync.py" in " ".join(command):
                return _Completed(1, "", "unclassified: tools/new.py")
            raise AssertionError("the audit must not run once the policy check failed")

        with mock.patch.object(contrib_check.subprocess, "run", side_effect=fake_run):
            policy, audit = contrib_check.publication_gate()
        self.assertEqual(policy.status, contrib_check.FAIL)
        self.assertEqual(audit.status, contrib_check.NOT_RUN)

    def test_the_maintainer_side_code_list_names_real_audit_codes(self) -> None:
        source = (ROOT / "tools" / "publish_audit.py").read_text(encoding="utf-8")
        for code in contrib_check.MAINTAINER_SIDE_CODES:
            self.assertIn(f'"{code}"', source, f"{code} is not a finding this audit can raise")


class TestMakeTargetWiring(unittest.TestCase):
    def test_the_makefile_exposes_contrib_check_through_the_help_catalog(self) -> None:
        makefile = read_makefile(ROOT)
        self.assertRegex(makefile, r"(?m)^contrib-check:")
        self.assertIn("tools/contrib_check.py", makefile)
        self.assertRegex(makefile, r"(?m)^\tcontrib-check \\$")
        self.assertRegex(
            makefile,
            r"(?m)^HELP_DESCRIPTION_contrib-check := .+$",
        )

    def test_the_target_runs_on_both_make_frontends(self) -> None:
        """mingw32-make and make share the recipe; nothing in it is host-specific."""
        makefile = read_makefile(ROOT)
        recipe = makefile.split("\ncontrib-check:\n", 1)[1].split("\n\n", 1)[0]
        self.assertIn("$(PYTHON)", recipe)
        for host_specific in ("mingw32", "cmd /c", "pwsh", "powershell"):
            self.assertNotIn(host_specific, recipe)


if __name__ == "__main__":
    unittest.main()
