# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for tools/regen_all.py, the one command that regenerates committed generated files."""

from __future__ import annotations

import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import regen_all  # noqa: E402

ROOT = regen_all.ROOT


class StepInventoryTests(unittest.TestCase):
    def test_outputs_are_tracked_files_owned_by_one_step(self) -> None:
        seen: dict[str, str] = {}
        for step in (*regen_all.STEPS, regen_all.PROVENANCE):
            for output in step.outputs:
                with self.subTest(output=output):
                    self.assertTrue((ROOT / output).is_file(), f"{step.name} names a missing output")
                    self.assertNotIn(output, seen, f"{output} belongs to {seen.get(output)} and {step.name}")
                    seen[output] = step.name

    def test_every_step_either_runs_or_says_why_not(self) -> None:
        for step in regen_all.STEPS:
            with self.subTest(step=step.name):
                self.assertNotEqual(step.render is None, not step.skip, "a step runs, or it is skipped with a reason")

    def test_staleness_messages_name_the_one_fix_command(self) -> None:
        # Each committed-output check prints regen_all as its fix, not its own generator.
        for rel in (
            "tools/recomp_helper_inventory.py",
            "tools/test_recomp_helper_inventory.py",
            "tools/waits_census.py",
            "tools/test_waits_census.py",
            "tools/verification_ledger.py",
            "tools/test_verification_ledger.py",
            "tools/gen_nidnames.py",
            "tools/test_gen_nidnames.py",
            "tools/test_hle_manifest.py",
            "tools/import_audit_gate.py",
            "tools/title_catalog_codegen.py",
            "tools/test_pgf_validate_parity.py",
            "tools/publish_audit.py",
        ):
            with self.subTest(path=rel):
                self.assertIn(regen_all.FIX_COMMAND, (ROOT / rel).read_text(encoding="utf-8"))


class RenderTests(unittest.TestCase):
    def test_each_public_generator_reproduces_its_committed_output(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            for step in regen_all.STEPS:
                if step.skip:
                    continue
                with self.subTest(step=step.name):
                    scratch = Path(tmp) / step.name.replace(" ", "_")
                    scratch.mkdir()
                    rendered = step.render(scratch)
                    self.assertEqual(sorted(rendered), sorted(step.outputs))
                    for path, data in rendered.items():
                        self.assertEqual(data, (ROOT / path).read_bytes(), f"{path} is stale")

    def test_baseline_refresh_refuses_a_coverage_downgrade(self) -> None:
        committed = json.loads((ROOT / "tools" / "import_audit_baseline.json").read_text(encoding="ascii"))
        nid, entry = next((k, v) for k, v in committed.items() if v["classification"] == "dedicated")
        degraded = dict(committed)
        degraded[nid] = dict(entry, classification="fake_success")
        with tempfile.TemporaryDirectory() as tmp:
            fresh = Path(tmp) / "baseline.json"
            fresh.write_text(json.dumps(degraded), encoding="ascii")
            with self.assertRaises(regen_all.StepError) as ctx:
                regen_all._refuse_baseline_downgrades(fresh)
            self.assertIn(nid, str(ctx.exception))
            fresh.write_text(json.dumps(committed), encoding="ascii")
            regen_all._refuse_baseline_downgrades(fresh)


class AdmissionBlockTests(unittest.TestCase):
    def test_only_the_admission_block_is_ignored(self) -> None:
        ledger = {"schema_version": 1, "entries": [{"path": "a", "sha256": "0" * 64}]}
        with_admission = dict(ledger, admission={"workflow": "admit-new-reviewed"})
        self.assertEqual(
            regen_all._without_admission(json.dumps(with_admission).encode()),
            regen_all._without_admission(json.dumps(ledger).encode()),
        )
        rehashed = {"schema_version": 1, "entries": [{"path": "a", "sha256": "1" * 64}]}
        self.assertNotEqual(
            regen_all._without_admission(json.dumps(with_admission).encode()),
            regen_all._without_admission(json.dumps(rehashed).encode()),
        )


class PublicLabelTests(unittest.TestCase):
    """A ledger that differs from the base's only in public label text is not stale."""

    def _ledger(self, authorship: str, *, classification: str = "project_authored_attested") -> dict:
        return {"schema_version": 2, "entries": [
            {"path": "src/a.c", "classification": classification,
             "evidence": {"record_id": "rec-a", "authorship": authorship}},
        ]}

    def test_only_known_label_text_is_ignored(self) -> None:
        sets = regen_all_labels()
        v1 = self._ledger(sets["v1"]["behavior-informed"])
        v2 = self._ledger(sets["v2"]["behavior-informed"])
        self.assertNotEqual(v1, v2)
        self.assertEqual(regen_all._without_labels(v1), regen_all._without_labels(v2))
        # Free text is not a label, and a changed claim is not a label change.
        self.assertNotEqual(regen_all._without_labels(v1), regen_all._without_labels(self._ledger("hand-written")))
        moved = self._ledger(sets["v2"]["behavior-informed"], classification="upstream_derived")
        self.assertNotEqual(regen_all._without_labels(v1), regen_all._without_labels(moved))


def regen_all_labels() -> dict[str, dict[str, str]]:
    sys.path.insert(0, str(Path(regen_all.__file__).resolve().parent))
    import provenance_ledger

    return provenance_ledger.PUBLIC_LABEL_SETS


class CheckModeTests(unittest.TestCase):
    def _run_check(self, steps, provenance=("OK", [], "")) -> tuple[int, str]:
        out = io.StringIO()
        with mock.patch.object(regen_all, "STEPS", steps), \
                mock.patch.object(regen_all, "_provenance", return_value=provenance), \
                contextlib.redirect_stdout(out):
            rc = regen_all.run(check=True, base=None)
        return rc, out.getvalue()

    def test_a_stale_output_fails_check_with_the_fix_command(self) -> None:
        path = "docs/PSP_INTR_WAITS_CENSUS.md"
        stale = regen_all.Step("fake", "fake", (path,), lambda tmp: {path: b"not the committed bytes"})
        rc, out = self._run_check((stale,))
        self.assertEqual(rc, 1)
        self.assertIn(f"STALE fake: {path}", out)
        self.assertIn(f"Fix with: {regen_all.FIX_COMMAND}", out)

    def test_current_outputs_and_skips_pass_check_without_writing(self) -> None:
        path = "docs/PSP_INTR_WAITS_CENSUS.md"
        current = regen_all.Step("fake", "fake", (path,), lambda tmp: {path: (ROOT / path).read_bytes()})
        skipped = regen_all.Step("private", "private", (), skip="needs private inputs")
        rc, out = self._run_check((current, skipped))
        self.assertEqual(rc, 0, out)
        self.assertIn("SKIP  private: needs private inputs", out)

    def test_stale_provenance_controls_fail_check(self) -> None:
        rc, out = self._run_check((), provenance=("STALE", ["assets/public_provenance_ledger.json"], ""))
        self.assertEqual(rc, 1)
        self.assertIn(regen_all.FIX_COMMAND, out)

    def test_a_generator_failure_fails_check(self) -> None:
        def broken(tmp):
            raise regen_all.StepError("boom")

        rc, out = self._run_check((regen_all.Step("broken", "broken", (), broken),))
        self.assertEqual(rc, 1)
        self.assertIn("FAIL  broken: boom", out)


class WriteModeTests(unittest.TestCase):
    def test_a_later_generator_failure_writes_nothing(self) -> None:
        # An earlier stale step must not be written when a later generator fails,
        # including with an exception that is not a StepError.
        def buggy(tmp):
            raise KeyError("missing")

        with tempfile.TemporaryDirectory() as root:
            stale = regen_all.Step("fresh", "fresh", ("out.txt",), lambda tmp: {"out.txt": b"new\n"})
            broken = regen_all.Step("buggy", "buggy", (), buggy)
            out = io.StringIO()
            provenance = mock.Mock(side_effect=AssertionError("provenance must not run"))
            git = mock.Mock(side_effect=AssertionError("nothing may be staged"))
            with mock.patch.object(regen_all, "ROOT", Path(root)), \
                    mock.patch.object(regen_all, "STEPS", (stale, broken)), \
                    mock.patch.object(regen_all, "_provenance", provenance), \
                    mock.patch.object(regen_all, "_git", git), \
                    contextlib.redirect_stdout(out):
                rc = regen_all.run(check=False, base=None)
            self.assertEqual(rc, 1)
            self.assertIn("FAIL  buggy: KeyError", out.getvalue())
            self.assertFalse((Path(root) / "out.txt").exists())

    def test_stale_provenance_controls_are_written_from_one_generation(self) -> None:
        # regen_all writes and stages the controls it generated in process; it
        # does not run contrib_refresh.py to generate them a second time.
        sys.path.insert(0, str(ROOT / "tools"))
        import contrib_refresh

        with tempfile.TemporaryDirectory() as root:
            git = mock.Mock(return_value=mock.Mock(returncode=0, stderr=""))
            with mock.patch.object(regen_all, "ROOT", Path(root)), \
                    mock.patch.object(contrib_refresh, "_generate", return_value=(b"ledger\n", b"export\n", [])) as gen, \
                    mock.patch.object(regen_all, "_git", git), \
                    mock.patch.object(regen_all.subprocess, "run",
                                      side_effect=AssertionError("no second generation")):
                status, paths, detail = regen_all._provenance(False, None)
            self.assertEqual(status, "WROTE", detail)
            self.assertEqual(sorted(paths), sorted([contrib_refresh.LEDGER, contrib_refresh.EXPORT]))
            self.assertEqual(gen.call_count, 1)
            self.assertEqual((Path(root) / contrib_refresh.LEDGER).read_bytes(), b"ledger\n")
            self.assertEqual((Path(root) / contrib_refresh.EXPORT).read_bytes(), b"export\n")
            git.assert_called_once_with("add", "--", contrib_refresh.LEDGER, contrib_refresh.EXPORT)


class ShaderStepTests(unittest.TestCase):
    def test_ui_shader_set_embeddings_are_listed(self) -> None:
        # regen_all --list defines generated content, so the UI shader set's
        # embeddings and manifest belong to a step that names its command.
        import shader_embed

        expected = {
            (shader_embed.UI_DIR / output).relative_to(ROOT).as_posix()
            for _stage, _source, output in shader_embed.UI_SHADERS
        }
        expected.add(shader_embed.UI_MANIFEST.relative_to(ROOT).as_posix())
        owners = [step for step in regen_all.STEPS if expected & set(step.outputs)]
        self.assertEqual(len(owners), 1, [step.name for step in owners])
        self.assertEqual(set(owners[0].outputs), expected)
        self.assertIn("--shader-set ui", owners[0].command)
        self.assertNotIn("make shader-verify", owners[0].skip or "")


def _git(repo: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run(
        ["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "core.autocrlf=false",
         "-c", "commit.gpgsign=false", *args],
        cwd=repo, capture_output=True, text=True, check=False,
    )
    if check and proc.returncode != 0:
        raise AssertionError(f"git {' '.join(args)} failed: {proc.stderr}{proc.stdout}")
    return proc


GEN = "docs/GEN.md"  # a whole-file output: a summary line, then one line per item of src/items.txt
COUNT = "docs/COUNT.md"  # both sides of two additions write the same new count, so Git merges it, stale
DOC = regen_all.HLE_INVENTORY  # a hand-written file holding a generated block
BEGIN, END = regen_all.HLE_CENSUS_BEGIN, regen_all.HLE_CENSUS_END


def _doc(prose_top: str, block: str, prose_bottom: str) -> str:
    return f"# Inventory\n\n{prose_top}\n\n{BEGIN}\n{block}\n{END}\n\n{prose_bottom}\n"


def _gen_text(items: list[str]) -> str:
    # The summary line changes on both sides of any two additions, so Git conflicts there.
    return f"items: {' '.join(items)}\n" + "".join(f"- {item}\n" for item in items)


def _count_text(items: list[str]) -> str:
    return f"count: {len(items)}\n"


def _block(items: list[str]) -> str:
    return f"census: {' '.join(items)}"


class ResolveMergeTests(unittest.TestCase):
    """`--resolve-merge` on a real Git merge whose only conflicts are in regenerated files.

    Two branches each add an item to ``src/items.txt`` at different places, so Git merges the source,
    but both rewrote the generated files from their own tree, so those conflict. The fake steps render
    from the working tree exactly as the real generators do.
    """

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.addCleanup(os.chdir, os.getcwd())  # main() moves into the repository it works on
        self.repo = Path(self._tmp.name) / "repo"
        (self.repo / "src").mkdir(parents=True)
        (self.repo / "docs").mkdir()
        _git(self.repo, "init", "-q", "-b", "main")
        self.renders = 0
        self.steps = (
            regen_all.Step("gen", "gen", (GEN,), self._render_gen),
            regen_all.Step("count", "count", (COUNT,), lambda tmp: {COUNT: _count_text(self._items()).encode()}),
            regen_all.Step("doc block", "doc", (DOC,), self._render_doc),
            regen_all.Step("shaders", "make shaders", ("src/shader.inc",), skip="needs glslc"),
        )
        patches = (
            mock.patch.object(regen_all, "ROOT", self.repo),
            mock.patch.object(regen_all, "STEPS", self.steps),
            mock.patch.object(regen_all, "_provenance", side_effect=self._fake_provenance),
        )
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)
        self.provenance_calls: list[bool] = []
        self.provenance_result: tuple[str, list[str], str] = ("OK", [], "")
        self._commit_tree("a\nm\nz\n", "top prose", "bottom prose", "base")

    # -- fake generators that read the tree, like the real ones -------------------------------------
    def _items(self) -> list[str]:
        return (self.repo / "src" / "items.txt").read_text(encoding="utf-8").split()

    def _render_gen(self, tmp: Path) -> dict[str, bytes]:
        self.renders += 1
        return {GEN: _gen_text(self._items()).encode()}

    def _render_doc(self, tmp: Path) -> dict[str, bytes]:
        text = (self.repo / DOC).read_text(encoding="utf-8")
        head, rest = text.split(BEGIN, 1)
        _, tail = rest.split(END, 1)
        return {DOC: f"{head}{BEGIN}\n{_block(self._items())}\n{END}{tail}".encode()}

    def _fake_provenance(self, check: bool, base: str | None) -> tuple[str, list[str], str]:
        self.provenance_calls.append(check)
        return self.provenance_result

    # -- repository helpers ------------------------------------------------------------------------
    def _write_tree(self, items: str, top: str, bottom: str) -> None:
        (self.repo / "src" / "items.txt").write_text(items, encoding="utf-8")
        listed = items.split()
        (self.repo / GEN).write_text(_gen_text(listed), encoding="utf-8")
        (self.repo / COUNT).write_text(_count_text(listed), encoding="utf-8")
        (self.repo / DOC).write_text(_doc(top, _block(listed), bottom), encoding="utf-8")

    def _commit_tree(self, items: str, top: str, bottom: str, message: str) -> None:
        self._write_tree(items, top, bottom)
        _git(self.repo, "add", "-A")
        _git(self.repo, "commit", "-q", "-m", message)

    def _branches(self, a_top: str = "top prose", b_bottom: str = "bottom prose") -> None:
        _git(self.repo, "checkout", "-q", "-b", "pr-a", "main")
        self._commit_tree("a\nA\nm\nz\n", a_top, "bottom prose", "pr-a")
        _git(self.repo, "checkout", "-q", "-b", "pr-b", "main")
        self._commit_tree("a\nm\nB\nz\n", "top prose", b_bottom, "pr-b")

    def _merge(self, *how: str) -> None:
        proc = _git(self.repo, "merge", *how, "pr-a", check=False)
        self.assertNotEqual(proc.returncode, 0, "the generated files must conflict for the test to mean anything")
        self.assertEqual(set(self._unmerged()), {GEN, DOC})

    def _unmerged(self) -> list[str]:
        return _git(self.repo, "diff", "--name-only", "--diff-filter=U").stdout.split()

    def _resolve(self) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = regen_all.main(["--resolve-merge"])
        return rc, out.getvalue()

    # -- the cases ---------------------------------------------------------------------------------
    def test_generated_conflicts_are_regenerated_from_the_merged_tree(self) -> None:
        self._branches()
        self._merge("--no-edit")
        rc, out = self._resolve()
        self.assertEqual(rc, 0, out)
        self.assertEqual(self._unmerged(), [])
        merged = ["a", "A", "m", "B", "z"]
        self.assertEqual((self.repo / "src" / "items.txt").read_text(encoding="utf-8").split(), merged)
        self.assertEqual((self.repo / GEN).read_text(encoding="utf-8"), _gen_text(merged))
        self.assertEqual((self.repo / DOC).read_text(encoding="utf-8"), _doc("top prose", _block(merged), "bottom prose"))
        self.assertEqual(_git(self.repo, "diff", "--name-only").stdout.strip(), "", "every result is staged")
        self.assertIn("regen_all: every generated file is current", out)
        _git(self.repo, "commit", "-q", "--no-edit")  # the merge finishes as an ordinary commit

    def test_an_output_git_merged_cleanly_but_stale_is_regenerated_too(self) -> None:
        # The real case: two HLE changes each move one NID between classes, so both sides write the
        # same new summary count and Git merges it without a conflict, one short of the truth.
        self._branches()
        self._merge("--no-edit")
        self.assertEqual((self.repo / COUNT).read_text(encoding="utf-8"), "count: 4\n")
        rc, out = self._resolve()
        self.assertEqual(rc, 0, out)
        self.assertEqual((self.repo / COUNT).read_text(encoding="utf-8"), "count: 5\n")
        self.assertIn(f"WROTE count: {COUNT}", out)

    def test_hand_written_text_around_a_generated_block_merges_from_both_sides(self) -> None:
        self._branches(a_top="top prose, edited by A", b_bottom="bottom prose, edited by B")
        self._merge("--no-edit")
        rc, out = self._resolve()
        self.assertEqual(rc, 0, out)
        self.assertEqual((self.repo / DOC).read_text(encoding="utf-8"),
                         _doc("top prose, edited by A", _block(["a", "A", "m", "B", "z"]), "bottom prose, edited by B"))

    def test_a_conflict_in_hand_written_text_around_the_block_is_refused(self) -> None:
        _git(self.repo, "checkout", "-q", "-b", "pr-a", "main")
        self._commit_tree("a\nA\nm\nz\n", "A's top", "bottom prose", "pr-a")
        _git(self.repo, "checkout", "-q", "-b", "pr-b", "main")
        self._commit_tree("a\nm\nB\nz\n", "B's top", "bottom prose", "pr-b")
        self._merge("--no-edit")
        before = (self.repo / GEN).read_bytes()
        rc, out = self._resolve()
        self.assertEqual(rc, 1, out)
        self.assertIn(f"CONFLICT {DOC}", out)
        self.assertEqual(set(self._unmerged()), {GEN, DOC}, "nothing is resolved while one file needs a person")
        self.assertEqual((self.repo / GEN).read_bytes(), before)

    def test_a_conflict_outside_the_generated_files_is_refused_untouched(self) -> None:
        _git(self.repo, "checkout", "-q", "-b", "pr-a", "main")
        self._commit_tree("a\nA\nz\n", "top prose", "bottom prose", "pr-a")  # replaces m
        _git(self.repo, "checkout", "-q", "-b", "pr-b", "main")
        self._commit_tree("a\nB\nz\n", "top prose", "bottom prose", "pr-b")  # also replaces m
        proc = _git(self.repo, "merge", "--no-edit", "pr-a", check=False)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("src/items.txt", self._unmerged())
        rc, out = self._resolve()
        self.assertEqual(rc, 1, out)
        self.assertIn("CONFLICT src/items.txt: not a generated file", out)
        self.assertIn("src/items.txt", self._unmerged())
        self.assertIn(GEN, self._unmerged())
        self.assertEqual(self.renders, 0, "no generator runs on a tree that still holds conflicts")

    def test_a_skipped_steps_output_is_not_taken_from_either_side(self) -> None:
        self.assertNotIn("src/shader.inc", regen_all._resolvable_paths())
        self.assertIn(GEN, regen_all._resolvable_paths())

    def test_a_generated_file_deleted_on_one_side_is_refused(self) -> None:
        _git(self.repo, "checkout", "-q", "-b", "pr-a", "main")
        (self.repo / GEN).unlink()
        _git(self.repo, "commit", "-q", "-am", "pr-a drops the output")
        _git(self.repo, "checkout", "-q", "-b", "pr-b", "main")
        self._commit_tree("a\nm\nB\nz\n", "top prose", "bottom prose", "pr-b")
        proc = _git(self.repo, "merge", "--no-edit", "pr-a", check=False)
        self.assertNotEqual(proc.returncode, 0)
        rc, out = self._resolve()
        self.assertEqual(rc, 1, out)
        self.assertIn(f"CONFLICT {GEN}: deleted on one side", out)

    def test_an_output_still_stale_after_regeneration_refuses_to_finish(self) -> None:
        self._branches()
        self._merge("--no-edit")
        drifting = iter(range(1000))
        unstable = regen_all.Step("unstable", "unstable", (GEN,), lambda tmp: {GEN: f"{next(drifting)}\n".encode()})
        with mock.patch.object(regen_all, "STEPS", (unstable, *self.steps[1:])):
            rc, out = self._resolve()
        self.assertEqual(rc, 1, out)
        self.assertIn("STALE unstable", out)
        self.assertIn("refusing to finish the merge", out)

    def test_a_true_merge_defers_the_provenance_refresh_to_its_own_commit(self) -> None:
        self._branches()
        self._merge("--no-edit")
        rc, out = self._resolve()
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.provenance_calls, [], "a merge's provenance base exists only once it is committed")
        self.assertIn("NOT_RUN provenance controls", out)
        self.assertIn(f"then run {regen_all.FIX_COMMAND}", out)

    def test_a_squash_merge_refreshes_and_checks_the_provenance_controls(self) -> None:
        self._branches()  # pr-b stands for the batch branch the desk folds each pull request into
        self._merge("--squash", "--no-commit")
        self.assertFalse(_git(self.repo, "rev-parse", "-q", "--verify", "MERGE_HEAD", check=False).stdout.strip())
        rc, out = self._resolve()
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.provenance_calls, [False, True], "refreshed, then checked")
        self.assertEqual(self._unmerged(), [])

    def test_conflicted_controls_that_cannot_be_refreshed_refuse_to_finish(self) -> None:
        ledger = "assets/public_provenance_ledger.json"
        (self.repo / "assets").mkdir()
        (self.repo / ledger).write_text("{}\n", encoding="utf-8")
        _git(self.repo, "add", "-A")
        _git(self.repo, "commit", "-q", "-m", "ledger")
        _git(self.repo, "checkout", "-q", "-b", "pr-a", "main")
        (self.repo / ledger).write_text('{"a": 1}\n', encoding="utf-8")
        _git(self.repo, "commit", "-q", "-am", "a")
        _git(self.repo, "checkout", "-q", "-b", "pr-b", "main")
        (self.repo / ledger).write_text('{"b": 1}\n', encoding="utf-8")
        _git(self.repo, "commit", "-q", "-am", "b")
        _git(self.repo, "checkout", "-q", "main")
        _git(self.repo, "merge", "-q", "--ff-only", "pr-b")
        self.assertNotEqual(_git(self.repo, "merge", "--squash", "--no-commit", "pr-a", check=False).returncode, 0)
        self.provenance_result = ("SKIP", [], "NEW_PUBLIC_PATH (x): needs maintainer authority")
        rc, out = self._resolve()
        self.assertEqual(rc, 1, out)
        self.assertIn("the provenance controls conflicted and could not be refreshed", out)

    def test_docs_name_the_command(self) -> None:
        text = (regen_all.TOOLS.parent / "docs" / "CI.md").read_text(encoding="utf-8")
        self.assertIn("## Merging regenerated files", text)
        self.assertIn(regen_all.MERGE_COMMAND, text)


if __name__ == "__main__":
    unittest.main()
