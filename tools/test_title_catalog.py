# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for title catalog generation, drift detection, and native C lookup parity."""

from __future__ import annotations

import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import title_catalog_codegen
import title_manifest
import test_public_title_isolation
from nk_core.title_registry import TitleRegistry


class SyntheticDiscIdTests(unittest.TestCase):
    """Synthetic disc-id assignment has exactly one home and fails closed.

    It used to live in two private copies -- one in tools/title_catalog_codegen.py
    for the native catalog, one in tools/nk_core/title_registry.py for the Python
    tooling -- and they drifted. Both then fell back to a SHARED "TEST00000"
    sentinel for an unassigned title, so the second unassigned title to appear
    collided with the first: the registry reported a conflict with the public
    canonical catalog that did not exist, and the native catalog emitted two
    entries claiming one disc id.
    """

    def test_native_catalog_projects_typed_loose_roots(self) -> None:
        manifest = title_manifest.load_manifest(ROOT / "assets" / "titles" / "synthetic.json")
        manifest["filesystem"]["loose_content_roots"] = [
            {"root": "second", "mount": "data", "precedence": 20},
            {"root": "first", "mount": "data", "precedence": 10},
        ]
        normalized = title_manifest.validate_manifest(manifest)
        header = title_catalog_codegen.generate_header("test-digest", [normalized])
        source = title_catalog_codegen.generate_source("test-digest", [normalized])
        self.assertIn("typedef struct {", header)
        self.assertIn("const NkLooseContentRoot *loose_content_roots;", header)
        self.assertIn("static const NkLooseContentRoot s_loose_roots_title_0[]", source)
        self.assertIn('{ "first", "data", 10U, false, NULL, 0 }', source)
        self.assertIn('{ "second", "data", 20U, false, NULL, 0 }', source)

    def test_both_modules_use_the_canonical_assignment(self) -> None:
        import title_catalog_codegen
        from nk_core import synthetic_disc_ids, title_registry

        self.assertIs(title_catalog_codegen.SYNTHETIC_DISC_ID_MAP,
                      synthetic_disc_ids.SYNTHETIC_DISC_IDS)
        self.assertIs(title_registry.SYNTHETIC_DISC_ID_MAP,
                      synthetic_disc_ids.SYNTHETIC_DISC_IDS)

    def test_the_generic_manifest_module_does_not_own_it(self) -> None:
        """tools/title_manifest.py is a generic helper and must not name a title.

        That is the contract tools/test_generic_title_planning_proof.py enforces,
        and it is the reason this assignment does not live there despite being
        manifest-shaped data.
        """
        import title_manifest

        self.assertFalse(hasattr(title_manifest, "SYNTHETIC_DISC_IDS"))

    def test_unassigned_synthetic_title_fails_closed(self) -> None:
        from nk_core import synthetic_disc_ids

        with self.assertRaisesRegex(synthetic_disc_ids.SyntheticDiscIdError,
                                    "has no assigned disc id"):
            synthetic_disc_ids.synthetic_disc_id("a-title-nobody-assigned-v1")

    def test_assignments_are_unique(self) -> None:
        from nk_core import synthetic_disc_ids

        values = list(synthetic_disc_ids.SYNTHETIC_DISC_IDS.values())
        self.assertEqual(len(values), len(set(values)),
                         "two synthetic titles share a disc id")

    def test_every_public_synthetic_manifest_is_assigned(self) -> None:
        import json
        from nk_core import synthetic_disc_ids

        # Tracked set, not the directory: an ignored private lookalike must
        # not enter the synthetic-disc-id sweep (#335).
        for manifest in test_public_title_isolation.tracked_titles():
            data = json.loads(manifest.read_text(encoding="utf-8"))
            if data.get("kind") != "synthetic":
                continue
            # Fails closed rather than returning a sentinel.
            synthetic_disc_ids.synthetic_disc_id(data["id"])


class TitleCatalogTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = Path(tempfile.mkdtemp(prefix="nk_catalog_test_"))

    def tearDown(self) -> None:
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def test_portability_doc_records_tracked_catalog_contract(self) -> None:
        """Keep the tracked-artifact rationale and exact regeneration commands documented."""
        portability_doc = (ROOT / "docs" / "PLATFORM_PORTABILITY.md").read_text(
            encoding="utf-8"
        )

        required_doc_fragments = (
            "src/core/generated/nk_title_catalog.h",
            "src/core/generated/nk_title_catalog.c",
            "intentionally tracked",
            "Make and CMake builds consume",
            "python tools/title_catalog_codegen.py",
            "python tools/title_catalog_codegen.py --verify",
            "TitleCatalogTests.test_live_catalog_verify_passes",
            "do not include generated binaries or private title data",
        )
        for fragment in required_doc_fragments:
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, portability_doc)

    def test_generated_header_states_snapshot_and_lock_contract(self) -> None:
        header = title_catalog_codegen.generate_header("test-digest", [])
        self.assertIn("package-validation cache has a separate lock", header)
        self.assertIn("owned copies", header)
        self.assertIn("must not call catalog APIs", header)

    def test_disc_id_lookup_uses_one_match_rule_for_overlays_and_static_entries(self) -> None:
        """Overlays and static entries share one primary-then-compatible match.

        Two hand-written copies of the loop can drift apart (they already
        differed on the empty-primary check), so the generator emits a single
        helper and both lookup passes call it.
        """
        source = title_catalog_codegen.generate_source("test-digest", [])
        start = source.index("const NkTitleEntry *nk_title_catalog_find_by_disc_id_locked(")
        end = source.index("bool nk_title_catalog_find_by_disc_id(", start)
        lookup = source[start:end]
        self.assertIn(
            "static bool entry_matches_disc_id(const NkTitleEntry *e, const char *norm)",
            source,
        )
        self.assertEqual(source.count("->compatible_disc_ids[c]"), 2)
        self.assertNotIn("compatible_disc_ids", lookup)
        self.assertIn("entry_matches_disc_id(ov, norm)", lookup)
        self.assertIn("entry_matches_disc_id(e, norm)", lookup)

    def test_live_catalog_verify_passes(self) -> None:
        """Verify that current live repo catalog matches assets/titles/ exactly."""
        res = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "title_catalog_codegen.py"), "--verify"],
            capture_output=True,
            text=True,
            cwd=str(ROOT),
        )
        self.assertEqual(res.returncode, 0, f"Catalog verification failed: {res.stderr}")
        self.assertIn("Native public title catalog is up to date", res.stdout)

    def test_catalog_codegen_drift_detection(self) -> None:
        """Verify that any modification to manifests causes --verify to fail."""
        # This used to stage a copy of assets/titles/ in a temp directory and
        # point --manifest-dir at it. The generator now refuses a manifest
        # directory outside the repository, because publication policy is
        # decided per tracked PATH and a copy in /tmp has no policy identity of
        # its own to be judged by. So drift is exercised where it actually
        # happens: against the tracked manifests, restored afterwards.
        temp_gen = self.temp_dir / "generated"

        def run_codegen(*extra: str) -> subprocess.CompletedProcess:
            return subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools" / "title_catalog_codegen.py"),
                    "--output-dir", str(temp_gen),
                    *extra,
                ],
                capture_output=True,
                text=True,
                cwd=str(ROOT),
            )

        res_gen = run_codegen()
        self.assertEqual(res_gen.returncode, 0, res_gen.stderr)

        # Verify passes initially
        res_ver = run_codegen("--verify")
        self.assertEqual(res_ver.returncode, 0, res_ver.stderr)

        # Mutate a tracked manifest, then always put it back.  Corrupting
        # the generated output instead would be trivially parallel-safe and
        # would test a weaker claim: that --verify notices a clobbered output
        # file, not that it notices a manifest whose generated output is now
        # stale.  The second is the regression this test exists for, so the
        # mutation stays and the module is listed in
        # discovery_contract._SERIAL_ONLY.
        # Mutating a tracked manifest verifies the real drift path; picking
        # [0] of an ambient glob could rewrite a developer's *private*
        # manifest instead (it is not even restored afterwards) (#335).
        manifest_to_modify = test_public_title_isolation.tracked_titles()[0]
        original_bytes = manifest_to_modify.read_bytes()
        try:
            data = json.loads(original_bytes.decode("utf-8"))
            data["display_name"] = "Drifted Title Name"
            manifest_to_modify.write_text(json.dumps(data, indent=2), encoding="utf-8")

            res_drift = run_codegen("--verify")
        finally:
            manifest_to_modify.write_bytes(original_bytes)

        self.assertNotEqual(res_drift.returncode, 0)
        self.assertIn("Native public title catalog is out of date", res_drift.stderr)

    def test_out_of_repository_manifest_dir_is_refused(self) -> None:
        """--manifest-dir outside the repository must fail, not borrow policy by name."""
        temp_titles = self.temp_dir / "titles"
        shutil.copytree(ROOT / "assets" / "titles", temp_titles)
        res = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools" / "title_catalog_codegen.py"),
                "--manifest-dir", str(temp_titles),
                "--output-dir", str(self.temp_dir / "generated"),
            ],
            capture_output=True,
            text=True,
            cwd=str(ROOT),
        )
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("outside the repository root", res.stderr)

    def test_native_c_catalog_compilation_and_lookup(self) -> None:
        """Compile generated C catalog with gcc and test native lookups in a C harness."""
        gcc = shutil.which("gcc")
        if not gcc:
            self.skipTest("TOOLCHAIN_MISSING: gcc not available in PATH on this host")

        test_c_file = self.temp_dir / "test_harness.c"
        cat_h = ROOT / "src" / "core" / "generated" / "nk_title_catalog.h"
        cat_c = ROOT / "src" / "core" / "generated" / "nk_title_catalog.c"

        # Derived from the manifests, not a literal: adding a public title
        # should not require editing a number in a test harness.  Ask the same
        # policy-aware collector the catalog generator uses instead of listing
        # the directory: assets/titles/ can also hold a developer's untracked
        # private manifest, which is never catalogued.
        expected_titles = len(
            title_catalog_codegen.collect_public_manifests(ROOT / "assets" / "titles")[0]
        )

        test_c_source = f"""#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "{cat_h.as_posix()}"

int main(void) {{
    printf("Catalog count: %d\\n", nk_title_catalog_count);
    assert(nk_title_catalog_count == {expected_titles});

    /* Test synthetic title lookups in public catalog */
    NkTitleEntrySnapshot t_synth = {{0}};
    assert(nk_title_catalog_find_by_disc_id("TEST00001", &t_synth));
    assert(strcmp(t_synth.entry.id, "synthetic-allegrex-v1") == 0);
    assert(t_synth.entry.kind == NK_TITLE_KIND_SYNTHETIC);

    NkTitleEntrySnapshot t_p5 = {{0}};
    assert(nk_title_catalog_find_by_disc_id("TEST00005", &t_p5));
    assert(strcmp(t_p5.entry.id, "pspdev-phase5-v1") == 0);

    /* Test hyphen and whitespace normalization */
    NkTitleEntrySnapshot t2 = {{0}};
    NkTitleEntrySnapshot t3 = {{0}};
    assert(nk_title_catalog_find_by_disc_id("test-00001", &t2));
    assert(strcmp(t2.entry.id, t_synth.entry.id) == 0);
    assert(nk_title_catalog_find_by_disc_id("  TEST_00005  ", &t3));
    assert(strcmp(t3.entry.id, t_p5.entry.id) == 0);

    /* The public HST manifest is the one catalogued retail title */
    NkTitleEntrySnapshot t_hst = {{0}};
    assert(nk_title_catalog_find_by_disc_id("UCUS-98701", &t_hst));
    assert(strcmp(t_hst.entry.id, "hst-ucus98701-v1") == 0);
    assert(t_hst.entry.kind == NK_TITLE_KIND_RETAIL);
    assert(strcmp(t_hst.entry.codegen_profile, "hst") == 0);
    assert(strcmp(t_hst.entry.hle_profile, "hst-current") == 0);
    nk_title_catalog_snapshot_release(&t_hst);

    /* Other retail IDs must return NULL in public catalog */
    NkTitleEntrySnapshot missing = {{0}};
    assert(!nk_title_catalog_find_by_disc_id("UCES01402", &missing));
    assert(!nk_title_catalog_find_by_disc_id("ULUS99999", &missing));

    /* Test in-memory private overlay */
    NkTitleEntry overlay;
    memset(&overlay, 0, sizeof(overlay));
    overlay.id = "private-test-v1";
    overlay.display_name = "Private Overlay Test";
    overlay.kind = NK_TITLE_KIND_RETAIL;
    overlay.primary_disc_id = "UCES01402";

    uint64_t start_epoch = nk_title_catalog_epoch();
    nk_title_catalog_register_overlay(&overlay);
    assert(nk_title_catalog_epoch() == start_epoch + 1);
    uint64_t overlay_epoch = nk_title_catalog_epoch();
    nk_title_catalog_register_overlay(&overlay);
    assert(nk_title_catalog_epoch() == overlay_epoch);
    NkTitleEntrySnapshot found_overlay = {{0}};
    NkTitleEntrySnapshot found_overlay_by_id = {{0}};
    assert(nk_title_catalog_find_by_disc_id("UCES01402", &found_overlay));
    assert(nk_title_catalog_find_by_id("private-test-v1", &found_overlay_by_id));
    assert(strcmp(found_overlay.entry.id, "private-test-v1") == 0);

    nk_title_catalog_clear_overlay();
    assert(strcmp(found_overlay.entry.display_name, "Private Overlay Test") == 0);
    assert(!nk_title_catalog_find_by_disc_id("UCES01402", &missing));
    assert(!nk_title_catalog_find_by_id("private-test-v1", &missing));
    uint64_t empty_epoch = nk_title_catalog_epoch();
    nk_title_catalog_clear_overlay();
    assert(nk_title_catalog_epoch() == empty_epoch);

    /* An overlay with no primary id matches through a compatible id, stored
       in any spelling the query normalization accepts. */
    static const char *const alias_ids[] = {{ "ulus-00007", NULL }};
    NkTitleEntry alias_overlay;
    memset(&alias_overlay, 0, sizeof(alias_overlay));
    alias_overlay.id = "alias-only-v1";
    alias_overlay.primary_disc_id = "";
    alias_overlay.compatible_disc_ids = alias_ids;
    assert(nk_title_catalog_register_overlay(&alias_overlay));
    NkTitleEntrySnapshot alias_found = {{0}};
    assert(nk_title_catalog_find_by_disc_id("ULUS00007", &alias_found));
    assert(strcmp(alias_found.entry.id, "alias-only-v1") == 0);
    nk_title_catalog_snapshot_release(&alias_found);
    assert(!nk_title_catalog_find_by_disc_id("ULUS00008", &missing));
    nk_title_catalog_clear_overlay();
    assert(!nk_title_catalog_find_by_disc_id("ULUS00007", &missing));

    NkTitleEntry capacity[9] = {{0}};
    char capacity_ids[9][32];
    char capacity_discs[9][32];
    for (int i = 0; i < 9; i++) {{
        snprintf(capacity_ids[i], sizeof(capacity_ids[i]), "capacity-title-%d", i);
        snprintf(capacity_discs[i], sizeof(capacity_discs[i]), "CAP%05d", i);
        capacity[i].id = capacity_ids[i];
        capacity[i].primary_disc_id = capacity_discs[i];
    }}
    uint64_t capacity_epoch = nk_title_catalog_epoch();
    for (int i = 0; i < NK_TITLE_MAX_OVERLAYS; i++)
        assert(nk_title_catalog_register_overlay(&capacity[i]));
    assert(nk_title_catalog_epoch() == capacity_epoch + 8);
    capacity_epoch = nk_title_catalog_epoch();
    /* An already registered entry is still accepted; a ninth title is refused
       and reported, never dropped silently. */
    assert(nk_title_catalog_register_overlay(&capacity[0]));
    assert(!nk_title_catalog_register_overlay(&capacity[8]));
    assert(!nk_title_catalog_find_by_id("capacity-title-8", &missing));
    assert(nk_title_catalog_epoch() == capacity_epoch);
    nk_title_catalog_clear_overlay();
    assert(nk_title_catalog_epoch() == capacity_epoch + 1);
    empty_epoch = nk_title_catalog_epoch();
    nk_title_catalog_clear_overlay();
    assert(nk_title_catalog_epoch() == empty_epoch);

    /* The snapshot copy accepts every module count the manifest parser
       accepts: one shared limit, so a valid 33-module overlay stays findable. */
    static NkModuleDefinition many_modules[NK_TITLE_MAX_MODULES];
    for (int i = 0; i < NK_TITLE_MAX_MODULES; i++) {{
        many_modules[i].name = "module.prx";
        many_modules[i].load_address = 0x08900000U + (uint32_t)i * 0x1000U;
    }}
    NkTitleEntry module_overlay;
    memset(&module_overlay, 0, sizeof(module_overlay));
    module_overlay.id = "many-modules-v1";
    module_overlay.primary_disc_id = "ULUS00004";
    module_overlay.modules = many_modules;
    module_overlay.module_count = 33;
    assert(nk_title_catalog_register_overlay(&module_overlay));
    NkTitleEntrySnapshot module_snapshot = {{0}};
    assert(nk_title_catalog_find_by_id("many-modules-v1", &module_snapshot));
    assert(module_snapshot.entry.module_count == 33);
    nk_title_catalog_snapshot_release(&module_snapshot);
    module_overlay.module_count = NK_TITLE_MAX_MODULES;
    assert(nk_title_catalog_find_by_id("many-modules-v1", &module_snapshot));
    assert(module_snapshot.entry.module_count == NK_TITLE_MAX_MODULES);
    nk_title_catalog_snapshot_release(&module_snapshot);
    module_overlay.module_count = NK_TITLE_MAX_MODULES + 1;
    assert(!nk_title_catalog_find_by_id("many-modules-v1", &module_snapshot));
    nk_title_catalog_clear_overlay();

    /* Test nonexistent disc ID */
    assert(!nk_title_catalog_find_by_disc_id("ULUS99999", &missing));
    assert(!nk_title_catalog_find_by_disc_id("", &missing));
    assert(!nk_title_catalog_find_by_disc_id(NULL, &missing));

    nk_title_catalog_snapshot_release(&found_overlay);
    nk_title_catalog_snapshot_release(&found_overlay_by_id);
    nk_title_catalog_snapshot_release(&t_synth);
    nk_title_catalog_snapshot_release(&t_p5);
    nk_title_catalog_snapshot_release(&t2);
    nk_title_catalog_snapshot_release(&t3);
    nk_title_catalog_snapshot_release(&found_overlay);
    nk_title_catalog_snapshot_release(&found_overlay_by_id);

    printf("ALL_NATIVE_CATALOG_C_ASSERTIONS_PASSED\\n");
    return 0;
}}
"""
        test_c_file.write_text(test_c_source, encoding="utf-8")
        exe_path = self.temp_dir / ("test_harness.exe" if sys.platform == "win32" else "test_harness")

        cmd_compile = [
            gcc,
            "-Wall", "-Wextra", "-Werror", "-std=c99",
            "-I", str(cat_h.parent),
            str(test_c_file),
            str(cat_c),
            "-o", str(exe_path),
        ]
        res_comp = subprocess.run(cmd_compile, capture_output=True, text=True)
        self.assertEqual(res_comp.returncode, 0, f"Compilation failed: {res_comp.stderr}")

        res_exec = subprocess.run([str(exe_path)], capture_output=True, text=True)
        self.assertEqual(res_exec.returncode, 0, f"Execution failed: {res_exec.stderr}")
        self.assertIn("ALL_NATIVE_CATALOG_C_ASSERTIONS_PASSED", res_exec.stdout)

    def _compile_and_run_catalog_harness(self, header: Path, source: Path, body: str) -> str:
        gcc = shutil.which("gcc")
        if not gcc:
            self.skipTest("TOOLCHAIN_MISSING: gcc not available in PATH on this host")
        harness = self.temp_dir / "limits_harness.c"
        harness.write_text(
            "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
            f'#include "{header.as_posix()}"\n'
            "int main(void) {\n" + body + '    printf("HARNESS_OK\\n");\n    return 0;\n}\n',
            encoding="utf-8",
        )
        exe_path = self.temp_dir / ("limits_harness.exe" if sys.platform == "win32" else "limits_harness")
        res_comp = subprocess.run(
            [gcc, "-Wall", "-Wextra", "-Werror", "-std=c99", "-I", str(header.parent),
             str(harness), str(source), "-o", str(exe_path)],
            capture_output=True, text=True,
        )
        self.assertEqual(res_comp.returncode, 0, f"Compilation failed: {res_comp.stderr}")
        res_exec = subprocess.run([str(exe_path)], capture_output=True, text=True)
        self.assertEqual(res_exec.returncode, 0, f"Execution failed: {res_exec.stdout}{res_exec.stderr}")
        self.assertIn("HARNESS_OK", res_exec.stdout)
        return res_exec.stdout

    def test_native_snapshot_admits_every_module_the_validator_admits(self) -> None:
        """A title with as many modules as title_manifest admits must still be found.

        The snapshot copy used a fixed cap of 32, below the validator's 256,
        so a valid title with more modules was reported as not found.
        """
        limit = title_manifest.MAX_TITLE_MODULES
        cat_h = ROOT / "src" / "core" / "generated" / "nk_title_catalog.h"
        cat_c = ROOT / "src" / "core" / "generated" / "nk_title_catalog.c"
        body = f"""    static NkModuleDefinition modules[{limit}];
    static char names[{limit}][16];
    for (int i = 0; i < {limit}; i++) {{
        snprintf(names[i], sizeof(names[i]), "mod%d.prx", i);
        modules[i].name = names[i];
    }}
    NkTitleEntry overlay;
    memset(&overlay, 0, sizeof(overlay));
    overlay.id = "many-modules-v1";
    overlay.display_name = "Many modules";
    overlay.kind = NK_TITLE_KIND_HOMEBREW;
    overlay.primary_disc_id = "MANY00001";
    overlay.modules = modules;
    overlay.module_count = {limit};
    nk_title_catalog_register_overlay(&overlay);
    NkTitleEntrySnapshot found = {{0}};
    if (!nk_title_catalog_find_by_id("many-modules-v1", &found)) {{
        printf("snapshot refused a title with {limit} modules\\n");
        return 1;
    }}
    if (found.entry.module_count != {limit} ||
        strcmp(found.entry.modules[{limit} - 1].name, "mod{limit - 1}.prx") != 0) return 2;
    nk_title_catalog_snapshot_release(&found);
    nk_title_catalog_clear_overlay();
"""
        self._compile_and_run_catalog_harness(cat_h, cat_c, body)

    def test_generated_header_limits_come_from_the_validator(self) -> None:
        titles = title_catalog_codegen.collect_public_manifests(ROOT / "assets" / "titles")[1]
        header = title_catalog_codegen.generate_header("0" * 16, titles)
        self.assertIn(
            f"#define NK_TITLE_MAX_LOOSE_CONTENT_ROOTS {title_manifest.MAX_LOOSE_CONTENT_ROOTS}\n", header
        )
        self.assertIn(
            f"#define NK_TITLE_MAX_LOOSE_CONTENT_EXCLUDES {title_manifest.MAX_LOOSE_CONTENT_EXCLUDES}\n", header
        )
        self.assertIn(f"#define NK_TITLE_MAX_MODULES {title_manifest.MAX_TITLE_MODULES}\n", header)

    def test_display_name_with_trigraph_sequences_survives_c99(self) -> None:
        """'??!' and '??/' in manifest text must reach C unchanged under -std=c99."""
        titles = copy.deepcopy(
            title_catalog_codegen.collect_public_manifests(ROOT / "assets" / "titles")[1]
        )
        target = next(t for t in titles if t["id"] == "synthetic-allegrex-v1")
        tricky = "Why??! ok??/"
        target["display_name"] = tricky
        out_h = self.temp_dir / "nk_title_catalog.h"
        out_c = self.temp_dir / "nk_title_catalog.c"
        out_h.write_text(title_catalog_codegen.generate_header("0" * 16, titles), encoding="utf-8", newline="")
        out_c.write_text(title_catalog_codegen.generate_source("0" * 16, titles), encoding="utf-8", newline="")
        expected = "".join(f"\\{b:03o}" for b in tricky.encode("utf-8"))
        body = f"""    NkTitleEntrySnapshot found = {{0}};
    if (!nk_title_catalog_find_by_id("synthetic-allegrex-v1", &found)) return 1;
    if (strcmp(found.entry.display_name, "{expected}") != 0) {{
        printf("display_name was [%s]\\n", found.entry.display_name);
        return 2;
    }}
    nk_title_catalog_snapshot_release(&found);
"""
        self._compile_and_run_catalog_harness(out_h, out_c, body)

    def test_snapshot_copy_failure_frees_only_owned_storage(self) -> None:
        """Fail each allocation of a snapshot copy in turn: the failed copy must free
        only what it allocated. A loose root's borrowed root/mount pointers used to
        stay in the owned array when its root copy failed, and release then freed
        the source's string literal (audit 2026-11-17, F-27efef84)."""
        gcc = shutil.which("gcc")
        if not gcc:
            self.skipTest("TOOLCHAIN_MISSING: gcc not available in PATH on this host")

        cat_h = ROOT / "src" / "core" / "generated" / "nk_title_catalog.h"
        cat_c = ROOT / "src" / "core" / "generated" / "nk_title_catalog.c"
        harness = self.temp_dir / "fault_harness.c"
        # The catalog source is compiled inside this translation unit, after the
        # C library headers, so its malloc/calloc/free calls go to a tracking
        # allocator that can fail the Nth allocation and refuses a foreign free.
        harness.write_text(f"""#if !defined(_WIN32) && !defined(_WIN64) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <pthread.h>
#endif

#define NK_FAULT_MAX_LIVE 4096
static void *s_live[NK_FAULT_MAX_LIVE];
static int s_live_count = 0;
static int s_alloc_count = 0;
static int s_fail_at = 0;

static void *nk_fault_track(void *p) {{
    if (!p) return NULL;
    if (s_live_count >= NK_FAULT_MAX_LIVE) {{ fprintf(stderr, "TRACK_FULL\\n"); exit(4); }}
    s_live[s_live_count++] = p;
    return p;
}}
static void *nk_fault_malloc(size_t size) {{
    if (++s_alloc_count == s_fail_at) return NULL;
    return nk_fault_track(malloc(size));
}}
static void *nk_fault_calloc(size_t count, size_t size) {{
    if (++s_alloc_count == s_fail_at) return NULL;
    return nk_fault_track(calloc(count, size));
}}
static void nk_fault_free(void *p) {{
    if (!p) return;
    for (int i = 0; i < s_live_count; i++) {{
        if (s_live[i] == p) {{
            s_live[i] = s_live[--s_live_count];
            free(p);
            return;
        }}
    }}
    fprintf(stderr, "FOREIGN_FREE after failing allocation %d\\n", s_fail_at);
    exit(3);
}}
#define malloc nk_fault_malloc
#define calloc nk_fault_calloc
#define free nk_fault_free
#include "{cat_c.as_posix()}"
#undef malloc
#undef calloc
#undef free

int main(void) {{
    static const char *const compat[] = {{ "ULES00001", "ULJS00002", NULL }};
    static const char *const excludes[] = {{ "skip/a.bin", "skip/b.bin" }};
    static const NkLooseContentRoot roots[] = {{
        {{ "loose", "", 0U, false, excludes, 2 }},
        {{ ".", "mnt", 1U, true, NULL, 0 }},
    }};
    static const NkModuleDefinition modules[] = {{
        {{ "one.prx", 0x08900000U, true }},
        {{ "two.prx", 0x08a00000U, false }},
    }};
    NkTitleEntry overlay;
    memset(&overlay, 0, sizeof(overlay));
    overlay.id = "fault-copy-v1";
    overlay.game_name = "fault-copy";
    overlay.display_name = "Fault Copy";
    overlay.primary_disc_id = "ULUS00003";
    overlay.compatible_disc_ids = compat;
    overlay.bss_metadata_source = "elf";
    overlay.data_root = "data";
    overlay.loose_content_roots = roots;
    overlay.loose_content_root_count = 2;
    overlay.memory_stick_root = "ms";
    overlay.hle_profile = "generic";
    overlay.codegen_profile = "none";
    overlay.modules = modules;
    overlay.module_count = 2;
    nk_title_catalog_register_overlay(&overlay);

    NkTitleEntrySnapshot snapshot;
    s_alloc_count = 0;
    s_fail_at = 0;
    if (!nk_title_catalog_find_by_id("fault-copy-v1", &snapshot)) return 5;
    int allocations = s_alloc_count;
    nk_title_catalog_snapshot_release(&snapshot);
    if (s_live_count != 0 || allocations < 10) return 6;
    for (int fail = 1; fail <= allocations; fail++) {{
        s_alloc_count = 0;
        s_fail_at = fail;
        if (nk_title_catalog_find_by_id("fault-copy-v1", &snapshot)) return 7;
        if (snapshot.owned_storage || s_live_count != 0) {{
            fprintf(stderr, "LEAK after failing allocation %d\\n", fail);
            return 8;
        }}
    }}
    s_fail_at = 0;
    nk_title_catalog_clear_overlay();
    printf("SNAPSHOT_FAULT_INJECTION_PASSED %d\\n", allocations);
    return 0;
}}
""", encoding="utf-8")
        exe_path = self.temp_dir / ("fault_harness.exe" if sys.platform == "win32" else "fault_harness")
        res_comp = subprocess.run(
            [gcc, "-Wall", "-Wextra", "-Werror", "-std=c99", "-I", str(cat_h.parent),
             str(harness), "-o", str(exe_path)],
            capture_output=True, text=True,
        )
        self.assertEqual(res_comp.returncode, 0, f"Compilation failed: {res_comp.stderr}")
        res_exec = subprocess.run([str(exe_path)], capture_output=True, text=True)
        self.assertEqual(res_exec.returncode, 0,
                         f"Execution failed ({res_exec.returncode}): {res_exec.stderr}")
        self.assertIn("SNAPSHOT_FAULT_INJECTION_PASSED", res_exec.stdout)

    def test_parity_between_python_registry_and_c_catalog(self) -> None:
        """Ensure Python TitleRegistry and C catalog headers have the exact same titles."""
        reg = TitleRegistry(include_defaults=True)
        _, manifests = title_catalog_codegen.collect_public_manifests(ROOT / "assets" / "titles")

        self.assertEqual(len(reg._profiles), len(manifests))
        for m in manifests:
            p = reg.lookup_by_id(m["id"])
            self.assertIsNotNone(p)
            self.assertEqual(p.name, m["display_name"])


    def test_collect_public_manifests_rejects_duplicate_title_and_disc_identities(self) -> None:
        """Verify that collect_public_manifests refuses colliding title catalog IDs or disc IDs."""
        temp_titles = self.temp_dir / "titles"
        temp_titles.mkdir()

        m1 = title_manifest.load_manifest(ROOT / "assets" / "titles" / "synthetic.json")
        m2 = title_manifest.load_manifest(ROOT / "assets" / "titles" / "synthetic-title2.json")

        # Case 1: Colliding title IDs
        m2_colliding_id = copy.deepcopy(m2)
        m2_colliding_id["id"] = m1["id"]

        (temp_titles / "synthetic.json").write_text(json.dumps(m1), encoding="utf-8")
        (temp_titles / "synthetic-title2.json").write_text(json.dumps(m2_colliding_id), encoding="utf-8")

        with self.assertRaisesRegex(ValueError, "catalog contains duplicate title identity"):
            title_catalog_codegen.collect_public_manifests(temp_titles, policy_dir=ROOT / "assets" / "titles")

        # Case 2: Colliding disc IDs
        m1_retail = copy.deepcopy(m1)
        m1_retail.pop("profile_zero", None)
        m1_retail["kind"] = "retail"
        m1_retail["disc"] = {"id": "TEST12345", "region": "NA", "revision_policy": "exact-disc-id"}

        m2_retail = copy.deepcopy(m2)
        m2_retail.pop("profile_zero", None)
        m2_retail["kind"] = "retail"
        m2_retail["disc"] = {"id": "TEST12345", "region": "NA", "revision_policy": "exact-disc-id"}

        (temp_titles / "synthetic.json").write_text(json.dumps(m1_retail), encoding="utf-8")
        (temp_titles / "synthetic-title2.json").write_text(json.dumps(m2_retail), encoding="utf-8")

        with self.assertRaisesRegex(ValueError, "catalog contains duplicate disc identity"):
            title_catalog_codegen.collect_public_manifests(temp_titles, policy_dir=ROOT / "assets" / "titles")

    def test_title_registry_rejects_duplicate_or_colliding_manifests(self) -> None:
        """Verify that TitleRegistry refuses loading or registering colliding title profiles."""
        temp_titles = self.temp_dir / "titles"
        temp_titles.mkdir()

        m1 = title_manifest.load_manifest(ROOT / "assets" / "titles" / "synthetic.json")
        m2 = title_manifest.load_manifest(ROOT / "assets" / "titles" / "synthetic-title2.json")

        # Duplicate title ID
        m2_colliding = copy.deepcopy(m2)
        m2_colliding["id"] = m1["id"]

        (temp_titles / "synthetic.json").write_text(json.dumps(m1), encoding="utf-8")
        (temp_titles / "synthetic-title2.json").write_text(json.dumps(m2_colliding), encoding="utf-8")

        reg = TitleRegistry(include_defaults=False)
        with self.assertRaisesRegex(RuntimeError, "conflicts with public canonical catalog"):
            reg.load_from_directory(temp_titles, enforce_public_policy=False)

    def test_identical_overlay_is_refused_unless_override_is_explicit(self) -> None:
        """A byte-for-byte copy of a public manifest is still an override in standard mode."""
        import contextlib
        import io

        public_dir = self.temp_dir / "public"
        public_dir.mkdir()
        m1 = title_manifest.load_manifest(ROOT / "assets" / "titles" / "synthetic.json")
        (public_dir / "synthetic.json").write_text(json.dumps(m1), encoding="utf-8")
        overlay_dir = self.temp_dir / "overlay"
        overlay_dir.mkdir()
        overlay = overlay_dir / "synthetic.json"
        overlay.write_text(json.dumps(m1), encoding="utf-8")

        reg = TitleRegistry(include_defaults=False)
        reg.load_from_directory(public_dir, enforce_public_policy=False)
        with self.assertRaisesRegex(RuntimeError, "conflicts with public canonical catalog"):
            reg.load_private_manifest(overlay)
        with contextlib.redirect_stderr(io.StringIO()):
            reg.load_private_manifest(overlay, allow_override=True)


if __name__ == "__main__":
    unittest.main()
