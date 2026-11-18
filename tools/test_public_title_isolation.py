#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Regression test suite for public/private title isolation.

Verifies:
1. Public title catalog only includes titles authorized by assets/public_source_profile.json.
2. Dropping an excluded or unclassified manifest into assets/titles/ does NOT leak into public C codegen.
3. tools/title_catalog_codegen.py --verify fails closed if generated C contains unauthorized titles or is out of sync.
4. The only tracked retail title manifest is the public HST manifest,
   and it is policy-included; no unclassified manifest is tracked.
5. The in-memory private overlay mechanism allows local acceptance without modifying public artifacts.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from nk_core.git_isolation import run_git
import publication_policy
import title_catalog_codegen
import title_manifest
from nk_core.title_registry import TitleRegistry


def tracked_paths(*filters: str) -> set[str]:
    """The repository's tracked file set, identical in any checkout state."""
    cmd = ["git", "ls-files", "--", *filters] if filters else ["git", "ls-files"]
    out = subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True, check=True)
    return {line.replace("\\", "/") for line in out.stdout.splitlines() if line.strip()}


def tracked_titles() -> list[Path]:
    """Tracked title manifests only -- the hermetic enumeration for public tests.

    Untracked or ignored files in assets/titles/ (private development
    manifests) are invisible to this listing, so a test consuming it cannot
    change behavior when a developer's private inputs exist on disk.
    """
    names = sorted(tracked_paths("assets/titles/*.json"))
    return [ROOT / "assets" / "titles" / name.split("/")[-1] for name in names]


def is_tracked(rel_posix: str) -> bool:
    """True when the exact path currently has tracked bytes in the index."""
    return rel_posix in tracked_paths(rel_posix)


#: Suffix of every manifest these tests plant in the checkout. ``.gitignore`` ignores
#: ``/assets/titles/*.nk-test-planted.json``, so a planted file never shows in ``git status``
#: as an untracked candidate, even while a test runs or after it is killed.
PLANTED_SUFFIX = ".nk-test-planted.json"


def is_ignored(rel_posix: str) -> bool:
    """True when Git's ignore rules cover the path (``git check-ignore``)."""
    probe = subprocess.run(["git", "check-ignore", "--quiet", "--no-index", "--", rel_posix],
                           cwd=str(ROOT), capture_output=True, text=True)
    if probe.returncode not in (0, 1):
        raise AssertionError(f"git check-ignore failed for {rel_posix}: {probe.stderr}")
    return probe.returncode == 0


def plant_untracked_manifest(case: unittest.TestCase, name: str, manifest: dict) -> Path:
    """Put an untracked, Git-ignored manifest in the checkout's assets/titles for one test.

    The exclusion tests are vacuous unless the file they must ignore exists. ``name`` must end
    in ``PLANTED_SUFFIX``, which ``.gitignore`` covers, so the planted file is ignored by Git
    and never appears as an untracked candidate. A file of that name already on disk is used
    as it is and never removed; otherwise the planted file is removed when the test ends.
    """
    rel = f"assets/titles/{name}"
    path = ROOT / rel
    case.assertTrue(name.endswith(PLANTED_SUFFIX), f"{name} must end in {PLANTED_SUFFIX}")
    case.assertFalse(is_tracked(rel), f"{name} must stay untracked")
    case.assertTrue(is_ignored(rel), f"{name} must be ignored by .gitignore")
    try:
        with path.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(manifest, stream, indent=2)
            stream.write("\n")
    except FileExistsError:
        return path
    case.addCleanup(path.unlink, missing_ok=True)
    return path


PRIVATE_TITLE_TESTS_ENV = "NK_PRIVATE_TITLE_TESTS"

#: The one retail manifest the public tree carries.
PUBLIC_RETAIL_MANIFEST = "hst-ucus98701.json"
PUBLIC_RETAIL_ID = "hst-ucus98701-v1"
PUBLIC_RETAIL_DISC_ID = "UCUS98701"
#: Another region's id for the same title: never catalogued.
UNCATALOGUED_RETAIL_DISC_ID = "UCES01402"


def private_title_tests_enabled(rel_posix: str) -> bool:
    """Whether a private-input acceptance test may run against ``rel_posix``.

    It runs where the path has tracked bytes (the private authority checkout),
    or where the developer explicitly opts in with NK_PRIVATE_TITLE_TESTS=1 and
    the ignored local file exists. Mere presence of an ignored file never opens
    the gate, so the default public result is identical in any checkout (#335).
    """
    if is_tracked(rel_posix):
        return True
    return os.environ.get(PRIVATE_TITLE_TESTS_ENV) == "1" and (ROOT / rel_posix).is_file()


class PublicTitleIsolationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.policy_path = ROOT / "assets" / "public_source_profile.json"
        self.policy = publication_policy.load_policy(self.policy_path)
        self.titles_dir = ROOT / "assets" / "titles"

    def test_tracked_titles_are_all_public_included(self) -> None:
        """Every tracked title in assets/titles/ must be explicitly INCLUDED by policy.

        The contract is about what the repository tracks, so enumerate the index
        rather than the directory: assets/titles/ can also hold a developer's
        untracked private manifest, and only tracked, included files are public.
        """
        json_files = tracked_titles()
        self.assertGreater(len(json_files), 0, "Expected at least one public title manifest")

        for mf in json_files:
            rel = mf.relative_to(ROOT).as_posix()
            res = self.policy.resolve(rel)
            self.assertEqual(
                res.disposition,
                publication_policy.INCLUDED,
                f"Manifest {rel} must be INCLUDED by public source policy, got {res.disposition}"
            )
            self.assertFalse(
                res.is_excluded,
                f"Manifest {rel} is marked as excluded in publication policy!"
            )
            # Retail-looking names are allowed only for the approved public manifest.
            if "hst" in mf.name.lower() or "ucus98701" in mf.name.lower():
                self.assertEqual(mf.name, PUBLIC_RETAIL_MANIFEST)
            self.assertNotIn("uces01402", mf.name.lower())
        self.assertIn(PUBLIC_RETAIL_MANIFEST, [mf.name for mf in json_files])

    def test_catalog_generator_excludes_unauthorized_and_private_manifests(self) -> None:
        """Dropping a private manifest into assets/titles must be ignored by public collector."""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp_titles = Path(tmpdir)
            # Copy over current public manifests
            for mf in self.titles_dir.glob("*.json"):
                shutil.copy2(mf, tmp_titles / mf.name)

            # Inject a fake retail manifest under a name the policy never classified
            fake_retail = {
                "schema_version": 1,
                "id": "private-retail-lookalike-v1",
                "display_name": "Private Injection Test",
                "kind": "retail",
                "game_base": "0x00000000",
                "game_entry": "0x00000000",
                "executables": [{"path": "PSP_GAME/SYSDIR/EBOOT.BIN", "role": "main"}],
                "disc": {
                    "primary_disc_id": UNCATALOGUED_RETAIL_DISC_ID,
                    "compatible_disc_ids": []
                }
            }
            retail_file = tmp_titles / PRIVATE_LOOKALIKE_NAME
            retail_file.write_text(json.dumps(fake_retail), encoding="utf-8")

            # Inject unclassified arbitrary manifest
            unclassified = {
                "schema_version": 1,
                "id": "unclassified-title-v1",
                "display_name": "Unclassified Title",
                "kind": "retail",
                "game_base": "0x08804000",
                "game_entry": "0x08804000",
                "executables": [{"path": "EBOOT.BIN", "role": "main"}],
                "disc": {
                    "primary_disc_id": "ULUS99999",
                    "compatible_disc_ids": []
                }
            }
            unclass_file = tmp_titles / "unclassified.json"
            unclass_file.write_text(json.dumps(unclassified), encoding="utf-8")

            # Collect public manifests using the real publication policy
            # collect_public_manifests now refuses an out-of-repository
            # directory outright, because a path outside ROOT has no
            # publication-policy identity. These manifests are verbatim copies
            # of the tracked ones, so this test states explicitly which
            # in-repository directory's decisions apply to the copies.
            manifest_files, validated_titles = title_catalog_codegen.collect_public_manifests(
                tmp_titles, policy=self.policy, policy_dir=self.titles_dir
            )

            collected_names = [p.name for p in manifest_files]
            collected_ids = [t["id"] for t in validated_titles]

            # Neither the private lookalike nor the unclassified manifest may be included
            self.assertNotIn(PRIVATE_LOOKALIKE_NAME, collected_names)
            self.assertNotIn("unclassified.json", collected_names)
            self.assertNotIn("private-retail-lookalike-v1", collected_ids)
            self.assertNotIn("unclassified-title-v1", collected_ids)

            # Public synthetic titles and the public HST manifest MUST be present
            self.assertIn("pspdev-phase5-v1", collected_ids)
            self.assertIn("synthetic-allegrex-v1", collected_ids)
            self.assertIn(PUBLIC_RETAIL_MANIFEST, collected_names)
            self.assertIn(PUBLIC_RETAIL_ID, collected_ids)

    def test_generated_c_contains_only_the_public_retail_title(self) -> None:
        """The generated C catalog names exactly the policy-included retail titles, nothing else."""
        gen_dir = ROOT / "src" / "core" / "generated"
        cat_h = gen_dir / "nk_title_catalog.h"
        cat_c = gen_dir / "nk_title_catalog.c"

        self.assertTrue(cat_h.is_file(), "nk_title_catalog.h does not exist")
        self.assertTrue(cat_c.is_file(), "nk_title_catalog.c does not exist")

        text_h = cat_h.read_text(encoding="utf-8")
        text_c = cat_c.read_text(encoding="utf-8")
        combined = text_h + "\n" + text_c

        forbidden_tokens = [UNCATALOGUED_RETAIL_DISC_ID, "private-retail-lookalike", "sha256"]
        for token in forbidden_tokens:
            self.assertNotIn(token, combined, f"Forbidden retail token '{token}' leaked into generated C catalog!")

        # Every retail entry comes from a tracked, policy-included manifest.
        included_retail = set()
        for mf in tracked_titles():
            manifest = title_manifest.load_manifest(mf)
            rel = mf.relative_to(ROOT).as_posix()
            if manifest["kind"] == "retail" and self.policy.resolve(rel).disposition == publication_policy.INCLUDED:
                included_retail.add(manifest["id"])
        self.assertEqual(included_retail, {PUBLIC_RETAIL_ID})
        self.assertEqual(text_c.count("NK_TITLE_KIND_RETAIL,"), len(included_retail))
        self.assertEqual(text_c.count(f'"{PUBLIC_RETAIL_DISC_ID}"'), 1)
        self.assertEqual(text_c.count(f'"{PUBLIC_RETAIL_ID}"'), 1)

    def test_verify_flag_detects_out_of_date_or_corrupted_catalog(self) -> None:
        """--verify passes on the tracked catalog and fails on a corrupted or missing copy of it.

        The negative cases run against a temporary copy of the generated files (--output-dir), so
        the tracked catalog is never touched; manifest drift is covered by tools/test_title_catalog.py.
        """
        cmd = [sys.executable, str(ROOT / "tools" / "title_catalog_codegen.py"), "--verify"]
        proc = subprocess.run(cmd, capture_output=True, text=True, cwd=str(ROOT))
        self.assertEqual(proc.returncode, 0, f"--verify failed on clean tree:\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}")

        gen_dir = ROOT / "src" / "core" / "generated"
        with tempfile.TemporaryDirectory(prefix="catalog-verify-") as tmp:
            out = Path(tmp)
            for name in ("nk_title_catalog.h", "nk_title_catalog.c"):
                shutil.copyfile(gen_dir / name, out / name)
            verify_copy = cmd + ["--output-dir", str(out)]
            proc = subprocess.run(verify_copy, capture_output=True, text=True, cwd=str(ROOT))
            self.assertEqual(proc.returncode, 0, proc.stderr)

            source = out / "nk_title_catalog.c"
            source.write_bytes(source.read_bytes() + b"/* corrupted */\n")
            proc = subprocess.run(verify_copy, capture_output=True, text=True, cwd=str(ROOT))
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            self.assertIn("Native public title catalog is out of date", proc.stderr)

            (out / "nk_title_catalog.h").unlink()
            proc = subprocess.run(verify_copy, capture_output=True, text=True, cwd=str(ROOT))
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            self.assertIn("Generated public catalog files do not exist", proc.stderr)

    def test_in_memory_private_overlay_registry(self) -> None:
        """TitleRegistry loads public titles by default, and can load private manifest on demand."""
        # The default registry already loaded the tracked catalog (self.titles_dir);
        # loading it again is a re-registration, which standard mode now refuses.
        reg = TitleRegistry()

        # Public titles are loaded
        self.assertIsNotNone(reg.find_by_disc_id("TEST00001"))
        self.assertIsNotNone(reg.find_by_disc_id("TEST00005"))

        # The public HST manifest is the built-in hst title; another region's id is not catalogued
        hst = reg.find_by_disc_id(PUBLIC_RETAIL_DISC_ID)
        self.assertIsNotNone(hst)
        self.assertEqual(hst.id, PUBLIC_RETAIL_ID)
        self.assertEqual(hst.codegen_profile, "hst")
        self.assertEqual(hst.runtime_profile, "hst-current")
        self.assertIsNone(reg.find_by_disc_id(UNCATALOGUED_RETAIL_DISC_ID))

        # Load private overlay from simulated manifest
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            fake_private = {
                "schema_version": 1,
                "id": "private-overlay-test-v1",
                "display_name": "Private Overlay Title",
                "kind": "synthetic",
                "executable": {
                    "base": 142622720,
                    "entry": 142622720,
                    "bss_metadata_source": "elf",
                    "extra_executable_spans": []
                },
                "modules": [],
                "filesystem": {
                    "data_root": "tests/fixtures/synthetic",
                    "memory_stick_root": "build/test/memstick",
                    "device_prefixes": ["host0:", "ms0:"]
                },
                "hle_profile": "synthetic-minimal",
                "codegen_profile": "none",
                "feature_requirements": ["allegrex-core"],
                "verification_profile": "synthetic-public",
                "runtime_bindings": {
                    "schema_version": 1,
                    "fallback_entry": 142622976,
                    "worker_thread_entry": 142623232,
                    "launcher_thread_entry": 142623488
                }
            }
            json.dump(fake_private, f)
            f_path = Path(f.name)

        try:
            entry = reg.load_private_manifest(f_path)
            self.assertEqual(entry.id, "private-overlay-test-v1")
            found = reg.find_by_id("private-overlay-test-v1")
            self.assertIsNotNone(found)
            self.assertEqual(found.id, "private-overlay-test-v1")
        finally:
            if f_path.is_file():
                f_path.unlink()

    def test_included_non_title_json_fails_closed(self) -> None:
        """A non-title JSON file placed in manifests must be rejected / fail closed."""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp_titles = Path(tmpdir)
            for mf in self.titles_dir.glob("*.json"):
                shutil.copy2(mf, tmp_titles / mf.name)

            # Create an arbitrary non-title JSON file (e.g. package.json or config)
            non_title = {"name": "not-a-title-manifest", "version": "1.0.0", "dependencies": {}}
            (tmp_titles / "pspdev-phase5.json").write_text(json.dumps(non_title), encoding="utf-8")

            # Must raise when attempting to collect public manifests
            # policy_dir keeps this exercising manifest VALIDATION rejection
            # rather than the new out-of-repository refusal.
            with self.assertRaises(title_manifest.TitleManifestError):
                title_catalog_codegen.collect_public_manifests(
                    tmp_titles, policy=self.policy, policy_dir=self.titles_dir
                )

    def test_out_of_repository_manifest_dir_fails_closed(self) -> None:
        """A manifest outside ROOT must not borrow a tracked path's policy decision."""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp_titles = Path(tmpdir)
            # Schema-valid, but NOT the tracked bytes of that name. Before the
            # fix this was named assets/titles/synthetic.json for policy
            # purposes, inherited that path's INCLUDED decision, and was
            # emitted into the public catalog with no record of its own.
            forged = json.loads((self.titles_dir / "synthetic.json").read_text(encoding="utf-8"))
            forged["display_name"] = "Forged Out-Of-Tree Title"
            (tmp_titles / "synthetic.json").write_text(json.dumps(forged), encoding="utf-8")

            with self.assertRaises(ValueError) as ctx:
                title_catalog_codegen.collect_public_manifests(tmp_titles, policy=self.policy)
            self.assertIn("outside the repository root", str(ctx.exception))

    def test_policy_dir_outside_repository_is_refused(self) -> None:
        """The explicit mapping cannot itself point outside the repository."""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp_titles = Path(tmpdir)
            with self.assertRaises(ValueError) as ctx:
                title_catalog_codegen.collect_public_manifests(
                    tmp_titles, policy=self.policy, policy_dir=tmp_titles
                )
            self.assertIn("not inside the repository root", str(ctx.exception))

    def test_malformed_manifest_rejection(self) -> None:
        """Manifests missing required schema fields or with bad types must be rejected."""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp_titles = Path(tmpdir)
            for mf in self.titles_dir.glob("*.json"):
                shutil.copy2(mf, tmp_titles / mf.name)

            # Malformed manifest: unsupported schema_version 999
            bad_schema = {
                "schema_version": 999,
                "id": "bad-schema-v1",
                "display_name": "Bad Schema Title",
                "kind": "synthetic"
            }
            (tmp_titles / "pspdev-phase5.json").write_text(json.dumps(bad_schema), encoding="utf-8")

            # policy_dir keeps this exercising manifest VALIDATION rejection
            # rather than the new out-of-repository refusal.
            with self.assertRaises(title_manifest.TitleManifestError):
                title_catalog_codegen.collect_public_manifests(
                    tmp_titles, policy=self.policy, policy_dir=self.titles_dir
                )

    def test_private_overlay_has_zero_effect_on_verify(self) -> None:
        """An untracked private manifest in assets/titles must have zero effect on catalog verify."""
        # 1. Clean verify must succeed
        cmd = [sys.executable, str(ROOT / "tools" / "title_catalog_codegen.py"), "--verify"]
        proc = subprocess.run(cmd, capture_output=True, text=True, cwd=str(ROOT))
        self.assertEqual(proc.returncode, 0, f"--verify failed on baseline: {proc.stderr}")

        # 2. A private overlay where --verify reads manifests: the checkout's assets/titles,
        # untracked. Were it read, its id would change the digest and fail the verify.
        fake_overlay = {
            "schema_version": 1,
            "id": "private-retail-lookalike-v1",
            "display_name": "Private Retail Overlay",
            "kind": "retail",
            "disc": {"id": UNCATALOGUED_RETAIL_DISC_ID, "region": "EU", "revision_policy": "exact-disc-id"}
        }
        overlay_path = plant_untracked_manifest(
            self, "private-overlay-lookalike" + PLANTED_SUFFIX, fake_overlay)
        self.assertTrue(overlay_path.is_file())

        proc2 = subprocess.run(cmd, capture_output=True, text=True, cwd=str(ROOT))
        self.assertEqual(proc2.returncode, 0, f"--verify failed with a private overlay present: {proc2.stderr}")
        self.assertIn("is up to date", proc2.stdout)
        self.assertEqual(proc2.stdout, proc.stdout, "the overlay changed the verified catalog")
        catalog = (ROOT / "src" / "core" / "generated" / "nk_title_catalog.c").read_text(encoding="utf-8")
        self.assertNotIn("private-retail-lookalike-v1", catalog)

    def test_overlay_collision_policy(self) -> None:
        """Section 4: Conflicting overlay on canonical public title must fail closed unless overridden."""
        # The default registry already holds the tracked catalog (self.titles_dir).
        reg = TitleRegistry()

        # Create a manifest that conflicts with public title 'synthetic-allegrex-v1'
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            collision = {
                "schema_version": 1,
                "id": "synthetic-allegrex-v1",
                "display_name": "Colliding Overlay",
                "kind": "synthetic",
                "executable": {
                    "base": 142622720,
                    "entry": 142622720,
                    "bss_metadata_source": "elf",
                    "extra_executable_spans": []
                },
                "modules": [],
                "filesystem": {
                    "data_root": "tests/fixtures/synthetic",
                    "memory_stick_root": "build/test/memstick",
                    "device_prefixes": ["host0:", "ms0:"]
                },
                "hle_profile": "synthetic-minimal",
                "codegen_profile": "none",
                "feature_requirements": ["allegrex-core"],
                "verification_profile": "synthetic-public",
            }
            json.dump(collision, f)
            f_path = Path(f.name)

        try:
            # 1. Standard mode: MUST REJECT
            with self.assertRaises(RuntimeError) as ctx:
                reg.load_private_manifest(f_path, allow_override=False)
            self.assertIn("conflicts with public canonical catalog", str(ctx.exception))

            # 2. Explicit developer override mode: ACCEPT
            profile = reg.load_private_manifest(f_path, allow_override=True)
            self.assertEqual(profile.id, "synthetic-allegrex-v1")
            self.assertEqual(profile.name, "Colliding Overlay")
        finally:
            if f_path.is_file():
                f_path.unlink()


# A retail-looking manifest the policy never classified. It is the stand-in for
# any developer's private title manifest now that the HST manifest is public.
PRIVATE_LOOKALIKE_NAME = "private-retail-lookalike" + PLANTED_SUFFIX
PRIVATE_LOOKALIKE_REL = f"assets/titles/{PRIVATE_LOOKALIKE_NAME}"

SYNTHETIC_LOOKALIKE = {
    "schema_version": 1,
    "id": "private-retail-lookalike",
    "display_name": "Synthetic ignored lookalike (hermetic-suite regression)",
    "kind": "retail",
    "disc": {"id": "TEST00000", "region": "TEST", "revision_policy": "exact-disc-id"},
    "executable": {"base": 0, "entry": 0, "bss_metadata_source": "none", "extra_executable_spans": []},
    "modules": [],
    "filesystem": {
        "data_root": "synthetic/none",
        "memory_stick_root": "build/synthetic/memstick",
        "device_prefixes": ["host0:", "ms0:"],
    },
    "hle_profile": "synthetic-minimal",
    "codegen_profile": "none",
    "feature_requirements": ["allegrex-core"],
    "verification_profile": "synthetic-public",
}


#: One publication-audit finding line: ``CODE: path: message``.
AUDIT_FINDING_RE = re.compile(r"^[A-Z][A-Z0-9_]+: ")


def staged_index_audit(rel_path: str, blob_bytes: bytes) -> subprocess.CompletedProcess:
    """Run the real publication audit with `rel_path` staged in a temp index.

    The blob is written to a temp file and injected with ``git hash-object``
    plus ``update-index --cacheinfo`` under ``GIT_INDEX_FILE``, so neither the
    developer's worktree nor their real index is read or modified. The audit
    then sees exactly what it would see if this material had been staged for
    a candidate.
    """
    with tempfile.TemporaryDirectory(prefix="hermetic-stage-") as tmp:
        tmp_path = Path(tmp)
        blob_file = tmp_path / "blob"
        blob_file.write_bytes(blob_bytes)
        index_file = tmp_path / "index"
        env = {**os.environ, "GIT_INDEX_FILE": str(index_file)}
        subprocess.run(["git", "read-tree", "HEAD"], cwd=str(ROOT), env=env, check=True)
        sha = subprocess.run(
            ["git", "hash-object", "-w", str(blob_file)],
            cwd=str(ROOT), capture_output=True, text=True, check=True,
        ).stdout.strip()
        subprocess.run(
            ["git", "update-index", "--add", "--cacheinfo", f"100644,{sha},{rel_path}"],
            cwd=str(ROOT), env=env, check=True,
        )
        return subprocess.run(
            [sys.executable, "tools/publish_audit.py", "--public-scope",
             "--provenance-self-consistency"],
            cwd=str(ROOT), env=env, capture_output=True, text=True, timeout=600,
        )


class TrackedSetHelperTests(unittest.TestCase):
    """The canonical enumeration must be ambient-state independent."""

    def test_tracked_titles_is_deterministic_and_ignores_untracked(self):
        names = [p.name for p in tracked_titles()]
        self.assertEqual(names, sorted(set(names)))
        self.assertEqual(
            names,
            sorted(p.split("/")[-1] for p in tracked_paths("assets/titles/*.json")),
        )
        self.assertNotIn(PRIVATE_LOOKALIKE_NAME, names)
        self.assertIn(PUBLIC_RETAIL_MANIFEST, names)
        self.assertGreater(len(names), 0, "public tree must keep at least one manifest")

    def test_is_tracked_gates_private_fixture_explicitly(self):
        self.assertFalse(is_tracked(PRIVATE_LOOKALIKE_REL))
        self.assertTrue(is_tracked("assets/titles/synthetic.json"))
        self.assertTrue(is_tracked(f"assets/titles/{PUBLIC_RETAIL_MANIFEST}"))
        self.assertFalse(is_tracked("assets/titles/does-not-exist.json"))

    def test_private_title_tests_open_only_on_tracking_or_explicit_opt_in(self):
        from unittest import mock
        present = ROOT / "tools" / "test_public_title_isolation.py"
        rel = present.relative_to(ROOT).as_posix()
        with mock.patch(__name__ + ".is_tracked", return_value=False):
            with mock.patch.dict(os.environ, {PRIVATE_TITLE_TESTS_ENV: ""}):
                self.assertFalse(private_title_tests_enabled(rel))
            with mock.patch.dict(os.environ, {PRIVATE_TITLE_TESTS_ENV: "1"}):
                self.assertTrue(private_title_tests_enabled(rel))
                self.assertFalse(private_title_tests_enabled("assets/titles/does-not-exist.json"))
        with mock.patch(__name__ + ".is_tracked", return_value=True):
            with mock.patch.dict(os.environ, {PRIVATE_TITLE_TESTS_ENV: ""}):
                self.assertTrue(private_title_tests_enabled(rel))


class CollectorHermeticityTests(unittest.TestCase):
    """Policy-keyed collection equals the tracked set in any checkout state."""

    def test_collector_output_equals_tracked_set(self):
        manifests, titles = title_catalog_codegen.collect_public_manifests(ROOT / "assets" / "titles")
        collected = sorted(p.name for p in manifests)
        tracked = sorted(p.name for p in tracked_titles())
        self.assertEqual(collected, tracked)
        self.assertNotIn(PRIVATE_LOOKALIKE_NAME, collected)
        self.assertIn(PUBLIC_RETAIL_MANIFEST, collected)
        ids = [t["id"] for t in titles]
        self.assertEqual(ids, sorted(set(ids)), "collector output must be deterministic")

    def test_registry_policy_filter_ignores_lookalike(self):
        lookalike = plant_untracked_manifest(self, PRIVATE_LOOKALIKE_NAME, SYNTHETIC_LOOKALIKE)
        self.assertTrue(lookalike.is_file())
        registry = TitleRegistry(include_defaults=False)
        registry.load_from_directory(ROOT / "assets" / "titles")
        ids = {p.id for p in registry.all_profiles()}
        self.assertNotIn("private-retail-lookalike", ids)
        self.assertIn(PUBLIC_RETAIL_ID, ids)
        manifest_count = len(list((ROOT / "assets" / "titles").glob("*.json")))
        self.assertLessEqual(len(ids), manifest_count)

    def test_ambient_glob_sensitivity_mechanism(self):
        """Control: a bare glob does see a planted file, so the exclusions above are not vacuous.

        This checks pathlib only; the collector and the registry are exercised by the tests above
        and by SyntheticIgnoredInputsRegressionTests.
        """
        with tempfile.TemporaryDirectory(prefix="hermetic-glob-") as tmp:
            titles = Path(tmp) / "assets" / "titles"
            titles.mkdir(parents=True)
            (titles / "public.json").write_text("{}", encoding="utf-8")
            before = sorted(p.name for p in titles.glob("*.json"))
            (titles / PRIVATE_LOOKALIKE_NAME).write_text("{}", encoding="utf-8")
            after = sorted(p.name for p in titles.glob("*.json"))
        self.assertEqual(before, ["public.json"])
        self.assertEqual(after, sorted(["public.json", PRIVATE_LOOKALIKE_NAME]))


class PublicHstManifestGateTests(unittest.TestCase):
    """The public HST manifest tests fail, never skip, when the manifest is not tracked."""

    def test_hst_manifest_tests_fail_rather_than_skip_when_untracked(self):
        import test_hst_manager_manifest
        import test_hst_title_manifest
        import test_public_title_isolation

        with mock.patch.object(test_public_title_isolation, "is_tracked", return_value=False):
            title_case = test_hst_title_manifest.HstTitleManifestTests(
                "test_manifest_is_tracked_and_public_included")
            result = unittest.TestResult()
            title_case.run(result)
            self.assertEqual(len(result.failures), 1, result.failures + result.errors)
            self.assertEqual(result.skipped, [])
            self.assertIn("must be tracked", result.failures[0][1])
            case = test_hst_manager_manifest.HstManagerManifestTests("require_hst_manifest")
            with self.assertRaises(AssertionError) as manager_ctx:
                case.require_hst_manifest()
        self.assertIn("must be tracked", str(manager_ctx.exception))


class PrivateEbootSuiteOptInTests(unittest.TestCase):
    """Private EBOOT test classes require explicit opt-in even when input exists."""

    CASES = (
        ("tools/test_analyze_tailcall.py", "TestTailcallPromotionOnEboot"),
        ("tools/test_codegen_no_shadow_stubs.py", "TestNoShadowStubs"),
        ("tools/test_decompme_export.py", "TestExportAgainstRealEboot"),
    )

    def _load_case_with_synthetic_eboot(
        self, rel_path: str, class_name: str, *, opted_in: bool, input_present: bool
    ):
        source = ROOT / rel_path
        eboot = ROOT / "place_game_here" / "EBOOT.elf"
        eboot_key = os.path.normcase(os.path.abspath(os.fspath(eboot)))
        original_isfile = os.path.isfile
        original_exists = Path.exists
        original_is_file = Path.is_file

        def is_eboot(candidate) -> bool:
            returned: bool = os.path.normcase(os.path.abspath(os.fspath(candidate))) == eboot_key
            return returned

        def fake_isfile(candidate) -> bool:
            return input_present if is_eboot(candidate) else original_isfile(candidate)

        def fake_exists(candidate) -> bool:
            return input_present if is_eboot(candidate) else original_exists(candidate)

        def fake_is_file(candidate) -> bool:
            return input_present if is_eboot(candidate) else original_is_file(candidate)

        namespace = {"__name__": "_private_eboot_suite_gate_probe", "__file__": str(source)}
        with mock.patch.dict(os.environ):
            os.environ.pop(PRIVATE_TITLE_TESTS_ENV, None)
            if opted_in:
                os.environ[PRIVATE_TITLE_TESTS_ENV] = "1"
            with (
                mock.patch("os.path.isfile", side_effect=fake_isfile),
                mock.patch.object(Path, "exists", fake_exists),
                mock.patch.object(Path, "is_file", fake_is_file),
            ):
                code = compile(source.read_text(encoding="utf-8"), str(source), "exec")
                exec(code, namespace)
        return namespace[class_name]

    def test_private_eboot_classes_require_explicit_opt_in(self):
        for rel_path, class_name in self.CASES:
            with self.subTest(path=rel_path, opted_in=False, input_present=True):
                case = self._load_case_with_synthetic_eboot(
                    rel_path, class_name, opted_in=False, input_present=True
                )
                self.assertTrue(getattr(case, "__unittest_skip__", False), f"{rel_path} opened with only an ambient EBOOT")
                self.assertIn(PRIVATE_TITLE_TESTS_ENV + "=1", getattr(case, "__unittest_skip_why__", ""))

            with self.subTest(path=rel_path, opted_in=True, input_present=True):
                case = self._load_case_with_synthetic_eboot(
                    rel_path, class_name, opted_in=True, input_present=True
                )
                self.assertFalse(getattr(case, "__unittest_skip__", False), f"{rel_path} did not open after explicit opt-in")

            with self.subTest(path=rel_path, opted_in=True, input_present=False):
                case = self._load_case_with_synthetic_eboot(
                    rel_path, class_name, opted_in=True, input_present=False
                )
                self.assertTrue(getattr(case, "__unittest_skip__", False), f"{rel_path} opened without its EBOOT")


class PublicationEnforcementTests(unittest.TestCase):
    """Untracked/ignored material stays invisible; staged material must fail."""

    def test_publication_audit_ignores_untracked_ignored_lookalike(self):
        lookalike = plant_untracked_manifest(self, PRIVATE_LOOKALIKE_NAME, SYNTHETIC_LOOKALIKE)
        self.assertTrue(lookalike.is_file())
        proc = subprocess.run(
            [sys.executable, "tools/publish_audit.py", "--tracked-only",
             "--public-scope", "--provenance-self-consistency"],
            cwd=str(ROOT), capture_output=True, text=True, timeout=600,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertNotIn(PRIVATE_LOOKALIKE_NAME, proc.stdout + proc.stderr)

    def test_staged_private_manifest_is_rejected(self):
        proc = staged_index_audit(
            PRIVATE_LOOKALIKE_REL,
            (json.dumps(SYNTHETIC_LOOKALIKE, indent=2) + "\n").encode("utf-8"),
        )
        self.assertNotEqual(proc.returncode, 0, "staged private lookalike must be rejected")
        combined = proc.stdout + proc.stderr
        self.assertIn("private-retail-lookalike", combined, f"rejection must name the path: {combined[:800]}")

    def test_staged_generated_source_without_its_header_is_rejected(self):
        """A staged generated source that lost its SPDX header fails the index leg, by name.

        Disc ids are not game data (AGENTS.md), so the audit has no disc-id finding and this test
        claims none: what it pins is that staged bytes, not the worktree's, are audited.
        """
        rel = "src/core/generated/nk_title_catalog.c"
        proc = staged_index_audit(rel, b"/* synthetic staged replacement for hermetic-suite regression */\n")
        combined = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 1, combined[-2000:])
        self.assertIn(f"SPDX: {rel}: missing SPDX identifier", proc.stderr)

    def test_audit_index_leg_matches_worktree_leg_on_clean_tree(self):
        def leg(*extra: str) -> tuple[subprocess.CompletedProcess, list[str]]:
            proc = subprocess.run(
                [sys.executable, "tools/publish_audit.py", "--tracked-only", *extra, "--public-scope",
                 "--provenance-self-consistency"],
                cwd=str(ROOT), capture_output=True, text=True, timeout=600,
            )
            # Findings and a FAIL summary go to stderr, an OK summary to stdout.
            findings = sorted(line for line in proc.stderr.splitlines() if AUDIT_FINDING_RE.match(line))
            return proc, findings

        index_leg, index_findings = leg()
        worktree_leg, worktree_findings = leg("--worktree")
        self.assertEqual(index_leg.returncode, 0, index_leg.stdout + index_leg.stderr)
        self.assertEqual(worktree_leg.returncode, index_leg.returncode,
                         worktree_leg.stdout + worktree_leg.stderr)
        self.assertEqual(worktree_findings, index_findings)
        self.assertIn("index content)", index_leg.stdout + index_leg.stderr)
        self.assertIn("worktree content)", worktree_leg.stdout + worktree_leg.stderr)


class SyntheticIgnoredInputsRegressionTests(unittest.TestCase):
    """The public title collector and registry give identical results whether or not
    ignored private inputs exist in the workspace (#335).

    Constructs a controlled temp workspace simulating ignored private-looking
    inputs using synthetic placeholder files (never real private inputs), and
    runs ``collect_public_manifests`` (with the in-repository policy mapping)
    and ``TitleRegistry.load_from_directory`` before and after they appear.
    """

    def test_synthetic_ignored_inputs_isolation(self) -> None:
        with tempfile.TemporaryDirectory(prefix="synthetic-workspace-") as tmp:
            repo_root = Path(tmp)
            run_git(["init"], cwd=repo_root, capture_output=True, check=True)

            titles_dir = repo_root / "assets" / "titles"
            titles_dir.mkdir(parents=True)
            pgh_dir = repo_root / "place_game_here" / "ISO"
            pgh_dir.mkdir(parents=True)
            build_dir = repo_root / "build" / "hst"
            build_dir.mkdir(parents=True)

            # A copy of the tracked public synthetic manifest: the collector judges it by the
            # policy decision for assets/titles/synthetic.json.
            public_manifest = titles_dir / "synthetic.json"
            shutil.copyfile(ROOT / "assets" / "titles" / "synthetic.json", public_manifest)
            policy = publication_policy.load_policy(ROOT / "assets" / "public_source_profile.json")

            def public_view() -> tuple[list[str], list[str], list[str]]:
                files, titles = title_catalog_codegen.collect_public_manifests(
                    titles_dir, policy=policy, policy_dir=ROOT / "assets" / "titles"
                )
                registry = TitleRegistry(include_defaults=False)
                registry.load_from_directory(titles_dir)
                return ([p.name for p in files], [t["id"] for t in titles],
                        sorted(profile.id for profile in registry.all_profiles()))

            gitignore = repo_root / ".gitignore"
            gitignore.write_text(
                f"/place_game_here/\n/build/\n/{PRIVATE_LOOKALIKE_REL}\n",
                encoding="utf-8",
            )

            run_git(["add", "."], cwd=repo_root, capture_output=True, check=True)
            run_git(["commit", "-m", "init", "--no-gpg-sign"], cwd=repo_root, capture_output=True, check=True)
            before = public_view()

            # Add synthetic ignored private inputs
            fake_private_manifest = titles_dir / PRIVATE_LOOKALIKE_NAME
            fake_private_manifest.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "id": "private-retail-lookalike",
                        "display_name": "Synthetic Lookalike",
                        "kind": "retail",
                        "disc": {"id": "TEST99999", "region": "TEST", "revision_policy": "exact-disc-id"},
                        "executable": {"base": 0, "entry": 0, "bss_metadata_source": "none", "extra_executable_spans": []},
                        "modules": [],
                        "filesystem": {
                            "data_root": "synthetic/none",
                            "memory_stick_root": "build/synthetic/memstick",
                            "device_prefixes": ["host0:", "ms0:"],
                        },
                        "hle_profile": "synthetic-minimal",
                        "codegen_profile": "none",
                        "feature_requirements": ["allegrex-core"],
                        "verification_profile": "synthetic-public",
                    },
                    indent=2,
                ),
                encoding="utf-8",
            )
            fake_eboot = repo_root / "place_game_here" / "EBOOT.elf"
            fake_eboot.write_bytes(b"\x7fELF-synthetic-placeholder")
            fake_iso = pgh_dir / "game.iso"
            fake_iso.write_bytes(b"synthetic-iso-placeholder")
            fake_obj = build_dir / "recomp.o"
            fake_obj.write_bytes(b"synthetic-build-artifact")

            # 1. Ambient glob sees both (vulnerable pattern)
            ambient_manifests = sorted(p.name for p in titles_dir.glob("*.json"))
            self.assertEqual(ambient_manifests, sorted([PRIVATE_LOOKALIKE_NAME, "synthetic.json"]))

            # 2. The real collector and registry do not: their output is unchanged by the lookalike.
            # The collector admits the copy under its in-repository policy identity; the registry
            # refuses a directory outside the repository outright, so it loads nothing either way.
            after = public_view()
            self.assertEqual(after, before)
            self.assertEqual(before[0], ["synthetic.json"])
            self.assertEqual(len(before[1]), 1)
            self.assertNotIn("private-retail-lookalike", before[1] + before[2])
            self.assertEqual(before[2], [])

            # 3. Tracked paths helper ignores the untracked / ignored lookalike
            tracked_out = run_git(
                ["ls-files", "assets/titles/*.json"],
                cwd=repo_root,
                capture_output=True,
                text=True,
                check=True,
            )
            tracked_set = [p.replace("\\", "/").split("/")[-1] for p in tracked_out.stdout.splitlines() if p.strip()]
            self.assertEqual(tracked_set, ["synthetic.json"])
            self.assertNotIn(PRIVATE_LOOKALIKE_NAME, tracked_set)

            # 4. Ignored private inputs are completely untracked
            all_tracked = run_git(
                ["ls-files"],
                cwd=repo_root,
                capture_output=True,
                text=True,
                check=True,
            ).stdout
            self.assertNotIn("place_game_here", all_tracked)
            self.assertNotIn("build/", all_tracked)
            self.assertNotIn(PRIVATE_LOOKALIKE_NAME, all_tracked)


if __name__ == "__main__":
    unittest.main()
