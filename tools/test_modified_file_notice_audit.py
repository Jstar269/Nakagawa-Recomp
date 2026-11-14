# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp contributors

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

try:
    from modified_file_notice_audit import audit
except ModuleNotFoundError:
    from tools.modified_file_notice_audit import audit


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
        return root, manifest_path.relative_to(root)

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
        # Each marker is required: the title alone, or the version line alone, is not the text.
        for text in (
            "GNU GENERAL PUBLIC LICENSE\nVersion 3, 29 June 2007\n",
            "GNU GENERAL PUBLIC LICENSE\nVersion 2, June 1991\n",
            "Version 2, June 1991\nThis License applies to any program or other work\n",
        ):
            with self.subTest(text=text):
                (root / "third_party/licenses/GPL-2.0.txt").write_text(text, encoding="utf-8")
                findings, _ = audit(root, manifest, tracked_only=False)
                self.assertIn(finding, findings)
                self.assertNotIn("LICENSE does not contain the repository GPL-3.0 text markers", findings)


if __name__ == "__main__":
    unittest.main()
