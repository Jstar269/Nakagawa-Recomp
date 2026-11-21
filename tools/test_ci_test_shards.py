# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Regression tests for the cost-weighted CI test sharding tool (#693)."""

from __future__ import annotations

import ast
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from typing import ClassVar, cast

try:
    import ci_paths
    import ci_test_shards
except ModuleNotFoundError:  # Running as ``python -m unittest tools.test_ci_test_shards``.
    import tools.ci_paths as ci_paths
    import tools.ci_test_shards as ci_test_shards


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "ci_test_shards.py"
WORKFLOW = ROOT / ".github" / "workflows" / "ci.yml"
MUTATING_PAIR = frozenset({"test_title_catalog", "test_publication_policy_gate"})


def _data(weights: dict[str, float], *, default: float = 0.5, separate: list[list[str]] | None = None):
    return ci_test_shards.parse_weight_data(
        {"default_seconds": default, "weights": weights, "separate": separate or []}
    )


def _run_cli(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(TOOL), *args],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )


def _install_root(start: Path) -> Path | None:
    """The Git for Windows root at or above start: the directory that holds git-bash.exe."""
    return next((parent for parent in (start, *start.parents)
                 if (parent / "git-bash.exe").is_file()), None)


def _git_for_windows_roots() -> list[Path]:
    """Candidate Git for Windows installation roots, most specific first."""
    roots: list[Path] = []
    git = shutil.which("git")
    if git:
        # The install that owns the git on PATH. A launcher, shim or package-manager link may
        # live outside the install, so ask git for its own exec path first (it lies inside
        # the install), then fall back to the resolved executable's location.
        try:
            exec_path = subprocess.run([git, "--exec-path"], capture_output=True, text=True,
                                       check=False, timeout=30).stdout.strip()
        except (OSError, subprocess.TimeoutExpired):
            exec_path = ""
        for start in ((Path(exec_path),) if exec_path else ()) + (Path(git).resolve().parent,):
            owner = _install_root(start)
            if owner is not None and owner not in roots:
                roots.append(owner)
    for variable in ("ProgramW6432", "ProgramFiles", "ProgramFiles(x86)"):
        if os.environ.get(variable):
            roots.append(Path(os.environ[variable]) / "Git")
    if os.environ.get("LOCALAPPDATA"):
        roots.append(Path(os.environ["LOCALAPPDATA"]) / "Programs" / "Git")
    return list(dict.fromkeys(roots))  # first occurrence wins; one install may be found twice


def _bash_path(path: str | Path) -> str:
    """path as the step's bash sees it. Git for Windows bash mounts each drive at /<drive>;
    a Windows path in a ':'-separated PATH would split at its drive colon."""
    text = str(path)
    if os.name != "nt" or len(text) < 2 or text[1] != ":":
        return text
    return "/" + text[0].lower() + text[2:].replace("\\", "/")


def _step_bash() -> tuple[str | None, str]:
    """The bash that runs the workflow step, or None with the reason there is none.

    The hosted runner executes the step with a POSIX bash. On Windows the bash named
    on PATH may be the WSL launcher (C:\\Windows\\System32\\bash.exe), which runs in
    Linux and cannot see the test's Windows scratch paths, so the Git for Windows bash
    is located explicitly instead.
    """
    if os.name != "nt":
        found = shutil.which("bash")
        return found, "" if found else "the workflow step is a bash script and no bash is on PATH"
    checked = []
    for root in _git_for_windows_roots():
        candidate = root / "bin" / "bash.exe"
        if candidate.is_file():
            return str(candidate), ""
        checked.append(str(candidate))
    return None, ("no Git for Windows bash found for the workflow step (checked "
                  + (", ".join(checked) if checked else "no Git install location known") + ")")


class LiveRepositoryPlanTests(unittest.TestCase):
    """The committed data and the tracked test modules, exactly as CI consumes them."""

    modules: ClassVar[list[str]]
    data: ClassVar[ci_test_shards.WeightData]
    python_job: ClassVar[str]
    num_shards: ClassVar[int]

    @classmethod
    def setUpClass(cls) -> None:
        cls.modules = ci_test_shards.discover_modules()
        cls.data = ci_test_shards.load_weight_data()
        workflow = WORKFLOW.read_text(encoding="utf-8")
        cls.python_job = workflow.split("\n  python_tools:", 1)[1].split("\n  native_tools:", 1)[0]
        cls.num_shards = ci_paths.PYTHON_SHARDS_FULL

    def run_shards(self, num_shards: int, select: str) -> list[str]:
        scheduled: list[str] = []
        for shard in range(num_shards):
            with self.subTest(shard=shard, select=select):
                result = _run_cli("--shard", str(shard), "--num-shards", str(num_shards), "--select", select)
                self.assertEqual(result.returncode, 0, result.stderr)
                names = result.stdout.splitlines()
                self.assertTrue(names, f"shard {shard} is empty")
                scheduled.extend(names)
        self.assertEqual(len(scheduled), len(set(scheduled)), "a module is scheduled on two shards")
        return scheduled

    def test_every_test_module_runs_exactly_once_across_the_cli_shards(self) -> None:
        scheduled = self.run_shards(self.num_shards, "all")
        expected = sorted(path.stem for path in (ROOT / "tools").glob("test_*.py"))
        self.assertEqual(sorted(scheduled), expected)
        self.assertIn("test_ci_test_shards", scheduled)

    def test_makefile_selection_runs_each_coupled_module_exactly_once(self) -> None:
        scheduled = self.run_shards(ci_paths.PYTHON_SHARDS_MAKEFILE, "makefile")
        coupled = ci_test_shards.makefile_coupled_modules(self.modules)
        self.assertEqual(sorted(scheduled), coupled)
        # Modules that read the Makefile text or build through make must be in it.
        for module in ("test_title_runtime_config", "test_build_truth", "test_public_ci_wiring",
                       "test_ci_paths", "test_production_smoke"):
            self.assertIn(module, coupled)

    def test_mutating_modules_are_declared_separate_and_never_share_a_shard(self) -> None:
        self.assertIn(MUTATING_PAIR, [group & MUTATING_PAIR for group in self.data.separate],
                      "the weight data must keep the tracked-state mutators in one separation group")
        plan = ci_test_shards.plan_shards(self.modules, self.data, self.num_shards)
        homes = [index for index, shard in enumerate(plan.shards) for name in shard if name in MUTATING_PAIR]
        self.assertEqual(len(homes), 2)
        self.assertNotEqual(homes[0], homes[1])

    def test_committed_data_passes_check(self) -> None:
        result = _run_cli("--check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_balance_beats_the_round_robin_it_replaces(self) -> None:
        plan = ci_test_shards.plan_shards(self.modules, self.data, self.num_shards)

        def cost(name: str) -> float:
            return cast(float, self.data.weights.get(name, self.data.default_seconds))

        round_robin = [0.0] * self.num_shards
        for index, name in enumerate(self.modules):
            round_robin[index % self.num_shards] += cost(name)
        self.assertLessEqual(max(plan.loads), max(round_robin))
        # Greedy placement bound: the module that finished the heaviest shard joined
        # it when that shard was the lightest, i.e. at most the mean load.
        bound = sum(plan.loads) / self.num_shards + max(cost(name) for name in self.modules)
        self.assertLessEqual(max(plan.loads), bound + 1e-6)

    def test_each_shard_count_is_the_fewest_at_which_the_heaviest_module_sets_the_job(self) -> None:
        """A shard cannot finish before its heaviest module, so the classifier's counts
        are the fewest shards whose slowest shard is within 5% of that module's weight:
        one shard fewer must leave a slower shard, and more would only add runners."""

        def slowest(modules: list[str], num_shards: int) -> float:
            loads: tuple[float, ...] = ci_test_shards.plan_shards(modules, self.data, num_shards).loads
            return max(loads)

        for select, modules, count in (
            ("all", self.modules, ci_paths.PYTHON_SHARDS_FULL),
            ("makefile", ci_test_shards.makefile_coupled_modules(self.modules), ci_paths.PYTHON_SHARDS_MAKEFILE),
        ):
            with self.subTest(select=select):
                floor = max(cast(float, self.data.weights.get(name, self.data.default_seconds)) for name in modules)
                self.assertLessEqual(slowest(modules, count), floor * 1.05,
                                     f"{count} shards leave the slowest shard well above its heaviest module")
                self.assertGreater(slowest(modules, count - 1), floor * 1.05,
                                   f"{count - 1} shards already reach the heaviest module; {count} wastes a runner")
        self.assertEqual(ci_test_shards.DEFAULT_NUM_SHARDS, ci_paths.PYTHON_SHARDS_FULL)

    def test_workflow_takes_its_shards_from_the_classifier_and_fails_closed(self) -> None:
        self.assertNotIn("(NR-1)%4", self.python_job, "the alphabetical round-robin must be gone")
        self.assertIn(
            "shard: ${{ fromJSON(needs.classify.outputs.python_shards || '[0, 1, 2, 3]') }}", self.python_job
        )
        fallback = re.search(r"python_shards \|\| '(\[[^']*\])'", self.python_job)
        self.assertIsNotNone(fallback)
        self.assertEqual(json.loads(cast(re.Match[str], fallback).group(1)),
                         list(range(ci_paths.PYTHON_SHARDS_FULL)))
        self.assertIn("NUM_SHARDS: ${{ needs.classify.outputs.python_num_shards }}", self.python_job)
        self.assertIn("PYTHON_SCOPE: ${{ needs.classify.outputs.python_scope }}", self.python_job)
        self.assertNotIn("< <(python tools/ci_test_shards.py", self.python_job,
                         "process substitution hides the planner's exit status")
        self.assertIn("set -euo pipefail", self.python_job)
        self.assertIn(
            'listing="$(python tools/ci_test_shards.py --shard "$SHARD" --num-shards "$NUM_SHARDS" '
            '--select "$PYTHON_SCOPE")"',
            self.python_job,
        )
        workflow = WORKFLOW.read_text(encoding="utf-8")
        classify_outputs = workflow.split("\n  classify:", 1)[1].split("\n    steps:", 1)[0]
        for output in ("python_scope", "python_num_shards", "python_shards"):
            self.assertIn(f"{output}: ${{{{ steps.paths.outputs.{output} }}}}", classify_outputs)

    def test_workflow_step_stops_on_a_failing_or_empty_planner(self) -> None:
        """Execute the step's own script with stand-in planners: a planner that fails
        or prints nothing must fail the step before unittest runs. A run whose skip summary
        counts a TOOLCHAIN_MISSING skip, or has no such count, fails after it (F-6a8c715a)."""
        bash, reason = _step_bash()
        if bash is None:
            self.skipTest(reason)
        lines = self.python_job.splitlines()
        start = next(i for i, line in enumerate(lines) if line.strip() == "run: |"
                     and any("ci_test_shards.py" in later for later in lines[i:i + 4]))
        indent = len(lines[start + 1]) - len(lines[start + 1].lstrip())
        body = []
        for line in lines[start + 1:]:
            if line.strip() and len(line) - len(line.lstrip()) < indent:
                break
            body.append(line[indent:])
        script = "\n".join(body) + "\n"
        planner_line = 'listing="$(python tools/ci_test_shards.py'
        self.assertEqual(script.count(planner_line), 1, script)

        no_skips = "Python test skip prefixes: NIGHTLY_TIER=0 WINDOWS_ONLY=0 PRIVATE_INPUT=0 TOOLCHAIN_MISSING=0"

        def run_step(step: str, planner: str,
                     summary: str = no_skips) -> tuple[subprocess.CompletedProcess[str], str | None]:
            """Run ``step`` with ``planner`` standing in for ci_test_shards.py and ``summary`` for
            skip_prefix_summary's line; the unittest modules, if it ran."""
            with tempfile.TemporaryDirectory() as scratch:
                stub = Path(scratch) / "python"
                ran = Path(scratch) / "unittest-args"
                step_summary = Path(scratch) / "step-summary.md"
                # Only the planner's invocation gets the planner's answer. Every other python
                # call succeeds, and the unittest one (under the coverage wrapper) records its
                # module arguments, so a step that goes on after a failed planner is seen.
                stub.write_text(
                    "#!/bin/sh\n"
                    'all="$*"\n'
                    'case " $all " in\n'
                    "  *skip_prefix_summary*)\n"
                    "    printf '%s\\n' '" + summary + "'\n"
                    "    exit 0\n"
                    "    ;;\n"
                    "  *ci_test_shards.py*)\n"
                    "    " + planner + "\n"
                    "    exit 0\n"
                    "    ;;\n"
                    '  *" -m unittest -v "*)\n'
                    '    modules="${all##*-m unittest -v }"\n'
                    '    printf \'%s\\n\' "$modules" > "' + _bash_path(ran) + '"\n'
                    "    ;;\n"
                    "esac\n"
                    "exit 0\n",
                    encoding="utf-8",
                )
                stub.chmod(0o755)
                env = {"PATH": f"{_bash_path(scratch)}:/usr/bin:/bin", "SHARD": "0", "NUM_SHARDS": "4",
                       "PYTHON_SCOPE": "all", "RUNNER_TEMP": _bash_path(scratch),
                       "GITHUB_STEP_SUMMARY": _bash_path(step_summary)}
                result = subprocess.run([bash, "-c", step], env=env, capture_output=True,
                                        text=True, check=False)
                modules = ran.read_text(encoding="utf-8").strip() if ran.exists() else None
            return result, modules

        listing = "printf 'test_a\\ntest_b\\n'"
        for label, planner, summary, expect_rc, expect_modules in (
            ("planner fails", "exit 2", no_skips, False, None),
            ("planner prints nothing", "exit 0", no_skips, False, None),
            ("planner lists modules", listing, no_skips, True, "test_a test_b"),
            # gcc is on every python_tools runner: a test that skipped for want of it is a gate not run.
            ("a test skipped for a missing compiler", listing,
             no_skips.replace("TOOLCHAIN_MISSING=0", "TOOLCHAIN_MISSING=2"), False, "test_a test_b"),
            ("ten tests skipped for a missing compiler", listing,
             no_skips.replace("TOOLCHAIN_MISSING=0", "TOOLCHAIN_MISSING=10"), False, "test_a test_b"),
            ("the summary has no TOOLCHAIN_MISSING count", listing,
             no_skips.replace(" TOOLCHAIN_MISSING=0", ""), False, "test_a test_b"),
        ):
            with self.subTest(label):
                result, modules = run_step(script, planner, summary)
                self.assertEqual(result.returncode == 0, expect_rc, result.stdout + result.stderr)
                if "TOOLCHAIN_MISSING=0" not in summary:
                    self.assertIn("TOOLCHAIN_MISSING", result.stderr)
                if expect_modules is None:
                    self.assertIsNone(modules, "unittest ran after a planner failure")
                else:
                    self.assertEqual(modules, expect_modules)
        # The stub must see a step that runs unittest after a failed planner. Failing-before: the
        # stub applied the planner's 'exit 2' to every python call, the unittest one included, so
        # this fallback still failed the step and never recorded the run it would make in CI.
        fallback = script.replace('--select "$PYTHON_SCOPE")"', '--select "$PYTHON_SCOPE")" || listing=test_fallback', 1)
        self.assertNotEqual(fallback, script)
        _result, modules = run_step(fallback, "exit 2")
        self.assertEqual(modules, "test_fallback", "the stub cannot see unittest running after a failed planner")

    def test_classifier_shard_outputs_match_the_planner_choices(self) -> None:
        for paths, scope in ((["tools/codegen.py"], "all"), (["Makefile"], "makefile")):
            with self.subTest(scope=scope):
                change = ("player-ui-regressions",) if scope == "makefile" else None
                result = ci_paths.classify(paths, makefile_change=change)
                self.assertEqual(result["python_scope"], scope)
                self.assertIn(result["python_scope"], ci_test_shards.SELECTIONS)
                self.assertEqual(json.loads(result["python_shards"]),
                                 list(range(int(result["python_num_shards"]))))


class PlannerTests(unittest.TestCase):
    def test_lpt_assigns_heaviest_first_to_the_least_loaded_shard(self) -> None:
        data = _data({"test_a": 7, "test_b": 6, "test_c": 5, "test_d": 4, "test_e": 3, "test_f": 3})
        plan = ci_test_shards.plan_shards(
            ["test_a", "test_b", "test_c", "test_d", "test_e", "test_f"], data, 2
        )
        # a->0, b->1, c->1 (6<7), d->0 (7<11), e->0 (11=11, lower index), f->1.
        self.assertEqual(plan.shards, (("test_a", "test_d", "test_e"), ("test_b", "test_c", "test_f")))
        self.assertEqual(plan.loads, (14.0, 14.0))

    def test_separation_overrides_the_least_loaded_choice(self) -> None:
        data = _data({"test_a": 10, "test_b": 9, "test_x": 1}, separate=[["test_x", "test_b"]])
        plan = ci_test_shards.plan_shards(["test_a", "test_b", "test_x"], data, 2)
        # Unconstrained, test_x would join test_b on the lighter shard 1.
        self.assertEqual(plan.shards, (("test_a", "test_x"), ("test_b",)))
        self.assertEqual(plan.loads, (11.0, 9.0))
        unconstrained = ci_test_shards.plan_shards(["test_a", "test_b", "test_x"], _data(data.weights), 2)
        self.assertEqual(unconstrained.shards, (("test_a",), ("test_b", "test_x")))

    def test_unknown_modules_get_the_default_weight_and_are_reported(self) -> None:
        data = _data({"test_a": 2.0}, default=0.25)
        plan = ci_test_shards.plan_shards(["test_a", "test_new"], data, 2)
        self.assertEqual(plan.unmeasured, ("test_new",))
        self.assertEqual(plan.loads, (2.0, 0.25))

    def test_plan_is_deterministic_and_independent_of_discovery_order(self) -> None:
        names = [f"test_m{index:03d}" for index in range(60)]
        data = _data({name: 1 + (index * 37 % 11) for index, name in enumerate(names)})
        first = ci_test_shards.plan_shards(names, data, 4)
        self.assertEqual(first, ci_test_shards.plan_shards(list(reversed(names)), data, 4))
        self.assertEqual(first, ci_test_shards.plan_shards(sorted(names, key=hash), data, 4))
        with_new = ci_test_shards.plan_shards(names + ["test_zz_new"], data, 4)
        self.assertEqual(with_new, ci_test_shards.plan_shards(["test_zz_new"] + names, data, 4))
        scheduled = [name for shard in with_new.shards for name in shard]
        self.assertEqual(sorted(scheduled), sorted(names + ["test_zz_new"]))
        self.assertEqual(ci_test_shards.check_plan(names + ["test_zz_new"], data, with_new), [])

    def test_shard_lists_are_name_ordered(self) -> None:
        data = _data({"test_c": 5, "test_a": 1, "test_b": 1})
        plan = ci_test_shards.plan_shards(["test_a", "test_b", "test_c"], data, 1)
        self.assertEqual(plan.shards, (("test_a", "test_b", "test_c"),))

    def test_more_shards_than_modules_leaves_trailing_shards_empty_without_error(self) -> None:
        data = _data({"test_a": 1})
        plan = ci_test_shards.plan_shards(["test_a"], data, 3)
        self.assertEqual(plan.shards, (("test_a",), (), ()))
        self.assertEqual(ci_test_shards.check_plan(["test_a"], data, plan), [])

    def test_group_larger_than_the_shard_count_is_refused(self) -> None:
        data = _data({"test_a": 1}, separate=[["test_a", "test_b", "test_c"]])
        with self.assertRaises(ci_test_shards.ShardDataError):
            ci_test_shards.plan_shards(["test_a", "test_b", "test_c"], data, 2)
        # Absent members do not count against the shard budget.
        plan = ci_test_shards.plan_shards(["test_a", "test_b"], data, 2)
        self.assertEqual(len([shard for shard in plan.shards if shard]), 2)

    def test_invalid_requests_are_refused(self) -> None:
        data = _data({"test_a": 1})
        for shards in (0, -1, True, 2.0):
            with self.subTest(shards=shards), self.assertRaises(ci_test_shards.ShardDataError):
                ci_test_shards.plan_shards(["test_a"], data, shards)  # type: ignore[arg-type]
        with self.assertRaises(ci_test_shards.ShardDataError):
            ci_test_shards.plan_shards(["test_a", "test_a"], data, 2)


class MakefileCouplingTests(unittest.TestCase):
    """--select makefile must be a superset of the modules a recipe edit can affect."""

    def test_selection_follows_imports_launches_and_unparseable_files(self) -> None:
        files = {
            "helper.py": "MAKEFILE = 'Makefile'\n",
            "middle.py": "import helper\n",
            "runner.py": "import subprocess\nsubprocess.run(['make', 'all'])\n",
            "plain.py": "VALUE = 1\n",
            "pkg/__init__.py": "",
            "pkg/inner.py": "from . import deep\n",
            "pkg/deep.py": "GOAL = 'gmake'\n",
            "test_direct.py": "TEXT = open('Makefile').read()\n",
            "test_import.py": "import helper\n",
            "test_transitive.py": "from tools import middle\n",
            "test_launch.py": "import subprocess, sys\nsubprocess.run([sys.executable, 'tools/runner.py'])\n",
            "test_package.py": "from pkg import inner\n",
            "test_broken.py": "def broken(:\n",
            "test_plain.py": "import plain\nimport json\n",
            "test_named_only.py": "NOTE = 'see plain.py for details'\n",
            "shell_runner.py": "import subprocess\nsubprocess.run(f\"make {'all'}\", shell=True)\n",
            "env_runner.py": "import os\nos.environ.get(\"MAKE\")\n",
            "oracle/__init__.py": "",
            "oracle/launch.py": "GOAL = '$(MAKE) smoke'\n",
            "test_shell.py": "import subprocess\nsubprocess.run(['python', 'tools/shell_runner.py'])\n",
            "test_env.py": "import subprocess, sys\nsubprocess.run([sys.executable, '-m', 'env_runner'])\n",
            "test_nested_path.py": "SCRIPT = 'tools/oracle/launch.py'\n",
            "test_dotted.py": "MODULE = 'oracle.launch'\n",
            "test_prose.py": "NOTE = 'we make no claims about plain'\n",
        }
        with tempfile.TemporaryDirectory() as scratch:
            tools_dir = Path(scratch)
            for name, text in files.items():
                (tools_dir / name).parent.mkdir(parents=True, exist_ok=True)
                (tools_dir / name).write_text(text, encoding="utf-8")
            modules = ci_test_shards.discover_modules(tools_dir)
            selected = ci_test_shards.makefile_coupled_modules(modules, tools_dir)
        self.assertEqual(
            selected,
            [
                "test_broken",
                "test_direct",
                "test_dotted",
                "test_env",
                "test_import",
                "test_launch",
                "test_nested_path",
                "test_package",
                "test_shell",
                "test_transitive",
            ],
        )

    def test_selection_follows_script_launchers_and_modules_outside_tools(self) -> None:
        """Failing-before: every edge was cut to tools/*.py units, so a test that runs a
        tools/ script which runs make, or imports a module outside tools/, was left out of
        the selection the docstring calls a superset."""
        files = {
            "tools/build.sh": "#!/bin/sh\nmake CC=gcc native-core-tests\n",
            "tools/runner.ps1": "& python \"$PSScriptRoot/helper.py\"\n",
            "tools/helper.py": "GOAL = '$(MAKE) smoke'\n",
            "tools/quiet.ps1": "Write-Host 'nothing to build'\n",
            "tools/test_shell_script.py": "import subprocess\nsubprocess.run(['sh', 'tools/build.sh'])\n",
            "tools/test_ps_script.py": "import subprocess\nsubprocess.run(['pwsh', '-File', 'tools/runner.ps1'])\n",
            "tools/test_quiet_script.py": "SCRIPT = 'tools/quiet.ps1'\n",
            "tools/test_outside.py": "import sys\nsys.path.insert(0, 'tests/fixtures/gen')\nimport fixture_gen\n",
            "tools/test_stdlib.py": "import json\nfrom tools import quiet_helper\n",
            "tools/quiet_helper.py": "VALUE = 1\n",
            "tests/fixtures/gen/fixture_gen.py": "VALUE = 2\n",
            "build/hst/private_input.py": "VALUE = 3\n",
        }
        with tempfile.TemporaryDirectory() as scratch:
            repo = Path(scratch)
            for name, text in files.items():
                (repo / name).parent.mkdir(parents=True, exist_ok=True)
                (repo / name).write_text(text, encoding="utf-8")
            tools_dir = repo / "tools"
            modules = ci_test_shards.discover_modules(tools_dir)
            selected = ci_test_shards.makefile_coupled_modules(modules, tools_dir, repo)
            self.assertEqual(ci_test_shards._outside_modules(repo, tools_dir), frozenset({"fixture_gen"}))
            alone = ci_test_shards.makefile_coupled_modules(modules, tools_dir, None)
        self.assertEqual(selected, ["test_outside", "test_ps_script", "test_shell_script"])
        self.assertEqual(alone, ["test_ps_script", "test_shell_script"])

    def test_unknown_module_names_are_kept(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            self.assertEqual(
                ci_test_shards.makefile_coupled_modules(["test_missing"], Path(scratch)), ["test_missing"]
            )


def _call_name(node: ast.AST) -> str | None:
    func = node.func if isinstance(node, ast.Call) else None
    if isinstance(func, ast.Attribute):
        return func.attr
    return func.id if isinstance(func, ast.Name) else None


def _which_gcc(node: ast.AST) -> bool:
    """``node`` is ``shutil.which("gcc", ...)``."""
    return (isinstance(node, ast.Call) and _call_name(node) == "which" and bool(node.args)
            and isinstance(node.args[0], ast.Constant) and node.args[0].value == "gcc")


def _target_names(target: ast.AST) -> list[str]:
    if isinstance(target, ast.Name):
        return [target.id]
    if isinstance(target, ast.Attribute):
        return [target.attr]
    if isinstance(target, (ast.Tuple, ast.List)):
        return [name for element in target.elts for name in _target_names(element)]
    return []


def _assigned_values(assign: ast.Assign | ast.AnnAssign) -> list[tuple[str, ast.AST]]:
    """(name, value) per target, pairing a tuple target with a tuple value element-wise."""
    value = assign.value
    assert value is not None
    pairs: list[tuple[str, ast.AST]] = []
    for target in assign.targets if isinstance(assign, ast.Assign) else [assign.target]:
        if (isinstance(target, (ast.Tuple, ast.List)) and isinstance(value, (ast.Tuple, ast.List))
                and len(target.elts) == len(value.elts)):
            pairs.extend((name, element) for part, element in zip(target.elts, value.elts, strict=True)
                         for name in _target_names(part))
        else:
            pairs.extend((name, value) for name in _target_names(target))
    return pairs


def _compiler_names(tree: ast.Module) -> set[str]:
    """Names (variables, attributes, functions) that hold or return the gcc a test looked for.

    A name assigned from an expression that calls ``which("gcc")`` holds it, and so does one
    assigned plainly (a name, ``or``, a conditional, a list) from such a name. A function whose
    ``return`` gives one returns it.
    """
    names: set[str] = set()

    def holds(value: ast.AST) -> bool:
        if any(_which_gcc(node) for node in ast.walk(value)):
            return True
        if isinstance(value, (ast.Name, ast.Attribute)):
            return _target_names(value)[0] in names
        if isinstance(value, ast.BoolOp):
            return any(holds(v) for v in value.values)
        if isinstance(value, ast.IfExp):
            return holds(value.body) or holds(value.orelse)
        if isinstance(value, (ast.List, ast.Tuple)):
            return any(holds(v) for v in value.elts)
        if isinstance(value, ast.Call):
            return _call_name(value) in names
        return False

    while True:
        before = len(names)
        for node in ast.walk(tree):
            if isinstance(node, (ast.Assign, ast.AnnAssign)) and node.value is not None:
                names.update(name for name, value in _assigned_values(node) if holds(value))
            elif isinstance(node, ast.FunctionDef) and any(
                    isinstance(r, ast.Return) and r.value is not None and holds(r.value)
                    for r in ast.walk(node)):
                names.add(node.name)
        if len(names) == before:
            return names


def _is_gcc(node: ast.AST, names: set[str]) -> bool:
    """``node`` is a gcc lookup or a name that holds or returns one (true when gcc was found)."""
    if _which_gcc(node):
        return True
    if isinstance(node, (ast.Name, ast.Attribute)):
        return _target_names(node)[0] in names
    return isinstance(node, ast.Call) and _call_name(node) in names


def _uses_gcc(test: ast.AST, names: set[str]) -> bool:
    """``test`` asks whether gcc was found, through ``not``, ``and``/``or`` and ``is (not) None``."""
    if isinstance(test, ast.UnaryOp) and isinstance(test.op, ast.Not):
        return _uses_gcc(test.operand, names)
    if isinstance(test, ast.BoolOp):
        return any(_uses_gcc(value, names) for value in test.values)
    if isinstance(test, ast.Compare) and len(test.ops) == 1 and isinstance(test.ops[0], (ast.Is, ast.IsNot)):
        return _uses_gcc(test.left, names)
    return _is_gcc(test, names)


def _means_no_gcc(test: ast.AST, value: bool, names: set[str]) -> bool:
    """Whether ``test`` evaluating to ``value`` proves gcc was not found."""
    if isinstance(test, ast.UnaryOp) and isinstance(test.op, ast.Not):
        return _means_no_gcc(test.operand, not value, names)
    if isinstance(test, ast.BoolOp):
        # A true ``and`` (a false ``or``) makes every operand so; otherwise one is, unknown which.
        every = isinstance(test.op, ast.And) == value
        proofs = [_means_no_gcc(operand, value, names) for operand in test.values]
        return any(proofs) if every else all(proofs)
    if (isinstance(test, ast.Compare) and len(test.ops) == 1 and isinstance(test.ops[0], (ast.Is, ast.IsNot))
            and isinstance(test.comparators[0], ast.Constant) and test.comparators[0].value is None):
        return _means_no_gcc(test.left, value != isinstance(test.ops[0], ast.Is), names)
    return not value and _is_gcc(test, names)


def _reason_text(reason: ast.AST, block: list[ast.stmt]) -> str | None:
    """The literal start of a skip reason; a name is looked up among ``block``'s assignments."""
    if isinstance(reason, ast.Constant) and isinstance(reason.value, str):
        return reason.value
    if isinstance(reason, ast.JoinedStr) and reason.values and isinstance(reason.values[0], ast.Constant):
        return str(reason.values[0].value)
    if isinstance(reason, ast.Name):
        for statement in block:
            if isinstance(statement, ast.Assign) and _target_names(statement.targets[0]) == [reason.id]:
                return _reason_text(statement.value, [])
    return None


def gcc_skip_problems(source: str, filename: str) -> list[str]:
    """Every skip that depends on finding gcc and is not named ``TOOLCHAIN_MISSING:`` (F-6a8c715a).

    A skip is a ``skipIf``/``skipUnless`` decorator, or a ``skipTest``/``SkipTest`` under an
    ``if``; it depends on gcc when its condition asks whether ``which("gcc")`` found one. Such a
    skip must fire only when gcc is missing: another requirement (make, nm, a platform) that
    could fire it takes its own skip first, so a ``TOOLCHAIN_MISSING:`` skip means just that.
    """
    tree = ast.parse(source, filename)
    names = _compiler_names(tree)
    parents = {child: parent for parent in ast.walk(tree) for child in ast.iter_child_nodes(parent)}
    problems: list[tuple[int, str]] = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call) or not node.args:
            continue
        kind = _call_name(node)
        block: list[ast.stmt] = []
        if kind in ("skipIf", "skipUnless") and len(node.args) >= 2:
            condition, reason, fires_when = node.args[0], node.args[1], kind == "skipIf"
        elif kind in ("skipTest", "SkipTest"):
            parent = parents.get(node)
            while parent is not None and not isinstance(parent, (ast.If, ast.FunctionDef, ast.Module)):
                parent = parents.get(parent)
            if not isinstance(parent, ast.If):
                continue
            in_body = any(node in ast.walk(statement) for statement in parent.body)
            condition, reason, fires_when = parent.test, node.args[0], in_body
            block = parent.body if in_body else parent.orelse
        else:
            continue
        if not _uses_gcc(condition, names):
            continue
        where = f"{filename}:{node.lineno}"
        if not _means_no_gcc(condition, fires_when, names):
            problems.append((node.lineno, f"{where}: the skip can fire with gcc found: {ast.unparse(condition)}"))
            continue
        text = _reason_text(reason, block)
        if text is None or not text.startswith("TOOLCHAIN_MISSING:"):
            problems.append((node.lineno, f"{where}: skip reason {ast.unparse(reason)} lacks the "
                                          "TOOLCHAIN_MISSING: prefix"))
    return [problem for _line, problem in sorted(problems)]


class ToolchainMissingSkipTests(unittest.TestCase):
    """A test that skips because gcc is missing says so with TOOLCHAIN_MISSING: (F-6a8c715a, F-a65c02d8)."""

    def test_the_prefix_is_a_counted_skip_reason(self) -> None:
        self.assertIn("TOOLCHAIN_MISSING:", ci_test_shards.SKIP_REASON_PREFIXES)
        output = ("test_a ... skipped 'TOOLCHAIN_MISSING: no C compiler on PATH'\n"
                  "test_b ... skipped 'no C compiler on PATH'\n")
        self.assertTrue(ci_test_shards.skip_prefix_summary(output).endswith(" TOOLCHAIN_MISSING=1"))

    def test_every_gcc_skip_in_the_test_modules_carries_the_prefix(self) -> None:
        problems = [problem for path in sorted((ROOT / "tools").glob("test_*.py"))
                    for problem in gcc_skip_problems(path.read_text(encoding="utf-8"), path.name)]
        self.assertEqual(problems, [], "\n".join(problems))

    def test_the_check_finds_each_skip_form(self) -> None:
        source = textwrap.dedent("""\
            import os, shutil, sys, unittest
            CC = shutil.which("gcc") or shutil.which("cc")
            NM = shutil.which("nm")

            def _toolchain():
                found = shutil.which("gcc")
                return found if found else None

            @unittest.skipUnless(CC, "no C compiler on PATH")
            class Bare(unittest.TestCase): pass

            @unittest.skipUnless(CC and NM, "TOOLCHAIN_MISSING: needs a C compiler and nm")
            class Mixed(unittest.TestCase): pass

            @unittest.skipUnless(_toolchain(), "needs gcc")
            class Helper(unittest.TestCase): pass

            class Named(unittest.TestCase):
                @classmethod
                def setUpClass(cls):
                    cls.gcc = shutil.which("gcc")
                    if not cls.gcc:
                        raise unittest.SkipTest("gcc not available")
                    windres, gcc = shutil.which("windres"), shutil.which("gcc")
                    if not (windres and gcc):
                        raise unittest.SkipTest("TOOLCHAIN_MISSING: windres and gcc")

                def test_a(self):
                    if not (os.environ.get("CC") or shutil.which("gcc")):
                        reason = "a C compiler is required"
                        self.skipTest(reason)

            @unittest.skipUnless(CC, "TOOLCHAIN_MISSING: no C compiler on PATH")
            class Good(unittest.TestCase):
                def test_b(self):
                    if shutil.which("gcc") is None:
                        self.skipTest(f"TOOLCHAIN_MISSING: {sys.platform} has no gcc")
                    if not _probe(CC):
                        self.skipTest("this gcc cannot build the backend")
                    if os.environ.get("CC") or shutil.which("gcc"):
                        reason = "TOOLCHAIN_MISSING: never"
                    else:
                        reason = "TOOLCHAIN_MISSING: no compiler"
                        raise unittest.SkipTest(reason)
            """)
        found = [problem.split(": ", 1) for problem in gcc_skip_problems(source, "t.py")]
        self.assertEqual([(where, text.split(":")[0]) for where, text in found], [
            ("t.py:9", "skip reason 'no C compiler on PATH' lacks the TOOLCHAIN_MISSING"),
            ("t.py:12", "the skip can fire with gcc found"),
            ("t.py:15", "skip reason 'needs gcc' lacks the TOOLCHAIN_MISSING"),
            ("t.py:23", "skip reason 'gcc not available' lacks the TOOLCHAIN_MISSING"),
            ("t.py:26", "the skip can fire with gcc found"),
            ("t.py:31", "skip reason reason lacks the TOOLCHAIN_MISSING"),
        ])


class WindowsOnlySelectionTests(unittest.TestCase):
    """--windows-only names every test that skips on every host but Windows, and nothing else."""

    def test_selection_reads_decorators_skip_statements_and_module_names(self) -> None:
        files = {
            "test_decorated.py": textwrap.dedent("""\
                import os, platform, sys, unittest
                WINDOWS = os.name == "nt"
                POSIX = sys.platform != "win32"

                class Methods(unittest.TestCase):
                    @unittest.skipUnless(os.name == "nt", "Windows")
                    def test_nt(self): pass
                    @unittest.skipUnless(sys.platform == "win32" and True, "Windows")
                    def test_win32_and(self): pass
                    @unittest.skipIf(sys.platform != "win32", "Windows")
                    def test_skip_if_not_win32(self): pass
                    @unittest.skipIf(os.name == "posix" or False, "Windows")
                    def test_posix_or(self): pass
                    @unittest.skipUnless(WINDOWS, "Windows")
                    def test_module_name(self): pass
                    @unittest.skipIf(POSIX, "Windows")
                    def test_module_other_host_name(self): pass
                    @unittest.skipUnless(platform.system() == "Windows", "Windows")
                    def test_platform_system(self): pass
                    @unittest.skipUnless(sys.platform.startswith("win"), "Windows")
                    def test_startswith(self): pass
                    @unittest.skipUnless(not (os.name != "nt"), "Windows")
                    def test_negated(self): pass
                    @unittest.skipUnless(os.name == "nt" or sys.platform == "darwin", "two hosts")
                    def test_two_hosts(self): pass
                    @unittest.skipIf(os.name == "nt", "POSIX")
                    def test_posix_only(self): pass
                    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux")
                    def test_linux_only(self): pass
                    @unittest.skipIf(os.name != "nt" and WINDOWS, "never")
                    def test_and_is_not_every_other_host(self): pass
                    def test_plain(self): pass

                @unittest.skipUnless(os.name == "nt", "Windows")
                class WholeClass(unittest.TestCase):
                    def test_a(self): pass
                """),
            "test_statements.py": textwrap.dedent("""\
                import os, shutil, sys, unittest

                class Bodies(unittest.TestCase):
                    def test_skip_test(self):
                        if os.name != "nt" or not shutil.which("pwsh"):
                            self.skipTest("Windows and pwsh")
                    def test_raise(self):
                        if sys.platform != "win32":
                            raise unittest.SkipTest("Windows")
                    def test_nested_is_not_read(self):
                        try:
                            pass
                        except OSError:
                            if os.name != "nt":
                                self.skipTest("only when the call failed")
                    def test_tool_presence(self):
                        if not shutil.which("mingw32-make"):
                            self.skipTest("a tool, not a platform")
                    def test_and_condition(self):
                        if os.name != "nt" and shutil.which("x"):
                            self.skipTest("only sometimes")

                class SetUpClassSkip(unittest.TestCase):
                    @classmethod
                    def setUpClass(cls):
                        if os.name != "nt":
                            raise unittest.SkipTest("Windows")
                    def test_a(self): pass
                """),
            "test_whole_module.py": textwrap.dedent("""\
                import sys, unittest
                if sys.platform != "win32":
                    raise unittest.SkipTest("Windows")
                class Anything(unittest.TestCase):
                    def test_a(self): pass
                """),
            "test_portable.py": "import unittest\nclass P(unittest.TestCase):\n    def test_a(self): pass\n",
        }
        with tempfile.TemporaryDirectory() as scratch:
            tools_dir = Path(scratch)
            for name, text in files.items():
                (tools_dir / name).write_text(text, encoding="utf-8")
            selected = ci_test_shards.windows_only_tests(ci_test_shards.discover_modules(tools_dir), tools_dir)
        self.assertEqual(selected, [
            "test_decorated.Methods.test_nt",
            "test_decorated.Methods.test_win32_and",
            "test_decorated.Methods.test_skip_if_not_win32",
            "test_decorated.Methods.test_posix_or",
            "test_decorated.Methods.test_module_name",
            "test_decorated.Methods.test_module_other_host_name",
            "test_decorated.Methods.test_platform_system",
            "test_decorated.Methods.test_startswith",
            "test_decorated.Methods.test_negated",
            "test_decorated.WholeClass",
            "test_statements.Bodies.test_skip_test",
            "test_statements.Bodies.test_raise",
            "test_statements.SetUpClassSkip",
            "test_whole_module",
        ])

    def test_an_unreadable_module_stops_the_selection(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            (Path(scratch) / "test_broken.py").write_text("def broken(:\n", encoding="utf-8")
            with self.assertRaises(ci_test_shards.ShardDataError):
                ci_test_shards.windows_only_tests(["test_broken"], Path(scratch))

    def test_cli_lists_the_live_windows_only_tests_but_not_those_run_elsewhere(self) -> None:
        result = _run_cli("--windows-only")
        self.assertEqual(result.returncode, 0, result.stderr)
        names = result.stdout.split()
        # Known members at the time of writing, one per detection route.
        for name in ("test_scratch_dir.WindowsLockTests",
                     "test_stage_install.InstallTests.test_shortcut_targets_the_installed_player",
                     "test_production_smoke.TestProductionSmokePackage."
                     "test_package_builds_to_real_spaced_destination_without_mocks"):
            self.assertIn(name, names)
        self.assertEqual(names, ci_test_shards.windows_only_tests(
            [m for m in ci_test_shards.discover_modules()
             if m not in ci_test_shards.WINDOWS_ONLY_RUN_ELSEWHERE]))
        self.assertTrue(ci_test_shards.windows_only_tests(["test_installer"]))
        self.assertFalse([name for name in names if name.split(".")[0] in ci_test_shards.WINDOWS_ONLY_RUN_ELSEWHERE])

    def test_cli_exits_2_on_an_empty_selection(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            (Path(scratch) / "test_portable.py").write_text("VALUE = 1\n", encoding="utf-8")
            result = _run_cli("--windows-only", "--tools-dir", scratch)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")
        self.assertIn("Windows-only selection is empty", result.stderr)


class CheckPlanTests(unittest.TestCase):
    def test_stale_weights_and_missing_group_members_fail_the_check(self) -> None:
        data = _data({"test_a": 1, "test_gone": 1}, separate=[["test_a", "test_also_gone"]])
        plan = ci_test_shards.plan_shards(["test_a", "test_b"], data, 2)
        errors = ci_test_shards.check_plan(["test_a", "test_b"], data, plan)
        self.assertTrue(any("test_gone" in error for error in errors), errors)
        self.assertTrue(any("test_also_gone" in error for error in errors), errors)

    def test_a_dropped_duplicated_or_colocated_module_fails_the_check(self) -> None:
        data = _data({"test_a": 1, "test_b": 1}, separate=[["test_a", "test_b"]])
        modules = ["test_a", "test_b", "test_c"]
        for shards, reason in (
            ((("test_a",), ("test_b",)), "dropped=['test_c']"),
            ((("test_a", "test_c"), ("test_b", "test_c")), "duplicated=['test_c']"),
            ((("test_a", "test_b", "test_c"), ()), "together on shard 0"),
        ):
            with self.subTest(reason=reason):
                plan = ci_test_shards.ShardPlan(shards=shards, loads=(0.0, 0.0), unmeasured=())
                errors = ci_test_shards.check_plan(modules, data, plan)
                self.assertTrue(any(reason in error for error in errors), errors)


class WeightDataValidationTests(unittest.TestCase):
    def test_malformed_documents_are_rejected(self) -> None:
        good = {"default_seconds": 0.5, "weights": {"test_a": 1.0}, "separate": []}
        cases = {
            "not an object": [],
            "unknown key": {**good, "pins": {}},
            "zero default": {**good, "default_seconds": 0},
            "boolean weight": {**good, "weights": {"test_a": True}},
            "negative weight": {**good, "weights": {"test_a": -1}},
            "infinite weight": {**good, "weights": {"test_a": float("inf")}},
            "non-test key": {**good, "weights": {"helper": 1.0}},
            "empty weights": {**good, "weights": {}},
            "singleton group": {**good, "separate": [["test_a"]]},
            "repeated group member": {**good, "separate": [["test_a", "test_a"]]},
            "group not a list": {**good, "separate": {"test_a": "test_b"}},
        }
        for label, raw in cases.items():
            with self.subTest(label), self.assertRaises(ci_test_shards.ShardDataError):
                ci_test_shards.parse_weight_data(raw)

    def test_cli_exits_2_with_no_module_list_on_bad_data(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            bad = Path(scratch) / "weights.json"
            for content in ("{not json", json.dumps({"default_seconds": -1, "weights": {"test_a": 1}})):
                with self.subTest(content=content):
                    bad.write_text(content, encoding="utf-8")
                    result = _run_cli("--shard", "0", "--weights", str(bad))
                    self.assertEqual(result.returncode, 2)
                    self.assertEqual(result.stdout, "")
            result = _run_cli("--shard", "0", "--weights", str(Path(scratch) / "absent.json"))
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, "")

    def test_cli_rejects_an_out_of_range_shard(self) -> None:
        result = _run_cli("--shard", "4", "--num-shards", "4")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")

    def test_cli_check_fails_on_stale_data(self) -> None:
        live = json.loads((ROOT / "tools" / "ci_test_weights.json").read_text(encoding="utf-8"))
        live["weights"]["test_module_that_was_deleted"] = 1.0
        with tempfile.TemporaryDirectory() as scratch:
            stale = Path(scratch) / "weights.json"
            stale.write_text(json.dumps(live), encoding="utf-8")
            result = _run_cli("--check", "--weights", str(stale))
        self.assertEqual(result.returncode, 1)
        self.assertIn("test_module_that_was_deleted", result.stderr)


if __name__ == "__main__":
    unittest.main()
