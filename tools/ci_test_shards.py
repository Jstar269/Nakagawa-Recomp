# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Deterministic cost-weighted partition of the Python test modules into CI shards.

The hosted ``python_tools`` job runs the ``tools/test_*.py`` modules on several
runners.  Alphabetical round-robin put the heaviest modules on the same runner by
accident, so one shard set the job's wall clock while the others idled.  This tool
replaces it with Longest-Processing-Time-first (LPT) bin packing over measured
durations of units:

1. a unit is a module, or, for a module the data file lists under ``split``, one of
   its test classes, named ``MODULE.CLASS`` (a name ``python -m unittest`` accepts
   where it accepts a module name); a split module is never scheduled whole;
2. every unit gets a weight -- its measured seconds from
   ``tools/ci_test_weights.json``, or that file's ``default_seconds`` when the unit
   has not been measured yet (a new test therefore never needs a data edit to run);
3. units are taken heaviest first, ties broken by name, and each goes to the
   least-loaded shard, ties broken by the lowest shard index;
4. a unit whose module belongs to a ``separate`` group never joins a shard that
   already holds a unit of another member of that group (units of one module may
   share a shard).

The partition depends only on the discovered module names, the split modules'
source text and the data file, so every runner computes the same plan independently
and no coordination is needed.  Each shard's units are printed in name order,
matching the order unittest used for the round-robin lists, so moving a unit between
shards never changes the relative order of the units that share a runner.

Usage::

    python tools/ci_test_shards.py --shard 0          # unit names for shard 0
    python tools/ci_test_shards.py --plan             # whole plan with estimates
    python tools/ci_test_shards.py --check            # validate the data and plan

``--check`` fails (exit 1) when the data names a module that no longer exists, a
split entry is not an existing test module, a ``MODULE.CLASS`` weight names a module
that is not split or a class it does not have, a split module also has a module-level
weight, or the plan does not schedule every unit exactly once (or schedules a split
module whole).  A weight that is not a positive finite number, a malformed or
repeated split entry, a separation group that is malformed or larger than the shard
count, and a split module whose classes cannot be read (see
``split_test_classes``) are data errors.  Unmeasured units are reported but are not an
error.  Any usage or data error exits 2 without printing a unit list, so a caller that
captures the list can never mistake a failure for an empty shard.
"""

from __future__ import annotations

import argparse
import ast
import builtins
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
DEFAULT_NUM_SHARDS = 6  # tools/ci_paths.py PYTHON_SHARDS_FULL; the tests keep them equal
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
    split: tuple[str, ...] = ()


@dataclass(frozen=True)
class ShardPlan:
    shards: tuple[tuple[str, ...], ...]
    loads: tuple[float, ...]
    unmeasured: tuple[str, ...]


_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def unit_module(unit: str) -> str:
    """The module a unit belongs to: the unit itself, or the part before ``.CLASS``."""

    return unit.split(".", 1)[0]


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
    unknown_keys = sorted(set(raw) - {"description", "default_seconds", "separate", "split", "weights"})
    if unknown_keys:
        raise ShardDataError(f"weight data has unknown keys: {', '.join(unknown_keys)}")
    default_seconds = _positive_seconds(raw.get("default_seconds"), "default_seconds")

    weights_raw = raw.get("weights")
    if not isinstance(weights_raw, dict) or not weights_raw:
        raise ShardDataError("weights must be a non-empty object of module -> seconds")
    weights: dict[str, float] = {}
    for module, seconds in weights_raw.items():
        # A key is a module name, or MODULE.CLASS for a class of a split module.
        parts = module.split(".") if isinstance(module, str) else []
        if (not isinstance(module, str) or not module.startswith("test_") or not 1 <= len(parts) <= 2
                or not all(_IDENTIFIER.fullmatch(part) for part in parts)):
            raise ShardDataError(f"weights key {module!r} is not a test module name or MODULE.CLASS")
        weights[module] = _positive_seconds(seconds, f"weights[{module!r}]")

    split_raw = raw.get("split", [])
    if not isinstance(split_raw, list) or not all(
            isinstance(name, str) and name.startswith("test_") and _IDENTIFIER.fullmatch(name)
            for name in split_raw):
        raise ShardDataError("split must be a list of test module names")
    if len(set(split_raw)) != len(split_raw):
        repeated = sorted({name for name in split_raw if split_raw.count(name) > 1})
        raise ShardDataError(f"split repeats a module: {', '.join(repeated)}")

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
    return WeightData(default_seconds, weights, tuple(separate), tuple(split_raw))


def load_weight_data(path: Path = DEFAULT_WEIGHTS) -> WeightData:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise ShardDataError(f"cannot read weight data {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise ShardDataError(f"weight data {path} is not valid JSON: {exc}") from exc
    return parse_weight_data(raw)


def plan_shards(units: Iterable[str], data: WeightData, num_shards: int) -> ShardPlan:
    """Partition ``units`` (module names and ``MODULE.CLASS`` names, see
    ``expand_units``) into ``num_shards`` shards with constrained LPT.

    Raises ``ShardDataError`` when the request cannot be satisfied: no shards, a
    duplicated unit name, or a separation group with more present members than
    there are shards.
    """

    if isinstance(num_shards, bool) or not isinstance(num_shards, int) or num_shards < 1:
        raise ShardDataError(f"num_shards must be a positive integer, got {num_shards!r}")
    unit_list = list(units)
    if len(set(unit_list)) != len(unit_list):
        raise ShardDataError("unit list contains duplicates")
    present = {unit_module(unit) for unit in unit_list}

    groups_of: dict[str, list[frozenset[str]]] = {}
    for group in data.separate:
        members = group & present
        if len(members) > num_shards:
            raise ShardDataError(
                f"separation group {sorted(group)} needs {len(members)} shards, only {num_shards} exist"
            )
        for member in members:
            groups_of.setdefault(member, []).append(members)

    def weight(unit: str) -> float:
        return data.weights.get(unit, data.default_seconds)

    shards: list[list[str]] = [[] for _ in range(num_shards)]
    loads = [0.0] * num_shards
    for unit in sorted(unit_list, key=lambda name: (-weight(name), name)):
        module = unit_module(unit)
        blocked: set[int] = set()
        for members in groups_of.get(module, ()):
            others = members - {module}
            for index, shard in enumerate(shards):
                if any(unit_module(placed) in others for placed in shard):
                    blocked.add(index)
        candidates = [index for index in range(num_shards) if index not in blocked]
        # Unreachable while every group fits in num_shards (checked above): a group
        # of k members blocks at most k - 1 shards before its last member lands.
        if not candidates:
            raise ShardDataError(f"no shard can accept {unit} without breaking a separation group")
        target = min(candidates, key=lambda index: (loads[index], index))
        shards[target].append(unit)
        loads[target] += weight(unit)

    unmeasured = tuple(sorted(unit for unit in unit_list if unit not in data.weights))
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
    split_classes: Mapping[str, Sequence[str]] | None = None,
) -> list[str]:
    """Return every reason the data or plan is not acceptable for CI (empty when clean).

    ``modules`` are the planned units (``expand_units``); ``tracked`` (default: the
    modules of the units) are every module that exists, against which the data is
    checked for stale names; ``split_classes`` (default: read from the units) maps each
    existing split module to its test classes, against which ``MODULE.CLASS`` weights
    are checked.
    """

    errors: list[str] = []
    present = set(modules)
    existing = set(tracked if tracked is not None else (unit_module(unit) for unit in modules))
    if split_classes is None:
        derived: dict[str, list[str]] = {}
        for unit in modules:
            if "." in unit:
                derived.setdefault(unit_module(unit), []).append(unit.split(".", 1)[1])
        split_classes = derived
    split = set(data.split)
    stale = sorted(key for key in data.weights if unit_module(key) not in existing)
    if stale:
        errors.append("weights name modules that no longer exist: " + ", ".join(stale))
    missing_split = sorted(split - existing)
    if missing_split:
        errors.append("split names modules that are not test modules: " + ", ".join(missing_split))
    whole = sorted(key for key in data.weights if key in split)
    if whole:
        errors.append("weights give a module-level weight to split modules: " + ", ".join(whole))
    class_keys = [key for key in data.weights if "." in key and unit_module(key) in existing]
    unsplit = sorted(key for key in class_keys if unit_module(key) not in split)
    if unsplit:
        errors.append("weights name classes of modules that are not split: " + ", ".join(unsplit))
    absent = sorted(key for key in class_keys if unit_module(key) in split
                    and key.split(".", 1)[1] not in split_classes.get(unit_module(key), ()))
    if absent:
        errors.append("weights name test classes that do not exist: " + ", ".join(absent))
    for group in data.separate:
        missing = sorted(group - existing)
        if missing:
            errors.append(f"separation group {sorted(group)} names missing modules: {', '.join(missing)}")
    scheduled = [unit for shard in plan.shards for unit in shard]
    if sorted(scheduled) != sorted(modules):
        duplicated = sorted({unit for unit in scheduled if scheduled.count(unit) > 1})
        dropped = sorted(present - set(scheduled))
        extra = sorted(set(scheduled) - present)
        errors.append(
            f"plan does not schedule every unit exactly once (duplicated={duplicated}, "
            f"dropped={dropped}, unknown={extra})"
        )
    scheduled_whole = sorted(split.intersection(scheduled))
    if scheduled_whole:
        errors.append("plan schedules split modules whole: " + ", ".join(scheduled_whole))
    for group in data.separate:
        for index, shard in enumerate(plan.shards):
            together = sorted(group.intersection(unit_module(unit) for unit in shard))
            if len(together) > 1:
                errors.append(f"plan puts separated modules together on shard {index}: {', '.join(together)}")
    if len(modules) >= len(plan.shards):
        for index, shard in enumerate(plan.shards):
            if not shard:
                errors.append(f"shard {index} is empty")
    return errors


# ---- Class units of split modules --------------------------------------------------
#
# A split module runs as one unit per test class, so the classes must be exactly the
# ones unittest's loader finds in the module (``loadTestsFromModule``: every module
# attribute that is a ``unittest.TestCase`` subclass).  They are read from the source
# with ``ast`` and never by importing it, so every runner computes the same plan from the
# tracked bytes.  A test class is a ``class`` statement in the module body whose base,
# decided statically, is ``unittest.TestCase`` (or ``IsolatedAsyncioTestCase``) through
# the module's own imports (``import unittest``, ``from unittest import TestCase``, any
# ``as`` name), or another class of the module that is one, transitively.  A class whose
# bases are all such classes that are not tests, or builtins the module does not rebind
# (``object``, ``Exception``), is a helper and is ignored.
#
# The reading fails closed, naming the module and the construct, on anything it cannot
# decide or that would let the loader see a different set of tests:
#
# * a base it cannot resolve inside the module (an imported class other than unittest's
#   test cases, a call or subscript, a starred base, a ``**`` keyword): it may or may not
#   be a TestCase;
# * a class decorator other than unittest's skip family, unittest.mock's patch family or
#   ``dataclasses.dataclass``: a decorator can replace the class;
# * a ``load_tests`` function (the loader would call it instead of collecting classes),
#   and a module-level ``__getattr__`` or ``__dir__`` (they change the attributes the
#   loader sees);
# * a ``class`` statement inside a module-level ``if``, ``try``, ``with``, ``for``,
#   ``while`` or ``match`` block: whether it exists depends on running the module;
# * a star import: it can bring in any TestCase;
# * a module-level binding that rebinds a class name (a second ``class`` statement, an
#   assignment, ``del``, an import, a ``global`` in a function) or rebinds a name the base
#   or decorator resolution relies on; and an assignment, walrus or ``for`` loop that
#   binds a second name to a test class, which the loader would run twice.
#
# What it does not see: a name imported from another module is not read, so an imported
# TestCase subclass would join the loader's collection but no unit.  Nor can it see
# dynamic bindings (``globals()``, ``setattr`` on the module).  The drift guard in
# tools/test_ci_test_shards.py loads every split module both ways on the live tree and
# fails on any difference, so such a module fails CI before it lands.

_TEST_BASES = frozenset({"unittest.TestCase", "unittest.IsolatedAsyncioTestCase"})
_CLASS_DECORATORS = frozenset({
    "unittest.skip", "unittest.skipIf", "unittest.skipUnless", "unittest.expectedFailure",
    "unittest.mock.patch", "unittest.mock.patch.object", "unittest.mock.patch.dict",
    "unittest.mock.patch.multiple", "dataclasses.dataclass",
})
_MODULE_HOOKS = {
    "load_tests": "a load_tests function (unittest's loader would call it instead of collecting classes)",
    "__getattr__": "a module-level __getattr__ (it changes the attributes unittest's loader sees)",
    "__dir__": "a module-level __dir__ (it changes the attributes unittest's loader sees)",
}
_BLOCKS: dict[type[ast.AST], str] = {
    ast.If: "if", ast.Try: "try", ast.TryStar: "try", ast.With: "with", ast.AsyncWith: "async with",
    ast.For: "for", ast.AsyncFor: "async for", ast.While: "while", ast.Match: "match",
}
_REBINDINGS = {
    "assign": "an assignment", "del": "a del statement", "import": "an import", "def": "a def statement",
    "global": "a global statement in a function", "for": "a for loop target", "with": "a with target",
    "except": "an except target", "match": "a match capture",
}
_BUILTIN_TYPES = frozenset(name for name, value in vars(builtins).items() if isinstance(value, type))


def _bound_names(target: ast.AST) -> list[str]:
    """The names an assignment, ``for`` or ``with`` target, or a ``del``, binds."""

    if isinstance(target, ast.Name):
        return [target.id]
    if isinstance(target, ast.Starred):
        return _bound_names(target.value)
    if isinstance(target, (ast.Tuple, ast.List)):
        return [name for element in target.elts for name in _bound_names(element)]
    return []  # an attribute or subscript target binds no module name


def _pattern_names(pattern: ast.AST) -> list[str]:
    names = []
    for node in ast.walk(pattern):
        if isinstance(node, (ast.MatchAs, ast.MatchStar)) and node.name:
            names.append(node.name)
        elif isinstance(node, ast.MatchMapping) and node.rest:
            names.append(node.rest)
    return names


def _module_level_nodes(body: list[ast.stmt]) -> Iterable[tuple[ast.AST, str | None]]:
    """Every node that runs at module level, with the keyword of the innermost
    module-level block around it (None: directly in the module body).

    Function, class and lambda bodies are not entered (their own name is still yielded
    through the defining statement)."""

    stack: list[tuple[ast.AST, str | None]] = [(statement, None) for statement in reversed(body)]
    while stack:
        node, block = stack.pop()
        yield node, block
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef, ast.Lambda)):
            # Decorators, bases and defaults run at module level; the body does not.
            outer: list[ast.AST] = list(getattr(node, "decorator_list", []))
            if isinstance(node, ast.ClassDef):
                outer += [*node.bases, *(keyword.value for keyword in node.keywords)]
            else:
                outer += [*node.args.defaults, *(d for d in node.args.kw_defaults if d is not None)]
            stack.extend((child, block) for child in reversed(outer))
            continue
        # Everything inside a block (its handlers and cases included) is inside that block.
        inner = _BLOCKS.get(type(node), block)
        stack.extend((child, inner) for child in reversed(list(ast.iter_child_nodes(node))))


def split_test_classes(path: Path) -> list[str]:
    """Return the names of ``path``'s unittest.TestCase classes in source order, read
    statically; raise ``ShardDataError`` naming the module and the construct when the
    reading cannot decide them (see the comment above)."""

    module = path.stem
    try:
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    except (OSError, UnicodeDecodeError, SyntaxError, ValueError) as exc:
        raise ShardDataError(f"cannot read {path.name} for its test classes: {exc}") from exc

    def refuse(node: ast.AST, construct: str) -> ShardDataError:
        return ShardDataError(
            f"{module} cannot be split into test classes: line {getattr(node, 'lineno', '?')}: {construct}")

    # Every module-level binding: name -> [(kind, import source or None, node)].
    bindings: dict[str, list[tuple[str, str | None, ast.AST]]] = {}

    def bind(name: str, kind: str, node: ast.AST, source: str | None = None) -> None:
        bindings.setdefault(name, []).append((kind, source, node))

    classes: list[ast.ClassDef] = []
    second_names: list[tuple[ast.AST, ast.AST]] = []  # (binding statement, bound value)
    for node, block in _module_level_nodes(tree.body):
        if isinstance(node, ast.ClassDef):
            if block:
                raise refuse(node, f"class {node.name} is defined inside a module-level {block} block")
            classes.append(node)
            bind(node.name, "class", node)
        elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            bind(node.name, "def", node)
        elif isinstance(node, ast.Import):
            for alias in node.names:
                if alias.asname:
                    bind(alias.asname, "import", node, alias.name)
                else:
                    first = alias.name.split(".")[0]
                    bind(first, "import", node, first)
        elif isinstance(node, ast.ImportFrom):
            for alias in node.names:
                if alias.name == "*":
                    raise refuse(node, f"a star import from {node.module or '.'}")
                source = f"{node.module}.{alias.name}" if node.module and not node.level else None
                bind(alias.asname or alias.name, "import", node, source)
        elif isinstance(node, (ast.Assign, ast.AnnAssign, ast.AugAssign)):
            targets: list[ast.expr] = list(node.targets) if isinstance(node, ast.Assign) else [node.target]
            for target in targets:
                for name in _bound_names(target):
                    bind(name, "assign", node)
            if node.value is not None:
                second_names.append((node, node.value))
        elif isinstance(node, ast.NamedExpr):
            bind(node.target.id, "assign", node)
            second_names.append((node, node.value))
        elif isinstance(node, ast.Delete):
            for target in node.targets:
                for name in _bound_names(target):
                    bind(name, "del", node)
        elif isinstance(node, (ast.For, ast.AsyncFor)):
            for name in _bound_names(node.target):
                bind(name, "for", node)
            second_names.append((node, node.iter))
        elif isinstance(node, (ast.With, ast.AsyncWith)):
            for item in node.items:
                if item.optional_vars is not None:
                    for name in _bound_names(item.optional_vars):
                        bind(name, "with", node)
        elif isinstance(node, ast.ExceptHandler) and node.name:
            bind(node.name, "except", node)
        elif isinstance(node, ast.match_case):
            for name in _pattern_names(node.pattern):
                bind(name, "match", node)
    for node in ast.walk(tree):
        if isinstance(node, ast.Global):
            for name in node.names:
                bind(name, "global", node)
    for name, construct in _MODULE_HOOKS.items():
        if name in bindings:
            raise refuse(bindings[name][0][2], construct)

    def imported(name: str) -> str | None:
        """The import source of a name every binding of which is that one import."""
        found = bindings.get(name, [])
        sources = {source for kind, source, _node in found}
        if found and all(kind == "import" for kind, _source, _node in found) and len(sources) == 1:
            return next(iter(sources))
        return None

    def dotted(expr: ast.expr) -> str | None:
        if isinstance(expr, ast.Name):
            return imported(expr.id)
        if isinstance(expr, ast.Attribute):
            root = dotted(expr.value)
            return f"{root}.{expr.attr}" if root else None
        return None

    by_name: dict[str, ast.ClassDef] = {}
    for node in classes:
        others = [kind for kind, _source, _node in bindings[node.name] if kind != "class"]
        if len(bindings[node.name]) > 1:
            what = "a second class statement" if not others else _REBINDINGS[others[0]]
            raise refuse(node, f"class name {node.name} is rebound ({what})")
        by_name[node.name] = node

    verdicts: dict[str, bool] = {}

    def is_test(node: ast.ClassDef, seen: frozenset[str] = frozenset()) -> bool:
        if node.name in verdicts:
            return verdicts[node.name]
        if node.name in seen:
            raise refuse(node, f"class {node.name} inherits from itself")
        if any(keyword.arg is None for keyword in node.keywords):
            raise refuse(node, f"class {node.name} takes ** keywords")
        verdict = False
        for base in node.bases:
            if isinstance(base, ast.Name) and base.id in by_name:
                verdict = is_test(by_name[base.id], seen | {node.name}) or verdict
            elif dotted(base) in _TEST_BASES:
                verdict = True
            elif isinstance(base, ast.Name) and base.id in _BUILTIN_TYPES and base.id not in bindings:
                continue
            else:
                raise refuse(base, f"base {ast.unparse(base)} of class {node.name} cannot be resolved "
                                   "inside the module")
        for decorator in node.decorator_list:
            target = decorator.func if isinstance(decorator, ast.Call) else decorator
            if dotted(target) not in _CLASS_DECORATORS:
                raise refuse(decorator, f"decorator {ast.unparse(target)} on class {node.name} may "
                                        "replace the class")
        verdicts[node.name] = verdict
        return verdict

    tests = [node.name for node in classes if is_test(node)]
    test_names = set(tests)
    for statement, value in second_names:
        elements = value.elts if isinstance(value, (ast.Tuple, ast.List, ast.Set)) else [value]
        for element in elements:
            if isinstance(element, ast.Name) and element.id in test_names:
                raise refuse(statement, f"a module-level binding gives test class {element.id} a second name")
    return tests


def expand_units(modules: Iterable[str], data: WeightData, tools_dir: Path = TOOLS_DIR) -> list[str]:
    """Return the sorted planning units of ``modules``: a split module contributes
    ``MODULE.CLASS`` for each of its test classes, every other module itself."""

    units: list[str] = []
    for module in modules:
        if module not in data.split:
            units.append(module)
            continue
        classes = split_test_classes(tools_dir / f"{module}.py")
        if not classes:
            raise ShardDataError(f"{module} is listed under split but defines no unittest.TestCase class")
        units.extend(f"{module}.{name}" for name in classes)
    return sorted(units)


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
            {"index": index, "estimated_seconds": load, "units": list(shard)}
            for index, (shard, load) in enumerate(zip(plan.shards, plan.loads, strict=True))
        ],
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--shard", type=int, help="print this shard's unit names, one per line")
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
        # A selected split module contributes all its class units.
        units = expand_units(modules, data, args.tools_dir)
        plan = plan_shards(units, data, args.num_shards)
    except ShardDataError as exc:
        print(f"ci_test_shards: {exc}", file=sys.stderr)
        return 2

    if args.shard is not None:
        if not 0 <= args.shard < args.num_shards:
            print(f"ci_test_shards: --shard must be in [0, {args.num_shards})", file=sys.stderr)
            return 2
        # Stale or unmeasured data never stops a shard from running its tests (the
        # --check gate owns that); a plan that drops, duplicates or co-locates
        # units, schedules a split module whole, or leaves this shard empty, does.
        blocking = [
            error
            for error in check_plan(units, data, plan, tracked=tracked)
            if error.startswith(("plan ", f"shard {args.shard} "))
        ]
        if blocking:
            for error in blocking:
                print(f"ci_test_shards: {error}", file=sys.stderr)
            return 2
        shard = plan.shards[args.shard]
        print(
            f"ci_test_shards: shard {args.shard} of {args.num_shards} ({args.select}): {len(shard)} units, "
            f"estimated {plan.loads[args.shard]:.1f}s "
            f"(shard estimates: {', '.join(f'{load:.1f}s' for load in plan.loads)})",
            file=sys.stderr,
        )
        for unit in shard:
            print(unit)
        return 0

    if args.plan:
        print(json.dumps(_plan_summary(plan), indent=2))
        return 0

    try:
        # Every existing split module, selected or not, so a class weight is checked against it.
        split_classes = {module: split_test_classes(args.tools_dir / f"{module}.py")
                         for module in data.split if module in tracked}
    except ShardDataError as exc:
        print(f"ci_test_shards: {exc}", file=sys.stderr)
        return 2
    errors = check_plan(units, data, plan, tracked=tracked, split_classes=split_classes)
    for index, (shard, load) in enumerate(zip(plan.shards, plan.loads, strict=True)):
        print(f"shard {index}: {len(shard)} units, estimated {load:.1f}s")
    if plan.unmeasured:
        print(
            f"{len(plan.unmeasured)} unmeasured units use default_seconds={data.default_seconds:g}: "
            + ", ".join(plan.unmeasured)
        )
    for error in errors:
        print(f"ERROR: {error}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
