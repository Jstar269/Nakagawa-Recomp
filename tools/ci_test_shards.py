# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Deterministic cost-weighted partition of the Python test modules into CI shards.

The hosted ``python_tools`` job runs the ``tools/test_*.py`` modules on several
runners.  Alphabetical round-robin put the heaviest modules on the same runner by
accident, so one shard set the job's wall clock while the others idled.  This tool
replaces it with Longest-Processing-Time-first (LPT) bin packing over measured
per-module durations:

1. every module gets a weight -- its measured seconds from
   ``tools/ci_test_weights.json``, or that file's ``default_seconds`` when the module
   has not been measured yet (a new test therefore never needs a data edit to run);
2. modules are taken heaviest first, ties broken by name, and each goes to the
   least-loaded shard, ties broken by the lowest shard index;
3. a module that belongs to a ``separate`` group never joins a shard that already
   holds another member of that group.

The partition depends only on the discovered module names and the data file, so
every runner computes the same plan independently and no coordination is needed.
Each shard's modules are printed in name order, matching the order unittest used for
the round-robin lists, so moving a module between shards never changes the relative
order of the modules that share a runner.

Usage::

    python tools/ci_test_shards.py --shard 0          # module names for shard 0
    python tools/ci_test_shards.py --plan             # whole plan with estimates
    python tools/ci_test_shards.py --check            # validate the data and plan

``--check`` fails (exit 1) when the data names a module that no longer exists, a
weight is not a positive finite number, a separation group is malformed or larger
than the shard count, or the plan does not schedule every discovered module exactly
once.  Unmeasured modules are reported but are not an error.  Any usage or data
error exits 2 without printing a module list, so a caller that captures the list
can never mistake a failure for an empty shard.
"""

from __future__ import annotations

import argparse
import ast
from dataclasses import dataclass
import json
import math
import os
from pathlib import Path
import re
import sys
from typing import Iterable, Mapping, Sequence
import unittest


ROOT = Path(__file__).resolve().parents[1]
TOOLS_DIR = ROOT / "tools"
DEFAULT_WEIGHTS = TOOLS_DIR / "ci_test_weights.json"
DEFAULT_NUM_SHARDS = 4  # tools/ci_paths.py PYTHON_SHARDS_FULL; the tests keep them equal
TEST_GLOB = "test_*.py"
SELECTIONS = ("all", "makefile")
#: TOOLCHAIN_MISSING: a test that needs gcc found none; hosted python_tools fails on any (F-6a8c715a).
SKIP_REASON_PREFIXES = ("NIGHTLY_TIER:", "WINDOWS_ONLY:", "PRIVATE_INPUT:", "TOOLCHAIN_MISSING:")


def nightly_tier(test_method):
    """Run a slow, non-blocking test only when ``NK_TEST_TIER=full`` is set."""

    return unittest.skipUnless(
        os.environ.get("NK_TEST_TIER") == "full",
        "NIGHTLY_TIER: set NK_TEST_TIER=full to run this slow case",
    )(test_method)


def skip_prefix_summary(unittest_output: str) -> str:
    """Return one stable summary line with unittest skips counted by reason prefix."""

    counts = dict.fromkeys(SKIP_REASON_PREFIXES, 0)
    for line in unittest_output.splitlines():
        marker = " ... skipped "
        if marker not in line:
            continue
        rendered_reason = line.rsplit(marker, 1)[1].strip()
        try:
            reason = ast.literal_eval(rendered_reason)
        except (SyntaxError, ValueError):
            continue
        if not isinstance(reason, str):
            continue
        for prefix in SKIP_REASON_PREFIXES:
            if reason.startswith(prefix):
                counts[prefix] += 1
                break
    fields = " ".join(f"{prefix[:-1]}={counts[prefix]}" for prefix in SKIP_REASON_PREFIXES)
    return f"Python test skip prefixes: {fields}"


class ShardDataError(ValueError):
    """The weight data or a requested plan is unusable."""


@dataclass(frozen=True)
class WeightData:
    default_seconds: float
    weights: Mapping[str, float]
    separate: tuple[frozenset[str], ...]


@dataclass(frozen=True)
class ShardPlan:
    shards: tuple[tuple[str, ...], ...]
    loads: tuple[float, ...]
    unmeasured: tuple[str, ...]


def discover_modules(tools_dir: Path = TOOLS_DIR) -> list[str]:
    """Return the sorted module names unittest discovery would load from ``tools_dir``."""

    return sorted(path.stem for path in tools_dir.glob(TEST_GLOB) if path.is_file())


def _positive_seconds(value: object, label: str) -> float:
    # bool is an int subclass; a JSON ``true`` is a data error, not one second.
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ShardDataError(f"{label} must be a number of seconds, got {value!r}")
    seconds = float(value)
    if not math.isfinite(seconds) or seconds <= 0.0:
        raise ShardDataError(f"{label} must be a positive finite number of seconds, got {value!r}")
    return seconds


def parse_weight_data(raw: object) -> WeightData:
    """Validate the decoded JSON weight document and return it as ``WeightData``."""

    if not isinstance(raw, dict):
        raise ShardDataError("weight data must be a JSON object")
    unknown_keys = sorted(set(raw) - {"description", "default_seconds", "separate", "weights"})
    if unknown_keys:
        raise ShardDataError(f"weight data has unknown keys: {', '.join(unknown_keys)}")
    default_seconds = _positive_seconds(raw.get("default_seconds"), "default_seconds")

    weights_raw = raw.get("weights")
    if not isinstance(weights_raw, dict) or not weights_raw:
        raise ShardDataError("weights must be a non-empty object of module -> seconds")
    weights: dict[str, float] = {}
    for module, seconds in weights_raw.items():
        if not isinstance(module, str) or not module.startswith("test_"):
            raise ShardDataError(f"weights key {module!r} is not a test module name")
        weights[module] = _positive_seconds(seconds, f"weights[{module!r}]")

    separate_raw = raw.get("separate", [])
    if not isinstance(separate_raw, list):
        raise ShardDataError("separate must be a list of module-name lists")
    separate: list[frozenset[str]] = []
    for index, group in enumerate(separate_raw):
        if (
            not isinstance(group, list)
            or len(group) < 2
            or not all(isinstance(name, str) and name.startswith("test_") for name in group)
        ):
            raise ShardDataError(f"separate[{index}] must list at least two test module names")
        if len(set(group)) != len(group):
            raise ShardDataError(f"separate[{index}] repeats a module")
        separate.append(frozenset(group))
    return WeightData(default_seconds, weights, tuple(separate))


def load_weight_data(path: Path = DEFAULT_WEIGHTS) -> WeightData:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise ShardDataError(f"cannot read weight data {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise ShardDataError(f"weight data {path} is not valid JSON: {exc}") from exc
    return parse_weight_data(raw)


def plan_shards(modules: Iterable[str], data: WeightData, num_shards: int) -> ShardPlan:
    """Partition ``modules`` into ``num_shards`` shards with constrained LPT.

    Raises ``ShardDataError`` when the request cannot be satisfied: no shards, a
    duplicated module name, or a separation group with more present members than
    there are shards.
    """

    if isinstance(num_shards, bool) or not isinstance(num_shards, int) or num_shards < 1:
        raise ShardDataError(f"num_shards must be a positive integer, got {num_shards!r}")
    module_list = list(modules)
    if len(set(module_list)) != len(module_list):
        raise ShardDataError("module list contains duplicates")
    present = set(module_list)

    groups_of: dict[str, list[frozenset[str]]] = {}
    for group in data.separate:
        members = group & present
        if len(members) > num_shards:
            raise ShardDataError(
                f"separation group {sorted(group)} needs {len(members)} shards, only {num_shards} exist"
            )
        for member in members:
            groups_of.setdefault(member, []).append(members)

    def weight(module: str) -> float:
        return data.weights.get(module, data.default_seconds)

    shards: list[list[str]] = [[] for _ in range(num_shards)]
    loads = [0.0] * num_shards
    for module in sorted(module_list, key=lambda name: (-weight(name), name)):
        blocked: set[int] = set()
        for members in groups_of.get(module, ()):
            for index, shard in enumerate(shards):
                if members.intersection(shard):
                    blocked.add(index)
        candidates = [index for index in range(num_shards) if index not in blocked]
        # Unreachable while every group fits in num_shards (checked above): a group
        # of k members blocks at most k - 1 shards before its last member lands.
        if not candidates:
            raise ShardDataError(f"no shard can accept {module} without breaking a separation group")
        target = min(candidates, key=lambda index: (loads[index], index))
        shards[target].append(module)
        loads[target] += weight(module)

    unmeasured = tuple(sorted(module for module in module_list if module not in data.weights))
    return ShardPlan(
        shards=tuple(tuple(sorted(shard)) for shard in shards),
        loads=tuple(round(load, 3) for load in loads),
        unmeasured=unmeasured,
    )


def check_plan(
    modules: Sequence[str],
    data: WeightData,
    plan: ShardPlan,
    *,
    tracked: Sequence[str] | None = None,
) -> list[str]:
    """Return every reason the data or plan is not acceptable for CI (empty when clean).

    ``modules`` are the planned modules; ``tracked`` (default: the same) are every
    module that exists, against which the data is checked for stale names.
    """

    errors: list[str] = []
    present = set(modules)
    existing = set(tracked if tracked is not None else modules)
    stale = sorted(set(data.weights) - existing)
    if stale:
        errors.append("weights name modules that no longer exist: " + ", ".join(stale))
    for group in data.separate:
        missing = sorted(group - existing)
        if missing:
            errors.append(f"separation group {sorted(group)} names missing modules: {', '.join(missing)}")
    scheduled = [module for shard in plan.shards for module in shard]
    if sorted(scheduled) != sorted(modules):
        duplicated = sorted({module for module in scheduled if scheduled.count(module) > 1})
        dropped = sorted(present - set(scheduled))
        extra = sorted(set(scheduled) - present)
        errors.append(
            f"plan does not schedule every module exactly once (duplicated={duplicated}, "
            f"dropped={dropped}, unknown={extra})"
        )
    for group in data.separate:
        for index, shard in enumerate(plan.shards):
            together = sorted(group.intersection(shard))
            if len(together) > 1:
                errors.append(f"plan puts separated modules together on shard {index}: {', '.join(together)}")
    if len(modules) >= len(plan.shards):
        for index, shard in enumerate(plan.shards):
            if not shard:
                errors.append(f"shard {index} is empty")
    return errors


# ---- Makefile-coupled selection (#702) ---------------------------------------------
#
# A recipe-only Makefile change (tools/ci_paths.py proves that property) can only
# change the outcome of a Python test that reads the Makefile or runs make, directly
# or through a tools module it imports or launches.  ``makefile_coupled_modules``
# computes that set statically and errs toward inclusion:
#
# * a module's dependencies are its local imports (``import x``, ``from tools import
#   x``, ``from psp_oracle import y``) *and* every local tool or package its string
#   literals name, which is how a test launches a tool as a subprocess:
#   ``"tools/codegen.py"``, ``"tools/psp_oracle/run_nakagawa.py"`` (the package),
#   ``"-m", "enhancement_package"`` or ``"psp_oracle.run_nakagawa"``;
# * a module couples to make when its text names the Makefile, a ``.mk`` file,
#   ``$(MAKE)``, ``gmake``/``mingw32-make``, or a string literal that starts with the
#   ``make`` or ``MAKE`` command word (``"make"``, ``f"make {goal}"``, ``"MAKE"``);
# * a shell or PowerShell script in tools/ (``"tools/nk_manager.ps1"``) is a unit too:
#   its dependencies are the local tools its text names, and it also couples when it
#   runs ``make`` as a bare command word;
# * an import of a repository module outside tools/ (a fixture generator under tests/
#   that a test puts on ``sys.path``) counts as coupled, because it is not inspected;
# * the closure is transitive, and a package dependency pulls in every file in it;
# * a file that cannot be read or parsed counts as coupled.
#
# The selection is therefore a superset of the modules a recipe edit can affect,
# never a guess at a subset.

MAKEFILE_COUPLING = re.compile(
    r"\bMakefile\b|\bmingw32-make\b|\bgmake\b|\$\(MAKE\)|\.mk\b"
    r"|[\"'](?:make|MAKE)(?:\.exe)?(?:[\"'\s]|$)"
)
# A script runs make as a bare command word, which the Python-literal form above does not see.
_SCRIPT_MAKE = re.compile(r"(?<![\w$.-])make(?:\.exe)?(?![\w.-])")
_SCRIPT_SUFFIXES = frozenset({".bat", ".cmd", ".ps1", ".psm1", ".sh"})
# Directories never walked for repository modules: local inputs and generated output, which
# hold no module a test imports (and must not be read by a planner that runs anywhere).
_UNWALKED_DIRS = frozenset({"build", "fs", "keys", "logs", "memstick", "node_modules", "oracle",
                            "place_game_here"})
_LITERAL_SEPARATORS = re.compile(r"[\s/\\]+")
_DOTTED_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*")


def _literal_names(value: str) -> set[str]:
    """Return every candidate local unit name a string literal can refer to.

    Each whitespace- or slash-separated token contributes its own name with a
    ``.py`` suffix removed, and, when it is a dotted name, its first component (the
    second after a leading ``tools``).  The caller intersects the result with the
    local units, so an unrelated word costs nothing and a near miss errs toward
    inclusion.
    """

    names: set[str] = set()
    for token in _LITERAL_SEPARATORS.split(value):
        token = token.strip("\"'`,.;:()[]{}")
        if token.endswith(".py"):
            token = token[:-3]
        if not _DOTTED_NAME.fullmatch(token):
            continue
        names.add(token)
        parts = token.split(".")
        names.add(parts[1] if parts[0] == "tools" and len(parts) > 1 else parts[0])
    return names


def _local_units(tools_dir: Path) -> dict[str, list[Path]]:
    units: dict[str, list[Path]] = {path.stem: [path] for path in tools_dir.glob("*.py") if path.is_file()}
    for package in tools_dir.iterdir():
        if package.is_dir() and not package.name.startswith((".", "__")):
            files = sorted(package.rglob("*.py"))
            if files:
                units.setdefault(package.name, []).extend(files)
    for script in tools_dir.iterdir():
        if script.is_file() and script.suffix.lower() in _SCRIPT_SUFFIXES:
            units[script.name] = [script]
    return units


def _outside_modules(repo_dir: Path | None, tools_dir: Path) -> frozenset[str]:
    """Return the names an import can resolve to among the repository's Python files
    outside ``tools_dir``: every ``.py`` file's stem and every package directory's name."""

    if repo_dir is None:
        return frozenset()
    names: set[str] = set()
    skip = tools_dir.resolve()
    for folder, dirs, files in os.walk(repo_dir):
        here = Path(folder)
        dirs[:] = sorted(
            name for name in dirs
            if not name.startswith(".") and name != "__pycache__"
            and not (here == repo_dir and name in _UNWALKED_DIRS)
            and (here / name).resolve() != skip
        )
        for name in files:
            if name.endswith(".py"):
                names.add(name[:-3])
        if "__init__.py" in files and here != repo_dir:
            names.add(here.name)
    return frozenset(names)


def _script_edges(path: Path, local: frozenset[str]) -> tuple[set[str], bool]:
    """Return the local units a script names, and whether it runs make or cannot be read."""

    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return set(), True
    return _literal_names(text) & local, bool(_SCRIPT_MAKE.search(text))


def _unit_edges(path: Path, local: frozenset[str], outside: frozenset[str] = frozenset()) -> tuple[set[str], bool]:
    """Return the local units ``path`` depends on, and whether it is unparseable or imports
    a repository module outside tools/ (``outside``), which is not inspected."""

    try:
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    except (OSError, UnicodeDecodeError, SyntaxError, ValueError):
        return set(), True
    edges: set[str] = set()
    imported: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                parts = alias.name.split(".")
                edges.add(parts[1] if parts[0] == "tools" and len(parts) > 1 else parts[0])
                imported.add(parts[0])
        elif isinstance(node, ast.ImportFrom):
            if not node.level and node.module:
                imported.add(node.module.split(".")[0])
            parts = (node.module or "").split(".")
            if node.level or parts == ["tools"] or not node.module:
                edges.update(alias.name for alias in node.names)
                if node.level:
                    # A relative import inside a package depends on that package.
                    edges.add(path.parent.name)
            edges.add(parts[1] if parts[0] == "tools" and len(parts) > 1 else parts[0])
        elif isinstance(node, ast.Constant) and isinstance(node.value, str):
            edges.update(_literal_names(node.value))
    return edges & local, bool((imported - local - {"tools"}) & outside)


def makefile_coupled_modules(modules: Iterable[str], tools_dir: Path = TOOLS_DIR,
                             repo_dir: Path | None = ROOT) -> list[str]:
    """Return the test modules whose transitive tool closure can observe the Makefile.

    ``repo_dir`` is the repository whose Python files outside ``tools_dir`` a test can
    import; ``None`` looks at ``tools_dir`` alone."""

    units = _local_units(tools_dir)
    local = frozenset(units)
    outside = _outside_modules(repo_dir, tools_dir) - local
    edges: dict[str, set[str]] = {}
    coupled_unit: dict[str, bool] = {}
    for name, files in units.items():
        deps: set[str] = set()
        coupled = False
        for file in files:
            if file.suffix.lower() in _SCRIPT_SUFFIXES:
                file_edges, unparseable = _script_edges(file, local)
            else:
                file_edges, unparseable = _unit_edges(file, local, outside)
            deps |= file_edges
            try:
                text = file.read_text(encoding="utf-8")
            except (OSError, UnicodeDecodeError):
                coupled = True
                continue
            coupled = coupled or unparseable or bool(MAKEFILE_COUPLING.search(text))
        deps.discard(name)
        edges[name] = deps
        coupled_unit[name] = coupled

    selected: list[str] = []
    for module in modules:
        if module not in units:
            selected.append(module)  # cannot be inspected, so it cannot be excluded
            continue
        # Walk the module's import closure; any coupled unit in it selects the module.
        # A unit missing from the map cannot be inspected, so it counts as coupled.
        visited = {module}
        stack = [module]
        reaches_coupled = False
        while stack and not reaches_coupled:
            current = stack.pop()
            reaches_coupled = coupled_unit.get(current, True)
            for dep in edges.get(current, ()):
                if dep not in visited:
                    visited.add(dep)
                    stack.append(dep)
        if reaches_coupled:
            selected.append(module)
    return sorted(selected)


# ---- Windows-only tests ------------------------------------------------------------
#
# python_tools runs every module on Linux, so a test that skips on every host but
# Windows never runs there.  ``windows_only_tests`` names those tests statically, so a
# hosted Windows step can run exactly them, and a new one joins that step without an
# edit.  A test is Windows-only when
#
# * it, or its class, is decorated ``skipUnless(P)`` where P holds only on Windows
#   (``os.name == "nt"``, ``sys.platform == "win32"``, ``sys.platform.startswith("win")``,
#   ``platform.system() == "Windows"``, a module-level name bound to one of those, the
#   negation of a host-only condition below, or an ``and`` with one of them as an operand),
#   or ``skipIf(Q)`` where Q holds on every other host (``os.name != "nt"`` and the other
#   negations, ``os.name == "posix"``, or an ``or`` with one of them as an operand);
# * a top-level ``if Q:`` statement of the test method (or of its class's ``setUp`` or
#   ``setUpClass``, for the whole class) skips (``self.skipTest(...)``, ``raise
#   unittest.SkipTest(...)``), with Q as above;
# * the module's own top level skips that way (the whole module).
#
# A skip whose condition is a tool's presence (``shutil.which("mingw32-make")``) is not
# a platform condition and is not selected.

#: Modules a hosted Windows step already runs whole, with the step; ``--windows-only``
#: leaves them out (tools/test_public_ci_wiring.py checks each step still runs its module).
WINDOWS_ONLY_RUN_ELSEWHERE = {
    "test_installer": "Run the installer tests with the pinned compiler",
}

_PLATFORM_SUBJECTS = frozenset({"os.name", "sys.platform", "platform.system()"})
_WINDOWS_VALUES = frozenset({"nt", "win32", "Windows"})


def _platform_subject(node: ast.expr) -> str | None:
    if isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name):
        name = f"{node.value.id}.{node.attr}"
        return name if name in _PLATFORM_SUBJECTS else None
    if (isinstance(node, ast.Call) and not node.args and not node.keywords
            and isinstance(node.func, ast.Attribute) and isinstance(node.func.value, ast.Name)
            and f"{node.func.value.id}.{node.func.attr}()" in _PLATFORM_SUBJECTS):
        return f"{node.func.value.id}.{node.func.attr}()"
    return None


def _platform_comparison(node: ast.expr) -> tuple[str, ast.cmpop, str] | None:
    """``(subject, operator, value)`` of ``os.name == "nt"`` and its mirror image."""
    if not (isinstance(node, ast.Compare) and len(node.ops) == 1 and len(node.comparators) == 1):
        return None
    left, right = node.left, node.comparators[0]
    for subject_node, value_node in ((left, right), (right, left)):
        subject = _platform_subject(subject_node)
        if subject and isinstance(value_node, ast.Constant) and isinstance(value_node.value, str):
            return subject, node.ops[0], value_node.value
    return None


class _PlatformConditions:
    """Classifies conditions, with the module-level names bound to a classified one."""

    def __init__(self, tree: ast.Module) -> None:
        self.windows_names: set[str] = set()
        self.other_host_names: set[str] = set()
        for node in tree.body:
            if isinstance(node, ast.Assign) and len(node.targets) == 1:
                target, value = node.targets[0], node.value
            elif isinstance(node, ast.AnnAssign) and node.value is not None:
                target, value = node.target, node.value
            else:
                continue
            if isinstance(target, ast.Name):
                if self.only_on_windows(value):
                    self.windows_names.add(target.id)
                elif self.on_every_other_host(value):
                    self.other_host_names.add(target.id)

    def only_on_windows(self, node: ast.expr) -> bool:
        """True when ``node`` can hold only on a Windows host."""
        comparison = _platform_comparison(node)
        if comparison:
            _, op, value = comparison
            return isinstance(op, ast.Eq) and value in _WINDOWS_VALUES
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                and node.func.attr == "startswith" and _platform_subject(node.func.value) == "sys.platform"
                and len(node.args) == 1 and isinstance(node.args[0], ast.Constant)
                and node.args[0].value == "win"):
            return True
        if isinstance(node, ast.Name):
            return node.id in self.windows_names
        if isinstance(node, ast.BoolOp) and isinstance(node.op, ast.And):
            return any(self.only_on_windows(value) for value in node.values)
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Not):
            return self.on_every_other_host(node.operand)
        return False

    def on_every_other_host(self, node: ast.expr) -> bool:
        """True when ``node`` holds on every host that is not Windows."""
        comparison = _platform_comparison(node)
        if comparison:
            subject, op, value = comparison
            if isinstance(op, ast.NotEq):
                return value in _WINDOWS_VALUES
            return isinstance(op, ast.Eq) and subject == "os.name" and value == "posix"
        if isinstance(node, ast.Name):
            return node.id in self.other_host_names
        if isinstance(node, ast.BoolOp) and isinstance(node.op, ast.Or):
            return any(self.on_every_other_host(value) for value in node.values)
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Not):
            return self.only_on_windows(node.operand)
        return False

    def windows_only_decorator(self, decorators: list[ast.expr]) -> bool:
        for decorator in decorators:
            if not (isinstance(decorator, ast.Call) and decorator.args):
                continue
            func = decorator.func
            name = func.attr if isinstance(func, ast.Attribute) else func.id if isinstance(func, ast.Name) else ""
            if name == "skipUnless" and self.only_on_windows(decorator.args[0]):
                return True
            if name == "skipIf" and self.on_every_other_host(decorator.args[0]):
                return True
        return False

    def windows_only_body(self, body: list[ast.stmt]) -> bool:
        """True when a top-level ``if`` in ``body`` skips on every host but Windows."""
        for statement in body:
            if not (isinstance(statement, ast.If) and statement.body
                    and self.on_every_other_host(statement.test)):
                continue
            first = statement.body[0]
            if isinstance(first, ast.Raise) and first.exc is not None:
                raised = first.exc.func if isinstance(first.exc, ast.Call) else first.exc
                if (isinstance(raised, ast.Attribute) and raised.attr == "SkipTest") or (
                        isinstance(raised, ast.Name) and raised.id == "SkipTest"):
                    return True
            if (isinstance(first, ast.Expr) and isinstance(first.value, ast.Call)
                    and isinstance(first.value.func, ast.Attribute) and first.value.func.attr == "skipTest"):
                return True
        return False


def windows_only_tests(modules: Iterable[str], tools_dir: Path = TOOLS_DIR) -> list[str]:
    """Return the unittest names (``module``, ``module.Class`` or ``module.Class.method``) of
    the Windows-only tests in ``modules``, sorted; a test inside a selected class or module is
    not named again."""

    selected: list[str] = []
    for module in sorted(modules):
        path = tools_dir / f"{module}.py"
        try:
            tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        except (OSError, UnicodeDecodeError, SyntaxError, ValueError) as exc:
            raise ShardDataError(f"cannot read {path.name} for the Windows-only selection: {exc}") from exc
        conditions = _PlatformConditions(tree)
        if conditions.windows_only_body(tree.body):
            selected.append(module)
            continue
        for node in tree.body:
            if not isinstance(node, ast.ClassDef):
                continue
            methods = [item for item in node.body if isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef))]
            setup = [item for item in methods if item.name in ("setUp", "setUpClass")]
            if conditions.windows_only_decorator(node.decorator_list) or any(
                    conditions.windows_only_body(item.body) for item in setup):
                selected.append(f"{module}.{node.name}")
                continue
            for item in methods:
                if item.name.startswith("test") and (
                        conditions.windows_only_decorator(item.decorator_list)
                        or conditions.windows_only_body(item.body)):
                    selected.append(f"{module}.{node.name}.{item.name}")
    return selected


def _plan_summary(plan: ShardPlan) -> dict[str, object]:
    return {
        "num_shards": len(plan.shards),
        "unmeasured": list(plan.unmeasured),
        "shards": [
            {"index": index, "estimated_seconds": load, "modules": list(shard)}
            for index, (shard, load) in enumerate(zip(plan.shards, plan.loads, strict=True))
        ],
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--shard", type=int, help="print this shard's module names, one per line")
    mode.add_argument("--plan", action="store_true", help="print the whole plan as JSON")
    mode.add_argument("--check", action="store_true", help="validate the weight data and the plan")
    mode.add_argument(
        "--windows-only",
        action="store_true",
        help="print the Windows-only tests no other hosted Windows step runs, one unittest name per line",
    )
    parser.add_argument("--num-shards", type=int, default=DEFAULT_NUM_SHARDS)
    parser.add_argument(
        "--select",
        choices=SELECTIONS,
        default="all",
        help="'all' plans every module; 'makefile' plans only the Makefile-coupled ones (#702)",
    )
    parser.add_argument("--weights", type=Path, default=DEFAULT_WEIGHTS)
    parser.add_argument("--tools-dir", type=Path, default=TOOLS_DIR, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)

    if args.windows_only:
        try:
            modules = [module for module in discover_modules(args.tools_dir)
                       if module not in WINDOWS_ONLY_RUN_ELSEWHERE]
            tests = windows_only_tests(modules, args.tools_dir)
            if not tests:
                raise ShardDataError("the Windows-only selection is empty")
        except ShardDataError as exc:
            print(f"ci_test_shards: {exc}", file=sys.stderr)
            return 2
        print(f"ci_test_shards: {len(tests)} Windows-only tests", file=sys.stderr)
        for test in tests:
            print(test)
        return 0

    try:
        data = load_weight_data(args.weights)
        modules = tracked = discover_modules(args.tools_dir)
        if not modules:
            raise ShardDataError(f"no {TEST_GLOB} modules found under {args.tools_dir}")
        if args.select == "makefile":
            modules = makefile_coupled_modules(modules, args.tools_dir)
            if not modules:
                raise ShardDataError("the Makefile-coupled selection is empty")
        plan = plan_shards(modules, data, args.num_shards)
    except ShardDataError as exc:
        print(f"ci_test_shards: {exc}", file=sys.stderr)
        return 2

    if args.shard is not None:
        if not 0 <= args.shard < args.num_shards:
            print(f"ci_test_shards: --shard must be in [0, {args.num_shards})", file=sys.stderr)
            return 2
        # Stale or unmeasured data never stops a shard from running its tests (the
        # --check gate owns that); a plan that drops, duplicates or co-locates
        # modules, or leaves this shard empty, does.
        blocking = [
            error
            for error in check_plan(modules, data, plan, tracked=tracked)
            if error.startswith(("plan ", f"shard {args.shard} "))
        ]
        if blocking:
            for error in blocking:
                print(f"ci_test_shards: {error}", file=sys.stderr)
            return 2
        shard = plan.shards[args.shard]
        print(
            f"ci_test_shards: shard {args.shard} of {args.num_shards} ({args.select}): {len(shard)} modules, "
            f"estimated {plan.loads[args.shard]:.1f}s "
            f"(shard estimates: {', '.join(f'{load:.1f}s' for load in plan.loads)})",
            file=sys.stderr,
        )
        for module in shard:
            print(module)
        return 0

    if args.plan:
        print(json.dumps(_plan_summary(plan), indent=2))
        return 0

    errors = check_plan(modules, data, plan, tracked=tracked)
    for index, (shard, load) in enumerate(zip(plan.shards, plan.loads, strict=True)):
        print(f"shard {index}: {len(shard)} modules, estimated {load:.1f}s")
    if plan.unmeasured:
        print(
            f"{len(plan.unmeasured)} unmeasured modules use default_seconds={data.default_seconds:g}: "
            + ", ".join(plan.unmeasured)
        )
    for error in errors:
        print(f"ERROR: {error}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
