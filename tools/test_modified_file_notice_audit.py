# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp contributors

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

try:
    from modified_file_notice_audit import audit
    from nk_core.git_isolation import isolated_git_env
except ModuleNotFoundError:
    from tools.modified_file_notice_audit import audit
    from tools.nk_core.git_isolation import isolated_git_env

NOTICES = (
    "// SPDX-License-Identifier: GPL-2.0-or-later\n"
    "// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)\n"
    "// Modified by Nakagawa Recomp contributors, 2026-08-11.\n"
    "// See NOTICE.md for upstream lineage and modification provenance.\n"
)


def _git(root: Path, *args: str) -> str:
    return subprocess.run(
        ["git", *args], cwd=root, env=isolated_git_env(root=root), check=True,
        capture_output=True, text=True,
    ).stdout


class ModifiedFileNoticeAuditTests(unittest.TestCase):
    def _fixture(self, source: str) -> tuple[Path, Path]:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        (root / "docs/provenance").mkdir(parents=True)
        (root / "third_party" / "licenses").mkdir(parents=True)
        (root / "LICENSE").write_text("GNU GENERAL PUBLIC LICENSE\nVersion 3\n", encoding="utf-8")
        (root / "third_party/licenses/GPL-2.0.txt").write_text(
            "GNU GENERAL PUBLIC LICENSE\nVersion 2, June 1991\n"
            "This License applies to any program or other work\n",
            encoding="utf-8",
        )
        (root / "third_party/licenses/SAL063_CREDITS.txt").write_text("sal063\n", encoding="utf-8")
        (root / "NOTICE.md").write_text("lineage\n", encoding="utf-8")
        (root / "src").mkdir()
        (root / "src/sample.c").write_text(source, encoding="utf-8")
        (root / "generated.inc").write_text("{0x01,}\n", encoding="utf-8")
        manifest = {
            "required_license_files": [
                {"path": "LICENSE"},
                {"path": "third_party/licenses/GPL-2.0.txt"},
                {"path": "third_party/licenses/SAL063_CREDITS.txt"},
                {"path": "NOTICE.md"},
            ],
            "files": [
                {
                    "path": "src/sample.c",
                    "source_kind": "textual_source",
                    "spdx": "GPL-2.0-or-later",
                    "expected_notice": {
                        "attribution_required": True,
                        "modification_required": True,
                        "date": "2026-08-11",
                        "pointer": "See NOTICE.md for upstream lineage and modification provenance.",
                    },
                },
                {
                    "path": "generated.inc",
                    "source_kind": "generated_output",
                    "generated_from": "src/sample.c",
                },
            ],
        }
        manifest_path = root / "docs/provenance/MODIFIED_FILE_NOTICES.json"
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        _git(root, "init", "-q")
        _git(root, "add", "-A")
        _git(root, "commit", "-qm", "Tracks the sample notices")
        return root, manifest_path.relative_to(root)

    @staticmethod
    def _edit_manifest(root: Path, manifest: Path, edit) -> None:
        document = json.loads((root / manifest).read_text(encoding="utf-8"))
        edit(document)
        (root / manifest).write_text(json.dumps(document), encoding="utf-8")
        _git(root, "add", manifest.as_posix())

    def test_passes_when_manifest_notices_are_present(self) -> None:
        root, manifest = self._fixture(
            "// SPDX-License-Identifier: GPL-2.0-or-later\n"
            "// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)\n"
            "// Modified by Nakagawa Recomp contributors, 2026-08-11.\n"
            "// See NOTICE.md for upstream lineage and modification provenance.\n"
        )
        findings, counts = audit(root, manifest, tracked_only=False)
        self.assertEqual(findings, [])
        self.assertEqual(counts, {"textual": 1, "generated": 1})

    def test_fails_when_modified_notice_is_missing(self) -> None:
        root, manifest = self._fixture(
            "// SPDX-License-Identifier: GPL-2.0-or-later\n"
            "// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)\n"
        )
        findings, _ = audit(root, manifest, tracked_only=False)
        self.assertIn("src/sample.c: explicit modified-file notice is absent", findings)
        self.assertIn("src/sample.c: provenance pointer is absent", findings)

    def test_fails_when_the_gpl2_text_lacks_its_markers(self) -> None:
        root, manifest = self._fixture(
            "// SPDX-License-Identifier: GPL-2.0-or-later\n"
            "// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)\n"
            "// Modified by Nakagawa Recomp contributors, 2026-08-11.\n"
            "// See NOTICE.md for upstream lineage and modification provenance.\n"
        )
        finding = "third_party/licenses/GPL-2.0.txt does not contain GPLv2 text markers"
        self.assertNotIn(finding, audit(root, manifest, tracked_only=False)[0])
        # Each of the three markers is required; every sample lacks exactly one of them: the
        # version line (a GPLv3 text), the scope sentence, the title.
        for text in (
            "GNU GENERAL PUBLIC LICENSE\nVersion 3, 29 June 2007\n"
            "This License applies to any program or other work\n",
            "GNU GENERAL PUBLIC LICENSE\nVersion 2, June 1991\n",
            "Version 2, June 1991\nThis License applies to any program or other work\n",
        ):
            with self.subTest(text=text):
                (root / "third_party/licenses/GPL-2.0.txt").write_text(text, encoding="utf-8")
                findings, _ = audit(root, manifest, tracked_only=False)
                self.assertIn(finding, findings)
                self.assertNotIn("LICENSE does not contain the repository GPL-3.0 text markers", findings)

    def test_a_lineage_header_file_outside_the_manifest_fails(self) -> None:
        """A file split out of an inherited one keeps its notices checked (audit F-1a297996)."""
        root, manifest = self._fixture(NOTICES)
        (root / "src/split.c").write_text(NOTICES + "int split;\n", encoding="utf-8")
        _git(root, "add", "src/split.c")
        finding = "src/split.c: carries the sal063 lineage header but is not in the notice manifest"
        for tracked_only in (False, True):
            with self.subTest(tracked_only=tracked_only):
                self.assertIn(finding, audit(root, manifest, tracked_only=tracked_only)[0])

        self._edit_manifest(root, manifest, lambda document: document.update(lineage_header_files={"files": [{
            "path": "src/split.c",
            "spdx": "GPL-2.0-or-later",
            "modified_notice": "Modified by Nakagawa Recomp contributors, 2026-08-11.",
            "pointer": "See NOTICE.md for upstream lineage and modification provenance.",
        }]}))
        findings, counts = audit(root, manifest, tracked_only=True)
        self.assertEqual(findings, [])
        self.assertEqual(counts, {"textual": 2, "generated": 1})

        without_notice = NOTICES.replace("// Modified by Nakagawa Recomp contributors, 2026-08-11.\n", "")
        (root / "src/split.c").write_text(without_notice, encoding="utf-8")
        _git(root, "add", "src/split.c")
        self.assertIn("src/split.c: explicit modified-file notice is absent",
                      audit(root, manifest, tracked_only=True)[0])
        # Prose that quotes the header line further down a file does not make it a lineage file.
        (root / "docs/notes.md").write_text(
            "# Notes\n" + "\n" * 20 + "// Derived from sal063/PSP-recompilation-project\n", encoding="utf-8")
        _git(root, "add", "docs/notes.md")
        self.assertNotIn("docs/notes.md: carries the sal063 lineage header but is not in the notice manifest",
                         audit(root, manifest, tracked_only=True)[0])

    def test_index_reads_ignore_an_inherited_git_dir(self) -> None:
        """An inherited GIT_DIR naming another checkout does not stand in for this index (audit F-fc74a2af)."""
        root, manifest = self._fixture("// SPDX-License-Identifier: GPL-2.0-or-later\n"
                                       "// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)\n")
        other, _ = self._fixture(NOTICES)
        self.assertEqual(audit(other, manifest, tracked_only=True)[0], [])
        with mock.patch.dict(os.environ, {"GIT_DIR": str(other / ".git"), "GIT_WORK_TREE": str(other)}):
            findings, _ = audit(root, manifest, tracked_only=True)
        self.assertIn("src/sample.c: explicit modified-file notice is absent", findings)

    def test_manifest_paths_must_stay_inside_the_repository(self) -> None:
        """``:/text`` is Git's commit-message search, and ``/x`` leaves the tree on Windows (audit F-9c833025)."""
        root, manifest = self._fixture(NOTICES)
        original = (root / manifest).read_bytes()
        for key, edit in (
            ("generated_from", lambda document: document["files"][1].update(generated_from="/Tracks")),
            ("files", lambda document: document["files"].append({
                "path": "/Tracks", "source_kind": "textual_source", "spdx": "GPL-2.0-or-later",
                "expected_notice": {"attribution_required": True},
            })),
            ("required_license_files",
             lambda document: document["required_license_files"].append({"path": "/Tracks"})),
        ):
            with self.subTest(key=key):
                self._edit_manifest(root, manifest, edit)
                findings, _ = audit(root, manifest, tracked_only=True)
                self.assertIn("/Tracks", " ".join(finding for finding in findings
                                                  if "escapes repository scope" in finding), findings)
                (root / manifest).write_bytes(original)
                _git(root, "add", manifest.as_posix())


if __name__ == "__main__":
    unittest.main()
