# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""tools/android_sysroot.py: the sysroot's inputs, layout and archive.

The bionic here is synthetic: a temporary git repository with bionic's folder layout and
files written for these tests. With NK_ANDROID_LLVM_BIN pointing at an LLVM bin folder,
the crt objects and stubs are compiled too; without it those tests are skipped.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import android_sysroot as sysroot  # noqa: E402

CRT_C = b"void nk_crt_marker(void) {}\n"
CRT_S = b'#include <private/bionic_asm.h>\n.section .note.android.ident,"a",%note\n.long PLATFORM_SDK_VERSION\n'
MAP = b"LIBC {\n  global:\n    nk_symbol;\n};\n"

FILES = {
    "libc/include/stdio.h": b"/* stdio */\r\n",  # the CRLF must survive: git objects, not a checkout
    "libc/include/android/api-level.h": b"/* api */\n",
    "libc/kernel/uapi/linux/netfilter_ipv4/ipt_TTL.h": b"/* upper */\n",
    "libc/kernel/uapi/linux/netfilter_ipv4/ipt_ttl.h": b"/* lower */\n",
    "libc/kernel/uapi/asm-arm64/asm/unistd.h": b"/* arm64 */\n",
    "libc/kernel/uapi/asm-x86/asm/unistd.h": b"/* x86 */\n",
    "libc/kernel/uapi/asm-generic/unistd.h": b"/* generic */\n",
    "libc/kernel/android/uapi/linux/compiler.h": b"/* compiler */\n",
    "libc/private/bionic_asm.h": b"/* asm */\n",
    "libc/bionic/libc_init_common.h": b"/* init */\n",
    "libc/arch-common/bionic/crtbegin.c": CRT_C,
    "libc/arch-common/bionic/crtbegin_so.c": CRT_C,
    "libc/arch-common/bionic/crtbrand.S": CRT_S,
    "libc/arch-common/bionic/crtend.S": b"#include <private/bionic_asm.h>\n",
    "libc/arch-common/bionic/crtend_so.S": b"#include <private/bionic_asm.h>\n",
    "libc/arch-common/bionic/crt_pad_segment.S": b"#include <private/bionic_asm.h>\n",
    "libc/libc.map.txt": MAP,
    "libm/libm.map.txt": b"LIBC {\n  global:\n    nk_math;\n};\n",
    "libdl/libdl.map.txt": b"LIBC {\n  global:\n    nk_dl;\n};\n",
    "libc/NOTICE": b"libc notice\n",
    "libm/NOTICE": b"libm notice\n",
    "libdl/NOTICE": b"libdl notice\n",
    "libc/SECRET_UNUSED.txt": b"not part of the sysroot\n",
}


EXTRA = {
    "system/logging/liblog/include/android/log.h": b"/* log */\n",
    "system/logging/liblog/liblog.map.txt": b"LIBLOG {\n  global:\n    nk_log;\n};\n",
    "system/logging/liblog/NOTICE": b"liblog notice\n",
    "frameworks/base/native/android/libandroid.map.txt": b"LIBANDROID {\n  global:\n    nk_android;\n};\n",
    "frameworks/base/NOTICE": b"base notice\n",
    "frameworks/native/vulkan/libvulkan/libvulkan.map.txt": b"LIBVULKAN {\n  global:\n    nk_vulkan;\n};\n",
    "frameworks/native/NOTICE": b"native notice\n",
    "Vulkan-Headers/include/vulkan/vulkan.h": b"/* vulkan */\n",
    "Vulkan-Headers/include/vk_video/vulkan_video_codecs_common.h": b"/* video */\n",
    "Vulkan-Headers/LICENSE.md": b"licence\n",
    "Vulkan-Headers/LICENSES/Apache-2.0.txt": b"apache\n",
    "Vulkan-Headers/LICENSES/MIT.txt": b"mit\n",
    "license-list-data/text/GPL-2.0-only.txt": b"gpl-2.0\n",
    "license-list-data/text/Linux-syscall-note.txt": b"syscall note\n",
}
EXTRA_PINS = {path: hashlib.sha256(data).hexdigest() for path, data in EXTRA.items()}


def _git(repo: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-c", "core.autocrlf=false", "-C", str(repo), *args],
        capture_output=True, text=True, check=True,
    ).stdout.strip()


class _SyntheticBionic(unittest.TestCase):
    """A synthetic bionic clone shared by the tests of one class."""

    _tmp: tempfile.TemporaryDirectory[str]
    repo: Path
    commit: str

    @classmethod
    def setUpClass(cls) -> None:
        cls._tmp = tempfile.TemporaryDirectory(prefix="nk-sysroot-test-")
        cls.repo = Path(cls._tmp.name) / "bionic"
        cls.repo.mkdir()
        _git(cls.repo, "init", "-q")
        _git(cls.repo, "config", "core.ignorecase", "false")
        # Write blobs straight into the index, so a case-insensitive file system cannot
        # merge the two ipt_* names.
        for path, data in FILES.items():
            blob = subprocess.run(
                ["git", "-C", str(cls.repo), "hash-object", "-w", "--stdin"],
                input=data, capture_output=True, check=True,
            ).stdout.decode().strip()
            _git(cls.repo, "update-index", "--add", "--cacheinfo", f"100644,{blob},{path}")
        tree = _git(cls.repo, "write-tree")
        env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
                   GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid",
                   GIT_AUTHOR_DATE="2026-01-01T00:00:00Z", GIT_COMMITTER_DATE="2026-01-01T00:00:00Z")
        cls.commit = subprocess.run(
            ["git", "-C", str(cls.repo), "commit-tree", tree, "-m", "synthetic bionic"],
            capture_output=True, text=True, check=True, env=env,
        ).stdout.strip()

    @classmethod
    def tearDownClass(cls) -> None:
        cls._tmp.cleanup()


@unittest.skipIf(shutil.which("git") is None, "git is not installed")
class SysrootTests(_SyntheticBionic):
    def test_only_the_named_inputs_are_read_byte_for_byte(self) -> None:
        files = sysroot.read_bionic(self.repo, self.commit)
        self.assertEqual(files["libc/include/stdio.h"], b"/* stdio */\r\n")
        self.assertNotIn("libc/SECRET_UNUSED.txt", files)
        self.assertIn("libc/kernel/uapi/linux/netfilter_ipv4/ipt_TTL.h", files)
        self.assertIn("libc/kernel/uapi/linux/netfilter_ipv4/ipt_ttl.h", files)

    def test_a_commit_the_clone_does_not_have_is_refused(self) -> None:
        with self.assertRaisesRegex(sysroot.SysrootError, "git rev-parse"):
            sysroot.read_bionic(self.repo, "0" * 40)

    def test_the_pinned_commit_is_the_default(self) -> None:
        # The synthetic clone lacks it, so the default refuses: no silent use of another commit.
        with self.assertRaises(sysroot.SysrootError):
            sysroot.read_bionic(self.repo)

    def test_headers_take_the_ndk_layout_and_only_this_architecture(self) -> None:
        headers = sysroot.header_entries(sysroot.read_bionic(self.repo, self.commit))
        self.assertEqual(
            sorted(headers),
            [
                "usr/include/aarch64-linux-android/asm/unistd.h",
                "usr/include/android/api-level.h",
                "usr/include/asm-generic/unistd.h",
                "usr/include/linux/compiler.h",
                "usr/include/linux/netfilter_ipv4/ipt_TTL.h",
                "usr/include/linux/netfilter_ipv4/ipt_ttl.h",
                "usr/include/stdio.h",
            ],
        )
        self.assertEqual(headers["usr/include/aarch64-linux-android/asm/unistd.h"].data, b"/* arm64 */\n")

    def test_two_sources_for_one_header_are_refused(self) -> None:
        files = {
            "libc/include/linux/compiler.h": b"a",
            "libc/kernel/android/uapi/linux/compiler.h": b"b",
        }
        with self.assertRaisesRegex(sysroot.SysrootError, "both map to usr/include/linux/compiler.h"):
            sysroot.header_entries(files)

    def test_case_collisions_are_found(self) -> None:
        self.assertEqual(
            sysroot.case_collisions(["a/X.h", "a/x.h", "a/y.h"]), {"a/X.h", "a/x.h"}
        )

    def test_the_archive_is_sorted_fixed_and_keeps_both_case_variants(self) -> None:
        headers = sysroot.header_entries(sysroot.read_bionic(self.repo, self.commit))
        out = Path(self._tmp.name)
        first = sysroot.write_archive(headers, out / "a.tar")
        second = sysroot.write_archive(dict(reversed(list(headers.items()))), out / "b.tar")
        self.assertEqual(first, second)
        self.assertEqual((out / "a.tar").read_bytes(), (out / "b.tar").read_bytes())
        with tarfile.open(out / "a.tar") as tar:
            members = tar.getmembers()
        names = [m.name for m in members]
        self.assertIn("usr/include/linux/netfilter_ipv4/ipt_TTL.h", names)
        self.assertIn("usr/include/linux/netfilter_ipv4/ipt_ttl.h", names)
        self.assertLess(names.index("usr"), names.index("usr/include"))
        for member in members:
            self.assertEqual((member.mtime, member.uid, member.gid), (0, 0, 0))
            self.assertIn(member.mode, (0o644, 0o755))

    def test_the_notices_ship_under_their_source_paths(self) -> None:
        notices = sysroot.notice_entries(sysroot.read_bionic(self.repo, self.commit))
        self.assertEqual(sorted(notices), sorted(f"NOTICES/bionic/{p}" for p in sysroot.NOTICES))


class ExtraInputTests(unittest.TestCase):
    """The pinned inputs from outside bionic."""

    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory(prefix="nk-sysroot-extra-")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        for path, data in EXTRA.items():
            (self.root / path).parent.mkdir(parents=True, exist_ok=True)
            (self.root / path).write_bytes(data)

    def test_the_pinned_files_are_read(self) -> None:
        self.assertEqual(sysroot.read_extra(self.root, EXTRA_PINS), EXTRA)

    def test_a_changed_or_missing_input_is_refused(self) -> None:
        (self.root / "Vulkan-Headers/LICENSE.md").write_bytes(b"other\n")
        with self.assertRaisesRegex(sysroot.SysrootError, "LICENSE.md: sha256"):
            sysroot.read_extra(self.root, EXTRA_PINS)
        (self.root / "Vulkan-Headers/LICENSE.md").unlink()
        with self.assertRaisesRegex(sysroot.SysrootError, "pinned input missing"):
            sysroot.read_extra(self.root, EXTRA_PINS)

    def test_headers_stubs_and_notices_take_the_ndk_layout(self) -> None:
        headers = sysroot.extra_header_entries(EXTRA)
        self.assertEqual(sorted(headers), [
            "usr/include/android/log.h",
            "usr/include/vk_video/vulkan_video_codecs_common.h",
            "usr/include/vulkan/vulkan.h",
        ])
        stubs = sysroot.stub_inputs(FILES, EXTRA)
        self.assertEqual([(s.library, s.unversioned) for s in stubs], [
            ("libc.so", False), ("libm.so", False), ("libdl.so", False),
            ("liblog.so", True), ("libandroid.so", True), ("libvulkan.so", True),
        ])
        notices = sysroot.notice_entries(FILES, EXTRA)
        self.assertEqual(notices["NOTICES/linux-uapi/Linux-syscall-note.txt"].data, b"syscall note\n")
        self.assertEqual(len(notices), len(sysroot.NOTICES) + len(sysroot.EXTRA_NOTICES))

    def test_an_extra_header_bionic_also_has_is_refused(self) -> None:
        clash = {"usr/include/stdio.h": sysroot.Entry(b"/* other */\n")}
        with self.assertRaisesRegex(sysroot.SysrootError, "usr/include/stdio.h"):
            sysroot.header_entries(FILES, clash)

    def test_the_repository_pins_every_input_the_builder_names(self) -> None:
        document = json.loads(sysroot.INPUTS_FILE.read_text(encoding="utf-8"))
        names = set(document["files"])
        wanted = {source for source, _ in sysroot.EXTRA_HEADERS}
        wanted |= {source for _, source in sysroot.EXTRA_STUBS}
        wanted |= {source for source, _ in sysroot.EXTRA_NOTICES}
        self.assertLessEqual(wanted, names)
        for tree, _ in sysroot.EXTRA_HEADER_TREES:
            self.assertTrue(any(name.startswith(tree + "/") for name in names), tree)
        sources = document["sources"]
        for source in sources.values():
            self.assertRegex(source["commit"], r"^[0-9a-f]{40}$")
        for name, entry in document["files"].items():
            self.assertTrue(any(name.startswith(prefix + "/") for prefix in sources), name)
            self.assertRegex(entry["sha256"], r"^[0-9a-f]{64}$")
            self.assertRegex(entry["git_blob"], r"^[0-9a-f]{40}$")
        self.assertEqual(sysroot.load_pins(), {n: e["sha256"] for n, e in document["files"].items()})


def _clang() -> Path | None:
    folder = os.environ.get("NK_ANDROID_LLVM_BIN")
    if not folder:
        return None
    for name in ("clang", "clang.exe"):
        if (Path(folder) / name).is_file():
            return Path(folder) / name
    return None


@unittest.skipIf(shutil.which("git") is None, "git is not installed")
class BuildTests(_SyntheticBionic):
    """The objects and stubs, compiled from the synthetic sources."""

    def test_crt_objects_and_stubs_are_built_for_the_api_level(self) -> None:
        clang = _clang()
        if clang is None:
            self.skipTest("set NK_ANDROID_LLVM_BIN to an LLVM bin folder")
        files = sysroot.read_bionic(self.repo, self.commit)
        headers = sysroot.header_entries(files)
        with tempfile.TemporaryDirectory(prefix="nk-sysroot-build-") as tmp:
            objects = sysroot.build_objects(files, headers, clang, 29, Path(tmp))
        lib = "usr/lib/aarch64-linux-android/29"
        self.assertEqual(
            sorted(objects),
            sorted(f"{lib}/{name}" for name in (
                "crtbegin_dynamic.o", "crtbegin_so.o", "crtend_android.o", "crtend_so.o",
                "crt_pad_segment.o", "libc.so", "libm.so", "libdl.so")),
        )
        for path, entry in objects.items():
            self.assertTrue(entry.data.startswith(b"\x7fELF\x02\x01"), path)  # 64-bit little-endian
            self.assertEqual(entry.data[18:20], b"\xb7\x00", path)  # EM_AARCH64
        self.assertIn(b"nk_symbol", objects[f"{lib}/libc.so"].data)
        # crtbrand's note carries the API level, 29, linked into both crtbegin objects.
        for name in ("crtbegin_dynamic.o", "crtbegin_so.o"):
            self.assertIn(b"nk_crt_marker", objects[f"{lib}/{name}"].data)
            self.assertIn((29).to_bytes(4, "little"), objects[f"{lib}/{name}"].data)

    def test_the_extra_stubs_are_built_unversioned(self) -> None:
        clang = _clang()
        if clang is None:
            self.skipTest("set NK_ANDROID_LLVM_BIN to an LLVM bin folder")
        files = sysroot.read_bionic(self.repo, self.commit)
        headers = sysroot.header_entries(files, sysroot.extra_header_entries(EXTRA))
        with tempfile.TemporaryDirectory(prefix="nk-sysroot-build-") as tmp:
            objects = sysroot.build_objects(files, headers, clang, 29, Path(tmp),
                                            sysroot.stub_inputs(files, EXTRA))
        lib = "usr/lib/aarch64-linux-android/29"
        for library, symbol in (("liblog.so", b"nk_log"), ("libandroid.so", b"nk_android"),
                                ("libvulkan.so", b"nk_vulkan")):
            data = objects[f"{lib}/{library}"].data
            self.assertIn(symbol, data, library)
            self.assertNotIn(b"LIBLOG", data, library)  # no version names
            self.assertNotIn(b"LIBANDROID", data, library)
        self.assertIn(b"LIBC", objects[f"{lib}/libc.so"].data)


if __name__ == "__main__":
    unittest.main()
